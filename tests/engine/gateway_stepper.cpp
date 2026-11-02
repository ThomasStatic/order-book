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

#include "engine_harness.hpp"
#include "gateway_frames.hpp"
#include "gateway_rig.hpp"
#include "order_book/network/entry_check.hpp"

namespace order_book::engine_testing {

    namespace {

        using namespace order_book::network;
        using namespace order_book::runtime;
        using order_book::gateway_testing::Bytes;
        using order_book::gateway_testing::helloFrame;
        using order_book::gateway_testing::rawCommandFrame;

        void check(bool condition, const std::string& message) {
            if (!condition) {
                throw std::runtime_error(message);
            }
        }

        struct Line {
            Correlation correlation;
            BookEvent event;
            bool last;

            bool operator==(const Line&) const = default;
        };

        // A command the entry check passed, and where its frame ends in its connection's stream.
        struct StreamCommand {
            Correlation correlation;
            std::size_t connection;
            std::size_t frameEnd;
            EntryAccepted accepted;
        };

        struct SteppedStream {
            std::vector<Instrument> instruments;
            std::vector<Bytes> bytes;                           // per connection
            std::vector<std::size_t> frames;                    // the frames expected back, per connection
            std::map<std::string, std::vector<Line>> expected;  // from the books, per instrument
            std::map<Correlation, BookEvent> refusedAsMalformed;
            std::set<Correlation> refusedAtEntry;               // with a Refused frame
            std::vector<StreamCommand> commands;
        };

