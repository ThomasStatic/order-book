#include "order_book/replay_parser.hpp"

#include <cstring>
#include <initializer_list>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace order_book::replay {
    namespace {
        std::string columnName(std::size_t column) {
            return std::string(columnNames[column]);
        }

        // Exact, since splitRecord splits on every comma.
        std::string joinFields(const CsvRow& row) {
            std::string text;
            for (std::size_t i = 0; i < row.fields.size(); ++i) {
                if (i > 0) {
                    text += ',';
                }
                text += row.fields[i];
            }
            return text;
        }

        ParseError recordError(const CsvRow& row, ParseErrorCode code, std::string message) {
            return ParseError{row.lineNumber, code, "", joinFields(row), std::move(message)};
        }

        ParseError fieldError(const CsvRow& row, std::size_t column, ParseErrorCode code, std::string message) {
            return ParseError{row.lineNumber, code, columnName(column), row.fields[column], std::move(message)};
        }

        // command is empty for the columns read before the command is known.
        ParseError missingField(const CsvRow& row, std::size_t column, const std::string& command) {
            if (command.empty()) {
                return fieldError(row, column, ParseErrorCode::MISSING_REQUIRED_FIELD, columnName(column) + " is required");
            }
            const char* article = column == orderIdColumn ? "an " : "a ";
            return fieldError(row, column, ParseErrorCode::MISSING_REQUIRED_FIELD,
                              command + " requires " + article + columnName(column));
        }

        struct NumberResult {
            std::uint64_t value = 0;
            std::optional<ParseError> error;
        };

        // A required field holding a positive decimal number no larger than max. Characters
        // are compared directly, so no locale applies. command is as for missingField.
        NumberResult parseRequiredNumber(const CsvRow& row, std::size_t column, std::uint64_t max,
                                         const std::string& command) {
            const std::string& text = row.fields[column];
            if (text.empty()) {
                return {0, missingField(row, column, command)};
            }
            for (char c : text) {
                if (c < '0' || c > '9') {
                    const char* expected = column == priceColumn ? "a whole number of ticks" : "an unsigned whole number";
                    return {0, fieldError(row, column, ParseErrorCode::INVALID_NUMBER,
                                          "'" + text + "' is not " + expected)};
                }
            }

            std::uint64_t value = 0;
            for (char c : text) {
                const auto digit = static_cast<std::uint64_t>(c - '0');
                // value * 10 + digit <= max, without overflowing.
                if (value > (max - digit) / 10) {
                    return {0, fieldError(row, column, ParseErrorCode::NUMBER_OUT_OF_RANGE,
                                          "'" + text + "' exceeds the maximum " + columnName(column) + " "
                                          + std::to_string(max))};
                }
                value = value * 10 + digit;
            }

            if (value == 0) {
                return {0, fieldError(row, column, ParseErrorCode::INVALID_VALUE,
                                      columnName(column) + " must be positive, not '" + text + "'")};
            }
            return {value, std::nullopt};
        }

        // order_id, price, and quantity, which share one engine type.
        static_assert(std::numeric_limits<OrderId>::max() == std::numeric_limits<Price>::max()
                      && std::numeric_limits<Price>::max() == std::numeric_limits<Quantity>::max());
        constexpr std::uint64_t engineFieldMax = std::numeric_limits<OrderId>::max();
    }  // namespace

    void splitRecordInto(std::string_view text, std::size_t lineNumber, CsvRow& row) {
        row.lineNumber = lineNumber;
        std::size_t count = 0;
        std::size_t start = 0;
        while (true) {
            const std::size_t comma = text.find(',', start);
            const std::string_view field =
                comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
            if (count < row.fields.size()) {
                row.fields[count].assign(field);
            } else {
                row.fields.emplace_back(field);
            }
            ++count;
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        row.fields.resize(count);
    }

    CsvRow splitRecord(std::string_view text, std::size_t lineNumber) {
        CsvRow row{lineNumber, {}};
        splitRecordInto(text, lineNumber, row);
        return row;
    }

    std::optional<ParseError> checkHeader(std::string_view text) {
        if (text == expectedHeader) {
            return std::nullopt;
        }
        const std::string expected = "'" + std::string(expectedHeader) + "'";
        if (std::holds_alternative<ReplayRecord>(parseRow(splitRecord(text, 1)))) {
            return ParseError{1, ParseErrorCode::MISSING_HEADER, "", std::string(text),
                              "missing header: line 1 is a data record; the first line must be " + expected};
        }
        return ParseError{1, ParseErrorCode::INVALID_HEADER, "", std::string(text), "header must be " + expected};
    }

    ParseResult parseRow(const CsvRow& row, std::optional<std::uint64_t> previousSequence) {
        if (row.fields.empty() || (row.fields.size() == 1 && row.fields[0].empty())) {
            return recordError(row, ParseErrorCode::EMPTY_RECORD, "empty record (blank lines are not allowed)");
        }
        for (const std::string& field : row.fields) {
            if (field.find('"') != std::string::npos) {
                return recordError(row, ParseErrorCode::QUOTE_CHARACTER, "quote characters are not allowed (fields are never quoted)");
            }
        }
        if (row.fields.size() != columnCount) {
            return recordError(row, ParseErrorCode::WRONG_COLUMN_COUNT,
                               "expected " + std::to_string(columnCount) + " fields, found " + std::to_string(row.fields.size()));
        }

        const NumberResult sequence = parseRequiredNumber(row, sequenceColumn, std::numeric_limits<std::uint64_t>::max(), "");
        if (sequence.error) {
            return *sequence.error;
        }
        if (previousSequence && sequence.value <= *previousSequence) {
            return fieldError(row, sequenceColumn, ParseErrorCode::SEQUENCE_NOT_INCREASING,
                              std::to_string(sequence.value) + " does not follow " + std::to_string(*previousSequence)
                              + " (sequence numbers must strictly increase)");
        }

        const std::string& commandText = row.fields[commandColumn];
        if (commandText.empty()) {
            return missingField(row, commandColumn, "");
        }
        if (commandText != "ADD" && commandText != "CANCEL") {
            return fieldError(row, commandColumn, ParseErrorCode::UNKNOWN_COMMAND,
                              "'" + commandText + "' is not a command (expected ADD or CANCEL)");
        }

        const NumberResult orderId = parseRequiredNumber(row, orderIdColumn, engineFieldMax, commandText);
        if (orderId.error) {
            return *orderId.error;
        }

        if (commandText == "CANCEL") {
            for (std::size_t column : {sideColumn, priceColumn, quantityColumn}) {
                if (!row.fields[column].empty()) {
                    return fieldError(row, column, ParseErrorCode::UNEXPECTED_FIELD,
                                      "CANCEL must leave " + columnName(column) + " empty");
                }
            }
            return ReplayRecord{row.lineNumber, sequence.value, CancelCommand{static_cast<OrderId>(orderId.value)}};
        }

        const std::string& sideText = row.fields[sideColumn];
        if (sideText.empty()) {
            return missingField(row, sideColumn, commandText);
        }
        if (sideText != "BUY" && sideText != "SELL") {
            return fieldError(row, sideColumn, ParseErrorCode::INVALID_SIDE,
                              "'" + sideText + "' is not a side (expected BUY or SELL)");
        }
        const Side side = sideText == "BUY" ? Side::BUY : Side::SELL;

        const NumberResult price = parseRequiredNumber(row, priceColumn, engineFieldMax, commandText);
        if (price.error) {
            return *price.error;
        }

        const NumberResult quantity = parseRequiredNumber(row, quantityColumn, engineFieldMax, commandText);
        if (quantity.error) {
            return *quantity.error;
        }

        return ReplayRecord{row.lineNumber, sequence.value,
                            AddCommand{static_cast<OrderId>(orderId.value), side, static_cast<Price>(price.value),
                                       static_cast<Quantity>(quantity.value)}};
    }

    ReadResult ReplayReader::next() {
        if (finalResult) {
            return *finalResult;
        }

        if (!headerChecked) {
            headerChecked = true;
            if (!readLine()) {
                finalResult = ReadResult{ParseError{1, ParseErrorCode::MISSING_HEADER, "", "",
                    "missing header: the input is empty; the first line must be '" + std::string(expectedHeader) + "'"}};
                return *finalResult;
            }
            if (std::optional<ParseError> error = checkHeader(line)) {
                finalResult = ReadResult{std::move(*error)};
                return *finalResult;
            }
        }

        if (!readLine()) {
            finalResult = ReadResult{EndOfInput{}};
            return *finalResult;
        }
        splitRecordInto(line, linesRead, row);
        ParseResult result = parseRow(row, previousSequence);
        if (ReplayRecord* record = std::get_if<ReplayRecord>(&result)) {
            previousSequence = record->sequence;
            return std::move(*record);
        }
        finalResult = ReadResult{std::get<ParseError>(std::move(result))};
        return *finalResult;
    }

    bool ReplayReader::refill() {
        if (inputExhausted) {
            return false;
        }
        chunk.resize(chunkCapacity);
        const auto capacity = static_cast<std::streamsize>(chunkCapacity);
        // readsome copies only what the stream has already buffered, and peek makes it fetch
        // more when nothing is. So a stream that fails while fetching never takes bytes it
        // had already delivered with it (istream::read can: if the buffer throws, it reports
        // nothing read), and the complete lines before a failure still stand, as they did when
        // the reader read a line at a time.
        std::streamsize count = input.readsome(chunk.data(), capacity);
        if (count == 0 && input.good()
            && !std::char_traits<char>::eq_int_type(input.peek(), std::char_traits<char>::eof())) {
            count = input.readsome(chunk.data(), capacity);
        }
        chunkSize = static_cast<std::size_t>(count);
        chunkPosition = 0;
        if (chunkSize > 0) {
            return true;
        }
        if (input.bad()) {
            throw std::runtime_error("error reading replay input");
        }
        inputExhausted = true;
        return false;
    }

    bool ReplayReader::readLine() {
        line.clear();
        bool consumedAny = false;
        while (true) {
            if (chunkPosition == chunkSize && !refill()) {
                // The input ended. Any text since the last LF is a final line with no ending,
                // so a CR at its end is not a line ending and stays in its field. No text at
                // all means there is no further line.
                if (!consumedAny) {
                    return false;
                }
                ++linesRead;
                return true;
            }
            const char* const begin = chunk.data() + chunkPosition;
            const std::size_t available = chunkSize - chunkPosition;
            const void* const newline = std::memchr(begin, '\n', available);
            if (newline == nullptr) {
                line.append(begin, available);
                chunkPosition = chunkSize;
                consumedAny = true;
                continue;
            }
            const std::size_t length = static_cast<std::size_t>(static_cast<const char*>(newline) - begin);
            line.append(begin, length);
            chunkPosition += length + 1;
            ++linesRead;
            // The LF ends the line; a CR directly before it is part of the line ending.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
    }

    std::string formatParseError(const ParseError& error, std::string_view path) {
        std::string text(path);
        text += ':';
        text += std::to_string(error.lineNumber);
        text += ": ";
        if (!error.column.empty()) {
            text += error.column;
            text += ": ";
        }
        text += error.message;
        return text;
    }
}  // namespace order_book::replay
