#include "gateway_stepper.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "connection_streams.hpp"
#include "engine_harness.hpp"
#include "gateway_frames.hpp"
#include "gateway_rig.hpp"
#include "order_book/command_log.hpp"
#include "order_book/little_endian.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/runtime/log_storage.hpp"

namespace order_book::engine_testing {

    namespace {

        using namespace order_book::network;
        using namespace order_book::runtime;
        using order_book::gateway_testing::Bytes;
        using order_book::gateway_testing::helloFrame;

        void check(bool condition, const std::string& message) {
            if (!condition) {
                throw std::runtime_error(message);
            }
        }

        StreamShape shapeOf(const StepperConfig& config) {
            return StreamShape{config.instruments, config.shards, config.connections, config.pinned, config.plantExtraAnswer};
        }

        // Sleeping is only safe with nothing in a ring, and with the wait watching every connection
        // that still has bytes to move: written to, if it is owed answers, counted here from the
        // bytes the gateway took and the frames the client holds, and read from, if it sent bytes
        // not yet taken. None of it comes from what the gateway says about itself.
        void checkNothingInFlight(Rig& rig, std::size_t thread, std::vector<Client>& clients, std::span<const SocketInterest> interest) {
            for (const std::unique_ptr<ShardActor>& shard : rig.shards) {
                const RingLink& link = *shard->links[thread];
                check(!link.commands.hasItems() && !link.responses.hasItems(), "a gateway slept with a command in flight");
            }
            if (thread != 0 && rig.handOff->ringOf(thread).hasItems()) {
                check(rig.socketsOf(thread).wakePending(), "a gateway slept on a connection handed to it, unwoken");
            }
            const auto watched = [&interest](SocketId socket, bool forWriting) {
                return std::any_of(interest.begin(), interest.end(), [&](const SocketInterest& wanted) {
                    return wanted.socket == socket && (forWriting ? wanted.write : wanted.read || wanted.write);
                });
            };
            for (Client& client : clients) {
                if (rig.sockets.holder(client.socket) != thread || rig.sockets.closed(client.socket)) {
                    // Another thread's, or not yet accepted, which wakes a real wait through the
                    // listener, or handed on, which the wake checked above covers.
                    continue;
                }
                const std::size_t taken = rig.sockets.bytesRead(client.socket);
                const std::size_t commandsRead = taken < helloFrame().size() ? 0 : (taken - helloFrame().size()) / 48;
                if (client.answered() != commandsRead) {
                    check(watched(client.socket, true), "a gateway slept owing a connection answers it wasn't watching to send");
                }
                if (rig.sockets.bytesSent(client.socket) != taken) {
                    check(watched(client.socket, false), "a gateway slept on bytes it wasn't watching to read");
                }
            }
        }

        // Follows each shard's log in the storage as it grows, decoding each record through the
        // log's own reader functions, so what counts as logged comes from the bytes the storage
        // holds, never from what the log writer reports.
        class LogFollower {
        public:
            explicit LogFollower(Rig& rig) {
                for (const std::unique_ptr<ShardActor>& shard : rig.shards) {
                    const std::string& name = shard->writer->fileName();
                    const std::vector<std::byte> header = rig.logStorage.read(name, 0);  // all it holds yet
                    files.push_back(FileState{name, header.size(), replay::logChainSeed(header), 0});
                }
            }

            void follow(Rig& rig) {
                for (std::size_t shard = 0; shard < files.size(); ++shard) {
                    FileState& file = files[shard];
                    const std::vector<std::byte> bytes = rig.logStorage.read(file.name, file.offset);
                    const std::size_t whole = bytes.size() / replay::logRecordBytes;
                    for (std::size_t i = 0; i < whole; ++i) {
                        const std::span<const std::byte> record = std::span<const std::byte>(bytes).subspan(i * replay::logRecordBytes, replay::logRecordBytes);
                        const replay::ParseResult decoded = replay::decodeLogRecord(record, file.checksum, file.records, file.records);
                        const auto* read = std::get_if<replay::ReplayRecord>(&decoded);
                        check(read != nullptr, "shard " + std::to_string(shard) + "'s log reads back record by record");
                        logged[read->symbol.text()].push_back(read->command);
                        file.checksum = little_endian::load32(record, 0);
                        ++file.records;
                    }
                    file.offset += whole * replay::logRecordBytes;
                    check(rig.shards[shard]->durable.load() <= file.records, "the durable position never passes what the storage holds");
                }
            }

