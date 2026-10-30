#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <list>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"

// Fault injection: every allocation in this executable goes through this operator new,
// which can be armed to throw std::bad_alloc on the Nth allocation from now.
namespace {
long allocationsBeforeFailure = -1;  // negative = disarmed
long allocationCount = 0;            // every allocation, failed or not, for tests that count them

// MSVC's checked iterators allocate bookkeeping proxies inside noexcept container members
// (e.g. vector's default constructor), so failing one calls std::terminate. The test only
// runs without them, e.g. in a Release build; otherwise CTest reports it as skipped.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool kIteratorDebugging = true;
#else
constexpr bool kIteratorDebugging = false;
#endif
constexpr int kSkippedExitCode = 77;  // matches SKIP_RETURN_CODE in CMakeLists.txt
}

void* operator new(std::size_t size) {
    ++allocationCount;
    if (allocationsBeforeFailure == 0) {
        allocationsBeforeFailure = -1;
        throw std::bad_alloc();
    }
    if (allocationsBeforeFailure > 0) {
        --allocationsBeforeFailure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can inspect internals.
struct OrderBookTestAccess {
    static ConsumptionResult consumeLevel(OrderBook& book, Side side, Price price, Quantity quantity) {
        return book.consumeLevel(side, price, quantity);
    }
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }

    static const order_book::OrderQueue& orders(const PriceLevel& level) { return level.orders; }
    static bool noneTriggered(const OrderBook& book) { return !book.triggeredStops || book.triggeredStops->empty(); }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::Order;
using order_book::OrderBook;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// Same invariants as execution_test: one index entry per resting order, pointing at it,
// no leftover entries, no empty levels, and each level's aggregate matching its orders.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side,
                          std::size_t& liveOrders, const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        unsigned int remainingSum = 0;
        for (const Order& order : Access::orders(level)) {
            ++liveOrders;
            remainingSum += order.getRemainingQuantity();
            auto indexItr = Access::index(book).find(order.getOrderId());
            expect(indexItr != Access::index(book).end(), context + ": every resting order should be indexed");
            expect(&*indexItr->second.itr == &order, context + ": an index entry should point at its order");
            expect(indexItr->second.tickPrice == price && indexItr->second.side == side,
                   context + ": an index entry should record the level price and side");
        }
        expect(level.getAggregateQuantity() == remainingSum,
               context + ": a level's aggregate quantity should equal its orders' remaining quantities");
    }
}

void expectIndexConsistent(const OrderBook& book, const std::string& context) {
    std::size_t liveOrders = 0;
    expectSideConsistent(book, Access::bids(book), Side::BUY, liveOrders, context);
    expectSideConsistent(book, Access::asks(book), Side::SELL, liveOrders, context);
    expect(Access::index(book).size() == liveOrders,
           context + ": the index should hold exactly one entry per live resting order");
    // The library's checker should agree with these hand-written checks.
    const order_book::InvariantResult invariants = book.checkInvariants();
    expect(invariants.isValid(),
           context + ": the book should pass its invariant check, got " + order_book::formatInvariantResult(invariants));
}

// Runs `operation` on a freshly set-up book with its Nth allocation failing, for
// N = 0, 1, 2, ... until it completes; `afterFailure` checks the book after each throw.
// Returns how many failures were injected, so callers can confirm the test is not vacuous.
template <typename Setup, typename Operation, typename AfterFailure>
long failEachAllocationInTurn(Setup setup, Operation operation, AfterFailure afterFailure,
                              const std::string& context) {
    for (long failAt = 0; failAt < 1000; ++failAt) {
        OrderBook book{};
        setup(book);

        bool threw = false;
        allocationsBeforeFailure = failAt;
        try {
            operation(book);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        allocationsBeforeFailure = -1;

        const std::string attempt = context + " (allocation " + std::to_string(failAt) + " failed)";
        expectIndexConsistent(book, attempt);
        if (!threw) {
            return failAt;
        }
        afterFailure(book, attempt);
    }
    expect(false, context + ": the operation never completed");
    return 0;
}

void testAddOrderToNewLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [](OrderBook& book) { book.addOrder(Order(2, 101, 5, 2, Side::SELL)); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).size() == 1, context + ": a failed add should not leave a new level behind");
            expect(Access::index(book).size() == 1, context + ": a failed add should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 2, context + ": a failed add should not consume a sequence number");
        },
        "addOrder (new level)");
    expect(failures > 0, "addOrder (new level): at least one allocation failure should have been injected");
}

