#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "order_book/book_event.hpp"
#include "order_book/command.hpp"

// How commands reach a shard and events come back (ADR 0011). Not public API.
namespace order_book::runtime {

    inline constexpr std::size_t defaultCommandRingSlots = 4096;
    inline constexpr std::size_t defaultResponseRingSlots = 16384;

    // A bounded ring with one writer thread and one reader thread, lock-free. The two sides'
    // indices sit on cache lines of their own, and each side keeps its last view of the other's,
    // so it touches the shared line only when that view runs out. Indices count up for ever, and
    // a slot is an index modulo the capacity, so full and empty need no spare slot. Allocates only
    // when built.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the alignment asked for, which is the point
#endif
    template <typename Item>
    class Ring {
        static_assert(std::is_trivially_copyable_v<Item>, "a ring's items cross threads by copy");

    public:
        // capacity must be a power of two; anything else throws std::invalid_argument.
        explicit Ring(std::size_t capacity) : slots(capacity), mask(capacity - 1), items(makeItems(capacity)) {}

        Ring(const Ring&) = delete;
        Ring& operator=(const Ring&) = delete;

        std::size_t capacity() const noexcept { return slots; }

        // Writer only. False, with nothing done, if the ring is full.
        bool tryPush(const Item& item) noexcept {
            const std::size_t tail = writer.index.load(std::memory_order_relaxed);
            if (tail - writer.otherSide == slots) {
                writer.otherSide = reader.index.load(std::memory_order_acquire);
                if (tail - writer.otherSide == slots) {
                    return false;
                }
            }
            items[tail & mask] = item;
            writer.index.store(tail + 1, std::memory_order_release);
            return true;
        }

        // Reader only. Whether anything is waiting right now.
        bool hasItems() const noexcept {
            return writer.index.load(std::memory_order_acquire) != reader.index.load(std::memory_order_relaxed);
        }

        // Reader only. Calls take(item) on up to limit items, oldest first, and returns how many.
        // Their slots go back to the writer together, once take has seen them all.
        template <typename Take>
        std::size_t drain(std::size_t limit, Take&& take) {
            const std::size_t head = reader.index.load(std::memory_order_relaxed);
            std::size_t available = reader.otherSide - head;
            if (available < limit) {
                reader.otherSide = writer.index.load(std::memory_order_acquire);
                available = reader.otherSide - head;
            }
            const std::size_t count = available < limit ? available : limit;
            for (std::size_t i = 0; i < count; ++i) {
                take(static_cast<const Item&>(items[(head + i) & mask]));
            }
            if (count != 0) {
                reader.index.store(head + count, std::memory_order_release);
            }
            return count;
        }

    private:
        struct alignas(64) Side {
            std::atomic<std::size_t> index{0};
            std::size_t otherSide = 0;  // this side's last view of the other's index
        };

        static std::unique_ptr<Item[]> makeItems(std::size_t capacity) {
            if (!std::has_single_bit(capacity)) {
                throw std::invalid_argument("a ring's capacity must be a power of two");
            }
            return std::make_unique<Item[]>(capacity);
        }

        const std::size_t slots;
        const std::size_t mask;
        const std::unique_ptr<Item[]> items;
        Side writer;
        Side reader;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    // Sits outside Command, and the log accepts only a Command, so a stop marker can't be logged.
    enum class ControlKind : std::uint8_t { COMMAND, STOP };

    // One ring element towards a shard. The ring it came on names the ring its response takes.
    struct CommandElement {
        std::uint64_t correlation;
        replay::Command command;
        std::uint32_t slot;
        std::uint32_t connection;  // an index plus a generation, so a reused index hears nothing
        ControlKind control;

        // A producer's last element: it will send nothing more.
        static constexpr CommandElement stopMarker() noexcept {
            return CommandElement{0, replay::CancelCommand{0}, 0, 0, ControlKind::STOP};
        }
    };
    static_assert(sizeof(CommandElement) == 64, "a command element fills one cache line");
    static_assert(std::is_trivially_copyable_v<CommandElement>, "a command element crosses threads by copy");

    // SHARD_STOPPED carries no event: it says this ring will hold nothing more.
    enum class ResponseKind : std::uint8_t { EVENT, LAST_EVENT, SHARD_STOPPED };

    // One event on its way back. Results never cross threads, and a response larger than the ring
    // streams through it.
    struct ResponseRecord {
        std::uint64_t correlation;
        std::uint32_t connection;
        std::uint32_t slot;
        BookEvent event;
        ResponseKind kind;
    };
    static_assert(sizeof(ResponseRecord) == 48, "a response record is one fixed size");
    static_assert(std::is_trivially_copyable_v<ResponseRecord>, "a response record crosses threads by copy");

}  // namespace order_book::runtime
