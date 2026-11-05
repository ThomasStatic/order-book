#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include "order_book/book_event.hpp"
#include "order_book/command_log.hpp"
#include "order_book/event_format.hpp"
#include "order_book/network/client_stream.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_inputs.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/symbol.hpp"

namespace {
    namespace replay = order_book::replay;

    // Exit statuses: a file that replays to the end exits 0, even when the book rejected
    // some commands, and so does --help.
    constexpr int inputError = 1;    // the file can't be opened or read, or is malformed
    constexpr int usageError = 2;
    constexpr int engineFailure = 3; // the book threw, or failed an invariant check
    // send's own: 1 is also no connection, and 3 a connection lost before every answer came.
    constexpr int connectionLost = 3;
    constexpr int someRefused = 4;   // every command answered, some refused: safe to send again

    constexpr std::string_view usageText =
        "usage: order_book_cli replay <path>... [--trades] [--events] [--book] [--validate none|final|per-command]\n"
        "                                       [--symbol SYM] [--no-statistics]\n"
        "       order_book_cli convert <path>... --out <path> [--symbol SYM]\n"
        "       order_book_cli send <file> --connect ADDR:PORT [--symbol SYM]\n"
        "       order_book_cli checkpoint describe|verify <file>\n"
        "       order_book_cli demo\n"
        "       order_book_cli --help\n";

    constexpr std::string_view helpText =
        "Replays a CSV command file (docs/replay-format.md) or a binary command log\n"
        "(docs/command-log-format.md), told apart by its first bytes, on an empty order book and\n"
        "prints replay statistics. Never prompts: everything is set on the command line.\n"
        "\n"
        "commands:\n"
        "  replay <path>...   replay the input, then print its statistics; a directory means every\n"
        "                     log file in it\n"
        "  convert <path>...  turn a CSV into a log, or a log into a v3 CSV; several inputs or a\n"
        "                     directory make --out a directory of commands-<symbol>.csv\n"
        "  send <file>        send the file's commands to a running order_book_engine and print\n"
        "                     its answers as replay --events --no-statistics prints the file\n"
        "  checkpoint describe <file>\n"
        "                     print a checkpoint file's two headers (docs/checkpoint-format.md),\n"
        "                     checking nothing past its first bytes\n"
        "  checkpoint verify <file>\n"
        "                     check a checkpoint file's checksum, then rebuild its book from the\n"
        "                     image; print ok, or the first fault's byte offset, code, and message\n"
        "  demo               print the events of a short scripted session\n"
        "  help               print this help\n"
        "\n"
        "replay options, in any order before or after the file:\n"
        "  --trades           print each trade, tagged with its command's line, before the statistics\n"
        "  --events           print every event instead, tagged with its command's line; this\n"
        "                     includes the trades\n"
        "  --book             print the final book after the statistics, best price first, then\n"
        "                     the last trade price\n"
        "  --validate MODE    check the book's invariants: none, final (the default), or\n"
        "                     per-command (after every command; slower)\n"
        "  --symbol SYM       let a v1 or v2 file stand in for this one instrument; a file that\n"
        "                     names its own symbols refuses it\n"
        "  --no-statistics    leave out the statistics\n"
        "  --                 treat the next argument as the file, even if it starts with '-'\n"
        "  -h, --help         print this help and exit\n"
        "\n"
        "send options, in any order before or after the file:\n"
        "  --connect ADDR:PORT  the engine's IPv4 address and port; required\n"
        "  --symbol SYM         as for replay, and required for a v1 or v2 file\n"
        "  --                   as for replay\n"
        "\n"
        "exit status:\n"
        "  0  replayed to the end, even if the book rejected some commands; or help printed;\n"
        "     for send, every command answered; for checkpoint, the file described or verified\n"
        "  1  input error: the file can't be opened or read, or is malformed (what ran before\n"
        "     the error is still printed); for send, also no connection; for checkpoint, not a\n"
        "     checkpoint file, or one verify refuses\n"
        "  2  usage error\n"
        "  3  engine failure: the book threw, or failed an invariant check; for send, the\n"
        "     connection was lost or the engine sent ProtocolError before every command was\n"
        "     answered, so what ran is unknown (this wins over 1 and 4)\n"
        "  4  send only: every command answered, but at least one was Refused, so that one\n"
        "     certainly did not run and is safe to send again\n"
        "\n"
        "A file that names instruments prints one section per symbol, sorted by symbol bytes,\n"
        "each holding one book's output.\n"
        "\n"
        "examples:\n"
        "  order_book_cli replay orders.csv\n"
        "  order_book_cli replay orders.csv --trades\n"
        "  order_book_cli replay orders.csv --trades --book\n"
        "  order_book_cli replay orders.csv --events --book --validate per-command\n"
        "  order_book_cli send orders.csv --connect 127.0.0.1:9000 --symbol AAPL\n";

