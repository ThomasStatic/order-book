#include "order_book/replay_inputs.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "order_book/command_log.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/replay_summary.hpp"

namespace order_book::replay {
    namespace {
        namespace fs = std::filesystem;

        std::optional<LogHeader> readLogHeader(const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            CommandLogReader reader(file);
            reader.next();
            if (const LogHeader* header = reader.header()) {
                return *header;
            }
            return std::nullopt;
        }

        struct EngineLog {
            fs::path path;
            std::uint32_t shard;
            std::uint64_t firstPosition;
        };

        std::string pathText(const fs::path& path) {
            return path.generic_string();
        }

        // Cut records name a placement but no longer belong to a shard's history.
        bool isCutRecords(const fs::path& path) {
            return path.filename().string().starts_with("cut-");
        }

        bool isBeingMade(const fs::path& path) {
            return path.extension() == ".tmp";
        }

        // Logs that name a placement, in the order they must run, and every other input after.
        std::vector<fs::path> runOrder(const std::vector<fs::path>& paths, std::vector<EngineLog>& engineLogs) {
            std::vector<fs::path> others;
            for (const fs::path& path : paths) {
                std::optional<LogHeader> header;
                if (isCommandLogFile(path) && !isCutRecords(path)) {
                    header = readLogHeader(path);
                }
                if (header && header->shardCount > 0) {
                    engineLogs.push_back(EngineLog{path, header->shardIndex, header->firstPosition});
                } else {
                    others.push_back(path);
                }
            }
            std::sort(engineLogs.begin(), engineLogs.end(), [](const EngineLog& left, const EngineLog& right) {
                return left.shard != right.shard ? left.shard < right.shard : left.firstPosition < right.firstPosition;
            });
            std::vector<fs::path> ordered;
            for (const EngineLog& log : engineLogs) {
                ordered.push_back(log.path);
            }
            ordered.insert(ordered.end(), others.begin(), others.end());
            return ordered;
        }

        bool startsAShard(const std::vector<EngineLog>& logs, std::size_t index) {
            return index == 0 || logs[index - 1].shard != logs[index].shard;
        }

        // Every shard's first log must stand at position 0, which its header alone can say.
        std::optional<std::string> checkShardStarts(const std::vector<EngineLog>& logs) {
            for (std::size_t i = 0; i < logs.size(); ++i) {
                if (startsAShard(logs, i) && logs[i].firstPosition != 0) {
                    return "shard " + std::to_string(logs[i].shard) + "'s logs start at position " +
                           std::to_string(logs[i].firstPosition) + ", not 0: " + pathText(logs[i].path);
                }
            }
            return std::nullopt;
        }

        // Each log's successor must follow it, and a symbol must stay on one shard. files holds
        // the logs first, in the same order, and may end early at an error.
        std::optional<std::string> checkEngineLogs(const std::vector<EngineLog>& logs, const std::vector<LoadedInput>& files) {
            std::map<Symbol, std::uint32_t> shardOf;
            for (std::size_t i = 0; i < logs.size() && i < files.size(); ++i) {
                if (!startsAShard(logs, i)) {
                    const std::uint64_t expected = logs[i - 1].firstPosition + files[i - 1].input.records.size();
                    if (logs[i].firstPosition != expected) {
                        return "a gap in shard " + std::to_string(logs[i].shard) + "'s logs: position " +
                               std::to_string(expected) + " is missing before " + pathText(logs[i].path);
                    }
                }
                for (const ReplayRecord& record : files[i].input.records) {
                    const auto [held, added] = shardOf.emplace(record.symbol, logs[i].shard);
                    if (!added && held->second != logs[i].shard) {
                        return "symbol " + record.symbol.text() + " is in the logs of shards " + std::to_string(held->second) +
                               " and " + std::to_string(logs[i].shard);
                    }
                }
            }
            return std::nullopt;
        }

        bool namesSymbols(const ReplayInput& input) {
            return input.format == ReplayFormat::V3;
        }

        // Closes with an error if the file could not be written.
        void writeBytes(const fs::path& path, const char* bytes, std::size_t size) {
            std::ofstream file(path, std::ios::binary);
            file.write(bytes, static_cast<std::streamsize>(size));
            file.close();
            if (!file) {
                throw std::runtime_error("could not write " + pathText(path));
            }
        }

