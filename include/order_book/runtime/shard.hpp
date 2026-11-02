#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "order_book/command.hpp"
#include "order_book/invariants.hpp"
#include "order_book/runtime/book_set.hpp"
#include "order_book/runtime/instruments.hpp"

namespace order_book::runtime {

    // What Shard::checkInvariants found broken.
    enum class ShardViolationKind {
        BOOK,               // a book's own check failed
        SLOT_MAPPING,       // a slot does not map back through the directory
        LOG_POSITION        // the next log position is not the first plus the commands taken
    };

    struct ShardViolation {
        ShardViolationKind kind;
        std::uint32_t slot;  // 0 when it names no slot
        std::string message;
    };

    // Where a shard stood when it took a command, for the engine's abort handler.
    struct ShardProgress {
        std::uint32_t slot = 0;
        std::uint64_t position = 0;

        bool operator==(const ShardProgress&) const = default;
    };

    // A plain single-threaded object, with the matching thread a thin loop around it. process
    // takes a command and slot with no origin, so replay and tests call exactly what the matching
    // thread calls. Like the BookSet it holds, it belongs to the thread that built it.
    class Shard {
    public:
        static constexpr std::size_t resultTrades = 512;
        static constexpr std::size_t resultEvents = 1024;

        // directory must outlive this. firstPosition is the log position of the first command.
        Shard(const InstrumentDirectory& directory, std::uint32_t shardNumber, const BookCapacity& defaults = defaultBookCapacity,
              std::uint64_t firstPosition = 0);

        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;

        // Runs one command on this slot's book, leaving its events in result(). slot must be
        // below size(). Checked builds record the slot and position first.
        void process(const replay::Command& command, std::uint32_t slot);

        const replay::CommandResult& result() const noexcept { return lastResult; }
        const BookSet& books() const noexcept { return bookSet; }
        std::size_t size() const noexcept { return bookSet.size(); }

        std::uint64_t nextPosition() const noexcept { return nextLogPosition; }
        std::uint64_t commandsTaken() const noexcept { return taken; }

        // In checked builds, the command process took last or is running. Zeroes otherwise.
        ShardProgress lastProgress() const noexcept { return progress; }

        // Every book's own check, that each slot maps back through the directory to this shard
        // and slot, and that the next log position is the first plus the commands taken.
        std::optional<ShardViolation> checkInvariants() const;

    private:
        friend struct ShardTestAccess;

        const InstrumentDirectory* instruments;
        BookSet bookSet;
        replay::CommandResult lastResult;
        std::uint64_t logStart;
        std::uint64_t nextLogPosition;
        std::uint64_t taken = 0;
        ShardProgress progress;
    };

}  // namespace order_book::runtime
