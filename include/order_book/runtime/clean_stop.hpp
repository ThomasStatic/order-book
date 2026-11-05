#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "order_book/runtime/shard.hpp"
#include "order_book/symbol.hpp"

// A clean stop's last three steps (spec #58's D6): the matching thread checks its live books and
// writes their images, and the roll forward compares each with what the log rebuilds, byte for
// byte, publishing only a book that matches. That comparison is the determinism check a Release
// build has. Not public API. See docs/internals/runtime.md.
namespace order_book::runtime {

    // Each live book's image, by slot, or nothing when the buffers could not be allocated.
    using LiveImages = std::optional<std::vector<std::vector<std::byte>>>;

    // Step 3, on the matching thread once its loop has stopped: Shard::checkInvariants in full,
    // aborting on a violation with "order book runtime: clean stop: shard <n>[ <symbol>]: the live
    // book fails its check: <why>", since a book's index and level handles are in no image, so no
    // comparison can see them; then each book's image, into buffers allocated here. A bad_alloc in
    // the buffers gives nothing, and nothing is thrown; one in the check aborts too, since a sound
    // shard's check allocates nothing and only describing a violation does. With images false,
    // the check alone, as an engine with no log needs.
    LiveImages captureLiveImages(const Shard& shard, bool images = true) noexcept;

    // What a clean stop did with one book.
    enum class StopOutcome {
        NOT_ROLLED,      // the roll forward did not reach the log's end: nothing published
        PUBLISHED,       // what the log rebuilds matches the live book, and stands at the log's end
        DIFFERS,         // they differ: nothing published, and the live image saved beside the checkpoints
        NO_IMAGE,        // the live image could not be allocated: nothing published
        PUBLISH_FAILED,  // they matched, and the checkpoint could not be published
    };

    struct StoppedBook {
        StopOutcome outcome = StopOutcome::NOT_ROLLED;
        std::uint64_t differsAt = 0;  // DIFFERS: the first byte at which the two images differ
        bool saved = false;           // DIFFERS: the live image was saved

        bool operator==(const StoppedBook&) const = default;
    };

    // ckpt-<symbol>-<position, 20 digits>.live, #34's name for a live image that differed. It is
    // in the checkpoint format, so `checkpoint describe` and `verify` read it, and no start reads
    // it, since its name is not a checkpoint's.
    std::string liveImageFileName(const Symbol& symbol, std::uint64_t position);

    // The diagnostic for a book not published, without the engine's prefix, or nothing for one
    // that was, such as "clean stop incomplete: AAPL differs at byte 812, saved
    // ckpt-AAPL-00000000000000000120.live; the next start recovers from the log".
    std::optional<std::string> formatStoppedBook(const Symbol& symbol, std::uint64_t logEnd, const StoppedBook& book);

}  // namespace order_book::runtime
