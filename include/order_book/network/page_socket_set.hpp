#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "order_book/network/socket_set.hpp"

namespace order_book::network {

    // The in-page connection (docs/internals/network.md): a SocketSet whose clients live in the same
    // process, carrying exactly what TCP carries, an ordered stream of bytes each way, half-close,
    // close, and reset, and nothing else. It is the browser's set, beside PollSocketSet, with no
    // Emscripten code, so it is built and tested on every platform; InMemorySocketSet stays the
    // test double.
    //
    // Each direction is a buffer bounded as a socket's is, so a write takes only what fits and a
    // full buffer pushes back into the gateway's unsent bytes, high-water mark, hard limit, and
    // parking. What a side has read is trimmed. Slots are made as connections come and reused once
    // both sides are done with one, under a generation, so a stale SocketId is caught.
    //
    // The gateway side is one gateway thread's, and the client side one other thread's, the
    // engine worker's runtime thread in the browser. Whenever the gateway writes, frees room by
    // reading, shuts down its sending, or closes, the set calls clientHasSomething, with no lock
    // held: it must not block or call back into the set. A connection ends on the client side as
    // ENDED, once the gateway shut down its sending and every byte before that was taken, or
    // RESET, once the gateway closed without shutting down first, dropping what it hadn't taken.
    class PageSocketSet final : public SocketSet {
    public:
        // Room in each direction of each connection.
        static constexpr std::size_t defaultBufferBytes = 64 * 1024;

        explicit PageSocketSet(std::function<void()> tellTheClient, std::size_t buffer = defaultBufferBytes);
        PageSocketSet(const PageSocketSet&) = delete;
        PageSocketSet& operator=(const PageSocketSet&) = delete;

        // The client's side, from one thread.
        enum class ClientEnd {
            OPEN,
            ENDED,  // every byte the gateway sent was taken, then it shut down its sending
            RESET,  // the gateway closed at once; what it hadn't taken is gone
        };
        // A new connection, waiting to be accepted; none once the listener is closed.
        std::optional<SocketId> clientConnect();
        // Takes as much of bytes as there is room for, which may be none, and nothing once the
        // client has half-closed or the connection has ended.
        std::size_t clientSend(SocketId socket, std::span<const std::byte> bytes);
        // Takes up to most of what the gateway has sent, which frees that much room for it.
        std::vector<std::byte> clientReceive(SocketId socket, std::size_t most = static_cast<std::size_t>(-1));
        ClientEnd clientEnd(SocketId socket) const;
        void clientHalfClose(SocketId socket);
        // Ends the connection at once both ways: the gateway's next read or write finds it gone.
        void clientReset(SocketId socket);
        // The client is done with the connection, which may then be reused once the gateway has
        // closed it too. The id means nothing afterwards.
        void clientRelease(SocketId socket);

        void wait(std::span<const SocketInterest> interest, WaitMode mode) override;
        std::optional<SocketId> accept() override;
        ReadResult read(SocketId socket, std::span<std::byte> into) override;
        std::optional<std::size_t> write(SocketId socket, std::span<const std::byte> bytes) override;
        void shutdownSend(SocketId socket) override;
        void close(SocketId socket) override;
        void closeListener() override;
        void wake() noexcept override;
        // The browser runs one gateway thread, so no connection is ever handed on: both throw
        // std::logic_error.
        HandedSocket release(SocketId socket) override;
        SocketId adopt(HandedSocket socket) override;

        // Slots made so far, the session's peak of connections at once.
        std::size_t slots() const;

    private:
        struct Slot {
            std::uint16_t generation = 0;
            bool inUse = false;
            std::deque<std::byte> toEngine;
            std::deque<std::byte> toClient;
            bool clientHalfClosed = false;
            bool clientReset = false;
            bool clientReleased = false;
            bool sendShutDown = false;  // the gateway's
            bool gatewayClosed = false;
            bool resetByGateway = false;
        };

        static SocketId idOf(std::size_t index, std::uint16_t generation) noexcept;
        // The slot this id names, or std::logic_error for an id it no longer names.
        Slot& slotOf(SocketId socket);
        const Slot& slotOf(SocketId socket) const;
        bool ready(std::span<const SocketInterest> interest) const;
        void freeIfDone(Slot& slot);

        const std::function<void()> clientHasSomething;
        const std::size_t bufferBytes;
        mutable std::mutex lock;
        std::condition_variable changed;
        std::vector<Slot> slotList;
        std::deque<SocketId> waitingToBeAccepted;
        bool listenerClosed = false;
        bool woken = false;
    };

}  // namespace order_book::network