// On an empty book the first order also needs its queue's first block: the book's node pool
// takes a chunk from the heap, so this is where a queue node's allocation can fail. (On a
// book that already holds an order the pool has blocks to spare, and only the level and
// index allocations can.)
void testFirstOrderOnEmptyBookRollsBack() {
    const auto check = [](const OrderBook& book, const std::string& context) {
        expect(Access::bids(book).empty() && Access::asks(book).empty() && Access::index(book).empty(),
               context + ": a failed first order should leave the book empty");
    };
    const long added = failEachAllocationInTurn(
        [](OrderBook&) {},
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [check](const OrderBook& book, const std::string& context) {
            check(book, context);
            expect(Access::nextSequenceNum(book) == 1, context + ": a failed add should not consume a sequence number");
        },
        "addOrder (empty book)");
    const long submitted = failEachAllocationInTurn(
        [](OrderBook&) {},
        [](OrderBook& book) { book.submitOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [check](const OrderBook& book, const std::string& context) { check(book, context); },
        "submitOrder (empty book)");
    expect(added >= 3 && submitted >= 3,
           "an empty book's first order should fail at its level, its queue's chunk, and its index entry");
}

// The index holds twelve entries in sixteen slots, so the thirteenth order grows it: joining
// the existing level then allocates only the index's new table, and if that fails the order
// is unqueued again and the level left as it was.
void testIndexGrowthOnExistingLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int id = 1; id <= 12; ++id) {
                book.addOrder(Order(id, 100, 5, 1, Side::SELL));
            }
        },
        [](OrderBook& book) { book.addOrder(Order(13, 100, 7, 1, Side::SELL)); },
        [](const OrderBook& book, const std::string& context) {
            const auto& level = Access::asks(book).at(100);
            expect(level.getOrderCount() == 12 && level.getAggregateQuantity() == 60,
                   context + ": a failed add should leave the existing level as it was");
            expect(Access::index(book).size() == 12 && Access::index(book).count(13) == 0,
                   context + ": a failed add should not leave an index entry");
        },
        "addOrder (existing level, index grows)");
    expect(failures == 1, "addOrder (existing level, index grows): only the index's growth should allocate");
}

void testAddOrderToExistingLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [](OrderBook& book) { book.addOrder(Order(2, 100, 7, 2, Side::SELL)); },
        [](const OrderBook& book, const std::string& context) {
            const auto& level = Access::asks(book).at(100);
            expect(level.getOrderCount() == 1 && level.getAggregateQuantity() == 5,
                   context + ": a failed add should leave the existing level as it was");
            expect(Access::index(book).size() == 1, context + ": a failed add should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 2, context + ": a failed add should not consume a sequence number");
        },
        "addOrder (existing level)");
    // The level exists, its queue's pool has blocks to spare, and the index has room, so
    // joining it allocates nothing at all: the operation completes before any failure fires.
    expect(failures == 0, "addOrder (existing level): joining a level with room should allocate nothing");
}

// A submission that does not cross rests whole without matching. Whatever allocation fails,
// the book is left as it was: no index entry, no order, no new or emptied level. Its sequence
// number is consumed, since submitOrder numbers an order as soon as it is accepted and every
// allocation comes after that.
void expectRestingSubmitRollsBack(Order incoming, bool allocates, const std::string& context) {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [incoming](OrderBook& book) { book.submitOrder(incoming); },
        [](const OrderBook& book, const std::string& attempt) {
            const auto& asks = Access::asks(book);
            expect(asks.size() == 1 && asks.at(100).getOrderCount() == 1 && asks.at(100).getAggregateQuantity() == 5,
                   attempt + ": a failed resting submission should leave the asks as they were");
            expect(Access::bids(book).empty(), attempt + ": a failed resting submission should not leave a bid level");
            expect(Access::index(book).size() == 1 && Access::index(book).count(2) == 0,
                   attempt + ": a failed resting submission should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 3,
                   attempt + ": a resting submission accepted before failing should still have consumed its number");
        },
        context);
    // Joining an existing level allocates nothing (see testAddOrderToExistingLevelRollsBack);
    // a new level does.
    expect(allocates ? failures > 0 : failures == 0,
           context + (allocates ? ": at least one allocation failure should have been injected"
                                : ": joining a level with room should allocate nothing"));
}

void testRestingSubmitRollsBack() {
    expectRestingSubmitRollsBack(Order(2, 100, 5, 9, Side::SELL), false, "submitOrder resting (existing level)");
    expectRestingSubmitRollsBack(Order(2, 99, 5, 9, Side::BUY), true, "submitOrder resting (new level)");
}

void testSubmitOrderStaysConsistent() {
    // Sweeps two asks, then rests the remainder as a new bid level.
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 2, 1, Side::SELL));
            book.addOrder(Order(3, 101, 2, 2, Side::SELL));
        },
        [](OrderBook& book) { book.submitOrder(Order(2, 101, 6, 3, Side::BUY)); },
        [](const OrderBook& book, const std::string& context) {
            // Executions before the failure may stand, but the remainder must not be half-rested.
            expect(!book.hasBids() && Access::index(book).find(2) == Access::index(book).end(),
                   context + ": a failed submission should not rest the incoming order");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 2 - executions,
                   context + ": every execution made should have filled exactly one resting order");
            expect(Access::nextSequenceNum(book) == 4,
                   context + ": a submission accepted before failing should still have consumed its number");
        },
        "submitOrder");
    expect(failures > 0, "submitOrder: at least one allocation failure should have been injected");
}

