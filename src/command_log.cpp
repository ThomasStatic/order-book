#include "order_book/command_log.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "order_book/xxh64.hpp"

namespace order_book::replay {
    namespace {
        // Byte values are the format's own, never the C++ enumerators, and 0 is always absent.
        constexpr std::uint8_t commandAdd = 1;
        constexpr std::uint8_t commandCancel = 2;
        constexpr std::uint8_t commandReplace = 3;

        constexpr std::uint8_t flagPostOnly = 1;

        // Field offsets within a record.
        constexpr std::size_t commandOffset = 4;
        constexpr std::size_t sideOffset = 5;
        constexpr std::size_t orderTypeOffset = 6;
        constexpr std::size_t timeInForceOffset = 7;
        constexpr std::size_t symbolOffset = 8;
        constexpr std::size_t orderIdOffset = 16;
        constexpr std::size_t priceOffset = 20;
        constexpr std::size_t quantityOffset = 24;
        constexpr std::size_t stopPriceOffset = 28;
        constexpr std::size_t ownerOffset = 32;
        constexpr std::size_t flagsOffset = 36;
        constexpr std::size_t selfTradePreventionOffset = 37;
        constexpr std::size_t reservedOffset = 38;

        constexpr std::uint8_t sideByte(Side side) noexcept {
            return side == Side::BUY ? 1 : 2;
        }

        constexpr std::uint8_t orderTypeByte(OrderType type) noexcept {
            switch (type) {
                case OrderType::LIMIT: return 1;
                case OrderType::MARKET: return 2;
                case OrderType::STOP: return 3;
                case OrderType::STOP_LIMIT: return 4;
            }
            return 0;
        }

        constexpr std::uint8_t timeInForceByte(TimeInForce timeInForce) noexcept {
            switch (timeInForce) {
                case TimeInForce::GTC: return 1;
                case TimeInForce::IOC: return 2;
                case TimeInForce::FOK: return 3;
            }
            return 0;
        }

        constexpr std::uint8_t selfTradePreventionByte(SelfTradePrevention mode) noexcept {
            switch (mode) {
                case SelfTradePrevention::CANCEL_RESTING: return 1;
                case SelfTradePrevention::CANCEL_INCOMING: return 2;
                case SelfTradePrevention::CANCEL_BOTH: return 3;
            }
            return 0;
        }

        void store8(std::span<std::byte> out, std::size_t offset, std::uint8_t value) noexcept {
            out[offset] = static_cast<std::byte>(value);
        }

        void store16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
            store8(out, offset, static_cast<std::uint8_t>(value & 0xFFU));
            store8(out, offset + 1, static_cast<std::uint8_t>(value >> 8));
        }

