#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "order_book/node_pool.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"

namespace {
// When zero, the next allocation throws std::bad_alloc instead; when positive, that many
// succeed first; negative is disarmed. Counts every allocation while armed or not.
long allocationsBeforeFailure = -1;
long allocations = 0;
// Every pointer a test counts is written here, so the compiler cannot remove an allocation
// and its free as unused (Clang may, for std::allocator), which would skip the count.
void* volatile escaped = nullptr;
}  // namespace

void* operator new(std::size_t size) {
    ++allocations;
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

// GCC inlines these where it can see the pointer came from operator new and warns that free
// does not match it; the operator new above allocates with malloc, so it does.
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

namespace order_book {

// Befriended by OrderBook; reads the pool and the queues' allocators' targets.
struct OrderBookTestAccess {
    static const NodePool* pool(const OrderBook& book) { return book.nodePool.get(); }
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
};

}  // namespace order_book

namespace {

// Every count is read into a variable before expect is called: expect's message is a
// std::string built from a literal, which allocates, and argument evaluation order is
// unspecified (MSVC builds the message first), so reading the counter inside the call
// could count the message.
using order_book::NodePool;
using order_book::Order;
using order_book::OrderBook;
using order_book::PoolAllocator;
using order_book::Side;
using Access = order_book::OrderBookTestAccess;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

struct Node {
    void* links[2];
    unsigned int payload[8];
};

void testBlocksAreReused() {
    NodePool pool;
    PoolAllocator<Node> allocator(&pool);
    std::vector<Node*> nodes;
    nodes.reserve(1000);
    const long before = allocations;
    for (int i = 0; i < 1000; ++i) {
        nodes.push_back(allocator.allocate(1));
    }
    const long chunkAllocations = allocations - before;  // read before expect builds its message
    expect(chunkAllocations > 0 && chunkAllocations < 1000 / 100 + 2,
           "a thousand nodes should take a few chunks, not a thousand allocations");
    expect(std::set<Node*>(nodes.begin(), nodes.end()).size() == nodes.size(), "every node gets its own block");
    for (Node* node : nodes) {
        expect(reinterpret_cast<std::uintptr_t>(node) % alignof(Node) == 0, "blocks are aligned for the node");
    }

    Node* const freed = nodes[500];
    allocator.deallocate(freed, 1);
    const long afterFree = allocations;
    Node* const reused = allocator.allocate(1);
    const bool noHeap = allocations == afterFree;
    expect(reused == freed && noHeap, "a freed block is handed to the next node without the heap");
    for (Node* node : nodes) {
        allocator.deallocate(node, 1);
    }
    const long beforeReuse = allocations;
    for (int i = 0; i < 1000; ++i) {
        nodes[static_cast<std::size_t>(i)] = allocator.allocate(1);
    }
    const bool noNewChunks = allocations == beforeReuse;
    expect(noNewChunks, "freed blocks serve later nodes without new chunks");
    for (Node* node : nodes) {
        allocator.deallocate(node, 1);
    }
}

void testOtherRequestsGoToTheHeap() {
    NodePool pool;
    PoolAllocator<Node> nodes(&pool);
    PoolAllocator<double> doubles(nodes);
    PoolAllocator<char[24]> twentyFours(nodes);
    PoolAllocator<char[40]> forties(nodes);
    PoolAllocator<char[72]> seventyTwos(nodes);
    PoolAllocator<Node> heapOnly;

    // Four sizes claim the pool's four classes; each is then served from its own blocks.
    Node* const node = nodes.allocate(1);
    double* const single = doubles.allocate(1);
    auto* const twentyFour = twentyFours.allocate(1);
    auto* const forty = forties.allocate(1);
    escaped = node;
    escaped = single;
    escaped = twentyFour;
    escaped = forty;
    doubles.deallocate(single, 1);
    const long beforeReuse = allocations;
    double* const reusedSingle = doubles.allocate(1);
    escaped = reusedSingle;
    const long afterReuse = allocations;

    // All readings first, then the checks (see above).
    const long before = allocations;
    auto* const seventyTwo = seventyTwos.allocate(1);  // a fifth size: the heap
    escaped = seventyTwo;
    const long afterFifth = allocations;
    double* const array = doubles.allocate(4);  // more than one object: the heap
    escaped = array;
    Node* const nodeArray = nodes.allocate(3);
    escaped = nodeArray;
    const long afterArrays = allocations;
    Node* const unpooled = heapOnly.allocate(1);  // no pool: the heap
    escaped = unpooled;
    const long afterUnpooled = allocations;

    expect(reusedSingle == single && afterReuse == beforeReuse, "each size class reuses its own freed blocks");
    expect(afterFifth == before + 1, "a size beyond the pool's classes goes to the heap");
    expect(afterArrays == afterFifth + 2, "arrays go to the heap");
    expect(afterUnpooled == afterArrays + 1, "an allocator without a pool uses the heap");
    expect(!(heapOnly == nodes) && nodes == doubles, "allocators are equal exactly when they share a pool");

    seventyTwos.deallocate(seventyTwo, 1);
    doubles.deallocate(array, 4);
    nodes.deallocate(nodeArray, 3);
    heapOnly.deallocate(unpooled, 1);
    doubles.deallocate(reusedSingle, 1);
    twentyFours.deallocate(twentyFour, 1);
    forties.deallocate(forty, 1);
    nodes.deallocate(node, 1);
}

void testFailedChunkChangesNothing() {
    // The first allocation a class makes is its chunk list's room, the second the chunk
    // itself; failing either leaves the pool usable.
    for (long failAt : {0L, 1L}) {
        NodePool pool;
        PoolAllocator<Node> allocator(&pool);
        allocationsBeforeFailure = failAt;
        bool threw = false;
        try {
            escaped = allocator.allocate(1);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool fired = allocationsBeforeFailure == -1;
        allocationsBeforeFailure = -1;
        expect(threw && fired, "a node whose chunk cannot be allocated throws (" + std::to_string(failAt) + ")");
        Node* const node = allocator.allocate(1);
        escaped = node;
        expect(node != nullptr, "the pool still works after a failed chunk");
        allocator.deallocate(node, 1);
    }
}

void testBookNodesComeFromItsPool() {
    // 500 resting orders on 10 levels: without the pool, each would allocate a queue node
    // and an index node; with it, the queue nodes come from a few chunks.
    OrderBook book;
    const long before = allocations;
    for (unsigned int id = 1; id <= 500; ++id) {
        book.submitOrder(Order(id, 100 - id % 10, 1, 1, Side::BUY));
    }
    const long made = allocations - before;
    expect(book.activeOrderCount() == 500, "every order rests");
    expect(made < 650, "resting orders should take their queue nodes from the book's pool, made "
                           + std::to_string(made) + " allocations for 500 orders");
}

// Each node type the book makes is served from one of the pool's size classes, not the heap
// fallback: once its first chunk exists, more nodes of that type allocate nothing of their
// own. A resting order's queue node (an Order and its owner) and a pending stop's node have
// classes of their own.
void testEachNodeTypeHasASizeClass() {
    OrderBook book;
    book.reserve(400);
    book.submitOrder(Order(1, 100, 1, 1, Side::BUY));
    long before = allocations;
    for (unsigned int id = 2; id <= 200; ++id) {
        book.submitOrder(Order(id, 100, 1, 1, Side::BUY));  // same level, so no map node
    }
    long made = allocations - before;
    expect(made == 0, "resting orders at one level should take their nodes from the pool, made "
                          + std::to_string(made) + " allocations for 199 orders");

    const auto stop = [](order_book::OrderId id) {
        order_book::OrderRequest request{id, Side::BUY, 0, 1, order_book::OrderType::STOP, order_book::TimeInForce::IOC};
        request.stopPrice = 120;
        return request;
    };
    book.submitOrder(stop(1001));
    before = allocations;
    for (unsigned int id = 1002; id <= 1200; ++id) {
        book.submitOrder(stop(id));  // same stop price, so no map node
    }
    made = allocations - before;
    expect(book.pendingStopCount() == 200, "every stop pends");
    // Only the pending index grows, by doubling: 16 to 256 slots is at most five growths, each
    // one allocation, or two in MSVC Debug builds, whose checked vector allocates a proxy too.
    expect(made <= 10, "pending stops should take their nodes from the pool, made " + std::to_string(made)
                           + " allocations for 199 stops");
}

// Fills a book with orders on several levels, some partly filled, so moves carry real queues.
void populate(OrderBook& book, unsigned int firstId) {
    for (unsigned int i = 0; i < 40; ++i) {
        book.submitOrder(Order(firstId + i, 100 - i % 5, 10, 1, Side::BUY));
        book.submitOrder(Order(firstId + 100 + i, 110 + i % 5, 10, 1, Side::SELL));
    }
    book.submitOrder(Order(firstId + 300, 110, 15, 1, Side::BUY));  // partly fills the best ask
}

void testMovedBookKeepsItsPool() {
    OrderBook original;
    populate(original, 1);
    const NodePool* const pool = Access::pool(original);
    OrderBook moved(std::move(original));
    expect(Access::pool(moved) == pool && Access::pool(original) == nullptr,
           "moving a book takes its pool, at the same address");
    expect(moved.checkInvariants().isValid() && moved.activeOrderCount() == 79, "the moved book is intact");
    for (unsigned int i = 0; i < 40; i += 3) {
        expect(moved.cancelOrder(1 + i).isSuccess(), "orders rested before the move cancel afterwards");
    }
    moved.submitOrder(Order(900, 99, 5, 1, Side::BUY));
    expect(moved.checkInvariants().isValid(), "the moved book keeps working");

    // A moved-from book has no pool; anything it rests afterwards takes its nodes from the heap.
    original.submitOrder(Order(7, 50, 1, 1, Side::BUY));
    expect(original.activeOrderCount() == 1 && original.cancelOrder(7).isSuccess()
               && original.checkInvariants().isValid(),
           "a moved-from book can still be used");
}

void testMoveAssignmentReleasesIntoTheRightPool() {
    OrderBook target;
    populate(target, 1);
    OrderBook source;
    populate(source, 1000);
    const NodePool* const sourcePool = Access::pool(source);
    target = std::move(source);
    expect(Access::pool(target) == sourcePool, "move assignment takes the source's pool");
    expect(target.checkInvariants().isValid() && target.activeOrderCount() == 79
               && Access::index(target).count(1000) == 1 && Access::index(target).count(1) == 0,
           "move assignment replaces the target's orders with the source's");
    for (unsigned int i = 0; i < 40; ++i) {
        target.cancelOrder(1000 + i);
    }
    populate(target, 5000);
    expect(target.checkInvariants().isValid(), "the assigned book keeps working");

    // Through two more books: each move takes the pool along, and the books left behind
    // still work.
    OrderBook second(std::move(target));
    OrderBook third;
    populate(third, 9000);
    third = std::move(second);
    expect(Access::pool(third) == sourcePool && third.checkInvariants().isValid(), "the pool follows the orders");
    populate(second, 20000);
    populate(target, 30000);
    expect(second.checkInvariants().isValid() && target.checkInvariants().isValid(), "books moved from still work");
    target = std::move(third);

    OrderBook& alias = target;
    target = std::move(alias);
    expect(target.checkInvariants().isValid() && Access::pool(target) == sourcePool,
           "self-move-assignment leaves the book as it was");
}

}  // namespace

int main() {
    testBlocksAreReused();
    testOtherRequestsGoToTheHeap();
    testFailedChunkChangesNothing();
    testBookNodesComeFromItsPool();
    testEachNodeTypeHasASizeClass();
    testMovedBookKeepsItsPool();
    testMoveAssignmentReleasesIntoTheRightPool();
    std::cout << "node pool tests passed" << std::endl;
    return EXIT_SUCCESS;
}
