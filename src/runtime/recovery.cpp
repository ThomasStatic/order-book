#include "order_book/runtime/recovery.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "log_file_reader.hpp"

namespace order_book::runtime {

    static_assert(LogWriter::mostBytesPerWrite <= tornWriteReach, "one write must stay within what recovery cuts as torn");

    namespace {

        using namespace log_reading;

        std::string where(std::uint32_t shard, std::uint64_t position, const std::string& file) {
            return "shard " + std::to_string(shard) + " position " + std::to_string(position) + " file " + file;
        }

        std::string finding(const replay::ParseError& error) {
            return std::string(replay::parseErrorCodeName(error.code)) + ": " + error.message;
        }

        [[noreturn]] void stopRecovery(const std::string& what) {
            throw RecoveryError(RecoveryError::Kind::RECOVERY_STOPPED, what);
        }

        // 1.5 times, rounded up.
        std::size_t halfAgain(std::size_t held) {
            return held + (held + 1) / 2;
        }

        // How many bytes from offset on a write reached: up to the last one that isn't zero, since
        // an engine never writes a zero record, so an extended file's zero tail counts for none.
        std::uint64_t writtenFrom(LogFile& file, std::uint64_t offset) {
            std::vector<std::byte> chunk(64 * 1024);
            std::uint64_t lastWritten = offset;
            for (std::uint64_t at = offset;;) {
                const std::size_t got = file.read(at, chunk);
                for (std::size_t i = got; i-- > 0;) {
                    if (chunk[i] != std::byte{0}) {
                        lastWritten = at + i + 1;
                        break;
                    }
                }
                if (got < chunk.size()) {
                    return lastWritten - offset;
                }
                at += got;
            }
        }

        // For a file that holds no record, the bytes a write reached past its header, which
        // removing it drops; nothing for one that holds a record, or that replay will stop on. A
        // first record's room all zeros holds none, since an engine never writes a zero command
        // byte, and neither does a first record torn as it was written.
        std::optional<std::uint64_t> droppedWithoutARecord(LogStorage& storage, const LogFileEntry& entry) {
            const FileHeader header = readHeader(storage, entry.name);
            if (!std::holds_alternative<replay::LogHeader>(header.parsed)) {
                return std::nullopt;
            }
            // One handle at a time: a file opened to cut shares no writing with another handle.
            std::uint64_t written = 0;
            {
                const std::unique_ptr<LogFile> file = storage.open(entry.name);
                written = writtenFrom(*file, header.bytes);
            }
            if (written == 0) {
                return 0;  // all zeros: the next file made ahead, as it was made
            }
            // A first record that is all zeros may still be a hole that valid records follow,
            // which replay decides on.
            LogFileReader reader(storage, entry.name);
            const replay::ReadResult result = reader.next();
            const auto* error = std::get_if<replay::ParseError>(&result);
            if (error == nullptr || error->code != replay::ParseErrorCode::TORN_TAIL) {
                return std::nullopt;
            }
            return written;
        }

        // Where a shard's log ends, if it ended as a clean stop leaves it: trailing files with no
        // record set aside, as a clean stop's last file or a start's first can be, the last that
        // holds one read whole to its end. Nothing for a torn tail, a torn first record, or any
        // finding the reader makes.
        std::optional<std::uint64_t> cleanEnd(LogStorage& storage, const std::vector<LogFileEntry>& files) {
            std::size_t last = files.size();
            std::optional<std::uint64_t> end;
            while (last > 0 && !end) {
                const LogFileEntry& file = files[last - 1];
                const std::optional<std::uint64_t> dropped = droppedWithoutARecord(storage, file);
                if (dropped == std::uint64_t{0}) {
                    --last;
                    continue;
                }
                if (dropped) {
                    return std::nullopt;  // a first record torn as it was written
                }
                LogFileReader reader(storage, file.name);
                std::uint64_t records = 0;
                for (;;) {
                    const replay::ReadResult result = reader.next();
                    if (std::holds_alternative<replay::ReplayRecord>(result)) {
                        ++records;
                    } else if (std::holds_alternative<replay::EndOfInput>(result)) {
                        end = file.firstPosition + records;
                        break;
                    } else {
                        return std::nullopt;
                    }
                }
            }
            if (!end) {
                end = files.empty() ? 0 : files.front().firstPosition;
            }
            // Each file set aside must start at that end, as a clean stop's or a start's does: one
            // past it held records a crash zeroed, which the files after it still count.
            for (std::size_t i = last; i < files.size(); ++i) {
                if (files[i].firstPosition != *end) {
                    return std::nullopt;
                }
            }
            return end;
        }

