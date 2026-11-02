#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/order_book.hpp"
#include "order_book/symbol.hpp"

// The instruments an engine may trade, and which shard's books hold each. Not public API: the
// engine's contract to the outside is the instrument list's format and its flags.
namespace order_book::runtime {

    using replay::Symbol;

    // What a book holds without growing when its list row leaves a column empty. levels is per
    // side. --capacity-orders, --capacity-stops, and --capacity-levels replace these.
    inline constexpr BookCapacity defaultBookCapacity{4096, 1024, 256};

    // One row of the instrument list. An absent field means the default.
    struct Instrument {
        Symbol symbol;
        std::optional<std::uint32_t> shard;  // a pin, in place of the symbol's hash
        std::optional<std::size_t> orders;
        std::optional<std::size_t> stops;
        std::optional<std::size_t> levels;

        bool operator==(const Instrument&) const = default;
    };

    // The capacity this instrument's book is warmed to.
    BookCapacity capacityOf(const Instrument& instrument, const BookCapacity& defaults);

    enum class InstrumentListErrorCode {
        // File level, with no column.
        MISSING_HEADER,  // empty input
        INVALID_HEADER,  // a first line that is not exactly the header
        NO_INSTRUMENTS,  // a header and no rows
        // Record level, with no column.
        EMPTY_RECORD,
        QUOTE_CHARACTER,
        WRONG_COLUMN_COUNT,
        // Field level, naming the column.
        INVALID_SYMBOL,      // not 1 to 8 of A-Z, 0-9, '.', '-'
        DUPLICATE_SYMBOL,    // already on an earlier row
        INVALID_NUMBER,      // anything but decimal digits
        NUMBER_OUT_OF_RANGE, // above maxInstrumentListNumber
        INVALID_VALUE,       // a capacity of zero
        SHARD_OUT_OF_RANGE   // a pin at or past the shard count
    };

    // The largest number any column holds, the same on every platform.
    inline constexpr std::uint64_t maxInstrumentListNumber = 0xFFFFFFFFu;

    struct InstrumentListError {
        std::size_t lineNumber;  // physical and 1-based, the header being line 1
        InstrumentListErrorCode code;
        std::string column;   // empty for file- and record-level errors
        std::string message;  // for people; its wording is not part of the format

        bool operator==(const InstrumentListError&) const = default;
    };

    using InstrumentListResult = std::variant<std::vector<Instrument>, InstrumentListError>;

    // The list in text, to the replay format's strictness: the exact header
    // symbol,shard,orders,stops,levels then five fields on every row, LF or CRLF, no quoting,
    // blank lines, or comments, digits only. An empty field means the default, and shard may
    // be 0, unlike any replay column. Reports the first problem. shardCount must be positive.
    InstrumentListResult parseInstrumentList(std::string_view text, std::uint32_t shardCount);

    // <path>:<line>: <column>: <message>, without the column when the error names none.
    std::string formatInstrumentListError(std::string_view path, const InstrumentListError& error);

    // The specified placement hash: 64-bit FNV-1a over the 8 symbol bytes. Never std::hash,
    // which differs by platform, since a log records the placement it was written under.
    std::uint64_t symbolHash(const Symbol& symbol) noexcept;

    // Which shard an unpinned symbol belongs to. It depends on nothing but the symbol and the
    // shard count, so adding instruments moves nothing.
    std::uint32_t hashedShard(const Symbol& symbol, std::uint32_t shardCount) noexcept;

    struct BookLocation {
        std::uint32_t shard;
        std::uint32_t slot;  // dense within the shard's BookSet; in no format and never sent

        bool operator==(const BookLocation&) const = default;
    };

    // Maps a symbol to its shard and slot. Built once, then immutable, so any thread reads it
    // without locks. A shard's slots are in symbol order, so a list's line order means nothing.
    class InstrumentDirectory {
    public:
        // Throws std::invalid_argument for what the list parser refuses: no shards, no
        // instruments, an empty or repeated symbol, or a pin at or past shardCount.
        InstrumentDirectory(const std::vector<Instrument>& instruments, std::uint32_t shardCount);

        // Where this symbol's book lives, or nothing for a symbol not on the list.
        std::optional<BookLocation> find(const Symbol& symbol) const noexcept;

        std::uint32_t shardCount() const noexcept { return shards; }
        std::size_t size() const noexcept { return bySymbol.size(); }

        // This shard's instruments, by slot. A shard may hold none.
        std::span<const Instrument> onShard(std::uint32_t shard) const;

    private:
        struct Entry {
            Symbol symbol;
            BookLocation location;
        };

        std::uint32_t shards;
        std::vector<Entry> bySymbol;       // sorted by symbol
        std::vector<Instrument> bySlot;    // sorted by shard, then slot
        std::vector<std::size_t> shardStarts;  // shard's first index in bySlot, then the size
    };

}  // namespace order_book::runtime
