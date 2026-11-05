#include "order_book/runtime/clean_stop.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

#include "order_book/order_book.hpp"
#include "order_book/runtime/roll_forward.hpp"

namespace order_book::runtime {

    LiveImages captureLiveImages(const Shard& shard, bool images) noexcept {
        std::optional<ShardViolation> violation;
        try {
            violation = shard.checkInvariants();
        } catch (const std::bad_alloc&) {
            // A sound shard's check allocates nothing: only describing a violation does.
            std::fprintf(stderr, "order book runtime: clean stop: shard %u: the live book fails its check, and memory ran out describing it\n",
                         static_cast<unsigned>(shard.books().shard()));
            std::fflush(stderr);
            std::abort();
        }
        try {
            if (violation) {
                const bool namesABook = violation->kind == ShardViolationKind::BOOK || violation->kind == ShardViolationKind::SLOT_MAPPING;
                std::fprintf(stderr, "order book runtime: clean stop: shard %u%s%s: the live book fails its check: %s\n",
                             static_cast<unsigned>(shard.books().shard()), namesABook ? " " : "",
                             namesABook ? shard.books().symbol(violation->slot).text().c_str() : "", violation->message.c_str());
                std::fflush(stderr);
                std::abort();
            }
            if (!images) {
                return std::nullopt;
            }
            std::vector<std::vector<std::byte>> written(shard.size());
            for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
                const OrderBook& book = shard.books().book(slot);
                written[slot].resize(book.imageSize());
                static_cast<void>(book.writeImage(written[slot]));
            }
            return written;
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    std::string liveImageFileName(const Symbol& symbol, std::uint64_t position) {
        // The checkpoint's own name, another extension.
        constexpr std::string_view checkpointExtension = ".obckpt";
        std::string name = checkpointFileName(symbol, position);
        name.resize(name.size() - checkpointExtension.size());
        return name + ".live";
    }

    std::optional<std::string> formatStoppedBook(const Symbol& symbol, std::uint64_t logEnd, const StoppedBook& book) {
        const std::string lead = "clean stop incomplete: " + symbol.text();
        const std::string tail = "; the next start recovers from the log";
        switch (book.outcome) {
            case StopOutcome::PUBLISHED:
                return std::nullopt;
            case StopOutcome::DIFFERS:
                return lead + " differs at byte " + std::to_string(book.differsAt) +
                       (book.saved ? ", saved " : ", and could not save ") + liveImageFileName(symbol, logEnd) + tail;
            case StopOutcome::NO_IMAGE:
                return lead + "'s image buffer could not be allocated" + tail;
            case StopOutcome::NOT_ROLLED:
                return lead + " could not be rolled forward to the log's end, " + std::to_string(logEnd) + tail;
            case StopOutcome::PUBLISH_FAILED:
                return lead + "'s checkpoint at " + std::to_string(logEnd) + " could not be published" + tail;
        }
        return lead + tail;
    }

}  // namespace order_book::runtime
