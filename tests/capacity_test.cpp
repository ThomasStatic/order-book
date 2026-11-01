#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <utility>

#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"

// reserve(BookCapacity) and capacityGrowths() through the public API. Every allocation in this
// executable is counted, so a warm book can be shown to reach the heap not at all.
namespace {
long allocations = 0;
}

void* operator new(std::size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace order_book;

// Every count is read into a variable before expect is called: its message allocates.
void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// MSVC Debug's checked iterators allocate on the matching path, so only the growth count can be
// asserted there, and the heap count only where they are off.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool heapCountsAreExact = false;
#else
constexpr bool heapCountsAreExact = true;
#endif

OrderRequest restingAt(OrderId id, Side side, Price price) {
    return OrderRequest{id, side, price, 1};
}

OrderRequest stopAt(OrderId id, Side side, Price stopPrice) {
    OrderRequest request{id, side, 0, 1, OrderType::STOP, TimeInForce::IOC};
    request.stopPrice = stopPrice;
    return request;
}

constexpr Price bestBidPrice = 100000;
constexpr Price bestAskPrice = 100001;

// orders resting across levels levels per side, then stops pending across levels stop prices
// per side, all from a cold start or inside a reservation.
void fill(OrderBook& book, const BookCapacity& capacity) {
    SubmissionResult out = SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE);
    OrderId id = 1;
    for (std::size_t i = 0; i < capacity.orders; ++i) {
        const auto level = static_cast<Price>(i / 2 % capacity.levels);
        const OrderRequest request = i % 2 == 0 ? restingAt(id, Side::BUY, bestBidPrice - level)
                                                : restingAt(id, Side::SELL, bestAskPrice + level);
        book.submitOrder(request, out);
        ++id;
    }
    for (std::size_t i = 0; i < capacity.stops; ++i) {
        const auto level = static_cast<Price>(i / 2 % capacity.levels);
        const OrderRequest request = i % 2 == 0 ? stopAt(id, Side::BUY, bestAskPrice + 1000 + level)
                                                : stopAt(id, Side::SELL, bestBidPrice - 1000 - level);
        book.submitOrder(request, out);
        ++id;
    }
}

void testAColdBookCountsItsGrowths() {
    OrderBook book;
    expect(book.capacityGrowths() == 0, "a new book has not grown");
    fill(book, BookCapacity{2000, 2000, 300});
    expect(book.capacityGrowths() > 0, "a book filled from cold counts its growths");
    expect(book.activeOrderCount() == 2000 && book.pendingStopCount() == 2000, "and refuses nothing");
}

void testAReservedBookNeverGrows() {
    const BookCapacity capacity{2000, 2000, 300};
    OrderBook book;
    book.reserve(capacity);
    expect(book.capacityGrowths() == 0, "reserve itself is not a growth");

    const long before = allocations;
    fill(book, capacity);
    const long made = allocations - before;
    expect(book.capacityGrowths() == 0, "filling a book to its capacity does not grow it");
    if (heapCountsAreExact) {
        expect(made == 0, "filling a book to its capacity reaches the heap " + std::to_string(made) + " times");
    }
    expect(book.activeOrderCount() == 2000 && book.pendingStopCount() == 2000, "every order rests and every stop pends");
    expect(book.checkInvariants().isValid(), "a reserved book is a sound book");
}

void testLevelsArePerSide() {
    OrderBook book;
    book.reserve(BookCapacity{64, 0, 32});
    fill(book, BookCapacity{64, 0, 32});  // 32 levels on each side
    expect(book.capacityGrowths() == 0, "levels are counted per side");
}

void testPastCapacityGrowsAndNeverRefuses() {
    OrderBook book;
    book.reserve(BookCapacity{8, 0, 4});
    fill(book, BookCapacity{5000, 0, 500});
    expect(book.activeOrderCount() == 5000, "a book past its capacity refuses nothing");
    expect(book.capacityGrowths() > 0, "and counts what it grew");
    expect(book.checkInvariants().isValid(), "and stays sound");
}

void testReserveChangesNothingTheBookReports() {
    OrderBook book;
    fill(book, BookCapacity{20, 4, 4});
    const BookSnapshot before = book.snapshot(100);
    const std::uint64_t growths = book.capacityGrowths();

    book.reserve(BookCapacity{10000, 1000, 1000});

    const BookSnapshot after = book.snapshot(100);
    expect(after.bids.size() == before.bids.size() && after.asks.size() == before.asks.size(), "the same levels");
    expect(after.totalActiveVisibleOrders == before.totalActiveVisibleOrders, "the same orders");
    expect(book.pendingStopCount() == 4, "the same stops");
    expect(!book.getLastTradePrice(), "no trade");
    expect(book.capacityGrowths() == growths, "reserve adds no growth");
    expect(book.checkInvariants().isValid(), "a sound book");

    // The next command's numbers show that no sequence number or trade ID was consumed.
    OrderBook untouched;
    fill(untouched, BookCapacity{20, 4, 4});
    const SubmissionResult one = book.submitOrder(OrderRequest{500, Side::BUY, bestAskPrice, 3});
    const SubmissionResult other = untouched.submitOrder(OrderRequest{500, Side::BUY, bestAskPrice, 3});
    expect(one.getGeneratedTrades().size() == other.getGeneratedTrades().size(), "the same match");
    for (std::size_t i = 0; i < one.getGeneratedTrades().size(); ++i) {
        expect(one.getGeneratedTrades()[i].getTradeId() == other.getGeneratedTrades()[i].getTradeId(),
               "the same trade IDs");
    }
    expect(one.getEvents() == other.getEvents(), "the same events");
}

