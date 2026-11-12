#include "order_book/cli/commands.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_inputs.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/runtime/offline.hpp"
#include "order_book/symbol.hpp"

namespace order_book::cli {

    namespace {

        constexpr std::string_view usageLines =
            "usage: order_book_cli replay <path>... [--trades] [--events] [--book] [--validate none|final|per-command]\n"
            "                                       [--symbol SYM] [--no-statistics]\n"
            "       order_book_cli convert <path>... --out <path> [--symbol SYM]\n"
            "       order_book_cli send <file> --connect ADDR:PORT [--symbol SYM]\n"
            "       order_book_cli checkpoint describe|verify <file>\n"
            "       order_book_cli checkpoint roll --log-dir DIR [--checkpoint-dir DIR]\n"
            "       order_book_cli verify --log-dir DIR [--checkpoint-dir DIR] [--validate final|per-command]\n"
            "       order_book_cli cut-log --log-dir DIR --shard S --position P [--checkpoint-dir DIR]\n"
            "       order_book_cli demo\n"
            "       order_book_cli --help\n";

        // What a command writes as it goes, read back as a CommandResult at the end.
        struct Printed {
            std::ostringstream out;
            std::ostringstream err;

            CommandResult finish(int exit) const { return CommandResult{out.str(), err.str(), exit}; }
        };

        // One engine log still has to stand at position 0, or its books would start wrong. Prints
        // why not, if it doesn't.
        bool refusedLog(Printed& printed, const std::string& path) {
            if (!replay::isCommandLogFile(path)) {
                return false;
            }
            const replay::ReplayInputs checked = replay::readReplayInputs({std::filesystem::path(path)});
            if (checked.refusal) {
                printed.err << "order_book_cli: " << *checked.refusal << '\n';
            }
            return checked.refusal.has_value();
        }

        struct ReplaySettings {
            std::vector<std::string> paths;
            std::string path;
            bool printTrades = false;
            bool printEvents = false;  // every event, which covers printTrades
            bool printBook = false;
            bool printStatistics = true;
            replay::InvariantValidation validation = replay::InvariantValidation::FINAL;
            std::optional<replay::Symbol> symbol;
        };

        replay::ReplayOutput outputOf(const ReplaySettings& settings) {
            return replay::ReplayOutput{settings.printTrades, settings.printEvents, settings.printBook, settings.printStatistics};
        }

        using ErrorText = std::function<std::string(const replay::ReplayError&)>;

        // A book that failed, then, for a failed final check, where replaying it alone found it
        // broken. The result was lost with the failure, so nothing else prints.
        void printBookFailure(Printed& printed, std::string_view prefix, const replay::ReplayError& error,
                              const std::optional<replay::ReplayError>& located, const ErrorText& text) {
            printed.err << prefix << text(error) << '\n';
            if (replay::isFailedFinalCheck(error)) {
                printed.err << prefix << "replaying with per-command checks to find the first failing command\n";
                if (located) {
                    printed.err << prefix << text(*located) << '\n';
                } else {
                    printed.err << prefix << "the per-command replay found no failure\n";
                }
            }
        }

        // Each book's run, then every section, then any diagnostic, as a single book does below.
        // A book that fails prints only its diagnostic, prefixed with its symbol.
        CommandResult runInstruments(Printed& printed, const ReplaySettings& settings, const replay::ReplayInput& input) {
            replay::ReplayOptions options;
            options.invariantValidation = settings.validation;
            const replay::InstrumentsOutcome outcome = replay::replayInstruments(
                replay::groupBySymbol(input.records, settings.symbol.value_or(replay::Symbol{})), options);

            if (const auto* failure = std::get_if<replay::InstrumentFailure>(&outcome)) {
                printBookFailure(printed, "symbol " + failure->symbol.text() + ": ", failure->error, failure->located,
                                 [&settings](const replay::ReplayError& error) { return replay::formatReplayError(error, settings.path); });
                return printed.finish(engineFailure);
            }

            printed.out << replay::formatInstrumentSections(std::get<std::vector<replay::InstrumentReplay>>(outcome), outputOf(settings));
            if (input.error) {
                printed.err << replay::formatReplayError(*input.error, settings.path) << '\n';
                return printed.finish(inputError);
            }
            return printed.finish(0);
        }

