#include "order_book/network/in_memory_socket_set.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace order_book::network {

    SocketId InMemorySocketSet::connect() {
        sockets.emplace_back();
        return static_cast<SocketId>(sockets.size() - 1);
    }

    void InMemorySocketSet::clientSend(SocketId socket, std::span<const std::byte> bytes) {
        std::vector<std::byte>& toEngine = sockets.at(socket).toEngine;
        toEngine.insert(toEngine.end(), bytes.begin(), bytes.end());
    }

    std::vector<std::byte> InMemorySocketSet::clientReceive(SocketId socket) {
        return std::exchange(sockets.at(socket).toClient, {});
    }

    void InMemorySocketSet::clientHalfClose(SocketId socket) {
        sockets.at(socket).halfClosed = true;
    }

    void InMemorySocketSet::clientReset(SocketId socket) {
        sockets.at(socket).reset = true;
    }

    void InMemorySocketSet::wait() {
        ++waitCount;
        if (waitAction) {
            waitAction();
        }
    }

    std::optional<SocketId> InMemorySocketSet::accept() {
        if (listenerGone || nextToAccept == sockets.size()) {
            return std::nullopt;
        }
        return nextToAccept++;
    }

    ReadResult InMemorySocketSet::read(SocketId socket, std::span<std::byte> into) {
        if (readAction) {
            readAction(socket);
        }
        Socket& held = sockets.at(socket);
        ++held.reads;
        if (held.reset) {
            return ReadResult{0, ReadStatus::FAILED};
        }
        const std::size_t bytes = std::min({into.size(), readLimit, held.toEngine.size() - held.position});
        if (bytes != 0) {
            std::memcpy(into.data(), held.toEngine.data() + held.position, bytes);
            held.position += bytes;
        }
        const bool ended = held.halfClosed && held.position == held.toEngine.size() && bytes == 0;
        return ReadResult{bytes, ended ? ReadStatus::ENDED : ReadStatus::OPEN};
    }

    std::optional<std::size_t> InMemorySocketSet::write(SocketId socket, std::span<const std::byte> bytes) {
        Socket& held = sockets.at(socket);
        ++held.writes;
        if (held.reset) {
            return std::nullopt;
        }
        const std::span<const std::byte> taken = bytes.first(std::min(bytes.size(), writeLimit));
        held.toClient.insert(held.toClient.end(), taken.begin(), taken.end());
        return taken.size();
    }

    void InMemorySocketSet::shutdownSend(SocketId socket) {
        sockets.at(socket).sendShutDown = true;
    }

    void InMemorySocketSet::close(SocketId socket) {
        sockets.at(socket).closed = true;
    }

    void InMemorySocketSet::closeListener() {
        listenerGone = true;
    }

}  // namespace order_book::network
