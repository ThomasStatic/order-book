#include "order_book/command.hpp"

#include <type_traits>
#include <utility>

#include "order_book/order_book.hpp"

namespace order_book::replay {

    OrderRequest toRequest(const AddCommand& command) {
        return OrderRequest{command.orderId, command.side,     command.price,   command.quantity, command.orderType,
                            command.timeInForce, command.stopPrice, command.postOnly, command.owner,
                            command.selfTradePrevention};
    }

    ReplaceRequest toRequest(const ReplaceCommand& command) {
        return ReplaceRequest{command.orderId, command.price, command.quantity,
                              command.postOnly, command.owner, command.selfTradePrevention};
    }

    SubmissionResult executeCommand(OrderBook& book, const AddCommand& command) {
        return book.submitOrder(toRequest(command));
    }

    CancellationResult executeCommand(OrderBook& book, const CancelCommand& command) {
        return book.cancelOrder(command.orderId);
    }

    ReplaceResult executeCommand(OrderBook& book, const ReplaceCommand& command) {
        return book.replaceOrder(toRequest(command));
    }

    CommandOutcome executeCommand(OrderBook& book, const Command& command) {
        return std::visit([&book](const auto& alternative) -> CommandOutcome { return executeCommand(book, alternative); },
                          command);
    }

    CommandResult::CommandResult()
        : submitted(SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE)),
          cancelled(CancellationResult::rejected(1, RejectionReason::ORDER_NOT_FOUND)),
          replaced(ReplaceResult::rejected(1, RejectionReason::ORDER_NOT_FOUND)) {}

    void CommandResult::reserve(std::size_t trades, std::size_t events) {
        submitted.reserve(trades, events);
        replaced.reserve(trades, events);
    }

    void executeCommand(OrderBook& book, const Command& command, CommandResult& out) {
        std::visit(
            [&](const auto& alternative) {
                using Kind = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Kind, AddCommand>) {
                    book.submitOrder(toRequest(alternative), out.submitted);
                    out.lastEvents = out.submitted.getEvents();
                } else if constexpr (std::is_same_v<Kind, CancelCommand>) {
                    out.cancelled = book.cancelOrder(alternative.orderId);
                    out.lastEvents = out.cancelled.getEvents();
                } else {
                    static_assert(std::is_same_v<Kind, ReplaceCommand>, "every Command alternative needs a book call");
                    book.replaceOrder(toRequest(alternative), out.replaced);
                    out.lastEvents = out.replaced.getEvents();
                }
            },
            command);
    }

}  // namespace order_book::replay
