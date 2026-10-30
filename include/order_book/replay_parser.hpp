#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_type.hpp"
#include "self_trade_prevention.hpp"
#include "side.hpp"
#include "time_in_force.hpp"
#include "types.hpp"

// Parser for the CSV replay format (docs/replay-format.md). It turns text into typed
// commands and knows nothing about the book's state: a well-formed command the book then
// refuses is the book's business, not a parse error. It is built into its own library,
// order_book_replay, so the order_book library itself knows nothing about CSV.
namespace order_book::replay {

    inline constexpr std::string_view expectedHeader = "sequence,command,order_id,side,price,quantity";
    inline constexpr std::size_t columnCount = 6;

    // Positions in expectedHeader, and each column's name there (as ParseError::column holds it).
    inline constexpr std::size_t sequenceColumn = 0;
    inline constexpr std::size_t commandColumn = 1;
    inline constexpr std::size_t orderIdColumn = 2;
    inline constexpr std::size_t sideColumn = 3;
    inline constexpr std::size_t priceColumn = 4;
    inline constexpr std::size_t quantityColumn = 5;
    inline constexpr std::array<std::string_view, columnCount> columnNames = {
        "sequence", "command", "order_id", "side", "price", "quantity"};

    // Which layout a file uses, named by its header: v1's six columns, or v2's twelve, which
    // append the order behaviors' instructions (docs/adr/0005-replay-format-v2-is-a-second-exact-header.md).
    enum class ReplayFormat { V1, V2 };

    inline constexpr std::string_view expectedHeaderV2 =
        "sequence,command,order_id,side,price,quantity,order_type,time_in_force,stop_price,post_only,owner,"
        "self_trade_prevention";
    inline constexpr std::size_t columnCountV2 = 12;

    inline constexpr std::size_t orderTypeColumn = 6;
    inline constexpr std::size_t timeInForceColumn = 7;
    inline constexpr std::size_t stopPriceColumn = 8;
    inline constexpr std::size_t postOnlyColumn = 9;
    inline constexpr std::size_t ownerColumn = 10;
    inline constexpr std::size_t selfTradePreventionColumn = 11;
    inline constexpr std::array<std::string_view, columnCountV2> columnNamesV2 = {
        "sequence", "command", "order_id", "side", "price", "quantity",
        "order_type", "time_in_force", "stop_price", "post_only", "owner", "self_trade_prevention"};

    // One record split on every comma, before any field is interpreted. lineNumber is the
    // physical, 1-based line in the input (the header is line 1).
    struct CsvRow {
        std::size_t lineNumber;
        std::vector<std::string> fields;

        bool operator==(const CsvRow&) const = default;
    };

