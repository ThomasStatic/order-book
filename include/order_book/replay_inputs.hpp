#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "replay_engine.hpp"
#include "symbol.hpp"

// Reading several replay inputs, and converting between logs and CSVs. A directory means every
// log file in it. Logs an engine wrote run in shard then position order, whatever the argument
// order, and every other input stays in argument order. Nothing here touches a book.
namespace order_book::replay {

    struct LoadedInput {
        std::filesystem::path path;
        ReplayInput input;
    };

    struct ReplayInputs {
        std::vector<LoadedInput> files;  // in run order; reading stops at the first with an error
        // Why these logs can't be replayed together: a shard's logs that don't start at position
        // 0, a gap between files, or a symbol in two shards' logs. Nothing should run.
        std::optional<std::string> refusal;
    };

    // Each directory's log files, sorted, and every other path as it is, in argument order. A
    // directory leaves out a file still being made (.tmp) and cut records (cut-), which stay
    // out of a replay unless named directly.
    std::vector<std::filesystem::path> expandInputs(const std::vector<std::filesystem::path>& paths);

    ReplayInputs readReplayInputs(const std::vector<std::filesystem::path>& paths);

    struct RecordOrigin {
        const LoadedInput* file;
        std::size_t line;
    };

    // Records from several files, with each record's lineNumber replaced by an index into tags,
    // which hold "<file name>:<line>", so a line that repeats across files stays told apart.
    // The files must outlive this, which points into them.
    struct MergedRecords {
        std::vector<ReplayRecord> records;
        std::vector<std::string> tags;
        std::vector<RecordOrigin> origins;  // by the same index
    };
    MergedRecords mergeRecords(const std::vector<const LoadedInput*>& files);

    // The diagnostic for an error from running merged records: it names the record's own file
    // and line, not its place in the merge. An error with no line names no file.
    std::string formatMergedError(const ReplayError& error, const MergedRecords& merged);

    // The usage error for --symbol on an input that names its own symbols.
    std::string symbolOnNamedInput(const std::filesystem::path& path);

    struct ConvertOutcome {
        std::size_t records = 0;
        std::vector<std::filesystem::path> outputs;
        // A usage error: --symbol missing on a v1 or v2 input, or given on one that names symbols.
        std::optional<std::string> usage;
        // Refused before reading: an existing output, or several logs that can't go together.
        std::optional<std::string> refusal;
        // Bad input part-way: what came before it is still written, and with nothing before
        // it no file is, so a retry isn't refused for an output it never meant to make.
        std::optional<ReplayError> error;
        std::filesystem::path errorPath;
    };

    // Direction comes from each input's first bytes. One input gives one file, keeping its
    // interleaving: a CSV becomes a log with no placement at position 0, and a log a v3 CSV,
    // whatever position or placement it had. Several inputs or a directory make out a directory
    // of commands-<symbol>.csv, each holding one instrument, numbered from 1, and are refused
    // as replay refuses them, since a gap would quietly lose commands. An existing output is
    // refused, never overwritten.
    ConvertOutcome convertReplayFiles(const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& out,
                                      std::optional<Symbol> symbol);

}  // namespace order_book::replay
