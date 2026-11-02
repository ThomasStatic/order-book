#include "order_book/runtime/shard.hpp"

namespace order_book::runtime {

    Shard::Shard(const InstrumentDirectory& directory, std::uint32_t shardNumber, const BookCapacity& defaults,
                 std::uint64_t firstPosition)
        : instruments(&directory), bookSet(directory, shardNumber, defaults), logStart(firstPosition), nextLogPosition(firstPosition) {
        lastResult.reserve(resultTrades, resultEvents);
    }

    void Shard::process(const replay::Command& command, std::uint32_t slot) {
        if constexpr (invariantChecksAfterCommands) {
            progress = ShardProgress{slot, nextLogPosition};
        }
        bookSet.execute(slot, command, lastResult);
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
        return std::nullopt;
    }

}  // namespace order_book::runtime
