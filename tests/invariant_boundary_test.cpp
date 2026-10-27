#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "order_book/invariants.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so this test can break the book on purpose.
struct OrderBookTestAccess {
    static ConsumptionResult consumeLevel(OrderBook& book, Side side, Price price, Quantity quantity) {
        return book.consumeLevel(side, price, quantity);
    }
    static std::uint64_t& aggregate(OrderBook& book, Price askPrice) { return book.asks.at(askPrice).quantity; }
    static Price& indexedPrice(OrderBook& book, OrderId orderId) { return book.orderIndex.at(orderId).tickPrice; }
};

}  // namespace order_book

namespace {

using order_book::Order;
using order_book::OrderBook;
using order_book::Side;

// An ask of 5 at 100 (order 1) and a bid of 4 at 90 (order 2). Each run breaks the book,
// then runs the command named on the command line. With automatic checks on, the command
// should abort at its invariant boundary, printing
//   order book invariant violated after <command>: <violation> ...
// which CTest matches; nothing else this program prints contains that line.
// Unused when automatic checks are off, since main then skips.
[[maybe_unused]] int run(const std::string& command) {
    using Access = order_book::OrderBookTestAccess;
    OrderBook book;
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.addOrder(Order(2, 90, 4, 1, Side::BUY));

    if (command == "cancelOrderThrows") {
        // Order 2's entry names a price with no level, so cancelOrder throws before changing
        // anything. The boundary still checks as the exception unwinds, and aborts there.
        Access::indexedPrice(book, 2) = 91;
        try {
            book.cancelOrder(2);
        } catch (const std::exception& error) {
            std::cerr << "FAILED: cancelOrder's exception escaped its boundary unchecked: " << error.what()
                      << std::endl;
            return EXIT_FAILURE;
        }
        std::cerr << "FAILED: cancelOrder returned for an order whose level is missing" << std::endl;
        return EXIT_FAILURE;
    }

    // The rest break the ask level at 100 (cached as 6), which the command leaves alone.
    Access::aggregate(book, 100) = 6;
    if (command == "addOrder") {
        book.addOrder(Order(3, 105, 1, 1, Side::SELL));
    } else if (command == "submitOrder") {
        book.submitOrder(Order(3, 91, 1, 1, Side::BUY));
    } else if (command == "cancelOrder") {
        book.cancelOrder(2);
    } else if (command == "consumeLevel") {
        Access::consumeLevel(book, Side::BUY, 90, 1);
    } else {
        std::cerr << "FAILED: unknown command " << command << std::endl;
        return EXIT_FAILURE;
    }
    std::cerr << "FAILED: " << command << " returned from a broken book without aborting" << std::endl;
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    if constexpr (!order_book::invariantChecksAfterCommands) {
        std::cout << "SKIPPED: automatic invariant checks are off in this build" << std::endl;
        return EXIT_SUCCESS;
    } else {
        if (argc != 2) {
            std::cerr << "FAILED: usage: invariant_boundary_tests <command>" << std::endl;
            return EXIT_FAILURE;
        }
#ifdef _MSC_VER
        // The abort is expected: skip the debug CRT's message box and crash report.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        // CTest fails any test killed by a signal, whatever it printed, so turn the expected
        // abort into a normal exit and let the output alone decide.
        std::signal(SIGABRT, [](int) { std::_Exit(EXIT_SUCCESS); });
        return run(argv[1]);
    }
}
