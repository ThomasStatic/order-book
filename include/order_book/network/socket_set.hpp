#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace order_book::network {

    using SocketId = std::uint32_t;
    // A connection on its way from one gateway thread's set to another's: what release gives up
    // and adopt takes on. Trivially copyable, so it can cross threads on a ring.
    using HandedSocket = std::uint64_t;

    enum class ReadStatus {
        OPEN,
        ENDED,   // the client half-closed, after everything it sent: it still hears what it's owed
        FAILED,  // the connection is gone, and nothing more will come
    };

    struct ReadResult {
        std::size_t bytes;
        ReadStatus status;
    };

    // What a wait should watch a connection for.
    struct SocketInterest {
        SocketId socket;
        bool read;
        bool write;
    };

    enum class WaitMode {
        LOOK,   // see what is ready, without waiting
        SLEEP,  // wait until something is, or the set is woken
    };

    // What a gateway thread does to its sockets. Virtual, since each call wraps a system call of
    // 2 µs or more and no per-frame work crosses it. It belongs to one gateway thread, except
    // wake, and never blocks except in wait.
    class SocketSet {
    public:
        virtual ~SocketSet() = default;

        // Finds what is ready among these, the listener, and the set's own wake, so that a read or
        // accept afterwards costs a system call only where one can succeed. With SLEEP, waits
        // until there is something.
        virtual void wait(std::span<const SocketInterest> interest, WaitMode mode) = 0;
        // A new connection, if one is waiting.
        virtual std::optional<SocketId> accept() = 0;
        // Takes whatever has arrived, up to into's size.
        virtual ReadResult read(SocketId socket, std::span<std::byte> into) = 0;
        // Returns how much of bytes the socket took, which may be none of it, or nothing if the
        // connection is gone.
        virtual std::optional<std::size_t> write(SocketId socket, std::span<const std::byte> bytes) = 0;
        // Ends what this side sends, so the client reads to the end of its stream.
        virtual void shutdownSend(SocketId socket) = 0;
        virtual void close(SocketId socket) = 0;
        // From now on accept finds nothing. A set with no listener has nothing to close.
        virtual void closeListener() = 0;
        // Ends a sleeping wait, now or the next one, from any thread.
        virtual void wake() noexcept = 0;
        // Gives a connection up without closing it, for another gateway thread's set to adopt.
        virtual HandedSocket release(SocketId socket) = 0;
        // Takes on a connection another set released, as if just accepted.
        virtual SocketId adopt(HandedSocket socket) = 0;
    };

}  // namespace order_book::network
