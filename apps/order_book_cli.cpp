#include <cstdlib>
#include <iostream>
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

    // Exit statuses for `replay`: a file that replays to the end exits 0, even when the book
    // rejected some commands.
    constexpr int inputError = 1;    // the file can't be opened or read, or is malformed
    constexpr int usageError = 2;
    constexpr int engineFailure = 3; // the book threw, or failed an invariant check

    // Printing lives here, outside the engine: the book only returns events.
    void printEvents(const std::vector<order_book::BookEvent>& events) {
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

    int usage() {
        std::cerr << "usage: order_book_cli [replay <file.csv> [--validate none|final|per-command]]\n";
        return usageError;
    }

    // Replays path on an empty book: its events, a blank line if there were any, then the
    // summary, on stdout. Input that stops part-way still prints what ran, then the
    // diagnostic on stderr. An engine failure prints only its diagnostic, since the result
    // was lost with it; a failed final check is replayed again on a fresh book, checking
    // after every command, to find the first line after which the book is broken.
    int runReplay(const std::string& path, replay::InvariantValidation validation) {
        const replay::ReplayInput input = replay::readReplayFile(path);

        replay::ReplayOptions options;
        options.invariantValidation = validation;
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
        for (const replay::ReplayEvent& event : result.events) {
            std::cout << replay::formatReplayEvent(event) << '\n';
        }
        if (!result.events.empty()) {
            std::cout << '\n';
        }
        std::cout << replay::formatReplaySummary(result);
        std::cout.flush();

        if (input.error) {
            std::cerr << replay::formatReplayError(*input.error, path) << '\n';
            return inputError;
        }
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
        return runDemo();
    }
    if (args[0] != "replay" || (args.size() != 2 && args.size() != 4)) {
        return usage();
    }

    replay::InvariantValidation validation = replay::InvariantValidation::FINAL;
    if (args.size() == 4) {
        if (args[2] != "--validate") {
            return usage();
        }
        if (args[3] == "none") {
            validation = replay::InvariantValidation::NONE;
        } else if (args[3] == "final") {
            validation = replay::InvariantValidation::FINAL;
        } else if (args[3] == "per-command") {
            validation = replay::InvariantValidation::PER_COMMAND;
        } else {
            return usage();
        }
    }
    return runReplay(std::string(args[1]), validation);
}
