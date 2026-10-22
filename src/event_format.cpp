#include <type_traits>
#include <variant>

#include "order_book/event_format.hpp"

namespace order_book {
    namespace {
        const char* sideName(Side side) {
            return side == Side::BUY ? "BUY" : "SELL";
        }
    }  // namespace

    std::string formatEvent(const BookEvent& event) {
        return std::visit([](const auto& e) -> std::string {
            using Event = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<Event, OrderAccepted>) {
                return "OrderAccepted id=" + std::to_string(e.orderId) + " side=" + sideName(e.side)
                     + " price=" + std::to_string(e.price) + " qty=" + std::to_string(e.quantity);
            }
            else if constexpr (std::is_same_v<Event, OrderRejected>) {
                return "OrderRejected id=" + std::to_string(e.orderId) + " reason=" + rejectionReasonName(e.reason);
            }
            else if constexpr (std::is_same_v<Event, TradeExecuted>) {
                return "TradeExecuted incoming=" + std::to_string(e.incomingOrderId)
                     + " resting=" + std::to_string(e.restingOrderId)
                     + " price=" + std::to_string(e.executionPrice) + " qty=" + std::to_string(e.executionQuantity);
            }
            else if constexpr (std::is_same_v<Event, OrderCancelled>) {
                return "OrderCancelled id=" + std::to_string(e.orderId) + " qty=" + std::to_string(e.cancelledQuantity);
            }
            else {
                static_assert(std::is_same_v<Event, CancelRejected>, "every BookEvent alternative needs a format");
                return "CancelRejected id=" + std::to_string(e.orderId) + " reason=" + rejectionReasonName(e.reason);
            }
        }, event);
    }
}  // namespace order_book
