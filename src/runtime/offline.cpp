#include "order_book/runtime/offline.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "log_file_reader.hpp"
#include "order_book/command_log.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/shard.hpp"
#include "order_book/xxh64.hpp"

namespace order_book::runtime {

    namespace {

        const std::atomic<bool> neverStopping{false};

        // Both stores' locks, the checkpoints' only where they are another store. Throws
        // LogStorageError (LOCKED) while anyone else holds either.
        std::pair<std::unique_ptr<StorageLock>, std::unique_ptr<StorageLock>> lockStores(LogStorage& logs, LogStorage& checkpoints) {
            std::unique_ptr<StorageLock> held = logs.lock();
            return {std::move(held), &checkpoints == &logs ? nullptr : checkpoints.lock()};
        }

        // A shard's log files, by first position, a file being made left out.
        std::vector<LogFileEntry> shardFiles(const LogStorage& logs, std::uint32_t shard) {
            std::vector<LogFileEntry> files;
            for (const std::string& name : logs.names()) {
                const std::optional<LogFileNameParts> parts = parseLogFileName(name);
                if (parts && !parts->temporary && parts->shard == shard) {
                    files.push_back(LogFileEntry{name, parts->firstPosition});
                }
            }
            std::sort(files.begin(), files.end(), [](const LogFileEntry& a, const LogFileEntry& b) { return a.firstPosition < b.firstPosition; });
            return files;
        }

        std::vector<std::byte> readWhole(LogStorage& storage, const std::string& name) {
            const std::unique_ptr<LogFile> file = storage.open(name);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
            bytes.resize(file->read(0, bytes));
            return bytes;
        }

        replay::LogHeader headerOf(LogStorage& logs, const std::string& name, std::uint64_t& headerBytes) {
            const log_reading::FileHeader header = log_reading::readHeader(logs, name);
            if (const auto* error = std::get_if<replay::ParseError>(&header.parsed)) {
                throw RecoveryError(RecoveryError::Kind::RECOVERY_STOPPED, name + ": " + error->message);
            }
            headerBytes = header.bytes;
            return std::get<replay::LogHeader>(header.parsed);
        }

        std::uint32_t chainChecksum(std::span<const std::byte> record, std::uint32_t previous) {
            return static_cast<std::uint32_t>(replay::xxh64(record.subspan(4), previous) & 0xFFFFFFFFULL);
        }

        void store32(std::span<std::byte> bytes, std::uint32_t value) {
            for (std::size_t i = 0; i < 4; ++i, value >>= 8) {
                bytes[i] = static_cast<std::byte>(value & 0xFF);
            }
        }

        // A storage failure as it names itself, a lock another holds included.
        std::string storageMessage(const LogStorageError& error) {
            return error.what();
        }

    }  // namespace

    InstrumentDirectory directoryOfStore(LogStorage& logs, LogStorage& checkpoints) {
        std::map<std::uint32_t, std::string> newest;  // by shard, its newest file
        for (const std::string& name : logs.names()) {
            const std::optional<LogFileNameParts> parts = parseLogFileName(name);
            if (parts && !parts->temporary) {
                const auto known = newest.find(parts->shard);
                if (known == newest.end() || parseLogFileName(known->second)->firstPosition < parts->firstPosition) {
                    newest[parts->shard] = name;
                }
            }
        }
        if (!newest.contains(0)) {
            throw RecoveryError(RecoveryError::Kind::START_REFUSED, "the log directory holds no log for shard 0, so nothing says its placement");
        }
        std::uint64_t ignored = 0;
        const std::uint32_t shardCount = headerOf(logs, newest.at(0), ignored).shardCount;
        std::vector<Instrument> instruments;
        std::vector<Symbol> listed;
        for (const auto& [shard, name] : newest) {
            if (shard >= shardCount) {
                continue;  // a log a placement change left
            }
            for (const Symbol& symbol : headerOf(logs, name, ignored).symbols) {
                if (std::find(listed.begin(), listed.end(), symbol) == listed.end()) {
                    listed.push_back(symbol);
                    instruments.push_back(Instrument{symbol, shard, std::nullopt, std::nullopt, std::nullopt});
                }
            }
        }
        // A new instrument's empty checkpoint, which no log header lists yet: its newest's shard.
        std::map<std::string, std::pair<std::uint64_t, std::string>> unlisted;
        for (const std::string& name : checkpoints.names()) {
            const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
            if (!parts || parts->temporary || std::find(listed.begin(), listed.end(), parts->symbol) != listed.end()) {
                continue;
            }
            auto& found = unlisted[parts->symbol.text()];
            if (found.second.empty() || found.first < parts->position) {
                found = {parts->position, name};
            }
        }
        for (const auto& [text, newestCheckpoint] : unlisted) {
            const auto decoded = decodeCheckpoint(readWhole(checkpoints, newestCheckpoint.second));
            const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
            if (checkpoint != nullptr && checkpoint->header.shardCount == shardCount && checkpoint->header.shardIndex < shardCount) {
                instruments.push_back(Instrument{checkpoint->header.symbol, checkpoint->header.shardIndex, std::nullopt, std::nullopt, std::nullopt});
            }
            // Otherwise the survey refuses it as a checkpoint off the list, as a start would.
        }
        return InstrumentDirectory(std::move(instruments), shardCount);
    }

