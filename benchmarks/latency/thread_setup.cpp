#include "thread_setup.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include "order_book/runtime/thread_placement.hpp"

// The engine's own placement, so a latency run and the engine pin and raise the same way.
namespace order_book::latency {

void pinCurrentThread(unsigned int core) {
    if (const std::optional<std::string> refused = runtime::pinThisThread(core)) {
        throw std::runtime_error("could not pin to core " + std::to_string(core) + ": " + *refused);
    }
}

void raiseCurrentThreadPriority() {
    if (const std::optional<std::string> refused = runtime::setThisThreadPriority(runtime::ThreadPriority::HIGH)) {
        throw std::runtime_error("could not raise this thread's priority: " + *refused);
    }
}

}  // namespace order_book::latency