// A sweep of six trades outgrows the result's inline storage: the trades vector grows after
// four executions already stand (matchOrder makes room before each execution, so a failure
// there happens before the fifth), and the seven events spill to the heap when the result is
// built. Whatever fails, every execution made is a filled resting order, and the remainder
// never rests.
void testLongSweepStaysConsistent() {
    std::vector<long> injected;
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 6; ++i) {
                book.addOrder(Order(10 + i, 100 + i, 1, 1, Side::SELL));
            }
        },
        [](OrderBook& book) { book.submitOrder(Order(2, 106, 7, 1, Side::BUY)); },
        [&injected](const OrderBook& book, const std::string& context) {
            expect(!book.hasBids() && Access::index(book).count(2) == 0,
                   context + ": a failed sweep should not rest the incoming order");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 6 - executions && Access::asks(book).size() == 6 - executions,
                   context + ": every execution made should have filled exactly one resting order and its level");
            expect(Access::nextSequenceNum(book) == 8,
                   context + ": a submission accepted before failing should still have consumed its number");
            injected.push_back(static_cast<long>(executions));
        },
        "submitOrder (six-trade sweep)");
    expect(failures > 0, "submitOrder (six-trade sweep): at least one allocation failure should have been injected");
    expect(!injected.empty() && injected.front() == 4,
           "the first allocation of a six-trade sweep should be the trades vector growing after four executions");
}

// An IOC sweep of six trades outgrows the result's inline storage, then expires its remainder.
// v1's contract holds: whatever fails, every execution made stands, and nothing rests.
void testImmediateOrCancelSweepStaysConsistent() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 6; ++i) {
                book.addOrder(Order(10 + i, 100 + i, 1, 1, Side::SELL));
            }
        },
        [](OrderBook& book) {
            book.submitOrder(order_book::OrderRequest{2, Side::BUY, 106, 9, order_book::OrderType::LIMIT,
                                                      order_book::TimeInForce::IOC});
        },
        [](const OrderBook& book, const std::string& context) {
            expect(!book.hasBids() && Access::index(book).count(2) == 0,
                   context + ": an IOC should never rest, even when it fails");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 6 - executions && Access::asks(book).size() == 6 - executions,
                   context + ": every execution made should have filled exactly one resting order and its level");
            expect(Access::nextSequenceNum(book) == 8,
                   context + ": an IOC accepted before failing should still have consumed its number");
        },
        "IOC submitOrder (six-trade sweep)");
    expect(failures > 0, "IOC (six-trade sweep): at least one allocation failure should have been injected");
}

// A fillable FOK of six trades outgrows the result's inline storage. It is strongly atomic: it
// makes room for every trade and event before it is numbered or anything trades, so whatever
// allocation fails, nothing changes, counters included.
void testFillOrKillIsAllOrNothing() {
    const auto setup = [](OrderBook& book) {
        for (unsigned int i = 0; i < 6; ++i) {
            book.addOrder(Order(10 + i, 100 + i, 2, 1, Side::SELL));
        }
    };
    OrderBook pristine;
    setup(pristine);
    const order_book::SequenceNum sequence = Access::nextSequenceNum(pristine);
    const long failures = failEachAllocationInTurn(
        setup,
        [](OrderBook& book) {
            book.submitOrder(order_book::OrderRequest{2, Side::BUY, 106, 11, order_book::OrderType::LIMIT,
                                                      order_book::TimeInForce::FOK});
        },
        [sequence](const OrderBook& book, const std::string& context) {
            expect(Access::index(book).size() == 6 && Access::asks(book).size() == 6 && !book.hasBids(),
                   context + ": a failed FOK should leave every resting order in place");
            expect(Access::asks(book).at(105).getAggregateQuantity() == 2,
                   context + ": a failed FOK should fill nothing");
            expect(Access::nextSequenceNum(book) == sequence && Access::nextTradeId(book) == 1,
                   context + ": a failed FOK should consume no sequence number or trade ID");
        },
        "FOK submitOrder (six-trade fill)");
    expect(failures > 0, "FOK (six trades): at least one allocation failure should have been injected");
}

