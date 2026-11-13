#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "order_book/cli/commands.hpp"
#include "order_book/network/client_stream.hpp"
#include "order_book/network/page_socket_set.hpp"

// A client's connection over the in-page set, driven without ever blocking: the engine worker has
// no thread to spare, so it moves bytes on each wake instead (docs/internals/wasm.md). Not public
// API.
namespace order_book::wasm {

    class PageConnection {
    public:
        // Connects, writing Hello and these commands as room allows. Nothing if there is no set,
        // with no engine running, or the set takes no connection, as once its listener has closed.
        static std::optional<PageConnection> open(std::shared_ptr<network::PageSocketSet> sockets,
                                                   std::vector<network::ClientCommand> commands = {});

        PageConnection(PageConnection&& other) noexcept;
        PageConnection& operator=(PageConnection&&) = delete;
        PageConnection(const PageConnection&) = delete;
        PageConnection& operator=(const PageConnection&) = delete;
        // Resets a connection still open, and lets the set reuse its slot.
        ~PageConnection();

        // One more command, on a connection that stays open.
        void add(network::ClientCommand command) { client.add(std::move(command)); }
        // Half-closes once every command is written: no more will come.
        void finish() noexcept { finishing = true; }

        // Moves what it can both ways: writes what fits, half-closes if finishing, reads what
        // came, and ends the stream once the connection has ended and nothing is left. Call it on
        // every wake; it never waits. It reads at most mostToReceive bytes, which a credit window
        // limits, and gives how many it read.
        std::size_t pump(std::size_t mostToReceive = static_cast<std::size_t>(-1));

        const network::ClientStream& stream() const noexcept { return client; }
        // Every byte read on this connection so far.
        std::size_t received() const noexcept { return receivedBytes; }
        network::ClientStream& stream() noexcept { return client; }
        // The stream ended or broke: nothing more will come.
        bool done() const noexcept { return !client.reading(); }

    private:
        PageConnection(std::shared_ptr<network::PageSocketSet> sockets, network::SocketId id, std::vector<network::ClientCommand> commands);

        std::shared_ptr<network::PageSocketSet> set;  // held, so a run's set outlives its connections
        network::SocketId socket;
        network::ClientStream client;
        bool finishing = false;
        bool halfClosed = false;
        bool released = false;
        std::size_t receivedBytes = 0;
    };

    // `order_book_cli send` of a prepared file over the in-page set, as the terminal's `send` and
    // the seed use it: every command, a half-close, and every answer to the end of the stream.
    class PageSend {
    public:
        // Nothing if there is no set or it takes no connection.
        static std::optional<PageSend> start(std::shared_ptr<network::PageSocketSet> sockets, cli::PreparedSend prepared);

        // On each wake: send's result once the stream has ended, otherwise nothing yet.
        std::optional<cli::CommandResult> pump(std::size_t mostToReceive = static_cast<std::size_t>(-1));
        std::size_t received() const noexcept { return connection.received(); }
        // The run has ended: what hasn't come never will, so the next pump gives the result.
        void abandon() { connection.stream().ended(); }

    private:
        PageSend(cli::PreparedSend prepared, PageConnection connection);

        cli::PreparedSend session;
        PageConnection connection;
    };

}  // namespace order_book::wasm
