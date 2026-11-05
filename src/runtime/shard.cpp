#include "order_book/runtime/shard.hpp"

#include <stdexcept>

namespace order_book::runtime {

    Shard::Shard(const InstrumentDirectory& directory, std::uint32_t shardNumber, const BookCapacity& defaults,
                 std::uint64_t firstPosition)
        : instruments(&directory), bookSet(directory, shardNumber, defaults), logStart(firstPosition), nextLogPosition(firstPosition) {
        lastResult.reserve(resultTrades, resultEvents);
    }

    namespace {
        // The capacities of everything a result reuses, which only ever grow.
        std::size_t heldCapacity(const replay::CommandResult& result) noexcept {
            return result.submission().getGeneratedTrades().capacity() + result.submission().getEvents().capacity() +
                   result.replacement().getGeneratedTrades().capacity() + result.replacement().getEvents().capacity();
        }
    }  // namespace

    void Shard::startAt(std::uint64_t firstPosition) {
        if (taken != 0) {
            throw std::logic_error("Shard::startAt after a command was taken");
        }
        logStart = firstPosition;
        nextLogPosition = firstPosition;
    }

    void Shard::process(const replay::Command& command, std::uint32_t slot) {
        if constexpr (invariantChecksAfterCommands) {
            progress = ShardProgress{slot, nextLogPosition};
        }
        const std::size_t before = heldCapacity(lastResult);
        bookSet.execute(slot, command, lastResult);
        if (heldCapacity(lastResult) != before) {
            ++growths;
        }
        ++nextLogPosition;
        ++taken;
    }

    std::optional<ShardViolation> Shard::checkInvariants() const {
        for (std::uint32_t slot = 0; slot < bookSet.size(); ++slot) {
            const InvariantResult result = bookSet.book(slot).checkInvariants();
            if (result.violation) {
                return ShardViolation{ShardViolationKind::BOOK, slot, formatInvariantResult(result)};
            }
            const std::optional<BookLocation> location = instruments->find(bookSet.symbol(slot));
            if (location != BookLocation{bookSet.shard(), slot}) {
                return ShardViolation{ShardViolationKind::SLOT_MAPPING, slot,
                                      "slot " + std::to_string(slot) + " does not map back to shard " +
                                          std::to_string(bookSet.shard())};
            }
        }
        if (nextLogPosition != logStart + taken) {
            return ShardViolation{ShardViolationKind::LOG_POSITION, 0,
                                  "next log position " + std::to_string(nextLogPosition) + " is not " +
                                      std::to_string(logStart) + " plus " + std::to_string(taken) + " commands taken"};
        }
        if (durable != nullptr) {
            const std::uint64_t now = durable->load();
            if (now < durableSeen) {
                return ShardViolation{ShardViolationKind::DURABLE_POSITION, 0,
                                      "durable position " + std::to_string(now) + " went back from " + std::to_string(durableSeen)};
            }
            if (now > nextLogPosition) {
                return ShardViolation{ShardViolationKind::DURABLE_POSITION, 0,
                                      "durable position " + std::to_string(now) + " passes the next log position " +
                                          std::to_string(nextLogPosition)};
            }
            durableSeen = now;
        }
        return std::nullopt;
    }

}  // namespace order_book::runtime
