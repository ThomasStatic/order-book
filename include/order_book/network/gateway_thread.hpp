#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "order_book/network/entry_check.hpp"
#include "order_book/network/socket_set.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"

namespace order_book::network {

    // The most connections one gateway thread holds.
    inline constexpr std::uint32_t connectionsPerThread = 64;
    // A pass takes at most this many frames from a connection, which is the matching thread's batch.
    inline constexpr std::size_t framesPerPass = runtime::takeBatch;
    // A pass skips the wait while a reader holds frames, but never more than this many in a row,
    // so one client streaming can't keep every other socket's readiness from being looked at.
    inline constexpr std::size_t mostWaitsSkipped = 16;

    // A command element's 32-bit connection: an index, and a generation that moves on when the
    // index is closed, so a reused index can't receive a dead connection's responses.
    constexpr std::uint32_t connectionHandle(std::uint32_t index, std::uint16_t generation) noexcept {
        return (static_cast<std::uint32_t>(generation) << 16) | index;
    }

    constexpr std::uint32_t connectionIndex(std::uint32_t handle) noexcept {
        return handle & 0xffffU;
    }

    struct GatewaySettings {
        // Past this many unsent bytes a connection is backlogged, and not read until they drain.
        std::size_t highWaterMark = 1024 * 1024;
        // Past this many it closes.
        std::size_t hardLimit = 16 * 1024 * 1024;
        // How long, once every shard has stopped, connections have to take what they're owed.
        std::chrono::nanoseconds stopGrace = std::chrono::seconds(5);
        // How long a thread with nothing in flight or parked looks before it sleeps, the matching
        // thread's setting, so 0 sleeps at once and runtime::unlimitedSpin never does.
        std::chrono::nanoseconds spinWindow = runtime::defaultSpinWindow;
        // The gateway's only clock, read only while idle or stopping.
        std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now;
    };

    // One shard as a gateway thread sees it. This thread is the only writer of commands and the
    // only reader of responses.
    struct ShardLink {
        runtime::Ring<runtime::CommandElement>* commands;
        runtime::Ring<runtime::ResponseRecord>* responses;
        runtime::Doorbell* doorbell;
    };

    // What a gateway thread does, as a plain object, the way Shard is, so a test can step it on one
    // thread and the thread itself is a thin loop around pass. The directory, rings, doorbells, and
    // sockets must outlive it.
    class GatewayThread {
    public:
        GatewayThread(const runtime::InstrumentDirectory& directory, std::vector<ShardLink> shards, SocketSet& sockets,
                      GatewaySettings settings = {});

        // One pass: waits, unless a connection's reader still holds frames and the last
        // mostWaitsSkipped passes didn't wait either, sleeping only if mayWait has held for the
        // spin window; accepts, drains every response ring into
        // per-connection buffers, retries parked connections oldest first, reads and answers up
        // to framesPerPass frames from each connection that is neither parked nor backlogged,
        // rings the doorbells, then makes one write per connection with bytes to send, closing
        // those that are done. Returns whether it moved anything.
        bool pass();

        // Closes the listener, refuses parked commands, and from now on reads only to refuse, then
        // sends each shard a stop marker. Once every shard has stopped, connections stop being
        // read, take what they're owed, and close, within the stop grace.
        void beginStop();

        // Whether a stop has run its course: every shard stopped, and every connection closed.
        bool finished() const noexcept;

        // Whether nothing is in flight, parked, or read and waiting, and no stop has begun, so no
        // response can arrive during a sleep, and a matching thread never makes a system call on
        // the way back. Unsent bytes don't keep it awake, since a wait watches for room.
        bool mayWait() const noexcept;

    private:
        struct Parked {
            std::uint32_t shard;
            runtime::CommandElement element;
        };

        struct Connection {
            Connection() : reader(FrameSource::CLIENT) {}

            FrameReader reader;
            std::vector<std::byte> unsent;
            std::optional<Parked> parked;
            std::optional<FrameError> error;  // sent once its commands have been answered
            std::uint32_t inFlight = 0;       // this connection's share of the thread's count
            SocketId socket = 0;
            std::uint16_t generation = 0;
            bool open = false;
            bool reading = false;
            bool framesWaiting = false;  // a pass stopped with frames possibly still in reader
            bool finishing = false;      // closes once it has answered everything it read
        };

        // What a pass reads: open, reading, neither parked nor backlogged.
        bool readable(const Connection& connection) const noexcept;
        bool framesInAReader() const noexcept;
        WaitMode waitMode();
        void listInterest();
        void acceptConnections();
        void refuseConnection(SocketId socket);
        void closeConnection(std::uint32_t index);
        void drainResponses();
        void retryParked();
        void refuseParked();
        void sendStopMarkers();
        void beginFlush();
        void readConnection(std::uint32_t index);
        void takeFrame(std::uint32_t index, const Frame& frame);
        void takeCommand(std::uint32_t index, const Frame& frame);
        void queue(std::uint32_t index, const EntryAccepted& accepted, Correlation correlation);
        bool tryQueue(std::uint32_t index, std::uint32_t shard, const runtime::CommandElement& element);
        void ringDoorbells();
        void sendResponses();
        void sendTo(std::uint32_t index);
        std::uint32_t handleOf(std::uint32_t index) const noexcept {
            return connectionHandle(index, connections[index].generation);
        }

        const runtime::InstrumentDirectory& directory;
        std::vector<ShardLink> shards;
        SocketSet& sockets;
        GatewaySettings settings;
        std::vector<Connection> connections;
        // What this thread knows of each shard.
        struct ShardState {
            std::uint32_t parkedOn = 0;    // connections parked on it
            bool retryFailed = false;      // in this pass's retry, so nobody younger passes it
            bool pushedTo = false;         // since its doorbell last rang
            bool markerOwed = false;       // after a stop begins, until its marker is in
        };

        std::vector<std::uint32_t> parkedOldestFirst;  // connection indices
        std::vector<ShardState> shardStates;
        std::vector<std::byte> readBuffer;
        std::vector<SocketInterest> interest;
        std::uint64_t inFlight = 0;  // commands pushed whose last event hasn't come back
        bool moved = false;          // in this pass
        std::size_t waitsSkipped = 0;  // passes in a row
        bool idle = false;
        std::chrono::steady_clock::time_point idleSince;
        std::size_t shardsStopped = 0;
        bool stopBegun = false;
        bool flushing = false;  // every shard has stopped
        std::chrono::steady_clock::time_point graceEnds;
    };

    // The real thread: passes until a stop has run its course, beginning one once stopRequested
    // is set, and yielding after a pass that moved nothing, so a small machine still progresses
    // while this spins. Whoever sets stopRequested must then wake the thread's SocketSet, and
    // both that store and this load are sequentially consistent, as the wake's own flag is, so a
    // stop can't be missed between a thread's last look at the flag and its sleep.
    void runGateway(GatewayThread& gateway, const std::atomic<bool>& stopRequested);

}  // namespace order_book::network
