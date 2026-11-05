#include "order_book/network/tcp.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace order_book::network {

    namespace {
#ifdef _WIN32
        using Native = SOCKET;
        using PollEntry = WSAPOLLFD;

        // Once per process, before the first socket.
        void startSockets() {
            static const int started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data);
            }();
            if (started != 0) {
                throw std::system_error(started, std::system_category(), "WSAStartup");
            }
        }

        int lastError() noexcept { return WSAGetLastError(); }
        bool wouldBlock(int error) noexcept { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
        int closeNative(Native socket) noexcept { return closesocket(socket); }
        int pollNative(PollEntry* entries, std::size_t count, int timeout) noexcept {
            return WSAPoll(entries, static_cast<ULONG>(count), timeout);
        }
        constexpr int shutdownSendHow = SD_SEND;
        constexpr int sendFlags = 0;
        using Length = int;
#else
        using Native = int;
        using PollEntry = pollfd;

        void startSockets() {}
        int lastError() noexcept { return errno; }
        bool wouldBlock(int error) noexcept { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
        int closeNative(Native socket) noexcept { return ::close(socket); }
        int pollNative(PollEntry* entries, std::size_t count, int timeout) noexcept {
            return ::poll(entries, static_cast<nfds_t>(count), timeout);
        }
        constexpr int shutdownSendHow = SHUT_WR;
        constexpr int sendFlags = MSG_NOSIGNAL;
        using Length = std::size_t;
#endif

        Native native(NativeSocket socket) noexcept { return static_cast<Native>(socket); }

        [[noreturn]] void fail(const char* what) {
            throw std::system_error(lastError(), std::system_category(), what);
        }

        // Network byte order by hand, since htons can be a macro that trips -Wconversion.
        sockaddr_in ipv4(const std::array<std::uint8_t, 4>& host, std::uint16_t port) noexcept {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            const std::array<std::uint8_t, 2> portBytes{static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port & 0xffU)};
            std::memcpy(&address.sin_port, portBytes.data(), portBytes.size());
            std::memcpy(&address.sin_addr, host.data(), host.size());
            return address;
        }

        std::uint16_t portOf(const sockaddr_in& address) noexcept {
            std::array<std::uint8_t, 2> portBytes{};
            std::memcpy(portBytes.data(), &address.sin_port, portBytes.size());
            return static_cast<std::uint16_t>((portBytes[0] << 8) | portBytes[1]);
        }

        TcpSocket openStream() {
            startSockets();
            const Native created = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            TcpSocket socket(static_cast<NativeSocket>(created));
            if (!socket.valid()) {
                fail("socket");
            }
            return socket;
        }
    }  // namespace

    TcpSocket::TcpSocket(TcpSocket&& other) noexcept : held(std::exchange(other.held, invalidSocket)) {}

    TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            close();
            held = std::exchange(other.held, invalidSocket);
        }
        return *this;
    }

    TcpSocket::~TcpSocket() {
        close();
    }

    TcpSocket TcpSocket::listenLoopback(std::uint16_t port) {
        TcpSocket socket = bindIpv4({127, 0, 0, 1}, port);
        socket.listen();
        return socket;
    }

    TcpSocket TcpSocket::bindIpv4(const std::array<std::uint8_t, 4>& host, std::uint16_t port) {
        TcpSocket socket = openStream();
        const int on = 1;
#ifdef _WIN32
        const int option = SO_EXCLUSIVEADDRUSE;
#else
        const int option = SO_REUSEADDR;
#endif
        if (::setsockopt(native(socket.held), SOL_SOCKET, option, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
            fail("setsockopt");
        }
        const sockaddr_in address = ipv4(host, port);
        if (::bind(native(socket.held), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            fail("bind");
        }
        socket.setNonBlocking();
        return socket;
    }

    void TcpSocket::listen() const {
        if (::listen(native(held), SOMAXCONN) != 0) {
            fail("listen");
        }
    }

    TcpSocket TcpSocket::connectLoopback(std::uint16_t port) {
        return connectIpv4({127, 0, 0, 1}, port);
    }

    TcpSocket TcpSocket::connectIpv4(const std::array<std::uint8_t, 4>& host, std::uint16_t port) {
        TcpSocket socket = openStream();
        const sockaddr_in address = ipv4(host, port);
        if (::connect(native(socket.held), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            fail("connect");
        }
        socket.setNoDelay();
        return socket;
    }

    std::optional<TcpSocket> TcpSocket::accept() const {
        const Native accepted = ::accept(native(held), nullptr, nullptr);
        TcpSocket socket(static_cast<NativeSocket>(accepted));
        if (!socket.valid()) {
            return std::nullopt;  // none waiting, or one that went before it could be taken
        }
        // Linux doesn't pass a listener's flags on, so both are set here, always.
        socket.setNonBlocking();
        socket.setNoDelay();
        return socket;
    }

    void TcpSocket::setNonBlocking() const {
#ifdef _WIN32
        u_long on = 1;
        // The SDK's FIONBIO is an unsigned long, which ioctlsocket takes as a long.
        if (ioctlsocket(native(held), static_cast<long>(FIONBIO), &on) != 0) {
            fail("ioctlsocket");
        }
#else
        const int flags = fcntl(native(held), F_GETFL, 0);
        if (flags < 0 || fcntl(native(held), F_SETFL, flags | O_NONBLOCK) != 0) {
            fail("fcntl");
        }
#endif
    }

    void TcpSocket::setNoDelay() const {
        const int on = 1;
        if (::setsockopt(native(held), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
            fail("setsockopt TCP_NODELAY");
        }
    }

    bool TcpSocket::noDelay() const {
        int value = 0;
        socklen_t size = sizeof(value);
        if (::getsockopt(native(held), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), &size) != 0) {
            fail("getsockopt TCP_NODELAY");
        }
        return value != 0;
    }

    std::uint16_t TcpSocket::localPort() const {
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        if (::getsockname(native(held), reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            fail("getsockname");
        }
        return portOf(address);
    }

    ReadResult TcpSocket::receive(std::span<std::byte> into) const noexcept {
        const std::size_t asked = std::min<std::size_t>(into.size(), INT_MAX);
        const auto received = ::recv(native(held), reinterpret_cast<char*>(into.data()), static_cast<Length>(asked), 0);
        if (received > 0) {
            return ReadResult{static_cast<std::size_t>(received), ReadStatus::OPEN};
        }
        if (received == 0) {
            return ReadResult{0, ReadStatus::ENDED};
        }
        return ReadResult{0, wouldBlock(lastError()) ? ReadStatus::OPEN : ReadStatus::FAILED};
    }

    std::optional<std::size_t> TcpSocket::send(std::span<const std::byte> bytes) const noexcept {
        const std::size_t offered = std::min<std::size_t>(bytes.size(), INT_MAX);
        const auto sent = ::send(native(held), reinterpret_cast<const char*>(bytes.data()), static_cast<Length>(offered), sendFlags);
        if (sent >= 0) {
            return static_cast<std::size_t>(sent);
        }
        if (wouldBlock(lastError())) {
            return std::size_t{0};
        }
        return std::nullopt;
    }

    void TcpSocket::shutdownSend() const noexcept {
        static_cast<void>(::shutdown(native(held), shutdownSendHow));
    }

    void TcpSocket::close() noexcept {
        if (valid()) {
            static_cast<void>(closeNative(native(held)));
            held = invalidSocket;
        }
    }

    struct Poller::Entries {
        std::vector<PollEntry> list;
    };

    Poller::Poller() : entries(std::make_unique<Entries>()) {}
    Poller::~Poller() = default;

    void Poller::clear() noexcept {
        entries->list.clear();
    }

    void Poller::add(NativeSocket socket, bool read, bool write) {
        PollEntry entry{};
        entry.fd = native(socket);
        entry.events = static_cast<short>((read ? POLLIN : 0) | (write ? POLLOUT : 0));
        entries->list.push_back(entry);
    }

    void Poller::wait(int timeoutMilliseconds) {
        for (PollEntry& entry : entries->list) {
            entry.revents = 0;
        }
        if (pollNative(entries->list.data(), entries->list.size(), timeoutMilliseconds) < 0 && !wouldBlock(lastError())) {
            fail("poll");
        }
    }

    bool Poller::readable(std::size_t entry) const noexcept {
        return (entries->list[entry].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }

    bool Poller::writable(std::size_t entry) const noexcept {
        return (entries->list[entry].revents & (POLLOUT | POLLERR)) != 0;
    }

}  // namespace order_book::network
