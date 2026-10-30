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
            return std::string(columnNamesV2[column]);
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
            const char first = columnNamesV2[column].front();
            const char* article = first == 'o' || first == 'a' || first == 'e' || first == 'i' || first == 'u' ? "an " : "a ";
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
                    const char* expected = column == priceColumn || column == stopPriceColumn ? "a whole number of ticks"
                                                                                              : "an unsigned whole number";
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

    std::variant<ReplayFormat, ParseError> headerFormat(std::string_view text) {
        if (text == expectedHeader) {
            return ReplayFormat::V1;
        }
        if (text == expectedHeaderV2) {
            return ReplayFormat::V2;
        }
        const std::string expected = "'" + std::string(expectedHeader) + "' (v1) or '" + std::string(expectedHeaderV2) + "' (v2)";
        for (const ReplayFormat format : {ReplayFormat::V1, ReplayFormat::V2}) {
            if (std::holds_alternative<ReplayRecord>(parseRow(splitRecord(text, 1), format, std::nullopt))) {
                return ParseError{1, ParseErrorCode::MISSING_HEADER, "", std::string(text),
                                  "missing header: line 1 is a data record; the first line must be " + expected};
            }
        }
        return ParseError{1, ParseErrorCode::INVALID_HEADER, "", std::string(text), "header must be " + expected};
    }

    std::optional<ParseError> checkHeader(std::string_view text) {
        std::variant<ReplayFormat, ParseError> format = headerFormat(text);
        if (ParseError* error = std::get_if<ParseError>(&format)) {
            return std::move(*error);
        }
        return std::nullopt;
    }

    namespace {
        // v1's rules for side, price, and quantity on an ADD, or their absence on a CANCEL.
        ParseResult parseV1Tail(const CsvRow& row, std::uint64_t sequence, const std::string& commandText,
                                OrderId orderId) {
            if (commandText == "CANCEL") {
                for (std::size_t column : {sideColumn, priceColumn, quantityColumn}) {
                    if (!row.fields[column].empty()) {
                        return fieldError(row, column, ParseErrorCode::UNEXPECTED_FIELD,
                                          "CANCEL must leave " + columnName(column) + " empty");
                    }
                }
                return ReplayRecord{row.lineNumber, sequence, CancelCommand{orderId}};
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

            return ReplayRecord{row.lineNumber, sequence,
                                AddCommand{orderId, side, static_cast<Price>(price.value),
                                           static_cast<Quantity>(quantity.value)}};
        }

        std::optional<ParseError> expectEmpty(const CsvRow& row, std::size_t column, const std::string& why) {
            if (row.fields[column].empty()) {
                return std::nullopt;
            }
            return fieldError(row, column, ParseErrorCode::UNEXPECTED_FIELD, why + " must leave " + columnName(column) + " empty");
        }

        struct OwnershipResult {
            OwnerId owner = 0;
            SelfTradePrevention mode = SelfTradePrevention::CANCEL_RESTING;
            bool postOnly = false;
            std::optional<ParseError> error;
        };

        // post_only (empty or Y), then owner (empty or positive), then self_trade_prevention,
        // required exactly when there is an owner. postOnlyAllowed is false for an ADD that is
        // not a GTC LIMIT order, which may not be post-only.
        OwnershipResult parseInstructions(const CsvRow& row, bool postOnlyAllowed) {
            OwnershipResult result;
            const std::string& postOnlyText = row.fields[postOnlyColumn];
            if (!postOnlyText.empty() && postOnlyText != "Y") {
                result.error = fieldError(row, postOnlyColumn, ParseErrorCode::INVALID_POST_ONLY,
                                          "'" + postOnlyText + "' is not post_only (expected Y or empty)");
                return result;
            }
            result.postOnly = postOnlyText == "Y";
            if (result.postOnly && !postOnlyAllowed) {
                result.error = fieldError(row, postOnlyColumn, ParseErrorCode::INVALID_COMBINATION,
                                          "post_only needs a GTC LIMIT order");
                return result;
            }

            if (!row.fields[ownerColumn].empty()) {
                const NumberResult owner =
                    parseRequiredNumber(row, ownerColumn, std::numeric_limits<OwnerId>::max(), "");
                if (owner.error) {
                    result.error = owner.error;
                    return result;
                }
                result.owner = static_cast<OwnerId>(owner.value);
            }

            const std::string& modeText = row.fields[selfTradePreventionColumn];
            if (result.owner == 0) {
                if (!modeText.empty()) {
                    result.error = fieldError(row, selfTradePreventionColumn, ParseErrorCode::UNEXPECTED_FIELD,
                                              "self_trade_prevention needs an owner; leave it empty without one");
                }
                return result;
            }
            if (modeText.empty()) {
                result.error = fieldError(row, selfTradePreventionColumn, ParseErrorCode::MISSING_REQUIRED_FIELD,
                                          "an owner requires a self_trade_prevention");
                return result;
            }
            if (modeText == "CANCEL_RESTING") {
                result.mode = SelfTradePrevention::CANCEL_RESTING;
            } else if (modeText == "CANCEL_INCOMING") {
                result.mode = SelfTradePrevention::CANCEL_INCOMING;
            } else if (modeText == "CANCEL_BOTH") {
                result.mode = SelfTradePrevention::CANCEL_BOTH;
            } else {
                result.error = fieldError(row, selfTradePreventionColumn, ParseErrorCode::INVALID_SELF_TRADE_PREVENTION,
                                          "'" + modeText + "' is not a mode (expected CANCEL_RESTING, CANCEL_INCOMING, or CANCEL_BOTH)");
            }
            return result;
        }

        ParseResult parseV2Add(const CsvRow& row, std::uint64_t sequence, OrderId orderId) {
            const std::string command = "ADD";
            const std::string& typeText = row.fields[orderTypeColumn];
            if (typeText.empty()) {
                return missingField(row, orderTypeColumn, command);
            }
            OrderType type = OrderType::LIMIT;
            if (typeText == "LIMIT") {
                type = OrderType::LIMIT;
            } else if (typeText == "MARKET") {
                type = OrderType::MARKET;
            } else if (typeText == "STOP") {
                type = OrderType::STOP;
            } else if (typeText == "STOP_LIMIT") {
                type = OrderType::STOP_LIMIT;
            } else {
                return fieldError(row, orderTypeColumn, ParseErrorCode::INVALID_ORDER_TYPE,
                                  "'" + typeText + "' is not an order type (expected LIMIT, MARKET, STOP, or STOP_LIMIT)");
            }
            const bool priceless = type == OrderType::MARKET || type == OrderType::STOP;
            const bool stop = type == OrderType::STOP || type == OrderType::STOP_LIMIT;

            const std::string& timeInForceText = row.fields[timeInForceColumn];
            if (timeInForceText.empty()) {
                return missingField(row, timeInForceColumn, command);
            }
            TimeInForce timeInForce = TimeInForce::GTC;
            if (timeInForceText == "GTC") {
                timeInForce = TimeInForce::GTC;
            } else if (timeInForceText == "IOC") {
                timeInForce = TimeInForce::IOC;
            } else if (timeInForceText == "FOK") {
                timeInForce = TimeInForce::FOK;
            } else {
                return fieldError(row, timeInForceColumn, ParseErrorCode::INVALID_TIME_IN_FORCE,
                                  "'" + timeInForceText + "' is not a time in force (expected GTC, IOC, or FOK)");
            }
            if (priceless && timeInForce == TimeInForce::GTC) {
                return fieldError(row, timeInForceColumn, ParseErrorCode::INVALID_COMBINATION,
                                  "a " + typeText + " order must be IOC or FOK, not GTC");
            }

            const std::string& sideText = row.fields[sideColumn];
            if (sideText.empty()) {
                return missingField(row, sideColumn, command);
            }
            if (sideText != "BUY" && sideText != "SELL") {
                return fieldError(row, sideColumn, ParseErrorCode::INVALID_SIDE,
                                  "'" + sideText + "' is not a side (expected BUY or SELL)");
            }
            const Side side = sideText == "BUY" ? Side::BUY : Side::SELL;

            Price price = 0;
            if (priceless) {
                if (std::optional<ParseError> error = expectEmpty(row, priceColumn, "a " + typeText + " ADD")) {
                    return *error;
                }
            } else {
                const NumberResult parsed = parseRequiredNumber(row, priceColumn, engineFieldMax, command);
                if (parsed.error) {
                    return *parsed.error;
                }
                price = static_cast<Price>(parsed.value);
            }

            const NumberResult quantity = parseRequiredNumber(row, quantityColumn, engineFieldMax, command);
            if (quantity.error) {
                return *quantity.error;
            }

            Price stopPrice = 0;
            if (stop) {
                const NumberResult parsed = parseRequiredNumber(row, stopPriceColumn, engineFieldMax, "a " + typeText + " ADD");
                if (parsed.error) {
                    return *parsed.error;
                }
                stopPrice = static_cast<Price>(parsed.value);
            } else if (std::optional<ParseError> error = expectEmpty(row, stopPriceColumn, "a " + typeText + " ADD")) {
                return *error;
            }

            const OwnershipResult instructions =
                parseInstructions(row, type == OrderType::LIMIT && timeInForce == TimeInForce::GTC);
            if (instructions.error) {
                return *instructions.error;
            }
            return ReplayRecord{row.lineNumber, sequence,
                                AddCommand{orderId, side, price, static_cast<Quantity>(quantity.value), type, timeInForce,
                                           stopPrice, instructions.postOnly, instructions.owner, instructions.mode}};
        }

        ParseResult parseV2Replace(const CsvRow& row, std::uint64_t sequence, OrderId orderId) {
            const std::string command = "REPLACE";
            for (std::size_t column : {orderTypeColumn, timeInForceColumn, sideColumn}) {
                if (std::optional<ParseError> error = expectEmpty(row, column, command)) {
                    return *error;
                }
            }
            const NumberResult price = parseRequiredNumber(row, priceColumn, engineFieldMax, command);
            if (price.error) {
                return *price.error;
            }
            const NumberResult quantity = parseRequiredNumber(row, quantityColumn, engineFieldMax, command);
            if (quantity.error) {
                return *quantity.error;
            }
            if (std::optional<ParseError> error = expectEmpty(row, stopPriceColumn, command)) {
                return *error;
            }
            // A replacement is always a GTC limit order, so post-only is always allowed.
            const OwnershipResult instructions = parseInstructions(row, true);
            if (instructions.error) {
                return *instructions.error;
            }
            return ReplayRecord{row.lineNumber, sequence,
                                ReplaceCommand{orderId, static_cast<Price>(price.value),
                                               static_cast<Quantity>(quantity.value), instructions.postOnly,
                                               instructions.owner, instructions.mode}};
        }

        ParseResult parseV2Tail(const CsvRow& row, std::uint64_t sequence, const std::string& commandText,
                                OrderId orderId) {
            if (commandText == "CANCEL") {
                for (std::size_t column : {orderTypeColumn, timeInForceColumn, sideColumn, priceColumn, quantityColumn,
                                           stopPriceColumn, postOnlyColumn, ownerColumn, selfTradePreventionColumn}) {
                    if (std::optional<ParseError> error = expectEmpty(row, column, "CANCEL")) {
                        return *error;
                    }
                }
                return ReplayRecord{row.lineNumber, sequence, CancelCommand{orderId}};
            }
            if (commandText == "REPLACE") {
                return parseV2Replace(row, sequence, orderId);
            }
            return parseV2Add(row, sequence, orderId);
        }
    }  // namespace

    ParseResult parseRow(const CsvRow& row, std::optional<std::uint64_t> previousSequence) {
        return parseRow(row, ReplayFormat::V1, previousSequence);
    }

    ParseResult parseRow(const CsvRow& row, ReplayFormat format, std::optional<std::uint64_t> previousSequence) {
        if (row.fields.empty() || (row.fields.size() == 1 && row.fields[0].empty())) {
            return recordError(row, ParseErrorCode::EMPTY_RECORD, "empty record (blank lines are not allowed)");
        }
        for (const std::string& field : row.fields) {
            if (field.find('"') != std::string::npos) {
                return recordError(row, ParseErrorCode::QUOTE_CHARACTER, "quote characters are not allowed (fields are never quoted)");
            }
        }
        const std::size_t fields = format == ReplayFormat::V1 ? columnCount : columnCountV2;
        if (row.fields.size() != fields) {
            return recordError(row, ParseErrorCode::WRONG_COLUMN_COUNT,
                               "expected " + std::to_string(fields) + " fields, found " + std::to_string(row.fields.size()));
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
        const bool known = commandText == "ADD" || commandText == "CANCEL" ||
                           (format == ReplayFormat::V2 && commandText == "REPLACE");
        if (!known) {
            return fieldError(row, commandColumn, ParseErrorCode::UNKNOWN_COMMAND,
                              "'" + commandText + "' is not a command (expected "
                                  + (format == ReplayFormat::V1 ? "ADD or CANCEL" : "ADD, CANCEL, or REPLACE") + ")");
        }

        const NumberResult orderId = parseRequiredNumber(row, orderIdColumn, engineFieldMax, commandText);
        if (orderId.error) {
            return *orderId.error;
        }
        const auto id = static_cast<OrderId>(orderId.value);
        return format == ReplayFormat::V1 ? parseV1Tail(row, sequence.value, commandText, id)
                                          : parseV2Tail(row, sequence.value, commandText, id);
    }

    ReadResult ReplayReader::next() {
        if (finalResult) {
            return *finalResult;
        }

        if (!headerChecked) {
            headerChecked = true;
            if (!readLine()) {
                finalResult = ReadResult{ParseError{1, ParseErrorCode::MISSING_HEADER, "", "",
                    "missing header: the input is empty; the first line must be '" + std::string(expectedHeader)
                        + "' (v1) or '" + std::string(expectedHeaderV2) + "' (v2)"}};
                return *finalResult;
            }
            std::variant<ReplayFormat, ParseError> format = headerFormat(line);
            if (ParseError* error = std::get_if<ParseError>(&format)) {
                finalResult = ReadResult{std::move(*error)};
                return *finalResult;
            }
            layout = std::get<ReplayFormat>(format);
        }

        if (!readLine()) {
            finalResult = ReadResult{EndOfInput{}};
            return *finalResult;
        }
        splitRecordInto(line, linesRead, row);
        ParseResult result = parseRow(row, layout, previousSequence);
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
