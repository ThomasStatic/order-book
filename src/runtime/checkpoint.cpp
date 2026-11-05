#include "order_book/runtime/checkpoint.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "order_book/little_endian.hpp"
#include "order_book/order_book.hpp"
#include "order_book/xxh64.hpp"

namespace order_book::runtime {

    namespace {
        using little_endian::load16;
        using little_endian::load32;
        using little_endian::load64;
        using little_endian::store16;
        using little_endian::store32;
        using little_endian::store64;

        namespace field {
            constexpr std::size_t magic = 0;
            constexpr std::size_t formatVersion = 8;
            constexpr std::size_t minimumReader = 12;
            constexpr std::size_t headerSize = 16;
            constexpr std::size_t engineMajor = 20;
            constexpr std::size_t engineMinor = 22;
            constexpr std::size_t enginePatch = 24;
            constexpr std::size_t reserved16 = 26;
            constexpr std::size_t shardCount = 28;
            constexpr std::size_t shardIndex = 32;
            constexpr std::size_t reserved32 = 36;
            constexpr std::size_t position = 40;
            constexpr std::size_t symbol = 48;
            constexpr std::size_t imageSize = 56;
        }  // namespace field

        // Checked first by decode and describe alike, so a file of another kind, or a text-mode
        // copy, fails at once.
        std::optional<CheckpointError> wrongMagic(std::span<const std::byte> file) {
            if (file.size() >= checkpointMagic.size() &&
                std::memcmp(file.data(), checkpointMagic.data(), checkpointMagic.size()) == 0) {
                return std::nullopt;
            }
            return CheckpointError{CheckpointErrorCode::BAD_MAGIC, field::magic, "the file does not start with OBCKPT\\r\\n"};
        }
    }  // namespace

    std::vector<std::byte> encodeCheckpoint(const CheckpointHeader& header, std::span<const std::byte> image) {
        std::vector<std::byte> file(checkpointHeaderBytes + image.size() + checkpointChecksumBytes);
        const std::span<std::byte> out(file);
        std::memcpy(out.data() + field::magic, checkpointMagic.data(), checkpointMagic.size());
        store32(out, field::formatVersion, checkpointFormatVersion);
        store32(out, field::minimumReader, checkpointFormatVersion);
        store32(out, field::headerSize, static_cast<std::uint32_t>(checkpointHeaderBytes));
        store16(out, field::engineMajor, header.engineMajor);
        store16(out, field::engineMinor, header.engineMinor);
        store16(out, field::enginePatch, header.enginePatch);
        store32(out, field::shardCount, header.shardCount);
        store32(out, field::shardIndex, header.shardIndex);
        store64(out, field::position, header.position);
        const std::array<char, replay::Symbol::size> symbol = header.symbol.bytes();
        std::memcpy(out.data() + field::symbol, symbol.data(), symbol.size());
        store64(out, field::imageSize, image.size());
        std::copy(image.begin(), image.end(), file.begin() + checkpointHeaderBytes);
        const std::size_t body = checkpointHeaderBytes + image.size();
        store64(out, body, replay::xxh64(std::span<const std::byte>(file).first(body)));
        return file;
    }

