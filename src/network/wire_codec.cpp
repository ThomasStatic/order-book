#include "order_book/network/wire_codec.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#include "order_book/little_endian.hpp"

namespace order_book::network {
    using namespace little_endian;

    namespace {
        constexpr std::array<char, 8> wireMagic{'O', 'B', 'W', 'I', 'R', 'E', '\r', '\n'};

        constexpr std::uint8_t flagLast = 1;

        // Header fields.
        constexpr std::size_t lengthOffset = 0;
        constexpr std::size_t typeOffset = 2;
        constexpr std::size_t flagsOffset = 3;
        constexpr std::size_t correlationOffset = 4;

        // Body fields, from the end of the header.
        constexpr std::size_t versionOffset = 8;      // a Hello's or a Welcome's, after the magic
        constexpr std::size_t helloReservedOffset = 10;
        constexpr std::size_t afterSymbolOffset = Symbol::size;  // where a Refused or an event goes on

        constexpr std::uint8_t sideBuy = 1;
        constexpr std::uint8_t sideSell = 2;

        std::uint8_t sideByte(Side side) noexcept {
            return side == Side::BUY ? sideBuy : sideSell;
        }

        std::optional<Side> sideFromByte(std::uint8_t byte) noexcept {
            if (byte == sideBuy) {
                return Side::BUY;
            }
            if (byte == sideSell) {
                return Side::SELL;
            }
            return std::nullopt;
        }

        // The whole frame, with its header written and its body zeroed.
        std::span<std::byte> startFrame(std::span<std::byte> out, std::size_t size, FrameType type, bool last,
                                        Correlation correlation) noexcept {
            const std::span<std::byte> frame = out.first(size);
            std::fill(frame.begin(), frame.end(), std::byte{0});
            store16(frame, lengthOffset, static_cast<std::uint16_t>(size));
            store8(frame, typeOffset, static_cast<std::uint8_t>(type));
            store8(frame, flagsOffset, last ? flagLast : 0);
            store64(frame, correlationOffset, correlation);
            return frame.subspan(frameHeaderBytes);
        }

        std::size_t encodeGreeting(FrameType type, bool last, Correlation correlation, std::uint16_t version,
                                   std::span<std::byte> out) noexcept {
            if (out.size() < helloFrameBytes) {
                return 0;
            }
            const std::span<std::byte> body = startFrame(out, helloFrameBytes, type, last, correlation);
            std::memcpy(body.data(), wireMagic.data(), wireMagic.size());
            store16(body, versionOffset, version);
            return helloFrameBytes;
        }

        void storeSymbol(std::span<std::byte> body, const std::array<char, Symbol::size>& symbol) noexcept {
            std::memcpy(body.data(), symbol.data(), symbol.size());
        }

        std::array<char, Symbol::size> loadSymbol(std::span<const std::byte> body) noexcept {
            std::array<char, Symbol::size> symbol{};
            std::memcpy(symbol.data(), body.data(), symbol.size());
            return symbol;
        }

        // What every event frame after its symbol is made of: 4-byte fields, in order.
        void storeWords(std::span<std::byte> fields, std::initializer_list<std::uint32_t> words) noexcept {
            std::size_t offset = 0;
            for (const std::uint32_t word : words) {
                store32(fields, offset, word);
                offset += sizeof(word);
            }
        }

        struct EventShape {
            FrameType type;
            std::size_t size;
        };

        constexpr EventShape shapeOf(const OrderAccepted&) noexcept { return {FrameType::ORDER_ACCEPTED, 36}; }
        constexpr EventShape shapeOf(const OrderRejected&) noexcept { return {FrameType::ORDER_REJECTED, 28}; }
        constexpr EventShape shapeOf(const TradeExecuted&) noexcept { return {FrameType::TRADE_EXECUTED, 36}; }
        constexpr EventShape shapeOf(const OrderCancelled&) noexcept { return {FrameType::ORDER_CANCELLED, 28}; }
        constexpr EventShape shapeOf(const CancelRejected&) noexcept { return {FrameType::CANCEL_REJECTED, 28}; }
        constexpr EventShape shapeOf(const OrderExpired&) noexcept { return {FrameType::ORDER_EXPIRED, 28}; }
        constexpr EventShape shapeOf(const SelfTradePrevented&) noexcept { return {FrameType::SELF_TRADE_PREVENTED, 36}; }
        constexpr EventShape shapeOf(const StopTriggered&) noexcept { return {FrameType::STOP_TRIGGERED, 32}; }
        constexpr EventShape shapeOf(const OrderReplaced&) noexcept { return {FrameType::ORDER_REPLACED, 40}; }
        constexpr EventShape shapeOf(const ReplaceRejected&) noexcept { return {FrameType::REPLACE_REJECTED, 28}; }

