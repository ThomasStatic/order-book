#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "order_book/small_vector.hpp"

namespace {
// When zero, the next allocation throws std::bad_alloc instead; when positive, that many
// succeed first; negative is disarmed.
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

using order_book::SmallVector;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

struct Pair {
    unsigned int first;
    unsigned int second;
    bool operator==(const Pair&) const = default;
};

using Small = SmallVector<Pair, 3>;

Small filled(unsigned int count) {
    Small values;
    for (unsigned int i = 0; i < count; ++i) {
        values.push_back({i, i * 10});
    }
    return values;
}

// Whether the first `count` elements are filled(count)'s.
bool startsWith(const Small& values, unsigned int count) {
    if (values.size() < count) {
        return false;
    }
    for (unsigned int i = 0; i < count; ++i) {
        if (!(values[i] == Pair{i, i * 10})) {
            return false;
        }
    }
    return true;
}

bool holds(const Small& values, unsigned int count) {
    if (values.size() != count) {
        return false;
    }
    for (unsigned int i = 0; i < count; ++i) {
        if (!(values[i] == Pair{i, i * 10})) {
            return false;
        }
    }
    return true;
}

void testEmpty() {
    const Small values;
    expect(values.empty() && values.size() == 0, "a new vector is empty");
    expect(values.capacity() == 3 && !values.usesHeap(), "a new vector has its inline capacity and no heap block");
    expect(values.begin() == values.end(), "an empty vector's range is empty");
    expect(values == std::vector<Pair>{}, "an empty vector equals an empty std::vector");
}

void testInlineThenHeap() {
    Small values;
    for (unsigned int i = 0; i < 3; ++i) {
        values.push_back({i, i * 10});
        expect(!values.usesHeap(), "up to the inline capacity, elements stay inline");
    }
    values.push_back({3, 30});
    expect(values.usesHeap() && values.capacity() >= 4, "past the inline capacity, elements move to the heap");
    for (unsigned int i = 4; i < 40; ++i) {
        values.push_back({i, i * 10});
    }
    expect(holds(values, 40), "every element survives each growth, in order");
    expect(values.front() == Pair{0, 0} && values.back() == Pair{39, 390}, "front and back are the ends");
}

void testPushBackOfOwnElement() {
    // Full on the heap, so growing frees the block that holds the element being pushed: it
    // must be copied first (AddressSanitizer reports the use after free otherwise).
    Small values = filled(6);
    expect(values.usesHeap() && values.size() == values.capacity(), "the vector is full on the heap");
    values.push_back(values[2]);
    expect(values.size() == 7 && values[6] == Pair{2, 20} && startsWith(values, 6),
           "pushing one of the vector's own elements while growing on the heap");
    Small inlineValues = filled(3);
    inlineValues.push_back(inlineValues[0]);
    expect(inlineValues.size() == 4 && inlineValues[3] == Pair{0, 0},
           "pushing one of the vector's own elements while moving off the inline storage");
}

void testFailedGrowthChangesNothing() {
    // reserve and push_back give the strong guarantee the engine relies on: when growing
    // throws, the elements, size, and capacity are as they were.
    for (unsigned int count : {3u, 6u}) {
        Small values = filled(count);
        const std::size_t capacity = values.capacity();
        const Pair* const block = values.data();
        allocationsBeforeFailure = 0;
        bool threw = false;
        try {
            values.push_back({99, 99});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        allocationsBeforeFailure = -1;
        expect(threw && holds(values, count) && values.capacity() == capacity && values.data() == block,
               "a push_back whose growth fails leaves the vector unchanged (" + std::to_string(count) + ")");

        allocationsBeforeFailure = 0;
        threw = false;
        try {
            values.reserve(100);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        allocationsBeforeFailure = -1;
        expect(threw && holds(values, count) && values.capacity() == capacity && values.data() == block,
               "a reserve that fails leaves the vector unchanged (" + std::to_string(count) + ")");
        values.push_back({count, count * 10});
        expect(holds(values, count + 1), "the vector still grows after a failed attempt");
    }
}

void testCopyOfSmallHeapVectorIsInline() {
    Small values = filled(2);
    values.reserve(20);
    const Small copy(values);
    expect(values.usesHeap() && !copy.usesHeap() && copy.capacity() == 3 && holds(copy, 2),
           "a copy of a heap vector that fits inline is inline");
}

void testInlineCapacityOfOne() {
    SmallVector<Pair, 1> values;
    values.push_back({1, 1});
    expect(!values.usesHeap() && values.size() == 1, "one element fits inline");
    for (unsigned int i = 2; i <= 9; ++i) {
        values.push_back({i, i});
    }
    expect(values.usesHeap() && values.size() == 9 && values[8] == Pair{9, 9} && values[0] == Pair{1, 1},
           "an inline capacity of one grows like any other");
    SmallVector<Pair, 1> moved(std::move(values));
    expect(moved.size() == 9 && values.empty() && values.capacity() == 1, "moving with an inline capacity of one");
}

void testReserve() {
    Small values = filled(2);
    values.reserve(2);
    expect(!values.usesHeap() && values.capacity() == 3, "reserving no more than the capacity changes nothing");
    values.reserve(10);
    expect(values.usesHeap() && values.capacity() == 10 && holds(values, 2), "reserving more moves the elements");
    const Pair* const before = values.data();
    for (unsigned int i = 2; i < 10; ++i) {
        values.push_back({i, i * 10});
    }
    expect(values.data() == before && holds(values, 10), "pushing within a reservation does not reallocate");
}

void testCopy() {
    for (unsigned int count : {0u, 2u, 3u, 7u}) {
        const Small original = filled(count);
        const Small copy(original);
        expect(holds(copy, count) && holds(original, count) && copy == original,
               "a copy holds the same elements and leaves the source alone (" + std::to_string(count) + ")");
        expect(count == 0 || copy.data() != original.data(), "a copy owns its own elements");

        Small assigned = filled(5);
        assigned = original;
        expect(holds(assigned, count), "copy assignment replaces the elements (" + std::to_string(count) + ")");
        const Small& alias = assigned;
        assigned = alias;
        expect(holds(assigned, count), "self-assignment keeps the elements");
    }
}

void testMove() {
    for (unsigned int count : {0u, 2u, 3u, 7u}) {
        Small source = filled(count);
        const bool heap = source.usesHeap();
        const Pair* const block = source.data();
        Small moved(std::move(source));
        expect(holds(moved, count), "a moved-to vector holds the elements (" + std::to_string(count) + ")");
        expect(source.empty() && !source.usesHeap() && source.capacity() == 3,
               "a moved-from vector is empty with its inline capacity");
        expect(!heap || moved.data() == block, "moving a heap vector takes its block without copying");

        Small assigned = filled(6);
        assigned = std::move(moved);
        expect(holds(assigned, count) && moved.empty(), "move assignment takes the elements (" + std::to_string(count) + ")");

        source.push_back({9, 90});
        expect(source.size() == 1 && source[0] == Pair{9, 90}, "a moved-from vector can be reused");
    }
}

void testComparisonsAndConversions() {
    const Small values = filled(4);
    std::vector<Pair> expected;
    for (unsigned int i = 0; i < 4; ++i) {
        expected.push_back({i, i * 10});
    }
    expect(values == expected && expected == values, "equal to a std::vector with the same elements, either way round");
    expected.back().second = 0;
    expect(!(values == expected), "not equal when one element differs");
    expected.pop_back();
    expect(!(values == expected), "not equal when the sizes differ");
    expect(values == filled(4) && !(values == filled(3)), "comparison between two small vectors");

    const std::span<const Pair> span = values;
    expect(span.size() == 4 && span.data() == values.data(), "a span binds to the elements in place");
    const std::vector<Pair> copied = values.toVector();
    expect(copied.size() == 4 && copied[3] == Pair{3, 30} && copied.data() != values.data(),
           "toVector copies the elements");

    const Small listed{{1, 2}, {3, 4}};
    expect(listed.size() == 2 && listed[1] == Pair{3, 4} && !listed.usesHeap(), "an initializer list fills the vector");
    const Small longList{{1, 1}, {2, 2}, {3, 3}, {4, 4}};
    expect(longList.size() == 4 && longList.usesHeap() && longList[3] == Pair{4, 4},
           "an initializer list longer than the inline capacity goes to the heap");
}

}  // namespace

int main() {
    testEmpty();
    testInlineThenHeap();
    testPushBackOfOwnElement();
    testFailedGrowthChangesNothing();
    testCopyOfSmallHeapVectorIsInline();
    testInlineCapacityOfOne();
    testReserve();
    testCopy();
    testMove();
    testComparisonsAndConversions();
    std::cout << "small vector tests passed" << std::endl;
    return EXIT_SUCCESS;
}
