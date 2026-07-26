#include <iostream>
#include "order_book/order.hpp"

int main() {
    std::cout << "order_book_cli placeholder" << std::endl;

    std::cout << "creating an order object" << std::endl;
    order::Order order(1, 1, 1, 1, order::Side::BUY);




    return 0;
}