        // A memcpy where the machine is little-endian: MSVC does not merge a loop of byte stores,
        // and a record's encode and read run per command.
        template <typename Word>
        void storeWord(std::span<std::byte> out, std::size_t offset, Word value) noexcept {
            if constexpr (std::endian::native == std::endian::little) {
                std::memcpy(out.data() + offset, &value, sizeof(value));
            } else {
                for (std::size_t i = 0; i < sizeof(value); ++i) {
                    store8(out, offset + i, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU));
                }
            }
        }

        void store32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
            storeWord(out, offset, value);
        }

        void store64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
            storeWord(out, offset, value);
        }

        std::uint8_t load8(std::span<const std::byte> in, std::size_t offset) noexcept {
            return static_cast<std::uint8_t>(in[offset]);
        }

        std::uint16_t load16(std::span<const std::byte> in, std::size_t offset) noexcept {
            return static_cast<std::uint16_t>(load8(in, offset) | (load8(in, offset + 1) << 8));
        }

        template <typename Word>
        Word loadWord(std::span<const std::byte> in, std::size_t offset) noexcept {
            Word value = 0;
            if constexpr (std::endian::native == std::endian::little) {
                std::memcpy(&value, in.data() + offset, sizeof(value));
            } else {
                for (std::size_t i = 0; i < sizeof(value); ++i) {
                    value = static_cast<Word>(value | (static_cast<Word>(load8(in, offset + i)) << (i * 8)));
                }
            }
            return value;
        }

        std::uint32_t load32(std::span<const std::byte> in, std::size_t offset) noexcept {
            return loadWord<std::uint32_t>(in, offset);
        }

        std::uint64_t load64(std::span<const std::byte> in, std::size_t offset) noexcept {
            return loadWord<std::uint64_t>(in, offset);
        }

        std::uint32_t chainChecksum(std::span<const std::byte> record, std::uint32_t previousChecksum) noexcept {
            return static_cast<std::uint32_t>(xxh64(record.subspan(commandOffset), previousChecksum) & 0xFFFFFFFFULL);
        }

        void encodeInstructions(std::span<std::byte> out, bool postOnly, OwnerId owner, SelfTradePrevention mode) noexcept {
            store32(out, ownerOffset, owner);
            store8(out, flagsOffset, postOnly ? flagPostOnly : 0);
            store8(out, selfTradePreventionOffset, owner == 0 ? 0 : selfTradePreventionByte(mode));
        }

        // Everything a row's checks see, unpacked.
        struct Fields {
            std::uint8_t command;
            std::uint8_t side;
            std::uint8_t orderType;
            std::uint8_t timeInForce;
            OrderId orderId;
            Price price;
            Quantity quantity;
            Price stopPrice;
            OwnerId owner;
            bool postOnly;
            std::uint8_t selfTradePrevention;
        };

        class Decoder {
        public:
            Decoder(const Fields& unpacked, std::size_t line) : fields(unpacked), lineNumber(line) {}

            ParseError error(ParseErrorCode code, std::size_t column, std::uint64_t value, const std::string& message) const {
                return ParseError{lineNumber, code, std::string(columnNamesV3[column]), std::to_string(value), message};
            }

            // Each returns the first problem with one part of the record, in a v3 row's order.
            std::optional<ParseError> checkCancel() const {
                const std::array<std::pair<std::size_t, std::uint64_t>, 9> columns{{
                    {orderTypeColumn, fields.orderType},
                    {timeInForceColumn, fields.timeInForce},
                    {sideColumn, fields.side},
                    {priceColumn, fields.price},
                    {quantityColumn, fields.quantity},
                    {stopPriceColumn, fields.stopPrice},
                    {postOnlyColumn, fields.postOnly ? 1U : 0U},
                    {ownerColumn, fields.owner},
                    {selfTradePreventionColumn, fields.selfTradePrevention},
                }};
                for (const auto& [column, value] : columns) {
                    if (value != 0) {
                        return error(ParseErrorCode::UNEXPECTED_FIELD, column, value,
                                     "CANCEL must leave " + std::string(columnNamesV3[column]) + " absent");
                    }
                }
                return std::nullopt;
            }

            std::optional<ParseError> required(std::size_t column, std::uint64_t value, const char* command) const {
                if (value != 0) {
                    return std::nullopt;
                }
                return error(ParseErrorCode::MISSING_REQUIRED_FIELD, column, value,
                             std::string(command) + " requires " + std::string(columnNamesV3[column]));
            }

            std::optional<ParseError> absent(std::size_t column, std::uint64_t value, const char* command) const {
                if (value == 0) {
                    return std::nullopt;
                }
                return error(ParseErrorCode::UNEXPECTED_FIELD, column, value,
                             std::string(command) + " must leave " + std::string(columnNamesV3[column]) + " absent");
            }

            std::optional<ParseError> checkAdd() const {
                if (std::optional<ParseError> problem = required(orderTypeColumn, fields.orderType, "ADD")) {
                    return problem;
                }
                if (fields.orderType > 4) {
                    return error(ParseErrorCode::INVALID_ORDER_TYPE, orderTypeColumn, fields.orderType, "not an order type");
                }
                if (std::optional<ParseError> problem = required(timeInForceColumn, fields.timeInForce, "ADD")) {
                    return problem;
                }
                if (fields.timeInForce > 3) {
                    return error(ParseErrorCode::INVALID_TIME_IN_FORCE, timeInForceColumn, fields.timeInForce,
                                 "not a time in force");
                }
                const bool priceless = fields.orderType == 2 || fields.orderType == 3;
                const bool stop = fields.orderType == 3 || fields.orderType == 4;
                if (priceless && fields.timeInForce == 1) {
                    return error(ParseErrorCode::INVALID_COMBINATION, timeInForceColumn, fields.timeInForce,
                                 "a MARKET or STOP order must be IOC or FOK, not GTC");
                }
                if (std::optional<ParseError> problem = required(sideColumn, fields.side, "ADD")) {
                    return problem;
                }
                if (fields.side > 2) {
                    return error(ParseErrorCode::INVALID_SIDE, sideColumn, fields.side, "not a side");
                }
                if (std::optional<ParseError> problem = priceless ? absent(priceColumn, fields.price, "a priceless ADD")
                                                                  : required(priceColumn, fields.price, "ADD")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = required(quantityColumn, fields.quantity, "ADD")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = stop ? required(stopPriceColumn, fields.stopPrice, "a stop ADD")
                                                             : absent(stopPriceColumn, fields.stopPrice, "a non-stop ADD")) {
                    return problem;
                }
                return checkInstructions(fields.orderType == 1 && fields.timeInForce == 1);
            }

            std::optional<ParseError> checkReplace() const {
                if (std::optional<ParseError> problem = absent(orderTypeColumn, fields.orderType, "REPLACE")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = absent(timeInForceColumn, fields.timeInForce, "REPLACE")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = absent(sideColumn, fields.side, "REPLACE")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = required(priceColumn, fields.price, "REPLACE")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = required(quantityColumn, fields.quantity, "REPLACE")) {
                    return problem;
                }
                if (std::optional<ParseError> problem = absent(stopPriceColumn, fields.stopPrice, "REPLACE")) {
                    return problem;
                }
                return checkInstructions(true);  // a replacement is always a GTC limit order
            }

            std::optional<ParseError> checkInstructions(bool postOnlyAllowed) const {
                if (fields.postOnly && !postOnlyAllowed) {
                    return error(ParseErrorCode::INVALID_COMBINATION, postOnlyColumn, 1, "post_only needs a GTC LIMIT order");
                }
                if (fields.owner == 0) {
                    if (fields.selfTradePrevention != 0) {
                        return error(ParseErrorCode::UNEXPECTED_FIELD, selfTradePreventionColumn, fields.selfTradePrevention,
                                     "self_trade_prevention needs an owner");
                    }
                    return std::nullopt;
                }
                if (fields.selfTradePrevention == 0) {
                    return error(ParseErrorCode::MISSING_REQUIRED_FIELD, selfTradePreventionColumn, 0,
                                 "an owner requires a self_trade_prevention");
                }
                if (fields.selfTradePrevention > 3) {
                    return error(ParseErrorCode::INVALID_SELF_TRADE_PREVENTION, selfTradePreventionColumn,
                                 fields.selfTradePrevention, "not a mode");
                }
                return std::nullopt;
            }

            SelfTradePrevention mode() const {
                return fields.selfTradePrevention == 2   ? SelfTradePrevention::CANCEL_INCOMING
                       : fields.selfTradePrevention == 3 ? SelfTradePrevention::CANCEL_BOTH
                                                         : SelfTradePrevention::CANCEL_RESTING;
            }

            Command command() const {
                if (fields.command == commandCancel) {
                    return CancelCommand{fields.orderId};
                }
                if (fields.command == commandReplace) {
                    return ReplaceCommand{fields.orderId, fields.price,  fields.quantity,
                                          fields.postOnly, fields.owner, mode()};
                }
                constexpr std::array<OrderType, 5> types{OrderType::LIMIT, OrderType::LIMIT, OrderType::MARKET,
                                                         OrderType::STOP, OrderType::STOP_LIMIT};
                constexpr std::array<TimeInForce, 4> times{TimeInForce::GTC, TimeInForce::GTC, TimeInForce::IOC,
                                                           TimeInForce::FOK};
                return AddCommand{fields.orderId, fields.side == 1 ? Side::BUY : Side::SELL, fields.price, fields.quantity,
                                  types[fields.orderType],     times[fields.timeInForce],   fields.stopPrice,
                                  fields.postOnly,             fields.owner,                mode()};
            }

        private:
            const Fields& fields;
            std::size_t lineNumber;
        };

        const char* commandName(const Command& command) {
            return std::holds_alternative<AddCommand>(command)      ? "ADD"
                   : std::holds_alternative<CancelCommand>(command) ? "CANCEL"
                                                                    : "REPLACE";
        }

        const char* orderTypeName(OrderType type) {
            switch (type) {
                case OrderType::LIMIT: return "LIMIT";
                case OrderType::MARKET: return "MARKET";
                case OrderType::STOP: return "STOP";
                case OrderType::STOP_LIMIT: return "STOP_LIMIT";
            }
            return "";
        }

        const char* timeInForceName(TimeInForce timeInForce) {
            switch (timeInForce) {
                case TimeInForce::GTC: return "GTC";
                case TimeInForce::IOC: return "IOC";
                case TimeInForce::FOK: return "FOK";
            }
            return "";
        }

        const char* modeName(SelfTradePrevention mode) {
            switch (mode) {
                case SelfTradePrevention::CANCEL_RESTING: return "CANCEL_RESTING";
                case SelfTradePrevention::CANCEL_INCOMING: return "CANCEL_INCOMING";
                case SelfTradePrevention::CANCEL_BOTH: return "CANCEL_BOTH";
            }
            return "";
        }

        std::string numberOrEmpty(std::uint32_t value) {
            return value == 0 ? std::string() : std::to_string(value);
        }

        // post_only, owner, self_trade_prevention, as a v2 row's last three columns.
        std::string instructionColumns(bool postOnly, OwnerId owner, SelfTradePrevention mode) {
            return std::string(postOnly ? "Y" : "") + ',' + numberOrEmpty(owner) + ',' + (owner == 0 ? "" : modeName(mode));
        }

        ParseError fileError(std::size_t lineNumber, ParseErrorCode code, std::string message) {
            return ParseError{lineNumber, code, "", "", std::move(message)};
        }
    }  // namespace

    std::vector<std::byte> encodeLogHeader(const LogHeader& header) {
        const std::size_t symbolsEnd = header.headerBytes + header.symbols.size() * Symbol::size;
        std::vector<std::byte> bytes(symbolsEnd + sizeof(std::uint64_t), std::byte{0});
        const std::span<std::byte> out(bytes);
        std::memcpy(bytes.data(), logMagic.data(), logMagic.size());
        store32(out, 8, header.formatVersion);
        store32(out, 12, header.minimumReaderVersion);
        store32(out, 16, header.headerBytes);
        store32(out, 20, header.recordBytes);
        store16(out, 24, header.engineMajor);
        store16(out, 26, header.engineMinor);
        store16(out, 28, header.enginePatch);
        store64(out, 32, header.firstPosition);
        store32(out, 40, header.shardCount);
        store32(out, 44, header.shardIndex);
        store32(out, 48, static_cast<std::uint32_t>(header.symbols.size()));
        std::size_t offset = header.headerBytes;
        for (const Symbol& symbol : header.symbols) {
            const std::array<char, Symbol::size> stored = symbol.bytes();
            std::memcpy(bytes.data() + offset, stored.data(), stored.size());
            offset += stored.size();
        }
        store64(out, symbolsEnd, xxh64(out.first(symbolsEnd)));
        return bytes;
    }

    std::uint32_t logChainSeed(std::span<const std::byte> encodedHeader) noexcept {
        return load32(encodedHeader, encodedHeader.size() - sizeof(std::uint64_t));
    }

    std::uint32_t encodeLogRecord(const Symbol& symbol, const Command& command, std::uint32_t previousChecksum,
                                  std::span<std::byte, logRecordBytes> out) noexcept {
        std::memset(out.data(), 0, out.size());
        const std::span<std::byte> record(out);
        const std::array<char, Symbol::size> stored = symbol.bytes();
        std::memcpy(out.data() + symbolOffset, stored.data(), stored.size());
        std::visit(
            [&](const auto& held) {
                using Kind = std::decay_t<decltype(held)>;
                store32(record, orderIdOffset, held.orderId);
                if constexpr (std::is_same_v<Kind, AddCommand>) {
                    assert(!malformedReason(toRequest(held)));
                    store8(record, commandOffset, commandAdd);
                    store8(record, sideOffset, sideByte(held.side));
                    store8(record, orderTypeOffset, orderTypeByte(held.orderType));
                    store8(record, timeInForceOffset, timeInForceByte(held.timeInForce));
                    store32(record, priceOffset, held.price);
                    store32(record, quantityOffset, held.quantity);
                    store32(record, stopPriceOffset, held.stopPrice);
                    encodeInstructions(record, held.postOnly, held.owner, held.selfTradePrevention);
                } else if constexpr (std::is_same_v<Kind, CancelCommand>) {
                    store8(record, commandOffset, commandCancel);
                } else {
                    static_assert(std::is_same_v<Kind, ReplaceCommand>, "every Command alternative needs a record");
                    assert(!malformedReason(toRequest(held)));
                    store8(record, commandOffset, commandReplace);
                    store32(record, priceOffset, held.price);
                    store32(record, quantityOffset, held.quantity);
                    encodeInstructions(record, held.postOnly, held.owner, held.selfTradePrevention);
                }
            },
            command);
        const std::uint32_t checksum = chainChecksum(record, previousChecksum);
        store32(record, 0, checksum);
        return checksum;
    }

    ParseResult decodeLogRecord(std::span<const std::byte> record, std::uint32_t previousChecksum,
                                std::uint64_t position, std::uint64_t filePosition) {
        const auto lineNumber = static_cast<std::size_t>(filePosition + 2);
        if (load32(record, 0) != chainChecksum(record, previousChecksum)) {
            return fileError(lineNumber, ParseErrorCode::CORRUPT_RECORD, "record checksum does not chain from the one before");
        }
        // Bytes this reader does not know: zero means today's behavior, and anything else is a
        // field it would misreplay by skipping.
        const std::uint8_t flags = load8(record, flagsOffset);
        if ((flags & ~flagPostOnly) != 0 || load16(record, reservedOffset) != 0 ||
            std::any_of(record.begin() + logRecordBytes, record.end(), [](std::byte extra) { return extra != std::byte{0}; })) {
            return fileError(lineNumber, ParseErrorCode::UNKNOWN_FIELD, "record uses a field this reader does not know");
        }

        const Fields fields{load8(record, commandOffset),
                            load8(record, sideOffset),
                            load8(record, orderTypeOffset),
                            load8(record, timeInForceOffset),
                            load32(record, orderIdOffset),
                            load32(record, priceOffset),
                            load32(record, quantityOffset),
                            load32(record, stopPriceOffset),
                            load32(record, ownerOffset),
                            (flags & flagPostOnly) != 0,
                            load8(record, selfTradePreventionOffset)};
        const Decoder decoder(fields, lineNumber);
        if (fields.command == 0 || fields.command > commandReplace) {
            return decoder.error(ParseErrorCode::UNKNOWN_COMMAND, commandColumn, fields.command, "not a command");
        }
        if (fields.orderId == 0) {
            return decoder.error(ParseErrorCode::INVALID_VALUE, orderIdColumn, 0, "order_id must not be zero");
        }
        const std::optional<ParseError> problem = fields.command == commandAdd      ? decoder.checkAdd()
                                                  : fields.command == commandCancel ? decoder.checkCancel()
                                                                                    : decoder.checkReplace();
        if (problem) {
            return *problem;
        }
        std::array<char, Symbol::size> stored{};
        std::memcpy(stored.data(), record.data() + symbolOffset, stored.size());
        const std::optional<Symbol> symbol = Symbol::fromBytes(std::span<const char, Symbol::size>(stored));
        if (!symbol) {
            return ParseError{lineNumber, ParseErrorCode::INVALID_SYMBOL, "symbol", std::string(stored.data(), stored.size()),
                              "not a symbol (1 to 8 of A-Z, 0-9, '.', '-')"};
        }
        return ReplayRecord{lineNumber, position + 1, decoder.command(), *symbol};
    }

    std::string formatReplayRow(const ReplayRecord& record) {
        std::string row = std::to_string(record.sequence) + ',' + commandName(record.command) + ',';
        std::visit(
            [&](const auto& held) {
                using Kind = std::decay_t<decltype(held)>;
                row += std::to_string(held.orderId) + ',';
                if constexpr (std::is_same_v<Kind, AddCommand>) {
                    row += std::string(held.side == Side::BUY ? "BUY" : "SELL") + ',' + numberOrEmpty(held.price) + ',' +
                           std::to_string(held.quantity) + ',' + orderTypeName(held.orderType) + ',' +
                           timeInForceName(held.timeInForce) + ',' + numberOrEmpty(held.stopPrice) + ',' +
                           instructionColumns(held.postOnly, held.owner, held.selfTradePrevention);
                } else if constexpr (std::is_same_v<Kind, CancelCommand>) {
                    row += ",,,,,,,,";
                } else {
                    static_assert(std::is_same_v<Kind, ReplaceCommand>, "every Command alternative needs a row");
                    row += ',' + std::to_string(held.price) + ',' + std::to_string(held.quantity) + ",,,," +
                           instructionColumns(held.postOnly, held.owner, held.selfTradePrevention);
                }
            },
            record.command);
        return row + ',' + record.symbol.text();
    }

    CommandLogReader::CommandLogReader(std::istream& source, std::size_t capacity)
        : input(source), chunkCapacity(capacity > 0 ? capacity : 1) {}

    bool CommandLogReader::refill() {
        if (inputExhausted) {
            return false;
        }
        chunk.resize(chunkCapacity);
        input.read(chunk.data(), static_cast<std::streamsize>(chunkCapacity));
        chunkSize = static_cast<std::size_t>(input.gcount());
        chunkPosition = 0;
        if (input.bad()) {
            throw std::runtime_error("reading the command log failed");
        }
        inputExhausted = chunkSize < chunkCapacity;
        return chunkSize > 0;
    }

    std::size_t CommandLogReader::read(std::byte* out, std::size_t wanted) {
        std::size_t copied = 0;
        while (copied < wanted) {
            if (chunkPosition == chunkSize && !refill()) {
                break;
            }
            const std::size_t taken = std::min(wanted - copied, chunkSize - chunkPosition);
            std::memcpy(out + copied, chunk.data() + chunkPosition, taken);
            chunkPosition += taken;
            copied += taken;
        }
        return copied;
    }

    std::optional<ParseError> CommandLogReader::readHeader() {
        const auto corrupt = [](std::string message) {
            return fileError(1, ParseErrorCode::CORRUPT_HEADER, std::move(message));
        };
        std::vector<std::byte> bytes(logHeaderBytes);
        if (read(bytes.data(), bytes.size()) != bytes.size()) {
            return corrupt("the file ends inside the log header");
        }
        const std::span<const std::byte> fixed(bytes);
        if (std::memcmp(bytes.data(), logMagic.data(), logMagic.size()) != 0) {
            return corrupt("not a command log: wrong magic");
        }
        LogHeader header;
        header.formatVersion = load32(fixed, 8);
        header.minimumReaderVersion = load32(fixed, 12);
        if (header.minimumReaderVersion > logFormatVersion) {
            return fileError(1, ParseErrorCode::UNSUPPORTED_VERSION,
                             "this log needs reader version " + std::to_string(header.minimumReaderVersion) +
                                 ", and this reader is version " + std::to_string(logFormatVersion));
        }
        header.headerBytes = load32(fixed, 16);
        header.recordBytes = load32(fixed, 20);
        if (header.headerBytes < logHeaderBytes || header.recordBytes < logRecordBytes) {
            return corrupt("the header states sizes smaller than the format's");
        }
        header.engineMajor = load16(fixed, 24);
        header.engineMinor = load16(fixed, 26);
        header.enginePatch = load16(fixed, 28);
        header.firstPosition = load64(fixed, 32);
        header.shardCount = load32(fixed, 40);
        header.shardIndex = load32(fixed, 44);
        const std::uint64_t symbolCount = load32(fixed, 48);

        // Bounded by the file: a corrupt size must not ask for gigabytes it will never read.
        std::uint64_t remaining = header.headerBytes - logHeaderBytes + symbolCount * Symbol::size + sizeof(std::uint64_t);
        while (remaining > 0) {
            const std::size_t step = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 64 * 1024));
            const std::size_t before = bytes.size();
            bytes.resize(before + step);
            if (read(bytes.data() + before, step) != step) {
                return corrupt("the file ends inside the log header");
            }
            remaining -= step;
        }

        const std::span<const std::byte> whole(bytes);
        const std::size_t checksumAt = bytes.size() - sizeof(std::uint64_t);
        if (load64(whole, checksumAt) != xxh64(whole.first(checksumAt))) {
            return corrupt("the header's checksum does not match");
        }
        if (load16(whole, 30) != 0 || load32(whole, 52) != 0 || load64(whole, 56) != 0) {
            return fileError(1, ParseErrorCode::UNKNOWN_FIELD, "the header uses a field this reader does not know");
        }
        const auto extra = whole.subspan(logHeaderBytes, header.headerBytes - logHeaderBytes);
        if (std::any_of(extra.begin(), extra.end(), [](std::byte value) { return value != std::byte{0}; })) {
            return fileError(1, ParseErrorCode::UNKNOWN_FIELD, "the header uses a field this reader does not know");
        }
        for (std::uint64_t i = 0; i < symbolCount; ++i) {
            std::array<char, Symbol::size> stored{};
            std::memcpy(stored.data(), bytes.data() + header.headerBytes + i * Symbol::size, stored.size());
            const std::optional<Symbol> symbol = Symbol::fromBytes(std::span<const char, Symbol::size>(stored));
            if (!symbol) {
                return ParseError{1, ParseErrorCode::INVALID_SYMBOL, "symbol", std::string(stored.data(), stored.size()),
                                  "the header names something that is not a symbol"};
            }
            header.symbols.push_back(*symbol);
        }
        header.chainSeed = logChainSeed(whole);
        parsedHeader = std::move(header);
        previousChecksum = parsedHeader.chainSeed;
        record.resize(parsedHeader.recordBytes);
        headerRead = true;
        return std::nullopt;
    }

    ParseError CommandLogReader::classifyFailure(std::size_t lineNumber) {
        std::uint32_t before = load32(record, 0);
        std::vector<std::byte> later(record.size());
        std::uint64_t position = parsedHeader.firstPosition + recordsRead;
        while (read(later.data(), later.size()) == later.size()) {
            ++position;
            if (std::holds_alternative<ReplayRecord>(decodeLogRecord(later, before, position, recordsRead))) {
                return fileError(lineNumber, ParseErrorCode::CORRUPT_RECORD,
                                 "a record is damaged, and valid records follow it");
            }
            before = load32(later, 0);
        }
        return fileError(lineNumber, ParseErrorCode::TORN_TAIL, "the log ends in a torn tail: nothing valid follows");
    }

    ReadResult CommandLogReader::next() {
        if (finalResult) {
            return *finalResult;
        }
        if (!headerRead) {
            if (std::optional<ParseError> problem = readHeader()) {
                finalResult = ReadResult{std::move(*problem)};
                return *finalResult;
            }
        }

        const std::size_t got = read(record.data(), record.size());
        if (got == 0) {
            finalResult = ReadResult{EndOfInput{}};
            return *finalResult;
        }
        const auto lineNumber = static_cast<std::size_t>(recordsRead + 2);
        if (got < record.size()) {
            finalResult = ReadResult{fileError(lineNumber, ParseErrorCode::TORN_TAIL, "the log ends inside a record")};
            return *finalResult;
        }

        ParseResult result = decodeLogRecord(record, previousChecksum, parsedHeader.firstPosition + recordsRead, recordsRead);
        if (ReplayRecord* decoded = std::get_if<ReplayRecord>(&result)) {
            previousChecksum = load32(record, 0);
            ++recordsRead;
            return std::move(*decoded);
        }
        ParseError& problem = std::get<ParseError>(result);
        finalResult = ReadResult{problem.code == ParseErrorCode::CORRUPT_RECORD ? classifyFailure(lineNumber) : std::move(problem)};
        return *finalResult;
    }

    bool isCommandLogFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::array<char, logMagic.size()> first{};
        file.read(first.data(), static_cast<std::streamsize>(first.size()));
        return file.gcount() == static_cast<std::streamsize>(first.size()) && first == logMagic;
    }

    ReplayInput readCommandLogFile(const std::filesystem::path& path) {
        ReplayInput replayInput;
        replayInput.format = ReplayFormat::V3;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt, "cannot open command log", std::nullopt};
            return replayInput;
        }
        CommandLogReader reader(file);
        try {
            while (true) {
                ReadResult result = reader.next();
                if (ReplayRecord* decoded = std::get_if<ReplayRecord>(&result)) {
                    replayInput.records.push_back(std::move(*decoded));
                } else if (ParseError* problem = std::get_if<ParseError>(&result)) {
                    const std::size_t lineNumber = problem->lineNumber;
                    std::string message = problem->message;
                    replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, lineNumber, std::move(message),
                                                    std::move(*problem)};
                    return replayInput;
                } else {
                    return replayInput;
                }
            }
        } catch (const std::runtime_error& failure) {
            replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt, failure.what(), std::nullopt};
            return replayInput;
        }
    }

    ReplayInput readReplayOrLogFile(const std::filesystem::path& path) {
        return isCommandLogFile(path) ? readCommandLogFile(path) : readReplayFile(path);
    }

}  // namespace order_book::replay