// A FOK that cannot fill is accepted then wholly expired without allocating: its two events fit
// inline, and the walk that decides it only reads.
void testUnfillableFillOrKillDoesNotAllocate() {
    OrderBook book{};
    for (unsigned int i = 0; i < 6; ++i) {
        book.addOrder(Order(10 + i, 100 + i, 2, 1, Side::SELL));
    }
    allocationsBeforeFailure = 0;
    bool threw = false;
    bool expired = false;
    try {
        const order_book::SubmissionResult result = book.submitOrder(order_book::OrderRequest{
            2, Side::BUY, 106, 13, order_book::OrderType::LIMIT, order_book::TimeInForce::FOK});
        expired = result.getStatus() == order_book::SubmissionStatus::ACCEPTED && result.getGeneratedTrades().empty();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    const bool stillArmed = allocationsBeforeFailure == 0;
    allocationsBeforeFailure = -1;
    expect(!threw && stillArmed && expired, "an unfillable FOK should expire without allocating");
    expectIndexConsistent(book, "unfillable FOK without allocating");
}

// A market sweep of six trades past the result's inline storage keeps v1's contract: every
// execution made stands, and nothing rests.
void testMarketSweepStaysConsistent() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 6; ++i) {
                book.addOrder(Order(10 + i, 100 + 50 * i, 1, 1, Side::BUY));
            }
        },
        [](OrderBook& book) {
            book.submitOrder(order_book::OrderRequest{2, Side::SELL, 0, 9, order_book::OrderType::MARKET,
                                                      order_book::TimeInForce::IOC});
        },
        [](const OrderBook& book, const std::string& context) {
            expect(!book.hasAsks() && Access::index(book).count(2) == 0, context + ": a market order should never rest");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 6 - executions && Access::bids(book).size() == 6 - executions,
                   context + ": every execution made should have filled exactly one resting order and its level");
        },
        "market submitOrder (six-trade sweep)");
    expect(failures > 0, "market (six-trade sweep): at least one allocation failure should have been injected");
}

// A post-only order that rests is strongly atomic and numbered last: whatever allocation fails
// (its level, its queue node, the index), the book and its counters are as they were. Only a
// new level, or the first order of a book, allocates.
void expectPostOnlyRestRollsBack(bool seeded, const std::string& context) {
    const auto setup = [seeded](OrderBook& book) {
        if (seeded) {
            book.addOrder(Order(1, 100, 5, 1, Side::BUY));
            book.addOrder(Order(3, 102, 5, 1, Side::SELL));
        }
    };
    const std::size_t resting = seeded ? 2 : 0;
    const long failures = failEachAllocationInTurn(
        setup,
        [](OrderBook& book) {
            order_book::OrderRequest request{2, Side::BUY, 101, 5};
            request.postOnly = true;
            book.submitOrder(request);
        },
        [resting](const OrderBook& book, const std::string& attempt) {
            expect(Access::index(book).size() == resting && Access::bids(book).size() == resting / 2,
                   attempt + ": a failed post-only order should not rest");
            expect(Access::nextSequenceNum(book) == resting + 1 && Access::nextTradeId(book) == 1,
                   attempt + ": a failed post-only order should consume no sequence number");
        },
        context);
    expect(failures > 0, context + ": at least one allocation failure should have been injected");
}

void testPostOnlyRestRollsBack() {
    expectPostOnlyRestRollsBack(true, "post-only resting (new level)");
    expectPostOnlyRestRollsBack(false, "post-only resting (first order of a book)");
}

// A post-only order that would take is accepted then wholly expired without allocating.
void testPostOnlyThatWouldTakeDoesNotAllocate() {
    OrderBook book{};
    book.addOrder(Order(3, 102, 5, 1, Side::SELL));
    order_book::OrderRequest request{2, Side::BUY, 102, 5};
    request.postOnly = true;
    allocationsBeforeFailure = 0;
    bool threw = false;
    try {
        book.submitOrder(request);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    const bool stillArmed = allocationsBeforeFailure == 0;
    allocationsBeforeFailure = -1;
    expect(!threw && stillArmed && book.activeOrderCount() == 1, "a post-only order that would take should not allocate");
}

order_book::OrderRequest ownedRequest(order_book::OrderId id, Side side, order_book::Price price, unsigned int quantity,
                                      order_book::OwnerId owner,
                                      order_book::TimeInForce timeInForce = order_book::TimeInForce::GTC) {
    order_book::OrderRequest request{id, side, price, quantity, order_book::OrderType::LIMIT, timeInForce};
    request.owner = owner;
    return request;
}

// Asks 100..105, one lot each, alternating between owner 7 and owner 8.
void setUpAlternatingOwners(OrderBook& book) {
    for (unsigned int i = 0; i < 6; ++i) {
        book.submitOrder(ownedRequest(10 + i, Side::SELL, 100 + i, 1, i % 2 == 0 ? 7 : 8));
    }
}

// A CANCEL_RESTING sweep of three preventions and three trades outgrows the result's inline
// storage. Whatever fails, every prevention and execution made stands, and no remainder rests.
void testCancelRestingSweepStaysConsistent() {
    const long failures = failEachAllocationInTurn(
        setUpAlternatingOwners,
        [](OrderBook& book) { book.submitOrder(ownedRequest(2, Side::BUY, 105, 9, 7)); },
        [](const OrderBook& book, const std::string& context) {
            expect(!book.hasBids() && Access::index(book).count(2) == 0,
                   context + ": a failed sweep should not rest the incoming order");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == Access::asks(book).size() && Access::index(book).size() <= 6 - executions,
                   context + ": every execution and prevention made should have removed one resting order");
        },
        "CANCEL_RESTING sweep");
    expect(failures > 0, "CANCEL_RESTING sweep: at least one allocation failure should have been injected");
}