            std::map<std::string, std::vector<replay::Command>> logged;

        private:
            struct FileState {
                std::string name;
                std::size_t offset;  // past what has been decoded
                std::uint32_t checksum;
                std::uint64_t records;
            };
            std::vector<FileState> files;
        };

        // Where each command that passed entry stands in its instrument's log: one connection owns
        // each instrument and sends in order, so its k-th such command that runs is its
        // instrument's k-th record.
        std::map<Correlation, std::pair<std::string, std::size_t>> logPlaces(const ConnectionStreams& stream) {
            std::map<std::string, std::size_t> counts;
            std::map<Correlation, std::pair<std::string, std::size_t>> places;
            for (const StreamCommand& command : stream.commands) {
                const std::string symbol = command.accepted.symbol.text();
                places[command.correlation] = {symbol, counts[symbol]++};
            }
            return places;
        }

        // No client holds an answer to a command whose record the storage doesn't hold yet.
        void checkReleasedAreLogged(const std::map<Correlation, std::pair<std::string, std::size_t>>& places, const LogFollower& follower,
                                    std::vector<Client>& clients, std::vector<std::size_t>& checked) {
            for (std::size_t i = 0; i < clients.size(); ++i) {
                const std::vector<Received>& frames = clients[i].frames();
                for (; checked[i] < frames.size(); ++checked[i]) {
                    const Received& frame = frames[checked[i]];
                    const auto place = places.find(frame.correlation);
                    if (!frame.event || place == places.end()) {
                        continue;  // answered by entry or refused, so never logged
                    }
                    const auto logged = follower.logged.find(place->second.first);
                    const std::size_t records = logged == follower.logged.end() ? 0 : logged->second.size();
                    check(place->second.second < records, "a response to " + place->second.first + "'s command " +
                                                              std::to_string(place->second.second) + " was released with only " +
                                                              std::to_string(records) + " of its records in the log");
                }
            }
        }

        // Each instrument's log holds exactly these commands, in order.
        void checkLogHolds(const LogFollower& follower, const std::map<std::string, std::vector<replay::Command>>& ran) {
            for (const auto& [symbol, commands] : ran) {
                const auto logged = follower.logged.find(symbol);
                check(logged != follower.logged.end() && logged->second == commands, symbol + "'s log holds exactly the commands that ran, in order");
            }
            for (const auto& [symbol, commands] : follower.logged) {
                check(ran.contains(symbol), symbol + " is logged but nothing of it ran");
            }
        }

        void checkUnstopped(const ConnectionStreams& stream, std::vector<Client>& clients) {
            std::vector<std::vector<Received>> received;
            for (Client& client : clients) {
                received.push_back(client.frames());
            }
            const std::vector<std::string> differences = differencesFromSingleThreaded(stream, received);
            check(differences.empty(), "every instrument's answers equal the single-threaded run's, but " +
                                           (differences.empty() ? std::string() : differences.front()));
        }

