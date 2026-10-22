#include <iostream>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/event_format.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace {
    // Printing lives here, outside the engine: the book only returns events.
    void printEvents(const std::vector<order_book::BookEvent>& events) {
        for (const order_book::BookEvent& event : events) {
            std::cout << order_book::formatEvent(event) << '\n';
        }
    }
}  // namespace

int main() {
    using order_book::Order;
    using order_book::Side;

    order_book::OrderBook book;

    printEvents(book.submitOrder(Order(1, 101, 5, 1, Side::SELL)).getEvents());
    printEvents(book.submitOrder(Order(2, 100, 3, 1, Side::SELL)).getEvents());
    printEvents(book.submitOrder(Order(2, 99, 1, 1, Side::BUY)).getEvents());
    printEvents(book.submitOrder(Order(3, 101, 6, 1, Side::BUY)).getEvents());
    printEvents(book.cancelOrder(1).getEvents());
    printEvents(book.cancelOrder(1).getEvents());

    return 0;
}
