#include <cstddef>
#include <cstdlib>
#include <iostream>

#include "order_book/node_pool.hpp"

// Reads a block after it went back to its pool. The pool keeps the memory, so only poisoning
// can catch this: under ASan the read reports use-after-poison and the program dies, which is
// what CTest looks for, and without ASan the read goes unnoticed and there is nothing to test.
namespace {

struct Node {
    void* links[2];
    unsigned int payload[8];
};

[[maybe_unused]] volatile unsigned int observed = 0;

}  // namespace

int main() {
#if defined(ORDER_BOOK_POISON_POOL)
    order_book::NodePool pool;
    auto* const live = static_cast<Node*>(pool.allocate(sizeof(Node), alignof(Node)));
    auto* const stale = static_cast<Node*>(pool.allocate(sizeof(Node), alignof(Node)));
    live->payload[0] = 1;
    stale->payload[7] = 7;
    pool.deallocate(stale, sizeof(Node), alignof(Node));
    observed = live->payload[0];  // a neighbouring live block is untouched
    std::cout << "reading a freed block" << std::endl;
    observed = stale->payload[7];  // past the link, so the whole block must be poisoned
    std::cout << "FAILED: a stale block read was not caught" << std::endl;
    return EXIT_FAILURE;
#else
    std::cout << "SKIPPED: no AddressSanitizer in this build" << std::endl;
    return EXIT_SUCCESS;
#endif
}