        void checkStopped(const StepperConfig& config, const ConnectionStreams& stream, Rig& rig, std::vector<Client>& clients,
                          StepperResult& result, const LogFollower* follower) {
            std::map<std::string, std::vector<replay::Command>> ranCommands;
            std::map<std::string, std::vector<Line>> answered;
            std::set<Correlation> ran;
            std::set<Correlation> refusedByStop;
            for (Client& client : clients) {
                for (const Received& frame : client.frames()) {
                    if (refusedForStopping(frame)) {
                        refusedByStop.insert(frame.correlation);
                    } else if (frame.event && !stream.refusedAsMalformed.contains(frame.correlation)) {
                        ran.insert(frame.correlation);
                        answered[frame.event->symbol.text()].push_back(Line{frame.correlation, frame.event->event, frame.last});
                    }
                }
                check(rig.sockets.closed(client.socket), "a finished gateway has closed every connection");
            }

            const InstrumentDirectory directory(stream.instruments, config.shards);
            std::vector<std::unique_ptr<Shard>> witnesses;
            for (std::uint32_t shard = 0; shard < config.shards; ++shard) {
                witnesses.push_back(std::make_unique<Shard>(directory, shard));
            }
            std::map<std::string, std::vector<Line>> expected;
            // Per connection, ran, then refused by the stop, then never read: a stage only moves on.
            enum Stage { RAN, REFUSED, UNREAD };
            std::vector<Stage> stage(config.connections, RAN);
            for (const StreamCommand& command : stream.commands) {
                const bool didRun = ran.contains(command.correlation);
                const bool refused = refusedByStop.contains(command.correlation);
                check(!(didRun && refused), "no command both ran and was refused");
                const Stage now = didRun ? RAN : refused ? REFUSED : UNREAD;
                check(now >= stage[command.connection], "a connection's commands ran, were refused, and went unread, out of that order");
                stage[command.connection] = now;
                if (now == UNREAD) {
                    check(command.frameEnd > rig.sockets.bytesRead(clients[command.connection].socket),
                          "a command the gateway read got no answer");
                    continue;
                }
                if (now == REFUSED) {
                    continue;
                }
                ranCommands[command.accepted.symbol.text()].push_back(command.accepted.command);
                Shard& witness = *witnesses[command.accepted.location.shard];
                witness.process(command.accepted.command, command.accepted.location.slot);
                const std::span<const BookEvent> events = witness.result().events();
                for (std::size_t i = 0; i < events.size(); ++i) {
                    expected[command.accepted.symbol.text()].push_back(Line{command.correlation, events[i], i + 1 == events.size()});
                }
            }
            check(answered == expected, "what ran equals a single-threaded run of those commands");
            if (follower != nullptr) {
                checkLogHolds(*follower, ranCommands);
            }
            result.commandsRun = ran.size();
            result.commandsRefused = refusedByStop.size();
        }