    int printHelp() {
        std::cout << usageText << '\n' << helpText;
        return EXIT_SUCCESS;
    }

    // One line saying what was wrong, then the usage, on stderr.
    int usage(std::string_view problem) {
        std::cerr << "order_book_cli: " << problem << '\n' << usageText;
        return usageError;
    }

    bool isHelpOption(std::string_view arg) {
        return arg == "-h" || arg == "--help";
    }

    std::string repeated(std::string_view option) {
        return "'" + std::string(option) + "' was given twice";
    }

    std::string notASymbol(std::string_view text) {
        return "'" + std::string(text) + "' is not a symbol (1 to 8 of A-Z, 0-9, '.', '-')";
    }

    // One engine log still has to stand at position 0, or its books would start wrong. Prints
    // why not, if it doesn't.
    bool refusedLog(const std::string& path) {
        if (!replay::isCommandLogFile(path)) {
            return false;
        }
        const replay::ReplayInputs checked = replay::readReplayInputs({std::filesystem::path(path)});
        if (checked.refusal) {
            std::cerr << "order_book_cli: " << *checked.refusal << '\n';
        }
        return checked.refusal.has_value();
    }

    // Printing lives here, outside the engine: the book only returns events.
    void printEvents(std::span<const order_book::BookEvent> events) {
        for (const order_book::BookEvent& event : events) {
            std::cout << order_book::formatEvent(event) << '\n';
        }
    }

