#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/book_image.hpp"
#include "order_book/image_header.hpp"
#include "order_book/symbol.hpp"

// The checkpoint file (docs/checkpoint-format.md, ADR 0009): one book's image wrapped with its
// symbol, its shard, the log position it stands at, and a trailing XXH64. A pure codec over bytes:
// it never touches a file, and it judges the wrapper only, since the image is the matching
// library's to judge (OrderBook::restore).
namespace order_book::runtime {

    inline constexpr std::array<char, 8> checkpointMagic{'O', 'B', 'C', 'K', 'P', 'T', '\r', '\n'};
    inline constexpr std::uint32_t checkpointFormatVersion = 1;
    inline constexpr std::size_t checkpointHeaderBytes = 64;  // the fixed part
    inline constexpr std::size_t checkpointChecksumBytes = 8;

    // What a checkpoint says about its image. The versions and sizes are the codec's own.
    struct CheckpointHeader {
        // The writing engine's version: recorded, not enforced.
        std::uint16_t engineMajor = 0;
        std::uint16_t engineMinor = 0;
        std::uint16_t enginePatch = 0;
        // The placement the image's position belongs to: a position means something only in its
        // own shard's log.
        std::uint32_t shardCount = 0;
        std::uint32_t shardIndex = 0;
        // Every command for this symbol before it is in the image, and none at or after it.
        std::uint64_t position = 0;
        replay::Symbol symbol;

        bool operator==(const CheckpointHeader&) const = default;
    };

    // A file whose wrapper decoded: its header, and its image as a view into the bytes decoded.
    // Not yet a checkpoint in the glossary's sense: nothing has proved the image restores.
    struct DecodedCheckpoint {
        CheckpointHeader header;
        std::span<const std::byte> image;
    };

    // Why decodeCheckpoint refused a file. An open set: codes are only ever appended.
    enum class CheckpointErrorCode {
        BAD_MAGIC,            // the first 8 bytes are not OBCKPT\r\n
        TOO_SHORT,            // fewer bytes than a header and a checksum
        CHECKSUM_MISMATCH,    // the trailing XXH64 is not that of the bytes before it
        UNSUPPORTED_VERSION,  // a format version this reader cannot read
        SIZE_BELOW_KNOWN,     // a stated header size below this version's
        LENGTH_MISMATCH,      // a header, image, or section that runs past the checksum, or stops short of it
        NONZERO_RESERVED,     // reserved bytes that are not 0
        INVALID_SHARD,        // a shard count of 0, or a shard index not below it
        INVALID_SYMBOL        // symbol bytes that are not a symbol
    };

    constexpr const char* checkpointErrorCodeName(CheckpointErrorCode code) {
        switch(code) {
            case CheckpointErrorCode::BAD_MAGIC: return "BAD_MAGIC";
            case CheckpointErrorCode::TOO_SHORT: return "TOO_SHORT";
            case CheckpointErrorCode::CHECKSUM_MISMATCH: return "CHECKSUM_MISMATCH";
            case CheckpointErrorCode::UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
            case CheckpointErrorCode::SIZE_BELOW_KNOWN: return "SIZE_BELOW_KNOWN";
            case CheckpointErrorCode::LENGTH_MISMATCH: return "LENGTH_MISMATCH";
            case CheckpointErrorCode::NONZERO_RESERVED: return "NONZERO_RESERVED";
            case CheckpointErrorCode::INVALID_SHARD: return "INVALID_SHARD";
            case CheckpointErrorCode::INVALID_SYMBOL: return "INVALID_SYMBOL";
        }
        return "UNKNOWN";
    }

    struct CheckpointError {
        CheckpointErrorCode code;
        // The first byte of the field at fault, or the file's length when it is the length.
        std::size_t offset = 0;
        std::string message;
    };

    // The whole file, format 1: the header, the image as given, no section, and the checksum. It
    // writes the header as given, so a header decodeCheckpoint would refuse makes a file it
    // refuses.
    std::vector<std::byte> encodeCheckpoint(const CheckpointHeader& header, std::span<const std::byte> image);

    // Checks the wrapper and returns its header and image, or the first fault, in the order
    // docs/checkpoint-format.md gives. It checks the image's length but not its bytes: restore
    // does that. Unknown trailing header bytes and sections are skipped.
    std::variant<DecodedCheckpoint, CheckpointError> decodeCheckpoint(std::span<const std::byte> file);

    // What order_book_cli checkpoint verify reports: the wrapper's fault, or restore's, its offset
    // moved into the file.
    struct CheckpointFault {
        using Code = std::variant<CheckpointErrorCode, RestoreErrorCode>;
        Code code;
        std::size_t offset = 0;  // in the file
        std::string message;
    };

    // decodeCheckpoint, then OrderBook::restore on the image, which is destroyed here: nothing if
    // the file would restore. Lets std::bad_alloc propagate. Prints nothing.
    std::optional<CheckpointFault> verifyCheckpoint(std::span<const std::byte> file);

    // "<path>: <offset>: <code>: <message>", the code as its enumerator's name.
    std::string formatCheckpointFault(std::string_view path, const CheckpointFault& fault);

    // Both headers as stored, for order_book_cli checkpoint describe: nothing past the magic is
    // checked, so a damaged file still describes.
    struct CheckpointDescription {
        std::size_t fileBytes = 0;
        std::uint32_t formatVersion = 0;
        std::uint32_t minimumReaderVersion = 0;
        std::uint32_t headerBytes = 0;
        std::uint16_t engineMajor = 0;
        std::uint16_t engineMinor = 0;
        std::uint16_t enginePatch = 0;
        std::uint16_t reservedAt26 = 0;
        std::uint32_t shardCount = 0;
        std::uint32_t shardIndex = 0;
        std::uint32_t reservedAt36 = 0;
        std::uint64_t position = 0;
        std::array<char, replay::Symbol::size> symbol{};
        std::uint64_t imageBytes = 0;
        // Read at the stated header bytes, if the file holds an image header there.
        std::optional<ImageHeader> image;
    };

    // BAD_MAGIC, or TOO_SHORT for fewer bytes than the fixed header; nothing else is refused.
    std::variant<CheckpointDescription, CheckpointError> describeCheckpoint(std::span<const std::byte> file);

    // One "key: value" line per field, the image's after the wrapper's, ending
    // "checksum: not checked".
    std::string formatCheckpointDescription(const CheckpointDescription& description);

}  // namespace order_book::runtime
