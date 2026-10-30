#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include "order_book/book_event.hpp"
#include "order_book/event_format.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_summary.hpp"

namespace {
    namespace replay = order_book::replay;

    // Exit statuses: a file that replays to the end exits 0, even when the book rejected
    // some commands, and so does --help.
    constexpr int inputError = 1;    // the file can't be opened or read, or is malformed
    constexpr int usageError = 2;
    constexpr int engineFailure = 3; // the book threw, or failed an invariant check

    constexpr std::string_view usageText =
        "usage: order_book_cli replay <file.csv> [--trades] [--events] [--book] [--validate none|final|per-command]\n"
        "       order_book_cli demo\n"
        "       order_book_cli --help\n";

    constexpr std::string_view helpText =
        "Replays a CSV command file (docs/replay-format.md) on an empty order book and prints\n"
        "replay statistics. Never prompts: everything is set on the command line.\n"
        "\n"
        "commands:\n"
        "  replay <file.csv>  replay the file, then print its statistics\n"
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
        "  --                 treat the next argument as the file, even if it starts with '-'\n"
        "  -h, --help         print this help and exit\n"
        "\n"
        "exit status:\n"
        "  0  replayed to the end, even if the book rejected some commands; or help printed\n"
        "  1  input error: the file can't be opened or read, or is malformed (what ran before\n"
        "     the error is still printed)\n"
        "  2  usage error\n"
        "  3  engine failure: the book threw, or failed an invariant check\n"
        "\n"
        "examples:\n"
        "  order_book_cli replay orders.csv\n"
        "  order_book_cli replay orders.csv --trades\n"
        "  order_book_cli replay orders.csv --trades --book\n"
        "  order_book_cli replay orders.csv --events --book --validate per-command\n";

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
        std::string path;
        bool printTrades = false;
        bool printEvents = false;  // every event, which covers printTrades
        bool printBook = false;
        replay::InvariantValidation validation = replay::InvariantValidation::FINAL;
    };

    // Replays settings.path on an empty book. On stdout: the trades or events asked for, a
    // blank line if any were printed, the statistics, then the final book if asked for.
    // Input that stops part-way still prints what ran, then the diagnostic on stderr. An
    // engine failure prints only its diagnostic, since the result was lost with it; a failed
    // final check is replayed again on a fresh book, checking after every command, to find
    // the first line after which the book is broken.
    int runReplay(const ReplaySettings& settings) {
        const std::string& path = settings.path;
        const replay::ReplayInput input = replay::readReplayFile(path);

        replay::ReplayOptions options;
        options.invariantValidation = settings.validation;
        order_book::OrderBook book;
        const replay::ReplayOutcome outcome = replay::ReplayEngine::run(input.records, book, options);

        if (const replay::ReplayError* failure = std::get_if<replay::ReplayError>(&outcome)) {
            std::cout.flush();
            std::cerr << replay::formatReplayError(*failure, path) << '\n';
            if (failure->code == replay::ReplayErrorCode::INVARIANT_VIOLATION && !failure->lineNumber) {
                std::cerr << "replaying with per-command checks to find the first failing command\n";
                options.invariantValidation = replay::InvariantValidation::PER_COMMAND;
                order_book::OrderBook freshBook;
                const replay::ReplayOutcome rerun = replay::ReplayEngine::run(input.records, freshBook, options);
                if (const replay::ReplayError* located = std::get_if<replay::ReplayError>(&rerun)) {
                    std::cerr << replay::formatReplayError(*located, path) << '\n';
                } else {
                    std::cerr << "the per-command replay found no failure\n";
                }
            }
            return engineFailure;
        }

        const replay::ReplayResult& result = std::get<replay::ReplayResult>(outcome);
        bool printedAny = false;
        for (const replay::ReplayEvent& event : result.events) {
            const bool isTrade = std::holds_alternative<order_book::TradeExecuted>(event.event);
            if (settings.printEvents || (settings.printTrades && isTrade)) {
                std::cout << replay::formatReplayEvent(event) << '\n';
                printedAny = true;
            }
        }
        if (printedAny) {
            std::cout << '\n';
        }
        std::cout << replay::formatReplayStatistics(result);
        if (settings.printBook) {
            std::cout << replay::formatFinalBook(result);
        }
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
        bool havePath = false;
        bool optionsEnded = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
            } else if (!optionsEnded && arg == "--trades") {
                settings.printTrades = true;
            } else if (!optionsEnded && arg == "--events") {
                settings.printEvents = true;
            } else if (!optionsEnded && arg == "--book") {
                settings.printBook = true;
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
            } else if (havePath) {
                return usage("unexpected argument '" + std::string(arg) + "'; replay takes one file");
            } else {
                settings.path = std::string(arg);
                havePath = true;
            }
        }
        if (!havePath) {
            return usage("replay needs a file to read");
        }
        return runReplay(settings);
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
    if (command == "demo" || command == "help") {
        if (!rest.empty()) {
            return usage("unexpected argument '" + std::string(rest[0]) + "'; "
                         + std::string(command) + " takes none");
        }
        return command == "demo" ? runDemo() : printHelp();
    }
    return usage("unknown command '" + std::string(command) + "'");
}