        [[noreturn]] void refusePlacementChange(const std::string& what) {
            throw RecoveryError(RecoveryError::Kind::PLACEMENT_CHANGED,
                                what + " and the last run did not stop cleanly; restart on the old placement and stop cleanly first");
        }

        std::vector<std::byte> readWholeFile(LogStorage& storage, const std::string& name) {
            const std::unique_ptr<LogFile> file = storage.open(name);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
            bytes.resize(file->read(0, bytes));
            return bytes;
        }

    }  // namespace

    LogSurvey surveyLog(LogStorage& storage, LogStorage& checkpointStore, const InstrumentDirectory& directory) {
        struct Found {
            LogFileNameParts name;
            std::string file;
            replay::LogHeader header;
        };
        LogSurvey survey;
        survey.shards.resize(directory.shardCount());
        std::vector<Found> found;
        for (const std::string& name : storage.names()) {
            if (!namedLikeALogFile(name)) {
                continue;  // the lock, cut records, and anything else that isn't the log's
            }
            const std::optional<LogFileNameParts> parsed = parseLogFileName(name);
            if (!parsed) {
                throw RecoveryError(RecoveryError::Kind::START_REFUSED, name + " is named like a log file, but not as an engine names one");
            }
            if (parsed->temporary) {
                survey.temporaries.push_back(name);
                continue;
            }
            FileHeader header = readHeader(storage, name);
            if (const auto* error = std::get_if<replay::ParseError>(&header.parsed)) {
                stopRecovery(where(parsed->shard, parsed->firstPosition, name) + ": " + finding(*error));
            }
            replay::LogHeader& read = std::get<replay::LogHeader>(header.parsed);
            if (read.shardIndex != parsed->shard || read.firstPosition != parsed->firstPosition) {
                stopRecovery(where(parsed->shard, parsed->firstPosition, name) + ": its header says shard " + std::to_string(read.shardIndex) +
                     " position " + std::to_string(read.firstPosition));
            }
            if (read.shardCount == 0) {
                stopRecovery(where(parsed->shard, parsed->firstPosition, name) + ": its header holds no placement, so no engine wrote it");
            }
            found.push_back(Found{*parsed, name, std::move(read)});
        }
        // Names sort by shard, then first position, since both are fixed width.
        for (const Found& file : found) {
            for (const Symbol& symbol : file.header.symbols) {
                if (!directory.find(symbol)) {
                    throw RecoveryError(RecoveryError::Kind::START_REFUSED,
                                        file.file + " names " + symbol.text() + ", which is not on the instrument list");
                }
            }
            survey.named.insert(survey.named.end(), file.header.symbols.begin(), file.header.symbols.end());
        }
        std::sort(survey.named.begin(), survey.named.end());
        survey.named.erase(std::unique(survey.named.begin(), survey.named.end()), survey.named.end());

        // Each book's checkpoints, by name: a symbol off the list refuses the start, as in a header.
        survey.checkpoints.resize(directory.shardCount());
        survey.newBooks.resize(directory.shardCount());
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            survey.checkpoints[shard].resize(directory.onShard(shard).size());
        }
        for (const std::string& name : checkpointStore.names()) {
            const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
            if (!parts) {
                continue;
            }
            if (parts->temporary) {
                survey.checkpointTemporaries.push_back(name);
                continue;
            }
            const std::optional<BookLocation> location = directory.find(parts->symbol);
            if (!location) {
                throw RecoveryError(RecoveryError::Kind::START_REFUSED,
                                    name + " names " + parts->symbol.text() + ", which is not on the instrument list");
            }
            survey.checkpoints[location->shard][location->slot].push_back(parts->position);
        }
        // A listed instrument with no checkpoint that a header names lost one; named nowhere, new.
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            const std::span<const Instrument> instruments = directory.onShard(shard);
            for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
                std::vector<std::uint64_t>& positions = survey.checkpoints[shard][slot];
                std::sort(positions.rbegin(), positions.rend());
                if (!positions.empty()) {
                    continue;
                }
                if (std::binary_search(survey.named.begin(), survey.named.end(), instruments[slot].symbol)) {
                    stopRecovery("shard " + std::to_string(shard) + " symbol " + instruments[slot].symbol.text() +
                                 ": a log file's header names it, and no checkpoint holds it: a lost checkpoint");
                }
                survey.newBooks[shard].push_back(slot);
            }
        }
        // Every shard's files, a shard no longer running included, and the current ones' to recover.
        std::map<std::uint32_t, std::vector<LogFileEntry>> logs;
        for (const Found& file : found) {
            logs[file.name.shard].push_back(LogFileEntry{file.file, file.name.firstPosition});
            if (file.name.shard < directory.shardCount()) {
                survey.shards[file.name.shard].push_back(LogFileEntry{file.file, file.name.firstPosition});
            }
        }

        // Placement (D6b): each current shard's newest header against the directory. A shard past
        // the count is a log a change left, which no shard writes, so it moves nothing.
        std::optional<std::string> change;
        for (std::size_t i = 0; i < found.size() && !change; ++i) {
            const bool newest = i + 1 == found.size() || found[i + 1].name.shard != found[i].name.shard;
            if (!newest || found[i].name.shard >= directory.shardCount()) {
                continue;
            }
            const replay::LogHeader& header = found[i].header;
            if (header.shardCount != directory.shardCount()) {
                change = "the log was written by " + std::to_string(header.shardCount) + " shards and this start has " +
                         std::to_string(directory.shardCount());
                break;
            }
            for (const Symbol& symbol : header.symbols) {
                const std::uint32_t now = directory.find(symbol)->shard;
                if (now != header.shardIndex) {
                    change = symbol.text() + " moved from shard " + std::to_string(header.shardIndex) + " to shard " + std::to_string(now);
                    break;
                }
            }
        }
        if (!change) {
            return survey;
        }

        // Every tail must be empty: each book's newest checkpoint at the end of the log its own
        // header names, each log ending cleanly.
        const std::string& what = *change;
        std::map<std::uint32_t, std::optional<std::uint64_t>> ends;
        const auto endOf = [&](std::uint32_t shard) -> std::optional<std::uint64_t> {
            if (const auto known = ends.find(shard); known != ends.end()) {
                return known->second;
            }
            const auto files = logs.find(shard);
            const std::optional<std::uint64_t> end = files == logs.end() ? std::optional<std::uint64_t>(0) : cleanEnd(storage, files->second);
            ends.emplace(shard, end);
            return end;
        };
        PlacementChange placement;
        placement.what = *change;
        // Each book with a checkpoint: where it stands, and where a rewrap takes it, if anywhere.
        struct Standing {
            const Symbol* symbol;
            const std::vector<std::uint64_t>* positions;
            std::uint32_t shard;  // under the placement it was written by, the same if not rewrapped
            std::optional<std::uint64_t> to;
        };
        std::vector<Standing> standing;
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            const std::span<const Instrument> instruments = directory.onShard(shard);
            for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
                const std::vector<std::uint64_t>& positions = survey.checkpoints[shard][slot];
                if (positions.empty()) {
                    continue;  // new: its empty checkpoint goes where its shard's log ends
                }
                const Symbol& symbol = instruments[slot].symbol;
                const std::vector<std::byte> bytes = readWholeFile(checkpointStore, checkpointFileName(symbol, positions.front()));
                const auto decoded = decodeCheckpoint(bytes);
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                if (checkpoint == nullptr || checkpoint->header.symbol != symbol || checkpoint->header.position != positions.front()) {
                    refusePlacementChange(what);  // nothing to say where its tail ends
                }
                const CheckpointHeader& header = checkpoint->header;
                if (endOf(header.shardIndex) != header.position) {
                    refusePlacementChange(what);
                }
                std::optional<std::uint64_t> to;
                if (header.shardCount != directory.shardCount() || header.shardIndex != shard) {
                    to = endOf(shard);
                    if (!to) {
                        refusePlacementChange(what);
                    }
                    placement.rewraps.push_back(Rewrap{symbol, header.position, header.shardIndex, header.shardCount, shard,
                                                       directory.shardCount(), *to});
                }
                standing.push_back(Standing{&symbol, &positions, header.shardIndex, to});
            }
        }
        // An older checkpoint's position voids where a fall-back to it would replay a log across
        // the change: a rewrapped book's, which a new count makes every book, and a book's on a
        // shard some book left, whose log holds that book's records after it. One a rewrap is
        // landing on, which a crash left, stays.
        std::vector<std::uint32_t> left;
        for (const Rewrap& rewrap : placement.rewraps) {
            left.push_back(rewrap.fromShard);
        }
        for (const Standing& book : standing) {
            const bool voided = book.to || std::find(left.begin(), left.end(), book.shard) != left.end();
            for (std::size_t older = 1; voided && older < book.positions->size(); ++older) {
                if ((*book.positions)[older] != book.to) {
                    placement.dropped.push_back(checkpointFileName(*book.symbol, (*book.positions)[older]));
                }
            }
        }
        survey.placement = std::move(placement);
        return survey;
    }

    void changePlacement(LogStorage& checkpoints, const PlacementChange& change) {
        for (const std::string& name : change.dropped) {
            checkpoints.remove(name);
        }
        for (const Rewrap& rewrap : change.rewraps) {
            const std::string from = checkpointFileName(rewrap.symbol, rewrap.from);
            const std::vector<std::byte> bytes = readWholeFile(checkpoints, from);
            const auto decoded = decodeCheckpoint(bytes);
            const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
            if (checkpoint == nullptr) {
                stopRecovery(from + " no longer decodes, so it can't be rewrapped for the new placement");
            }
            CheckpointHeader header = checkpoint->header;
            header.engineMajor = ORDER_BOOK_VERSION_MAJOR;
            header.engineMinor = ORDER_BOOK_VERSION_MINOR;
            header.enginePatch = ORDER_BOOK_VERSION_PATCH;
            header.shardCount = rewrap.toShards;
            header.shardIndex = rewrap.toShard;
            header.position = rewrap.to;
            // Over any file of its name: the one it came from, at the same position, or one an
            // interrupted change left.
            publishCheckpoint(checkpoints, header, checkpoint->image, true);
            if (rewrap.to != rewrap.from) {
                checkpoints.remove(from);
            }
        }
    }

    RecoveredShard recoverShard(LogStorage& storage, LogStorage& checkpointStore, const InstrumentDirectory& directory,
                                const std::vector<LogFileEntry>& files, const std::vector<std::vector<std::uint64_t>>& bookCheckpoints,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping) {
        return recoverShard(storage, checkpointStore, directory, files, bookCheckpoints, shard, defaults, stopping, RecoveryOptions{});
    }

    RecoveredShard recoverShard(LogStorage& storage, LogStorage& checkpointStore, const InstrumentDirectory& directory,
                                const std::vector<LogFileEntry>& files, const std::vector<std::vector<std::uint64_t>>& bookCheckpoints,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping, const RecoveryOptions& options) {
        const std::uint32_t number = shard.books().shard();
        const std::span<const Instrument> instruments = directory.onShard(number);
        RecoveredShard recovered;
        // What is wrong with a book, its own check first, or nothing.
        const auto faultOf = [&options](const OrderBook& book) -> std::optional<std::string> {
            const InvariantResult invariants = book.checkInvariants();
            if (!invariants.isValid()) {
                return formatInvariantResult(invariants);
            }
            return options.check ? options.check(book) : std::nullopt;
        };
        const auto broken = [](const std::string& what) {
            throw RecoveryError(RecoveryError::Kind::BOOK_BROKEN, what);
        };

        // Each book from its newest checkpoint that restores, or an older one.
        recovered.restoredFrom.assign(instruments.size(), 0);
        std::optional<std::uint64_t> oldest;
        for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
            const std::vector<std::uint64_t> none;
            const std::vector<std::uint64_t>& positions = slot < bookCheckpoints.size() ? bookCheckpoints[slot] : none;
            if (positions.empty()) {
                continue;  // new: an empty book, which nothing in the log names
            }
            const Symbol& symbol = instruments[slot].symbol;
            std::string why;
            bool restored = false;
            for (const std::uint64_t position : positions) {
                const std::string name = checkpointFileName(symbol, position);
                std::vector<std::byte> bytes;
                {
                    const std::unique_ptr<LogFile> file = checkpointStore.open(name);
                    bytes.resize(static_cast<std::size_t>(file->size()));
                    bytes.resize(file->read(0, bytes));
                }
                const auto decoded = decodeCheckpoint(bytes);
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                std::string fault;
                if (checkpoint == nullptr) {
                    const CheckpointError& error = std::get<CheckpointError>(decoded);
                    fault = std::string(checkpointErrorCodeName(error.code)) + " at " + std::to_string(error.offset) + ": " + error.message;
                } else if (checkpoint->header.symbol != symbol || checkpoint->header.position != position) {
                    fault = "its header says " + checkpoint->header.symbol.text() + " at " + std::to_string(checkpoint->header.position);
                } else if (checkpoint->header.shardCount != directory.shardCount() || checkpoint->header.shardIndex != number) {
                    throw RecoveryError(RecoveryError::Kind::PLACEMENT_CHANGED,
                                        name + " was written by shard " + std::to_string(checkpoint->header.shardIndex) + " of " +
                                            std::to_string(checkpoint->header.shardCount) + ", and this start places " + symbol.text() +
                                            " on shard " + std::to_string(number) + " of " + std::to_string(directory.shardCount()) +
                                            ", though the logs' headers name this placement: a placement change was cut short, so "
                                            "start on the placement that checkpoint names to finish it");
                } else {
                    std::variant<OrderBook, RestoreError> book = OrderBook::restore(checkpoint->image);
                    if (const auto* error = std::get_if<RestoreError>(&book)) {
                        fault = std::string(restoreErrorCodeName(error->code)) + " at " + std::to_string(error->offset) + ": " + error->message;
                    } else {
                        if (options.perCommand) {
                            if (const std::optional<std::string> wrong = faultOf(std::get<OrderBook>(book))) {
                                broken("shard " + std::to_string(number) + " symbol " + symbol.text() + ": " + name +
                                       " restores a book that fails its check: " + *wrong);
                            }
                        }
                        shard.replaceBook(slot, std::move(std::get<OrderBook>(book)));
                        recovered.restoredFrom[slot] = position;
                        oldest = oldest ? std::min(*oldest, position) : position;
                        restored = true;
                        break;
                    }
                }
                why += (why.empty() ? "" : "; ") + name + ": " + fault;
            }
            if (!restored) {
                stopRecovery("shard " + std::to_string(number) + " symbol " + symbol.text() + ": no checkpoint restores: " + why);
            }
            if (recovered.restoredFrom[slot] != positions.front()) {
                ++recovered.fellBack;
            }
        }
        // With no book standing anywhere, as when a placement change moved every book away or the
        // shard holds only new ones, nothing here needs a record: only the last file is read, its
        // records passed over, their chain still checked, and the shard starts where it ends.
        const bool bookless = !oldest;
        recovered.start = oldest.value_or(std::numeric_limits<std::uint64_t>::max());
        for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
            if (slot >= bookCheckpoints.size() || bookCheckpoints[slot].empty()) {
                recovered.restoredFrom[slot] = recovered.start;
            }
        }
        if (!bookless) {
            shard.startAt(recovered.start);
        }
        std::vector<LogFileEntry> kept = files;
        while (!kept.empty()) {
            const std::optional<std::uint64_t> dropped = droppedWithoutARecord(storage, kept.back());
            if (!dropped) {
                break;
            }
            if (!options.readOnly) {
                storage.remove(kept.back().name);
            }
            recovered.tailCutBytes += *dropped;
            ++recovered.filesRemoved;
            kept.pop_back();
        }

        constexpr std::uint64_t recordsBetweenLooks = 1024;
        // Where the last file's tail is cut: a torn tail, or a hole valid records follow.
        struct Cut {
            std::string file;
            std::uint64_t offset = 0;
            std::uint64_t headerBytes = 0;
            std::uint64_t recordBytes = 0;
            std::optional<std::string> holeFinding;  // what the reader said, for a hole valid records follow
        };
        std::optional<Cut> cut;
        // The files before the one the oldest book stands in are not needed, and may be gone.
        std::size_t first = 0;
        while (first + 1 < kept.size() && kept[first + 1].firstPosition <= recovered.start) {
            ++first;
        }
        const LogFileEntry* earliest = !kept.empty() ? &kept[first] : !files.empty() ? &files.front() : nullptr;
        if (earliest != nullptr && earliest->firstPosition > recovered.start) {
            // Files with no record count too: a log that resumes past the oldest book lost what lay between.
            stopRecovery(where(number, recovered.start, earliest->name) + ": a gap: the oldest book's checkpoint stands at position " +
                         std::to_string(recovered.start) + ", and the log starts at " + std::to_string(earliest->firstPosition));
        }
        // Where the log ends so far: with no file holding a record, where the first trailing file
        // removed began, or 0 with no file at all.
        std::uint64_t logEnd = !kept.empty() ? kept[first].firstPosition : files.empty() ? 0 : files.front().firstPosition;
        for (std::size_t i = first; i < kept.size(); ++i) {
            const LogFileEntry& entry = kept[i];
            if (i != first && entry.firstPosition != logEnd) {
                stopRecovery(where(number, logEnd, entry.name) + ": a gap: the file before it ends at position " + std::to_string(logEnd) +
                             ", and this one starts at " + std::to_string(entry.firstPosition));
            }
            LogFileReader reader(storage, entry.name);
            for (std::uint64_t inFile = 0;; ++inFile) {
                if (inFile % recordsBetweenLooks == 0 && stopping.load(std::memory_order_seq_cst)) {
                    throw RecoveryInterrupted{};
                }
                replay::ReadResult result = reader.next();
                if (const auto* record = std::get_if<replay::ReplayRecord>(&result)) {
                    const std::uint64_t position = entry.firstPosition + inFile;
                    logEnd = position + 1;
                    if (position < recovered.start) {
                        continue;  // before every book's checkpoint
                    }
                    const std::optional<BookLocation> location = directory.find(record->symbol);
                    if (!location || location->shard != number) {
                        stopRecovery(where(number, shard.nextPosition(), entry.name) + " symbol " + record->symbol.text() + " command " +
                             replay::formatReplayRow(*record) + ": the record names an instrument this shard doesn't hold");
                    }
                    if (position < recovered.restoredFrom[location->slot]) {
                        shard.skip();  // its book's checkpoint holds it already
                    } else {
                        shard.process(record->command, location->slot);
                        if (options.perCommand) {
                            if (const std::optional<std::string> wrong = faultOf(shard.books().book(location->slot))) {
                                broken(where(number, position, entry.name) + " symbol " + record->symbol.text() + " command " +
                                       replay::formatReplayRow(*record) + ": the book fails its check after it: " + *wrong);
                            }
                        }
                    }
                    continue;
                }
                if (std::holds_alternative<replay::EndOfInput>(result)) {
                    break;
                }
                const replay::ParseError& error = std::get<replay::ParseError>(result);
                if (reader.header() == nullptr) {
                    stopRecovery(where(number, entry.firstPosition, entry.name) + ": " + finding(error));
                }
                const bool last = i + 1 == kept.size();
                const bool tear = error.code == replay::ParseErrorCode::TORN_TAIL || error.code == replay::ParseErrorCode::CORRUPT_RECORD;
                if (tear && last) {
                    const std::uint64_t headerBytes = wholeHeaderBytes(*reader.header());
                    cut = Cut{entry.name, headerBytes + inFile * reader.header()->recordBytes, headerBytes, reader.header()->recordBytes,
                              std::nullopt};
                    if (error.code == replay::ParseErrorCode::CORRUPT_RECORD) {
                        cut->holeFinding = finding(error);
                    }
                    break;
                }
                // The record's own position, which a record before the replay's start has too.
                stopRecovery(where(number, entry.firstPosition + inFile, entry.name) + ": " + finding(error) +
                     (tear ? ", in a file before the last, which its log writer flushed whole" : ""));
            }
        }
        if (bookless) {
            recovered.start = logEnd;
            std::fill(recovered.restoredFrom.begin(), recovered.restoredFrom.end(), logEnd);
            shard.startAt(logEnd);
        }

        std::uint64_t cutBytes = 0;
        if (cut) {
            const std::unique_ptr<LogFile> file = storage.open(cut->file);
            cutBytes = writtenFrom(*file, cut->offset);
            // A whole number of records, since a write starts and ends on one.
            const std::uint64_t reach = (cutBytes + cut->recordBytes - 1) / cut->recordBytes * cut->recordBytes;
            if (cut->holeFinding && reach > tornWriteReach) {
                stopRecovery(where(number, shard.nextPosition(), cut->file) + ": " + *cut->holeFinding + ", with " + std::to_string(reach) +
                             " bytes of records from it to the end of what was written, more than one write's " + std::to_string(tornWriteReach));
            }
        }

        // No book may stand past what the log holds: the disk lost data it had confirmed.
        // Every checkpoint counts, one that didn't decode included: its name says the disk had the log
        // that far.
        for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
            const std::uint64_t newest = slot < bookCheckpoints.size() && !bookCheckpoints[slot].empty()
                                             ? std::max(bookCheckpoints[slot].front(), recovered.restoredFrom[slot])
                                             : recovered.restoredFrom[slot];
            if (newest > logEnd) {
                stopRecovery("shard " + std::to_string(number) + " position " + std::to_string(newest) + " symbol " +
                             instruments[slot].symbol.text() + ": its checkpoint stands past the log's end, " + std::to_string(logEnd) +
                             ": the disk lost data it had confirmed");
            }
        }

        // A book broken by the replay names the range it broke in, since only a check after every
        // command finds the one that broke it.
        const auto brokeIn = [&](std::uint32_t slot, const std::string& why) {
            broken("shard " + std::to_string(number) + " positions " + std::to_string(recovered.restoredFrom[slot]) + " to " +
                   std::to_string(shard.nextPosition()) + " symbol " + shard.books().symbol(slot).text() + ": replay left the book broken: " +
                   why + "; verify --validate per-command finds the command");
        };
        if (const std::optional<ShardViolation> violation = shard.checkInvariants()) {
            if (violation->kind == ShardViolationKind::BOOK) {
                brokeIn(violation->slot, violation->message);
            }
            stopRecovery("shard " + std::to_string(number) + " position " + std::to_string(shard.nextPosition()) + " symbol " +
                 shard.books().symbol(violation->slot).text() + ": replay left the shard broken: " + violation->message);
        }
        if (options.check) {
            for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
                if (const std::optional<std::string> wrong = options.check(shard.books().book(slot))) {
                    brokeIn(slot, *wrong);
                }
            }
        }

        recovered.warmed.reserve(instruments.size());
        for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
            const BookCapacity declared = capacityOf(instruments[slot], defaults);
            const BookCapacity held = shard.books().book(slot).holding();
            const BookCapacity wanted{std::max(declared.orders, halfAgain(held.orders)), std::max(declared.stops, halfAgain(held.stops)),
                                      std::max(declared.levels, halfAgain(held.levels))};
            shard.reserve(slot, wanted);
            recovered.warmed.push_back(wanted);
        }

        if (cut) {
            recovered.tailCutBytes += cutBytes;
            if (cut->offset == cut->headerBytes) {
                // Nothing is left in it, so it goes, as a trailing file with no records does, and
                // the next file can start where it did.
                if (!options.readOnly) {
                    storage.remove(cut->file);
                }
                ++recovered.filesRemoved;
            } else if (!options.readOnly) {
                const std::unique_ptr<LogFile> file = storage.open(cut->file);
                file->cut(cut->offset);
                file->flush();
                file->close();
            }
        }
        recovered.replayed = shard.nextPosition();
        return recovered;
    }

}  // namespace order_book::runtime