// A FOK with three preventions and three trades reserves its whole result before anything
// changes: whatever fails, nothing does, counters included.
void testFillOrKillWithPreventionsIsAllOrNothing() {
    const long failures = failEachAllocationInTurn(
        setUpAlternatingOwners,
        [](OrderBook& book) {
            book.submitOrder(ownedRequest(2, Side::BUY, 105, 3, 7, order_book::TimeInForce::FOK));
        },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::index(book).size() == 6 && Access::nextTradeId(book) == 1 && Access::nextSequenceNum(book) == 7,
                   context + ": a failed FOK should change nothing, counters included");
        },
        "FOK with preventions");
    expect(failures > 0, "FOK with preventions: at least one allocation failure should have been injected");
}

order_book::OrderRequest stopRequest(order_book::OrderId id, Side side, order_book::Price stopPrice) {
    order_book::OrderRequest request{id, side, 0, 5, order_book::OrderType::STOP, order_book::TimeInForce::IOC};
    request.stopPrice = stopPrice;
    return request;
}

// Storing a pending stop is strongly atomic and numbered last: whatever allocation fails (the
// stop queue's node, the stop level's map node, the index), the book, the next sequence number,
// and the pending count are as they were.
template <typename Setup>
void expectStopStorageRollsBack(Setup setup, const order_book::OrderRequest& request, const std::string& context) {
    OrderBook pristine;
    setup(pristine);
    const std::size_t pending = pristine.pendingStopCount();
    const order_book::SequenceNum sequence = Access::nextSequenceNum(pristine);
    const long failures = failEachAllocationInTurn(
        setup, [&request](OrderBook& book) { book.submitOrder(request); },
        [&](const OrderBook& book, const std::string& attempt) {
            expect(book.pendingStopCount() == pending && !book.findPendingStop(request.id),
                   attempt + ": a failed stop should not pend");
            expect(Access::nextSequenceNum(book) == sequence && Access::nextTradeId(book) == 1,
                   attempt + ": a failed stop should consume no sequence number");
        },
        context);
    expect(failures > 0, context + ": at least one allocation failure should have been injected");
}

void testStopStorageRollsBack() {
    expectStopStorageRollsBack([](OrderBook&) {}, stopRequest(50, Side::BUY, 110), "first stop on an empty book");
    expectStopStorageRollsBack([](OrderBook& book) { book.submitOrder(stopRequest(40, Side::BUY, 110)); },
                               stopRequest(50, Side::BUY, 111), "stop at a new stop price");
    // The index holds 12 entries before it grows from 16 slots.
    expectStopStorageRollsBack(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 12; ++i) {
                book.submitOrder(stopRequest(30 + i, Side::SELL, 90));
            }
        },
        stopRequest(50, Side::SELL, 90), "stop that grows the index");
}

