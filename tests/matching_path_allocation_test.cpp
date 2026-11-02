#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "order_book/command_log.hpp"
#include "order_book/invariants.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"
#include "order_book/replace_request.hpp"
#include "workload.hpp"

// Counts every heap call on this thread, so a run judges only the thread that matches.
namespace {
thread_local std::size_t heapCalls = 0;
}

void* operator new(std::size_t size) {
    ++heapCalls;
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
namespace bench = order_book::benchmark;

constexpr int skipped = 77;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Body>
std::size_t heapCallsOf(Body body) {
    const std::size_t before = heapCalls;
    body();
    return heapCalls - before;
}

// The tests below prove nothing unless the count can move, and a book past its capacity is what
// must still reach the heap.
void testTheCountSeesAHeapCall() {
    expect(heapCallsOf([] { std::vector<std::uint64_t> grown(1000, 1); }) != 0, "a vector's block is counted");
    OrderBook book;
    book.reserve(BookCapacity{1, 1, 1});
    const std::size_t calls = heapCallsOf([&book] {
        for (OrderId id = 1; id <= 1000; ++id) {
            book.addOrder(Order(id, 100 + id, 1, 1, Side::SELL));
        }
    });
    expect(calls != 0 && book.capacityGrowths() != 0, "a book past its capacity reaches the heap, and counts it");
}

void testEveryWorkloadOnAWarmShard() {
    for (const bench::WorkloadConfig& config : bench::builtInWorkloads()) {
        const std::vector<bench::Workload> workloads =
            bench::instrumentWorkloads(config, bench::timedCommandCount(bench::BenchmarkSize::SMOKE), 42, 1);
        bench::ShardRun run(workloads);
        const std::size_t calls = heapCallsOf([&run] { run.run(); });
        expect(calls == 0, config.name + " made " + std::to_string(calls) + " heap calls on a warm shard");
        run.check();
    }
}

void testSeveralInstrumentsOnAWarmShard() {
    const std::vector<bench::Workload> workloads =
        bench::instrumentWorkloads(bench::findWorkload("quote-refresh"), bench::timedCommandCount(bench::BenchmarkSize::SMOKE), 42, 8);
    bench::ShardRun run(workloads);
    expect(heapCallsOf([&run] { run.run(); }) == 0, "books sharing a shard make no heap call between them");
    run.check();
}

OrderRequest request(OrderId id, Side side, Price price, Quantity quantity) {
    OrderRequest order{};
    order.id = id;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    return order;
}

// A book with room for everything below, and results sized for it.
struct WarmBook {
    OrderBook book;
    SubmissionResult submitted;
    ReplaceResult replaced;

    WarmBook() : submitted(SubmissionResult::rejected(1, 0, RejectionReason::INVALID_PRICE)), replaced(ReplaceResult::rejected(1, RejectionReason::ORDER_NOT_FOUND)) {
        book.reserve(BookCapacity{64, 64, 64});
        submitted.reserve(64, 128);
        replaced.reserve(64, 128);
    }
};

void testEveryTypeAndTimeInForce() {
    WarmBook warm;
    const std::size_t calls = heapCallsOf([&warm] {
        OrderId id = 1;
        for (Price price = 100; price < 110; ++price) {
            warm.book.submitOrder(request(id++, Side::SELL, price, 5), warm.submitted);
        }
        for (const TimeInForce timeInForce : {TimeInForce::GTC, TimeInForce::IOC, TimeInForce::FOK}) {
            OrderRequest limit = request(id++, Side::BUY, 100, 1);
            limit.timeInForce = timeInForce;
            warm.book.submitOrder(limit, warm.submitted);
        }
        for (const TimeInForce timeInForce : {TimeInForce::IOC, TimeInForce::FOK}) {
            OrderRequest market = request(id++, Side::BUY, 0, 1);
            market.type = OrderType::MARKET;
            market.timeInForce = timeInForce;
            warm.book.submitOrder(market, warm.submitted);
        }
        OrderRequest postOnly = request(id++, Side::BUY, 90, 1);
        postOnly.postOnly = true;
        warm.book.submitOrder(postOnly, warm.submitted);
    });
    expect(calls == 0, "a submission of any type and time in force made " + std::to_string(calls) + " heap calls");
}

void testACascade() {
    WarmBook warm;
    for (OrderId id = 1; id <= 10; ++id) {
        warm.book.submitOrder(request(id, Side::SELL, 100 + id, 2), warm.submitted);
    }
    // Each stop sits one tick past the last, so the trades of one trigger the next.
    for (OrderId id = 11; id <= 18; ++id) {
        OrderRequest stop = request(id, Side::BUY, 0, 2);
        stop.type = OrderType::STOP;
        stop.timeInForce = TimeInForce::IOC;
        stop.stopPrice = 101 + (id - 11);
        warm.book.submitOrder(stop, warm.submitted);
    }
    const std::size_t calls = heapCallsOf([&warm] { warm.book.submitOrder(request(20, Side::BUY, 101, 2), warm.submitted); });
    expect(calls == 0, "a cascade made " + std::to_string(calls) + " heap calls");
    expect(warm.book.pendingStopCount() == 0, "the cascade ran every stop");
}

void testReplacesFourPaths() {
    WarmBook warm;
    warm.book.submitOrder(request(1, Side::BUY, 100, 10), warm.submitted);
    warm.book.submitOrder(request(2, Side::SELL, 120, 10), warm.submitted);

    ReplaceRequest decrease{};
    decrease.id = 1;
    decrease.price = 100;
    decrease.quantity = 8;
    ReplaceRequest instructions = decrease;
    instructions.owner = 3;
    ReplaceRequest increase = instructions;
    increase.quantity = 12;
    ReplaceRequest price = increase;
    price.price = 101;

    expect(heapCallsOf([&] { warm.book.replaceOrder(decrease, warm.replaced); }) == 0, "a decrease");
    expect(heapCallsOf([&] { warm.book.replaceOrder(instructions, warm.replaced); }) == 0, "new instructions");
    expect(heapCallsOf([&] { warm.book.replaceOrder(increase, warm.replaced); }) == 0, "an increase");
    expect(heapCallsOf([&] { warm.book.replaceOrder(price, warm.replaced); }) == 0, "a new price");
    expect(warm.replaced.isSuccess(), "the last replace succeeded");
}

void testCancelOrder() {
    WarmBook warm;
    warm.book.submitOrder(request(1, Side::BUY, 100, 10), warm.submitted);
    OrderRequest stop = request(2, Side::SELL, 0, 1);
    stop.type = OrderType::STOP;
    stop.timeInForce = TimeInForce::IOC;
    stop.stopPrice = 50;
    warm.book.submitOrder(stop, warm.submitted);

    expect(heapCallsOf([&warm] {
               warm.book.cancelOrder(1);
               warm.book.cancelOrder(2);
               warm.book.cancelOrder(99);
           }) == 0,
           "a cancel of a resting order, a pending stop, and an unknown ID");
}

void testEncodeLogRecord() {
    const replay::Symbol symbol = *replay::Symbol::parse("AAPL");
    std::array<std::byte, replay::logRecordBytes> record{};
    replay::AddCommand add{};
    add.orderId = 1;
    add.side = Side::BUY;
    add.price = 100;
    add.quantity = 5;
    const replay::Command command = add;
    expect(heapCallsOf([&] { replay::encodeLogRecord(symbol, command, 7, std::span<std::byte, replay::logRecordBytes>(record)); }) == 0,
           "encoding a record");
}

// Unused when automatic checks are on, since main then skips.
[[maybe_unused]] void runAll() {
    testTheCountSeesAHeapCall();
    testEveryWorkloadOnAWarmShard();
    testSeveralInstrumentsOnAWarmShard();
    testEveryTypeAndTimeInForce();
    testACascade();
    testReplacesFourPaths();
    testCancelOrder();
    testEncodeLogRecord();
}

}  // namespace

int main() {
    // A checked build's own checks may allocate, and that is not the matching path.
    if constexpr (invariantChecksAfterCommands) {
        std::cout << "SKIPPED: automatic invariant checks are on in this build" << std::endl;
        return skipped;
    } else {
        runAll();
        std::cout << "matching path allocation tests passed" << std::endl;
        return EXIT_SUCCESS;
    }
}
