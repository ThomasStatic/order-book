#include "order_book/wasm/page_connection.hpp"

#include <utility>

namespace order_book::wasm {

    std::optional<PageConnection> PageConnection::open(std::shared_ptr<network::PageSocketSet> sockets,
                                                       std::vector<network::ClientCommand> commands) {
        if (!sockets) {
            return std::nullopt;
        }
        const std::optional<network::SocketId> socket = sockets->clientConnect();
        if (!socket) {
            return std::nullopt;
        }
        std::optional<PageConnection> connection(PageConnection(std::move(sockets), *socket, std::move(commands)));
        connection->pump();
        return connection;
    }

    PageConnection::PageConnection(std::shared_ptr<network::PageSocketSet> sockets, network::SocketId id,
                                   std::vector<network::ClientCommand> commands)
        : set(std::move(sockets)), socket(id), client(std::move(commands)) {}

    PageConnection::PageConnection(PageConnection&& other) noexcept
        : set(std::move(other.set)),
          socket(other.socket),
          client(std::move(other.client)),
          finishing(other.finishing),
          halfClosed(other.halfClosed),
          released(other.released) {
        other.released = true;
    }

    PageConnection::~PageConnection() {
        if (released) {
            return;
        }
        if (set->clientEnd(socket) == network::PageSocketSet::ClientEnd::OPEN) {
            set->clientReset(socket);
        }
        set->clientRelease(socket);
    }

    std::size_t PageConnection::pump(std::size_t mostToReceive) {
        if (done()) {
            return 0;
        }
        for (std::span<const std::byte> out = client.outgoing(); !out.empty(); out = client.outgoing()) {
            const std::size_t taken = set->clientSend(socket, out);
            client.wrote(taken);
            if (taken < out.size()) {
                break;
            }
        }
        if (finishing && !halfClosed && client.allWritten()) {
            set->clientHalfClose(socket);
            halfClosed = true;
        }
        std::size_t received = 0;
        while (received < mostToReceive) {
            const std::vector<std::byte> bytes = set->clientReceive(socket, mostToReceive - received);
            if (bytes.empty()) {
                break;
            }
            received += bytes.size();
            client.received(bytes);
        }
        // Ended only once nothing is left to take, so a limited pump never ends it early.
        if (set->clientEnd(socket) != network::PageSocketSet::ClientEnd::OPEN) {
            client.ended();
        }
        return received;
    }

    std::optional<PageSend> PageSend::start(std::shared_ptr<network::PageSocketSet> sockets, cli::PreparedSend prepared) {
        std::optional<PageConnection> connection = PageConnection::open(std::move(sockets), prepared.commands);
        if (!connection) {
            return std::nullopt;
        }
        connection->finish();
        connection->pump();
        return PageSend(std::move(prepared), std::move(*connection));
    }

    PageSend::PageSend(cli::PreparedSend prepared, PageConnection opened) : session(std::move(prepared)), connection(std::move(opened)) {}

    std::optional<cli::CommandResult> PageSend::pump() {
        connection.pump();
        if (!connection.done()) {
            return std::nullopt;
        }
        return cli::finishSend(session, connection.stream());
    }

}  // namespace order_book::wasm
