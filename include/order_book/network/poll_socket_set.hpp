#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "order_book/network/socket_set.hpp"
#include "order_book/network/tcp.hpp"

namespace order_book::network {

    // The SocketSet a real gateway thread uses: one listener on the loopback interface, the
    // connections it accepts, and poll or WSAPoll over them. A read or accept makes a system call
    // only for a socket the last wait found ready. It belongs to one gateway thread, except wake.
    class PollSocketSet final : public SocketSet {
    public:
        // Port 0 lets the system pick.
        explicit PollSocketSet(std::uint16_t port = 0);

        std::uint16_t port() const noexcept { return listeningPort; }
        // Ends a sleeping wait, from any thread, through a loopback socket of this set's own.
        void wake() noexcept;
        bool noDelay(SocketId socket) const;

        void wait(std::span<const SocketInterest> interest, WaitMode mode) override;
        std::optional<SocketId> accept() override;
        ReadResult read(SocketId socket, std::span<std::byte> into) override;
        std::optional<std::size_t> write(SocketId socket, std::span<const std::byte> bytes) override;
        void shutdownSend(SocketId socket) override;
        // After shutdownSend the socket lingers: every wait reads and discards what the client
        // still sends, and it closes once the client closes too, or after lingerBytes or
        // lingerTime. Over TCP, closing with unread bytes resets the connection, which can cost
        // the client what it was sent last. Without shutdownSend it closes at once.
        void close(SocketId socket) override;
        void closeListener() override;

        static constexpr std::size_t lingerBytes = 16 * 1024 * 1024;
        static constexpr std::chrono::seconds lingerTime{5};

    private:
        struct Slot {
            TcpSocket socket;
            bool readable = false;
            bool shutDown = false;
            std::size_t discarded = 0;                       // while lingering
            std::chrono::steady_clock::time_point lingerEnds;
        };

        void release(SocketId socket);
        void linger();

        TcpSocket listener;
        TcpSocket wakeReceiver;
        TcpSocket wakeSender;
        std::atomic<bool> wakePending{false};
        std::uint16_t listeningPort = 0;
        bool listening = true;
        bool listenerReadable = false;
        std::vector<std::optional<Slot>> slots;
        std::vector<SocketId> freeSlots;
        std::vector<SocketId> lingering;
        Poller poller;
    };

}  // namespace order_book::network