        void runSchedule(const StepperConfig& config, const std::vector<EngineCommand>& lines, StepperResult& result) {
            const ConnectionStreams stream = connectionStreams(shapeOf(config), lines);
            Rig rig(stream.instruments, config.shards, 2, {}, config.gatewayThreads,
                    config.log ? std::optional<Durability>(Durability::MACHINE) : std::nullopt);
            std::optional<LogFollower> follower;
            std::map<Correlation, std::pair<std::string, std::size_t>> places;
            std::vector<std::size_t> checkedFrames(config.connections, 0);
            if (config.log) {
                follower.emplace(rig);
                places = logPlaces(stream);
            }
            std::mt19937_64 random(config.seed);
            std::vector<Client> clients;
            clients.reserve(config.connections);
            for (std::size_t i = 0; i < config.connections; ++i) {
                clients.emplace_back(rig.sockets);
            }
            for (std::size_t thread = 0; thread < config.gatewayThreads; ++thread) {
                rig.socketsOf(thread).onWait([&, thread](std::span<const SocketInterest> interest) {
                    checkNothingInFlight(rig, thread, clients, interest);
                    ++result.sleeps;
                });
            }
            const auto everyGatewayFinished = [&rig] {
                return std::all_of(rig.gateways.begin(), rig.gateways.end(),
                                   [](const std::unique_ptr<GatewayThread>& gateway) { return gateway->finished(); });
            };

            const auto answeredInFull = [&] {
                for (std::size_t i = 0; i < config.connections; ++i) {
                    if (clients[i].frames().size() != stream.frames[i]) {
                        return false;
                    }
                }
                return true;
            };
            const std::uint64_t stopAt = config.stop ? 1 + random() % 3000 : 0;
            std::vector<std::size_t> sent(config.connections, 0);
            std::size_t steps = 0;
            bool stopRequested = false;
            while (config.stop ? !everyGatewayFinished() : !answeredInFull()) {
                check(++steps < 40'000'000, "the stepped run stopped making progress");
                stopRequested = stopRequested || steps == stopAt;
                const std::uint64_t choice = random();
                // A log writer is a fourth actor. Without a log the schedules stay what they were.
                if (config.log && choice % 4 == 3) {
                    rig.writeLog(static_cast<std::uint32_t>((choice >> 8) % config.shards));
                } else if (choice % 3 == 0) {
                    const std::size_t connection = (choice >> 8) % config.connections;
                    const Bytes& bytes = stream.bytes[connection];
                    const std::size_t chunk = std::min<std::size_t>(bytes.size() - sent[connection], 1 + (choice >> 16) % 150);
                    clients[connection].send(std::span<const std::byte>(bytes).subspan(sent[connection], chunk));
                    sent[connection] += chunk;
                } else if (choice % 3 == 1) {
                    rig.sockets.limitRead(1 + (choice >> 8) % 200);
                    rig.sockets.limitWrite(1 + (choice >> 16) % 200);
                    GatewayThread& gateway = *rig.gateways[(choice >> 40) % config.gatewayThreads];
                    if (stopRequested) {
                        gateway.beginStop();
                    }
                    gateway.pass();
                } else {
                    rig.match(static_cast<std::uint32_t>((choice >> 8) % config.shards));
                }
                if (follower) {
                    follower->follow(rig);
                    checkReleasedAreLogged(places, *follower, clients, checkedFrames);
                }
            }
            if (config.stop) {
                if (follower) {
                    // Every shard has stopped, so each log writer has its STOP, and finishes.
                    for (std::uint32_t shard = 0; shard < config.shards; ++shard) {
                        rig.writeLog(shard);
                        const ShardActor& actor = *rig.shards[shard];
                        check(actor.writer->stopped(), "a stopped shard's log writer finishes");
                        const std::optional<InMemoryLogStorage::File> file = rig.logStorage.file(actor.writer->fileName());
                        check(file && file->closed && file->flushes == 2, "flushing and closing its file, flushed once already when it was made");
                        check(!actor.shard.checkInvariants(), "and the shard, durable position included, is sound");
                    }
                    follower->follow(rig);
                }
                checkStopped(config, stream, rig, clients, result, follower ? &*follower : nullptr);
            } else {
                checkUnstopped(stream, clients);
                if (follower) {
                    std::map<std::string, std::vector<replay::Command>> everything;
                    for (const StreamCommand& command : stream.commands) {
                        everything[command.accepted.symbol.text()].push_back(command.accepted.command);
                    }
                    checkLogHolds(*follower, everything);
                    for (const std::unique_ptr<ShardActor>& shard : rig.shards) {
                        check(!shard->shard.checkInvariants(), "every shard, durable position included, is sound");
                    }
                }
            }
            for (std::size_t i = 0; i < clients.size(); ++i) {
                const std::optional<std::size_t> holder = rig.sockets.holder(clients[i].socket);
                // A stop can close a connection unread anywhere: never accepted, held back for a
                // full ring, or mid-hand-off.
                const bool closedUnread = rig.sockets.closed(clients[i].socket) && rig.sockets.bytesRead(clients[i].socket) == 0;
                check(holder == i % config.gatewayThreads || (config.stop && closedUnread),
                      "connection " + std::to_string(i) + " ended on the wrong gateway thread");
            }
            result.waitsInPlace = rig.waitsInPlace;
        }

    }  // namespace

    StepperResult runStepperSchedule(const StepperConfig& config) {
        EngineSeedConfig generation;
        generation.seed = config.streamSeed.value_or(config.seed);
        generation.instruments = config.instruments;
        generation.shards = config.shards;
        generation.commands = config.commands;
        generation.logWritePercent = 0;  // its log writers are actors of their own
        generation.crashes = false;
        return runStepperSchedule(config, generateEngineStream(generation));
    }

    StepperResult runStepperSchedule(const StepperConfig& config, const std::vector<EngineCommand>& lines) {
        StepperResult result;
        try {
            runSchedule(config, lines, result);
        } catch (const std::exception& failure) {
            result.failure = failure.what();
        }
        return result;
    }

}  // namespace order_book::engine_testing
