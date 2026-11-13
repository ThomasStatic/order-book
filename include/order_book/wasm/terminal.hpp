#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "order_book/cli/commands.hpp"
#include "order_book/network/page_socket_set.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/wasm/page_connection.hpp"
#include "order_book/wasm/settings.hpp"
#include "order_book/wasm/shadow_books.hpp"

// The browser terminal's grammar and printing behind run(line) (spec #127, "order_book_wasm";
// docs/internals/wasm.md). Not public API.
namespace order_book::wasm {

    // `order_book_cli send` of a file over the in-page set, its arguments as send takes them:
    // what prepareSend refuses, cannotConnect's text if the set takes no connection, or the send
    // under way.
    std::variant<PageSend, cli::CommandResult> startSend(std::shared_ptr<network::PageSocketSet> sockets, std::span<const std::string_view> args);

    // The v3 replay header, the first line of what Terminal::collected gives.
    inline constexpr std::string_view replayHeader =
        "sequence,command,order_id,side,price,quantity,order_type,time_in_force,stop_price,post_only,owner,self_trade_prevention,symbol";

    // A client command is one v3 replay row on the terminal's standing connection. Its
    // correlation is its line in collected(), the header being line 1, so it prints what send of
    // that CSV prints for the line, under the same `line N:`, and a session's client commands,
    // collected, are a CSV whose native send prints the same answers. buy and sell take an ID from
    // the shadows (ShadowBooks::assignOrderId), print it, then the answers.
    //
    //   buy|sell SYMBOL QUANTITY [PRICE] [stop=PRICE] [tif=gtc|ioc|fok] [post-only] [owner=N]
    //            [stp=cancel-resting|cancel-incoming|cancel-both]
    //   cancel SYMBOL ID
    //   replace SYMBOL ID PRICE QUANTITY [post-only] [owner=N] [stp=...]
    //   book SYMBOL      what replay --book prints for a final book, from the shadow
    //   status           a line per shard: its instruments and how many records its log holds
    //
    // A price makes a LIMIT order, none a MARKET order; stop= makes them STOP_LIMIT and STOP.
    // The time in force is GTC for a LIMIT or STOP_LIMIT order and IOC otherwise, unless given;
    // an owner with no stp= is CANCEL_RESTING. The row is then checked exactly as a replay row,
    // so a bad one prints the replay parser's column and message, exiting 2. A symbol off the
    // instrument list is refused before it takes a line.
    //
    // The CLI's own commands, send and replay printing exactly what order_book_cli prints and
    // verify what it prints of a store, with its own argument errors, since it takes no
    // directories; and the sample
    // files, named from the working directory, which is the samples' root (valid/ and
    // malformed/, as in tests/replay), so a path prints as typed:
    //
    //   send FILE [--symbol SYMBOL] [--connect ADDR]   on a connection of its own, to its end
    //   replay FILE... [OPTIONS]                       on fresh books, not the engine's
    //   verify [--validate final|per-command]          of the page store; refused while the
    //                                                  engine runs, since the engine holds its lock
    //   ls [DIRECTORY]   a sample directory's entries, a directory's with a trailing /
    //   cat FILE         a sample file's bytes
    //   help             every command, the page's own (start, stop, crash, reset, clear) too
    //
    // send, replay, and verify may keep the program name, as a recorded session types them
    // (`order_book_cli replay valid/stops.csv`). Anything else prints `<word>: ...` and exits 2.
    class Terminal {
    public:
        Terminal(ShadowBooks& books, runtime::LogStorage& pageStore, std::size_t window = creditWindow)
            : shadows(books), store(pageStore), windowBytes(window) {}

        // The engine is ready: the standing connection opens on this run's set, in place of any
        // earlier one, and send connects there. Nothing opens if the set takes no connection.
        // Call pump first to collect a command a disconnect lost: none may be waiting.
        void connect(std::shared_ptr<network::PageSocketSet> set);
        // The run has ended, so its connection is gone; a command waiting on it is lost, and
        // pump says so, as it does a send under way, reported as send reports a connection lost.
        void disconnect();

        // One typed line: its result now, or nothing while it waits on the engine, when pump on
        // each wake gives the result once the answer has come. A line while one waits is refused.
        std::optional<cli::CommandResult> run(std::string_view line);
        std::optional<cli::CommandResult> pump();

        // The terminal's credit window, over its standing connection and its sends: it reads only
        // while the bytes read and not yet consumed are under W. progress gives the bytes read
        // since it last asked, which the engine worker posts for the page to acknowledge with
        // consumed, so a page too busy to take its messages stops the reading, and the gateway
        // then holds the rest, as a slow reader of a TCP connection would. send's output is whole
        // only at the end, sorted by symbol, so the window counts bytes, not lines.
        std::size_t progress() noexcept;
        void consumed(std::size_t bytes) noexcept;
        std::size_t room() const noexcept;

        // The session's client commands so far, as a v3 CSV: the header, then a row each.
        std::string collected() const { return std::string(replayHeader) + '\n' + rows; }

    private:
        struct Waiting {
            std::string verb;
            network::Correlation correlation = 0;
            std::string assigned;  // "assigned id=N\n" for a buy or sell
        };

        std::optional<cli::CommandResult> clientCommand(std::string_view verb, std::span<const std::string_view> words);
        std::optional<cli::CommandResult> send(std::string_view verb, const replay::ReplayRecord& record, std::string assigned);
        std::optional<cli::CommandResult> answer();
        std::optional<cli::CommandResult> sendFile(std::span<const std::string_view> args);
        std::optional<cli::CommandResult> sent();
        cli::CommandResult book(std::string_view symbol) const;
        cli::CommandResult status() const;
        cli::CommandResult verify(std::span<const std::string_view> args);
        cli::CommandResult help() const;
        void pumpStanding();
        std::optional<cli::CommandResult> pumpSending();

        ShadowBooks& shadows;
        runtime::LogStorage& store;
        std::shared_ptr<network::PageSocketSet> sockets;  // this run's, while it runs
        std::optional<PageConnection> standing;
        std::optional<PageSend> sending;
        std::size_t windowBytes;
        std::size_t read = 0;          // bytes read on every connection the terminal has had
        std::size_t announced = 0;     // of those, what progress has given
        std::size_t acknowledged = 0;  // of those, what the page has consumed
        std::size_t commands = 0;  // client commands sent so far
        std::string rows;
        std::optional<Waiting> waiting;
    };

}  // namespace order_book::wasm
