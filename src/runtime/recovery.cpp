#include "order_book/runtime/recovery.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

        constexpr std::string_view placementRule =
            ", and this build can't change placement while a log exists, so start on the old placement";

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
        for (std::size_t i = 0; i < found.size(); ++i) {
            const bool newest = i + 1 == found.size() || found[i + 1].name.shard != found[i].name.shard;
            if (!newest) {
                continue;
            }
            const replay::LogHeader& header = found[i].header;
            if (header.shardCount != directory.shardCount()) {
                throw RecoveryError(RecoveryError::Kind::PLACEMENT_CHANGED,
                                    "the log was written by " + std::to_string(header.shardCount) + " shards and this start has " +
                                        std::to_string(directory.shardCount()) + std::string(placementRule));
            }
            for (const Symbol& symbol : header.symbols) {
                const std::uint32_t now = directory.find(symbol)->shard;
                if (now != header.shardIndex) {
                    throw RecoveryError(RecoveryError::Kind::PLACEMENT_CHANGED,
                                        symbol.text() + " moved from shard " + std::to_string(header.shardIndex) + " to shard " +
                                            std::to_string(now) + std::string(placementRule));
                }
            }
        }
        for (const Found& file : found) {
            survey.shards[file.name.shard].push_back(LogFileEntry{file.file, file.name.firstPosition});
        }
        return survey;
    }

    RecoveredShard recoverShard(LogStorage& storage, LogStorage& checkpointStore, const InstrumentDirectory& directory,
                                const std::vector<LogFileEntry>& files, const std::vector<std::vector<std::uint64_t>>& bookCheckpoints,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping) {
        const std::uint32_t number = shard.books().shard();
        const std::span<const Instrument> instruments = directory.onShard(number);
        RecoveredShard recovered;

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
                                            std::string(placementRule));
                } else {
                    std::variant<OrderBook, RestoreError> book = OrderBook::restore(checkpoint->image);
                    if (const auto* error = std::get_if<RestoreError>(&book)) {
                        fault = std::string(restoreErrorCodeName(error->code)) + " at " + std::to_string(error->offset) + ": " + error->message;
                    } else {
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
        recovered.start = oldest.value_or(0);
        for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
            if (slot >= bookCheckpoints.size() || bookCheckpoints[slot].empty()) {
                recovered.restoredFrom[slot] = recovered.start;
            }
        }
        shard.startAt(recovered.start);
        std::vector<LogFileEntry> kept = files;
        while (!kept.empty()) {
            const std::optional<std::uint64_t> dropped = droppedWithoutARecord(storage, kept.back());
            if (!dropped) {
                break;
            }
            storage.remove(kept.back().name);
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
                stopRecovery(where(number, shard.nextPosition(), entry.name) + ": " + finding(error) +
                     (tear ? ", in a file before the last, which its log writer flushed whole" : ""));
            }
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

        if (const std::optional<ShardViolation> violation = shard.checkInvariants()) {
            stopRecovery("shard " + std::to_string(number) + " position " + std::to_string(shard.nextPosition()) + " symbol " +
                 shard.books().symbol(violation->slot).text() + ": replay left a book broken: " + violation->message);
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
                storage.remove(cut->file);
                ++recovered.filesRemoved;
            } else {
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
