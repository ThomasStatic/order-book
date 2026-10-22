#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "command.hpp"
#include "order_book/book_event.hpp"
#include "order_book/fill_status.hpp"
#include "order_book/rejection_reason.hpp"

namespace order_book::randomized {

// A set of IDs that can also be indexed, so the generator can pick one uniformly. Its
// order depends only on the inserts and erases made, so it is the same on every run.
class IdPool {
public:
    bool contains(OrderId orderId) const { return positions.find(orderId) != positions.end(); }
    bool empty() const { return ids.empty(); }
    std::size_t size() const { return ids.size(); }
    OrderId at(std::size_t position) const { return ids.at(position); }

    void insert(OrderId orderId);
    // Moves the last ID into the erased one's place.
    void erase(OrderId orderId);

private:
    std::vector<OrderId> ids;
    std::unordered_map<OrderId, std::size_t> positions;
};

struct ModelOrder {
    OrderId id;
    Side side;
    Price price;
    Quantity open;
};

// Everything a command should report: whether it succeeded, why not, its exact events, and
// for a submission what was left of it and its fill status.
struct ExpectedOutcome {
    bool success = false;
    std::optional<RejectionReason> rejection;
    std::vector<BookEvent> events;
    Quantity remaining = 0;
    FillStatus fillStatus = FillStatus::NONE;
};

struct ModelLevel {
    Price price;
    std::uint64_t totalQuantity;
    // Oldest first.
    std::vector<std::pair<OrderId, Quantity>> orders;
};

// A deliberately plain model of the book, written separately from the engine: validation in
// the documented order, then price-time matching at the resting price. It predicts each
// command's exact outcome and tracks which IDs rest and which have been used, which is the
// view the generator draws from. Assumes the book starts empty.
class ReferenceBook {
public:
    // Oldest first.
    using Queue = std::list<OrderId>;

    // Applies the command to the model and returns what the engine should report for it.
    ExpectedOutcome apply(const Command& command);

    const IdPool& restingIds() const { return resting; }
    // IDs accepted at some point that rest no longer: filled, cancelled, or filled on arrival.
    const IdPool& removedIds() const { return removed; }

    const ModelOrder* find(OrderId orderId) const;
    std::optional<Price> best(Side side) const;
    std::size_t restingCount() const { return orders.size(); }
    std::size_t levelCount() const { return bids.size() + asks.size(); }
    // Best price first.
    std::vector<ModelLevel> levels(Side side) const;
    // The same, without copying: each level's queue of IDs, read with find.
    const std::map<Price, Queue, std::greater<Price>>& bidQueues() const { return bids; }
    const std::map<Price, Queue, std::less<Price>>& askQueues() const { return asks; }

private:
    std::optional<RejectionReason> rejectionFor(const Command& command) const;
    ExpectedOutcome submit(const Command& command);
    ExpectedOutcome cancel(OrderId orderId);
    void rest(const ModelOrder& order);
    void retire(OrderId orderId);
    template <typename Levels>
    void matchAgainst(Levels& levels, const Command& incoming, Quantity& open, std::vector<BookEvent>& events);
    template <typename Levels>
    std::vector<ModelLevel> describe(const Levels& levels) const;

    std::unordered_map<OrderId, ModelOrder> orders;
    std::map<Price, Queue, std::greater<Price>> bids;
    std::map<Price, Queue, std::less<Price>> asks;
    IdPool resting;
    IdPool removed;
};

}  // namespace order_book::randomized
