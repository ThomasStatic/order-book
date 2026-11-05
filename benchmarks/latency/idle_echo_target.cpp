#include "idle_echo_target.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace order_book::latency {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
const Socket noSocket = INVALID_SOCKET;
int lastSocketError() { return WSAGetLastError(); }
void closeSocket(Socket socket) { closesocket(socket); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
using SocketLength = int;
using IoLength = int;
constexpr int sendFlags = 0;
#else
using Socket = int;
constexpr Socket noSocket = -1;
int lastSocketError() { return errno; }
void closeSocket(Socket socket) { ::close(socket); }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
using SocketLength = socklen_t;
using IoLength = std::size_t;
constexpr int sendFlags = MSG_NOSIGNAL;
#endif

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("idle echo: could not " + what + ": error " + std::to_string(lastSocketError()));
}

void noDelay(Socket socket) {
    int on = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on) != 0) {
        fail("turn off Nagle's algorithm");
    }
}

void nonBlocking(Socket socket) {
#ifdef _WIN32
    u_long on = 1;
    // The SDK's FIONBIO is an unsigned long, which ioctlsocket takes as a long.
    if (ioctlsocket(socket, static_cast<long>(FIONBIO), &on) != 0) {
        fail("make a socket non-blocking");
    }
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail("make a socket non-blocking");
    }
#endif
}

// Every byte, on a blocking socket, or a non-blocking one that spins while it is full.
void sendAll(Socket socket, const char* bytes, std::size_t length) {
    while (length > 0) {
        const auto sent = ::send(socket, bytes, static_cast<IoLength>(length), sendFlags);
        if (sent < 0) {
            if (wouldBlock(lastSocketError())) {
                continue;
            }
            fail("send");
        }
        bytes += sent;
        length -= static_cast<std::size_t>(sent);
    }
}

// One read from the active connection, sent straight back. False once the client has closed it.
bool echo(Socket socket, char* buffer, std::size_t size) {
    const auto received = ::recv(socket, buffer, static_cast<IoLength>(size), 0);
    if (received == 0) {
        return false;
    }
    if (received < 0) {
        fail("receive on the server");
    }
    sendAll(socket, buffer, static_cast<std::size_t>(received));
    return true;
}

// Whether the server looks without sleeping: within the spin window of the last message.
bool looking(std::chrono::nanoseconds spinWindow, std::chrono::steady_clock::time_point idleSince) {
    if (spinWindow == runtime::unlimitedSpin) {
        return true;
    }
    return spinWindow > std::chrono::nanoseconds::zero() && std::chrono::steady_clock::now() - idleSince < spinWindow;
}

[[noreturn]] void idleWoke() {
    throw std::runtime_error("idle echo: an idle connection became ready, which none should");
}

constexpr std::size_t serverBufferBytes = 4096;

}  // namespace

struct IdleEchoTarget::Sockets {
#ifdef _WIN32
    bool started = false;  // WSAStartup succeeded
#endif
    Socket listener = noSocket;
    Socket activeClient = noSocket;
    std::vector<Socket> clients;  // the idle connections' client ends
    std::vector<Socket> served;   // the server's ends: the active one first, then the idle

    ~Sockets() {
        for (const Socket socket : served) {
            closeSocket(socket);
        }
        for (const Socket socket : clients) {
            closeSocket(socket);
        }
        if (activeClient != noSocket) {
            closeSocket(activeClient);
        }
        if (listener != noSocket) {
            closeSocket(listener);
        }
#ifdef _WIN32
        if (started) {
            WSACleanup();
        }
#endif
    }
};

const char* echoSocketsName(EchoSockets sockets) {
    switch (sockets) {
        case EchoSockets::POLL: return "poll";
        case EchoSockets::COMPLETION_PORT: return "completion-port";
        case EchoSockets::EPOLL: return "epoll";
    }
    return "poll";
}

std::optional<EchoSockets> parseEchoSockets(std::string_view name) {
    if (name == "poll") {
        return EchoSockets::POLL;
    }
#ifdef _WIN32
    if (name == "completion-port") {
        return EchoSockets::COMPLETION_PORT;
    }
#else
    if (name == "epoll") {
        return EchoSockets::EPOLL;
    }
#endif
    return std::nullopt;
}

IdleEchoTarget::IdleEchoTarget(const benchmark::Workload& workload, const IdleEchoSetup& setup) : sockets_(std::make_unique<Sockets>()) {
    static_cast<void>(workload);  // each command is one message; the workload gives only the count
    Sockets& s = *sockets_;
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("idle echo: could not start Winsock");
    }
    s.started = true;
#else
    // Both ends of every connection, and room for the rest of the process.
    const auto needed = static_cast<rlim_t>(2 * (setup.idleConnections + 1) + 256);
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < needed) {
        files.rlim_cur = std::min(needed, files.rlim_max);
        setrlimit(RLIMIT_NOFILE, &files);
        if (getrlimit(RLIMIT_NOFILE, &files) != 0 || files.rlim_cur < needed) {
            throw std::runtime_error("idle echo: " + std::to_string(setup.idleConnections) + " idle connections need " +
                                     std::to_string(needed) + " open files, past this process's limit");
        }
    }
