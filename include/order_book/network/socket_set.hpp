#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace order_book::network {

    using SocketId = std::uint32_t;

    enum class ReadStatus {
        OPEN,
        FAILED,  // the connection is gone, and nothing more will come
    };

    struct ReadResult {
        std::size_t bytes;
        ReadStatus status;
    };

    // What a gateway thread does to its sockets. Virtual, since each call wraps a system call of
    // 2 µs or more and no per-frame work crosses it. It belongs to one gateway thread, and never
    // blocks except in wait.
    class SocketSet {
    public:
        virtual ~SocketSet() = default;

        // Sleeps until a socket may have something to do.
        virtual void wait() = 0;
        // A new connection, if one is waiting.
        virtual std::optional<SocketId> accept() = 0;
        // Takes whatever has arrived, up to into's size.
        virtual ReadResult read(SocketId socket, std::span<std::byte> into) = 0;
        // Returns how much of bytes the socket took, which may be none of it.
        virtual std::size_t write(SocketId socket, std::span<const std::byte> bytes) = 0;
        virtual void close(SocketId socket) = 0;
    };

}  // namespace order_book::network
