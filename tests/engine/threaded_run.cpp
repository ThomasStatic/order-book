#include "threaded_run.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "connection_streams.hpp"
#include "engine_harness.hpp"
#include "gateway_rig.hpp"
#include "gateway_stepper.hpp"
#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/in_memory_socket_set.hpp"
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/durable_position.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"
#include "order_book/wasm/shadow_books.hpp"

namespace order_book::engine_testing {

    using namespace order_book::network;
    using namespace order_book::runtime;

    namespace {

        EngineSeedConfig generationOf(const ThreadedConfig& config) {
            EngineSeedConfig generation;
            generation.seed = config.seed;
            generation.instruments = config.instruments;
            generation.shards = config.shards;
            generation.commands = config.commands;
            generation.logWritePercent = 0;  // commands alone, as connections send them
            generation.crashes = false;
            generation.restores = 0;
            generation.rolls = 0;
            generation.damages = 0;
            generation.cleanStops = 0;
            return generation;
        }

        StreamShape shapeOf(const ThreadedConfig& config) {
            return StreamShape{config.instruments, config.shards, config.instruments, true, config.plant == ThreadedPlant::EVERY_RUN,
                               config.connectionsPerInstrument};
        }

        // Records in the log file of the shard with the most symbols, so a run rolls across many.
        constexpr std::uint64_t recordsPerSmallestFile = 25;

        // How many of these frames end a command's answer: everything but the Welcome.
        std::size_t answeredIn(const std::vector<Received>& frames) {
            return static_cast<std::size_t>(std::count_if(frames.begin(), frames.end(), [](const Received& frame) {
                return frame.last && frame.type != FrameType::WELCOME && frame.type != FrameType::PROTOCOL_ERROR;
            }));
        }

        // A thread's failure, printed at once, since a failed thread can leave the others waiting
        // for ever and the test's timeout is then all that ends the run.
        class Failures {
        public:
            void record(const std::string& who) {
                std::string what = "an unknown exception";
                try {
                    throw;
                } catch (const std::exception& error) {
                    what = error.what();
                } catch (...) {
                }
                std::cerr << "FAILED: " << who << ": " << what << std::endl;
                const std::lock_guard guard(lock);
                if (!first) {
                    first = who + ": " + what;
                }
            }

            std::optional<std::string> take() {
                const std::lock_guard guard(lock);
                return first;
            }

        private:
            std::mutex lock;
            std::optional<std::string> first;
        };

        struct ShardRun {
            explicit ShardRun(std::size_t logSlots) : logRing(logSlots) {}

            Doorbell doorbell;
            std::vector<std::unique_ptr<RingLink>> links;  // one per gateway thread
            std::thread thread;
            Ring<LogEntry> logRing;
            DurablePosition durable;
            Doorbell writerBell;
            std::thread writerThread;
            std::uint64_t logEnd = 0;  // the writer's, once it has stopped
            std::vector<std::vector<std::byte>> images;  // each book's by slot, once its matching thread stopped
        };

