#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "command.hpp"
#include "replay_engine.hpp"
#include "replay_parser.hpp"
#include "symbol.hpp"

// The binary command log (docs/command-log-format.md, ADR 0010): a header, then fixed 40-byte
// little-endian records of well-formed commands, each one's checksum seeded with the one
// before it. A log holds exactly what a v3 CSV can, so the two are interchangeable inputs: a
// record reads as a ReplayRecord with the sequence and line it has in its CSV twin. This is a
// pure codec and a reader. It never writes a file, and it knows nothing of shards.
namespace order_book::replay {

    inline constexpr std::array<char, 8> logMagic{'O', 'B', 'C', 'L', 'O', 'G', '\r', '\n'};
    inline constexpr std::uint32_t logFormatVersion = 1;
    inline constexpr std::size_t logHeaderBytes = 64;  // the fixed part
    inline constexpr std::size_t logRecordBytes = 40;

    // A record after its checksum, which a wire Command frame carries unchanged.
    inline constexpr std::size_t commandBodyBytes = 36;

    inline constexpr std::uint8_t rawCommandAdd = 1;
    inline constexpr std::uint8_t rawCommandCancel = 2;
    inline constexpr std::uint8_t rawCommandReplace = 3;
    inline constexpr std::uint8_t rawFlagPostOnly = 1;

    // A command body's fields, unchecked. The log and the wire share this unpack and then check
    // it their own ways, since they must report the same bad byte differently.
    struct RawCommand {
        std::uint8_t command;
        std::uint8_t side;
        std::uint8_t orderType;
        std::uint8_t timeInForce;
        std::array<char, Symbol::size> symbol;
        OrderId orderId;
        Price price;
        Quantity quantity;
        Price stopPrice;
        OwnerId owner;
        std::uint8_t flags;
        std::uint8_t selfTradePrevention;
        std::uint16_t reserved;

        bool operator==(const RawCommand&) const = default;
    };

    RawCommand unpackCommandBody(std::span<const std::byte, commandBodyBytes> body) noexcept;

    // Fills out with this command's fields, which it does not check. A mode with no owner is
    // written absent. Never allocates or throws.
    void packCommandBody(const Symbol& symbol, const Command& command, std::span<std::byte, commandBodyBytes> out) noexcept;

    struct LogHeader {
        std::uint32_t formatVersion = logFormatVersion;
        std::uint32_t minimumReaderVersion = logFormatVersion;
        std::uint32_t headerBytes = logHeaderBytes;
        std::uint32_t recordBytes = logRecordBytes;
        // The writing engine's version: recorded, not enforced. 0.0.0 means no engine wrote it,
        // as in a converted file, which is what keeps that file's round trip byte-identical.
        std::uint16_t engineMajor = 0;
        std::uint16_t engineMinor = 0;
        std::uint16_t enginePatch = 0;
        std::uint64_t firstPosition = 0;  // the log position of this file's first record
        // The placement this file was written under, an outcome and not a recipe: replay and
        // conversion ignore it. A shard count of 0 means none, as in a converted file.
        std::uint32_t shardCount = 0;
        std::uint32_t shardIndex = 0;
        std::vector<Symbol> symbols;  // this file's, sorted
        // The first record's chain seed: the low 32 bits of the header's checksum. Set by reading.
        std::uint32_t chainSeed = 0;

        bool operator==(const LogHeader&) const = default;
    };

    // The whole header, checksum included. The chain seed is its last 8 bytes' low 32 bits, read
    // with logChainSeed.
    std::vector<std::byte> encodeLogHeader(const LogHeader& header);
    std::uint32_t logChainSeed(std::span<const std::byte> encodedHeader) noexcept;

    // Fills out with one record and returns its checksum, which seeds the next. Never allocates
    // or throws. The command must have passed the entry check: a malformed record would make
    // the log unreadable, which checked builds assert. A mode with no owner is written absent.
    std::uint32_t encodeLogRecord(const Symbol& symbol, const Command& command, std::uint32_t previousChecksum,
                                  std::span<std::byte, logRecordBytes> out) noexcept;

    // Checks one record against the chain and decodes it, reporting the first problem as a
    // v3 row would. record may be longer than logRecordBytes when a header says so, and bytes
    // past what this reader knows must be zero. position is its log position, and filePosition
    // its index in this file: sequence is position + 1 and lineNumber is filePosition + 2, as
    // in its CSV twin. A checksum that does not chain is CORRUPT_RECORD.
    ParseResult decodeLogRecord(std::span<const std::byte> record, std::uint32_t previousChecksum,
                                std::uint64_t position, std::uint64_t filePosition);

    // One v3 row, with no line ending, for conversion. The record's sequence is printed as it is.
    std::string formatReplayRow(const ReplayRecord& record);

    // The counterpart of ReplayReader: the header on the first call, then a record per call,
    // allocating nothing per record, failing fast as the CSV does. After a bad record it reads on
    // only to tell a torn tail from damage mid-file: CORRUPT_RECORD if a later record passes its
    // chain and field checks, otherwise TORN_TAIL. After the first error, or the end, every later
    // call returns it again. Throws std::runtime_error if the stream itself fails to read.
    class CommandLogReader {
    public:
        static constexpr std::size_t defaultChunkCapacity = 64 * 1024;
        explicit CommandLogReader(std::istream& source, std::size_t capacity = defaultChunkCapacity);

        ReadResult next();

        // Null until the header has been read.
        const LogHeader* header() const { return headerRead ? &parsedHeader : nullptr; }

    private:
        // Copies up to `wanted` bytes of the input into out, and returns how many there were.
        std::size_t read(std::byte* out, std::size_t wanted);
        bool refill();
        std::optional<ParseError> readHeader();
        // Reads the rest of the input to name the failure of the record just read.
        ParseError classifyFailure(std::size_t lineNumber);

        std::istream& input;
        std::size_t chunkCapacity;
        std::vector<char> chunk;
        std::size_t chunkPosition = 0;
        std::size_t chunkSize = 0;
        bool inputExhausted = false;
        bool headerRead = false;
        LogHeader parsedHeader;
        std::vector<std::byte> record;  // reused for every record
        std::uint32_t previousChecksum = 0;
        std::uint64_t recordsRead = 0;
        std::optional<ReadResult> finalResult;
    };

    // Whether a file's first 8 bytes are a log's magic. False for one that can't be opened.
    bool isCommandLogFile(const std::filesystem::path& path);

    // Every record up to the first error, as readReplayFile does for a CSV, with format V3.
    ReplayInput readCommandLogFile(const std::filesystem::path& path);

    // Whichever the file's first bytes say it is.
    ReplayInput readReplayOrLogFile(const std::filesystem::path& path);

}  // namespace order_book::replay