void testASecondCallCostsNothing() {
    const BookCapacity capacity{5000, 1000, 500};
    OrderBook book;
    book.reserve(capacity);
    const long before = allocations;
    book.reserve(capacity);
    book.reserve(BookCapacity{100, 10, 10});
    const long made = allocations - before;
    expect(made == 0, "a second reserve of no more should allocate nothing");
}

// What rests already counts, so reserving what a book holds and more adds only the difference,
// and after it the book reaches its new capacity without growing.
void testReserveTopsUpOnlyWhatIsMissing() {
    OrderBook book;
    book.reserve(BookCapacity{100, 0, 10});
    fill(book, BookCapacity{100, 0, 10});
    book.reserve(BookCapacity{4000, 0, 200});
    const std::uint64_t growths = book.capacityGrowths();
    fill(book, BookCapacity{0, 0, 1});  // a no-op that keeps the helper honest about zero counts

    SubmissionResult out = SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE);
    OrderId id = 1000;
    for (Price level = 0; level < 180; ++level) {
        book.submitOrder(restingAt(id++, Side::BUY, bestBidPrice - 20 - level), out);
    }
    while (book.activeOrderCount() < 4000) {
        book.submitOrder(restingAt(id++, Side::SELL, bestAskPrice), out);
    }
    expect(book.capacityGrowths() == growths, "a topped-up book reaches its new capacity without growing");
}

void testZeroCapacityIsANoOp() {
    OrderBook book;
    const long before = allocations;
    book.reserve(BookCapacity{});
    const long made = allocations - before;
    expect(made == 0, "reserving nothing should allocate nothing");
    book.submitOrder(restingAt(1, Side::BUY, 100));
    expect(book.activeOrderCount() == 1, "and the book still works");
}

void testMovingABookKeepsItsCapacity() {
    const BookCapacity capacity{2000, 100, 300};
    OrderBook book;
    book.reserve(capacity);
    OrderBook moved(std::move(book));
    fill(moved, capacity);
    expect(moved.capacityGrowths() == 0, "a moved book keeps what was reserved for it");

    OrderBook target;
    target = std::move(moved);
    expect(target.capacityGrowths() == 0, "move assignment takes the count along");
    expect(target.activeOrderCount() == 2000, "and the orders");
}

// A book that grew counts those growths after a move too.
void testGrowthsSurviveAMove() {
    OrderBook book;
    fill(book, BookCapacity{500, 0, 100});
    const std::uint64_t growths = book.capacityGrowths();
    expect(growths > 0, "the book grew");
    const OrderBook moved(std::move(book));
    expect(moved.capacityGrowths() == growths, "a move does not lose a book's growths");
}

// A cancel, a trade, and a level emptied and refilled all stay inside what was reserved.
void testCommandsInsideCapacityDoNotGrow() {
    const BookCapacity capacity{1000, 200, 100};
    OrderBook book;
    book.reserve(capacity);
    fill(book, capacity);
    SubmissionResult out = SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE);
    for (OrderId id = 1; id <= 500; ++id) {
        book.cancelOrder(id);
    }
    OrderId next = 5000;
    for (int i = 0; i < 500; ++i) {
        book.submitOrder(restingAt(next, i % 2 == 0 ? Side::BUY : Side::SELL,
                                   i % 2 == 0 ? bestBidPrice - static_cast<Price>(i % 50)
                                              : bestAskPrice + static_cast<Price>(i % 50)),
                         out);
        ++next;
    }
    expect(book.capacityGrowths() == 0, "commands inside the capacity do not grow the book");
}

}  // namespace

int main() {
    testAColdBookCountsItsGrowths();
    testAReservedBookNeverGrows();
    testLevelsArePerSide();
    testPastCapacityGrowsAndNeverRefuses();
    testReserveChangesNothingTheBookReports();
    testASecondCallCostsNothing();
    testReserveTopsUpOnlyWhatIsMissing();
    testZeroCapacityIsANoOp();
    testMovingABookKeepsItsCapacity();
    testGrowthsSurviveAMove();
    testCommandsInsideCapacityDoNotGrow();
    std::cout << "capacity tests passed\n";
    return EXIT_SUCCESS;
}