        void writeLog(const fs::path& path, const std::vector<ReplayRecord>& records, Symbol standIn) {
            std::vector<std::byte> bytes = encodeLogHeader(LogHeader{});
            std::uint32_t checksum = logChainSeed(bytes);
            bytes.reserve(bytes.size() + records.size() * logRecordBytes);
            for (const ReplayRecord& record : records) {
                std::array<std::byte, logRecordBytes> encoded;
                checksum = encodeLogRecord(record.symbol.empty() ? standIn : record.symbol, record.command, checksum, encoded);
                bytes.insert(bytes.end(), encoded.begin(), encoded.end());
            }
            writeBytes(path, reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        void writeCsv(const fs::path& path, const std::vector<ReplayRecord>& records, bool renumber) {
            std::string text(expectedHeaderV3);
            text += '\n';
            std::uint64_t sequence = 0;
            for (ReplayRecord record : records) {
                if (renumber) {
                    record.sequence = ++sequence;
                }
                text += formatReplayRow(record);
                text += '\n';
            }
            writeBytes(path, text.data(), text.size());
        }

        void noteError(ConvertOutcome& outcome, const LoadedInput& file) {
            outcome.error = file.input.error;
            outcome.errorPath = file.path;
        }
    }  // namespace

    std::vector<fs::path> expandInputs(const std::vector<fs::path>& paths) {
        std::vector<fs::path> expanded;
        for (const fs::path& path : paths) {
            std::error_code ignored;
            if (!fs::is_directory(path, ignored)) {
                expanded.push_back(path);
                continue;
            }
            std::vector<fs::path> logs;
            for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
                if (entry.is_regular_file() && !isBeingMade(entry.path()) && !isCutRecords(entry.path()) &&
                    isCommandLogFile(entry.path())) {
                    logs.push_back(entry.path());
                }
            }
            std::sort(logs.begin(), logs.end());
            expanded.insert(expanded.end(), logs.begin(), logs.end());
        }
        return expanded;
    }

    ReplayInputs readReplayInputs(const std::vector<fs::path>& paths) {
        ReplayInputs inputs;
        std::vector<EngineLog> engineLogs;
        const std::vector<fs::path> ordered = runOrder(expandInputs(paths), engineLogs);
        inputs.refusal = checkShardStarts(engineLogs);
        if (inputs.refusal) {
            return inputs;
        }
        for (const fs::path& path : ordered) {
            inputs.files.push_back(LoadedInput{path, readReplayOrLogFile(path)});
            if (inputs.files.back().input.error) {
                break;
            }
        }
        inputs.refusal = checkEngineLogs(engineLogs, inputs.files);
        return inputs;
    }

    MergedRecords mergeRecords(const std::vector<const LoadedInput*>& files) {
        MergedRecords merged;
        for (const LoadedInput* file : files) {
            const std::string name = file->path.filename().string();
            for (ReplayRecord record : file->input.records) {
                merged.tags.push_back(name + ':' + std::to_string(record.lineNumber));
                merged.origins.push_back(RecordOrigin{file, record.lineNumber});
                record.lineNumber = merged.records.size();
                merged.records.push_back(std::move(record));
            }
        }
        return merged;
    }

    std::string formatMergedError(const ReplayError& error, const MergedRecords& merged) {
        if (!error.lineNumber || *error.lineNumber >= merged.origins.size()) {
            return formatReplayError(error, "several inputs");
        }
        const RecordOrigin& origin = merged.origins[*error.lineNumber];
        ReplayError located = error;
        located.lineNumber = origin.line;
        return formatReplayError(located, pathText(origin.file->path));
    }

    std::string symbolOnNamedInput(const fs::path& path) {
        return "--symbol is for a v1 or v2 file; '" + pathText(path) + "' names its own symbols";
    }

    ConvertOutcome convertReplayFiles(const std::vector<fs::path>& inputs, const fs::path& out, std::optional<Symbol> symbol) {
        ConvertOutcome outcome;
        std::error_code ignored;
        if (fs::exists(out, ignored)) {
            outcome.refusal = "'" + pathText(out) + "' already exists";
            return outcome;
        }
        const std::vector<fs::path> expanded = expandInputs(inputs);
        const bool several = expanded.size() != 1 || (inputs.size() == 1 && fs::is_directory(inputs[0], ignored));

        ReplayInputs read;
        if (several) {
            read = readReplayInputs(inputs);
            if (read.refusal) {
                outcome.refusal = read.refusal;
                return outcome;
            }
        } else {
            read.files.push_back(LoadedInput{expanded[0], readReplayOrLogFile(expanded[0])});
        }
        for (const LoadedInput& file : read.files) {
            // A file that failed at its header has no format to judge it by.
            if (file.input.records.empty() && file.input.error) {
                continue;
            }
            if (namesSymbols(file.input) && symbol) {
                outcome.usage = symbolOnNamedInput(file.path);
                return outcome;
            }
            if (!namesSymbols(file.input) && !symbol) {
                outcome.usage = "'" + pathText(file.path) + "' names no symbol, so it needs --symbol";
                return outcome;
            }
        }

        std::vector<ReplayRecord> records;
        for (const LoadedInput& file : read.files) {
            records.insert(records.end(), file.input.records.begin(), file.input.records.end());
            if (file.input.error) {
                noteError(outcome, file);
            }
        }
        outcome.records = records.size();
        if (records.empty() && outcome.error) {
            return outcome;
        }
        const Symbol standIn = symbol.value_or(Symbol{});
        if (!several) {
            if (isCommandLogFile(read.files[0].path)) {
                writeCsv(out, records, false);
            } else {
                writeLog(out, records, standIn);
            }
            outcome.outputs.push_back(out);
            return outcome;
        }
        fs::create_directories(out);
        for (InstrumentRecords& instrument : groupBySymbol(records, standIn)) {
            for (ReplayRecord& record : instrument.records) {
                record.symbol = instrument.symbol;
            }
            const fs::path path = out / ("commands-" + instrument.symbol.text() + ".csv");
            writeCsv(path, instrument.records, true);
            outcome.outputs.push_back(path);
        }
        return outcome;
    }

}  // namespace order_book::replay