        // Several inputs. A v1 or v2 file runs alone, unless --symbol makes it one instrument;
        // files that name symbols run together, in one set of sections, after them. Lines repeat
        // across files, so an event names its file too. Reading stopped at the first error, so
        // what came before it still runs and prints, and then the diagnostic.
        CommandResult runSeveral(Printed& printed, const ReplaySettings& settings) {
            const std::vector<std::filesystem::path> paths(settings.paths.begin(), settings.paths.end());
            const replay::ReplayInputs inputs = replay::readReplayInputs(paths);
            if (inputs.refusal) {
                printed.err << "order_book_cli: " << *inputs.refusal << '\n';
                return printed.finish(inputError);
            }

            std::vector<const replay::LoadedInput*> named;
            std::vector<const replay::LoadedInput*> alone;
            for (const replay::LoadedInput& file : inputs.files) {
                const bool namesSymbols = file.input.format == replay::ReplayFormat::V3;
                if (namesSymbols && settings.symbol) {
                    return usage(replay::symbolOnNamedInput(file.path));
                }
                (namesSymbols || settings.symbol ? named : alone).push_back(&file);
            }

            replay::ReplayOptions options;
            options.invariantValidation = settings.validation;
            bool printedAny = false;
            for (const replay::LoadedInput* file : alone) {
                const replay::MergedRecords merged = replay::mergeRecords({file});
                OrderBook book;
                const replay::ReplayOutcome outcome = replay::ReplayEngine::run(merged.records, book, options);
                if (const replay::ReplayError* failure = std::get_if<replay::ReplayError>(&outcome)) {
                    std::optional<replay::ReplayError> located;
                    if (replay::isFailedFinalCheck(*failure)) {
                        located = replay::locateFailedFinalCheck(merged.records, options);
                    }
                    printBookFailure(printed, "", *failure, located,
                                     [&merged](const replay::ReplayError& error) { return replay::formatMergedError(error, merged); });
                    return printed.finish(engineFailure);
                }
                replay::ReplayOutput output = outputOf(settings);
                output.lineTags = &merged.tags;
                printed.out << (printedAny ? "\n" : "") << replay::formatReplayOutput(std::get<replay::ReplayResult>(outcome), output);
                printedAny = true;
            }

            const replay::MergedRecords merged = replay::mergeRecords(named);
            const replay::InstrumentsOutcome outcome =
                replay::replayInstruments(replay::groupBySymbol(merged.records, settings.symbol.value_or(replay::Symbol{})), options);
            if (const auto* failure = std::get_if<replay::InstrumentFailure>(&outcome)) {
                printBookFailure(printed, "symbol " + failure->symbol.text() + ": ", failure->error, failure->located,
                                 [&merged](const replay::ReplayError& error) { return replay::formatMergedError(error, merged); });
                return printed.finish(engineFailure);
            }
            replay::ReplayOutput output = outputOf(settings);
            output.lineTags = &merged.tags;
            const std::string sections =
                replay::formatInstrumentSections(std::get<std::vector<replay::InstrumentReplay>>(outcome), output);
            printed.out << (printedAny && !sections.empty() ? "\n" : "") << sections;

            if (!inputs.files.empty() && inputs.files.back().input.error) {
                const replay::LoadedInput& last = inputs.files.back();
                printed.err << replay::formatReplayError(*last.input.error, last.path.generic_string()) << '\n';
                return printed.finish(inputError);
            }
            return printed.finish(0);
        }

