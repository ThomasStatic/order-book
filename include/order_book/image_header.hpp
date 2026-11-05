#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// A book image's header as stored, for a tool that describes an image without restoring it
// (order_book_cli checkpoint describe). Implementation detail, not public API: it follows the
// image format (docs/checkpoint-format.md), whose header may grow, and may change in any release.
namespace order_book {

    struct ImageHeader {
        std::uint32_t imageVersion = 0;
        std::uint32_t minimumReaderVersion = 0;
        std::uint32_t headerBytes = 0;
        std::uint32_t levelRecordBytes = 0;
        std::uint32_t orderRecordBytes = 0;
        std::uint32_t stopRecordBytes = 0;
        std::uint32_t nextSequenceNumber = 0;
        std::uint32_t nextTradeId = 0;
        std::uint32_t lastTradePrice = 0;
        std::uint32_t bidLevels = 0;
        std::uint32_t askLevels = 0;
        std::uint32_t orders = 0;
        std::uint32_t stops = 0;
        std::uint32_t orderIndexSize = 0;
        std::uint32_t stopIndexSize = 0;
        std::uint32_t reserved = 0;

        bool operator==(const ImageHeader&) const = default;
    };

    // The first 64 bytes' fields, checking none of them, or nothing if there are fewer bytes.
    // Only OrderBook::restore judges an image.
    std::optional<ImageHeader> readImageHeader(std::span<const std::byte> image) noexcept;

}  // namespace order_book
