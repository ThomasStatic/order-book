#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "order_book/replay_parser.hpp"

namespace {

using order_book::Side;
using order_book::replay::AddCommand;
using order_book::replay::CancelCommand;
using order_book::replay::checkHeader;
using order_book::replay::columnNames;
using order_book::replay::commandColumn;
using order_book::replay::CsvRow;
using order_book::replay::EndOfInput;
using order_book::replay::expectedHeader;
using order_book::replay::formatParseError;
using order_book::replay::orderIdColumn;
using order_book::replay::ParseError;
using order_book::replay::ParseErrorCode;
using order_book::replay::parseErrorCodeName;
using order_book::replay::ParseResult;
using order_book::replay::parseRow;
using order_book::replay::priceColumn;
using order_book::replay::quantityColumn;
using order_book::replay::ReadResult;
using order_book::replay::ReplayReader;
using order_book::replay::ReplayRecord;
using order_book::replay::sequenceColumn;
using order_book::replay::sideColumn;
using order_book::replay::splitRecord;

constexpr std::uint64_t maxSequence = std::numeric_limits<std::uint64_t>::max();
constexpr unsigned int maxField = std::numeric_limits<unsigned int>::max();

// A valid ADD to vary one field of.
const std::vector<std::string> validAdd = {"7", "ADD", "1001", "BUY", "100", "50"};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expectThrows(const auto& fn, const std::string& message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

std::string join(const std::vector<std::string>& fields) {
    std::string text;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        text += (i > 0 ? "," : "") + fields[i];
    }
    return text;
}

std::string addWith(std::size_t column, const std::string& value) {
    std::vector<std::string> fields = validAdd;
    fields[column] = value;
    return join(fields);
}

std::string describe(const ParseError& error) {
    return std::string(parseErrorCodeName(error.code)) + " (" + formatParseError(error, "row") + ")";
}

ParseResult parseText(const std::string& text, std::optional<std::uint64_t> previous = std::nullopt) {
    return parseRow(splitRecord(text, 2), previous);
}

ReplayRecord expectRecord(const ParseResult& result, const std::string& context) {
    if (const ParseError* error = std::get_if<ParseError>(&result)) {
        expect(false, context + ": expected a record, got " + describe(*error));
    }
    return std::get<ReplayRecord>(result);
}

ParseError expectError(const ParseResult& result, ParseErrorCode code, const std::string& column,
                       const std::string& context) {
    const ParseError* error = std::get_if<ParseError>(&result);
    expect(error != nullptr, context + ": expected " + parseErrorCodeName(code) + ", got a record");
    expect(error->code == code, context + ": expected " + parseErrorCodeName(code) + ", got " + describe(*error));
    expect(error->column == column, context + ": expected column '" + column + "', got " + describe(*error));
    expect(!error->message.empty(), context + ": the error has no message");
    return *error;
}

// A data row's error, checking that it names line 2 and carries the faulty text: the field
// for a field-level error, the whole record otherwise.
void expectRowError(const std::string& text, ParseErrorCode code, const std::string& column,
                    std::optional<std::uint64_t> previous = std::nullopt) {
    const ParseError error = expectError(parseText(text, previous), code, column, "'" + text + "'");
    expect(error.lineNumber == 2, "'" + text + "': wrong line " + std::to_string(error.lineNumber));
    if (column.empty()) {
        expect(error.value == text, "'" + text + "': record-level value is '" + error.value + "'");
    } else {
        const std::size_t index = static_cast<std::size_t>(
            std::find(columnNames.begin(), columnNames.end(), column) - columnNames.begin());
        expect(error.value == splitRecord(text, 2).fields.at(index),
               "'" + text + "': field value is '" + error.value + "'");
    }
}

std::uint64_t numberAt(const ReplayRecord& record, std::size_t column) {
    if (column == sequenceColumn) {
        return record.sequence;
    }
    const auto& add = std::get<AddCommand>(record.command);
    switch (column) {
        case orderIdColumn: return add.orderId;
        case priceColumn: return add.price;
        case quantityColumn: return add.quantity;
    }
    expect(false, "no number in column " + std::to_string(column));
    return 0;
}

void testSplitRecord() {
    expect(splitRecord(expectedHeader, 1).fields == std::vector<std::string>(columnNames.begin(), columnNames.end()),
           "the column names are the header's");
    expect(splitRecord("1,ADD,1001,BUY,100,50", 9) == CsvRow{9, {"1", "ADD", "1001", "BUY", "100", "50"}},
           "split a full record");
    expect(splitRecord("4,CANCEL,1001,,,", 3) == CsvRow{3, {"4", "CANCEL", "1001", "", "", ""}},
           "empty fields survive splitting, trailing ones included");
    expect(splitRecord("", 5) == CsvRow{5, {""}}, "an empty line is one empty field");
    expect(splitRecord(",", 1) == CsvRow{1, {"", ""}}, "a lone comma is two empty fields");
    expect(splitRecord(" a ,\"b\",c\r", 1) == CsvRow{1, {" a ", "\"b\"", "c\r"}},
           "splitting keeps spaces, quotes, and CRs as written");
}

void testValidRows() {
    expect(expectRecord(parseText("1,ADD,1001,BUY,100,50"), "buy")
               == ReplayRecord{2, 1, AddCommand{1001, Side::BUY, 100, 50}},
           "an ADD to buy");
    expect(expectRecord(parseText("2,ADD,1002,SELL,105,30"), "sell")
               == ReplayRecord{2, 2, AddCommand{1002, Side::SELL, 105, 30}},
           "an ADD to sell");
    expect(expectRecord(parseText("4,CANCEL,1001,,,"), "cancel") == ReplayRecord{2, 4, CancelCommand{1001}},
           "a CANCEL");
    expect(expectRecord(parseText("18446744073709551615,ADD,4294967295,SELL,4294967295,4294967295"), "limits")
               == ReplayRecord{2, maxSequence, AddCommand{maxField, Side::SELL, maxField, maxField}},
           "every column at its maximum");
    expect(expectRecord(parseText("1,ADD,1,BUY,1,1"), "minimums") == ReplayRecord{2, 1, AddCommand{1, Side::BUY, 1, 1}},
           "every column at its minimum");

    const CsvRow row = splitRecord("3,ADD,1003,BUY,105,20", 41);
    expect(expectRecord(parseRow(row), "line number").lineNumber == 41, "a record keeps its row's line number");
    expect(parseRow(row) == parseRow(row) && parseRow(row, 2) == parseRow(row, 2),
           "parsing the same row twice gives the same result");
    expect(parseRow(splitRecord("1,MODIFY,1,,,", 8)) == parseRow(splitRecord("1,MODIFY,1,,,", 8)),
           "parsing the same bad row twice gives the same error");
}

void testRecordFraming() {
    expectRowError("", ParseErrorCode::EMPTY_RECORD, "");
    expect(std::holds_alternative<ParseError>(parseRow(CsvRow{2, {}})), "a row with no fields at all is empty");
    expectRowError("3,\"ADD\",1003,BUY,105,20", ParseErrorCode::QUOTE_CHARACTER, "");
    expectRowError("3,ADD,1003,BUY,105,20\"", ParseErrorCode::QUOTE_CHARACTER, "");
    expectRowError("4,CANCEL,1001,\"\",,", ParseErrorCode::QUOTE_CHARACTER, "");
    expectRowError("\"", ParseErrorCode::QUOTE_CHARACTER, "");
    expectRowError("3,\"ADD,1003\",BUY,105,20", ParseErrorCode::QUOTE_CHARACTER, "");  // a quote comes before the count
    expectRowError("3,ADD,1003,BUY,105", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    expectRowError("3,ADD,1003,BUY,105,20,", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    expectRowError("4,CANCEL,1001", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    expectRowError(",", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    expectRowError("ADD", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    expectRowError(",,,,,,,,,,", ParseErrorCode::WRONG_COLUMN_COUNT, "");
    // Six fields, but all empty: framing is fine, and the first column is missing.
    expectRowError(",,,,,", ParseErrorCode::MISSING_REQUIRED_FIELD, "sequence");
}

void testCommands() {
    for (const char* command : {"MODIFY", "add", "Add", "cancel", "ADDD", "ADD ", " ADD", "CANCEL\r", "BUY", "0"}) {
        expectRowError(addWith(commandColumn, command), ParseErrorCode::UNKNOWN_COMMAND, "command");
    }
    expectRowError(addWith(commandColumn, ""), ParseErrorCode::MISSING_REQUIRED_FIELD, "command");
    expectRowError("4,REPLACE,1001,,,", ParseErrorCode::UNKNOWN_COMMAND, "command");
}

// The conversion rules from the design notes, applied to every numeric column.
void testNumbers() {
    for (std::size_t column : {sequenceColumn, orderIdColumn, priceColumn, quantityColumn}) {
        const std::string name(columnNames[column]);
        const auto accepts = [&](const std::string& text, std::uint64_t value) {
            const ReplayRecord record = expectRecord(parseText(addWith(column, text)), name + " '" + text + "'");
            expect(numberAt(record, column) == value, name + " '" + text + "' parsed as " + std::to_string(numberAt(record, column)));
        };

        accepts("123", 123);
        accepts("000123", 123);
        accepts("0000000000000000000000000000000000000042", 42);  // longer than any uint64_t, all leading zeros

        expectRowError(addWith(column, "0"), ParseErrorCode::INVALID_VALUE, name);
        expectRowError(addWith(column, "000"), ParseErrorCode::INVALID_VALUE, name);
        expectRowError(addWith(column, "00000000000000000000000000000"), ParseErrorCode::INVALID_VALUE, name);

        // Arabic-Indic digit one is a digit to some locales, but not here.
        for (const char* text : {"-5", "+5", "-0", "12.5", "100.00", "1.", ".5", "1e3", "1E3", "12abc", "abc",
                                 "0x10", " 5", "5 ", " ", "\t5", "5\r", "1_000", "\xd9\xa1"}) {
            expectRowError(addWith(column, text), ParseErrorCode::INVALID_NUMBER, name);
        }

        expectRowError(addWith(column, ""), ParseErrorCode::MISSING_REQUIRED_FIELD, name);
        expectRowError(addWith(column, "12398120937018273018273012873012837"), ParseErrorCode::NUMBER_OUT_OF_RANGE, name);
        expectRowError(addWith(column, "18446744073709551616"), ParseErrorCode::NUMBER_OUT_OF_RANGE, name);
        expectRowError(addWith(column, "99999999999999999999"), ParseErrorCode::NUMBER_OUT_OF_RANGE, name);
    }

    // sequence is a std::uint64_t; order_id, price, and quantity are the engine's unsigned int.
    expect(numberAt(expectRecord(parseText(addWith(sequenceColumn, "18446744073709551615")), "max sequence"),
                    sequenceColumn) == maxSequence, "the largest sequence");
    expect(numberAt(expectRecord(parseText(addWith(sequenceColumn, "00018446744073709551615")), "padded max sequence"),
                    sequenceColumn) == maxSequence, "the largest sequence with leading zeros");
    expect(numberAt(expectRecord(parseText(addWith(sequenceColumn, "4294967296")), "sequence past unsigned int"),
                    sequenceColumn) == 4294967296ULL, "a sequence beyond unsigned int");
    for (std::size_t column : {orderIdColumn, priceColumn, quantityColumn}) {
        const std::string name(columnNames[column]);
        expect(numberAt(expectRecord(parseText(addWith(column, "4294967295")), name + " max"), column) == maxField,
               "the largest " + name);
        expect(numberAt(expectRecord(parseText(addWith(column, "0004294967295")), name + " padded max"), column) == maxField,
               "the largest " + name + " with leading zeros");
        expectRowError(addWith(column, "4294967296"), ParseErrorCode::NUMBER_OUT_OF_RANGE, name);
        expectRowError(addWith(column, "10000000000"), ParseErrorCode::NUMBER_OUT_OF_RANGE, name);
    }

    // CANCEL's order_id follows the same rules.
    expect(expectRecord(parseText("4,CANCEL,0001001,,,"), "padded cancel") == ReplayRecord{2, 4, CancelCommand{1001}},
           "a CANCEL with leading zeros");
    expectRowError("4,CANCEL,0,,,", ParseErrorCode::INVALID_VALUE, "order_id");
    expectRowError("4,CANCEL,-1001,,,", ParseErrorCode::INVALID_NUMBER, "order_id");
    expectRowError("4,CANCEL,4294967296,,,", ParseErrorCode::NUMBER_OUT_OF_RANGE, "order_id");
}

void testSequenceOrdering() {
    expect(expectRecord(parseText("6,ADD,1,BUY,1,1", 5), "after 5").sequence == 6, "6 follows 5");
    expect(expectRecord(parseText("100,ADD,1,BUY,1,1", 5), "gap").sequence == 100, "gaps are allowed");
    expect(expectRecord(parseText("0010,ADD,1,BUY,1,1", 9), "padded").sequence == 10, "values compare, not text");
    expectRowError("5,ADD,1,BUY,1,1", ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 5);
    expectRowError("4,ADD,1,BUY,1,1", ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 5);
    expectRowError("005,CANCEL,1,,,", ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 5);
    expectRowError("1,ADD,1,BUY,1,1", ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", maxSequence);
    expect(std::holds_alternative<ReplayRecord>(parseText("1,ADD,1,BUY,1,1")), "the first record may start anywhere");
    // Syntax and range come before ordering, and ordering before the later columns.
    expectRowError("x,ADD,1,BUY,1,1", ParseErrorCode::INVALID_NUMBER, "sequence", 5);
    expectRowError("0,ADD,1,BUY,1,1", ParseErrorCode::INVALID_VALUE, "sequence", 5);
    expectRowError("3,MODIFY,0,SHORT,abc,-1", ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 5);
}

void testSides() {
    for (const char* side : {"SHORT", "buy", "Buy", "sell", " BUY", "BUY ", "B", "S", "BID", "ASK", "0", "1"}) {
        expectRowError(addWith(sideColumn, side), ParseErrorCode::INVALID_SIDE, "side");
    }
    expectRowError(addWith(sideColumn, ""), ParseErrorCode::MISSING_REQUIRED_FIELD, "side");
}

void testFieldPresence() {
    expectRowError("3,ADD,,BUY,105,20", ParseErrorCode::MISSING_REQUIRED_FIELD, "order_id");
    expectRowError("3,ADD,1003,,105,20", ParseErrorCode::MISSING_REQUIRED_FIELD, "side");
    expectRowError("3,ADD,1002,SELL,,20", ParseErrorCode::MISSING_REQUIRED_FIELD, "price");  // never read as zero
    expectRowError("3,ADD,1003,BUY,105,", ParseErrorCode::MISSING_REQUIRED_FIELD, "quantity");
    expectRowError("4,CANCEL,,,,", ParseErrorCode::MISSING_REQUIRED_FIELD, "order_id");
    expectRowError(",ADD,1003,BUY,105,20", ParseErrorCode::MISSING_REQUIRED_FIELD, "sequence");

    // Columns a CANCEL doesn't use must hold no characters at all; the value doesn't matter.
    expectRowError("4,CANCEL,1001,BUY,,", ParseErrorCode::UNEXPECTED_FIELD, "side");
    expectRowError("4,CANCEL,1001,SHORT,,", ParseErrorCode::UNEXPECTED_FIELD, "side");
    expectRowError("5,CANCEL,1002,,0,", ParseErrorCode::UNEXPECTED_FIELD, "price");
    expectRowError("5,CANCEL,1002,,abc,", ParseErrorCode::UNEXPECTED_FIELD, "price");
    expectRowError("4,CANCEL,1001,,,50", ParseErrorCode::UNEXPECTED_FIELD, "quantity");
    expectRowError("4,CANCEL,1001, ,,", ParseErrorCode::UNEXPECTED_FIELD, "side");
    expectRowError("4,CANCEL,1001,,,\r", ParseErrorCode::UNEXPECTED_FIELD, "quantity");
}

// Only the first problem is reported, in the documented column order.
void testCheckingOrder() {
    expectRowError("x,MODIFY,0,SHORT,abc,-1", ParseErrorCode::INVALID_NUMBER, "sequence");
    expectRowError("1,MODIFY,0,SHORT,abc,-1", ParseErrorCode::UNKNOWN_COMMAND, "command");
    expectRowError("1,ADD,0,SHORT,abc,-1", ParseErrorCode::INVALID_VALUE, "order_id");
    expectRowError("1,ADD,5,SHORT,abc,-1", ParseErrorCode::INVALID_SIDE, "side");
    expectRowError("1,ADD,5,BUY,abc,-1", ParseErrorCode::INVALID_NUMBER, "price");
    expectRowError("1,ADD,5,BUY,,-1", ParseErrorCode::MISSING_REQUIRED_FIELD, "price");
    expectRowError("1,ADD,5,BUY,10,-1", ParseErrorCode::INVALID_NUMBER, "quantity");
    expectRowError("1,CANCEL,abc,BUY,1,1", ParseErrorCode::INVALID_NUMBER, "order_id");
    expectRowError("1,CANCEL,5,BUY,1,1", ParseErrorCode::UNEXPECTED_FIELD, "side");
    expectRowError("1,CANCEL,5,,1,1", ParseErrorCode::UNEXPECTED_FIELD, "price");
}

void testHeader() {
    expect(!checkHeader(expectedHeader).has_value(), "the exact header passes");
    expect(!checkHeader("sequence,command,order_id,side,price,quantity").has_value(), "the header literal passes");

    const auto expectHeaderError = [](const std::string& text, ParseErrorCode code) {
        const std::optional<ParseError> error = checkHeader(text);
        expect(error.has_value(), "header '" + text + "' should fail");
        expect(error->code == code, "header '" + text + "': got " + describe(*error));
        expect(error->lineNumber == 1 && error->column.empty() && error->value == text,
               "header '" + text + "' is a line-1 error with no column");
    };
    expectHeaderError("\xEF\xBB\xBFsequence,command,order_id,side,price,quantity", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("sequence,command,order_id,side,price,quantity,", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("sequence,command,order_id,side,price,quantity\r", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("sequence,command,order_id,price,side,quantity", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("Sequence,Command,Order_Id,Side,Price,Quantity", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("sequence, command,order_id,side,price,quantity", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("sequence,command,order_id,side,price", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("", ParseErrorCode::INVALID_HEADER);
    expectHeaderError("1,ADD,1001,BUY,100,50", ParseErrorCode::MISSING_HEADER);
    expectHeaderError("4,CANCEL,1001,,,", ParseErrorCode::MISSING_HEADER);
}

struct ReadOutcome {
    std::vector<ReplayRecord> records;
    std::optional<ParseError> error;
};

ReadOutcome readAll(std::istream& input) {
    ReplayReader reader(input);
    ReadOutcome outcome;
    while (true) {
        ReadResult result = reader.next();
        if (ReplayRecord* record = std::get_if<ReplayRecord>(&result)) {
            outcome.records.push_back(*record);
        } else if (ParseError* error = std::get_if<ParseError>(&result)) {
            outcome.error = *error;
            return outcome;
        } else {
            return outcome;
        }
    }
}

ReadOutcome readText(const std::string& text) {
    std::istringstream input(text);
    return readAll(input);
}

void expectReadError(const std::string& text, std::size_t line, ParseErrorCode code, const std::string& column,
                     std::size_t recordsBefore) {
    const ReadOutcome outcome = readText(text);
    const std::string context = "reading '" + text + "'";
    expect(outcome.error.has_value(), context + ": expected an error");
    expect(outcome.error->code == code, context + ": got " + describe(*outcome.error));
    expect(outcome.error->lineNumber == line, context + ": wrong line in " + describe(*outcome.error));
    expect(outcome.error->column == column, context + ": wrong column in " + describe(*outcome.error));
    expect(outcome.records.size() == recordsBefore, context + ": wrong number of records before the error");
}

void testReader() {
    const std::string header = std::string(expectedHeader);

    expectReadError("", 1, ParseErrorCode::MISSING_HEADER, "", 0);
    expectReadError("1,ADD,1001,BUY,100,50\n", 1, ParseErrorCode::MISSING_HEADER, "", 0);
    expectReadError("\n", 1, ParseErrorCode::INVALID_HEADER, "", 0);
    expectReadError(header + ",\n1,ADD,1001,BUY,100,50\n", 1, ParseErrorCode::INVALID_HEADER, "", 0);

    expect(readText(header).records.empty() && !readText(header).error, "a header without a line ending");
    expect(readText(header + "\n").records.empty() && !readText(header + "\n").error, "a header alone");
    expect(readText(header + "\r\n").records.empty() && !readText(header + "\r\n").error, "a CRLF header alone");

    const std::vector<ReplayRecord> twoRecords = {ReplayRecord{2, 1, AddCommand{1001, Side::BUY, 100, 50}},
                                                  ReplayRecord{3, 3, CancelCommand{1001}}};
    for (const std::string& text : {header + "\n1,ADD,1001,BUY,100,50\n3,CANCEL,1001,,,\n",
                                    header + "\r\n1,ADD,1001,BUY,100,50\r\n3,CANCEL,1001,,,\r\n",
                                    header + "\n1,ADD,1001,BUY,100,50\r\n3,CANCEL,1001,,,",
                                    header + "\r\n1,ADD,1001,BUY,100,50\n3,CANCEL,1001,,,\r\n"}) {
        const ReadOutcome outcome = readText(text);
        expect(!outcome.error && outcome.records == twoRecords, "LF, CRLF, mixed endings, and no final newline all read the same");
    }

    // Physical line numbers: blank lines count, and so does the header.
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\n\n3,CANCEL,1001,,,\n", 3, ParseErrorCode::EMPTY_RECORD, "", 1);
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\n\n", 3, ParseErrorCode::EMPTY_RECORD, "", 1);
    expectReadError(header + "\r\n1,ADD,1001,BUY,100,50\r\n\r\n", 3, ParseErrorCode::EMPTY_RECORD, "", 1);
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\n2,ADD,1002,SELL,105,30\n3,ADD,1003,BUY,105\n", 4,
                    ParseErrorCode::WRONG_COLUMN_COUNT, "", 2);

    // A CR is a line ending only directly before an LF; anywhere else it stays in its field.
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\r", 2, ParseErrorCode::INVALID_NUMBER, "quantity", 0);
    expectReadError(header + "\n1,CANCEL,1001,,,\r", 2, ParseErrorCode::UNEXPECTED_FIELD, "quantity", 0);
    expectReadError(header + "\n1,ADD,1001,BUY\r,100,50\n", 2, ParseErrorCode::INVALID_SIDE, "side", 0);
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\r\r\n", 2, ParseErrorCode::INVALID_NUMBER, "quantity", 0);
    expectReadError(header + "\r\r\n", 1, ParseErrorCode::INVALID_HEADER, "", 0);
    expectReadError(header + "\r", 1, ParseErrorCode::INVALID_HEADER, "", 0);

    // Sequence ordering spans records, by value.
    expectReadError(header + "\n5,ADD,1001,BUY,100,50\n6,ADD,1002,SELL,105,30\n4,ADD,1003,BUY,105,20\n", 4,
                    ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 2);
    expectReadError(header + "\n1,ADD,1001,BUY,100,50\n01,CANCEL,1001,,,\n", 3,
                    ParseErrorCode::SEQUENCE_NOT_INCREASING, "sequence", 1);
    const ReadOutcome padded = readText(header + "\n9,ADD,1001,BUY,100,50\n010,CANCEL,1001,,,\n");
    expect(!padded.error && padded.records.size() == 2 && padded.records[1].sequence == 10,
           "a padded sequence compares by value");

    // Fail fast: once an error or the end is reached, it is returned again and nothing more is read.
    {
        std::istringstream input(header + "\n1,ADD,1001,BUY,100,50\n1,ADD,1002,SELL,105,30\n2,CANCEL,1001,,,\n");
        ReplayReader reader(input);
        expect(std::holds_alternative<ReplayRecord>(reader.next()), "the first record reads");
        const ReadResult error = reader.next();
        expect(std::holds_alternative<ParseError>(error), "the repeated sequence fails");
        const std::streampos position = input.tellg();
        expect(reader.next() == error && reader.next() == error, "the error is returned again");
        expect(input.tellg() == position, "nothing is read after an error");
    }
    {
        std::istringstream input(header + "\n1,ADD,1001,BUY,100,50\n");
        ReplayReader reader(input);
        expect(std::holds_alternative<ReplayRecord>(reader.next()), "the only record reads");
        expect(std::holds_alternative<EndOfInput>(reader.next()), "then the input ends");
        expect(std::holds_alternative<EndOfInput>(reader.next()), "and stays ended");
    }
    {
        std::istringstream input("");
        ReplayReader reader(input);
        const ReadResult error = reader.next();
        expect(std::holds_alternative<ParseError>(error) && reader.next() == error, "a missing header is returned again");
    }

    expectThrows([] {
        std::istream broken(nullptr);
        ReplayReader reader(broken);
        reader.next();
    }, "a stream that fails to read throws");
}

void testFormatting() {
    const ParseError field{4, ParseErrorCode::MISSING_REQUIRED_FIELD, "price", "", "ADD requires a price"};
    expect(formatParseError(field, "orders.csv") == "orders.csv:4: price: ADD requires a price", "a field-level diagnostic");
    const ParseError record{4, ParseErrorCode::WRONG_COLUMN_COUNT, "", "3,ADD,1003,BUY,105", "expected 6 fields, found 5"};
    expect(formatParseError(record, "orders.csv") == "orders.csv:4: expected 6 fields, found 5", "a record-level diagnostic");

    // The real messages, as a CLI would print them.
    const ParseError sequence = std::get<ParseError>(parseRow(splitRecord("2,ADD,1003,BUY,105,20", 4), 2));
    expect(formatParseError(sequence, "orders.csv")
               == "orders.csv:4: sequence: 2 does not follow 2 (sequence numbers must strictly increase)",
           "the sequence diagnostic: " + formatParseError(sequence, "orders.csv"));
    const ParseError decimal = std::get<ParseError>(parseRow(splitRecord("3,ADD,1003,BUY,100.50,20", 4)));
    expect(formatParseError(decimal, "orders.csv") == "orders.csv:4: price: '100.50' is not a whole number of ticks",
           "the decimal price diagnostic: " + formatParseError(decimal, "orders.csv"));
    const ParseError cancel = std::get<ParseError>(parseRow(splitRecord("3,CANCEL,1003,,105,", 4)));
    expect(formatParseError(cancel, "orders.csv") == "orders.csv:4: price: CANCEL must leave price empty",
           "the cancel diagnostic: " + formatParseError(cancel, "orders.csv"));
    const ParseError count = std::get<ParseError>(parseRow(splitRecord("3,ADD,1003,BUY,105", 4)));
    expect(formatParseError(count, "orders.csv") == "orders.csv:4: expected 6 fields, found 5",
           "the field count diagnostic: " + formatParseError(count, "orders.csv"));
    const ParseError header = *checkHeader("sequence,command");
    expect(formatParseError(header, "orders.csv")
               == "orders.csv:1: header must be 'sequence,command,order_id,side,price,quantity'",
           "the header diagnostic: " + formatParseError(header, "orders.csv"));
}

// The agreed sample files in tests/replay/ (see its README).
struct MalformedSample {
    std::size_t line;
    std::string column;
    ParseErrorCode code;
    std::size_t recordsBefore;
};

void testSampleFiles(const std::filesystem::path& root) {
    using Code = ParseErrorCode;
    const std::map<std::string, std::vector<ReplayRecord>> valid = {
        {"basic.csv", {ReplayRecord{2, 1, AddCommand{1001, Side::BUY, 100, 50}},
                       ReplayRecord{3, 2, AddCommand{1002, Side::SELL, 105, 30}},
                       ReplayRecord{4, 3, AddCommand{1003, Side::BUY, 105, 20}},
                       ReplayRecord{5, 4, CancelCommand{1001}}}},
        {"book_rejections.csv", {ReplayRecord{2, 1, AddCommand{3001, Side::BUY, 100, 10}},
                                 ReplayRecord{3, 2, AddCommand{3001, Side::SELL, 110, 5}},
                                 ReplayRecord{4, 3, CancelCommand{9999}},
                                 ReplayRecord{5, 4, CancelCommand{3001}},
                                 ReplayRecord{6, 5, CancelCommand{3001}},
                                 ReplayRecord{7, 6, AddCommand{3001, Side::SELL, 110, 5}}}},
        {"crlf_line_endings.csv", {ReplayRecord{2, 1, AddCommand{5001, Side::BUY, 100, 10}},
                                   ReplayRecord{3, 2, AddCommand{5002, Side::SELL, 100, 4}}}},
        {"header_only.csv", {}},
        {"leading_zeros.csv", {ReplayRecord{2, 1, AddCommand{7001, Side::BUY, 100, 10}},
                               ReplayRecord{3, 2, AddCommand{7002, Side::SELL, 100, 4}},
                               ReplayRecord{4, 10, CancelCommand{7001}}}},
        {"limits.csv", {ReplayRecord{2, 1, AddCommand{maxField, Side::SELL, maxField, maxField}},
                        ReplayRecord{3, 2, AddCommand{1, Side::BUY, 1, 1}},
                        ReplayRecord{4, maxSequence, CancelCommand{maxField}}}},
        {"no_final_newline.csv", {ReplayRecord{2, 1, AddCommand{6001, Side::SELL, 300, 7}},
                                  ReplayRecord{3, 2, CancelCommand{6001}}}},
        {"price_time_priority.csv", {ReplayRecord{2, 1, AddCommand{2001, Side::SELL, 101, 10}},
                                     ReplayRecord{3, 2, AddCommand{2002, Side::SELL, 101, 15}},
                                     ReplayRecord{4, 3, AddCommand{2003, Side::SELL, 102, 20}},
                                     ReplayRecord{5, 4, AddCommand{2004, Side::BUY, 99, 5}},
                                     ReplayRecord{6, 5, AddCommand{2005, Side::BUY, 102, 50}},
                                     ReplayRecord{7, 6, AddCommand{2006, Side::SELL, 99, 8}}}},
        {"sequence_gaps.csv", {ReplayRecord{2, 10, AddCommand{4001, Side::SELL, 200, 5}},
                               ReplayRecord{3, 20, AddCommand{4002, Side::SELL, 200, 5}},
                               ReplayRecord{4, 35, CancelCommand{4001}},
                               ReplayRecord{5, 36, AddCommand{4003, Side::BUY, 200, 5}}}},
    };
    const std::map<std::string, MalformedSample> malformed = {
        {"empty_file.csv", {1, "", Code::MISSING_HEADER, 0}},
        {"missing_header.csv", {1, "", Code::MISSING_HEADER, 0}},
        {"header_wrong_order.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"header_missing_column.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"header_duplicate_column.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"header_extra_column.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"header_wrong_case.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"header_with_bom.csv", {1, "", Code::INVALID_HEADER, 0}},
        {"quoted_field.csv", {4, "", Code::QUOTE_CHARACTER, 2}},
        {"too_few_fields.csv", {4, "", Code::WRONG_COLUMN_COUNT, 2}},
        {"too_many_fields.csv", {4, "", Code::WRONG_COLUMN_COUNT, 2}},
        {"blank_line.csv", {4, "", Code::EMPTY_RECORD, 2}},
        {"trailing_blank_line.csv", {5, "", Code::EMPTY_RECORD, 3}},
        {"sequence_repeated.csv", {4, "sequence", Code::SEQUENCE_NOT_INCREASING, 2}},
        {"sequence_decreasing.csv", {4, "sequence", Code::SEQUENCE_NOT_INCREASING, 2}},
        {"sequence_zero.csv", {2, "sequence", Code::INVALID_VALUE, 0}},
        {"sequence_not_a_number.csv", {4, "sequence", Code::INVALID_NUMBER, 2}},
        {"sequence_out_of_range.csv", {4, "sequence", Code::NUMBER_OUT_OF_RANGE, 2}},
        {"command_unknown.csv", {4, "command", Code::UNKNOWN_COMMAND, 2}},
        {"command_lowercase.csv", {4, "command", Code::UNKNOWN_COMMAND, 2}},
        {"command_empty.csv", {4, "command", Code::MISSING_REQUIRED_FIELD, 2}},
        {"order_id_zero.csv", {4, "order_id", Code::INVALID_VALUE, 2}},
        {"add_missing_order_id.csv", {4, "order_id", Code::MISSING_REQUIRED_FIELD, 2}},
        {"cancel_missing_order_id.csv", {4, "order_id", Code::MISSING_REQUIRED_FIELD, 2}},
        {"cancel_order_id_zero.csv", {4, "order_id", Code::INVALID_VALUE, 2}},
        {"side_unknown.csv", {4, "side", Code::INVALID_SIDE, 2}},
        {"side_lowercase.csv", {4, "side", Code::INVALID_SIDE, 2}},
        {"add_missing_side.csv", {4, "side", Code::MISSING_REQUIRED_FIELD, 2}},
        {"add_missing_price.csv", {4, "price", Code::MISSING_REQUIRED_FIELD, 2}},
        {"price_zero.csv", {4, "price", Code::INVALID_VALUE, 2}},
        {"price_not_a_number.csv", {4, "price", Code::INVALID_NUMBER, 2}},
        {"price_decimal.csv", {4, "price", Code::INVALID_NUMBER, 2}},
        {"price_negative.csv", {4, "price", Code::INVALID_NUMBER, 2}},
        {"price_leading_plus.csv", {4, "price", Code::INVALID_NUMBER, 2}},
        {"price_whitespace.csv", {4, "price", Code::INVALID_NUMBER, 2}},
        {"price_out_of_range.csv", {4, "price", Code::NUMBER_OUT_OF_RANGE, 2}},
        {"add_missing_quantity.csv", {4, "quantity", Code::MISSING_REQUIRED_FIELD, 2}},
        {"quantity_zero.csv", {4, "quantity", Code::INVALID_VALUE, 2}},
        {"quantity_zero_padded.csv", {4, "quantity", Code::INVALID_VALUE, 2}},
        {"quantity_negative.csv", {4, "quantity", Code::INVALID_NUMBER, 2}},
        {"quantity_out_of_range.csv", {4, "quantity", Code::NUMBER_OUT_OF_RANGE, 2}},
        {"cancel_with_side.csv", {4, "side", Code::UNEXPECTED_FIELD, 2}},
        {"cancel_with_price.csv", {4, "price", Code::UNEXPECTED_FIELD, 2}},
        {"cancel_with_quantity.csv", {4, "quantity", Code::UNEXPECTED_FIELD, 2}},
        {"cancel_with_space.csv", {4, "side", Code::UNEXPECTED_FIELD, 2}},
    };

    // Binary mode, so CRs reach the reader exactly as written.
    const auto readFile = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        expect(input.is_open(), "cannot open " + path.string());
        return readAll(input);
    };

    std::size_t validSeen = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "valid")) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".csv") {
            continue;
        }
        const auto expected = valid.find(name);
        expect(expected != valid.end(), "valid/" + name + " has no expected records in this test");
        const ReadOutcome outcome = readFile(entry.path());
        expect(!outcome.error, "valid/" + name + ": " + (outcome.error ? describe(*outcome.error) : ""));
        expect(outcome.records == expected->second, "valid/" + name + ": records differ from the expected ones");
        ++validSeen;
    }
    expect(validSeen == valid.size(), "a listed valid sample is missing from " + (root / "valid").string());

    std::size_t malformedSeen = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "malformed")) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".csv") {
            continue;
        }
        const auto expected = malformed.find(name);
        expect(expected != malformed.end(), "malformed/" + name + " has no expected error in this test");
        const MalformedSample& sample = expected->second;
        const ReadOutcome outcome = readFile(entry.path());
        const std::string context = "malformed/" + name;
        expect(outcome.error.has_value(), context + ": read without an error");
        expect(outcome.error->code == sample.code && outcome.error->lineNumber == sample.line
                   && outcome.error->column == sample.column,
               context + ": expected " + parseErrorCodeName(sample.code) + " at line " + std::to_string(sample.line)
                   + " column '" + sample.column + "', got " + describe(*outcome.error));
        expect(outcome.records.size() == sample.recordsBefore, context + ": wrong number of records before the error");
        ++malformedSeen;
    }
    expect(malformedSeen == malformed.size(), "a listed malformed sample is missing from " + (root / "malformed").string());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "FAILED: usage: replay_parser_tests <path to tests/replay>" << std::endl;
        return EXIT_FAILURE;
    }

    testSplitRecord();
    testValidRows();
    testRecordFraming();
    testCommands();
    testNumbers();
    testSequenceOrdering();
    testSides();
    testFieldPresence();
    testCheckingOrder();
    testHeader();
    testReader();
    testFormatting();
    testSampleFiles(argv[1]);

    std::cout << "replay parser tests passed" << std::endl;
    return EXIT_SUCCESS;
}