#endif
    s.listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.listener == noSocket) {
        fail("make a socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    SocketLength length = sizeof address;
    if (::bind(s.listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 || ::listen(s.listener, SOMAXCONN) != 0 ||
        ::getsockname(s.listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail("listen on loopback");
    }
    // One connection at a time, so the first accepted is the active one.
    const auto connectOne = [&s, &address]() -> std::pair<Socket, Socket> {
        const Socket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client == noSocket) {
            fail("make a socket");
        }
        if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            closeSocket(client);
            fail("connect over loopback");
        }
        const Socket served = ::accept(s.listener, nullptr, nullptr);
        if (served == noSocket) {
            closeSocket(client);
            fail("accept a connection");
        }
        return {client, served};
    };
    const auto [activeClient, activeServed] = connectOne();
    s.activeClient = activeClient;
    s.served.push_back(activeServed);
    noDelay(s.activeClient);
    noDelay(activeServed);
    nonBlocking(s.activeClient);
    s.clients.reserve(setup.idleConnections);
    s.served.reserve(setup.idleConnections + 1);
    for (std::size_t i = 0; i < setup.idleConnections; ++i) {
        const auto [client, served] = connectOne();
        s.clients.push_back(client);
        s.served.push_back(served);
    }
    server_ = std::thread([this, setup] {
        try {
            if (setup.setUpServer) {
                setup.setUpServer();
            }
            serve(setup);
        } catch (...) {
            serverFailure_ = std::current_exception();
            serverFailed_.store(true);
        }
    });
}

IdleEchoTarget::~IdleEchoTarget() {
    stop();
}

void IdleEchoTarget::prepare(std::size_t index) {
    const auto value = static_cast<std::uint64_t>(index);
    for (std::size_t i = 0; i < message_.size(); ++i) {
        message_[i] = static_cast<std::byte>((value >> (8 * (i % 8))) & 0xFF);
    }
    reply_.fill(std::byte{0});
}

void IdleEchoTarget::exchange() {
    const Socket client = sockets_->activeClient;
    sendAll(client, reinterpret_cast<const char*>(message_.data()), message_.size());
    std::size_t received = 0;
    while (received < reply_.size()) {
        const auto read = ::recv(client, reinterpret_cast<char*>(reply_.data() + received), static_cast<IoLength>(reply_.size() - received), 0);
        if (read > 0) {
            received += static_cast<std::size_t>(read);
        } else if (read == 0) {
            throw std::runtime_error("idle echo: the server closed the active connection");
        } else if (!wouldBlock(lastSocketError())) {
            fail("receive the echo");
        } else if (serverFailed_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("idle echo: the server stopped");
        }
    }
    ++exchanged_;
}

void IdleEchoTarget::check() {
    if (reply_ != message_) {
        ++mismatched_;
    }
}

void IdleEchoTarget::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
#ifdef _WIN32
    shutdown(sockets_->activeClient, SD_SEND);
#else
    shutdown(sockets_->activeClient, SHUT_WR);
#endif
    if (server_.joinable()) {
        server_.join();
    }
}

void IdleEchoTarget::checkOutcome() {
    stop();
    if (serverFailure_) {
        std::rethrow_exception(serverFailure_);
    }
    if (mismatched_ != 0) {
        throw std::runtime_error("idle echo: " + std::to_string(mismatched_) + " of " + std::to_string(exchanged_) +
                                 " messages came back other than sent");
    }
}

