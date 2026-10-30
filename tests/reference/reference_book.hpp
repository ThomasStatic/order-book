#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/fill_status.hpp"
#include "order_book/order_request.hpp"
#include "order_book/rejection_reason.hpp"
#include "order_book/side.hpp"
#include "order_book/types.hpp"

// An independent model of the order book, for the randomized harness and the benchmark
// workloads. It is written separately from the engine and never touches an OrderBook, so a
// misreading of the rules in one need not be shared by the other.
namespace order_book::reference {

// A set of IDs that can also be indexed, so a generator can pick one uniformly. Its order
// depends only on the inserts and erases made, so it is the same on every run.
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
    OwnerId owner = 0;
};

// Everything a command should report: whether it succeeded, why not, its exact events, and
// for a submission what was left of it and its fill status (at the end of its own match).
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

// One step of a match plan, with one resting order (the maker), at its price: a trade of
// `quantity`, or a prevented self-trade that removes the maker whole (`quantity` is what it had
// open) or, under CANCEL_INCOMING, leaves it (`quantity` 0).
struct PlanStep {
    OrderId maker;
    Price price;
    Quantity quantity;
    bool prevented = false;
};

// What an incoming order would do against the book as it stands, worked out without changing
// it: each step in execution order, how much of the order they fill, and whether self-trade
// prevention stops it (its remainder then expires, even on a GTC order).
struct MatchPlan {
    std::vector<PlanStep> steps;
    Quantity filled = 0;
    bool stopped = false;
};

// What the model has seen so far, counted from the outcomes it predicted.
struct ModelCounters {
    std::size_t submissionsAccepted = 0;
    std::size_t submissionsRejected = 0;
    std::size_t trades = 0;
    std::size_t expired = 0;
    std::size_t preventions = 0;
    std::size_t stopsTriggered = 0;
    std::size_t cancelled = 0;
    std::size_t cancelsRejected = 0;
};

// A deliberately plain model of the book: validation in the documented order, then
// price-time matching at the resting price, worked out as a match plan that walks the
// opposing queues order by order (never a level's aggregate, so a bug in the engine's
// shortcuts shows) and then applied. Each trade triggers the pending stops it reaches, which
// run after the command's own order, breadth first. It predicts each command's exact outcome
// and tracks which IDs rest, which pend, and which have been used, which is the view a
// generator draws from. Starts empty.
class ReferenceBook {
public:
    // Oldest first.
    using Queue = std::list<OrderId>;

    // submitOrder(OrderRequest): the request's checks in their fixed order, then a live ID
    // (resting or pending). An IOC remainder expires rather than rests; a FOK applies its plan
    // only if the plan fills it, and otherwise discards it and expires whole; a post-only
    // order that would take expires whole; a stop pends, or expires whole if already past its
    // trigger.
    ExpectedOutcome submit(const OrderRequest& request);
    // submitOrder(Order), for an Order with these fields, marked FILLED if markedFilled: the
    // Order's own checks (which accept only a type that can rest), then FILLED, then a live ID.
    ExpectedOutcome submitOrder(const OrderRequest& fields, bool markedFilled);
    // Cancels a resting order or a pending stop.
    ExpectedOutcome cancel(OrderId orderId);

    // The plan for an incoming order on `side` with this limit price (0 for none, a market
    // order), quantity, owner (0 for none), and self-trade prevention mode. Changes nothing.
    MatchPlan plan(Side side, Price limitPrice, Quantity quantity, OwnerId owner = 0,
                   SelfTradePrevention mode = SelfTradePrevention::CANCEL_RESTING) const;

    const IdPool& restingIds() const { return resting; }
    const IdPool& pendingIds() const { return pending; }
    // IDs accepted at some point that are live no longer: filled, cancelled, expired, or a
    // stop that triggered and did not rest.
    const IdPool& removedIds() const { return removed; }

    const ModelOrder* find(OrderId orderId) const;
    // The request a pending stop was accepted with, or nothing.
    const OrderRequest* findPendingStop(OrderId orderId) const;
    std::optional<Price> best(Side side) const;
    // The price of the most recent trade, or nothing before the first.
    std::optional<Price> lastTradePrice() const { return lastTrade; }
    const ModelCounters& counters() const { return seen; }
    std::size_t restingCount() const { return orders.size(); }
    std::size_t pendingCount() const { return stops.size(); }
    std::size_t levelCount() const { return bids.size() + asks.size(); }
    // Best price first.
    std::vector<ModelLevel> levels(Side side) const;
    // The same, without copying: each level's queue of IDs, read with find.
    const std::map<Price, Queue, std::greater<Price>>& bidQueues() const { return bids; }
    const std::map<Price, Queue, std::less<Price>>& askQueues() const { return asks; }

private:
    struct Entry {
        ModelOrder order;
        Queue::iterator position;  // in its level's queue
    };
    struct StopEntry {
        OrderRequest request;
        Queue::iterator position;  // in its stop level's queue
    };
    struct Triggered {
        OrderRequest request;
        Price tradePrice;
    };

    std::optional<RejectionReason> requestRejection(const OrderRequest& request) const;
    ExpectedOutcome reject(OrderId orderId, Quantity quantity, RejectionReason reason);
    ExpectedOutcome expireWhole(const OrderRequest& request);
    // Runs an incoming order (the submitted one, or a triggered stop as its market or limit
    // order) to the end of its own lifecycle, appending its events; returns how much it filled.
    Quantity runOrder(const OrderRequest& order, std::vector<BookEvent>& events);
    void runCascade(std::vector<BookEvent>& events);
    void applyPlan(const MatchPlan& plan, OrderId incomingId, std::vector<BookEvent>& events);
    void triggerAt(Price tradePrice);
    void rest(const ModelOrder& order);
    void pend(const OrderRequest& stop);
    // Takes a resting order off its level and forgets it.
    void remove(OrderId orderId);
    void unpend(OrderId orderId);
    template <typename Levels>
    void planAgainst(const Levels& levels, Side side, Price limitPrice, Quantity quantity, OwnerId owner,
                     SelfTradePrevention mode, MatchPlan& plan) const;
    template <typename Levels>
    std::vector<ModelLevel> describe(const Levels& levels) const;

    std::unordered_map<OrderId, Entry> orders;
    std::map<Price, Queue, std::greater<Price>> bids;
    std::map<Price, Queue, std::less<Price>> asks;
    std::unordered_map<OrderId, StopEntry> stops;
    std::map<Price, Queue, std::less<Price>> buyStops;     // nearest to triggering first
    std::map<Price, Queue, std::greater<Price>> sellStops;
    std::deque<Triggered> triggered;                       // during a command only
    IdPool resting;
    IdPool pending;
    IdPool removed;
    std::optional<Price> lastTrade;
    ModelCounters seen;
};

}  // namespace order_book::reference
