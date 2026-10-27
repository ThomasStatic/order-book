#pragma once

#include <optional>

#include "book_event.hpp"
#include "rejection_reason.hpp"
#include "small_vector.hpp"
#include "types.hpp"

namespace order_book {
    // A cancellation's single event, held inline so reporting it allocates nothing.
    using CancellationEvents = SmallVector<BookEvent, 1>;

    // Returned by OrderBook::cancelOrder. Built only through its factories, so its outcome,
    // reason, and single event always agree.
    class CancellationResult {
    private:
        bool success;
        std::optional<RejectionReason> rejectionReason;
        CancellationEvents events;

        CancellationResult(bool succeeded, std::optional<RejectionReason> reason, BookEvent event);

    public:
        // A cancelled order: success, no reason, and one OrderCancelled with what it still had
        // left. Throws std::invalid_argument on a zero ID or quantity.
        static CancellationResult cancelled(OrderId orderId, Quantity cancelledQuantity);

        // A refused cancellation: one CancelRejected. Unchecked ID, so it can echo whatever
        // was asked for, including zero. Throws std::invalid_argument if the reason is NONE.
        static CancellationResult rejected(OrderId orderId, RejectionReason reason);

        bool isSuccess() const;
        std::optional<RejectionReason> getRejectionReason() const;
        const CancellationEvents& getEvents() const;
    };
}  // namespace order_book
