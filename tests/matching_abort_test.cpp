#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

#include "order_book/invariants.hpp"
#include "order_book/runtime/matching_loop.hpp"

// An exception escaping a command must abort, naming the shard, instrument, and command, which
// CTest matches. It fails allocations once the books are running, as a real failure would, so
// the message path can't allocate either. Skipped where automatic checks are on, since a book's
// own checks then run in that failure too.
namespace {
bool failAllocations = false;
}

void* operator new(std::size_t size) {
    if (failAllocations) {
        throw std::bad_alloc();
    }
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

[[maybe_unused]] int run() {
    const InstrumentDirectory directory({Instrument{*Symbol::parse("AAPL"), 0, std::nullopt, std::nullopt, std::nullopt}}, 1);
    Shard shard(directory, 0, BookCapacity{1, 1, 1}, 40);
    Doorbell doorbell;
    Ring<CommandElement> commands(1024);
    Ring<ResponseRecord> responses(4096);
    MatchingLoop loop(shard, doorbell, {RingPair{&commands, &responses}});

    failAllocations = true;
    for (OrderId id = 1; id <= 1000; ++id) {
        replay::AddCommand add{};
        add.orderId = id;
        add.side = Side::BUY;
        add.price = 100 + id;
        add.quantity = 1;
        commands.tryPush(CommandElement{id, add, 0, 0, ControlKind::COMMAND});
    }
    while (loop.pass() != 0) {
    }
    std::cerr << "FAILED: a book past its capacity should have needed the heap" << std::endl;
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    if constexpr (invariantChecksAfterCommands) {
        std::cout << "SKIPPED: automatic invariant checks are on in this build" << std::endl;
        return EXIT_SUCCESS;
    } else {
#ifdef _MSC_VER
        // The abort is expected: skip the debug CRT's message box and crash report.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        // CTest fails any test killed by a signal, whatever it printed, so turn the expected
        // abort into a normal exit and let the output alone decide.
        std::signal(SIGABRT, [](int) { std::_Exit(EXIT_SUCCESS); });
        return run();
    }
}
