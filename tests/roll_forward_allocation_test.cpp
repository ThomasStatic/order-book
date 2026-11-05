#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/roll_forward.hpp"

// A bad_alloc anywhere in a roll-forward pass (spec #58's D4b, #35): each allocation of a roll is
// failed in turn, and the roll must count a lost pass, publish only true checkpoints, and leave a
// store from which the next roll catches up to an uninterrupted run byte for byte. Under MSVC's
// debug iterators, which allocate inside noexcept members, failing one terminates, so the test
// skips there. See docs/internals/testing.md.
namespace {
std::atomic<long> allocationsBeforeFailure{-1};  // negative = disarmed
std::atomic<bool> failed{false};

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool iteratorDebugging = true;
#else
constexpr bool iteratorDebugging = false;
#endif
constexpr int skipped = 77;
}  // namespace

void* operator new(std::size_t size) {
    long left = allocationsBeforeFailure.load();
    while (left >= 0) {
        if (allocationsBeforeFailure.compare_exchange_weak(left, left - 1)) {
            if (left == 0) {
                failed.store(true);
                throw std::bad_alloc();
            }
            break;
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

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

namespace {

using namespace order_book;
using namespace order_book::runtime;
using Bytes = std::vector<std::byte>;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        allocationsBeforeFailure.store(-1);
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

struct Placed {
    std::uint32_t slot;
    replay::Command command;
};

// Adds on slots 0 and 1 that trade, and a cancel of every fifth order; slot 2 has none.
std::vector<Placed> commandsFor(std::size_t count) {
    std::vector<Placed> placed;
    for (std::size_t i = 1; i <= count; ++i) {
        // After the first 30, MSFT gets none: a book with resting orders that later passes rewrap.
        const auto slot = static_cast<std::uint32_t>(i <= 30 ? i % 2 : 0);
        if (i % 5 == 0) {
            placed.push_back(Placed{slot, replay::CancelCommand{static_cast<OrderId>(i - 2)}});
            continue;
        }
        replay::AddCommand add{};
        add.orderId = static_cast<OrderId>(i);
        add.side = i % 2 == 0 ? Side::BUY : Side::SELL;
        add.price = static_cast<Price>(100 + (i * 7) % 5);
        add.quantity = static_cast<Quantity>(1 + i % 3);
        placed.push_back(Placed{slot, add});
    }
    return placed;
}

std::map<std::string, Bytes> filesOf(const InMemoryLogStorage& storage) {
    std::map<std::string, Bytes> files;
    for (const std::string& name : storage.names()) {
        files.emplace(name, storage.read(name, 0));
    }
    return files;
}

// Each checkpoint a store holds is one the start held or the uninterrupted run published; a .tmp
// may stand, for the next roller to remove.
void expectCheckpointsSound(const InMemoryLogStorage& storage, const std::map<std::string, Bytes>& before,
                            const std::map<std::string, Bytes>& reference, const std::string& what) {
    for (const std::string& name : storage.names()) {
        const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
        if (!parts || parts->temporary) {
            continue;
        }
        const Bytes held = storage.read(name, 0);
        const bool known = (reference.contains(name) && held == reference.at(name)) || (before.contains(name) && held == before.at(name));
        expect(known, what + ": " + name + " is the start's or the uninterrupted run's");
    }
}

}  // namespace

int main() {
    if (iteratorDebugging) {
        std::cout << "roll_forward_allocation_tests skipped: MSVC's debug iterators allocate in noexcept members\n";
        return skipped;
    }
    try {
        const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0), on("QUIET", 0), on("IBM", 1)}, 2);
        const std::vector<Placed> commands = commandsFor(100);
        InMemoryLogStorage logged;
        {
            const std::uint64_t fileBytes =
                logFileShape(directory, 0, Durability::MACHINE, defaultLogFileBytes).headerBytes + 25 * replay::logRecordBytes;
            Housekeeping housekeeping(logged, directory, Durability::MACHINE, fileBytes);
            Ring<LogEntry> ring(4096);
            DurablePosition durable(0);
            LogWriter writer(housekeeping, 0, ring, durable, 0);
            for (const Placed& placed : commands) {
                expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "room in the log ring");
            }
            while (writer.pass() != 0 || housekeeping.pass() != 0) {
            }
        }
        const auto noReport = [](const std::string&) {};

        // Two of the four files rolled first, so the failing roll restores books from checkpoints
        // and rewraps a quiet one.
        InMemoryLogStorage start(logged.contents());
        ShardRoller(start, directory, 0, noReport).roll(50);
        InMemoryLogStorage uninterrupted(start.contents());
        ShardRoller(uninterrupted, directory, 0, noReport).roll(100);
        const std::map<std::string, Bytes> reference = filesOf(uninterrupted);

        long failures = 0;
        for (long failAt = 0;; ++failAt) {
            InMemoryLogStorage storage(start.contents());
            ShardRoller roller(storage, directory, 0, noReport);
            failed.store(false);
            allocationsBeforeFailure.store(failAt);
            roller.roll(100);
            allocationsBeforeFailure.store(-1);
            const std::string what = "allocation " + std::to_string(failAt) + " failed";
            if (!failed.load()) {
                expect(roller.counts().passesLost == 0 && filesOf(storage) == reference, "a roll that fails nothing is the reference");
                break;
            }
            ++failures;
            expect(roller.counts().passesLost == 1, what + ": the roll counts one lost pass");
            expectCheckpointsSound(storage, filesOf(start), reference, what);
            roller.roll(100);
            expect(roller.counts().passesLost == 1 && roller.filesBehind() == 0, what + ": the next roll catches up");
            InMemoryLogStorage restarted(storage.contents());
            ShardRoller(restarted, directory, 0, noReport);  // removes any .tmp, as a start does
            expect(filesOf(restarted) == reference, what + ": byte for byte");
            expect(failAt < 100000, "a roll makes a bounded number of allocations");
        }
        expect(failures > 100, "a roll makes many allocations to fail: " + std::to_string(failures));
        std::cout << "roll_forward_allocation_tests passed: " << failures << " allocations failed in turn\n";
    } catch (const std::exception& error) {
        allocationsBeforeFailure.store(-1);
        std::cerr << "FAILED: unexpected exception: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