// Paths that never allocate: cancelling a pending stop, a stop that arrives past its trigger,
// and (served from the pool's size class for stop nodes) a second stop at a stop price.
void testStopPathsDoNotAllocate() {
    OrderBook book{};
    book.submitOrder(stopRequest(40, Side::BUY, 110));
    book.submitOrder(stopRequest(41, Side::BUY, 111));
    book.addOrder(Order(1, 100, 1, 1, Side::SELL));
    book.submitOrder(order_book::OrderRequest{2, Side::BUY, 100, 1});  // last trade at 100
    const auto armed = [&book](const auto& operation, const std::string& what) {
        allocationsBeforeFailure = 0;
        bool threw = false;
        try {
            operation();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool stillArmed = allocationsBeforeFailure == 0;
        allocationsBeforeFailure = -1;
        expect(!threw && stillArmed, what + " should not allocate");
        expectIndexConsistent(book, what);
    };
    bool cancelled = false;
    armed([&book, &cancelled] { cancelled = book.cancelOrder(41).isSuccess(); }, "cancelling a pending stop");
    expect(cancelled, "the stop should cancel");
    armed([&book] { book.submitOrder(stopRequest(42, Side::BUY, 99)); }, "a stop past its trigger");
    armed([&book] { book.submitOrder(stopRequest(43, Side::BUY, 110)); }, "a second stop at a stop price");
    expect(book.pendingStopCount() == 2, "two stops should pend");
}

// Every v2 submission rejection allocates nothing: each request check's refusal, and a
// duplicate of a resting order's ID and of a pending stop's. The rejection holds its one
// event inline, and validation reads the book without changing it.
void testRequestRejectionsDoNotAllocate() {
    using order_book::OrderRequest;
    using order_book::OrderType;
    using order_book::RejectionReason;
    using order_book::SelfTradePrevention;
    using order_book::TimeInForce;
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.submitOrder(stopRequest(2, Side::BUY, 110));

    struct Case {
        const char* name;
        OrderRequest request;
        RejectionReason reason;
    };
    const auto with = [](OrderRequest request, auto change) {
        change(request);
        return request;
    };
    const OrderRequest limit{10, Side::BUY, 90, 5};
    const std::vector<Case> cases{
        {"a zero ID", with(limit, [](OrderRequest& r) { r.id = 0; }), RejectionReason::INVALID_ORDER_ID},
        {"a missing price", with(limit, [](OrderRequest& r) { r.price = 0; }), RejectionReason::INVALID_PRICE},
        {"a zero quantity", with(limit, [](OrderRequest& r) { r.quantity = 0; }), RejectionReason::INVALID_QUANTITY},
        {"an invalid side", with(limit, [](OrderRequest& r) { r.side = static_cast<Side>(7); }), RejectionReason::INVALID_SIDE},
        {"an invalid type", with(limit, [](OrderRequest& r) { r.type = static_cast<OrderType>(9); }),
         RejectionReason::UNSUPPORTED_ORDER_TYPE},
        {"a market order with a price", with(limit, [](OrderRequest& r) {
             r.type = OrderType::MARKET;
             r.timeInForce = TimeInForce::IOC;
         }), RejectionReason::PRICE_NOT_ALLOWED},
        {"an invalid time in force", with(limit, [](OrderRequest& r) { r.timeInForce = static_cast<TimeInForce>(9); }),
         RejectionReason::INVALID_TIME_IN_FORCE},
        {"a GTC market order", with(limit, [](OrderRequest& r) {
             r.type = OrderType::MARKET;
             r.price = 0;
         }), RejectionReason::TIME_IN_FORCE_NOT_ALLOWED},
        {"a stop with no stop price", with(limit, [](OrderRequest& r) { r.type = OrderType::STOP_LIMIT; }),
         RejectionReason::INVALID_STOP_PRICE},
        {"a stop price on a limit order", with(limit, [](OrderRequest& r) { r.stopPrice = 95; }),
         RejectionReason::STOP_PRICE_NOT_ALLOWED},
        {"post-only on an IOC", with(limit, [](OrderRequest& r) {
             r.postOnly = true;
             r.timeInForce = TimeInForce::IOC;
         }), RejectionReason::POST_ONLY_NOT_ALLOWED},
        {"an invalid self-trade prevention mode",
         with(limit, [](OrderRequest& r) { r.selfTradePrevention = static_cast<SelfTradePrevention>(9); }),
         RejectionReason::INVALID_SELF_TRADE_PREVENTION},
        {"a resting order's ID", with(limit, [](OrderRequest& r) { r.id = 1; }), RejectionReason::DUPLICATE_ORDER_ID},
        {"a pending stop's ID", with(limit, [](OrderRequest& r) { r.id = 2; }), RejectionReason::DUPLICATE_ORDER_ID},
    };
    for (const Case& testCase : cases) {
        std::optional<RejectionReason> reason;
        allocationsBeforeFailure = 0;
        bool threw = false;
        try {
            const order_book::SubmissionResult result = book.submitOrder(testCase.request);
            reason = result.getStatus() == order_book::SubmissionStatus::REJECTED ? std::optional(result.getRejectionReason())
                                                                                  : std::nullopt;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool stillArmed = allocationsBeforeFailure == 0;
        allocationsBeforeFailure = -1;
        expect(!threw && stillArmed, std::string("rejecting ") + testCase.name + " should not allocate");
        expect(reason == testCase.reason, std::string("rejecting ") + testCase.name + " should give its reason");
    }
    expect(book.activeOrderCount() == 1 && book.pendingStopCount() == 1, "no rejection should change the book");
    expectIndexConsistent(book, "after the rejections");
}

// A cascade of three stops that fails inside the second stop's run: the submitter has
// finished, the first stop's trade stands, the second keeps a market order's contract (its
// trades made stand), and the third, triggered by the second, is discarded: nowhere, and
// nothing pends past its trigger.
void testCascadeFailureDiscardsTriggeredStops() {
    std::vector<long> secondStopTrades;
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 8; ++i) {
                book.addOrder(Order(10 + i, 100 + i, 1, 1, Side::SELL));  // asks 100..107
            }
            order_book::OrderRequest first = stopRequest(31, Side::BUY, 100);  // runs first: buys at 101
            first.quantity = 1;
            order_book::OrderRequest second = stopRequest(32, Side::BUY, 101);  // sweeps 102..106
            order_book::OrderRequest third = stopRequest(33, Side::BUY, 102);   // triggered by the second
            third.quantity = 1;
            book.submitOrder(first);
            book.submitOrder(second);
            book.submitOrder(third);
        },
        [](OrderBook& book) { book.submitOrder(order_book::OrderRequest{2, Side::BUY, 100, 1}); },
        [&secondStopTrades](const OrderBook& book, const std::string& context) {
            expect(Access::noneTriggered(book), context + ": triggered stops should be discarded");
            expect(book.pendingStopCount() == 0 && !book.findPendingStop(33) && Access::index(book).count(33) == 0,
                   context + ": the third stop should be nowhere");
            expect(Access::index(book).count(10) == 0 && Access::index(book).count(11) == 0,
                   context + ": the submitter's and the first stop's trades should stand");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(executions >= 2 && Access::index(book).size() == 8 - executions,
                   context + ": every trade made should have filled one ask");
            secondStopTrades.push_back(static_cast<long>(executions) - 2);
        },
        "three-stop cascade");
    expect(failures > 0, "three-stop cascade: at least one allocation failure should have been injected");
    expect(!secondStopTrades.empty() && secondStopTrades.front() >= 1,
           "the first failure should come inside the second stop's run, after its first trade");
}