        // Replays settings.path on an empty book. On stdout: the trades or events asked for, a
        // blank line if any were printed, the statistics, then the final book if asked for.
        // Input that stops part-way still prints what ran, then the diagnostic on stderr. An
        // engine failure prints only its diagnostic, since the result was lost with it; a failed
        // final check is replayed again on a fresh book, checking after every command, to find
        // the first line after which the book is broken.
        CommandResult runReplay(Printed& printed, const ReplaySettings& settings) {
            const std::string& path = settings.path;
            if (refusedLog(printed, path)) {
                return printed.finish(inputError);
            }
            const replay::ReplayInput input = replay::readReplayOrLogFile(path);
            if (input.format == replay::ReplayFormat::V3) {
                if (settings.symbol) {
                    return usage(replay::symbolOnNamedInput(path));
                }
                return runInstruments(printed, settings, input);
            }
            if (settings.symbol) {
                return runInstruments(printed, settings, input);
            }

            replay::ReplayOptions options;
            options.invariantValidation = settings.validation;
            OrderBook book;
            const replay::ReplayOutcome outcome = replay::ReplayEngine::run(input.records, book, options);

            if (const replay::ReplayError* failure = std::get_if<replay::ReplayError>(&outcome)) {
                std::optional<replay::ReplayError> located;
                if (replay::isFailedFinalCheck(*failure)) {
                    located = replay::locateFailedFinalCheck(input.records, options);
                }
                printBookFailure(printed, "", *failure, located,
                                 [&path](const replay::ReplayError& error) { return replay::formatReplayError(error, path); });
                return printed.finish(engineFailure);
            }

            printed.out << replay::formatReplayOutput(std::get<replay::ReplayResult>(outcome), outputOf(settings));
            if (input.error) {
                printed.err << replay::formatReplayError(*input.error, path) << '\n';
                return printed.finish(inputError);
            }
            return printed.finish(0);
        }

    }  // namespace

    std::string_view usageText() { return usageLines; }

    CommandResult usage(std::string_view problem) {
        return CommandResult{{}, "order_book_cli: " + std::string(problem) + '\n' + std::string(usageLines), usageError};
    }

    std::string repeated(std::string_view option) {
        return "'" + std::string(option) + "' was given twice";
    }

    std::string notASymbol(std::string_view text) {
        return "'" + std::string(text) + "' is not a symbol (1 to 8 of A-Z, 0-9, '.', '-')";
    }

