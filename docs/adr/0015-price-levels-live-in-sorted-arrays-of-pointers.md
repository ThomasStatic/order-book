# Price levels live in sorted arrays of pointers

A book's four level containers (bids, asks, buy stops, sell stops) are sorted arrays of `{price, pointer to level}`, best price at the back, with each level object in the book's node pool. Finding a level gallops from the best end and then bisects, so a price near the touch stays in the last cache line, and because the array holds pointers, levels never move and the `PriceLevel*` in every index entry stays valid. This replaces `std::map`, which was the last heap call on most commands, and it matches or beats a pooled `std::map` on every workload that could be read: with result reuse, 8 to 17% over v2.0.0 on all but `small-book` and `cancellation-heavy`, against 3 to 9% for the pooled map. The price is a tail a client can reach on purpose: a new level behind every other level shifts the whole array, 8.3 µs at 20,000 levels on one side and 236 µs at 200,000, where `std::map` stays near 300 ns. That needs one resting order at each distinct price, and at 2,000 levels, the deepest benchmark book, the array costs 900 ns.

Those numbers come from a throwaway Release (MSVC) program outside the repo: medians of 9 paired rounds at 100k commands and 3 at 1M, in one session on one machine, with no Linux run, and differences under about 4 points are noise. Stop levels were on pooled maps in those runs, so the array for stops is chosen but not measured. They are not a recorded baseline.

## Considered options

- **`std::map` with `PoolAllocator`.** The smallest change, and it keeps O(log L) at every depth. But MSVC's map allocates a head node when moved, through an allocator the moved-from map still holds, so a moved-from book would keep a node in the pool its successor owns, and moving a book could throw. That is the trap that put `triggeredStops` behind a pointer, and the maps would need the same indirection.
- **A contiguous array of levels themselves.** The replacement `docs/OPTIMIZATION.md` deferred. Level handles would have to become indices and `PriceLevel` movable. Holding pointers keeps both as they are.
- **A pooled B-tree, or an array near the touch over a map behind it.** Either bounds the tail, at the cost of a container of our own with no measurement yet saying the tail matters.

## Consequences

- A new level far from the touch costs time proportional to the levels better than it, and any client can place one. Report it, and do not call level insertion O(log L).
- A moved-from book holds nothing in any pool, so `OrderBook`'s move needs no new indirection and cannot throw for this reason.
- The array's own growth is a heap call, so its size is part of a book's capacity, and growing it counts as one.
- The ordering violations `checkInvariants` could never see through `std::map` can now happen, so they get tests, and `restore` must keep refusing out-of-order levels because nothing sorts them any more.