    int runDemo() {
        using order_book::Order;
        using order_book::Side;

        order_book::OrderBook book;

        printEvents(book.submitOrder(Order(1, 101, 5, 1, Side::SELL)).getEvents());
        printEvents(book.submitOrder(Order(2, 100, 3, 1, Side::SELL)).getEvents());
        printEvents(book.submitOrder(Order(2, 99, 1, 1, Side::BUY)).getEvents());
        printEvents(book.submitOrder(Order(3, 101, 6, 1, Side::BUY)).getEvents());
        printEvents(book.cancelOrder(1).getEvents());
        printEvents(book.cancelOrder(1).getEvents());

        return EXIT_SUCCESS;
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
    void printBookFailure(std::string_view prefix, const replay::ReplayError& error,
                          const std::optional<replay::ReplayError>& located, const ErrorText& text) {
        std::cout.flush();
        std::cerr << prefix << text(error) << '\n';
        if (replay::isFailedFinalCheck(error)) {
            std::cerr << prefix << "replaying with per-command checks to find the first failing command\n";
            if (located) {
                std::cerr << prefix << text(*located) << '\n';
            } else {
                std::cerr << prefix << "the per-command replay found no failure\n";
            }
        }
    }

    // Each book's run, then every section, then any diagnostic, as a single book does below. A
    // book that fails prints only its diagnostic, prefixed with its symbol.
    int runInstruments(const ReplaySettings& settings, const replay::ReplayInput& input) {
        replay::ReplayOptions options;
        options.invariantValidation = settings.validation;
        const replay::InstrumentsOutcome outcome = replay::replayInstruments(
            replay::groupBySymbol(input.records, settings.symbol.value_or(replay::Symbol{})), options);

        if (const auto* failure = std::get_if<replay::InstrumentFailure>(&outcome)) {
            printBookFailure("symbol " + failure->symbol.text() + ": ", failure->error, failure->located,
                             [&settings](const replay::ReplayError& error) { return replay::formatReplayError(error, settings.path); });
            return engineFailure;
        }

        std::cout << replay::formatInstrumentSections(std::get<std::vector<replay::InstrumentReplay>>(outcome), outputOf(settings));
        std::cout.flush();
        if (input.error) {
            std::cerr << replay::formatReplayError(*input.error, settings.path) << '\n';
            return inputError;
        }
        return EXIT_SUCCESS;
    }

    // Several inputs. A v1 or v2 file runs alone, unless --symbol makes it one instrument; files
    // that name symbols run together, in one set of sections, after them. Lines repeat across
    // files, so an event names its file too. Reading stopped at the first error, so what came
    // before it still runs and prints, and then the diagnostic.
    int runSeveral(const ReplaySettings& settings) {
        const std::vector<std::filesystem::path> paths(settings.paths.begin(), settings.paths.end());
        const replay::ReplayInputs inputs = replay::readReplayInputs(paths);
        if (inputs.refusal) {
            std::cerr << "order_book_cli: " << *inputs.refusal << '\n';
            return inputError;
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
            order_book::OrderBook book;
            const replay::ReplayOutcome outcome = replay::ReplayEngine::run(merged.records, book, options);
            if (const replay::ReplayError* failure = std::get_if<replay::ReplayError>(&outcome)) {
                std::optional<replay::ReplayError> located;
                if (replay::isFailedFinalCheck(*failure)) {
                    located = replay::locateFailedFinalCheck(merged.records, options);
                }
                printBookFailure("", *failure, located,
                                 [&merged](const replay::ReplayError& error) { return replay::formatMergedError(error, merged); });
                return engineFailure;
            }
            replay::ReplayOutput output = outputOf(settings);
            output.lineTags = &merged.tags;
            std::cout << (printedAny ? "\n" : "") << replay::formatReplayOutput(std::get<replay::ReplayResult>(outcome), output);
            printedAny = true;
        }

        const replay::MergedRecords merged = replay::mergeRecords(named);
        const replay::InstrumentsOutcome outcome =
            replay::replayInstruments(replay::groupBySymbol(merged.records, settings.symbol.value_or(replay::Symbol{})), options);
        if (const auto* failure = std::get_if<replay::InstrumentFailure>(&outcome)) {
            printBookFailure("symbol " + failure->symbol.text() + ": ", failure->error, failure->located,
                             [&merged](const replay::ReplayError& error) { return replay::formatMergedError(error, merged); });
            return engineFailure;
        }
        replay::ReplayOutput output = outputOf(settings);
        output.lineTags = &merged.tags;
        const std::string sections =
            replay::formatInstrumentSections(std::get<std::vector<replay::InstrumentReplay>>(outcome), output);
        std::cout << (printedAny && !sections.empty() ? "\n" : "") << sections;
        std::cout.flush();

        if (!inputs.files.empty() && inputs.files.back().input.error) {
            const replay::LoadedInput& last = inputs.files.back();
            std::cerr << replay::formatReplayError(*last.input.error, last.path.generic_string()) << '\n';
            return inputError;
        }
        return EXIT_SUCCESS;
    }

    // Replays settings.path on an empty book. On stdout: the trades or events asked for, a
    // blank line if any were printed, the statistics, then the final book if asked for.
    // Input that stops part-way still prints what ran, then the diagnostic on stderr. An
    // engine failure prints only its diagnostic, since the result was lost with it; a failed
    // final check is replayed again on a fresh book, checking after every command, to find
    // the first line after which the book is broken.
    int runReplay(const ReplaySettings& settings) {
        const std::string& path = settings.path;
        if (refusedLog(path)) {
            return inputError;
        }
        const replay::ReplayInput input = replay::readReplayOrLogFile(path);
        if (input.format == replay::ReplayFormat::V3) {
            if (settings.symbol) {
                return usage(replay::symbolOnNamedInput(path));
            }
            return runInstruments(settings, input);
        }
        if (settings.symbol) {
            return runInstruments(settings, input);
        }

        replay::ReplayOptions options;
        options.invariantValidation = settings.validation;
        order_book::OrderBook book;
        const replay::ReplayOutcome outcome = replay::ReplayEngine::run(input.records, book, options);

        if (const replay::ReplayError* failure = std::get_if<replay::ReplayError>(&outcome)) {
            std::optional<replay::ReplayError> located;
            if (replay::isFailedFinalCheck(*failure)) {
                located = replay::locateFailedFinalCheck(input.records, options);
            }
            printBookFailure("", *failure, located,
                             [&path](const replay::ReplayError& error) { return replay::formatReplayError(error, path); });
            return engineFailure;
        }

        std::cout << replay::formatReplayOutput(std::get<replay::ReplayResult>(outcome), outputOf(settings));
        std::cout.flush();

        if (input.error) {
            std::cerr << replay::formatReplayError(*input.error, path) << '\n';
            return inputError;
        }
        return EXIT_SUCCESS;
    }

    // args are what follows "replay". Options may come before or after the file; after
    // "--", the next argument is the file whatever it looks like.
    int replayCommand(const std::vector<std::string_view>& args) {
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
                    return usage("unknown --validate mode '" + std::string(mode)
                                 + "'; expected none, final, or per-command");
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
        const std::vector<std::filesystem::path> paths(settings.paths.begin(), settings.paths.end());
        const std::vector<std::filesystem::path> expanded = replay::expandInputs(paths);
        if (expanded.size() == 1) {
            settings.path = expanded[0] == paths[0] ? settings.paths[0] : expanded[0].generic_string();
            return runReplay(settings);
        }
        return runSeveral(settings);
    }

    // args are what follows "convert".
    int convertCommand(const std::vector<std::string_view>& args) {
        std::vector<std::filesystem::path> inputs;
        std::optional<std::filesystem::path> out;
        std::optional<replay::Symbol> symbol;
        std::set<std::string_view> given;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (arg.starts_with("--") && !given.insert(arg).second) {
                return usage(repeated(arg));
            }
            if (arg == "--out" || arg == "--symbol") {
                if (i + 1 == args.size()) {
                    return usage(std::string(arg) + " needs a value");
                }
                const std::string_view value = args[++i];
                if (arg == "--out") {
                    out = std::filesystem::path(value);
                } else {
                    symbol = replay::Symbol::parse(value);
                    if (!symbol) {
                        return usage(notASymbol(value));
                    }
                }
            } else if (arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                inputs.emplace_back(arg);
            }
        }
        if (inputs.empty()) {
            return usage("convert needs a file to read");
        }
        if (!out) {
            return usage("convert needs --out");
        }

        const replay::ConvertOutcome outcome = replay::convertReplayFiles(inputs, *out, symbol);
        if (outcome.usage) {
            return usage(*outcome.usage);
        }
        if (outcome.refusal) {
            std::cerr << "order_book_cli: " << *outcome.refusal << '\n';
            return inputError;
        }
        if (outcome.error) {
            std::cerr << replay::formatReplayError(*outcome.error, outcome.errorPath.generic_string()) << '\n';
            return inputError;
        }
        return EXIT_SUCCESS;
    }

