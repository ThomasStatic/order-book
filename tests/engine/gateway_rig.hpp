#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/in_memory_socket_set.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

// One gateway thread and every shard, stepped by one thread on the in-memory SocketSet, and the
// clients that talk to it: what gateway_tests and the stepper drive. A check that fails throws
// std::runtime_error. See docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    struct ShardActor {
        ShardActor(const runtime::InstrumentDirectory& directory, std::uint32_t number, std::size_t ringSlots);

        runtime::Shard shard;
        runtime::Doorbell doorbell;
        runtime::Ring<runtime::CommandElement> commands;
        runtime::Ring<runtime::ResponseRecord> responses;
        std::optional<runtime::MatchingLoop> loop;
        bool reportedStop = false;
    };

    // A matching thread that finds its response ring full waits in place, and the only thing that
    // can make room is the gateway, so that wait runs one gateway pass. Its gateway sleeps as soon
    // as it may, on a clock only the test moves.
    class Rig {
    public:
        Rig(std::vector<runtime::Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots,
            network::GatewaySettings settings = {});

        // One pass, and a shard with every producer's marker reports that it has stopped, which is
        // all run does then.
        std::size_t match(std::uint32_t shard = 0);
        // Every actor in turn, far more times than any test here needs.
        void settle();

        runtime::InstrumentDirectory directory;
        network::InMemorySocketSet sockets;
        std::vector<std::unique_ptr<ShardActor>> shards;
        std::optional<network::GatewayThread> gateway;
        std::size_t waitsInPlace = 0;
        std::chrono::steady_clock::time_point now;
    };

    struct Received {
        network::FrameType type;
        bool last;
        network::Correlation correlation;
        std::uint16_t version = 0;
        std::optional<network::EventFrame> event;
        std::optional<network::RefusedFrame> refused;
        std::optional<network::ProtocolErrorFrame> error;
    };

    class Client {
    public:
        explicit Client(network::InMemorySocketSet& sockets);

        void send(std::span<const std::byte> bytes) { sockets.clientSend(socket, bytes); }
        // Everything received so far, in order.
        const std::vector<Received>& frames();
        // How many commands have had their last frame.
        std::size_t answered();

        network::InMemorySocketSet& sockets;
        network::SocketId socket;

    private:
        network::FrameReader reader;
        std::vector<Received> received;
    };

    bool refusedForStopping(const Received& frame);

}  // namespace order_book::engine_testing
