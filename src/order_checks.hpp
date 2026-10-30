#pragma once

#include <optional>

#include "order_book/order_request.hpp"
#include "order_book/rejection_reason.hpp"
#include "order_book/side.hpp"
#include "order_book/types.hpp"

// Internal to the library: the field checks Order and OrderRequest share, written once.
namespace order_book::checks {

    // The per-field checks, in their fixed order: a zero ID, a missing price (where the
    // order needs one), a zero quantity, then a side other than BUY or SELL.
    inline std::optional<RejectionReason> fieldReason(OrderId id, Price price, bool priceRequired, Quantity quantity,
                                                      Side side) {
        if(id == 0) {
            return RejectionReason::INVALID_ORDER_ID;
        }
        if(priceRequired && price == 0) {
            return RejectionReason::INVALID_PRICE;
        }
        if(quantity == 0) {
            return RejectionReason::INVALID_QUANTITY;
        }
        if(!isValidSide(side)) {
            return RejectionReason::INVALID_SIDE;
        }
        return std::nullopt;
    }

    // Why a request is malformed, by the first failing check, or nothing if it is well
    // formed. It reads only the request, never a book: whether its ID is live is the
    // book's check, made after this one.
    inline std::optional<RejectionReason> requestReason(const OrderRequest& request) {
        if(const std::optional<RejectionReason> reason =
               fieldReason(request.id, request.price, true, request.quantity, request.side)) {
            return reason;
        }
        if(!isValidOrderType(request.type)) {
            return RejectionReason::UNSUPPORTED_ORDER_TYPE;
        }
        return std::nullopt;
    }

}  // namespace order_book::checks