    // args are what follows "send". One file, sent over one connection: its answers print as
    // replay --events --no-statistics prints the file, once the engine's stream has ended.
    int sendCommand(const std::vector<std::string_view>& args) {
        std::vector<std::string_view> files;
        std::optional<order_book::runtime::ListenAddress> address;
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
                    address = order_book::runtime::parseListenAddress(value);
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

        const std::string path(files[0]);
        if (refusedLog(path)) {
            return inputError;
        }
        const replay::ReplayInput input = replay::readReplayOrLogFile(path);
        if (input.records.empty() && input.error) {
            std::cerr << replay::formatReplayError(*input.error, path) << '\n';
            return inputError;
        }
        if (input.format == replay::ReplayFormat::V3 && symbol) {
            return usage(replay::symbolOnNamedInput(path));
        }
        if (input.format != replay::ReplayFormat::V3 && !symbol) {
            return usage("'" + path + "' names no symbol, so it needs --symbol");
        }

        std::vector<order_book::network::ClientCommand> commands;
        commands.reserve(input.records.size());
        for (const replay::ReplayRecord& record : input.records) {
            commands.push_back({record.lineNumber, record.symbol.empty() ? *symbol : record.symbol, record.command});
        }

        order_book::network::TcpSocket socket;
        try {
            socket = order_book::network::TcpSocket::connectIpv4(address->host, address->port);
        } catch (const std::system_error& error) {
            std::cerr << "order_book_cli: cannot connect to " << order_book::runtime::formatListenAddress(*address) << ": "
                      << error.what() << '\n';
            return inputError;
        }
        order_book::network::ClientStream stream(std::move(commands));
        try {
            order_book::network::exchange(stream, socket);
        } catch (const std::system_error& error) {
            // poll failed: the connection is over, and what came still prints.
            std::cerr << "order_book_cli: " << error.what() << '\n';
            stream.ended();
        }

        std::cout << stream.sections();
        std::cout.flush();
        for (const std::string& problem : stream.diagnostics()) {
            std::cerr << "order_book_cli: " << problem << '\n';
        }
        if (input.error) {
            std::cerr << replay::formatReplayError(*input.error, path) << '\n';
        }
        // 3 wins over 1, and both over 4: what ran unknown is worse than a file cut short.
        if (stream.end() == order_book::network::ConnectionEnd::LOST) {
            return connectionLost;
        }
        if (input.error) {
            return inputError;
        }
        return stream.end() == order_book::network::ConnectionEnd::REFUSED ? someRefused : EXIT_SUCCESS;
    }