        // The size a client needs of a frame it knows, or nothing for one it doesn't.
        std::optional<std::size_t> sizeOf(std::uint8_t type) noexcept {
            switch (static_cast<FrameType>(type)) {
                case FrameType::HELLO:
                case FrameType::WELCOME: return helloFrameBytes;
                case FrameType::COMMAND: return commandFrameBytes;
                case FrameType::REFUSED: return refusedFrameBytes;
                case FrameType::PROTOCOL_ERROR: return protocolErrorFrameBytes;
                case FrameType::ORDER_ACCEPTED: return shapeOf(OrderAccepted{}).size;
                case FrameType::ORDER_REJECTED: return shapeOf(OrderRejected{}).size;
                case FrameType::TRADE_EXECUTED: return shapeOf(TradeExecuted{}).size;
                case FrameType::ORDER_CANCELLED: return shapeOf(OrderCancelled{}).size;
                case FrameType::CANCEL_REJECTED: return shapeOf(CancelRejected{}).size;
                case FrameType::ORDER_EXPIRED: return shapeOf(OrderExpired{}).size;
                case FrameType::SELF_TRADE_PREVENTED: return shapeOf(SelfTradePrevented{}).size;
                case FrameType::STOP_TRIGGERED: return shapeOf(StopTriggered{}).size;
                case FrameType::ORDER_REPLACED: return shapeOf(OrderReplaced{}).size;
                case FrameType::REPLACE_REJECTED: return shapeOf(ReplaceRejected{}).size;
            }
            return std::nullopt;
        }

        bool isZero(std::uint32_t first, std::uint32_t second = 0) noexcept {
            return first == 0 && second == 0;
        }

        // Which of a command's fields it leaves unused, which must be zero.
        bool hasUnexpectedField(const replay::RawCommand& raw) noexcept {
            if ((raw.flags & ~replay::rawFlagPostOnly) != 0 || raw.reserved != 0) {
                return true;
            }
            if (raw.command == replay::rawCommandAdd) {
                return false;
            }
            if (raw.side != 0 || raw.orderType != 0 || raw.timeInForce != 0 || raw.stopPrice != 0) {
                return true;
            }
            return raw.command == replay::rawCommandCancel &&
                   !(isZero(raw.price, raw.quantity) && isZero(raw.owner, raw.flags) && raw.selfTradePrevention == 0);
        }

        replay::RawCommand rawCommandOf(std::span<const std::byte> body) noexcept {
            return replay::unpackCommandBody(body.first<replay::commandBodyBytes>());
        }

        bool hasMagic(std::span<const std::byte> body) noexcept {
            return std::memcmp(body.data(), wireMagic.data(), wireMagic.size()) == 0;
        }
    }  // namespace

    std::uint16_t reasonCode(RejectionReason reason) noexcept {
        switch (reason) {
            case RejectionReason::NONE: return 0;
            case RejectionReason::DUPLICATE_ORDER_ID: return 1;
            case RejectionReason::ORDER_NOT_FOUND: return 2;
            case RejectionReason::INVALID_ORDER_ID: return 3;
            case RejectionReason::INVALID_PRICE: return 4;
            case RejectionReason::INVALID_QUANTITY: return 5;
            case RejectionReason::INVALID_SIDE: return 6;
            case RejectionReason::UNSUPPORTED_ORDER_TYPE: return 7;
            case RejectionReason::ORDER_NOT_ACTIVE: return 8;
            case RejectionReason::WOULD_CROSS_BOOK: return 9;
            case RejectionReason::PRICE_NOT_ALLOWED: return 10;
            case RejectionReason::INVALID_TIME_IN_FORCE: return 11;
            case RejectionReason::TIME_IN_FORCE_NOT_ALLOWED: return 12;
            case RejectionReason::INVALID_STOP_PRICE: return 13;
            case RejectionReason::STOP_PRICE_NOT_ALLOWED: return 14;
            case RejectionReason::POST_ONLY_NOT_ALLOWED: return 15;
            case RejectionReason::INVALID_SELF_TRADE_PREVENTION: return 16;
            case RejectionReason::REPLACE_QUANTITY_TOO_LOW: return 17;
            case RejectionReason::ORDER_NOT_REPLACEABLE: return 18;
        }
        return 0;
    }

