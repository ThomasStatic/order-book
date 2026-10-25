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

#include "side.hpp"
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
        Price price;
        Quantity quantity;

        bool operator==(const AddCommand&) const = default;
    };

    struct CancelCommand {
        OrderId orderId;

        bool operator==(const CancelCommand&) const = default;
    };

    using Command = std::variant<AddCommand, CancelCommand>;

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
        INVALID_VALUE            // a well-formed number the column forbids: zero
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

    // The header check for the input's first line, which the caller has read as line 1.
    std::optional<ParseError> checkHeader(std::string_view text);

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

    struct EndOfInput {
        bool operator==(const EndOfInput&) const = default;
    };

    using ReadResult = std::variant<ReplayRecord, ParseError, EndOfInput>;

    // Reads a replay input one line at a time: the header on the first call, then one data
    // record per call, enforcing strictly increasing sequence numbers across records. A
    // record ends with LF or CRLF, and the last may have no line ending; a CR anywhere else
    // stays in its field. Open files in binary mode, so the stream keeps CRs exactly as
    // written. After the first error, or the end, every later call returns it again without
    // reading further (the format fails fast). Throws std::runtime_error if the stream itself
    // fails to read.
    class ReplayReader {
    public:
        explicit ReplayReader(std::istream& source) : input(source) {}

        ReadResult next();

    private:
        std::optional<std::string> readLine();

        std::istream& input;
        std::size_t linesRead = 0;
        bool headerChecked = false;
        std::optional<std::uint64_t> previousSequence;
        std::optional<ReadResult> finalResult;
    };

    // One diagnostic line: "<path>:<line>: <column>: <message>", or without the column for
    // file- and record-level errors.
    std::string formatParseError(const ParseError& error, std::string_view path);

}  // namespace order_book::replay
