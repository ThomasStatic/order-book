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

    // Each directory's log files, sorted, and every other path as it is, in argument order.
    std::vector<std::filesystem::path> expandInputs(const std::vector<std::filesystem::path>& paths);

    ReplayInputs readReplayInputs(const std::vector<std::filesystem::path>& paths);

    // Records from several files, with each record's lineNumber replaced by an index into tags,
    // which hold "<file name>:<line>", so a line that repeats across files stays told apart.
    struct MergedRecords {
        std::vector<ReplayRecord> records;
        std::vector<std::string> tags;
    };
    MergedRecords mergeRecords(const std::vector<const LoadedInput*>& files);

    struct ConvertOutcome {
        std::size_t records = 0;
        std::vector<std::filesystem::path> outputs;
        // A usage error: --symbol missing on a v1 or v2 input, or given on one that names symbols.
        std::optional<std::string> usage;
        // Refused before reading: an existing output, or logs that can't go together.
        std::optional<std::string> refusal;
        // Bad input part-way: what came before it is still written.
        std::optional<ReplayError> error;
        std::filesystem::path errorPath;
    };

    // Direction comes from each input's first bytes. One input gives one file, keeping its
    // interleaving: a CSV becomes a log with no placement at position 0, and a log a v3 CSV.
    // Several inputs or a directory make out a directory of commands-<symbol>.csv, each holding
    // one instrument, numbered from 1. An existing output is refused, never overwritten.
    ConvertOutcome convertReplayFiles(const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& out,
                                      std::optional<Symbol> symbol);

}  // namespace order_book::replay