    std::optional<RejectionReason> reasonFromCode(std::uint16_t code) noexcept {
        constexpr std::array reasons{
            RejectionReason::DUPLICATE_ORDER_ID,
            RejectionReason::ORDER_NOT_FOUND,
            RejectionReason::INVALID_ORDER_ID,
            RejectionReason::INVALID_PRICE,
            RejectionReason::INVALID_QUANTITY,
            RejectionReason::INVALID_SIDE,
            RejectionReason::UNSUPPORTED_ORDER_TYPE,
            RejectionReason::ORDER_NOT_ACTIVE,
            RejectionReason::WOULD_CROSS_BOOK,
            RejectionReason::PRICE_NOT_ALLOWED,
            RejectionReason::INVALID_TIME_IN_FORCE,
            RejectionReason::TIME_IN_FORCE_NOT_ALLOWED,
            RejectionReason::INVALID_STOP_PRICE,
            RejectionReason::STOP_PRICE_NOT_ALLOWED,
            RejectionReason::POST_ONLY_NOT_ALLOWED,
            RejectionReason::INVALID_SELF_TRADE_PREVENTION,
            RejectionReason::REPLACE_QUANTITY_TOO_LOW,
            RejectionReason::ORDER_NOT_REPLACEABLE,
        };
        if (code == 0 || code > reasons.size()) {
            return std::nullopt;
        }
        return reasons[code - 1U];
    }

    FrameReader::FrameReader(FrameSource from) : source(from) {
        buffer.reserve(4096);
    }

