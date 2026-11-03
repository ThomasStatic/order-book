#include "order_book/network/in_memory_socket_set.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace order_book::network {

    InMemorySocketSet::InMemorySocketSet(Mode mode) : network(std::make_shared<Network>(mode)) {}

    InMemorySocketSet::InMemorySocketSet(MemberOf, InMemorySocketSet& networkSet) : network(networkSet.network) {
        const std::lock_guard guard(network->lock);
        index = network->members++;
    }

    SocketId InMemorySocketSet::connect() {
        const std::lock_guard guard(network->lock);
        network->sockets.emplace_back();
        network->changed.notify_all();
        return static_cast<SocketId>(network->sockets.size() - 1);
    }

    const InMemorySocketSet::Socket& InMemorySocketSet::at(SocketId socket) const {
        return network->sockets.at(socket);
    }

    InMemorySocketSet::Socket& InMemorySocketSet::held(SocketId socket) {
        Socket& found = network->sockets.at(socket);
        if (found.heldBy != index) {
            throw std::logic_error("gateway thread " + std::to_string(index) + " used connection " + std::to_string(socket) +
                                   ", which it doesn't hold");
        }
        return found;
    }

    void InMemorySocketSet::clientSend(SocketId socket, std::span<const std::byte> bytes) {
        const std::lock_guard guard(network->lock);
        std::vector<std::byte>& toEngine = network->sockets.at(socket).toEngine;
        toEngine.insert(toEngine.end(), bytes.begin(), bytes.end());
        network->changed.notify_all();
    }

    std::vector<std::byte> InMemorySocketSet::clientReceive(SocketId socket) {
        const std::lock_guard guard(network->lock);
        return std::exchange(network->sockets.at(socket).toClient, {});
    }

    std::vector<std::byte> InMemorySocketSet::clientReceiveWaiting(SocketId socket) {
        std::unique_lock guard(network->lock);
        network->changed.wait(guard, [&] {
            const Socket& found = at(socket);
            return !found.toClient.empty() || found.closed;
        });
        return std::exchange(network->sockets.at(socket).toClient, {});
    }

    void InMemorySocketSet::clientHalfClose(SocketId socket) {
        const std::lock_guard guard(network->lock);
        network->sockets.at(socket).halfClosed = true;
        network->changed.notify_all();
    }

    void InMemorySocketSet::clientReset(SocketId socket) {
        const std::lock_guard guard(network->lock);
        network->sockets.at(socket).reset = true;
        network->changed.notify_all();
    }

    void InMemorySocketSet::limitRead(std::size_t bytes) {
        const std::lock_guard guard(network->lock);
        network->readLimit = bytes;
    }

    void InMemorySocketSet::limitWrite(std::size_t bytes) {
        const std::lock_guard guard(network->lock);
        network->writeLimit = bytes;
    }

    std::size_t InMemorySocketSet::waits() const {
        const std::lock_guard guard(network->lock);
        return waitCount;
    }

    std::size_t InMemorySocketSet::looks() const {
        const std::lock_guard guard(network->lock);
        return lookCount;
    }

    bool InMemorySocketSet::wakePending() const {
        const std::lock_guard guard(network->lock);
        return woken;
    }

    std::size_t InMemorySocketSet::reads(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).reads;
    }

    std::size_t InMemorySocketSet::writes(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).writes;
    }

    std::size_t InMemorySocketSet::bytesRead(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).position;
    }

    std::size_t InMemorySocketSet::bytesSent(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).toEngine.size();
    }

    bool InMemorySocketSet::closed(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).closed;
    }

    bool InMemorySocketSet::accepted(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return socket < network->nextToAccept;
    }

    bool InMemorySocketSet::sendShutDown(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).sendShutDown;
    }

    bool InMemorySocketSet::listenerClosed() const {
        const std::lock_guard guard(network->lock);
        return network->listenerGone;
    }

    std::optional<std::size_t> InMemorySocketSet::holder(SocketId socket) const {
        const std::lock_guard guard(network->lock);
        return at(socket).heldBy;
    }

    // Under the lock. Writing always has room here, so any write interest is ready.
    bool InMemorySocketSet::ready(std::span<const SocketInterest> interest) const {
        if (index == 0 && !network->listenerGone && network->nextToAccept < network->sockets.size()) {
            return true;
        }
        return std::any_of(interest.begin(), interest.end(), [this](const SocketInterest& wanted) {
            const Socket& socket = at(wanted.socket);
            return wanted.write || (wanted.read && (socket.position < socket.toEngine.size() || socket.halfClosed || socket.reset));
        });
    }

    void InMemorySocketSet::wait(std::span<const SocketInterest> interest, WaitMode mode) {
        if (mode == WaitMode::LOOK) {
            const std::lock_guard guard(network->lock);
            ++lookCount;
            return;
        }
        if (network->mode == Mode::THREADED) {
            std::unique_lock guard(network->lock);
            ++waitCount;
            network->changed.wait(guard, [&] { return woken || ready(interest); });
            woken = false;
            return;
        }
        {
            const std::lock_guard guard(network->lock);
            ++waitCount;
        }
        if (waitAction) {
            waitAction(interest);  // unlocked, since it may call back into any member
        }
        const std::lock_guard guard(network->lock);
        woken = false;
    }

    std::optional<SocketId> InMemorySocketSet::accept() {
        const std::lock_guard guard(network->lock);
        if (index != 0 || network->listenerGone || network->nextToAccept == network->sockets.size()) {
            return std::nullopt;
        }
        network->sockets[network->nextToAccept].heldBy = index;
        return network->nextToAccept++;
    }

    ReadResult InMemorySocketSet::read(SocketId socket, std::span<std::byte> into) {
        if (readAction) {
            readAction(socket);
        }
        const std::lock_guard guard(network->lock);
        Socket& socketHeld = held(socket);
        ++socketHeld.reads;
        if (socketHeld.reset) {
            return ReadResult{0, ReadStatus::FAILED};
        }
        const std::size_t bytes = std::min({into.size(), network->readLimit, socketHeld.toEngine.size() - socketHeld.position});
        if (bytes != 0) {
            std::memcpy(into.data(), socketHeld.toEngine.data() + socketHeld.position, bytes);
            socketHeld.position += bytes;
        }
        const bool ended = socketHeld.halfClosed && socketHeld.position == socketHeld.toEngine.size() && bytes == 0;
        return ReadResult{bytes, ended ? ReadStatus::ENDED : ReadStatus::OPEN};
    }

    std::optional<std::size_t> InMemorySocketSet::write(SocketId socket, std::span<const std::byte> bytes) {
        const std::lock_guard guard(network->lock);
        Socket& socketHeld = held(socket);
        ++socketHeld.writes;
        if (socketHeld.reset) {
            return std::nullopt;
        }
        const std::span<const std::byte> taken = bytes.first(std::min(bytes.size(), network->writeLimit));
        socketHeld.toClient.insert(socketHeld.toClient.end(), taken.begin(), taken.end());
        network->changed.notify_all();
        return taken.size();
    }

    void InMemorySocketSet::shutdownSend(SocketId socket) {
        const std::lock_guard guard(network->lock);
        held(socket).sendShutDown = true;
        network->changed.notify_all();
    }

    void InMemorySocketSet::close(SocketId socket) {
        const std::lock_guard guard(network->lock);
        held(socket).closed = true;
        network->changed.notify_all();
    }

    // As over TCP, where closing a listener resets what was waiting to be accepted.
    void InMemorySocketSet::closeListener() {
        const std::lock_guard guard(network->lock);
        if (index != 0) {
            return;
        }
        network->listenerGone = true;
        for (std::size_t waiting = network->nextToAccept; waiting < network->sockets.size(); ++waiting) {
            network->sockets[waiting].closed = true;
        }
        network->changed.notify_all();
    }

    void InMemorySocketSet::wake() noexcept {
        const std::lock_guard guard(network->lock);
        woken = true;
        network->changed.notify_all();
    }

    HandedSocket InMemorySocketSet::release(SocketId socket) {
        const std::lock_guard guard(network->lock);
        Socket& socketHeld = held(socket);
        if (socketHeld.everHanded) {
            throw std::logic_error("connection " + std::to_string(socket) + " was handed on twice, though it stays on one thread for life");
        }
        socketHeld.heldBy.reset();
        socketHeld.everHanded = true;
        return socket;
    }

    SocketId InMemorySocketSet::adopt(HandedSocket handed) {
        const std::lock_guard guard(network->lock);
        const SocketId socket = static_cast<SocketId>(handed);
        Socket& found = network->sockets.at(socket);
        if (found.heldBy || !found.everHanded) {
            throw std::logic_error("gateway thread " + std::to_string(index) + " adopted connection " + std::to_string(socket) +
                                   ", which nobody had handed on");
        }
        found.heldBy = index;
        return socket;
    }

}  // namespace order_book::network
