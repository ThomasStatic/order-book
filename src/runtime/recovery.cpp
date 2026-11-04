#include "order_book/runtime/recovery.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <streambuf>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/runtime/log_writer.hpp"

namespace order_book::runtime {

    static_assert(LogWriter::mostBytesPerWrite <= tornWriteReach, "one write must stay within what recovery cuts as torn");

    namespace {

        // A LogFile as a stream, for CommandLogReader, reading at most limit bytes. A storage
        // failure escapes the reader as the LogStorageError it was, since its stream rethrows.
        class LogFileBuffer final : public std::streambuf {
        public:
            LogFileBuffer(LogFile& source, std::uint64_t limit) : file(source), end(limit), held(64 * 1024) {}

        protected:
            int_type underflow() override {
                if (gptr() < egptr()) {
                    return traits_type::to_int_type(*gptr());
                }
                const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(held.size(), end - std::min(end, offset)));
                const std::size_t got = wanted == 0 ? 0 : file.read(offset, std::as_writable_bytes(std::span(held.data(), wanted)));
                if (got == 0) {
                    return traits_type::eof();
                }
                offset += got;
                setg(held.data(), held.data(), held.data() + got);
                return traits_type::to_int_type(*gptr());
            }

        private:
            LogFile& file;
            std::uint64_t end;
            std::uint64_t offset = 0;
            std::vector<char> held;
        };

        // One log file opened and read through CommandLogReader, whole or only its header.
        class LogFileReader {
        public:
            LogFileReader(LogStorage& storage, const std::string& name, std::uint64_t limit = UINT64_MAX)
                : file(storage.open(name)), buffer(*file, limit), input(&buffer), reader(input) {
                input.exceptions(std::ios::badbit);
            }

            replay::ReadResult next() { return reader.next(); }
            const replay::LogHeader* header() const { return reader.header(); }

        private:
            std::unique_ptr<LogFile> file;
            LogFileBuffer buffer;
            std::istream input;
            replay::CommandLogReader reader;
        };

        std::uint32_t load32(std::span<const std::byte> bytes, std::size_t at) {
            std::uint32_t value = 0;
            for (std::size_t i = 4; i-- > 0;) {
                value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at + i]);
            }
            return value;
        }

        // What a header takes in its file, symbols and checksum included: where records start.
        std::uint64_t wholeHeaderBytes(const replay::LogHeader& header) {
            return std::uint64_t{header.headerBytes} + header.symbols.size() * Symbol::size + sizeof(std::uint64_t);
        }

        struct FileHeader {
            std::variant<replay::LogHeader, replay::ParseError> parsed;
            std::uint64_t bytes = 0;  // the whole header's
        };

        // Only the header, so a file's zero tail isn't read to find a torn tail nobody asked about.
        FileHeader readHeader(LogStorage& storage, const std::string& name) {
            std::uint64_t limit = replay::logHeaderBytes;
            {
                const std::unique_ptr<LogFile> file = storage.open(name);
                std::array<std::byte, replay::logHeaderBytes> fixed{};
                if (file->read(0, fixed) == fixed.size()) {
                    limit = std::min<std::uint64_t>(file->size(), std::uint64_t{load32(fixed, 16)} +
                                                                      std::uint64_t{load32(fixed, 48)} * Symbol::size +
                                                                      sizeof(std::uint64_t));
                }
            }
            LogFileReader reader(storage, name, limit);
            const replay::ReadResult result = reader.next();
            if (reader.header() == nullptr) {
                return FileHeader{std::get<replay::ParseError>(result), 0};
            }
            return FileHeader{*reader.header(), wholeHeaderBytes(*reader.header())};
        }

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

    LogSurvey surveyLog(LogStorage& storage, const InstrumentDirectory& directory) {
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

    RecoveredShard recoverShard(LogStorage& storage, const InstrumentDirectory& directory, const std::vector<LogFileEntry>& files,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping) {
        const std::uint32_t number = shard.books().shard();
        RecoveredShard recovered;
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
        for (std::size_t i = 0; i < kept.size(); ++i) {
            const LogFileEntry& entry = kept[i];
            const std::uint64_t expected = shard.nextPosition();
            if (entry.firstPosition != expected) {
                if (i == 0) {
                    stopRecovery(where(number, 0, entry.name) + ": the log starts at position " + std::to_string(entry.firstPosition) +
                         ", and this build recovers only from position 0");
                }
                stopRecovery(where(number, expected, entry.name) + ": a gap: the file before it ends at position " + std::to_string(expected) +
                     ", and this one starts at " + std::to_string(entry.firstPosition));
            }
            LogFileReader reader(storage, entry.name);
            for (std::uint64_t inFile = 0;; ++inFile) {
                if (inFile % recordsBetweenLooks == 0 && stopping.load(std::memory_order_seq_cst)) {
                    throw RecoveryInterrupted{};
                }
                replay::ReadResult result = reader.next();
                if (const auto* record = std::get_if<replay::ReplayRecord>(&result)) {
                    const std::optional<BookLocation> location = directory.find(record->symbol);
                    if (!location || location->shard != number) {
                        stopRecovery(where(number, shard.nextPosition(), entry.name) + " symbol " + record->symbol.text() + " command " +
                             replay::formatReplayRow(*record) + ": the record names an instrument this shard doesn't hold");
                    }
                    shard.process(record->command, location->slot);
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

        if (const std::optional<ShardViolation> violation = shard.checkInvariants()) {
            stopRecovery("shard " + std::to_string(number) + " position " + std::to_string(shard.nextPosition()) + " symbol " +
                 shard.books().symbol(violation->slot).text() + ": replay left a book broken: " + violation->message);
        }

        const std::span<const Instrument> instruments = directory.onShard(number);
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
