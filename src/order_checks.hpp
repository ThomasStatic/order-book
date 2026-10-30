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
    // The checks, numbered as in the spec's request table: 1-4 the fields (a price is needed
    // unless the type has none; an invalid type needs one, keeping v1's order), 5 the type, 6 a
    // price on a type that has none, 7a the time in force, 7b a MARKET order that is GTC, 9
    // post-only on anything but a GTC LIMIT order, 10 a self-trade prevention mode that is not
    // an enumerator, whatever the owner.
    inline std::optional<RejectionReason> requestReason(const OrderRequest& request) {
        if(const std::optional<RejectionReason> reason =
               fieldReason(request.id, request.price, !hasNoPrice(request.type), request.quantity, request.side)) {
            return reason;
        }
        if(!isValidOrderType(request.type)) {
            return RejectionReason::UNSUPPORTED_ORDER_TYPE;
        }
        if(hasNoPrice(request.type) && request.price != 0) {
            return RejectionReason::PRICE_NOT_ALLOWED;
        }
        if(!isValidTimeInForce(request.timeInForce)) {
            return RejectionReason::INVALID_TIME_IN_FORCE;
        }
        if(request.type == OrderType::MARKET && request.timeInForce == TimeInForce::GTC) {
            return RejectionReason::TIME_IN_FORCE_NOT_ALLOWED;
        }
        if(request.postOnly && (request.type != OrderType::LIMIT || request.timeInForce != TimeInForce::GTC)) {
            return RejectionReason::POST_ONLY_NOT_ALLOWED;
        }
        if(!isValidSelfTradePrevention(request.selfTradePrevention)) {
            return RejectionReason::INVALID_SELF_TRADE_PREVENTION;
        }
        return std::nullopt;
    }

}  // namespace order_book::checks
