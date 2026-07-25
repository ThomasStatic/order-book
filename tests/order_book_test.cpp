#include <cstdlib>
#include <iostream>

#include "order_book/order_book.hpp"

int main() {
    const order_book::OrderBook book{};
    (void)book;
    std::cout << "order_book smoke test passed" << std::endl;
    return EXIT_SUCCESS;
}
