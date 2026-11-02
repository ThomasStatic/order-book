#pragma once

#include <cstddef>
#include <span>

#include "order_book/runtime/shard.hpp"

// What the engine's abort handler prints, for the thread that aborted. The handler itself is
// apps/'s; this is the part that knows a shard. See docs/internals/runtime.md.
namespace order_book::runtime {

    // Names the shard the calling thread runs, for describeAbort, until destroyed. A matching
    // thread holds one once its shard is built.
    class AbortScope {
    public:
        explicit AbortScope(const Shard& shard) noexcept;
        ~AbortScope();
        AbortScope(const AbortScope&) = delete;
        AbortScope& operator=(const AbortScope&) = delete;

    private:
        const Shard* previous;
    };

    // The abort line for the calling thread, newline included, cut to fit buffer:
    // "order_book_engine: aborted: shard 2 symbol AAPL position 12345" when a checked build
    // recorded where this thread's shard stood, and "order_book_engine: aborted" otherwise.
    // Returns the bytes written. Allocates nothing and takes no lock, for a SIGABRT handler.
    std::size_t describeAbort(std::span<char> buffer) noexcept;

}  // namespace order_book::runtime
