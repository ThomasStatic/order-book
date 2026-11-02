#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/little_endian.hpp"
#include "order_book/network/wire_codec.hpp"

// Frames a test client sends, built here byte by byte rather than by the encoder under test, so
// any command, however malformed, can be sent. Shared by gateway_tests and gateway_socket_tests.
namespace order_book::gateway_testing {

    using Bytes = std::vector<std::byte>;

    inline Bytes helloFrame(network::Correlation correlation = 1) {
        Bytes frame(64);
        frame.resize(network::encodeHello(correlation, network::wireVersion, frame));
        return frame;
    }

    // A Command frame whose body is the command log's field layout, as the wire protocol says.
    inline Bytes rawCommandFrame(network::Correlation correlation, const replay::RawCommand& raw) {
        using namespace order_book::little_endian;
        Bytes frame(12 + replay::commandBodyBytes);
        const std::span<std::byte> out(frame);
        store16(out, 0, static_cast<std::uint16_t>(frame.size()));
        store8(out, 2, static_cast<std::uint8_t>(network::FrameType::COMMAND));
        store64(out, 4, correlation);
        const std::span<std::byte> body = out.subspan(12);
        store8(body, 0, raw.command);
        store8(body, 1, raw.side);
        store8(body, 2, raw.orderType);
        store8(body, 3, raw.timeInForce);
        std::memcpy(body.data() + 4, raw.symbol.data(), raw.symbol.size());
        store32(body, 12, raw.orderId);
        store32(body, 16, raw.price);
        store32(body, 20, raw.quantity);
        store32(body, 24, raw.stopPrice);
        store32(body, 28, raw.owner);
        store8(body, 32, raw.flags);
        store8(body, 33, raw.selfTradePrevention);
        store16(body, 34, raw.reserved);
        return frame;
    }

    // Whether rawCommandFrame wrote what the log's own reader takes back, so its layout is right.
    inline bool roundTrips(const Bytes& frame, const replay::RawCommand& raw) {
        const std::span<const std::byte> body = std::span<const std::byte>(frame).subspan(12);
        return replay::unpackCommandBody(std::span<const std::byte, replay::commandBodyBytes>(body.first<replay::commandBodyBytes>())) == raw;
    }

}  // namespace order_book::gateway_testing
