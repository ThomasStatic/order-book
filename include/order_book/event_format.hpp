#pragma once

#include <string>

#include "book_event.hpp"

namespace order_book {
    // One line of text per event, for printing. The engine never calls this: events are
    // its domain output, and how (or whether) to show them is up to the caller.
    std::string formatEvent(const BookEvent& event);
}  // namespace order_book
