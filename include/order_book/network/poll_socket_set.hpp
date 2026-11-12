#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "order_book/network/socket_set.hpp"
#include "order_book/network/tcp.hpp"

namespace order_book::network {

    // The SocketSet a real gateway thread uses: one listener, the connections it accepts or
    // adopts, and poll or WSAPoll over them. A read or accept makes a system call only for a
    // socket the last wait found ready. It belongs to one gateway thread, except wake.
    class PollSocketSet final : public SocketSet {
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;
        struct NoListener {};

        // A further gateway thread's, which takes connections only by adopt.
        explicit PollSocketSet(NoListener, Clock clock = std::chrono::steady_clock::now);

        // Listening on the loopback interface at once. Port 0 lets the system pick. clock times
        // lingering sockets and nothing else.
        explicit PollSocketSet(std::uint16_t port = 0, Clock clock = std::chrono::steady_clock::now);
        // Bound to this address, and listening only once startListening runs, so a busy port
        // fails here and no client can connect before the engine is ready.
        PollSocketSet(const std::array<std::uint8_t, 4>& host, std::uint16_t port, Clock clock = std::chrono::steady_clock::now);

        void startListening() override;
        // The port bound, which the system picked if it was asked for 0.
        std::uint16_t port() const noexcept { return listeningPort; }
        // Ends a sleeping wait, from any thread, through a loopback socket of this set's own. Every
        // change to its flag is a sequentially consistent exchange, as a doorbell's is, so a wake
        // after whatever it is for can't be lost between a wait's drain and a later check.
        void wake() noexcept override;
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
        // The native socket, which another set adopts. One after shutdownSend throws
        // std::logic_error, since it lingers here.
        HandedSocket release(SocketId socket) override;
        SocketId adopt(HandedSocket socket) override;

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

        void connectWakePair();
        SocketId hold(TcpSocket socket);
        void forget(SocketId socket);
        void linger();

        TcpSocket listener;
        TcpSocket wakeReceiver;
        TcpSocket wakeSender;
        std::atomic<bool> wakePending{false};
        Clock clock;
        std::uint16_t listeningPort = 0;
        bool listening = false;
        bool listenerReadable = false;
        std::vector<std::optional<Slot>> slots;
        std::vector<SocketId> freeSlots;
        std::vector<SocketId> lingering;
        Poller poller;
    };

}  // namespace order_book::network