    std::variant<DecodedCheckpoint, CheckpointError> decodeCheckpoint(std::span<const std::byte> file) {
        const auto refusal = [](CheckpointErrorCode code, std::size_t offset, std::string message) {
            return CheckpointError{code, offset, std::move(message)};
        };

        if (std::optional<CheckpointError> error = wrongMagic(file)) {
            return std::move(*error);
        }
        if (file.size() < checkpointHeaderBytes + checkpointChecksumBytes) {
            return refusal(CheckpointErrorCode::TOO_SHORT, file.size(), "the file is shorter than a header and a checksum");
        }
        // Then the checksum, before any field is trusted, so a truncation and a bad length field
        // fail the same check.
        const std::size_t checksumAt = file.size() - checkpointChecksumBytes;
        if (replay::xxh64(file.first(checksumAt)) != load64(file, checksumAt)) {
            return refusal(CheckpointErrorCode::CHECKSUM_MISMATCH, checksumAt,
                           "the checksum is not that of the bytes before it");
        }

        const std::uint32_t formatVersion = load32(file, field::formatVersion);
        const std::uint32_t minimumReader = load32(file, field::minimumReader);
        if (formatVersion == 0) {
            return refusal(CheckpointErrorCode::UNSUPPORTED_VERSION, field::formatVersion, "format version 0 is not a version");
        }
        // Any format version at or above 1 that a reader of version 1 may read.
        if (minimumReader != checkpointFormatVersion) {
            return refusal(CheckpointErrorCode::UNSUPPORTED_VERSION, field::minimumReader,
                           "minimum reader version " + std::to_string(minimumReader) + " of format version " +
                               std::to_string(formatVersion) + " is not one this reader, version 1, can read");
        }
        const std::uint32_t headerBytes = load32(file, field::headerSize);
        if (headerBytes < checkpointHeaderBytes) {
            return refusal(CheckpointErrorCode::SIZE_BELOW_KNOWN, field::headerSize,
                           "a header of " + std::to_string(headerBytes) + " bytes is below this version's 64");
        }
        if (headerBytes > checksumAt) {
            return refusal(CheckpointErrorCode::LENGTH_MISMATCH, field::headerSize,
                           "a header of " + std::to_string(headerBytes) + " bytes runs past the checksum at " +
                               std::to_string(checksumAt));
        }
        const std::uint64_t imageBytes = load64(file, field::imageSize);
        if (imageBytes > checksumAt - headerBytes) {
            return refusal(CheckpointErrorCode::LENGTH_MISMATCH, field::imageSize,
                           "an image of " + std::to_string(imageBytes) + " bytes at " + std::to_string(headerBytes) +
                               " runs past the checksum at " + std::to_string(checksumAt));
        }
        // Sections fill the rest exactly. Format 1 knows no type, so each is skipped by its length.
        constexpr std::size_t sectionHeaderBytes = 16;
        const std::size_t imageEnd = headerBytes + static_cast<std::size_t>(imageBytes);
        std::optional<std::size_t> reservedSection;
        for (std::size_t at = imageEnd; at < checksumAt;) {
            if (checksumAt - at < sectionHeaderBytes) {
                return refusal(CheckpointErrorCode::LENGTH_MISMATCH, at,
                               std::to_string(checksumAt - at) + " bytes before the checksum are too few for a section");
            }
            const std::uint64_t length = load64(file, at + 8);
            if (length > checksumAt - at - sectionHeaderBytes) {
                return refusal(CheckpointErrorCode::LENGTH_MISMATCH, at + 8,
                               "a section of " + std::to_string(length) + " bytes runs past the checksum at " +
                                   std::to_string(checksumAt));
            }
            if (!reservedSection && load32(file, at + 4) != 0) {
                reservedSection = at + 4;
            }
            at += sectionHeaderBytes + static_cast<std::size_t>(length);
        }

        // The header's own fields, then the sections', in the order they lie.
        if (load16(file, field::reserved16) != 0) {
            return refusal(CheckpointErrorCode::NONZERO_RESERVED, field::reserved16, "the header's reserved bytes at 26 are not 0");
        }
        const std::uint32_t shardCount = load32(file, field::shardCount);
        if (shardCount == 0) {
            return refusal(CheckpointErrorCode::INVALID_SHARD, field::shardCount, "a shard count of 0");
        }
        const std::uint32_t shardIndex = load32(file, field::shardIndex);
        if (shardIndex >= shardCount) {
            return refusal(CheckpointErrorCode::INVALID_SHARD, field::shardIndex,
                           "shard index " + std::to_string(shardIndex) + " is not below the shard count " +
                               std::to_string(shardCount));
        }
        if (load32(file, field::reserved32) != 0) {
            return refusal(CheckpointErrorCode::NONZERO_RESERVED, field::reserved32, "the header's reserved bytes at 36 are not 0");
        }
        std::array<char, replay::Symbol::size> symbol;
        std::memcpy(symbol.data(), file.data() + field::symbol, symbol.size());
        const std::optional<replay::Symbol> parsed = replay::Symbol::fromBytes(symbol);
        if (!parsed) {
            return refusal(CheckpointErrorCode::INVALID_SYMBOL, field::symbol,
                           "the symbol bytes are not 1 to 8 of A-Z, 0-9, '.', '-', padded with spaces");
        }
        if (reservedSection) {
            return refusal(CheckpointErrorCode::NONZERO_RESERVED, *reservedSection, "a section's reserved bytes are not 0");
        }

        DecodedCheckpoint checkpoint;
        CheckpointHeader& header = checkpoint.header;
        header.engineMajor = load16(file, field::engineMajor);
        header.engineMinor = load16(file, field::engineMinor);
        header.enginePatch = load16(file, field::enginePatch);
        header.shardCount = shardCount;
        header.shardIndex = shardIndex;
        header.position = load64(file, field::position);
        header.symbol = *parsed;
        checkpoint.image = file.subspan(headerBytes, static_cast<std::size_t>(imageBytes));
        return checkpoint;
    }

