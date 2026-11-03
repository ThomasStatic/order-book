#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "order_book/network/socket_set.hpp"

namespace order_book::network {

    // A SocketSet with no sockets: the test plays the clients, chooses how reads and writes split,
    // and sees every call. The first set made is the network, holding the clients' side and the
    // listener; a member made from it is another gateway thread's set over the same connections,
    // with no listener, which takes a connection only by adopt. A set may use only a connection it
    // holds, so one that moved between gateway threads twice, or was used by the wrong one, throws
    // std::logic_error.
    //
    // STEPPED, the default, is for one thread: a sleeping wait runs onWait and returns at once,
    // since nothing here can arrive while the test isn't looking. THREADED is for real gateway
    // threads beside the test's clients: every call takes the network's one lock, a sleeping wait
    // blocks until something it watches is ready, a connection waits to be accepted, or the set
    // is woken, and clientReceiveWaiting blocks for the engine's answer.
    class InMemorySocketSet final : public SocketSet {
    public:
        enum class Mode { STEPPED, THREADED };
        struct MemberOf {};

        explicit InMemorySocketSet(Mode mode = Mode::STEPPED);
        // A member of network's set: the next gateway thread's.
        InMemorySocketSet(MemberOf, InMemorySocketSet& network);

        InMemorySocketSet(const InMemorySocketSet&) = delete;
        InMemorySocketSet& operator=(const InMemorySocketSet&) = delete;

        // The client's side, from any member.
        SocketId connect();
        void clientSend(SocketId socket, std::span<const std::byte> bytes);
        std::vector<std::byte> clientReceive(SocketId socket);  // takes everything written so far
        // Blocks until there is something to take, or the connection has closed.
        std::vector<std::byte> clientReceiveWaiting(SocketId socket);
        void clientHalfClose(SocketId socket);
        void clientReset(SocketId socket);

        // The most one read or write may move from now on, in every member.
        void limitRead(std::size_t bytes);
        void limitWrite(std::size_t bytes);
        // STEPPED only. Runs inside every sleeping wait of this set, which then returns at once.
        void onWait(std::function<void(std::span<const SocketInterest>)> action) { waitAction = std::move(action); }
        // STEPPED only. Runs at the start of every read of this set, which is a point in the middle
        // of a pass, so a test can play another thread there.
        void onRead(std::function<void(SocketId)> action) { readAction = std::move(action); }

        std::size_t member() const noexcept { return index; }  // 0 for the network
        std::size_t waits() const;  // this set's sleeping ones
        std::size_t looks() const;
        // Whether a wake has come since this set's last sleeping wait.
        bool wakePending() const;
        std::size_t reads(SocketId socket) const;
        std::size_t writes(SocketId socket) const;
        std::size_t bytesRead(SocketId socket) const;
        std::size_t bytesSent(SocketId socket) const;
        bool closed(SocketId socket) const;
        bool accepted(SocketId socket) const;
        bool sendShutDown(SocketId socket) const;
        bool listenerClosed() const;
        // The member holding the connection, or nothing while it is handed between them, or
        // before it is accepted.
        std::optional<std::size_t> holder(SocketId socket) const;

        void wait(std::span<const SocketInterest> interest, WaitMode mode) override;
        std::optional<SocketId> accept() override;
        ReadResult read(SocketId socket, std::span<std::byte> into) override;
        std::optional<std::size_t> write(SocketId socket, std::span<const std::byte> bytes) override;
        void shutdownSend(SocketId socket) override;
        void close(SocketId socket) override;
        void closeListener() override;
        void wake() noexcept override;
        HandedSocket release(SocketId socket) override;
        SocketId adopt(HandedSocket socket) override;

    private:
        struct Socket {
            std::vector<std::byte> toEngine;
            std::size_t position = 0;  // of toEngine, how much the engine has read
            std::vector<std::byte> toClient;
            std::size_t reads = 0;
            std::size_t writes = 0;
            std::optional<std::size_t> heldBy;
            bool everHanded = false;
            bool halfClosed = false;
            bool reset = false;
            bool sendShutDown = false;
            bool closed = false;
        };

        struct Network {
            explicit Network(Mode networkMode) : mode(networkMode) {}

            const Mode mode;
            mutable std::mutex lock;
            std::condition_variable changed;
            std::vector<Socket> sockets;
            SocketId nextToAccept = 0;
            bool listenerGone = false;
            std::size_t members = 1;
            std::size_t readLimit = std::numeric_limits<std::size_t>::max();
            std::size_t writeLimit = std::numeric_limits<std::size_t>::max();
        };

        // A socket this set holds, under the lock.
        Socket& held(SocketId socket);
        const Socket& at(SocketId socket) const;
        bool ready(std::span<const SocketInterest> interest) const;

        std::shared_ptr<Network> network;
        std::size_t index = 0;
        bool woken = false;  // under the network's lock
        std::size_t waitCount = 0;
        std::size_t lookCount = 0;
        std::function<void(std::span<const SocketInterest>)> waitAction;
        std::function<void(SocketId)> readAction;
    };

}  // namespace order_book::network
