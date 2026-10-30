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
                // Price 0 is an order with no limit price; no other event carries price 0.
                return "OrderAccepted id=" + std::to_string(e.orderId) + " side=" + sideName(e.side)
                     + " price=" + (e.price == 0 ? std::string("MARKET") : std::to_string(e.price))
                     + " qty=" + std::to_string(e.quantity);
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
            else if constexpr (std::is_same_v<Event, CancelRejected>) {
                return "CancelRejected id=" + std::to_string(e.orderId) + " reason=" + rejectionReasonName(e.reason);
            }
            else if constexpr (std::is_same_v<Event, OrderExpired>) {
                return "OrderExpired id=" + std::to_string(e.orderId) + " qty=" + std::to_string(e.expiredQuantity);
            }
            else if constexpr (std::is_same_v<Event, SelfTradePrevented>) {
                return "SelfTradePrevented incoming=" + std::to_string(e.incomingOrderId)
                     + " resting=" + std::to_string(e.restingOrderId)
                     + " price=" + std::to_string(e.price) + " qty=" + std::to_string(e.cancelledQuantity);
            }
            else if constexpr (std::is_same_v<Event, StopTriggered>) {
                return "StopTriggered id=" + std::to_string(e.orderId) + " stop_price=" + std::to_string(e.stopPrice)
                     + " trade_price=" + std::to_string(e.tradePrice);
            }
            else if constexpr (std::is_same_v<Event, OrderReplaced>) {
                return "OrderReplaced id=" + std::to_string(e.orderId) + " side=" + sideName(e.side)
                     + " price=" + std::to_string(e.price) + " qty=" + std::to_string(e.quantity)
                     + " open=" + std::to_string(e.openQuantity) + " priority=" + (e.priorityKept ? "KEPT" : "LOST");
            }
            else {
                static_assert(std::is_same_v<Event, ReplaceRejected>, "every BookEvent alternative needs a format");
                return "ReplaceRejected id=" + std::to_string(e.orderId) + " reason=" + rejectionReasonName(e.reason);
            }
        }, event);
    }
}  // namespace order_book