    // Options may come before or after the file; after "--", the next argument is the file
    // whatever it looks like.
    CommandResult replay(std::span<const std::string_view> args) {
        ReplaySettings settings;
        bool optionsEnded = false;
        std::set<std::string_view> given;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
            } else if (!optionsEnded && arg.starts_with("--") && !given.insert(arg).second) {
                return usage(repeated(arg));
            } else if (!optionsEnded && arg == "--trades") {
                settings.printTrades = true;
            } else if (!optionsEnded && arg == "--events") {
                settings.printEvents = true;
            } else if (!optionsEnded && arg == "--book") {
                settings.printBook = true;
            } else if (!optionsEnded && arg == "--no-statistics") {
                settings.printStatistics = false;
            } else if (!optionsEnded && arg == "--symbol") {
                if (i + 1 == args.size()) {
                    return usage("--symbol needs a symbol");
                }
                const std::string_view text = args[++i];
                settings.symbol = replay::Symbol::parse(text);
                if (!settings.symbol) {
                    return usage(notASymbol(text));
                }
            } else if (!optionsEnded && arg == "--validate") {
                if (i + 1 == args.size()) {
                    return usage("--validate needs a mode: none, final, or per-command");
                }
                const std::string_view mode = args[++i];
                if (mode == "none") {
                    settings.validation = replay::InvariantValidation::NONE;
                } else if (mode == "final") {
                    settings.validation = replay::InvariantValidation::FINAL;
                } else if (mode == "per-command") {
                    settings.validation = replay::InvariantValidation::PER_COMMAND;
                } else {
                    return usage("unknown --validate mode '" + std::string(mode) + "'; expected none, final, or per-command");
                }
            } else if (!optionsEnded && arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                settings.paths.emplace_back(arg);
            }
        }
        if (settings.paths.empty()) {
            return usage("replay needs a file to read");
        }
        // One file keeps the "line N:" tag, however it was named. As typed, when it was typed.
        Printed printed;
        const std::vector<std::filesystem::path> paths(settings.paths.begin(), settings.paths.end());
        const std::vector<std::filesystem::path> expanded = replay::expandInputs(paths);
        if (expanded.size() == 1) {
            settings.path = expanded[0] == paths[0] ? settings.paths[0] : expanded[0].generic_string();
            return runReplay(printed, settings);
        }
        return runSeveral(printed, settings);
    }

    // One file, sent over one connection: its answers print as replay --events --no-statistics
    // prints the file, once the engine's stream has ended.
    std::variant<PreparedSend, CommandResult> prepareSend(std::span<const std::string_view> args) {
        std::vector<std::string_view> files;
        std::optional<runtime::ListenAddress> address;
        std::optional<replay::Symbol> symbol;
        std::set<std::string_view> given;
        bool optionsEnded = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
            } else if (!optionsEnded && arg.starts_with("--") && !given.insert(arg).second) {
                return usage(repeated(arg));
            } else if (!optionsEnded && (arg == "--connect" || arg == "--symbol")) {
                if (i + 1 == args.size()) {
                    return usage(std::string(arg) + " needs a value");
                }
                const std::string_view value = args[++i];
                if (arg == "--connect") {
                    address = runtime::parseListenAddress(value);
                    if (!address || address->port == 0) {
                        return usage("--connect needs an IPv4 address and a port other than 0, as 127.0.0.1:9000, not '" +
                                     std::string(value) + "'");
                    }
                } else {
                    symbol = replay::Symbol::parse(value);
                    if (!symbol) {
                        return usage(notASymbol(value));
                    }
                }
            } else if (!optionsEnded && arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                files.push_back(arg);
            }
        }
        if (files.empty()) {
            return usage("send needs a file to read");
        }
        if (files.size() > 1) {
            return usage("send takes one file, not " + std::to_string(files.size()));
        }
        if (!address) {
            return usage("send needs --connect");
        }

        Printed printed;
        PreparedSend send;
        send.path = std::string(files[0]);
        send.address = *address;
        if (refusedLog(printed, send.path)) {
            return printed.finish(inputError);
        }
        send.input = replay::readReplayOrLogFile(send.path);
        if (send.input.records.empty() && send.input.error) {
            printed.err << replay::formatReplayError(*send.input.error, send.path) << '\n';
            return printed.finish(inputError);
        }
        if (send.input.format == replay::ReplayFormat::V3 && symbol) {
            return usage(replay::symbolOnNamedInput(send.path));
        }
        if (send.input.format != replay::ReplayFormat::V3 && !symbol) {
            return usage("'" + send.path + "' names no symbol, so it needs --symbol");
        }

        send.commands.reserve(send.input.records.size());
        for (const replay::ReplayRecord& record : send.input.records) {
            send.commands.push_back({record.lineNumber, record.symbol.empty() ? *symbol : record.symbol, record.command});
        }
        return send;
    }

    CommandResult cannotConnect(const PreparedSend& send, std::string_view reason) {
        return CommandResult{{}, "order_book_cli: cannot connect to " + runtime::formatListenAddress(send.address) + ": " + std::string(reason) + '\n',
                             inputError};
    }

    CommandResult finishSend(const PreparedSend& send, const network::ClientStream& stream, std::string earlier) {
        Printed printed;
        printed.err << earlier;
        printed.out << stream.sections();
        for (const std::string& problem : stream.diagnostics()) {
            printed.err << "order_book_cli: " << problem << '\n';
        }
        if (send.input.error) {
            printed.err << replay::formatReplayError(*send.input.error, send.path) << '\n';
        }
        // 3 wins over 1, and both over 4: what ran unknown is worse than a file cut short.
        if (stream.end() == network::ConnectionEnd::LOST) {
            return printed.finish(connectionLost);
        }
        if (send.input.error) {
            return printed.finish(inputError);
        }
        return printed.finish(stream.end() == network::ConnectionEnd::REFUSED ? someRefused : 0);
    }

    std::variant<StoreFlags, CommandResult> parseStoreFlags(std::string_view command, std::span<const std::string_view> args,
                                                            std::span<const std::string_view> allowed) {
        StoreFlags flags;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            const bool known = arg == "--log-dir" || std::find(allowed.begin(), allowed.end(), arg) != allowed.end();
            if (!known) {
                return usage(arg.starts_with('-') ? "unknown option '" + std::string(arg) + "' for " + std::string(command)
                                                  : std::string(command) + " takes no file, only options: '" + std::string(arg) + "'");
            }
            if (i + 1 == args.size()) {
                return usage(std::string(arg) + " needs a value");
            }
            if (!flags.values.emplace(std::string(arg), std::string(args[++i])).second) {
                return usage(repeated(arg));
            }
        }
        if (!flags.values.contains("--log-dir")) {
            return usage(std::string(command) + " needs --log-dir DIR");
        }
        return flags;
    }

    std::variant<Stores, CommandResult> openStores(const StoreFlags& flags) {
        Stores stores;
        const std::string& logDir = flags.values.at("--log-dir");
        std::error_code error;
        if (!std::filesystem::is_directory(logDir, error)) {
            return CommandResult{{}, "order_book_cli: cannot use log directory " + logDir + ": no such directory\n", inputError};
        }
        stores.logs = std::make_unique<runtime::FileLogStorage>(logDir);
        if (const auto found = flags.values.find("--checkpoint-dir"); found != flags.values.end()) {
            if (!std::filesystem::is_directory(found->second, error)) {
                return CommandResult{{}, "order_book_cli: cannot use checkpoint directory " + found->second + ": no such directory\n",
                                     inputError};
            }
            if (!std::filesystem::equivalent(found->second, logDir, error)) {
                stores.checkpoints = std::make_unique<runtime::FileLogStorage>(found->second);
            }
        }
        return stores;
    }

    // Read-only: what a start would make of the store, and drop.
    CommandResult verify(std::span<const std::string_view> args) {
        constexpr std::array<std::string_view, 2> allowed{"--checkpoint-dir", "--validate"};
        std::variant<StoreFlags, CommandResult> parsed = parseStoreFlags("verify", args, allowed);
        if (auto* refused = std::get_if<CommandResult>(&parsed)) {
            return std::move(*refused);
        }
        const StoreFlags& flags = std::get<StoreFlags>(parsed);
        bool perCommand = false;
        if (const auto mode = flags.values.find("--validate"); mode != flags.values.end()) {
            if (mode->second != "final" && mode->second != "per-command") {
                return usage("unknown --validate mode '" + mode->second + "' for verify; expected final or per-command");
            }
            perCommand = mode->second == "per-command";
        }
        std::variant<Stores, CommandResult> opened = openStores(flags);
        if (auto* refused = std::get_if<CommandResult>(&opened)) {
            return std::move(*refused);
        }
        Stores& stores = std::get<Stores>(opened);
        return verifyStores(*stores.logs, stores.checkpointStore(), perCommand);
    }

    CommandResult verifyStores(runtime::LogStorage& logs, runtime::LogStorage& checkpoints, bool perCommand) {
        Printed printed;
        const runtime::StoreVerification verification = runtime::verifyStore(logs, checkpoints, perCommand);
        // Every shard verified, those before a failure included, then ok or the first failure.
        for (const runtime::VerifiedShard& shard : verification.shards) {
            printed.out << runtime::formatVerifiedShard(shard);
        }
        if (verification.outcome != runtime::VerifyOutcome::OK) {
            printed.err << "order_book_cli: verify: " << verification.failure << '\n';
            return printed.finish(verification.outcome == runtime::VerifyOutcome::BROKEN ? engineFailure : inputError);
        }
        printed.out << "ok\n";
        return printed.finish(0);
    }

}  // namespace order_book::cli