        void run(const ThreadedConfig& config, ThreadedResult& result) {
            const std::vector<EngineCommand> lines = generateEngineStream(generationOf(config));
            const ConnectionStreams streams = connectionStreams(shapeOf(config), lines);
            const InstrumentDirectory directory(streams.instruments, config.shards);
            const std::size_t commandSlots = config.ringSlots == 0 ? defaultCommandRingSlots : config.ringSlots;
            const std::size_t responseSlots = config.ringSlots == 0 ? defaultResponseRingSlots : config.ringSlots;
            const std::size_t logSlots = config.ringSlots == 0 ? defaultLogRingRecords : config.ringSlots;
            // The page's store, which the shadow books follow (ADR 0018), so they can be held to
            // the shards' books once every thread has stopped.
            wasm::ShadowBooks shadows(directory);
            PageLogStorage storage([&shadows](const PageLogStorage::Change& change) { shadows.follow(change); });
            Housekeeping housekeeping(storage, directory, Durability::MACHINE,
                                      leastLogFileBytes(directory) + (recordsPerSmallestFile - 1) * replay::logRecordBytes);

            InMemorySocketSet network(InMemorySocketSet::Mode::THREADED);
            std::vector<std::unique_ptr<InMemorySocketSet>> members;
            std::vector<SocketSet*> threadSockets{&network};
            for (std::size_t thread = 1; thread < config.gatewayThreads; ++thread) {
                members.push_back(std::make_unique<InMemorySocketSet>(InMemorySocketSet::MemberOf{}, network));
                threadSockets.push_back(members.back().get());
            }
            const auto socketsOf = [&](std::size_t thread) -> InMemorySocketSet& { return thread == 0 ? network : *members[thread - 1]; };
            ConnectionHandOff handOff(threadSockets);

            // Every connection is waiting before any thread starts, so thread 0 deals them all.
            std::vector<Client> clients;
            clients.reserve(streams.bytes.size());
            for (std::size_t i = 0; i < streams.bytes.size(); ++i) {
                clients.emplace_back(network);
            }

            Failures failures;
            std::vector<std::unique_ptr<ShardRun>> shards;
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                shards.push_back(std::make_unique<ShardRun>(logSlots));
                for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                    shards.back()->links.push_back(std::make_unique<RingLink>(commandSlots, responseSlots));
                }
            }
            GatewaySettings settings;
            settings.spinWindow = config.spinWindow;
            std::vector<std::unique_ptr<GatewayThread>> gateways;
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                std::vector<ShardLink> links;
                for (const std::unique_ptr<ShardRun>& shard : shards) {
                    links.push_back(ShardLink{&shard->links[thread]->commands, &shard->links[thread]->responses, &shard->doorbell,
                                              &shard->durable});
                }
                gateways.push_back(std::make_unique<GatewayThread>(directory, std::move(links), socketsOf(thread), settings, &handOff, thread));
            }

            // The housekeeping thread, then each shard's log writer, which makes its first file, and
            // its matching thread, built on its own thread, which owns its books.
            std::thread housekeeper([&] {
                try {
                    housekeeping.run();
                } catch (...) {
                    failures.record("housekeeping");
                }
            });
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                ShardRun& own = *shards[number];
                own.writerThread = std::thread([&, number] {
                    try {
                        LogWriter writer(housekeeping, number, own.logRing, own.durable);
                        writer.run(own.writerBell, config.spinWindow);
                        own.logEnd = writer.nextPosition();
                    } catch (...) {
                        failures.record("log writer " + std::to_string(number));
                    }
                });
                own.thread = std::thread([&, number] {
                    try {
                        Shard shard(directory, number);
                        shard.watchDurablePosition(own.durable);
                        std::vector<RingPair> rings;
                        for (const std::unique_ptr<RingLink>& link : own.links) {
                            rings.push_back(RingPair{&link->commands, &link->responses});
                        }
                        MatchingLoop loop(shard, own.doorbell, std::move(rings), config.spinWindow, std::function<void()>{},
                                          LogLink{&own.logRing, &own.writerBell});
                        loop.run();
                        if (const std::optional<ShardViolation> violation = shard.checkInvariants()) {
                            throw std::runtime_error("the stopped shard, durable position included: " + violation->message);
                        }
                        for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
                            const OrderBook& book = shard.books().book(slot);
                            std::vector<std::byte>& image = own.images.emplace_back(book.imageSize());
                            image.resize(book.writeImage(image));
                        }
                    } catch (...) {
                        failures.record("matching thread " + std::to_string(number));
                    }
                });
            }
            std::atomic<bool> stopRequested{false};
            std::vector<std::thread> gatewayThreads;
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                gatewayThreads.emplace_back([&, thread] {
                    try {
                        runGateway(*gateways[thread], stopRequested);
                    } catch (...) {
                        failures.record("gateway thread " + std::to_string(thread));
                    }
                });
            }

            // The clients, on this thread: chunks of each connection's stream in a seeded order,
            // taking whatever has come back as they go, then waiting for the rest. A failure here
            // still stops and joins every thread before it is reported.
            std::exception_ptr clientFailure;
            try {
            std::mt19937_64 random(config.seed ^ 0x7468726561646564ULL);
            std::vector<std::size_t> sent(clients.size(), 0);
            std::size_t unsent = clients.size();
            while (unsent != 0) {
                const std::uint64_t choice = random();
                const std::size_t connection = choice % clients.size();
                const gateway_testing::Bytes& bytes = streams.bytes[connection];
                if (sent[connection] == bytes.size()) {
                    continue;
                }
                const std::size_t chunk = std::min<std::size_t>(bytes.size() - sent[connection], 1 + (choice >> 16) % 2048);
                clients[connection].send(std::span<const std::byte>(bytes).subspan(sent[connection], chunk));
                sent[connection] += chunk;
                if (sent[connection] == bytes.size()) {
                    --unsent;
                }
                static_cast<void>(clients[connection].frames());
            }
            // Every command is answered once, by a frame flagged last, however many frames the
            // take order gives it.
            for (std::size_t connection = 0; connection < clients.size(); ++connection) {
                while (answeredIn(clients[connection].frames()) < streams.answers[connection]) {
                    if (network.closed(clients[connection].socket)) {
                        throw std::runtime_error("connection " + std::to_string(connection) + " closed before it was answered");
                    }
                    static_cast<void>(clients[connection].framesWaiting());
                }
            }
            } catch (...) {
                clientFailure = std::current_exception();
            }

            // The clean stop, as runEngine makes it.
            stopRequested.store(true, std::memory_order_seq_cst);
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                socketsOf(thread).wake();
            }
            for (std::thread& thread : gatewayThreads) {
                thread.join();
            }
            for (const std::unique_ptr<ShardRun>& shard : shards) {
                shard->thread.join();
            }
            // Each stopped matching thread told its writer to finish, and a writer may wait for
            // its next file until then, so housekeeping stops last.
            for (const std::unique_ptr<ShardRun>& shard : shards) {
                shard->writerThread.join();
            }
            housekeeping.stop();
            housekeeper.join();
            housekeeping.removeUnused();
            if (clientFailure) {
                std::rethrow_exception(clientFailure);
            }
            if (const std::optional<std::string> failure = failures.take()) {
                throw std::runtime_error(*failure);
            }

            // The run's own log, read back from the storage.
            std::map<std::string, std::vector<replay::Command>> logged;
            std::ostringstream log;
            log << "log records per shard:";
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                const ShardLogRecords read = readShardLog(storage, number);
                log << ' ' << read.records.size() << " in " << read.files << " files";
                if (read.records.size() != shards[number]->logEnd || shards[number]->durable.load() != shards[number]->logEnd) {
                    throw std::runtime_error("shard " + std::to_string(number) + "'s files hold " + std::to_string(read.records.size()) +
                                             " records, its writer ended at " + std::to_string(shards[number]->logEnd) +
                                             ", and its durable position is " + std::to_string(shards[number]->durable.load()));
                }
                for (const replay::ReplayRecord& record : read.records) {
                    logged[record.symbol.text()].push_back(record.command);
                }
            }
            // Each shadow book ran every record again as it reached the store, so it equals its
            // shard's book, or the engine isn't deterministic.
            if (const std::optional<std::string> problem = shadows.problem()) {
                result.differences.push_back("the shadow books stopped following the log: " + *problem);
            }
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                if (shadows.records(number) != shards[number]->logEnd) {
                    result.differences.push_back("shard " + std::to_string(number) + "'s shadows ran " +
                                                 std::to_string(shadows.records(number)) + " records of its log's " +
                                                 std::to_string(shards[number]->logEnd));
                }
                const std::span<const Instrument> onShard = directory.onShard(number);
                for (std::size_t slot = 0; slot < onShard.size(); ++slot) {
                    if (shadows.image(onShard[slot].symbol) != shards[number]->images.at(slot)) {
                        result.differences.push_back(onShard[slot].symbol.text() + "'s shadow book differs from its shard's");
                    }
                }
            }

            std::vector<std::vector<Received>> received;
            log << "\ngateway sleeps:";
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                log << ' ' << socketsOf(thread).waits();
                result.gatewaySleeps += socketsOf(thread).waits();
            }
            log << "\nanswers per connection (received/sent, thread):";
            for (std::size_t connection = 0; connection < clients.size(); ++connection) {
                received.push_back(clients[connection].frames());
                const std::optional<std::size_t> holder = network.holder(clients[connection].socket);
                log << ' ' << answeredIn(received.back()) << '/' << streams.answers[connection] << " t"
                    << (holder ? std::to_string(*holder) : std::string("-"));
                if (!network.closed(clients[connection].socket)) {
                    result.differences.push_back("connection " + std::to_string(connection) + " is still open after the stop");
                }
                if (holder != connection % config.gatewayThreads) {
                    result.differences.push_back("connection " + std::to_string(connection) + " ended on the wrong gateway thread");
                }
            }
            result.log = log.str();
            if (config.connectionsPerInstrument > 1) {
                // The take order decides each instrument's answers, so they are held to its log.
                try {
                    const std::vector<std::string> differences = differencesFromSingleThreaded(expectedFromLog(streams, logged), received);
                    result.differences.insert(result.differences.end(), differences.begin(), differences.end());
                } catch (const LogReplayDiverged&) {
                    result.logReplayDiverged = true;
                    throw;
                }
            } else {
                const std::vector<std::string> differences = differencesFromSingleThreaded(streams, received);
                result.differences.insert(result.differences.end(), differences.begin(), differences.end());
                // One connection per instrument, so each instrument's log is its commands in order.
                std::map<std::string, std::vector<replay::Command>> sent;
                for (const StreamCommand& command : streams.commands) {
                    sent[command.accepted.symbol.text()].push_back(command.accepted.command);
                }
                if (logged != sent) {
                    result.differences.push_back("the log doesn't hold exactly each instrument's commands, in order");
                }
            }
            if (config.plant == ThreadedPlant::THREADS_ONLY || config.plant == ThreadedPlant::SHARD) {
                result.differences.push_back("planted: a difference only this run reports");
            }
            if (!result.differences.empty()) {
                result.failure = std::to_string(result.differences.size()) + " differences from " +
                                 (config.connectionsPerInstrument > 1 ? "the log's replay" : "the single-threaded run") + ", first: " +
                                 result.differences.front();
            }
        }

        StepperConfig stepperConfigOf(const ThreadedConfig& config) {
            StepperConfig stepper;
            stepper.streamSeed = config.seed;
            stepper.commands = config.commands;
            stepper.instruments = config.instruments;
            stepper.shards = config.shards;
            stepper.connections = config.instruments;
            stepper.gatewayThreads = config.gatewayThreads;
            stepper.pinned = true;
            stepper.plantExtraAnswer = config.plant == ThreadedPlant::EVERY_RUN;
            return stepper;
        }

        std::string stepperReproduction(const StepperConfig& stepper, const std::vector<std::size_t>& kept) {
            std::string line = "engine_randomized_tests --stepper run --seed " + std::to_string(stepper.seed) + " --stream-seed " +
                               std::to_string(*stepper.streamSeed) + " --commands " + std::to_string(stepper.commands) + " --instruments " +
                               std::to_string(stepper.instruments) + " --shards " + std::to_string(stepper.shards) + " --gateway-threads " +
                               std::to_string(stepper.gatewayThreads) + " --connections " + std::to_string(stepper.connections) +
                               " --placement pinned";
            if (!kept.empty()) {
                line += " --lines ";
                for (std::size_t i = 0; i < kept.size(); ++i) {
                    line += (i == 0 ? "" : ",") + std::to_string(kept[i]);
                }
            }
            return line;
        }

        std::vector<EngineCommand> linesAt(const std::vector<EngineCommand>& all, const std::vector<std::size_t>& kept) {
            std::vector<EngineCommand> lines;
            lines.reserve(kept.size());
            for (const std::size_t index : kept) {
                lines.push_back(all[index]);
            }
            return lines;
        }

        // Removes chunks of lines, halving their size, while the schedule still fails, then single
        // lines until none can go: a 1-minimal stream, unless the budget runs out first.
        bool shrink(const StepperConfig& stepper, const std::vector<EngineCommand>& all, std::vector<std::size_t>& kept,
                    std::size_t attempts) {
            const auto fails = [&](const std::vector<std::size_t>& candidate) {
                return !runStepperSchedule(stepper, linesAt(all, candidate)).passed();
            };
            std::size_t chunk = std::max<std::size_t>(kept.size() / 2, 1);
            for (;;) {
                bool removed = false;
                for (std::size_t start = 0; start < kept.size();) {
                    if (attempts-- == 0) {
                        return false;
                    }
                    std::vector<std::size_t> candidate(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(start));
                    const std::size_t end = std::min(kept.size(), start + chunk);
                    candidate.insert(candidate.end(), kept.begin() + static_cast<std::ptrdiff_t>(end), kept.end());
                    if (!candidate.empty() && fails(candidate)) {
                        kept = std::move(candidate);
                        removed = true;
                    } else {
                        start += chunk;
                    }
                }
                if (chunk == 1 && !removed) {
                    return true;
                }
                chunk = std::max<std::size_t>(chunk / 2, 1);
            }
        }

    }  // namespace

    ThreadedResult runThreadedSchedule(const ThreadedConfig& config) {
        ThreadedResult result;
        try {
            run(config, result);
        } catch (const std::exception& failure) {
            result.failure = failure.what();
        }
        return result;
    }

    std::string threadedReproduction(const ThreadedConfig& config) {
        return "engine_randomized_tests --threads run --seed " + std::to_string(config.seed) + " --commands " + std::to_string(config.commands) +
               " --instruments " + std::to_string(config.instruments) + " --shards " + std::to_string(config.shards) + " --gateway-threads " +
               std::to_string(config.gatewayThreads) + " --ring-slots " + std::to_string(config.ringSlots) + " --spin-window-ns " +
               std::to_string(config.spinWindow.count()) +
               (config.connectionsPerInstrument > 1 ? " --connections-per-instrument " + std::to_string(config.connectionsPerInstrument)
                                                    : std::string());
    }

    ThreadedTriage triageThreadedFailure(const ThreadedConfig& config, const ThreadedResult& result, std::size_t stepperSchedules,
                                         std::size_t shrinkAttempts) {
        ThreadedTriage triage{ThreadedTriage::Verdict::TIMING_DEPENDENT, std::nullopt, {}, false, {}};
        std::ostringstream report;
        report << "real-thread failure: " << result.failure.value_or("none") << "\nsaved: " << threadedReproduction(config) << "\n  N="
               << config.shards << " M=" << config.gatewayThreads << " rings="
               << (config.ringSlots == 0 ? std::to_string(defaultCommandRingSlots) + "/" + std::to_string(defaultResponseRingSlots)
                                         : std::to_string(config.ringSlots) + "/" + std::to_string(config.ringSlots))
               << " spin window=" << config.spinWindow.count() << "ns\n";
        for (const std::string& difference : result.differences) {
            report << "  " << difference << '\n';
        }
        if (config.connectionsPerInstrument > 1) {
            // The run's own log was already replayed single-threaded, through a book and the
            // model; a disagreement there is a logic bug. The stepper holds a connection per
            // instrument to the single-threaded run, so it can't replay this take order.
            if (result.logReplayDiverged) {
                triage.verdict = ThreadedTriage::Verdict::LOGIC_BUG;
                report << "the run's own log, replayed single-threaded, differs between the book and the model: a logic bug, unshrunk\n";
            } else {
                report << "several connections on one instrument: the run's log replays single-threaded as the model does, and no"
                          " stepper schedule replays this take order, so it is timing-dependent and unshrunk\n";
            }
            report << "the run's log:\n" << result.log << '\n';
            triage.report = report.str();
            return triage;
        }

        EngineSeedConfig single = generationOf(config);
        if (config.plant == ThreadedPlant::SHARD) {
            single.plantDivergenceBefore = 0;
        }
        const EngineRunResult rerun = runEngineSeed(single);
        if (const std::optional<EngineFailure>& failure = rerun.failure) {
            triage.verdict = ThreadedTriage::Verdict::LOGIC_BUG;
            report << "single-threaded Shard::process fails too, at command " << failure->commandIndex << " of instrument "
                   << failure->instrument << ": " << failure->reason << "\na logic bug, unshrunk, since the engine harness has no shrinker"
                   << " yet; reproduce with: engine_randomized_tests --seed " << single.seed << " --commands " << single.commands
                   << " --instruments " << single.instruments << " --shards " << single.shards << '\n';
            triage.report = report.str();
            return triage;
        }
        report << "single-threaded Shard::process passes\n";

        const std::vector<EngineCommand> all = generateEngineStream(generationOf(config));
        StepperConfig stepper = stepperConfigOf(config);
        for (std::uint64_t schedule = 1; schedule <= stepperSchedules; ++schedule) {
            stepper.seed = schedule;
            const StepperResult stepped = runStepperSchedule(stepper, all);
            if (stepped.passed()) {
                continue;
            }
            triage.verdict = ThreadedTriage::Verdict::DETERMINISTIC;
            triage.stepperSeed = schedule;
            report << "stepper schedule " << schedule << " of " << stepperSchedules << " reproduces it: " << *stepped.failure
                   << "\nreproduce with: " << stepperReproduction(stepper, {}) << '\n';
            std::vector<std::size_t> kept(all.size());
            for (std::size_t i = 0; i < kept.size(); ++i) {
                kept[i] = i;
            }
            triage.shrunkInFull = shrink(stepper, all, kept, shrinkAttempts);
            triage.keptLines = kept;
            report << "shrunk " << (triage.shrunkInFull ? "in full" : "until the budget ran out") << " to " << kept.size() << " of "
                   << all.size() << " lines: " << stepperReproduction(stepper, kept) << '\n';
            triage.report = report.str();
            return triage;
        }
        report << "none of " << stepperSchedules << " stepper schedules reproduces it: timing-dependent, unshrunk\nthe run's log:\n"
               << result.log << '\n';
        triage.report = report.str();
        return triage;
    }

}  // namespace order_book::engine_testing
