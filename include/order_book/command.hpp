#pragma once

#include <cstddef>
#include <span>
#include <variant>

#include "book_event.hpp"
#include "cancellation_result.hpp"
#include "order_request.hpp"
#include "order_type.hpp"
#include "replace_request.hpp"
#include "replace_result.hpp"
#include "self_trade_prevention.hpp"
#include "side.hpp"
#include "submission_result.hpp"
#include "time_in_force.hpp"
#include "types.hpp"

namespace order_book {
    class OrderBook;
}

// What every format means by a command, and the one place that maps a command to a book call:
// replay, the command log, and the engine all run commands through executeCommand.
namespace order_book::replay {

    // A parsed command satisfies the Order constructor's preconditions: nonzero ID, price,
    // and quantity, and a valid side.
    struct AddCommand {
        OrderId orderId;
        Side side;
        Price price;  // 0 on a MARKET or STOP order (v2 only)
        Quantity quantity;
        // v2's columns; a v1 record has these defaults, a GTC limit order with no owner.
        OrderType orderType = OrderType::LIMIT;
        TimeInForce timeInForce = TimeInForce::GTC;
        Price stopPrice = 0;
        bool postOnly = false;
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const AddCommand&) const = default;
    };

    struct CancelCommand {
        OrderId orderId;

        bool operator==(const CancelCommand&) const = default;
    };

    // A replace of a resting order (v2 only), which keeps its ID and takes a new total.
    struct ReplaceCommand {
        OrderId orderId;
        Price price;
        Quantity quantity;  // the new total
        bool postOnly = false;
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const ReplaceCommand&) const = default;
    };

    // Open: later releases may append alternatives, never reorder one, so code that visits it
    // should handle a command it does not know.
    using Command = std::variant<AddCommand, CancelCommand, ReplaceCommand>;

    OrderRequest toRequest(const AddCommand& command);
    ReplaceRequest toRequest(const ReplaceCommand& command);

    // Each alternative's book call: ADD always runs the request overload, which for v1's fields
    // is exactly submitOrder(Order).
    SubmissionResult executeCommand(OrderBook& book, const AddCommand& command);
    CancellationResult executeCommand(OrderBook& book, const CancelCommand& command);
    ReplaceResult executeCommand(OrderBook& book, const ReplaceCommand& command);

    using CommandOutcome = std::variant<SubmissionResult, CancellationResult, ReplaceResult>;
    CommandOutcome executeCommand(OrderBook& book, const Command& command);

    // One reusable result of each kind, for a caller that runs many commands without reaching
    // the heap for results. Each kind's result is overwritten by the next command of that kind.
    class CommandResult {
    public:
        CommandResult();
        // Its events are a view into itself.
        CommandResult(const CommandResult&) = delete;
        CommandResult& operator=(const CommandResult&) = delete;

        // Room for this many trades and events in a submission's or a replace's result.
        void reserve(std::size_t trades, std::size_t events);

        // What the last command made, in execution order. Holds whichever result it filled.
        std::span<const BookEvent> events() const noexcept { return lastEvents; }

        const SubmissionResult& submission() const noexcept { return submitted; }
        const CancellationResult& cancellation() const noexcept { return cancelled; }
        const ReplaceResult& replacement() const noexcept { return replaced; }

    private:
        friend void executeCommand(OrderBook& book, const Command& command, CommandResult& out);

        SubmissionResult submitted;
        CancellationResult cancelled;
        ReplaceResult replaced;
        std::span<const BookEvent> lastEvents;  // viewed from whichever result holds them
    };

    // Overwrites out. If it throws, out stays valid with unspecified contents.
    void executeCommand(OrderBook& book, const Command& command, CommandResult& out);

}  // namespace order_book::replay
