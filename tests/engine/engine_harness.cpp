#include "engine_harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
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
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/closed_log.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/roll_forward.hpp"
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
        constexpr std::uint64_t crashStream = 0x27D4EB2F165667C5ULL;
        constexpr std::uint64_t restoreStream = 0x85EBCA77C2B2AE63ULL;
        constexpr std::uint64_t rollStream = 0x94D049BB133111EBULL;
        constexpr std::uint64_t damageStream = 0xBF58476D1CE4E5B9ULL;
        constexpr std::uint64_t cleanStopStream = 0xD6E8FEB86659FD93ULL;
        // Records in the log file of the shard with the most symbols, so a run rolls across many,
        // and a file spans two of a drive's pages, so a machine crash can tear a record or leave a
        // hole that valid records follow.
        constexpr std::uint64_t recordsPerSmallestFile = 120;

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
            ShardLog(runtime::Housekeeping& housekeeping, std::uint32_t shard, std::size_t records, runtime::ClosedLog& closed,
                     std::uint64_t firstPosition = 0)
                : ring(records), durable(firstPosition), writer(housekeeping, shard, ring, durable, firstPosition, &closed) {}

            runtime::Ring<runtime::LogEntry> ring;
            runtime::DurablePosition durable;
            runtime::LogWriter writer;
        };

        // A command as its shard's log holds it, in log order, so the model and witness can be
        // rebuilt from what a crash left. Its line in the stream orders it against other shards'
        // logs, which a placement change can spread one instrument across.
        struct Logged {
            std::size_t instrument;
            Command command;
            replay::Command bookCommand;
            std::size_t line;
        };

        const std::atomic<bool> neverStopping{false};

        // Everything that runs one instrument, beside its shard's book.
        struct Instrument {
            Symbol symbol;
            runtime::BookLocation location;
            ReferenceBook model;
            OrderBook witness;
        };

        class Run {
        public:
            explicit Run(const EngineSeedConfig& seedConfig)
                : config(seedConfig),
                  directory(std::make_unique<runtime::InstrumentDirectory>(listOf(seedConfig), seedConfig.shards)),
                  lock(storage.lock()),
                  housekeeping(makeHousekeeping()),
                  logged(seedConfig.shards),
                  released(seedConfig.shards, 0),
                  closed(std::make_unique<runtime::ClosedLog>(seedConfig.shards)) {
                // As the engine starts on an empty directory: every instrument is new, and gets an
                // empty-book checkpoint before its shard's first file.
                for (std::size_t i = 0; i < config.instruments; ++i) {
                    const Symbol symbol = symbolOf(i);
                    runtime::publishEmptyCheckpoint(storage, symbol, shardCount(), directory->find(symbol)->shard, 0);
                }
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    shards.push_back(std::make_unique<runtime::Shard>(*directory, shard, config.shardCapacity));
                    logs.push_back(std::make_unique<ShardLog>(*housekeeping, shard, config.logRingRecords, *closed));
                    shards.back()->watchDurablePosition(logs.back()->durable);
                }
                makeRollers();
                instruments.reserve(config.instruments);
                for (std::size_t i = 0; i < config.instruments; ++i) {
                    const Symbol symbol = symbolOf(i);
                    instruments.push_back(Instrument{symbol, *directory->find(symbol), ReferenceBook{}, OrderBook{}});
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
                    if (stream[index].action == EngineAction::CRASH_ENGINE || stream[index].action == EngineAction::CRASH_MACHINE) {
                        result.failure = crash(stream[index], index, result.stats);
                        continue;
                    }
                    if (stream[index].action == EngineAction::RESTORE) {
                        result.failure = restore(stream[index], index, result.stats);
                        continue;
                    }
                    if (stream[index].action == EngineAction::ROLL) {
                        result.failure = roll(index, result.stats);
                        continue;
                    }
                    if (stream[index].action == EngineAction::DAMAGE) {
                        damage(stream[index], result.stats);
                        continue;
                    }
                    if (stream[index].action == EngineAction::CLEAN_STOP) {
                        result.failure = cleanStop(stream[index], index, result.stats);
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
                    if (instrument.location.shard >= shards.size()) {
                        continue;  // a recovery that stopped built only the shards before it
                    }
                    result.stats.shardGrowths +=
                        shards[instrument.location.shard]->books().book(instrument.location.slot).capacityGrowths();
                    result.stats.witnessGrowths += instrument.witness.capacityGrowths();
                }
                return result;
            }

        private:
            std::uint32_t shardCount() const { return directory->shardCount(); }

            // Every instrument hashed to its shard, but one a CLEAN_STOP line pins.
            static std::vector<runtime::Instrument> listOf(const EngineSeedConfig& seedConfig, std::size_t pinned = 0,
                                                           std::optional<std::uint32_t> pin = std::nullopt) {
                std::vector<runtime::Instrument> list;
                for (std::size_t i = 0; i < seedConfig.instruments; ++i) {
                    list.push_back(runtime::Instrument{symbolOf(i), i == pinned ? pin : std::nullopt, std::nullopt, std::nullopt, std::nullopt});
                }
                return list;
            }

            std::unique_ptr<runtime::Housekeeping> makeHousekeeping() {
                return std::make_unique<runtime::Housekeeping>(
                    storage, *directory, config.durability,
                    runtime::leastLogFileBytes(*directory) + (recordsPerSmallestFile - 1) * replay::logRecordBytes);
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
                const network::EntryDecision decision = network::checkEntry(frame, *directory, false);

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
                return runOnShard(*accepted, line, instrument, expected, index, stats);
            }

            // One pass of a log writer, playing the housekeeping thread first if it waits for its
            // next file.
            std::size_t writeLog(ShardLog& shardLog) {
                const std::size_t written = shardLog.writer.pass();
                if (written != 0 || !shardLog.writer.waitingForFile()) {
                    return written;
                }
                housekeeping->pass();
                return shardLog.writer.pass();
            }

            // A LOG line: each shard's log writer does one write.
            void writeLogs(EngineStats& stats) {
                for (const std::unique_ptr<ShardLog>& shardLog : logs) {
                    const std::size_t written = writeLog(*shardLog);
                    stats.logWrites += written != 0 ? 1 : 0;
                    stats.recordsLogged += written;
                }
            }

            // What the matching thread does before running a command: its place in the log is
            // fixed, waiting for the log writer first if the ring is full.
            void logCommand(const network::EntryAccepted& accepted, const EngineCommand& line, std::size_t index, EngineStats& stats) {
                if (config.leaveUnloggedFrom && *config.leaveUnloggedFrom <= index && !leftUnlogged) {
                    leftUnlogged = true;
                    return;
                }
                logged[accepted.location.shard].push_back(Logged{line.instrument, *line.converted, accepted.command, index});
                ShardLog& shardLog = *logs[accepted.location.shard];
                while (!shardLog.ring.tryPush(runtime::LogEntry{accepted.command, accepted.location.slot, runtime::ControlKind::COMMAND})) {
                    const std::size_t written = writeLog(shardLog);
                    ++stats.logWrites;
                    stats.recordsLogged += written;
                }
            }

            std::optional<EngineFailure> runOnShard(const network::EntryAccepted& accepted, const EngineCommand& line,
                                                    Instrument& instrument, const ExpectedOutcome& expected, std::size_t index,
                                                    EngineStats& stats) {
                if (accepted.symbol != instrument.symbol || accepted.location != instrument.location) {
                    return failure(index, instrument, "entry routed a command to the wrong book");
                }
                logCommand(accepted, line, index, stats);
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

            // A RESTORE line, as runEngineSeed's comment in engine_harness.hpp says.
            std::optional<EngineFailure> restore(const EngineCommand& line, std::size_t index, EngineStats& stats) {
                Instrument& instrument = instruments[line.instrument];
                runtime::Shard& shard = *shards[instrument.location.shard];
                const std::vector<std::byte> image = imageOf(shard.books().book(instrument.location.slot));
                const std::vector<std::byte> witnessed = imageOf(instrument.witness);
                if (image != witnessed) {
                    const auto differs = std::mismatch(image.begin(), image.end(), witnessed.begin(), witnessed.end());
                    return failure(index, instrument, "the shard's book's image differs from the witness's, first at byte " +
                                                          std::to_string(differs.first - image.begin()));
                }
                auto restored = OrderBook::restore(image);
                if (const auto* refused = std::get_if<RestoreError>(&restored)) {
                    return failure(index, instrument, std::string("restore refused the shard's book's image: ") +
                                                          restoreErrorCodeName(refused->code) + " at " +
                                                          std::to_string(refused->offset) + ": " + refused->message);
                }
                if (imageOf(std::get<OrderBook>(restored)) != image) {
                    return failure(index, instrument, "the restored book's image differs from the one it was restored from");
                }
                shard.replaceBook(instrument.location.slot, std::move(std::get<OrderBook>(restored)));
                ++stats.restores;
                return std::nullopt;
            }

            // A worker's rollers, as made at a start: leftover .tmp files go, and each book's
            // checkpoints are read.
            void makeRollers() {
                rollers.clear();
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    rollers.push_back(std::make_unique<runtime::ShardRoller>(storage, *directory, shard, [](const std::string&) {}));
                }
            }

            // A ROLL line, as runEngineSeed's comment in engine_harness.hpp says.
            std::optional<EngineFailure> roll(std::size_t index, EngineStats& stats) {
                ++stats.rolls;
                std::vector<std::optional<std::uint64_t>> before;
                for (const Instrument& instrument : instruments) {
                    before.push_back(rollers[instrument.location.shard]->position(instrument.location.slot));
                }
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    runtime::ShardRoller& roller = *rollers[shard];
                    const std::uint64_t published = roller.counts().published;
                    stats.rollPasses += roller.roll(closed->end(shard));
                    stats.checkpoints += static_cast<std::size_t>(roller.counts().published - published);
                    if (const std::optional<std::uint64_t> floor = roller.retentionFloor(); floor && !config.keepLog) {
                        housekeeping->retain(shard, *floor);
                    }
                    if (roller.counts().writesFailed != 0 || roller.counts().passesLost != 0) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard) + "'s roll forward failed a write or lost a pass"};
                    }
                }
                const std::uint64_t deleted = housekeeping->filesDeleted();
                housekeeping->pass();
                stats.logFilesDeleted += housekeeping->filesDeleted() - deleted;
                for (std::size_t i = 0; i < instruments.size(); ++i) {
                    const Instrument& instrument = instruments[i];
                    const std::optional<std::uint64_t> at = rollers[instrument.location.shard]->position(instrument.location.slot);
                    if (!at) {
                        return failure(index, instrument, "the book has no checkpoint, though its empty one was published at the start");
                    }
                    if (at == before[i]) {
                        continue;  // not published by this roll: a DAMAGE line may have flipped a byte of it
                    }
                    const std::vector<std::byte> file = storage.read(runtime::checkpointFileName(instrument.symbol, *at), 0);
                    if (const std::optional<runtime::CheckpointFault> fault = runtime::verifyCheckpoint(file)) {
                        return failure(index, instrument, "its newest checkpoint does not verify: " +
                                                              runtime::formatCheckpointFault(runtime::checkpointFileName(instrument.symbol, *at), *fault));
                    }
                }
                return std::nullopt;
            }

            // Each book's newest checkpoint, restored, then the logged commands from its position
            // replayed on it, must give the witness's image: what recovery from checkpoints will do.
            std::optional<EngineFailure> checkCheckpoints(std::size_t index) {
                for (const Instrument& instrument : instruments) {
                    const std::uint32_t shard = instrument.location.shard;
                    std::vector<std::uint64_t> positions;
                    for (const std::string& name : storage.names()) {
                        const std::optional<runtime::CheckpointFileNameParts> parts = runtime::parseCheckpointFileName(name);
                        if (parts && !parts->temporary && parts->symbol == instrument.symbol) {
                            positions.push_back(parts->position);
                        }
                    }
                    std::sort(positions.rbegin(), positions.rend());
                    std::optional<std::uint64_t> at;
                    std::vector<std::byte> file;
                    std::optional<runtime::DecodedCheckpoint> found;
                    for (const std::uint64_t position : positions) {
                        file = storage.read(runtime::checkpointFileName(instrument.symbol, position), 0);
                        const auto decoded = runtime::decodeCheckpoint(file);
                        if (const auto* good = std::get_if<runtime::DecodedCheckpoint>(&decoded)) {
                            at = position;
                            found = *good;
                            break;
                        }
                    }
                    if (!at) {
                        return failure(index, instrument, "the book has no checkpoint that decodes");
                    }
                    if (*at > logged[shard].size()) {
                        return failure(index, instrument, "a checkpoint at " + std::to_string(*at) + " stands past its shard's log end, " +
                                                              std::to_string(logged[shard].size()));
                    }
                    const runtime::DecodedCheckpoint* checkpoint = &*found;
                    auto restored = OrderBook::restore(checkpoint->image);
                    if (std::holds_alternative<RestoreError>(restored)) {
                        return failure(index, instrument, "its newest checkpoint does not restore");
                    }
                    OrderBook& book = std::get<OrderBook>(restored);
                    for (std::size_t position = static_cast<std::size_t>(*at); position < logged[shard].size(); ++position) {
                        const Logged& entry = logged[shard][position];
                        if (entry.instrument == static_cast<std::size_t>(&instrument - instruments.data())) {
                            static_cast<void>(replay::executeCommand(book, entry.bookCommand));
                        }
                    }
                    if (imageOf(book) != imageOf(instrument.witness)) {
                        return failure(index, instrument, "its checkpoint at " + std::to_string(*at) +
                                                              ", with the log after it, does not give the witness's image");
                    }
                }
                return std::nullopt;
            }

            // A DAMAGE line: a byte of the book's newest checkpoint flipped, if an older one stands to
            // fall back to, so the next start, or the roller's next restore, does.
            void damage(const EngineCommand& line, EngineStats& stats) {
                const Instrument& instrument = instruments[line.instrument];
                std::vector<std::uint64_t> positions;
                for (const std::string& name : storage.names()) {
                    const std::optional<runtime::CheckpointFileNameParts> parts = runtime::parseCheckpointFileName(name);
                    if (parts && !parts->temporary && parts->symbol == instrument.symbol) {
                        positions.push_back(parts->position);
                    }
                }
                if (positions.size() < 2) {
                    return;  // nothing to fall back to: a start would stop
                }
                const std::string name = runtime::checkpointFileName(instrument.symbol, *std::max_element(positions.begin(), positions.end()));
                std::vector<std::byte> bytes = storage.read(name, 0);
                bytes[bytes.size() / 2] ^= std::byte{0x40};
                // Renamed into place, so the damage survives a machine crash as a drive's would.
                const std::unique_ptr<runtime::LogFile> file = storage.create(name + ".damaged", runtime::Durability::PROCESS);
                file->write(0, bytes);
                file->flush();
                storage.remove(name);
                file->rename(name);
                file->close();
                ++stats.damages;
            }

            static std::vector<std::byte> imageOf(const OrderBook& book) {
                std::vector<std::byte> image(book.imageSize());
                image.resize(book.writeImage(image));
                return image;
            }

            // A CRASH line, as runEngineSeed's comment in engine_harness.hpp says.
            std::optional<EngineFailure> crash(const EngineCommand& line, std::size_t index, EngineStats& stats) {
                const bool machine = line.action == EngineAction::CRASH_MACHINE;
                ++stats.crashes;
                for (std::size_t shard = 0; shard < logs.size(); ++shard) {
                    released[shard] = std::max(released[shard], logs[shard]->durable.load());
                }
                const bool losing = config.loseLogFilesAtFirstCrash && !crashedOnce;
                crashedOnce = true;
                if (machine && !losing) {
                    // The write each log writer was making, which the crash meets on its way: one
                    // pass, in the file it has, so never one past a full file's flush.
                    storage.interruptWrites();
                    for (const std::unique_ptr<ShardLog>& shardLog : logs) {
                        shardLog->writer.pass();
                    }
                }
                storage.crash(machine ? runtime::InMemoryLogStorage::Crash::MACHINE : runtime::InMemoryLogStorage::Crash::ENGINE,
                              line.crashSeed);
                if (losing) {
                    loseShardZerosNewerFiles();
                }
                // Everything the dead engine had, its log writers before the housekeeping they take
                // files from.
                logs.clear();
                rollers.clear();
                housekeeping.reset();
                shards.clear();
                lock.reset();

                lock = storage.lock();
                housekeeping = makeHousekeeping();
                std::vector<runtime::RecoveredShard> recovered;
                try {
                    const runtime::LogSurvey survey = runtime::surveyLog(storage, *directory);
                    for (const std::string& temporary : survey.temporaries) {
                        storage.remove(temporary);
                    }
                    for (const std::string& temporary : survey.checkpointTemporaries) {
                        storage.remove(temporary);
                    }
                    for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                        shards.push_back(std::make_unique<runtime::Shard>(*directory, shard, config.shardCapacity));
                        recovered.push_back(runtime::recoverShard(storage, storage, *directory, survey.shards[shard], survey.checkpoints[shard],
                                                                  *shards.back(), config.shardCapacity, neverStopping));
                    }
                } catch (const runtime::RecoveryError& error) {
                    // Every file is far below 1 MiB, so every hole a crash leaves is cut: no
                    // modelled crash can leave what recovery must stop on.
                    return EngineFailure{index, "", std::string("recovery after a crash failed: ") + error.what()};
                }

                const bool promised = !machine || config.durability == runtime::Durability::MACHINE;
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    const std::uint64_t survived = recovered[shard].replayed;
                    stats.fellBack += recovered[shard].fellBack;
                    stats.tailCutBytes += recovered[shard].tailCutBytes;
                    if (survived > logged[shard].size()) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard) + " recovered " + std::to_string(survived) +
                                                            " commands of the " + std::to_string(logged[shard].size()) + " it queued"};
                    }
                    if (promised && survived < released[shard]) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard) + " recovered " + std::to_string(survived) +
                                                            " commands, but had released responses up to " +
                                                            std::to_string(released[shard])};
                    }
                    stats.lostToCrashes += logged[shard].size() - survived;
                    logged[shard].resize(static_cast<std::size_t>(survived));
                    // A crash the promise doesn't cover may lose released responses: from here on
                    // only what survived was ever released.
                    released[shard] = std::min(released[shard], survived);
                }
                rebuildFromTheLog();
                for (const Instrument& instrument : instruments) {
                    const OrderBook& book = shards[instrument.location.shard]->books().book(instrument.location.slot);
                    if (const std::optional<std::string> difference = randomized::compareBookWithModel(book, instrument.model)) {
                        return failure(index, instrument, "after the crash, the shard's book differs from the model: " + *difference);
                    }
                    if (const std::optional<std::string> difference = randomized::compareBooks(book, instrument.witness, instrument.model)) {
                        return failure(index, instrument, "after the crash, the shard's book differs from the witness: " + *difference);
                    }
                }
                closed = std::make_unique<runtime::ClosedLog>(shardCount());
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    logs.push_back(std::make_unique<ShardLog>(*housekeeping, shard, config.logRingRecords, *closed, recovered[shard].replayed));
                    shards[shard]->watchDurablePosition(logs.back()->durable);
                }
                makeRollers();
                return checkCheckpoints(index);
            }

            // A CLEAN_STOP line, as runEngineSeed's comment in engine_harness.hpp says.
            std::optional<EngineFailure> cleanStop(const EngineCommand& line, std::size_t index, EngineStats& stats) {
                ++stats.cleanStops;
                // Steps 1 and 2: each log writer writes the rest, cuts, flushes, and closes its file,
                // then the next files no writer took go.
                for (const std::unique_ptr<ShardLog>& shardLog : logs) {
                    while (!shardLog->ring.tryPush(runtime::LogEntry::stop())) {
                        stats.recordsLogged += writeLog(*shardLog);
                    }
                    while (!shardLog->writer.stopped()) {
                        stats.recordsLogged += writeLog(*shardLog);
                    }
                }
                housekeeping->removeUnused();
                // Steps 3 to 5: each shard's live books, then its last roll, which must publish every
                // one at its log's end.
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    const runtime::LiveImages live = runtime::captureLiveImages(*shards[shard]);
                    const std::uint64_t end = logs[shard]->writer.nextPosition();
                    rollers[shard]->finish(end, live);
                    for (std::uint32_t slot = 0; slot < shards[shard]->size(); ++slot) {
                        const Symbol& symbol = shards[shard]->books().symbol(slot);
                        if (const std::optional<std::string> unpublished = runtime::formatStoppedBook(symbol, end, rollers[shard]->stopped(slot))) {
                            return EngineFailure{index, symbol.text(), "the clean stop: " + *unpublished};
                        }
                    }
                }
                // Everything the stopped engine had, its log writers before the housekeeping.
                logs.clear();
                rollers.clear();
                housekeeping.reset();
                shards.clear();
                lock.reset();
                // The line's placement, which changed if the count or any instrument's shard did.
                const std::uint32_t countBefore = shardCount();
                directory = std::make_unique<runtime::InstrumentDirectory>(listOf(config, line.instrument, line.pin), line.shards);
                const bool moved = std::any_of(instruments.begin(), instruments.end(), [this](const Instrument& instrument) {
                    return directory->find(instrument.symbol)->shard != instrument.location.shard;
                });
                if (moved || line.shards != countBefore) {
                    ++stats.placementChanges;
                    stats.symbolMoves += line.shards == countBefore ? 1 : 0;
                    placementChanged = true;
                }

                // The start, as the engine's: a change of placement first, then recovery.
                lock = storage.lock();
                housekeeping = makeHousekeeping();
                std::vector<runtime::RecoveredShard> recovered;
                try {
                    runtime::LogSurvey survey = runtime::surveyLog(storage, *directory);
                    for (const std::string& temporary : survey.temporaries) {
                        storage.remove(temporary);
                    }
                    for (const std::string& temporary : survey.checkpointTemporaries) {
                        storage.remove(temporary);
                    }
                    if (survey.placement && (!survey.placement->rewraps.empty() || !survey.placement->dropped.empty())) {
                        runtime::changePlacement(storage, *survey.placement);
                        stats.rewraps += survey.placement->rewraps.size();
                        survey = runtime::surveyLog(storage, *directory);
                    }
                    for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                        shards.push_back(std::make_unique<runtime::Shard>(*directory, shard, config.shardCapacity));
                        recovered.push_back(runtime::recoverShard(storage, storage, *directory, survey.shards[shard], survey.checkpoints[shard],
                                                                  *shards.back(), config.shardCapacity, neverStopping));
                    }
                } catch (const runtime::RecoveryError& error) {
                    return EngineFailure{index, "", std::string("the start after a clean stop failed: ") + error.what()};
                }
                if (logged.size() < shardCount()) {
                    logged.resize(shardCount());  // a shard new to this placement, its log starting at 0
                    released.resize(shardCount(), 0);
                }
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    if (recovered[shard].replayed != logged[shard].size()) {
                        return EngineFailure{index, "", "after a clean stop, shard " + std::to_string(shard) + " recovered " +
                                                            std::to_string(recovered[shard].replayed) + " commands of the " +
                                                            std::to_string(logged[shard].size()) + " it logged"};
                    }
                    if (recovered[shard].start != recovered[shard].replayed) {
                        return EngineFailure{index, "", "after a clean stop, shard " + std::to_string(shard) + " replayed from " +
                                                            std::to_string(recovered[shard].start) + ", not its log's end, " +
                                                            std::to_string(recovered[shard].replayed)};
                    }
                }
                for (Instrument& instrument : instruments) {
                    instrument.location = *directory->find(instrument.symbol);
                    const OrderBook& book = shards[instrument.location.shard]->books().book(instrument.location.slot);
                    if (const std::optional<std::string> difference = randomized::compareBooks(book, instrument.witness, instrument.model)) {
                        return failure(index, instrument, "after a clean stop, the shard's book differs from the witness: " + *difference);
                    }
                }
                closed = std::make_unique<runtime::ClosedLog>(shardCount());
                for (std::uint32_t shard = 0; shard < shardCount(); ++shard) {
                    logs.push_back(std::make_unique<ShardLog>(*housekeeping, shard, config.logRingRecords, *closed, recovered[shard].replayed));
                    shards[shard]->watchDurablePosition(logs.back()->durable);
                }
                makeRollers();
                return checkCheckpoints(index);
            }

            // A drive that lied: every file of shard 0's but its oldest gone.
            void loseShardZerosNewerFiles() {
                const std::string prefix = runtime::logFileName(0, 0).substr(0, 9);
                bool oldest = true;
                for (const std::string& name : storage.names()) {
                    if (!name.starts_with(prefix)) {
                        continue;
                    }
                    if (!oldest) {
                        storage.remove(name);
                    }
                    oldest = false;
                }
            }

            // Each model and witness from nothing, through the commands that survived in its
            // shard's log, in log order.
            void rebuildFromTheLog() {
                for (Instrument& instrument : instruments) {
                    instrument.model = ReferenceBook{};
                    instrument.witness = OrderBook{};
                    instrument.witness.reserve(config.witnessCapacity);
                }
                // In the stream's order, since after a placement change an instrument's commands
                // can lie in two shards' logs.
                std::vector<const Logged*> ordered;
                for (const std::vector<Logged>& shardLog : logged) {
                    for (const Logged& entry : shardLog) {
                        ordered.push_back(&entry);
                    }
                }
                std::sort(ordered.begin(), ordered.end(), [](const Logged* a, const Logged* b) { return a->line < b->line; });
                for (const Logged* entry : ordered) {
                    Instrument& instrument = instruments[entry->instrument];
                    randomized::applyCommand(instrument.model, entry->command);
                    static_cast<void>(replay::executeCommand(instrument.witness, entry->bookCommand));
                }
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
                        stats.recordsLogged += writeLog(shardLog);
                    }
                    while (!shardLog.writer.stopped()) {
                        stats.recordsLogged += writeLog(shardLog);
                    }
                    if (const std::optional<runtime::ShardViolation> violation = shards[shard]->checkInvariants()) {
                        return EngineFailure{index, "", "shard " + std::to_string(shard) + " after its log closed: " + violation->message};
                    }
                }
                // The next files no writer took, as a clean stop removes them.
                housekeeping->removeUnused();
                std::vector<replay::ReplayRecord> records;
                // Unless retention deleted the start of some shard's, or a placement change spread an
                // instrument over two shards' logs, whose positions can't be compared.
                bool wholeLog = !placementChanged;
                for (std::uint32_t shard = 0; shard < logs.size(); ++shard) {
                    try {
                        const ShardLogRecords read = readShardLog(storage, shard);
                        stats.logFiles += read.files;
                        records.insert(records.end(), read.records.begin(), read.records.end());
                        wholeLog = wholeLog && read.firstPosition == 0;
                        if (read.firstPosition + read.records.size() != logs[shard]->writer.nextPosition()) {
                            return EngineFailure{index, "", "shard " + std::to_string(shard) + "'s files end at " +
                                                                std::to_string(read.firstPosition + read.records.size()) +
                                                                ", not its log's end, " + std::to_string(logs[shard]->writer.nextPosition())};
                        }
                    } catch (const std::runtime_error& error) {
                        return EngineFailure{index, "", error.what()};
                    }
                }
                if (std::optional<EngineFailure> failed = checkCheckpoints(index)) {
                    return failed;
                }
                if (!wholeLog) {
                    return std::nullopt;  // each book's checkpoint and the log after it gave the witness, above
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
            std::unique_ptr<runtime::InstrumentDirectory> directory;  // a CLEAN_STOP line may change its shard count
            runtime::InMemoryLogStorage storage;
            std::unique_ptr<runtime::StorageLock> lock;
            std::unique_ptr<runtime::Housekeeping> housekeeping;
            std::vector<std::unique_ptr<runtime::Shard>> shards;
            std::vector<std::unique_ptr<ShardLog>> logs;
            std::vector<Instrument> instruments;
            std::vector<std::vector<Logged>> logged;  // by shard, from position 0
            std::vector<std::uint64_t> released;      // by shard: how far responses were released
            bool leftUnlogged = false;
            bool crashedOnce = false;
            bool placementChanged = false;  // once a CLEAN_STOP line changed the shard count
            std::unique_ptr<runtime::ClosedLog> closed;  // what each log writer has closed
            std::vector<std::unique_ptr<runtime::ShardRoller>> rollers;  // by shard
        };
    }  // namespace

    namespace {

        // readShardLog over any store: its names in order, and each file's bytes, checked whole.
        ShardLogRecords readShardFiles(const std::vector<std::string>& names, std::uint32_t shard,
                                       const std::function<std::vector<std::byte>(const std::string&)>& wholeFile) {
            ShardLogRecords read;
            // Named for their first positions, 20 digits each, so they sort in order.
            const std::string prefix = runtime::logFileName(shard, 0).substr(0, 9);
            for (const std::string& name : names) {
                if (!name.starts_with(prefix)) {
                    continue;
                }
                ++read.files;
                const std::vector<std::byte> bytes = wholeFile(name);
                std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::ios::binary);
                replay::CommandLogReader reader(input);
                std::uint64_t position = read.firstPosition + read.records.size();
                for (;;) {
                    const replay::ReadResult next = reader.next();
                    if (const auto* record = std::get_if<replay::ReplayRecord>(&next)) {
                        read.records.push_back(*record);
                    } else if (const auto* error = std::get_if<replay::ParseError>(&next)) {
                        throw std::runtime_error(name + " doesn't read back: " + error->message);
                    } else {
                        break;
                    }
                }
                if (read.files == 1 && reader.header() != nullptr) {
                    read.firstPosition = reader.header()->firstPosition;  // retention may have deleted those before it
                    position = read.firstPosition;
                } else if (reader.header() != nullptr && reader.header()->firstPosition > position) {
                    // Retention deleted the files between, and a machine crash brought back an older
                    // one whose removal had not reached the drive: the log starts again here.
                    read.records.erase(read.records.begin(), read.records.begin() + static_cast<std::ptrdiff_t>(position - read.firstPosition));
                    read.firstPosition = reader.header()->firstPosition;
                    position = read.firstPosition;
                }
                if (reader.header() == nullptr || reader.header()->firstPosition != position) {
                    throw std::runtime_error(name + " doesn't start where the last file ended, " + std::to_string(position));
                }
            }
            return read;
        }

    }  // namespace

    ShardLogRecords readShardLog(const runtime::InMemoryLogStorage& storage, std::uint32_t shard) {
        return readShardFiles(storage.names(), shard, [&](const std::string& name) {
            const std::optional<runtime::InMemoryLogStorage::File> file = storage.file(name);
            if (!file->closed || file->bytes.size() != file->size) {
                throw std::runtime_error(name + " wasn't closed whole: " + std::to_string(file->bytes.size()) + " of " +
                                         std::to_string(file->size) + " bytes written");
            }
            return file->bytes;
        });
    }

    ShardLogRecords readShardLog(const runtime::PageLogStorage& storage, std::uint32_t shard) {
        std::map<std::string, std::vector<std::byte>> files;
        for (runtime::PageLogStorage::StoredFile& file : storage.files()) {
            files.emplace(file.name, std::move(file.bytes));
        }
        std::vector<std::string> names;
        for (const auto& entry : files) {
            names.push_back(entry.first);
        }
        // A zero tail, which a clean stop cuts, reads as a record that doesn't chain.
        return readShardFiles(names, shard, [&](const std::string& name) { return files.at(name); });
    }

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
        // One or two crashes, each after a command picked from a source of their own.
        std::mt19937_64 crashes(config.seed ^ crashStream);
        std::vector<std::pair<std::size_t, EngineCommand>> crashAfter;
        if (config.crashes && config.commands != 0) {
            const std::size_t count = 1 + static_cast<std::size_t>(crashes() % 2);
            for (std::size_t i = 0; i < count; ++i) {
                const auto after = static_cast<std::size_t>(crashes() % config.commands);
                const bool machine = crashes() % 2 == 0;
                const std::uint64_t seed = crashes();
                crashAfter.emplace_back(after, EngineCommand{0, Command{}, std::nullopt, SymbolFault::NONE,
                                                             machine ? EngineAction::CRASH_MACHINE : EngineAction::CRASH_ENGINE, seed});
            }
            std::stable_sort(crashAfter.begin(), crashAfter.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        // RESTORE lines, each after a command, from a source of their own.
        std::mt19937_64 restores(config.seed ^ restoreStream);
        std::vector<std::pair<std::size_t, EngineCommand>> restoreAfter;
        if (config.commands != 0) {
            for (std::size_t i = 0; i < config.restores; ++i) {
                const auto after = static_cast<std::size_t>(restores() % config.commands);
                const auto instrument = static_cast<std::size_t>(restores() % config.instruments);
                restoreAfter.emplace_back(after, EngineCommand{instrument, Command{}, std::nullopt, SymbolFault::NONE, EngineAction::RESTORE});
            }
            std::stable_sort(restoreAfter.begin(), restoreAfter.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        // ROLL lines, each after a command, from a source of their own.
        std::mt19937_64 rolls(config.seed ^ rollStream);
        std::vector<std::size_t> rollAfter;
        if (config.commands != 0) {
            for (std::size_t i = 0; i < config.rolls; ++i) {
                rollAfter.push_back(static_cast<std::size_t>(rolls() % config.commands));
            }
            std::sort(rollAfter.begin(), rollAfter.end());
        }
        // DAMAGE lines, each after a command, for an instrument, from a source of their own.
        std::mt19937_64 damages(config.seed ^ damageStream);
        std::vector<std::pair<std::size_t, std::size_t>> damageAfter;
        if (config.commands != 0) {
            for (std::size_t i = 0; i < config.damages; ++i) {
                const auto after = static_cast<std::size_t>(damages() % config.commands);
                damageAfter.emplace_back(after, static_cast<std::size_t>(damages() % config.instruments));
            }
            std::stable_sort(damageAfter.begin(), damageAfter.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        // CLEAN_STOP lines, each after a command, with a shard count from 1 to 4, from a source of
        // their own.
        std::mt19937_64 cleanStops(config.seed ^ cleanStopStream);
        std::vector<std::pair<std::size_t, EngineCommand>> cleanStopAfter;
        if (config.commands != 0) {
            for (std::size_t i = 0; i < config.cleanStops; ++i) {
                const auto after = static_cast<std::size_t>(cleanStops() % config.commands);
                EngineCommand stop{0, Command{}, std::nullopt, SymbolFault::NONE, EngineAction::CLEAN_STOP, 0,
                                   static_cast<std::uint32_t>(1 + cleanStops() % 4)};
                if (cleanStops() % 2 == 0) {
                    stop.instrument = static_cast<std::size_t>(cleanStops() % config.instruments);
                    stop.pin = static_cast<std::uint32_t>(cleanStops() % stop.shards);
                }
                cleanStopAfter.emplace_back(after, std::move(stop));
            }
            std::stable_sort(cleanStopAfter.begin(), cleanStopAfter.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        std::size_t nextCleanStop = 0;
        std::size_t nextDamage = 0;
        std::size_t nextRoll = 0;
        std::size_t nextRestore = 0;
        std::size_t nextCrash = 0;
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
            while (nextCrash < crashAfter.size() && crashAfter[nextCrash].first == i) {
                if (config.damages != 0) {
                    // One just before every crash too, which the roller has no pass to heal first,
                    // so the recovery falls back and books on a shard stand at different positions.
                    stream.push_back(EngineCommand{static_cast<std::size_t>(damages() % config.instruments), Command{}, std::nullopt,
                                                   SymbolFault::NONE, EngineAction::DAMAGE});
                }
                stream.push_back(crashAfter[nextCrash++].second);
            }
            while (nextRestore < restoreAfter.size() && restoreAfter[nextRestore].first == i) {
                stream.push_back(restoreAfter[nextRestore++].second);
            }
            while (nextRoll < rollAfter.size() && rollAfter[nextRoll] == i) {
                ++nextRoll;
                stream.push_back(EngineCommand{0, Command{}, std::nullopt, SymbolFault::NONE, EngineAction::ROLL});
            }
            while (nextDamage < damageAfter.size() && damageAfter[nextDamage].first == i) {
                stream.push_back(EngineCommand{damageAfter[nextDamage++].second, Command{}, std::nullopt, SymbolFault::NONE, EngineAction::DAMAGE});
            }
            while (nextCleanStop < cleanStopAfter.size() && cleanStopAfter[nextCleanStop].first == i) {
                stream.push_back(cleanStopAfter[nextCleanStop++].second);
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