    std::optional<CheckpointFault> verifyCheckpoint(std::span<const std::byte> file) {
        const std::variant<DecodedCheckpoint, CheckpointError> decoded = decodeCheckpoint(file);
        if (const auto* error = std::get_if<CheckpointError>(&decoded)) {
            return CheckpointFault{error->code, error->offset, error->message};
        }
        const std::span<const std::byte> image = std::get<DecodedCheckpoint>(decoded).image;
        std::variant<OrderBook, RestoreError> restored = OrderBook::restore(image);
        if (auto* error = std::get_if<RestoreError>(&restored)) {
            const auto imageAt = static_cast<std::size_t>(image.data() - file.data());
            return CheckpointFault{error->code, imageAt + error->offset, std::move(error->message)};
        }
        return std::nullopt;
    }

    std::string formatCheckpointFault(std::string_view path, const CheckpointFault& fault) {
        const char* code = std::visit(
            [](auto value) {
                if constexpr (std::is_same_v<decltype(value), CheckpointErrorCode>) {
                    return checkpointErrorCodeName(value);
                } else {
                    return restoreErrorCodeName(value);
                }
            },
            fault.code);
        return std::string(path) + ": " + std::to_string(fault.offset) + ": " + code + ": " + fault.message;
    }

    std::variant<CheckpointDescription, CheckpointError> describeCheckpoint(std::span<const std::byte> file) {
        if (std::optional<CheckpointError> error = wrongMagic(file)) {
            return std::move(*error);
        }
        if (file.size() < checkpointHeaderBytes) {
            return CheckpointError{CheckpointErrorCode::TOO_SHORT, file.size(), "the file is shorter than a header"};
        }
        CheckpointDescription description;
        description.fileBytes = file.size();
        description.formatVersion = load32(file, field::formatVersion);
        description.minimumReaderVersion = load32(file, field::minimumReader);
        description.headerBytes = load32(file, field::headerSize);
        description.engineMajor = load16(file, field::engineMajor);
        description.engineMinor = load16(file, field::engineMinor);
        description.enginePatch = load16(file, field::enginePatch);
        description.shardCount = load32(file, field::shardCount);
        description.shardIndex = load32(file, field::shardIndex);
        description.reservedAt26 = load16(file, field::reserved16);
        description.reservedAt36 = load32(file, field::reserved32);
        description.position = load64(file, field::position);
        std::memcpy(description.symbol.data(), file.data() + field::symbol, description.symbol.size());
        description.imageBytes = load64(file, field::imageSize);
        if (description.headerBytes <= file.size()) {
            description.image = readImageHeader(file.subspan(description.headerBytes));
        }
        return description;
    }

    std::string formatCheckpointDescription(const CheckpointDescription& description) {
        std::string text;
        const auto line = [&text](std::string_view key, const std::string& value) {
            text.append(key).append(": ").append(value).append("\n");
        };
        const auto number = [&line](std::string_view key, std::uint64_t value) { line(key, std::to_string(value)); };

        number("file-bytes", description.fileBytes);
        number("format-version", description.formatVersion);
        number("minimum-reader-version", description.minimumReaderVersion);
        number("header-bytes", description.headerBytes);
        line("engine-version", std::to_string(description.engineMajor) + "." + std::to_string(description.engineMinor) +
                                   "." + std::to_string(description.enginePatch));
        number("reserved-at-26", description.reservedAt26);
        number("shard-count", description.shardCount);
        number("shard-index", description.shardIndex);
        number("reserved-at-36", description.reservedAt36);
        number("position", description.position);
        if (const std::optional<replay::Symbol> symbol = replay::Symbol::fromBytes(description.symbol)) {
            line("symbol", symbol->text());
        } else {
            // Its bytes, since they may be anything, a terminal's control codes among them.
            constexpr char digits[] = "0123456789abcdef";
            std::string bytes = "not a symbol:";
            for (const char stored : description.symbol) {
                const auto value = static_cast<unsigned char>(stored);
                bytes += ' ';
                bytes += digits[value >> 4];
                bytes += digits[value & 0x0FU];
            }
            line("symbol", bytes);
        }
        number("image-bytes", description.imageBytes);
        if (const std::optional<ImageHeader>& image = description.image) {
            number("image-version", image->imageVersion);
            number("image-minimum-reader-version", image->minimumReaderVersion);
            number("image-header-bytes", image->headerBytes);
            number("level-record-bytes", image->levelRecordBytes);
            number("order-record-bytes", image->orderRecordBytes);
            number("stop-record-bytes", image->stopRecordBytes);
            number("next-sequence-number", image->nextSequenceNumber);
            number("next-trade-id", image->nextTradeId);
            number("last-trade-price", image->lastTradePrice);
            number("bid-levels", image->bidLevels);
            number("ask-levels", image->askLevels);
            number("orders", image->orders);
            number("stops", image->stops);
            number("order-index-size", image->orderIndexSize);
            number("stop-index-size", image->stopIndexSize);
            number("image-reserved", image->reserved);
        } else {
            line("image-header", "past the end of the file");
        }
        line("checksum", "not checked");
        return text;
    }

}  // namespace order_book::runtime
