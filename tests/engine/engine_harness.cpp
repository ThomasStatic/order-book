#include "engine_harness.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include "command_generator.hpp"
#include "command_replayer.hpp"
#include "order_book/event_format.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book::engine_testing {
    namespace {
        using randomized::Command;
        using randomized::CommandIntent;
        using randomized::CommandType;
        using randomized::RandomCommandGenerator;
        using randomized::ReferenceBook;
        using randomized::SubmitOverload;
        using reference::ExpectedOutcome;

        // Mixes a seed into a source of its own: the interleaver's, the faults', and each
        // instrument's past the first.
        constexpr std::uint64_t interleaverStream = 0x9E3779B97F4A7C15ULL;
        constexpr std::uint64_t faultStream = 0xC2B2AE3D27D4EB4FULL;
        constexpr std::uint64_t logStream = 0x165667B19E3779F9ULL;

        Symbol symbolOf(std::size_t instrument) {
            return *Symbol::parse("I" + std::to_string(instrument));
        }

        std::vector<std::string> profilesOf(const EngineSeedConfig& config) {
            if (!config.profiles.empty()) {
                return config.profiles;
            }
            std::vector<std::string> names;
            for (const randomized::WorkloadProfile& profile : randomized::workloadProfiles()) {
                names.push_back(profile.name);
            }
            return names;
        }

        std::uint64_t instrumentSeed(std::uint64_t seed, std::size_t instrument) {
            return instrument == 0 ? seed : seed + instrument * interleaverStream;
        }

        std::uint8_t byteOf(auto enumerator) {
            return static_cast<std::uint8_t>(static_cast<int>(enumerator) + 1);
        }

        std::array<char, Symbol::size> faultySymbol(SymbolFault fault, const Symbol& symbol) {
            if (fault == SymbolFault::UNKNOWN) {
                return Symbol::parse("NOSUCH")->bytes();
            }
            if (fault == SymbolFault::MALFORMED) {
                std::array<char, Symbol::size> stored{};
                std::memcpy(stored.data(), "nosuch  ", stored.size());
                return stored;
            }
            return symbol.bytes();
        }

        std::vector<BookEvent> eventsOf(const replay::CommandOutcome& outcome) {
            return std::visit(
                [](const auto& result) {
                    const auto& events = result.getEvents();
                    return std::vector<BookEvent>(events.begin(), events.end());
                },
                outcome);
        }

        std::string describe(std::span<const BookEvent> events) {
            std::string text;
            for (const BookEvent& event : events) {
                text += formatEvent(event) + "; ";
            }
            return text.empty() ? "no events" : text;
        }

        bool sameEvents(std::span<const BookEvent> left, std::span<const BookEvent> right) {
            return std::equal(left.begin(), left.end(), right.begin(), right.end());
        }

        // One shard's log: its ring, as the matching thread fills it, its durable position, and
        // its log writer on the run's in-memory storage.
        struct ShardLog {
            ShardLog(runtime::LogStorage& storage, const runtime::InstrumentDirectory& directory, std::uint32_t shard, std::size_t records)
                : ring(records), writer(storage, directory, shard, runtime::Durability::MACHINE, ring, durable) {}

            runtime::Ring<runtime::LogEntry> ring;
            runtime::DurablePosition durable;
            runtime::LogWriter writer;
        };

        // Everything that runs one instrument, beside its shard's book.
        struct Instrument {
            Symbol symbol;
            runtime::BookLocation location;
            ReferenceBook model;
            OrderBook witness;
        };

        class Run {
        public:
            explicit Run(const EngineSeedConfig& seedConfig) : config(seedConfig), directory(listOf(seedConfig), seedConfig.shards) {
                for (std::uint32_t shard = 0; shard < config.shards; ++shard) {
                    shards.push_back(std::make_unique<runtime::Shard>(directory, shard, config.shardCapacity));
                    logs.push_back(std::make_unique<ShardLog>(storage, directory, shard, config.logRingRecords));
                    shards.back()->watchDurablePosition(logs.back()->durable);
                }
                instruments.reserve(config.instruments);
                for (std::size_t i = 0; i < config.instruments; ++i) {
                    const Symbol symbol = symbolOf(i);
                    instruments.push_back(Instrument{symbol, *directory.find(symbol), ReferenceBook{}, OrderBook{}});
                    instruments.back().witness.reserve(config.witnessCapacity);
                }
            }

            EngineRunResult run(const std::vector<EngineCommand>& stream) {
                EngineRunResult result;
                for (std::size_t index = 0; index < stream.size() && !result.failure; ++index) {
                    if (config.plantDivergenceBefore == index) {
                        plantDivergence();
                    }
                    if (stream[index].action == EngineAction::LOG) {
                        writeLogs(result.stats);
                        continue;
                    }
                    result.failure = execute(stream[index], index, result.stats);
                }
                if (!result.failure) {
                    result.failure = finish(stream.size());
                }
                if (!result.failure) {
                    result.failure = finishLogs(stream.size(), result.stats);
                }
                for (const Instrument& instrument : instruments) {
                    result.stats.shardGrowths +=
                        shards[instrument.location.shard]->books().book(instrument.location.slot).capacityGrowths();
                    result.stats.witnessGrowths += instrument.witness.capacityGrowths();
                }
                return result;
            }

        private:
            static std::vector<runtime::Instrument> listOf(const EngineSeedConfig& seedConfig) {
                std::vector<runtime::Instrument> list;
                for (std::size_t i = 0; i < seedConfig.instruments; ++i) {
                    list.push_back(runtime::Instrument{symbolOf(i), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
                }
                return list;
            }

            static EngineFailure failure(std::size_t index, const Instrument& instrument, std::string reason) {
                return EngineFailure{index, instrument.symbol.text(), std::move(reason)};
            }

            void plantDivergence() {
                const Instrument& instrument = instruments.front();
                replay::AddCommand planted{};
                planted.orderId = 4000000000U;
                planted.side = Side::BUY;
                planted.price = 1;
                planted.quantity = 1;
                runtime::Shard& shard = *shards[instrument.location.shard];
                shard.process(planted, instrument.location.slot);
                if (config.cancelPlantedOrder) {
                    shard.process(replay::CancelCommand{planted.orderId}, instrument.location.slot);
                }
            }

            std::optional<EngineFailure> execute(const EngineCommand& line, std::size_t index, EngineStats& stats) {
                Instrument& instrument = instruments[line.instrument];
                if (!line.converted) {
                    ++stats.dropped;
                    return std::nullopt;
                }
                const Command& command = *line.converted;
                const network::CommandFrame frame{index, rawCommandOf(command, faultySymbol(line.fault, instrument.symbol))};
                const network::EntryDecision decision = network::checkEntry(frame, directory, false);

                if (line.fault != SymbolFault::NONE) {
                    const auto* refused = std::get_if<network::EntryRefused>(&decision);
                    if (refused == nullptr || refused->code != network::RefusedCode::UNKNOWN_INSTRUMENT) {
                        return failure(index, instrument, "a bad symbol was not refused as an unknown instrument");
                    }
                    ++stats.refused;
                    return std::nullopt;
                }

                const ExpectedOutcome expected = randomized::applyCommand(instrument.model, command);
                if (const auto* rejected = std::get_if<network::EntryMalformed>(&decision)) {
                    ++stats.malformedAtEntry;
                    const std::span<const BookEvent> event(&rejected->event, 1);
                    if (!sameEvents(event, expected.events)) {
                        return failure(index, instrument,
                                       "entry answered " + describe(event) + " where the model says " + describe(expected.events));
                    }
                    return std::nullopt;
                }
                const auto* accepted = std::get_if<network::EntryAccepted>(&decision);
                if (accepted == nullptr) {
                    return failure(index, instrument, "entry refused a command with a good symbol, with the engine running");
                }
                ++stats.queued;
                return runOnShard(*accepted, instrument, expected, index, stats);
            }

            // A LOG line: each shard's log writer does one write.
            void writeLogs(EngineStats& stats) {
                for (const std::unique_ptr<ShardLog>& shardLog : logs) {
                    const std::size_t written = shardLog->writer.pass();
                    stats.logWrites += written != 0 ? 1 : 0;
                    stats.recordsLogged += written;
                }
            }

            // What the matching thread does before running a command: its place in the log is
            // fixed, waiting for the log writer first if the ring is full.
            void logCommand(const network::EntryAccepted& accepted, std::size_t index, EngineStats& stats) {
                if (config.leaveUnloggedFrom && *config.leaveUnloggedFrom <= index && !leftUnlogged) {
                    leftUnlogged = true;
                    return;
                }
                ShardLog& shardLog = *logs[accepted.location.shard];
                while (!shardLog.ring.tryPush(runtime::LogEntry{accepted.command, accepted.location.slot, runtime::ControlKind::COMMAND})) {
                    const std::size_t written = shardLog.writer.pass();
                    ++stats.logWrites;
                    stats.recordsLogged += written;
                }
            }

            std::optional<EngineFailure> runOnShard(const network::EntryAccepted& accepted, Instrument& instrument,
                                                    const ExpectedOutcome& expected, std::size_t index, EngineStats& stats) {
                if (accepted.symbol != instrument.symbol || accepted.location != instrument.location) {
                    return failure(index, instrument, "entry routed a command to the wrong book");
                }
                logCommand(accepted, index, stats);
                runtime::Shard& shard = *shards[accepted.location.shard];
                shard.process(accepted.command, accepted.location.slot);
                const std::span<const BookEvent> events = shard.result().events();

                const std::vector<BookEvent> witnessed = eventsOf(replay::executeCommand(instrument.witness, accepted.command));
                if (!sameEvents(events, witnessed)) {
                    return failure(index, instrument, "the shard said " + describe(events) + " where the witness says " + describe(witnessed));
                }
                if (!sameEvents(events, expected.events)) {
                    return failure(index, instrument, "the shard said " + describe(events) + " where the model says " + describe(expected.events));
                }
                const OrderBook& book = shard.books().book(accepted.location.slot);
                if (const std::optional<std::string> difference = randomized::compareBookWithModel(book, instrument.model)) {
                    return failure(index, instrument, "the shard's book differs from the model: " + *difference);
                }
                return std::nullopt;
            }

            // What every run ends on: every shard sound, and every witness equal to the model
            // its shard's book was compared with.
            std::optional<EngineFailure> finish(std::size_t index) {
                for (const std::unique_ptr<runtime::Shard>& shard : shards) {
                    if (const std::optional<runtime::ShardViolation> violation = shard->checkInvariants()) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard->books().shard()) + ": " + violation->message};
                    }
                }
                for (const Instrument& instrument : instruments) {
                    if (const std::optional<std::string> difference = randomized::compareBookWithModel(instrument.witness, instrument.model)) {
                        return failure(index, instrument, "the witness differs from the model: " + *difference);
                    }
                    const OrderBook& book = shards[instrument.location.shard]->books().book(instrument.location.slot);
                    if (const std::optional<std::string> difference = randomized::compareBookWithModel(book, instrument.model)) {
                        return failure(index, instrument, "the shard's book differs from the model: " + *difference);
                    }
                    if (const std::optional<std::string> difference = randomized::compareBooks(book, instrument.witness, instrument.model)) {
                        return failure(index, instrument, "the shard's book differs from the witness: " + *difference);
                    }
                    if (const std::optional<std::string> broken = randomized::checkBookPool(book)) {
                        return failure(index, instrument, "the shard's book's pool: " + *broken);
                    }
                    if (const std::optional<std::string> broken = randomized::checkBookPool(instrument.witness)) {
                        return failure(index, instrument, "the witness's pool: " + *broken);
                    }
                }
                return std::nullopt;
            }

            // Every run ends here: each log writer stopped, as a clean stop stops it, then each
            // shard's log replayed per instrument, which must give the witness's book, and each
            // shard, its durable position at the end of its log, sound.
            std::optional<EngineFailure> finishLogs(std::size_t index, EngineStats& stats) {
                for (std::size_t shard = 0; shard < logs.size(); ++shard) {
                    ShardLog& shardLog = *logs[shard];
                    while (!shardLog.ring.tryPush(runtime::LogEntry::stop())) {
                        stats.recordsLogged += shardLog.writer.pass();
                    }
                    while (!shardLog.writer.stopped()) {
                        stats.recordsLogged += shardLog.writer.pass();
                    }
                    if (const std::optional<runtime::ShardViolation> violation = shards[shard]->checkInvariants()) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard) + " after its log closed: " + violation->message};
                    }
                }
                std::vector<replay::ReplayRecord> records;
                for (std::size_t shard = 0; shard < logs.size(); ++shard) {
                    const std::vector<std::byte> bytes = storage.read(logs[shard]->writer.fileName(), 0);
                    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::ios::binary);
                    replay::CommandLogReader reader(input);
                    for (;;) {
                        const replay::ReadResult read = reader.next();
                        if (const auto* record = std::get_if<replay::ReplayRecord>(&read)) {
                            records.push_back(*record);
                        } else if (const auto* error = std::get_if<replay::ParseError>(&read)) {
                            return EngineFailure{index, "", "shard " + std::to_string(shard) + "'s log doesn't read back: " + error->message};
                        } else {
                            break;
                        }
                    }
                }
                const std::vector<replay::InstrumentRecords> grouped = replay::groupBySymbol(records);
                for (const Instrument& instrument : instruments) {
                    const auto found = std::find_if(grouped.begin(), grouped.end(),
                                                    [&instrument](const replay::InstrumentRecords& group) { return group.symbol == instrument.symbol; });
                    OrderBook replayed;
                    if (found != grouped.end()) {
                        const replay::ReplayOutcome outcome = replay::ReplayEngine::run(found->records, replayed);
                        if (const auto* error = std::get_if<replay::ReplayError>(&outcome)) {
                            return failure(index, instrument, "the log's replay failed: " + error->message);
                        }
                    }
                    if (const std::optional<std::string> difference = randomized::compareBooks(replayed, instrument.witness, instrument.model)) {
                        return failure(index, instrument, "the log's replay differs from the witness: " + *difference);
                    }
                }
                return std::nullopt;
            }

            EngineSeedConfig config;
            runtime::InstrumentDirectory directory;
            runtime::InMemoryLogStorage storage;
            std::vector<std::unique_ptr<runtime::Shard>> shards;
            std::vector<std::unique_ptr<ShardLog>> logs;
            std::vector<Instrument> instruments;
            bool leftUnlogged = false;
        };
    }  // namespace

    std::optional<Command> convertForEngine(const Command& command) {
        if (command.intent == CommandIntent::MARKED_FILLED || command.intent == CommandIntent::TYPE_CANNOT_REST) {
            return std::nullopt;
        }
        Command converted = command;
        converted.overload = SubmitOverload::REQUEST;
        // The wire clears a mode with no owner, so the model gets the request the book will.
        if (converted.owner == 0 && isValidSelfTradePrevention(converted.selfTradePrevention)) {
            converted.selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;
        }
        return converted;
    }

    replay::RawCommand rawCommandOf(const Command& command, const std::array<char, Symbol::size>& symbol) {
        replay::RawCommand raw{};
        raw.symbol = symbol;
        raw.orderId = command.orderId;
        if (command.type == CommandType::CANCEL) {
            raw.command = replay::rawCommandCancel;
            return raw;
        }
        raw.price = command.price;
        raw.quantity = command.quantity;
        raw.owner = command.owner;
        raw.flags = command.postOnly ? replay::rawFlagPostOnly : 0;
        raw.selfTradePrevention = byteOf(command.selfTradePrevention);
        if (command.type == CommandType::REPLACE) {
            raw.command = replay::rawCommandReplace;
            return raw;
        }
        raw.command = replay::rawCommandAdd;
        raw.side = byteOf(command.side);
        raw.orderType = byteOf(command.orderType);
        raw.timeInForce = byteOf(command.timeInForce);
        raw.stopPrice = command.stopPrice;
        return raw;
    }

    std::vector<EngineCommand> generateEngineStream(const EngineSeedConfig& config) {
        const std::vector<std::string> profiles = profilesOf(config);
        std::vector<RandomCommandGenerator> generators;
        std::vector<ReferenceBook> models(config.instruments);
        generators.reserve(config.instruments);
        for (std::size_t i = 0; i < config.instruments; ++i) {
            generators.emplace_back(instrumentSeed(config.seed, i), randomized::findProfile(profiles[i % profiles.size()]));
        }

        reference::Interleaver interleaver(config.seed ^ interleaverStream, config.instruments, config.spread);
        std::mt19937_64 faults(config.seed ^ faultStream);
        std::mt19937_64 logWrites(config.seed ^ logStream);
        std::vector<EngineCommand> stream;
        stream.reserve(config.commands);
        for (std::size_t i = 0; i < config.commands; ++i) {
            const std::size_t instrument = interleaver.next();
            const Command generated = generators[instrument].next(models[instrument]);
            randomized::applyCommand(models[instrument], generated);

            SymbolFault fault = SymbolFault::NONE;
            const std::uint64_t roll = faults();
            if (roll % 100 < config.symbolFaultPercent) {
                fault = (roll >> 32) % 2 == 0 ? SymbolFault::UNKNOWN : SymbolFault::MALFORMED;
            }
            stream.push_back(EngineCommand{instrument, generated, convertForEngine(generated), fault, EngineAction::COMMAND});
            if (config.logWritePercent != 0 && logWrites() % 100 < config.logWritePercent) {
                stream.push_back(EngineCommand{0, generated, std::nullopt, SymbolFault::NONE, EngineAction::LOG});
            }
        }
        return stream;
    }

    EngineRunResult runEngineSeed(const EngineSeedConfig& config) {
        if (config.instruments == 0 || config.shards == 0) {
            throw std::invalid_argument("an engine run needs instruments and shards");
        }
        Run run(config);
        return run.run(generateEngineStream(config));
    }

}  // namespace order_book::engine_testing