// Replace paths that never allocate: a refused replace, one that keeps priority, and a
// post-only one that would take.
void testReplacePathsDoNotAllocate() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.addOrder(Order(2, 100, 5, 1, Side::SELL));
    book.addOrder(Order(3, 99, 5, 1, Side::BUY));
    const auto armed = [&book](const auto& operation, const std::string& what) {
        allocationsBeforeFailure = 0;
        bool threw = false;
        bool succeeded = false;
        try {
            succeeded = operation();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool stillArmed = allocationsBeforeFailure == 0;
        allocationsBeforeFailure = -1;
        expect(!threw && stillArmed, what + " should not allocate");
        return succeeded;
    };
    const bool refused = armed([&book] { return !book.replaceOrder(order_book::ReplaceRequest{9, 100, 1}).isSuccess(); },
                               "a refused replace");
    const bool kept = armed([&book] { return book.replaceOrder(order_book::ReplaceRequest{2, 100, 3}).isSuccess(); },
                            "a priority-keeping replace");
    const bool taker = armed([&book] { return book.replaceOrder(order_book::ReplaceRequest{1, 99, 5, true}).isSuccess(); },
                             "a post-only replace that would take");
    expect(refused && kept && taker, "the replaces should be refused, keep priority, and expire");
    expectIndexConsistent(book, "replaces without allocating");
}

// A priority-losing replace to a new level is strongly atomic: a failure (the new level's map
// node) leaves the book, its counters included, as it was.
void testReplaceToNewLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 5, 1, Side::SELL));
            book.addOrder(Order(2, 100, 5, 1, Side::SELL));
        },
        [](OrderBook& book) { book.replaceOrder(order_book::ReplaceRequest{1, 105, 5}); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).size() == 1 && Access::asks(book).at(100).getOrderCount() == 2 &&
                       Access::nextSequenceNum(book) == 3,
                   context + ": a failed replace should leave the order where it was");
        },
        "replace to a new level");
    expect(failures > 0, "replace to a new level: at least one allocation failure should have been injected");
}

// A crossing replace keeps v1's contract: what was made stands, and no remainder rests behind
// a throw.
void testCrossingReplaceStaysConsistent() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 6; ++i) {
                book.addOrder(Order(10 + i, 100 + i, 1, 1, Side::SELL));
            }
            book.addOrder(Order(2, 90, 9, 1, Side::BUY));
        },
        [](OrderBook& book) { book.replaceOrder(order_book::ReplaceRequest{2, 106, 9}); },
        [](const OrderBook& book, const std::string& context) {
            expect(!book.hasBids() && Access::index(book).count(2) == 0,
                   context + ": a failed crossing replace should leave the order off the book");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::asks(book).size() == 6 - executions, context + ": every execution made should stand");
        },
        "crossing replace");
    expect(failures > 0, "crossing replace: at least one allocation failure should have been injected");
}

