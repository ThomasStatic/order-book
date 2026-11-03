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
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

// Every gateway thread and every shard, stepped by one thread on the in-memory SocketSet, and the
// clients that talk to them: what gateway_tests and the stepper drive. A check that fails throws
// std::runtime_error. See docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    // One gateway thread's pair of rings to one shard.
    struct RingLink {
        explicit RingLink(std::size_t ringSlots) : commands(ringSlots), responses(ringSlots) {}
        RingLink(std::size_t commandSlots, std::size_t responseSlots) : commands(commandSlots), responses(responseSlots) {}

        runtime::Ring<runtime::CommandElement> commands;
        runtime::Ring<runtime::ResponseRecord> responses;
    };

    struct ShardActor {
        ShardActor(const runtime::InstrumentDirectory& directory, std::uint32_t number, std::size_t ringSlots,
                   std::size_t gatewayThreads = 1);

        runtime::Shard shard;
        runtime::Doorbell doorbell;
        std::vector<std::unique_ptr<RingLink>> links;      // one per gateway thread
        runtime::Ring<runtime::CommandElement>& commands;  // gateway thread 0's
        runtime::Ring<runtime::ResponseRecord>& responses;
        std::optional<runtime::MatchingLoop> loop;
        bool reportedStop = false;
        // With a log: the shard's log ring, as small as its other rings, and its log writer.
        runtime::Ring<runtime::LogEntry> logRing;
        runtime::DurablePosition durable;
        runtime::Doorbell writerBell;
        std::optional<runtime::LogWriter> writer;
    };

    // A matching thread that finds its response ring or log ring full waits in place, and the only
    // things that can make room are a gateway and a log writer, so that wait runs one pass of each
    // gateway thread, then of each log writer. Each sleeps as soon as it may, on a clock only the
    // test moves. sockets is thread 0's, which listens, and each further thread has a member set
    // of its own over the same connections. With a durability level, every shard logs to
    // logStorage and its responses wait for its log writer.
    class Rig {
    public:
        Rig(std::vector<runtime::Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots,
            network::GatewaySettings settings = {}, std::size_t gatewayThreads = 1,
            std::optional<runtime::Durability> log = std::nullopt);

        // One pass, and a shard with every producer's marker reports that it has stopped, which is
        // all run does then.
        std::size_t match(std::uint32_t shard = 0);
        // One pass of the shard's log writer, if it has one: the records it wrote.
        std::size_t writeLog(std::uint32_t shard = 0);
        // Every actor in turn, far more times than any test here needs.
        void settle();
        network::InMemorySocketSet& socketsOf(std::size_t thread) { return thread == 0 ? sockets : *memberSockets[thread - 1]; }

        runtime::InstrumentDirectory directory;
        runtime::InMemoryLogStorage logStorage;
        std::unique_ptr<runtime::Housekeeping> housekeeping;  // with a log, on files of the default size
        network::InMemorySocketSet sockets;
        std::vector<std::unique_ptr<network::InMemorySocketSet>> memberSockets;
        std::vector<std::unique_ptr<ShardActor>> shards;
        std::unique_ptr<network::ConnectionHandOff> handOff;
        std::vector<std::unique_ptr<network::GatewayThread>> gateways;
        network::GatewayThread* gateway = nullptr;  // thread 0's
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
        // Takes what the engine has sent, blocking until there is some or the connection has
        // closed, for a client beside real gateway threads. Returns the frames received so far.
        const std::vector<Received>& framesWaiting();
        // How many commands have had their last frame.
        std::size_t answered();

        network::InMemorySocketSet& sockets;
        network::SocketId socket;

    private:
        const std::vector<Received>& take(const std::vector<std::byte>& bytes);

        network::FrameReader reader;
        std::vector<Received> received;
    };

    bool refusedForStopping(const Received& frame);

}  // namespace order_book::engine_testing
