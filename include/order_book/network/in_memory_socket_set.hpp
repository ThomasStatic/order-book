#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "order_book/network/socket_set.hpp"

namespace order_book::network {

    // A SocketSet with no sockets, for stepping a gateway on one thread: the test plays the
    // clients, chooses how reads and writes split, and sees every call. Not for threads.
    class InMemorySocketSet final : public SocketSet {
    public:
        // The client's side.
        SocketId connect();
        void clientSend(SocketId socket, std::span<const std::byte> bytes);
        std::vector<std::byte> clientReceive(SocketId socket);  // takes everything written so far
        void clientHalfClose(SocketId socket);
        void clientReset(SocketId socket);

        // The most one read or write may move from now on.
        void limitRead(std::size_t bytes) noexcept { readLimit = bytes; }
        void limitWrite(std::size_t bytes) noexcept { writeLimit = bytes; }
        // Runs inside every wait, which then returns at once, since nothing here can sleep.
        void onWait(std::function<void()> action) { waitAction = std::move(action); }
        // Runs at the start of every read, which is a point in the middle of a pass, so a test can
        // play another thread there.
        void onRead(std::function<void(SocketId)> action) { readAction = std::move(action); }

        std::size_t waits() const noexcept { return waitCount; }
        std::size_t reads(SocketId socket) const { return sockets.at(socket).reads; }
        std::size_t writes(SocketId socket) const { return sockets.at(socket).writes; }
        std::size_t bytesRead(SocketId socket) const { return sockets.at(socket).position; }
        bool closed(SocketId socket) const { return sockets.at(socket).closed; }
        bool sendShutDown(SocketId socket) const { return sockets.at(socket).sendShutDown; }
        bool listenerClosed() const noexcept { return listenerGone; }

        void wait() override;
        std::optional<SocketId> accept() override;
        ReadResult read(SocketId socket, std::span<std::byte> into) override;
        std::optional<std::size_t> write(SocketId socket, std::span<const std::byte> bytes) override;
        void shutdownSend(SocketId socket) override;
        void close(SocketId socket) override;
        void closeListener() override;

    private:
        struct Socket {
            std::vector<std::byte> toEngine;
            std::size_t position = 0;  // of toEngine, how much the engine has read
            std::vector<std::byte> toClient;
            std::size_t reads = 0;
            std::size_t writes = 0;
            bool halfClosed = false;
            bool reset = false;
            bool sendShutDown = false;
            bool closed = false;
        };

        std::vector<Socket> sockets;
        SocketId nextToAccept = 0;
        bool listenerGone = false;
        std::size_t readLimit = std::numeric_limits<std::size_t>::max();
        std::size_t writeLimit = std::numeric_limits<std::size_t>::max();
        std::function<void()> waitAction;
        std::function<void(SocketId)> readAction;
        std::size_t waitCount = 0;
    };

}  // namespace order_book::network
