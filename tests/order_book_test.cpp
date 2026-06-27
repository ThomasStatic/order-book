#include <gtest/gtest.h>

#include "order_book/order_book.hpp"

TEST(OrderBook, CanBeConstructed) {
    const order_book::OrderBook book{};
    (void)book;
    SUCCEED();
}