    // The whole file, or nothing if it can't be opened or read.
    std::optional<std::vector<std::byte>> readWholeFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad()) {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(text.size());
        if (!text.empty()) {  // an empty vector's data() may be null, which memcpy must not get
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return bytes;
    }

    // args are what follows "checkpoint". One file, read whole, with no lock: a published
    // checkpoint never changes.
    int checkpointCommand(const std::vector<std::string_view>& args) {
        namespace runtime = order_book::runtime;
        if (args.empty()) {
            return usage("checkpoint needs a command: describe or verify");
        }
        const std::string_view command = args[0];
        if (command != "describe" && command != "verify") {
            return usage("unknown checkpoint command '" + std::string(command) + "'; expected describe or verify");
        }
        std::vector<std::string_view> files;
        bool optionsEnded = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
            } else if (!optionsEnded && arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                files.push_back(arg);
            }
        }
        const std::string name = "checkpoint " + std::string(command);
        if (files.empty()) {
            return usage(name + " needs a file to read");
        }
        if (files.size() > 1) {
            return usage(name + " takes one file, not " + std::to_string(files.size()));
        }

        const std::string path(files[0]);
        const std::optional<std::vector<std::byte>> file = readWholeFile(path);
        if (!file) {
            std::cerr << path << ": cannot open checkpoint file\n";
            return inputError;
        }
        if (command == "describe") {
            const auto described = runtime::describeCheckpoint(*file);
            if (const auto* error = std::get_if<runtime::CheckpointError>(&described)) {
                std::cerr << runtime::formatCheckpointFault(path, {error->code, error->offset, error->message}) << '\n';
                return inputError;
            }
            std::cout << runtime::formatCheckpointDescription(std::get<runtime::CheckpointDescription>(described));
            return EXIT_SUCCESS;
        }
        if (const std::optional<runtime::CheckpointFault> fault = runtime::verifyCheckpoint(*file)) {
            std::cerr << runtime::formatCheckpointFault(path, *fault) << '\n';
            return inputError;
        }
        std::cout << "ok\n";
        return EXIT_SUCCESS;
    }
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Write '\n' as is, not as CRLF, so a replay prints the same bytes on every platform.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage("missing command");
    }
    // A help option anywhere before "--" wins over everything else on the line.
    for (const std::string_view arg : args) {
        if (arg == "--") {
            break;
        }
        if (isHelpOption(arg)) {
            return printHelp();
        }
    }

    const std::string_view command = args[0];
    const std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (command == "replay") {
        return replayCommand(rest);
    }
    if (command == "convert") {
        return convertCommand(rest);
    }
    if (command == "send") {
        return sendCommand(rest);
    }
    if (command == "checkpoint") {
        return checkpointCommand(rest);
    }
    if (command == "demo" || command == "help") {
        if (!rest.empty()) {
            return usage("unexpected argument '" + std::string(rest[0]) + "'; "
                         + std::string(command) + " takes none");
        }
        return command == "demo" ? runDemo() : printHelp();
    }
    return usage("unknown command '" + std::string(command) + "'");
}
