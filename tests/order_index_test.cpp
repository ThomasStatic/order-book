#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "order_book/order_index.hpp"

namespace {
// When zero, the next allocation throws std::bad_alloc instead; negative is disarmed.
long allocationsBeforeFailure = -1;
}  // namespace

void* operator new(std::size_t size) {
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

namespace {

using order_book::OrderId;
using order_book::OrderIndex;
using order_book::OrderLocation;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// A location that records which ID it belongs to (in its price), so lookups can be checked.
OrderLocation locationFor(OrderId id) {
    return OrderLocation(id * 7 + 1, id % 2 == 0 ? Side::BUY : Side::SELL, {}, nullptr);
}

// Every entry the index holds, by iteration, as an ordered map; each ID must appear once.
std::map<OrderId, unsigned int> contents(const OrderIndex& index) {
    std::map<OrderId, unsigned int> seen;
    for (const auto& [id, owner, location] : index) {
        static_cast<void>(owner);
        expect(seen.emplace(id, location.tickPrice).second, "iteration visits each entry once");
    }
    return seen;
}

// Whether the index holds exactly the model's entries, by size, iteration, find, and at.
void expectSame(const OrderIndex& index, const std::unordered_map<OrderId, unsigned int>& model,
                OrderId maxId, const std::string& context) {
    expect(index.size() == model.size() && index.empty() == model.empty(), context + ": size");
    const std::map<OrderId, unsigned int> seen = contents(index);
    expect(seen.size() == model.size(), context + ": iteration visits every entry");
    for (OrderId id = 1; id <= maxId; ++id) {
        const auto modelEntry = model.find(id);
        const auto found = index.find(id);
        if (modelEntry == model.end()) {
            expect(found == index.end() && index.count(id) == 0, context + ": an absent ID is not found");
        } else {
            expect(found != index.end() && found->first == id && found->second.tickPrice == modelEntry->second
                       && index.at(id).tickPrice == modelEntry->second && index.count(id) == 1,
                   context + ": a present ID is found with its location");
        }
    }
}

// Random emplaces and erases (by key and by iterator) against std::unordered_map. Above 12
// IDs, a small table makes collisions, wrapped probe runs, and backward shifts constant (12
// IDs fit a 16-slot table on distinct home slots, so that case checks only the plain path).
void testAgainstModel() {
    for (const OrderId maxId : {OrderId{12}, OrderId{40}, OrderId{300}, OrderId{5000}}) {
        std::mt19937_64 random(maxId);
        OrderIndex index;
        std::unordered_map<OrderId, unsigned int> model;
        for (int step = 0; step < 60000; ++step) {
            const auto roll = random() % 100;
            const auto drawn = random() % maxId;
            const OrderId id = static_cast<OrderId>(drawn + 1);
            if (roll < 55) {
                const auto [position, added] = index.emplace(id, locationFor(id));
                const bool modelAdded = model.emplace(id, id * 7 + 1).second;
                expect(added == modelAdded && position->first == id, "emplace agrees with the model");
            } else if (roll < 80) {
                expect(index.erase(id) == model.erase(id), "erase by key agrees with the model");
            } else {
                const auto found = index.find(id);
                if (found != index.end()) {
                    index.erase(found);
                    model.erase(id);
                }
            }
            if (step % 997 == 0) {
                expectSame(index, model, maxId, "step " + std::to_string(step) + " of " + std::to_string(maxId));
            }
        }
        expectSame(index, model, maxId, "end of " + std::to_string(maxId));
        index.clear();
        model.clear();
        expectSame(index, model, maxId, "after clear");
        index.emplace(1, locationFor(1));
        model.emplace(1, 8);
        expectSame(index, model, maxId, "reused after clear");
    }
}

// The home slot of an ID in a 16-slot table: the index's Fibonacci hash, copied here so the
// next test can choose IDs by where they land.
std::size_t homeIn16(OrderId id) {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL) >> 60);
}

// The first `count` IDs, from 1 up, whose home slot is `slot` in a 16-slot table.
std::vector<OrderId> idsWithHome(std::size_t slot, std::size_t count) {
    std::vector<OrderId> ids;
    for (OrderId id = 1; ids.size() < count; ++id) {
        if (homeIn16(id) == slot) {
            ids.push_back(id);
        }
    }
    return ids;
}

// A probe run that wraps past the table's last slot: entries homed at 14 and 15 fill slots
// 14, 15, 0, and 1, then one homed at 0 is pushed on to slot 2. Erasing each member in turn
// must leave every other one reachable, which takes the backward shift's wrapped branch
// (the gap after the wrap, or before it) and its rule that an entry never moves to a slot
// before its home.
void testWrappedProbeRun() {
    const std::vector<OrderId> at14 = idsWithHome(14, 3);
    const std::vector<OrderId> at15 = idsWithHome(15, 1);
    const std::vector<OrderId> at0 = idsWithHome(0, 1);
    const std::vector<OrderId> run = {at14[0], at15[0], at14[1], at14[2], at0[0]};
    for (std::size_t erased = 0; erased < run.size(); ++erased) {
        OrderIndex index;
        std::unordered_map<OrderId, unsigned int> model;
        for (const OrderId id : run) {
            index.emplace(id, locationFor(id));
            model.emplace(id, id * 7 + 1);
        }
        expect(index.capacity() == 16, "the run fits a 16-slot table");
        const OrderId maxId = *std::max_element(run.begin(), run.end());
        expectSame(index, model, maxId, "a wrapped run before erasing");
        if (erased % 2 == 0) {
            index.erase(run[erased]);
        } else {
            index.erase(index.find(run[erased]));
        }
        model.erase(run[erased]);
        expectSame(index, model, maxId, "a wrapped run after erasing its member " + std::to_string(erased));
        // Then empty it from the other end, checking after each erase.
        for (std::size_t other = run.size(); other-- > 0;) {
            if (other != erased) {
                index.erase(run[other]);
                model.erase(run[other]);
                expectSame(index, model, maxId, "emptying a wrapped run");
            }
        }
    }
}

