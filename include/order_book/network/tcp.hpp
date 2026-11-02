#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "order_book/network/socket_set.hpp"

// A thin wrapper over Winsock and POSIX sockets: loopback TCP only, which is all the engine and
// its tests listen on. Nothing here is a protocol. See docs/internals/network.md.
namespace order_book::network {

    // A SOCKET on Windows, a file descriptor elsewhere.
    using NativeSocket = std::uintptr_t;
    inline constexpr NativeSocket invalidSocket = static_cast<NativeSocket>(-1);

    // One socket, closed when this is destroyed. Setting one up throws std::system_error when
    // the system refuses; reading and writing report what happened instead.
    class TcpSocket {
    public:
        TcpSocket() = default;
        explicit TcpSocket(NativeSocket handle) noexcept : held(handle) {}
        TcpSocket(TcpSocket&& other) noexcept;
        TcpSocket& operator=(TcpSocket&& other) noexcept;
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;
        ~TcpSocket();

        // A non-blocking listener on 127.0.0.1. Port 0 lets the system pick, and localPort says
        // which it did.
        static TcpSocket listenLoopback(std::uint16_t port);
        // A non-blocking socket bound to this IPv4 address and port, not yet listening, so a busy
        // port fails now and nobody can connect before listen. It takes the address exclusively
        // on Windows, and with SO_REUSEADDR elsewhere, so a restart needn't wait out TIME_WAIT.
        static TcpSocket bindIpv4(const std::array<std::uint8_t, 4>& host, std::uint16_t port);
        void listen() const;
        // A blocking connection to 127.0.0.1, with TCP_NODELAY on.
        static TcpSocket connectLoopback(std::uint16_t port);

        // A waiting connection, non-blocking and with TCP_NODELAY on, or nothing if none waits.
        std::optional<TcpSocket> accept() const;

        void setNonBlocking() const;
        void setNoDelay() const;
        bool noDelay() const;
        std::uint16_t localPort() const;

        // On a non-blocking socket with nothing to read, 0 bytes and OPEN.
        ReadResult receive(std::span<std::byte> into) const noexcept;
        // How much was taken, 0 on a non-blocking socket with no room, or nothing if the
        // connection is gone. Never raises SIGPIPE.
        std::optional<std::size_t> send(std::span<const std::byte> bytes) const noexcept;
        void shutdownSend() const noexcept;
        void close() noexcept;

        bool valid() const noexcept { return held != invalidSocket; }
        NativeSocket handle() const noexcept { return held; }

    private:
        NativeSocket held = invalidSocket;
    };

    // One poll or WSAPoll over a list of sockets, built afresh before each wait.
    class Poller {
    public:
        Poller();
        ~Poller();
        Poller(const Poller&) = delete;
        Poller& operator=(const Poller&) = delete;

        void clear() noexcept;
        void add(NativeSocket socket, bool read, bool write);
        // Waits until a socket is ready, or not at all with timeout 0, or for ever with -1.
        void wait(int timeoutMilliseconds);
        // Readable includes a closed or failed socket, so the read that follows finds out.
        bool readable(std::size_t entry) const noexcept;
        bool writable(std::size_t entry) const noexcept;

    private:
        struct Entries;
        std::unique_ptr<Entries> entries;
    };

}  // namespace order_book::network
