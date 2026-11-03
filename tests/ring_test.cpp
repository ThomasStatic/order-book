#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include "order_book/runtime/ring.hpp"

// Counts every allocation, to prove pushing and draining make none.
namespace {
std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
    ++allocations;
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace order_book;
using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint64_t> drained(Ring<std::uint64_t>& ring, std::size_t limit) {
    std::vector<std::uint64_t> items;
    ring.drain(limit, [&items](std::uint64_t item) { items.push_back(item); });
    return items;
}

using Items = std::vector<std::uint64_t>;

void testCapacityIsAPowerOfTwo() {
    for (const std::size_t capacity : {std::size_t{0}, std::size_t{3}, std::size_t{4095}}) {
        bool refused = false;
        try {
            Ring<std::uint64_t> ring(capacity);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        expect(refused, "a capacity that is not a power of two is refused: " + std::to_string(capacity));
    }
    const Ring<std::uint64_t> smallest(1);
    expect(smallest.capacity() == 1, "one slot is a power of two");
    expect(Ring<std::uint64_t>(defaultCommandRingSlots).capacity() == 4096, "the default sizes are powers of two");
    expect(Ring<std::uint64_t>(defaultResponseRingSlots).capacity() == 16384, "both of them");
}

void testEmpty() {
    Ring<std::uint64_t> ring(4);
    expect(!ring.hasItems(), "a new ring says it is empty");
    expect(drained(ring, 8).empty(), "and holds nothing");
    expect(ring.tryPush(1), "a push succeeds");
    expect(ring.hasItems(), "the reader can see it");
    expect(drained(ring, 8) == Items{1}, "the item comes out");
    expect(!ring.hasItems() && drained(ring, 8).empty(), "and the ring is empty again");
}

void testExactlyFull() {
    Ring<std::uint64_t> ring(4);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        expect(ring.tryPush(i), "every slot takes an item");
    }
    expect(!ring.tryPush(5), "a full ring refuses");
    expect(!ring.tryPush(6), "and keeps refusing");
    expect(drained(ring, 1) == Items{1}, "the oldest comes out first");
    expect(ring.tryPush(7), "one slot is free again");
    expect(!ring.tryPush(8), "and that was the only one");
    expect(drained(ring, 8) == (Items{2, 3, 4, 7}), "the refused items never went in");
}

void testAOneSlotRing() {
    Ring<std::uint64_t> ring(1);
    for (std::uint64_t i = 0; i < 10; ++i) {
        expect(ring.tryPush(i), "the one slot is free");
        expect(!ring.tryPush(99), "and then it isn't");
        expect(drained(ring, 1) == Items{i}, "the item comes back");
    }
}

void testWrapAround() {
    Ring<std::uint64_t> ring(8);
    std::uint64_t next = 0;
    std::uint64_t expected = 0;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 5; ++i) {
            expect(ring.tryPush(next++), "the ring has room each round");
        }
        const Items items = drained(ring, 5);
        expect(items.size() == 5, "every item comes back");
        for (const std::uint64_t item : items) {
            expect(item == expected++, "in order, across the end of the array");
        }
    }
}

void testBatchLimits() {
    Ring<std::uint64_t> ring(16);
    for (std::uint64_t i = 0; i < 10; ++i) {
        ring.tryPush(i);
    }
    expect(drained(ring, 0).empty(), "a limit of 0 takes nothing");
    expect(drained(ring, 3) == (Items{0, 1, 2}), "a limit caps a batch");
    expect(drained(ring, 3) == (Items{3, 4, 5}), "and the next batch goes on from there");
    expect(drained(ring, 100) == (Items{6, 7, 8, 9}), "a limit past what is there takes all of it");
}

// What a matching thread needs before a take: how many are waiting, up to a limit, each looked
// at in place, and whether its log ring has room for them, with nothing taken or given back.
void testLookingAhead() {
    Ring<std::uint64_t> ring(4);
    expect(ring.waiting(8) == 0, "an empty ring has nothing waiting");
    expect(ring.hasRoomFor(4) && !ring.hasRoomFor(5), "and room for its capacity and no more");
    for (std::uint64_t item = 1; item <= 3; ++item) {
        ring.tryPush(item);
    }
    expect(ring.waiting(8) == 3 && ring.waiting(2) == 2, "waiting counts what is there, up to the limit");
    expect(ring.peek(0) == 1 && ring.peek(2) == 3, "peek sees each in place, oldest first");
    expect(ring.hasRoomFor(1) && !ring.hasRoomFor(2), "room for one more");
    expect(drained(ring, 1) == std::vector<std::uint64_t>{1}, "looking took nothing");
    expect(ring.hasRoomFor(2), "and a drain gives room back");
    ring.tryPush(4);
    ring.tryPush(5);
    expect(ring.waiting(8) == 4 && ring.peek(3) == 5, "peek follows the ring round");
}