void IdleEchoTarget::serve(const IdleEchoSetup& setup) {
    const std::vector<Socket>& served = sockets_->served;
    std::array<char, serverBufferBytes> buffer{};
    auto idleSince = std::chrono::steady_clock::now();
    if (setup.sockets == EchoSockets::POLL) {
#ifdef _WIN32
        std::vector<WSAPOLLFD> watched;
        for (const Socket socket : served) {
            watched.push_back(WSAPOLLFD{socket, POLLRDNORM, 0});
        }
#else
        std::vector<pollfd> watched;
        for (const Socket socket : served) {
            watched.push_back(pollfd{socket, POLLIN, 0});
        }
#endif
        for (;;) {
            const int timeout = looking(setup.spinWindow, idleSince) ? 0 : -1;
#ifdef _WIN32
            const int ready = WSAPoll(watched.data(), static_cast<ULONG>(watched.size()), timeout);
#else
            const int ready = ::poll(watched.data(), static_cast<nfds_t>(watched.size()), timeout);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
#endif
            if (ready < 0) {
                fail("wait in poll");
            }
            // Every entry is looked at, as a gateway pass looks at each connection.
            for (std::size_t i = 0; ready > 0 && i < watched.size(); ++i) {
                if (watched[i].revents == 0) {
                    continue;
                }
                if (i != 0) {
                    idleWoke();
                }
                if (!echo(served[0], buffer.data(), buffer.size())) {
                    return;
                }
                idleSince = std::chrono::steady_clock::now();
            }
        }
    }
#ifdef _WIN32
    if (setup.sockets == EchoSockets::COMPLETION_PORT) {
        // A receive stays posted on every connection, as a server on completion ports keeps one.
        struct Posted {
            OVERLAPPED overlapped{};
            WSABUF slice{};
            DWORD flags = 0;
            std::array<char, serverBufferBytes> bytes{};
        };
        std::vector<Posted> posted(served.size());
        // Cancels what is still posted and takes its completions before posted goes, however this
        // returns, since the system writes into each OVERLAPPED as it completes.
        struct Port {
            HANDLE handle = nullptr;
            const std::vector<Socket>* sockets = nullptr;
            std::size_t outstanding = 0;
            ~Port() {
                for (const Socket socket : *sockets) {
                    CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
                }
                // A cancelled receive always completes, so this waits for each.
                while (outstanding > 0) {
                    DWORD bytes = 0;
                    ULONG_PTR key = 0;
                    OVERLAPPED* overlapped = nullptr;
                    GetQueuedCompletionStatus(handle, &bytes, &key, &overlapped, INFINITE);
                    if (overlapped == nullptr) {
                        break;  // the port itself failed, and nothing more will come
                    }
                    --outstanding;
                }
                CloseHandle(handle);
            }
        } port{CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1), &served, 0};
        if (port.handle == nullptr) {
            throw std::runtime_error("idle echo: could not make a completion port: error " + std::to_string(GetLastError()));
        }
        const auto post = [&posted, &served, &port](std::size_t i) {
            Posted& one = posted[i];
            one.overlapped = OVERLAPPED{};
            one.slice = WSABUF{static_cast<ULONG>(one.bytes.size()), one.bytes.data()};
            one.flags = 0;
            if (WSARecv(served[i], &one.slice, 1, nullptr, &one.flags, &one.overlapped, nullptr) == SOCKET_ERROR &&
                WSAGetLastError() != WSA_IO_PENDING) {
                fail("post a receive");
            }
            ++port.outstanding;
        };
        for (std::size_t i = 0; i < served.size(); ++i) {
            if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(served[i]), port.handle, i, 0) == nullptr) {
                throw std::runtime_error("idle echo: could not join a socket to the completion port: error " +
                                         std::to_string(GetLastError()));
            }
            post(i);
        }
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const DWORD timeout = looking(setup.spinWindow, idleSince) ? 0 : INFINITE;
            const BOOL completed = GetQueuedCompletionStatus(port.handle, &bytes, &key, &overlapped, timeout);
            if (overlapped == nullptr) {
                if (GetLastError() == WAIT_TIMEOUT) {
                    continue;
                }
                throw std::runtime_error("idle echo: could not wait on the completion port: error " + std::to_string(GetLastError()));
            }
            --port.outstanding;
            if (key != 0) {
                idleWoke();
            }
            if (!completed) {
                throw std::runtime_error("idle echo: a receive on the active connection failed: error " +
                                         std::to_string(GetLastError()));
            }
            if (bytes == 0) {
                return;  // the client closed the active connection
            }
            sendAll(served[0], posted[0].bytes.data(), bytes);
            post(0);
            idleSince = std::chrono::steady_clock::now();
        }
    }
#else
    if (setup.sockets == EchoSockets::EPOLL) {
        struct Poller {
            int descriptor;
            ~Poller() { ::close(descriptor); }
        } poller{epoll_create1(EPOLL_CLOEXEC)};
        if (poller.descriptor < 0) {
            fail("make an epoll set");
        }
        for (std::size_t i = 0; i < served.size(); ++i) {
            epoll_event watch{};
            watch.events = EPOLLIN;
            watch.data.u64 = i;
            if (epoll_ctl(poller.descriptor, EPOLL_CTL_ADD, served[i], &watch) != 0) {
                fail("add a socket to the epoll set");
            }
        }
        std::array<epoll_event, 16> events{};
        for (;;) {
            const int timeout = looking(setup.spinWindow, idleSince) ? 0 : -1;
            const int ready = epoll_wait(poller.descriptor, events.data(), static_cast<int>(events.size()), timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("wait in epoll");
            }
            for (int i = 0; i < ready; ++i) {
                if (events[static_cast<std::size_t>(i)].data.u64 != 0) {
                    idleWoke();
                }
                if (!echo(served[0], buffer.data(), buffer.size())) {
                    return;
                }
                idleSince = std::chrono::steady_clock::now();
            }
        }
    }
#endif
    throw std::runtime_error(std::string("idle echo: ") + echoSocketsName(setup.sockets) + " is not on this platform");
}

}  // namespace order_book::latency