void testBasics() {
    OrderIndex index;
    expect(index.empty() && index.begin() == index.end() && index.find(5) == index.end(), "a new index is empty");
    expect(index.erase(5) == 0, "erasing from an empty index removes nothing");
    bool threw = false;
    try {
        (void)index.at(5);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect(threw, "at throws for an absent ID");
    threw = false;
    try {
        index.emplace(0, locationFor(1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw && index.empty() && index.find(0) == index.end(), "the reserved ID 0 is refused");

    const auto [first, added] = index.emplace(5, locationFor(5));
    expect(added && first->first == 5, "an emplace adds");
    const auto [again, addedAgain] = index.emplace(5, locationFor(6));
    expect(!addedAgain && again->second.tickPrice == locationFor(5).tickPrice, "a second emplace keeps the first");
    index.at(5).tickPrice = 99;
    expect(index.find(5)->second.tickPrice == 99, "at gives the stored location, to read or change");
}

void testCapacity() {
    OrderIndex index;
    index.reserve(1000);
    const std::size_t capacity = index.capacity();
    expect(capacity * 3 >= 1000 * 4 && (capacity & (capacity - 1)) == 0, "reserve sizes a power-of-two table");
    for (OrderId id = 1; id <= 1000; ++id) {
        index.emplace(id, locationFor(id));
    }
    expect(index.capacity() == capacity && index.size() == 1000, "no growth within the reservation");
    OrderId next = 1001;
    while ((index.size() + 1) * 4 <= capacity * 3) {  // up to exactly three quarters full
        index.emplace(next, locationFor(next));
        ++next;
    }
    expect(index.capacity() == capacity && index.size() * 4 == capacity * 3, "a table fills to three quarters");
    index.emplace(next, locationFor(next));
    expect(index.capacity() == capacity * 2, "one more entry past three quarters doubles the table");
    index.reserve(10);
    expect(index.capacity() == capacity * 2, "a smaller reservation never shrinks the table");
    bool threw = false;
    try {
        index.reserve(static_cast<std::size_t>(-1));
    } catch (const std::length_error&) {
        threw = true;
    }
    expect(threw && index.capacity() == capacity * 2 && index.size() == 1000 + (next - 1000),
           "an impossible reservation throws length_error and changes nothing");
}

void testFailedGrowthChangesNothing() {
    OrderIndex index;
    OrderId id = 1;
    while (index.size() < 12) {  // a 16-slot table, full to three quarters
        index.emplace(id, locationFor(id));
        ++id;
    }
    std::unordered_map<OrderId, unsigned int> model;
    for (OrderId present = 1; present < id; ++present) {
        model.emplace(present, present * 7 + 1);
    }
    const std::size_t capacity = index.capacity();
    allocationsBeforeFailure = 0;
    bool threw = false;
    try {
        index.emplace(id, locationFor(id));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    allocationsBeforeFailure = -1;
    expect(threw && index.capacity() == capacity, "an emplace whose growth fails throws");
    expectSame(index, model, id + 5, "after a failed growth");
    index.emplace(id, locationFor(id));
    model.emplace(id, id * 7 + 1);
    expectSame(index, model, id + 5, "growing after a failed attempt");

    allocationsBeforeFailure = 0;
    threw = false;
    try {
        index.reserve(100000);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    allocationsBeforeFailure = -1;
    expect(threw, "a reserve whose allocation fails throws");
    expectSame(index, model, id + 5, "after a failed reserve");
}

void testCopyAndMove() {
    OrderIndex original;
    std::unordered_map<OrderId, unsigned int> model;
    for (OrderId id = 1; id <= 100; ++id) {
        original.emplace(id, locationFor(id));
        model.emplace(id, id * 7 + 1);
    }
    const OrderIndex copy(original);
    expectSame(copy, model, 120, "a copy");

    OrderIndex moved(std::move(original));
    expectSame(moved, model, 120, "a moved-to index");
    expectSame(original, {}, 120, "a moved-from index is empty");
    original.emplace(3, locationFor(3));
    expectSame(original, {{3, 22}}, 120, "a moved-from index can be used again");

    OrderIndex assigned;
    assigned.emplace(500, locationFor(500));
    assigned = std::move(moved);
    expectSame(assigned, model, 600, "move assignment replaces the entries");
    expectSame(moved, {}, 120, "a moved-from index after assignment is empty");
    OrderIndex& alias = assigned;
    assigned = std::move(alias);
    expectSame(assigned, model, 600, "self-move-assignment keeps the entries");
}

}  // namespace

int main() {
    testBasics();
    testAgainstModel();
    testWrappedProbeRun();
    testCapacity();
    testFailedGrowthChangesNothing();
    testCopyAndMove();
    std::cout << "order index tests passed" << std::endl;
    return EXIT_SUCCESS;
}
