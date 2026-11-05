#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "timetable.hpp"
#include "workload.hpp"

// The idle-connection row's echo (#33): a small raw socket echo over loopback, with no gateway, no
// frames, and no book. One server thread watches one active connection and some idle ones, all
// for reading, through one way of waiting on sockets, and sends back whatever the active one
// brings. Each command is one 64-byte message out and back on the sending thread, which spins on
// its reply, so what the idle connections change is the server's wait alone. It decides whether
// a socket set other than poll is worth adding behind the SocketSet seam.
namespace order_book::latency {

// POLL is poll on Linux and WSAPoll on Windows, which the engine's PollSocketSet uses.
enum class EchoSockets { POLL, COMPLETION_PORT, EPOLL };

const char* echoSocketsName(EchoSockets sockets);
// Nothing for a name other than poll, completion-port, or epoll, or one this platform lacks:
// completion-port is Windows only and epoll Linux only.
std::optional<EchoSockets> parseEchoSockets(std::string_view name);

struct IdleEchoSetup {
    EchoSockets sockets = EchoSockets::POLL;
    std::size_t idleConnections = 0;
    // The server waits as a gateway thread does, looking without sleeping for this long after the
    // last message before it sleeps until one comes: 0 always sleeps.
    std::chrono::nanoseconds spinWindow = runtime::defaultSpinWindow;
    std::function<void()> setUpServer;  // runs first on the server thread, for pinning it
};

class IdleEchoTarget {
public:
    static constexpr std::size_t messageBytes = 64;

    // Opens every connection, active first, and starts the server, untimed. Throws
    // std::runtime_error with the system's reason if a socket call fails.
    IdleEchoTarget(const benchmark::Workload& workload, const IdleEchoSetup& setup);
    ~IdleEchoTarget();

    IdleEchoTarget(const IdleEchoTarget&) = delete;
    IdleEchoTarget& operator=(const IdleEchoTarget&) = delete;

    template <typename DriverClock>
    Stamps operator()(std::size_t index, DriverClock& clock) {
        prepare(index);
        const Ticks started = clock.now();
        exchange();
        const Ticks done = clock.now();
        check();
        return {started, done};
    }

    // Closes the active connection, which stops the server, and rethrows anything the server
    // threw. Throws std::runtime_error unless every message came back as sent.
    void checkOutcome();

    std::uint64_t capacityGrowths() const { return 0; }  // no book runs

    struct Sockets;  // the platform's, in the source

private:
    void prepare(std::size_t index);
    void exchange();
    void check();
    void serve(const IdleEchoSetup& setup);
    void stop() noexcept;

    std::unique_ptr<Sockets> sockets_;
    std::array<std::byte, messageBytes> message_{};
    std::array<std::byte, messageBytes> reply_{};
    std::size_t exchanged_ = 0;
    std::size_t mismatched_ = 0;
    std::atomic<bool> serverFailed_{false};
    std::exception_ptr serverFailure_;
    std::thread server_;
    bool stopped_ = false;
};

}  // namespace order_book::latency