    StoreVerification verifyStore(LogStorage& logs, LogStorage& checkpoints, bool perCommand, const BookCheck& check) {
        StoreVerification verification;
        try {
            const auto locks = lockStores(logs, checkpoints);
            const InstrumentDirectory directory = directoryOfStore(logs, checkpoints);
            const LogSurvey survey = surveyLog(logs, checkpoints, directory);
            if (survey.placement && (!survey.placement->rewraps.empty() || !survey.placement->dropped.empty())) {
                // A start would change placement first, which writes; verify can't follow it.
                throw RecoveryError(RecoveryError::Kind::START_REFUSED, "a start would change placement first (" + survey.placement->what +
                                                                            "), which verify can't follow without writing");
            }
            RecoveryOptions options;
            options.readOnly = true;
            options.perCommand = perCommand;
            options.check = check;
            for (std::uint32_t number = 0; number < directory.shardCount(); ++number) {
                Shard shard(directory, number);
                const RecoveredShard recovered = recoverShard(logs, checkpoints, directory, survey.shards[number], survey.checkpoints[number], shard,
                                                              defaultBookCapacity, neverStopping, options);
                VerifiedShard verified;
                verified.shard = number;
                verified.start = recovered.start;
                verified.end = recovered.replayed;
                verified.files = survey.shards[number].size();
                verified.tailCutBytes = recovered.tailCutBytes;
                verified.filesDropped = recovered.filesRemoved;
                for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
                    const std::vector<std::uint64_t>& positions = survey.checkpoints[number][slot];
                    verified.books.push_back(VerifiedBook{shard.books().symbol(slot), recovered.restoredFrom[slot],
                                                          !positions.empty() && recovered.restoredFrom[slot] != positions.front(), positions.empty()});
                }
                verification.shards.push_back(std::move(verified));
            }
        } catch (const RecoveryError& error) {
            verification.outcome = error.kind() == RecoveryError::Kind::BOOK_BROKEN ? VerifyOutcome::BROKEN : VerifyOutcome::STOPPED;
            verification.failure = error.what();
        } catch (const LogStorageError& error) {
            verification.outcome = VerifyOutcome::STOPPED;
            verification.failure = storageMessage(error);
        } catch (const std::exception& error) {
            // What no start would get past either, such as a header with no placement.
            verification.outcome = VerifyOutcome::STOPPED;
            verification.failure = error.what();
        }
        return verification;
    }

    std::string formatVerifiedShard(const VerifiedShard& shard) {
        std::string text = "shard " + std::to_string(shard.shard) + ": start=" + std::to_string(shard.start) + " end=" +
                           std::to_string(shard.end) + " files=" + std::to_string(shard.files) + " tail-cut-bytes=" +
                           std::to_string(shard.tailCutBytes) + " files-dropped=" + std::to_string(shard.filesDropped) + "\n";
        for (const VerifiedBook& book : shard.books) {
            text += "book " + book.symbol.text() + " shard=" + std::to_string(shard.shard) + " checkpoint=" + std::to_string(book.checkpoint) +
                    (book.isNew ? " new" : book.fellBack ? " fell-back" : "") + "\n";
        }
        return text;
    }

    StoreRoll rollStore(LogStorage& logs, LogStorage& checkpoints, const BookCheck& check) {
        StoreRoll result;
        try {
            const auto locks = lockStores(logs, checkpoints);
            const InstrumentDirectory directory = directoryOfStore(logs, checkpoints);
            static_cast<void>(surveyLog(logs, checkpoints, directory));  // a start's own checks first
            std::uint64_t writesFailed = 0;
            std::size_t stopped = 0;
            for (std::uint32_t number = 0; number < directory.shardCount(); ++number) {
                ShardRoller roller(logs, checkpoints, directory, number, [&result](const std::string& line) { result.reported.push_back(line); });
                roller.throwOnFailedProof(check);
                const std::vector<LogFileEntry> files = shardFiles(logs, number);
                if (files.size() < 2) {
                    continue;  // no file closed: the last one is closed only once a later one starts
                }
                result.files += roller.roll(files.back().firstPosition);
                result.books += roller.counts().published;
                writesFailed += roller.counts().writesFailed + roller.counts().passesLost;
                stopped += roller.lastRollStopped() ? 1 : 0;
            }
            if (stopped != 0) {
                result.outcome = RollOutcome::FAILED;
                result.failure = "the roll stopped on what it couldn't read, as reported; verify says what a start would make of it";
            } else if (writesFailed != 0) {
                result.outcome = RollOutcome::FAILED;
                result.failure = std::to_string(writesFailed) + (writesFailed == 1 ? " checkpoint was" : " checkpoints were") +
                                 " not published, which the next roll makes again";
            }
        } catch (const ProofFailed& failure) {
            result.outcome = RollOutcome::PROOF_FAILED;
            result.failure = failure.what();
        } catch (const RecoveryError& error) {
            result.outcome = RollOutcome::FAILED;
            result.failure = error.what();
        } catch (const LogStorageError& error) {
            result.outcome = RollOutcome::FAILED;
            result.failure = storageMessage(error);
        } catch (const std::exception& error) {
            result.outcome = RollOutcome::FAILED;
            result.failure = error.what();
        }
        return result;
    }

    std::string cutFileName(std::uint32_t shard, std::uint64_t position) {
        char text[64];
        std::snprintf(text, sizeof(text), "cut-%04u-%020llu.oblog", static_cast<unsigned>(shard), static_cast<unsigned long long>(position));
        return text;
    }

    LogCut cutLog(LogStorage& logs, LogStorage& checkpoints, std::uint32_t shard, std::uint64_t position) {
        LogCut result;
        try {
            const auto locks = lockStores(logs, checkpoints);
            const std::vector<LogFileEntry> files = shardFiles(logs, shard);
            std::size_t first = files.size();
            for (std::size_t i = 0; i < files.size(); ++i) {
                if (files[i].firstPosition <= position) {
                    first = i;
                }
            }
            if (first == files.size()) {
                result.failure = "shard " + std::to_string(shard) + " has no log file holding position " + std::to_string(position);
                return result;
            }
            // No checkpoint may hold the records being cut.
            for (const std::string& name : checkpoints.names()) {
                const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
                if (!parts || parts->temporary || parts->position <= position) {
                    continue;
                }
                const auto decoded = decodeCheckpoint(readWhole(checkpoints, name));
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                if (checkpoint == nullptr || checkpoint->header.shardIndex == shard) {
                    result.failure = name + " stands past position " + std::to_string(position) +
                                     ", so it holds what would be cut, which a start would then refuse as a checkpoint past the log's end";
                    return result;
                }
            }

            // The moved records' bytes, file by file, with each one's checksum sealed anew in the
            // cut file's chain as far as the old chain held.
            std::uint64_t headerBytes = 0;
            replay::LogHeader cutHeader = headerOf(logs, files[first].name, headerBytes);
            // A start gives every book a checkpoint, so a store holding none of this shard's is
            // the wrong one, and the check above saw nothing.
            const std::vector<std::string> checkpointNames = checkpoints.names();
            const bool anyHeld = std::any_of(checkpointNames.begin(), checkpointNames.end(), [&cutHeader](const std::string& name) {
                const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
                return parts && std::find(cutHeader.symbols.begin(), cutHeader.symbols.end(), parts->symbol) != cutHeader.symbols.end();
            });
            if (!anyHeld) {
                result.failure = "no checkpoint of shard " + std::to_string(shard) +
                                 "'s books is in the checkpoint directory; if they are in another, name it with --checkpoint-dir";
                return result;
            }
            cutHeader.firstPosition = position;
            std::vector<std::byte> cutBytes = replay::encodeLogHeader(cutHeader);
            std::uint32_t cutChain = replay::logChainSeed(cutBytes);
            bool sealing = true;
            std::uint64_t moved = 0;
            std::vector<std::vector<std::byte>> payloads;  // each moved record past its checksum, to compare a rerun's
            std::vector<std::uint64_t> positions;          // and where it stands
            std::uint64_t logEnd = files[first].firstPosition;  // past the last record room seen
            for (std::size_t i = first; i < files.size(); ++i) {
                std::uint64_t fileHeaderBytes = 0;
                const replay::LogHeader header = headerOf(logs, files[i].name, fileHeaderBytes);
                std::vector<std::byte> bytes = readWhole(logs, files[i].name);
                // Every record's room up to the last byte written, a hole, a zeroed record, and a
                // torn last one included, padded to a whole record; past it is an extended file's
                // zero tail, which holds nothing.
                std::size_t written = bytes.size();
                while (written > fileHeaderBytes && bytes[written - 1] == std::byte{0}) {
                    --written;
                }
                const std::size_t slots = written <= fileHeaderBytes ? 0 : static_cast<std::size_t>((written - fileHeaderBytes + header.recordBytes - 1) / header.recordBytes);
                bytes.resize(std::max(bytes.size(), static_cast<std::size_t>(fileHeaderBytes + slots * header.recordBytes)));
                std::uint32_t chain = header.chainSeed;
                for (std::uint64_t at = fileHeaderBytes, index = 0; index < slots; at += header.recordBytes, ++index) {
                    const std::span<const std::byte> record(bytes.data() + at, header.recordBytes);
                    const bool chained = sealing && log_reading::load32(record, 0) == chainChecksum(record, chain);
                    const std::uint64_t recordPosition = files[i].firstPosition + index;
                    logEnd = std::max(logEnd, recordPosition + 1);
                    if (chained) {
                        chain = log_reading::load32(record, 0);
                    }
                    if (recordPosition < position) {
                        if (!chained) {
                            result.failure = files[i].name + " doesn't chain at position " + std::to_string(recordPosition) +
                                             ", before the cut: cut there, or before it";
                            return result;
                        }
                        continue;
                    }
                    std::vector<std::byte> copy(record.begin(), record.end());
                    if (chained) {
                        store32(copy, chainChecksum(copy, cutChain));
                        cutChain = log_reading::load32(copy, 0);
                    } else {
                        sealing = false;  // what doesn't chain stays as it is, and all after it
                    }
                    payloads.emplace_back(copy.begin() + 4, copy.end());
                    positions.push_back(recordPosition);
                    cutBytes.insert(cutBytes.end(), copy.begin(), copy.end());
                    ++moved;
                }
            }
            if (moved == 0) {
                // Cut already, by a run whose report was lost, if its cut file stands and the log
                // ends at the position.
                const std::string done = cutFileName(shard, position);
                const std::vector<std::string> names = logs.names();
                if (std::find(names.begin(), names.end(), done) != names.end() && logEnd == position) {
                    const std::vector<std::byte> earlier = readWhole(logs, done);
                    result.cut = true;
                    result.file = done;
                    result.records = (earlier.size() - replay::encodeLogHeader(cutHeader).size()) / cutHeader.recordBytes;
                    return result;
                }
                result.failure = "shard " + std::to_string(shard) + " has no record at position " + std::to_string(position) + " to cut";
                return result;
            }

            // The cut file first, whole and flushed, so nothing moved is lost however this ends;
            // one an interrupted run left stands if the log holds a prefix of its records.
            const std::string name = cutFileName(shard, position);
            bool exists = false;
            for (const std::string& held : logs.names()) {
                exists = exists || held == name;
            }
            if (exists) {
                // An interrupted run's: every record still in the log from the position on must be
                // the one it holds there, whichever of its removals a crash undid.
                const std::vector<std::byte> earlier = readWhole(logs, name);
                const std::size_t earlierHeader = replay::encodeLogHeader(cutHeader).size();
                bool held = true;
                for (std::size_t r = 0; held && r < payloads.size(); ++r) {
                    const std::size_t at = earlierHeader + static_cast<std::size_t>(positions[r] - position) * cutHeader.recordBytes;
                    held = at + cutHeader.recordBytes <= earlier.size() &&
                           std::equal(payloads[r].begin(), payloads[r].end(), earlier.begin() + static_cast<std::ptrdiff_t>(at + 4));
                }
                if (!held) {
                    result.failure = name + " already exists and holds other records";
                    return result;
                }
                result.records = (earlier.size() - earlierHeader) / cutHeader.recordBytes;
            } else {
                for (std::size_t r = 0; r < positions.size(); ++r) {
                    if (positions[r] != position + r) {
                        result.failure = "shard " + std::to_string(shard) + "'s log has a gap at position " + std::to_string(position + r) +
                                         ", among the records to cut";
                        return result;
                    }
                }
                result.records = moved;
                const std::string temporary = name + ".tmp";
                for (const std::string& held : logs.names()) {
                    if (held == temporary) {
                        logs.remove(temporary);  // an interrupted run's, holding nothing anyone needs
                    }
                }
                std::unique_ptr<LogFile> file = logs.create(temporary, Durability::MACHINE);
                file->write(0, cutBytes);
                file->flush();
                file->rename(name);
                file->close();
            }

            // Then the log: the files after the record's, newest first, and its own cut at it,
            // to its header alone if the record is its first, which a start then removes.
            for (std::size_t i = files.size(); i-- > first + 1;) {
                logs.remove(files[i].name);
            }
            {
                const std::unique_ptr<LogFile> file = logs.open(files[first].name);
                file->cut(headerBytes + (position - files[first].firstPosition) * cutHeader.recordBytes);
                file->flush();
                file->close();
            }
            result.cut = true;
            result.file = name;
        } catch (const RecoveryError& error) {
            result.failure = error.what();
        } catch (const LogStorageError& error) {
            result.failure = storageMessage(error);
        } catch (const std::exception& error) {
            result.failure = error.what();
        }
        return result;
    }

}  // namespace order_book::runtime
