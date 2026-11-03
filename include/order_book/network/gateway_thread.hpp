#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

    // The most connections one gateway thread holds unless its settings say otherwise, and the
    // most they may say, since a connection's index has 16 bits.
    inline constexpr std::uint32_t connectionsPerThread = 64;
    inline constexpr std::size_t mostConnectionsPerThread = 65536;
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
        // Past this many open at once, a connection is accepted only to hear TOO_MANY_CONNECTIONS.
        // From 1 to mostConnectionsPerThread; anything else throws std::invalid_argument.
        std::size_t maxConnections = connectionsPerThread;
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
    // only reader of responses. A shard with a log has a durable position, and responses behind
    // a HOLD marker past it wait; with none, a marker holds nothing.
    struct ShardLink {
        runtime::Ring<runtime::CommandElement>* commands;
        runtime::Ring<runtime::ResponseRecord>* responses;
        runtime::Doorbell* doorbell;
        const runtime::DurablePosition* durable;
    };

    // How an engine's gateway threads share its connections. Thread 0 owns the listener and hands
    // each connection it accepts to the next thread round-robin, itself included, and a connection
    // stays where it lands for life. A hand-off is one small ring per other thread, which thread 0
    // alone writes and that thread alone reads, then a wake of that thread's SocketSet, so it
    // needs no lock and no shared counter. Built before any gateway thread, which it must outlive,
    // with every thread's sockets in thread order.
    class ConnectionHandOff {
    public:
        static constexpr std::size_t ringSlots = 64;

        explicit ConnectionHandOff(std::vector<SocketSet*> sockets);

        std::size_t threads() const noexcept { return sockets.size(); }
        SocketSet& socketsOf(std::size_t thread) const { return *sockets.at(thread); }
        // Thread 0's: puts the connection on that thread's ring, then wakes its set, in that
        // order, so a thread that drained its wake sees the connection. False, with nothing done,
        // if the ring is full.
        bool tryHandTo(std::size_t thread, HandedSocket socket) const;
        // Every other thread reads its own ring. Thread 0's is never used.
        runtime::Ring<HandedSocket>& ringOf(std::size_t thread) const { return *rings.at(thread); }

    private:
        std::vector<SocketSet*> sockets;
        std::vector<std::unique_ptr<runtime::Ring<HandedSocket>>> rings;
    };

    // What a gateway thread does, as a plain object, the way Shard is, so a test can step it on one
    // thread and the thread itself is a thin loop around pass. The directory, rings, doorbells, and
    // sockets must outlive it, and so must handOff, which is for an engine with several gateway
    // threads, of which this is number thread. With none, it is the only one.
    class GatewayThread {
    public:
        GatewayThread(const runtime::InstrumentDirectory& directory, std::vector<ShardLink> shards, SocketSet& sockets,
                      GatewaySettings settings = {}, ConnectionHandOff* handOff = nullptr, std::size_t thread = 0);

        // One pass: waits, unless a connection's reader still holds frames and the last
        // mostWaitsSkipped passes didn't wait either, sleeping only if mayWait has held for the
        // spin window; drains every response ring into per-connection buffers, stopping at a
        // HOLD marker the shard's durable position hasn't reached, accepts (thread
        // 0, handing on what isn't its own) or takes what was handed to it (any other), retries
        // parked connections oldest first, reads and answers up
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

        // Whether nothing is in flight, parked, read and waiting, or waiting to be handed on, and
        // no stop has begun, so no response can arrive during a sleep, and a matching thread never
        // makes a system call on the way back. Unsent bytes don't keep it awake, since a wait
        // watches for room.
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
        void takeHandedConnections();
        bool tryHandOnHeld();
        void takeOn(SocketId socket);
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
        ConnectionHandOff* handOff;
        std::size_t thread;
        std::size_t nextThread = 0;               // thread 0's: where the next connection goes
        struct HandingOn {
            std::size_t thread;
            HandedSocket socket;
        };
        std::optional<HandingOn> handingOn;       // thread 0's: released, its thread's ring full
        std::vector<Connection> connections;
        // What this thread knows of each shard.
        struct ShardState {
            std::uint32_t parkedOn = 0;    // connections parked on it
            bool retryFailed = false;      // in this pass's retry, so nobody younger passes it
            bool pushedTo = false;         // since its doorbell last rang
            bool markerOwed = false;       // after a stop begins, until its marker is in
            // The position a HOLD marker taken from its responses needs, until the log reaches
            // it: nothing after it drains until then. 0 holds nothing, since a marker needs a
            // command's position, so at least 1.
            std::uint64_t heldUntil = 0;
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