void testConsumeLevelFailsBeforeFilling() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 5, 1, Side::SELL));
            book.addOrder(Order(2, 100, 5, 2, Side::SELL));
        },
        [](OrderBook& book) { Access::consumeLevel(book, Side::SELL, 100, 7); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).at(100).getAggregateQuantity() == 10 && Access::index(book).size() == 2,
                   context + ": consumeLevel should only fail before any order is filled");
        },
        "consumeLevel");
    expect(failures > 0, "consumeLevel: at least one allocation failure should have been injected");
}

void testCancelOrderDoesNotAllocate() {
    // A cancellation's result holds its one event inline, and removing an order only frees,
    // so cancelOrder allocates nothing: with the very first allocation armed to fail, both a
    // successful and a refused cancellation complete, and the failure is never triggered.
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.addOrder(Order(2, 100, 5, 2, Side::SELL));

    allocationsBeforeFailure = 0;
    bool threw = false;
    bool cancelled = false;
    bool refused = false;
    try {
        cancelled = book.cancelOrder(1).isSuccess();
        refused = !book.cancelOrder(99).isSuccess();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    const bool stillArmed = allocationsBeforeFailure == 0;
    allocationsBeforeFailure = -1;

    expect(!threw && stillArmed, "cancelOrder should not allocate");
    expect(cancelled && refused, "cancelOrder should cancel a resting order and refuse an unknown one");
    expect(Access::asks(book).at(100).getAggregateQuantity() == 5 && Access::index(book).size() == 1,
           "the cancelled order should be gone and the other left resting");
    expectIndexConsistent(book, "cancelOrder without allocating");
}

// The request path costs v1 traffic nothing: a v1-shaped OrderRequest allocates exactly what
// submitOrder(Order) does for the same book and command, whether it rests at a new or an
// existing level, sweeps past the result's inline storage, or is rejected.
void testRequestAllocatesLikeOrder() {
    using order_book::OrderRequest;
    struct Case {
        const char* name;
        OrderRequest request;
    };
    const Case cases[] = {
        {"rest at a new level", {20, Side::BUY, 90, 5}},
        {"rest at an existing level", {20, Side::SELL, 100, 5}},
        {"partial fill then rest", {20, Side::BUY, 100, 7}},
        {"six-trade sweep", {20, Side::BUY, 105, 60}},
        {"duplicate", {10, Side::BUY, 90, 5}},
    };
    const auto setup = [](OrderBook& book) {
        for (unsigned int i = 0; i < 6; ++i) {
            book.addOrder(Order(10 + i, 100 + i, 5, 1, Side::SELL));
        }
    };
    for (const Case& testCase : cases) {
        OrderBook viaOrder;
        OrderBook viaRequest;
        setup(viaOrder);
        setup(viaRequest);
        const OrderRequest& request = testCase.request;
        const Order incoming(request.id, request.price, request.quantity, 1, request.side);

        long before = allocationCount;
        static_cast<void>(viaOrder.submitOrder(incoming));
        const long orderAllocations = allocationCount - before;
        before = allocationCount;
        static_cast<void>(viaRequest.submitOrder(request));
        const long requestAllocations = allocationCount - before;

        expect(requestAllocations == orderAllocations,
               std::string("a v1-shaped request should allocate what submitOrder(Order) does: ") + testCase.name + " ("
                   + std::to_string(requestAllocations) + " vs " + std::to_string(orderAllocations) + ")");
    }
}

}  // namespace

int main() {
    if constexpr (kIteratorDebugging) {
        std::cout << "allocation failure tests skipped: MSVC iterator debugging is on" << std::endl;
        return kSkippedExitCode;
    } else {
        testAddOrderToNewLevelRollsBack();
        testAddOrderToExistingLevelRollsBack();
        testFirstOrderOnEmptyBookRollsBack();
        testIndexGrowthOnExistingLevelRollsBack();
        testSubmitOrderStaysConsistent();
        testLongSweepStaysConsistent();
        testRestingSubmitRollsBack();
        testImmediateOrCancelSweepStaysConsistent();
        testFillOrKillIsAllOrNothing();
        testUnfillableFillOrKillDoesNotAllocate();
        testMarketSweepStaysConsistent();
        testPostOnlyRestRollsBack();
        testPostOnlyThatWouldTakeDoesNotAllocate();
        testCancelRestingSweepStaysConsistent();
        testFillOrKillWithPreventionsIsAllOrNothing();
        testStopStorageRollsBack();
        testStopPathsDoNotAllocate();
        testCascadeFailureDiscardsTriggeredStops();
        testReplacePathsDoNotAllocate();
        testReplaceToNewLevelRollsBack();
        testCrossingReplaceStaysConsistent();
        testConsumeLevelFailsBeforeFilling();
        testCancelOrderDoesNotAllocate();
        testRequestAllocatesLikeOrder();
        testRequestRejectionsDoNotAllocate();

        std::cout << "allocation failure tests passed" << std::endl;
        return EXIT_SUCCESS;
    }
}
