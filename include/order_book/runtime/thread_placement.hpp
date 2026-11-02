#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Pinning a thread to a core and raising its priority, for the engine's own threads, each calling
// these on itself before it is ready. See docs/internals/runtime.md.
namespace order_book::runtime {

    // high is THREAD_PRIORITY_HIGHEST on Windows and a nice value of -10 on Linux, never a
    // real-time policy, since a thread spinning under one can starve everything else on its core.
    enum class ThreadPriority { NORMAL, HIGH };

    // Each returns nothing once the calling thread is pinned or raised, or why the system refused:
    // a core this process may not run on, or a priority it may not take. Neither allocates on
    // success.
    std::optional<std::string> pinThisThread(std::uint32_t core);
    std::optional<std::string> setThisThreadPriority(ThreadPriority priority);

}  // namespace order_book::runtime