    // A parsed command satisfies the Order constructor's preconditions: nonzero ID, price,
    // and quantity, and a valid side.
    struct AddCommand {
        OrderId orderId;
        Side side;
        Price price;  // 0 on a MARKET or STOP order (v2 only)
        Quantity quantity;
        // v2's columns; a v1 record has these defaults, a GTC limit order with no owner.
        OrderType orderType = OrderType::LIMIT;
        TimeInForce timeInForce = TimeInForce::GTC;
        Price stopPrice = 0;
        bool postOnly = false;
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const AddCommand&) const = default;
    };

    struct CancelCommand {
        OrderId orderId;

        bool operator==(const CancelCommand&) const = default;
    };

    // A replace of a resting order (v2 only), which keeps its ID and takes a new total.
    struct ReplaceCommand {
        OrderId orderId;
        Price price;
        Quantity quantity;  // the new total
        bool postOnly = false;
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const ReplaceCommand&) const = default;
    };

    // Open: later releases may append alternatives, never reorder one, so code that visits it
    // should handle a command it does not know.
    using Command = std::variant<AddCommand, CancelCommand, ReplaceCommand>;

    struct ReplayRecord {
        std::size_t lineNumber;
        std::uint64_t sequence;  // the file's own number, never passed to the engine
        Command command;

        bool operator==(const ReplayRecord&) const = default;
    };

    enum class ParseErrorCode {
        // File level, always at line 1 with no column.
        MISSING_HEADER,          // empty input, or a first line that is a data record
        INVALID_HEADER,          // a first line that is not exactly expectedHeader
        // Record level, with no column.
        EMPTY_RECORD,
        QUOTE_CHARACTER,
        WRONG_COLUMN_COUNT,
        // Field level, naming the column.
        SEQUENCE_NOT_INCREASING,
        UNKNOWN_COMMAND,         // neither ADD nor CANCEL
        MISSING_REQUIRED_FIELD,  // empty where the command needs a value
        UNEXPECTED_FIELD,        // filled in where the command needs it empty
        INVALID_SIDE,            // neither BUY nor SELL
        INVALID_NUMBER,          // anything but decimal digits
        NUMBER_OUT_OF_RANGE,     // above the column's maximum
        INVALID_VALUE,           // a well-formed number the column forbids: zero
        // v2's columns. Open: later releases may append codes, never renumber one.
        INVALID_ORDER_TYPE,             // neither LIMIT, MARKET, STOP, nor STOP_LIMIT
        INVALID_TIME_IN_FORCE,          // neither GTC, IOC, nor FOK
        INVALID_POST_ONLY,              // neither empty nor Y
        INVALID_SELF_TRADE_PREVENTION,  // neither CANCEL_RESTING, CANCEL_INCOMING, nor CANCEL_BOTH
        INVALID_COMBINATION             // MARKET or STOP with GTC, or post-only on an ADD not a GTC LIMIT
    };

    // The enumerator's name, for printing codes as identifiers.
    constexpr const char* parseErrorCodeName(ParseErrorCode code) {
        switch(code) {
            case ParseErrorCode::MISSING_HEADER: return "MISSING_HEADER";
            case ParseErrorCode::INVALID_HEADER: return "INVALID_HEADER";
            case ParseErrorCode::EMPTY_RECORD: return "EMPTY_RECORD";
            case ParseErrorCode::QUOTE_CHARACTER: return "QUOTE_CHARACTER";
            case ParseErrorCode::WRONG_COLUMN_COUNT: return "WRONG_COLUMN_COUNT";
            case ParseErrorCode::SEQUENCE_NOT_INCREASING: return "SEQUENCE_NOT_INCREASING";
            case ParseErrorCode::UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
            case ParseErrorCode::MISSING_REQUIRED_FIELD: return "MISSING_REQUIRED_FIELD";
            case ParseErrorCode::UNEXPECTED_FIELD: return "UNEXPECTED_FIELD";
            case ParseErrorCode::INVALID_SIDE: return "INVALID_SIDE";
            case ParseErrorCode::INVALID_NUMBER: return "INVALID_NUMBER";
            case ParseErrorCode::NUMBER_OUT_OF_RANGE: return "NUMBER_OUT_OF_RANGE";
            case ParseErrorCode::INVALID_VALUE: return "INVALID_VALUE";
            case ParseErrorCode::INVALID_ORDER_TYPE: return "INVALID_ORDER_TYPE";
            case ParseErrorCode::INVALID_TIME_IN_FORCE: return "INVALID_TIME_IN_FORCE";
            case ParseErrorCode::INVALID_POST_ONLY: return "INVALID_POST_ONLY";
            case ParseErrorCode::INVALID_SELF_TRADE_PREVENTION: return "INVALID_SELF_TRADE_PREVENTION";
            case ParseErrorCode::INVALID_COMBINATION: return "INVALID_COMBINATION";
        }
        return "UNKNOWN";
    }

    struct ParseError {
        std::size_t lineNumber;
        ParseErrorCode code;
        std::string column;   // header name of the column at fault; empty for file- and record-level errors
        std::string value;    // that field's text, or the whole record for file- and record-level errors
        std::string message;  // for people; its wording is not part of the format

        bool operator==(const ParseError&) const = default;
    };

    using ParseResult = std::variant<ReplayRecord, ParseError>;

    // Splits on every comma; never fails. An empty line gives one empty field. The text must
    // already have its line ending removed.
    CsvRow splitRecord(std::string_view text, std::size_t lineNumber);

    // The same split into an existing row, replacing its line number and fields. It reuses
    // the row's storage, so a reader that splits every six-field record into one row stops
    // allocating once the row has grown (fields short enough for the small-string buffer
    // never allocate). text must not view the row's own fields. If an allocation fails, the
    // row is left valid but partly overwritten.
    void splitRecordInto(std::string_view text, std::size_t lineNumber, CsvRow& row);

    // The header check for the input's first line, which the caller has read as line 1.
    std::optional<ParseError> checkHeader(std::string_view text);

    // The layout line 1 names, or why it names none: MISSING_HEADER for a line that is a data
    // record in either layout, INVALID_HEADER for anything else.
    std::variant<ReplayFormat, ParseError> headerFormat(std::string_view text);

    // Checks one data record and turns it into a command, reporting only the first problem,
    // in the order the format defines: empty record, quote character, field count, sequence
    // (syntax and range, then strictly greater than previousSequence when one is given),
    // command, order_id, then side, price, and quantity. A pure function of its arguments.
    //
    // Numbers are decimal digits only, so signs, decimal points, exponents, and whitespace
    // are all refused, independent of locale. Leading zeros are ignored ("000123" is 123), and
    // zero, however written, is refused by every column. sequence may go up to 2^64 - 1,
    // the other columns up to their engine type's maximum.
    ParseResult parseRow(const CsvRow& row, std::optional<std::uint64_t> previousSequence = std::nullopt);

    // The same for a row of the given layout. A v1 row is checked exactly as above. A v2 row
    // has twelve fields and is checked in this order: the record (empty, quotes, field count),
    // sequence, command, order_id, then order_type and time_in_force (keyword, then MARKET or
    // STOP with GTC), side, price, quantity, stop_price, post_only (keyword, then the
    // combination), owner, and self_trade_prevention; within a column, present or empty as
    // the command and type require, then syntax, then zero. So a parsed ADD is always a
    // well-formed OrderRequest.
    ParseResult parseRow(const CsvRow& row, ReplayFormat format, std::optional<std::uint64_t> previousSequence);

    struct EndOfInput {
        bool operator==(const EndOfInput&) const = default;
    };

    using ReadResult = std::variant<ReplayRecord, ParseError, EndOfInput>;

    // Reads a replay input: the header on the first call, then one data record per call, enforcing strictly increasing sequence numbers across records. A
    // record ends with LF or CRLF, and the last may have no line ending; a CR anywhere else
    // stays in its field. Open files in binary mode, so the stream keeps CRs exactly as
    // written. After the first error, or the end, every later call returns it again without
    // reading further (the format fails fast). Throws std::runtime_error if the stream itself
    // fails to read, once the complete lines read before the failure have been returned.
    //
    // It reads the stream a chunk at a time, not a line at a time, so it may consume up to a
    // chunk beyond the last record or error it has returned: give it a stream of its own,
    // and do not read the stream afterwards expecting to resume where it stopped.
    class ReplayReader {
    public:
        // capacity is how many bytes it reads from source at a time (zero is taken as one);
        // the default suits any input, and tests pass small values to put chunk boundaries
        // inside records.
        static constexpr std::size_t defaultChunkCapacity = 64 * 1024;
        explicit ReplayReader(std::istream& source, std::size_t capacity = defaultChunkCapacity)
            : input(source), chunkCapacity(capacity > 0 ? capacity : 1) {}

        ReadResult next();

        // The layout the header named; V1 until the header has been read.
        ReplayFormat format() const { return layout; }

    private:
        // Reads the next line into the member `line`, reusing its storage; false at the end
        // of the input, when `line` is left unspecified. A line ends at an LF, which is
        // dropped along with a CR directly before it, or at the end of the input, where a
        // final CR stays.
        bool readLine();
        // Reads the next chunk of the input into `chunk`; false once the input is exhausted.
        bool refill();

        std::istream& input;
        std::size_t chunkCapacity;
        // The input is read a chunk at a time, not a character at a time, so the reader may
        // have consumed more of the stream than the records it has returned. Give it a stream
        // of its own.
        std::vector<char> chunk;
        std::size_t chunkPosition = 0;
        std::size_t chunkSize = 0;
        bool inputExhausted = false;
        // Reused for every record, so reading one allocates nothing once they have grown.
        std::string line;
        CsvRow row;
        std::size_t linesRead = 0;
        bool headerChecked = false;
        ReplayFormat layout = ReplayFormat::V1;
        std::optional<std::uint64_t> previousSequence;
        std::optional<ReadResult> finalResult;
    };

    // One diagnostic line: "<path>:<line>: <column>: <message>", or without the column for
    // file- and record-level errors.
    std::string formatParseError(const ParseError& error, std::string_view path);

}  // namespace order_book::replay