    void FrameReader::feed(std::span<const std::byte> bytes) {
        // What next has taken goes first, so the buffer stays as small as what is unread.
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(position));
        position = 0;
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    }

    std::optional<ProtocolError> FrameReader::checkHeader(std::uint8_t type, std::uint8_t flags, std::size_t length) const {
        if (length < frameHeaderBytes) {
            return ProtocolError::BAD_LENGTH;
        }
        const std::optional<std::size_t> known = sizeOf(type);
        if (source == FrameSource::ENGINE) {
            if ((flags & ~flagLast) != 0) {
                return ProtocolError::UNEXPECTED_FIELD;
            }
            return known && length < *known ? std::optional(ProtocolError::BAD_LENGTH) : std::nullopt;
        }
        const auto frameType = static_cast<FrameType>(type);
        if (frameType == FrameType::HELLO) {
            if (helloSeen) {
                return ProtocolError::DUPLICATE_HELLO;
            }
        } else if (!helloSeen) {
            return ProtocolError::HELLO_REQUIRED;
        } else if (frameType != FrameType::COMMAND) {
            return ProtocolError::UNKNOWN_FRAME_TYPE;
        }
        if (flags != 0) {
            return ProtocolError::UNEXPECTED_FIELD;
        }
        return length == *known ? std::nullopt : std::optional(ProtocolError::BAD_LENGTH);
    }

    std::optional<ProtocolError> FrameReader::checkBody(FrameType type, std::span<const std::byte> body) {
        if (type == FrameType::HELLO || type == FrameType::WELCOME) {
            if (!hasMagic(body)) {
                return ProtocolError::BAD_MAGIC;
            }
            const std::uint16_t version = load16(body, versionOffset);
            if (version == 0 || version > wireVersion) {
                return ProtocolError::UNSUPPORTED_VERSION;
            }
            if (load16(body, helloReservedOffset) != 0) {
                return ProtocolError::UNEXPECTED_FIELD;
            }
            helloSeen = true;
        } else if (type == FrameType::COMMAND && source == FrameSource::CLIENT) {
            const replay::RawCommand raw = rawCommandOf(body);
            if (raw.command == 0 || raw.command > replay::rawCommandReplace) {
                return ProtocolError::UNKNOWN_COMMAND;
            }
            if (hasUnexpectedField(raw)) {
                return ProtocolError::UNEXPECTED_FIELD;
            }
        }
        return std::nullopt;
    }

    FrameResult FrameReader::next() {
        if (failure) {
            return *failure;
        }
        while (true) {
            const std::span<const std::byte> unread = std::span<const std::byte>(buffer).subspan(position);
            if (unread.size() < frameHeaderBytes) {
                return NeedMore{};
            }
            const std::size_t length = load16(unread, lengthOffset);
            const std::uint8_t type = load8(unread, typeOffset);
            const std::uint8_t flags = load8(unread, flagsOffset);
            const Correlation correlation = load64(unread, correlationOffset);
            if (const std::optional<ProtocolError> problem = checkHeader(type, flags, length)) {
                failure = FrameError{*problem, correlation};
                return *failure;
            }
            if (unread.size() < length) {
                return NeedMore{};
            }
            if (!sizeOf(type)) {
                position += length;  // a client skips what it doesn't know
                continue;
            }
            const auto frameType = static_cast<FrameType>(type);
            const std::span<const std::byte> body = unread.subspan(frameHeaderBytes, length - frameHeaderBytes);
            if (const std::optional<ProtocolError> problem = checkBody(frameType, body)) {
                failure = FrameError{*problem, correlation};
                return *failure;
            }
            position += length;
            return Frame{frameType, (flags & flagLast) != 0, correlation, body};
        }
    }

    CommandFrame decodeCommandFrame(const Frame& frame) noexcept {
        return CommandFrame{frame.correlation, rawCommandOf(frame.body)};
    }

    std::uint16_t decodeVersion(const Frame& frame) noexcept {
        return load16(frame.body, versionOffset);
    }

    RefusedFrame decodeRefused(const Frame& frame) noexcept {
        return RefusedFrame{loadSymbol(frame.body), load32(frame.body, afterSymbolOffset),
                            load16(frame.body, afterSymbolOffset + 4)};
    }

    ProtocolErrorFrame decodeProtocolError(const Frame& frame) noexcept {
        return ProtocolErrorFrame{load16(frame.body, 0), load16(frame.body, 2)};
    }

    std::optional<EventFrame> decodeEvent(const Frame& frame) noexcept {
        if (frame.type < FrameType::ORDER_ACCEPTED) {
            return std::nullopt;  // other frames' bodies may be too short for a symbol
        }
        const std::array<char, Symbol::size> stored = loadSymbol(frame.body);
        const std::optional<Symbol> symbol = Symbol::fromBytes(std::span<const char, Symbol::size>(stored));
        if (!symbol) {
            return std::nullopt;
        }
        const std::span<const std::byte> fields = frame.body.subspan(afterSymbolOffset);
        const std::uint32_t first = load32(fields, 0);
        const std::uint32_t second = load32(fields, 4);
        const auto thirdField = [&fields] { return load32(fields, 8); };
        const auto fourthField = [&fields] { return load32(fields, 12); };

        const auto rejection = [&](auto make) -> std::optional<EventFrame> {
            const std::optional<RejectionReason> reason = reasonFromCode(load16(fields, 4));
            if (!reason) {
                return std::nullopt;
            }
            return EventFrame{*symbol, make(*reason)};
        };

        switch (frame.type) {
            case FrameType::ORDER_ACCEPTED: {
                const std::optional<Side> side = sideFromByte(load8(fields, 12));
                if (!side) {
                    return std::nullopt;
                }
                return EventFrame{*symbol, OrderAccepted{first, *side, second, thirdField()}};
            }
            case FrameType::ORDER_REJECTED:
                return rejection([first](RejectionReason reason) { return BookEvent{OrderRejected{first, reason}}; });
            case FrameType::CANCEL_REJECTED:
                return rejection([first](RejectionReason reason) { return BookEvent{CancelRejected{first, reason}}; });
            case FrameType::REPLACE_REJECTED:
                return rejection([first](RejectionReason reason) { return BookEvent{ReplaceRejected{first, reason}}; });
            case FrameType::TRADE_EXECUTED:
                return EventFrame{*symbol, TradeExecuted{first, second, thirdField(), fourthField()}};
            case FrameType::SELF_TRADE_PREVENTED:
                return EventFrame{*symbol, SelfTradePrevented{first, second, thirdField(), fourthField()}};
            case FrameType::ORDER_CANCELLED:
                return EventFrame{*symbol, OrderCancelled{first, second}};
            case FrameType::ORDER_EXPIRED:
                return EventFrame{*symbol, OrderExpired{first, second}};
            case FrameType::STOP_TRIGGERED:
                return EventFrame{*symbol, StopTriggered{first, second, thirdField()}};
            case FrameType::ORDER_REPLACED: {
                const std::optional<Side> side = sideFromByte(load8(fields, 16));
                if (!side) {
                    return std::nullopt;
                }
                return EventFrame{*symbol, OrderReplaced{first, *side, second, thirdField(), fourthField(), load8(fields, 17) != 0}};
            }
            default:
                return std::nullopt;
        }
    }

    std::size_t encodeHello(Correlation correlation, std::uint16_t version, std::span<std::byte> out) noexcept {
        return encodeGreeting(FrameType::HELLO, false, correlation, version, out);
    }

    std::size_t encodeWelcome(Correlation correlation, std::uint16_t version, std::span<std::byte> out) noexcept {
        return encodeGreeting(FrameType::WELCOME, true, correlation, version, out);
    }

    std::size_t encodeCommand(Correlation correlation, const Symbol& symbol, const replay::Command& command,
                              std::span<std::byte> out) noexcept {
        if (out.size() < commandFrameBytes) {
            return 0;
        }
        const std::span<std::byte> body = startFrame(out, commandFrameBytes, FrameType::COMMAND, false, correlation);
        replay::packCommandBody(symbol, command, body.first<replay::commandBodyBytes>());
        return commandFrameBytes;
    }

    std::size_t encodeRefused(Correlation correlation, const std::array<char, Symbol::size>& symbol, OrderId orderId,
                              RefusedCode code, std::span<std::byte> out) noexcept {
        if (out.size() < refusedFrameBytes) {
            return 0;
        }
        const std::span<std::byte> body = startFrame(out, refusedFrameBytes, FrameType::REFUSED, true, correlation);
        storeSymbol(body, symbol);
        store32(body, afterSymbolOffset, orderId);
        store16(body, afterSymbolOffset + 4, static_cast<std::uint16_t>(code));
        return refusedFrameBytes;
    }

    std::size_t encodeProtocolError(Correlation correlation, ProtocolError code, std::span<std::byte> out) noexcept {
        if (out.size() < protocolErrorFrameBytes) {
            return 0;
        }
        const std::span<std::byte> body = startFrame(out, protocolErrorFrameBytes, FrameType::PROTOCOL_ERROR, true, correlation);
        store16(body, 0, static_cast<std::uint16_t>(code));
        store16(body, 2, wireVersion);
        return protocolErrorFrameBytes;
    }

    std::size_t encodeEvent(Correlation correlation, const Symbol& symbol, const BookEvent& event, bool last,
                            std::span<std::byte> out) noexcept {
        return std::visit(
            [&](const auto& held) noexcept -> std::size_t {
                using Kind = std::decay_t<decltype(held)>;
                const EventShape shape = shapeOf(held);
                if (out.size() < shape.size) {
                    return 0;
                }
                const std::span<std::byte> body = startFrame(out, shape.size, shape.type, last, correlation);
                storeSymbol(body, symbol.bytes());
                const std::span<std::byte> fields = body.subspan(afterSymbolOffset);
                if constexpr (std::is_same_v<Kind, OrderAccepted>) {
                    storeWords(fields, {held.orderId, held.price, held.quantity});
                    store8(fields, 12, sideByte(held.side));
                } else if constexpr (std::is_same_v<Kind, TradeExecuted>) {
                    storeWords(fields, {held.incomingOrderId, held.restingOrderId, held.executionPrice, held.executionQuantity});
                } else if constexpr (std::is_same_v<Kind, SelfTradePrevented>) {
                    storeWords(fields, {held.incomingOrderId, held.restingOrderId, held.price, held.cancelledQuantity});
                } else if constexpr (std::is_same_v<Kind, OrderCancelled>) {
                    storeWords(fields, {held.orderId, held.cancelledQuantity});
                } else if constexpr (std::is_same_v<Kind, OrderExpired>) {
                    storeWords(fields, {held.orderId, held.expiredQuantity});
                } else if constexpr (std::is_same_v<Kind, StopTriggered>) {
                    storeWords(fields, {held.orderId, held.stopPrice, held.tradePrice});
                } else if constexpr (std::is_same_v<Kind, OrderReplaced>) {
                    storeWords(fields, {held.orderId, held.price, held.quantity, held.openQuantity});
                    store8(fields, 16, sideByte(held.side));
                    store8(fields, 17, held.priorityKept ? 1 : 0);
                } else {
                    static_assert(std::is_same_v<Kind, OrderRejected> || std::is_same_v<Kind, CancelRejected> ||
                                      std::is_same_v<Kind, ReplaceRejected>,
                                  "every BookEvent alternative needs a frame");
                    store32(fields, 0, held.orderId);
                    store16(fields, 4, reasonCode(held.reason));
                }
                return shape.size;
            },
            event);
    }

}  // namespace order_book::network