        SteppedStream steppedStream(const StepperConfig& config) {
            EngineSeedConfig generation;
            generation.seed = config.seed;
            generation.instruments = config.instruments;
            generation.shards = config.shards;
            generation.commands = config.commands;

            SteppedStream stream;
            std::vector<Symbol> symbols;
            for (std::size_t i = 0; i < config.instruments; ++i) {
                symbols.push_back(*Symbol::parse("SYM" + std::to_string(i)));
                stream.instruments.push_back(Instrument{symbols.back(), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            }
            const InstrumentDirectory directory(stream.instruments, config.shards);
            std::vector<std::unique_ptr<Shard>> witnesses;
            for (std::uint32_t shard = 0; shard < config.shards; ++shard) {
                witnesses.push_back(std::make_unique<Shard>(directory, shard));
            }

            stream.bytes.assign(config.connections, helloFrame());
            stream.frames.assign(config.connections, 1);  // each gets a Welcome
            const std::vector<EngineCommand> lines = generateEngineStream(generation);
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const EngineCommand& line = lines[index];
                if (!line.converted) {
                    continue;
                }
                const Correlation correlation = index + 1;
                const std::array<char, Symbol::size> stored =
                    line.fault == SymbolFault::NONE ? symbols[line.instrument].bytes() : Symbol::parse("ZZZZ")->bytes();
                const replay::RawCommand raw = rawCommandOf(*line.converted, stored);
                const std::size_t connection = line.instrument % config.connections;
                const Bytes frame = rawCommandFrame(correlation, raw);
                check(order_book::gateway_testing::roundTrips(frame, raw), "the stepper's frame is the command");
                stream.bytes[connection].insert(stream.bytes[connection].end(), frame.begin(), frame.end());

                const EntryDecision decision = checkEntry(CommandFrame{correlation, raw}, directory, false);
                if (std::holds_alternative<EntryRefused>(decision)) {
                    stream.refusedAtEntry.insert(correlation);
                    ++stream.frames[connection];
                } else if (const EntryMalformed* malformed = std::get_if<EntryMalformed>(&decision)) {
                    stream.refusedAsMalformed.insert({correlation, malformed->event});
                    ++stream.frames[connection];
                } else {
                    const EntryAccepted& accepted = std::get<EntryAccepted>(decision);
                    stream.commands.push_back(StreamCommand{correlation, connection, stream.bytes[connection].size(), accepted});
                    Shard& witness = *witnesses[accepted.location.shard];
                    witness.process(accepted.command, accepted.location.slot);
                    const std::span<const BookEvent> events = witness.result().events();
                    for (std::size_t i = 0; i < events.size(); ++i) {
                        stream.expected[symbols[line.instrument].text()].push_back(Line{correlation, events[i], i + 1 == events.size()});
                        ++stream.frames[connection];
                    }
                }
            }
            return stream;
        }

        // Sleeping is only safe with nothing in a ring, and with the wait watching every connection
        // that still has bytes to move: written to, if it is owed answers, counted here from the
        // bytes the gateway took and the frames the client holds, and read from, if it sent bytes
        // not yet taken. None of it comes from what the gateway says about itself.
        void checkNothingInFlight(Rig& rig, std::vector<Client>& clients, std::span<const SocketInterest> interest) {
            for (const std::unique_ptr<ShardActor>& shard : rig.shards) {
                check(!shard->commands.hasItems() && !shard->responses.hasItems(), "a gateway slept with a command in flight");
            }
            const auto watched = [&interest](SocketId socket, bool forWriting) {
                return std::any_of(interest.begin(), interest.end(), [&](const SocketInterest& wanted) {
                    return wanted.socket == socket && (forWriting ? wanted.write : wanted.read || wanted.write);
                });
            };
            for (Client& client : clients) {
                if (!rig.sockets.accepted(client.socket) || rig.sockets.closed(client.socket)) {
                    continue;  // a connection not yet accepted wakes a real wait through the listener
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

        void checkUnstopped(const SteppedStream& stream, std::vector<Client>& clients) {
            std::map<std::string, std::vector<Line>> answered;
            std::map<Correlation, BookEvent> malformed;
            std::set<Correlation> refused;
            for (Client& client : clients) {
                for (const Received& frame : client.frames()) {
                    if (frame.type == FrameType::REFUSED) {
                        refused.insert(frame.correlation);
                    } else if (frame.event && stream.refusedAsMalformed.contains(frame.correlation)) {
                        check(frame.last, "a malformed command's refusal is one whole answer");
                        malformed.insert({frame.correlation, frame.event->event});
                    } else if (frame.event) {
                        answered[frame.event->symbol.text()].push_back(Line{frame.correlation, frame.event->event, frame.last});
                    }
                }
            }
            check(answered == stream.expected, "every instrument's answers equal the single-threaded run's");
            check(malformed == stream.refusedAsMalformed, "and so do the malformed commands' refusals");
            check(refused == stream.refusedAtEntry, "and the Refused frames");
        }

        void checkStopped(const StepperConfig& config, const SteppedStream& stream, Rig& rig, std::vector<Client>& clients,
                          StepperResult& result) {
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
                Shard& witness = *witnesses[command.accepted.location.shard];
                witness.process(command.accepted.command, command.accepted.location.slot);
                const std::span<const BookEvent> events = witness.result().events();
                for (std::size_t i = 0; i < events.size(); ++i) {
                    expected[command.accepted.symbol.text()].push_back(Line{command.correlation, events[i], i + 1 == events.size()});
                }
            }
            check(answered == expected, "what ran equals a single-threaded run of those commands");
            result.commandsRun = ran.size();
            result.commandsRefused = refusedByStop.size();
        }

        void runSchedule(const StepperConfig& config, StepperResult& result) {
            const SteppedStream stream = steppedStream(config);
            Rig rig(stream.instruments, config.shards, 2);
            std::mt19937_64 random(config.seed);
            std::vector<Client> clients;
            clients.reserve(config.connections);
            for (std::size_t i = 0; i < config.connections; ++i) {
                clients.emplace_back(rig.sockets);
            }
            rig.sockets.onWait([&](std::span<const SocketInterest> interest) {
                checkNothingInFlight(rig, clients, interest);
                ++result.sleeps;
            });

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
            while (config.stop ? !rig.gateway->finished() : !answeredInFull()) {
                check(++steps < 40'000'000, "the stepped run stopped making progress");
                if (steps == stopAt) {
                    rig.gateway->beginStop();
                }
                const std::uint64_t choice = random();
                if (choice % 3 == 0) {
                    const std::size_t connection = (choice >> 8) % config.connections;
                    const Bytes& bytes = stream.bytes[connection];
                    const std::size_t chunk = std::min<std::size_t>(bytes.size() - sent[connection], 1 + (choice >> 16) % 150);
                    clients[connection].send(std::span<const std::byte>(bytes).subspan(sent[connection], chunk));
                    sent[connection] += chunk;
                } else if (choice % 3 == 1) {
                    rig.sockets.limitRead(1 + (choice >> 8) % 200);
                    rig.sockets.limitWrite(1 + (choice >> 16) % 200);
                    rig.gateway->pass();
                } else {
                    rig.match(static_cast<std::uint32_t>((choice >> 8) % config.shards));
                }
            }
            if (config.stop) {
                checkStopped(config, stream, rig, clients, result);
            } else {
                checkUnstopped(stream, clients);
            }
            result.waitsInPlace = rig.waitsInPlace;
        }

    }  // namespace

    StepperResult runStepperSchedule(const StepperConfig& config) {
        StepperResult result;
        try {
            runSchedule(config, result);
        } catch (const std::exception& failure) {
            result.failure = failure.what();
        }
        return result;
    }

}  // namespace order_book::engine_testing
