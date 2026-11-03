#include "threaded_run.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
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
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

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
            return generation;
        }

        StreamShape shapeOf(const ThreadedConfig& config) {
            return StreamShape{config.instruments, config.shards, config.instruments, true, config.plant == ThreadedPlant::EVERY_RUN};
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
            Doorbell doorbell;
            std::vector<std::unique_ptr<RingLink>> links;  // one per gateway thread
            std::thread thread;
        };

        void run(const ThreadedConfig& config, ThreadedResult& result) {
            const std::vector<EngineCommand> lines = generateEngineStream(generationOf(config));
            const ConnectionStreams streams = connectionStreams(shapeOf(config), lines);
            const InstrumentDirectory directory(streams.instruments, config.shards);
            const std::size_t commandSlots = config.ringSlots == 0 ? defaultCommandRingSlots : config.ringSlots;
            const std::size_t responseSlots = config.ringSlots == 0 ? defaultResponseRingSlots : config.ringSlots;

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
            clients.reserve(config.instruments);
            for (std::size_t i = 0; i < config.instruments; ++i) {
                clients.emplace_back(network);
            }

            Failures failures;
            std::vector<std::unique_ptr<ShardRun>> shards;
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                shards.push_back(std::make_unique<ShardRun>());
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
                    links.push_back(ShardLink{&shard->links[thread]->commands, &shard->links[thread]->responses, &shard->doorbell});
                }
                gateways.push_back(std::make_unique<GatewayThread>(directory, std::move(links), socketsOf(thread), settings, &handOff, thread));
            }

            // Each shard is built on its own thread, which owns its books.
            for (std::uint32_t number = 0; number < config.shards; ++number) {
                ShardRun& own = *shards[number];
                own.thread = std::thread([&, number] {
                    try {
                        Shard shard(directory, number);
                        std::vector<RingPair> rings;
                        for (const std::unique_ptr<RingLink>& link : own.links) {
                            rings.push_back(RingPair{&link->commands, &link->responses});
                        }
                        MatchingLoop loop(shard, own.doorbell, std::move(rings), config.spinWindow);
                        loop.run();
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
            for (std::size_t connection = 0; connection < clients.size(); ++connection) {
                while (clients[connection].frames().size() < streams.frames[connection]) {
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
            if (clientFailure) {
                std::rethrow_exception(clientFailure);
            }
            if (const std::optional<std::string> failure = failures.take()) {
                throw std::runtime_error(*failure);
            }

            std::vector<std::vector<Received>> received;
            std::ostringstream log;
            log << "gateway sleeps:";
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                log << ' ' << socketsOf(thread).waits();
                result.gatewaySleeps += socketsOf(thread).waits();
            }
            log << "\nframes per connection (received/expected, thread):";
            for (std::size_t connection = 0; connection < clients.size(); ++connection) {
                received.push_back(clients[connection].frames());
                const std::optional<std::size_t> holder = network.holder(clients[connection].socket);
                log << ' ' << received.back().size() << '/' << streams.frames[connection] << " t"
                    << (holder ? std::to_string(*holder) : std::string("-"));
                if (!network.closed(clients[connection].socket)) {
                    result.differences.push_back("connection " + std::to_string(connection) + " is still open after the stop");
                }
                if (holder != connection % config.gatewayThreads) {
                    result.differences.push_back("connection " + std::to_string(connection) + " ended on the wrong gateway thread");
                }
            }
            result.log = log.str();
            const std::vector<std::string> differences = differencesFromSingleThreaded(streams, received);
            result.differences.insert(result.differences.end(), differences.begin(), differences.end());
            if (config.plant == ThreadedPlant::THREADS_ONLY || config.plant == ThreadedPlant::SHARD) {
                result.differences.push_back("planted: a difference only this run reports");
            }
            if (!result.differences.empty()) {
                result.failure = std::to_string(result.differences.size()) + " differences from the single-threaded run, first: " +
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
               std::to_string(config.spinWindow.count());
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