// A drain that stops takes the item it stopped on, and leaves the rest for later.
void testADrainCanStop() {
    Ring<std::uint64_t> ring(8);
    for (std::uint64_t item = 1; item <= 5; ++item) {
        ring.tryPush(item);
    }
    std::vector<std::uint64_t> taken;
    const std::size_t count = ring.drainWhile(8, [&taken](std::uint64_t item) {
        taken.push_back(item);
        return item != 2;
    });
    expect(count == 2 && taken == std::vector<std::uint64_t>{1, 2}, "it stops after the item that said so");
    expect(drained(ring, 8) == std::vector<std::uint64_t>{3, 4, 5}, "and the rest wait");
    expect(ring.drainWhile(8, [](std::uint64_t) { return false; }) == 0, "an empty ring takes nothing");
}

void testNoAllocation() {
    Ring<std::uint64_t> ring(64);
    std::uint64_t sum = 0;
    const std::size_t before = allocations;
    for (std::uint64_t i = 0; i < 10000; ++i) {
        ring.tryPush(i);
        ring.tryPush(i);
        ring.drain(2, [&sum](std::uint64_t item) { sum += item; });
    }
    const std::size_t during = allocations - before;
    expect(during == 0, "pushing and draining allocate nothing");
    expect(sum == 9999ULL * 10000ULL, "every item came through");
}

void testElements() {
    static_assert(sizeof(CommandElement) == 64 && std::is_trivially_copyable_v<CommandElement>);
    static_assert(sizeof(ResponseRecord) == 48 && std::is_trivially_copyable_v<ResponseRecord>);
    static_assert(!std::is_same_v<replay::Command, CommandElement>, "only a Command can reach the log");

    Ring<CommandElement> commands(2);
    replay::AddCommand add{};
    add.orderId = 7;
    add.side = Side::SELL;
    add.price = 100;
    add.quantity = 5;
    expect(commands.tryPush(CommandElement{99, add, 3, 12, ControlKind::COMMAND}), "a command goes in");
    expect(commands.tryPush(CommandElement{0, replay::CancelCommand{1}, 0, 0, ControlKind::STOP}), "and a marker");
    std::vector<CommandElement> taken;
    commands.drain(4, [&taken](const CommandElement& element) { taken.push_back(element); });
    expect(taken.size() == 2 && taken[0].correlation == 99 && taken[0].slot == 3 && taken[0].connection == 12,
           "an element keeps its fields");
    expect(std::get<replay::AddCommand>(taken[0].command) == add, "and its command");
    expect(taken[1].control == ControlKind::STOP, "and a marker is told apart outside the command");

    Ring<ResponseRecord> responses(2);
    expect(responses.tryPush(ResponseRecord{99, 12, 3, OrderCancelled{7, 5}, ResponseKind::LAST_EVENT}), "a record goes in");
    std::vector<ResponseRecord> records;
    responses.drain(4, [&records](const ResponseRecord& record) { records.push_back(record); });
    expect(records.size() == 1 && records[0].event == BookEvent{OrderCancelled{7, 5}} && records[0].kind == ResponseKind::LAST_EVENT,
           "a record keeps its event, with a kind marking the last");
}

// A real second thread, so ThreadSanitizer and a weak memory model see the ordering. A fixed
// count, never a time limit, and a small ring, so both sides keep meeting full and empty.
void testOrderingAcrossThreads() {
    constexpr std::uint64_t items = 2000000;
    Ring<std::uint64_t> forward(16);
    Ring<std::uint64_t> back(16);

    std::thread echo([&forward, &back] {
        std::uint64_t seen = 0;
        while (seen < items) {
            seen += forward.drain(7, [&back](std::uint64_t item) {
                while (!back.tryPush(item * 3)) {
                    std::this_thread::yield();
                }
            });
        }
    });

    std::uint64_t pushed = 0;
    std::uint64_t received = 0;
    bool inOrder = true;
    while (received < items) {
        if (pushed < items && forward.tryPush(pushed)) {
            ++pushed;
        }
        received += back.drain(5, [&inOrder, &received, count = std::uint64_t{0}](std::uint64_t item) mutable {
            inOrder = inOrder && item == (received + count) * 3;
            ++count;
        });
    }
    echo.join();
    expect(inOrder, "every item arrives once, in order, through both rings");
    expect(received == items, "and none is lost");
}

}  // namespace

int main() {
    testCapacityIsAPowerOfTwo();
    testEmpty();
    testExactlyFull();
    testAOneSlotRing();
    testWrapAround();
    testBatchLimits();
    testLookingAhead();
    testADrainCanStop();
    testNoAllocation();
    testElements();
    testOrderingAcrossThreads();
    std::cout << "ring tests passed" << std::endl;
    return EXIT_SUCCESS;
}
