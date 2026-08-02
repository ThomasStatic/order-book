#include <iostream>
#include "order_book/order.hpp"

int main() {
    std::cout << "order_book_cli placeholder" << std::endl;

    std::cout << "creating an order object" << std::endl;
    order_book::Order order(1, 1, 1, 1, order_book::Side::BUY);

    return 0;
}
