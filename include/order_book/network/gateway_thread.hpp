#pragma once

#include <cstddef>
#include <cstdint>
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

    // A command element's 32-bit connection: an index, and a generation that moves on when the
    // index is closed, so a reused index can't receive a dead connection's responses.
    constexpr std::uint32_t connectionHandle(std::uint32_t index, std::uint16_t generation) noexcept {
        return (static_cast<std::uint32_t>(generation) << 16) | index;
    }

    constexpr std::uint32_t connectionIndex(std::uint32_t handle) noexcept {
        return handle & 0xffffU;
    }

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
        GatewayThread(const runtime::InstrumentDirectory& directory, std::vector<ShardLink> shards, SocketSet& sockets);

        // One pass: sleeps if mayWait, accepts, drains every response ring into per-connection
        // buffers, retries parked connections oldest first, reads and answers up to framesPerPass
        // frames from each connection that isn't parked, rings the doorbells, then makes one write
        // per connection with bytes to send.
        void pass();

        // Whether nothing is in flight, parked, read and waiting, or unsent, so no response can
        // arrive during a sleep, and a matching thread never makes a system call on the way back.
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
            SocketId socket = 0;
            std::uint16_t generation = 0;
            bool open = false;
            bool reading = false;
            bool framesWaiting = false;  // a pass stopped with frames possibly still in reader
        };

        void acceptConnections();
        void closeConnection(std::uint32_t index);
        void drainResponses();
        void retryParked();
        void readConnection(std::uint32_t index);
        void takeFrame(std::uint32_t index, const Frame& frame);
        void takeCommand(std::uint32_t index, const Frame& frame);
        void queue(std::uint32_t index, const EntryAccepted& accepted, Correlation correlation);
        bool tryQueue(std::uint32_t shard, const runtime::CommandElement& element);
        void ringDoorbells();
        void sendResponses();
        std::uint32_t handleOf(std::uint32_t index) const noexcept {
            return connectionHandle(index, connections[index].generation);
        }

        const runtime::InstrumentDirectory& directory;
        std::vector<ShardLink> shards;
        SocketSet& sockets;
        std::vector<Connection> connections;
        std::vector<std::uint32_t> parkedOldestFirst;  // connection indices
        std::vector<std::uint32_t> parkedOnShard;
        std::vector<char> shardBlocked;  // in this pass's retry, per shard
        std::vector<char> shardRung;     // pushed to since the last doorbell, per shard
        std::vector<std::byte> readBuffer;
        std::uint64_t inFlight = 0;  // commands pushed whose last event hasn't come back
    };

}  // namespace order_book::network
