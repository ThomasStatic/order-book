#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/rejection_reason.hpp"
#include "order_book/symbol.hpp"
#include "order_book/types.hpp"

// The wire protocol's frames, specified byte for byte in docs/wire-protocol.md: pure, with no
// sockets and no threads. Not public API: the contract to the outside is that document.
namespace order_book::network {

    using replay::Symbol;

    // Picked by the client, echoed on every response, never read or checked for uniqueness.
    using Correlation = std::uint64_t;

    inline constexpr std::uint16_t wireVersion = 1;

    inline constexpr std::size_t frameHeaderBytes = 12;
    inline constexpr std::size_t helloFrameBytes = 24;
    inline constexpr std::size_t commandFrameBytes = 48;
    inline constexpr std::size_t refusedFrameBytes = 28;
    inline constexpr std::size_t protocolErrorFrameBytes = 16;
    inline constexpr std::size_t maxEventFrameBytes = 40;

    // Values are the format's own. Open: 6 to 15 are free for frames that are not events, and
    // events go on past 25. Each event's value is its place in BookEvent, plus 16.
    enum class FrameType : std::uint8_t {
        HELLO = 1,
        WELCOME = 2,
        COMMAND = 3,
        REFUSED = 4,
        PROTOCOL_ERROR = 5,
        ORDER_ACCEPTED = 16,
        ORDER_REJECTED = 17,
        TRADE_EXECUTED = 18,
        ORDER_CANCELLED = 19,
        CANCEL_REJECTED = 20,
        ORDER_EXPIRED = 21,
        SELF_TRADE_PREVENTED = 22,
        STOP_TRIGGERED = 23,
        ORDER_REPLACED = 24,
        REPLACE_REJECTED = 25
    };

    // Append-only, and never the C++ enumerators.
    enum class ProtocolError : std::uint16_t {
        BAD_LENGTH = 1,
        BAD_MAGIC = 2,
        UNKNOWN_FRAME_TYPE = 3,
        HELLO_REQUIRED = 4,
        DUPLICATE_HELLO = 5,
        UNSUPPORTED_VERSION = 6,
        UNKNOWN_COMMAND = 7,
        UNEXPECTED_FIELD = 8,  // a non-zero unused or reserved byte
        TOO_MANY_CONNECTIONS = 9
    };

    // Append-only.
    enum class RefusedCode : std::uint16_t {
        UNKNOWN_INSTRUMENT = 1,
        ENGINE_STOPPING = 2  // this command would have been queued: retry
    };

    // A reason's value on the wire, from the protocol document's append-only table. 0 for NONE,
    // which no event carries. A reason this release doesn't know has no value going in, and
    // nothing coming back, so a client can skip it.
    std::uint16_t reasonCode(RejectionReason reason) noexcept;
    std::optional<RejectionReason> reasonFromCode(std::uint16_t code) noexcept;

    // Whose frames a reader reads. The engine accepts only the exact length for a type, in a
    // fixed order. A client skips a type it doesn't know and trailing bytes, by the length.
    enum class FrameSource { CLIENT, ENGINE };

    struct NeedMore {
        bool operator==(const NeedMore&) const = default;
    };

    // Its body is a view into the reader, good until the next call to feed or next.
    struct Frame {
        FrameType type;
        bool last;  // the last frame of a response
        Correlation correlation;
        std::span<const std::byte> body;
    };

    struct FrameError {
        ProtocolError code;
        Correlation correlation;  // the offending frame's, or 0 if it is shorter than a header

        bool operator==(const FrameError&) const = default;
    };

    using FrameResult = std::variant<Frame, NeedMore, FrameError>;

    // Takes bytes in any chunking and gives one frame at a time, the same whatever the chunks.
    // A frame's shape is judged here: a length under 12 once those 2 bytes are in, the rest of
    // its header once that is, and its body once that is. After an error, every later call
    // returns it again. It keeps the bytes it was
    // fed, so feed may grow its buffer, and nothing allocates per frame once that has settled.
    class FrameReader {
    public:
        // Reads the frames source sends.
        explicit FrameReader(FrameSource source);

        void feed(std::span<const std::byte> bytes);
        FrameResult next();

    private:
        // For a frame at least as long as a header.
        std::optional<ProtocolError> checkHeader(std::uint8_t type, std::uint8_t flags, std::size_t length) const;
        std::optional<ProtocolError> checkBody(FrameType type, std::span<const std::byte> body);

        FrameSource source;
        std::vector<std::byte> buffer;
        std::size_t position = 0;
        bool helloSeen = false;
        std::optional<FrameError> failure;
    };

    struct CommandFrame {
        Correlation correlation;
        replay::RawCommand command;
    };

    struct RefusedFrame {
        std::array<char, Symbol::size> symbol;  // as sent, which may not be a symbol
        OrderId orderId;
        std::uint16_t code;

        bool operator==(const RefusedFrame&) const = default;
    };

    struct ProtocolErrorFrame {
        std::uint16_t code;
        std::uint16_t engineVersion;

        bool operator==(const ProtocolErrorFrame&) const = default;
    };

    struct EventFrame {
        Symbol symbol;
        BookEvent event;

        bool operator==(const EventFrame&) const = default;
    };

    // Each reads a frame of its own type, which FrameReader has already judged.
    CommandFrame decodeCommandFrame(const Frame& frame) noexcept;
    std::uint16_t decodeVersion(const Frame& frame) noexcept;  // a Hello or a Welcome
    RefusedFrame decodeRefused(const Frame& frame) noexcept;
    ProtocolErrorFrame decodeProtocolError(const Frame& frame) noexcept;
    // Nothing if this is not an event frame, or holds what this release can't read: a symbol or
    // side that isn't one, or a reason it doesn't know.
    std::optional<EventFrame> decodeEvent(const Frame& frame) noexcept;

    // Each writes one frame at the start of out and returns its size, or 0 if it does not fit.
    // None allocates or throws.
    std::size_t encodeHello(Correlation correlation, std::uint16_t version, std::span<std::byte> out) noexcept;
    std::size_t encodeWelcome(Correlation correlation, std::uint16_t version, std::span<std::byte> out) noexcept;
    std::size_t encodeCommand(Correlation correlation, const Symbol& symbol, const replay::Command& command,
                              std::span<std::byte> out) noexcept;
    std::size_t encodeRefused(Correlation correlation, const std::array<char, Symbol::size>& symbol, OrderId orderId,
                              RefusedCode code, std::span<std::byte> out) noexcept;
    std::size_t encodeProtocolError(Correlation correlation, ProtocolError code, std::span<std::byte> out) noexcept;
    std::size_t encodeEvent(Correlation correlation, const Symbol& symbol, const BookEvent& event, bool last,
                            std::span<std::byte> out) noexcept;

}  // namespace order_book::network
