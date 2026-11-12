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
#include "order_book/wasm/page_connection.hpp"
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
    // so a bad one prints the replay parser's column and message, exiting 2.
    class Terminal {
    public:
        explicit Terminal(ShadowBooks& books) : shadows(books) {}

        // The engine is ready: the standing connection opens on this run's set, in place of any
        // earlier one. Nothing opens if the set takes no connection.
        // Call pump first to collect a command a disconnect lost: none may be waiting.
        void connect(std::shared_ptr<network::PageSocketSet> sockets);
        // The run has ended, so its connection is gone; a command waiting on it is lost, and
        // pump says so.
        void disconnect();

        // One typed line: its result now, or nothing while it waits on the engine, when pump on
        // each wake gives the result once the answer has come. A line while one waits is refused.
        std::optional<cli::CommandResult> run(std::string_view line);
        std::optional<cli::CommandResult> pump();

        // The session's client commands so far, as a v3 CSV: the header, then a row each.
        std::string collected() const { return std::string(replayHeader) + '\n' + rows; }

    private:
        struct Waiting {
            std::string verb;
            network::Correlation correlation = 0;
            std::string assigned;  // "assigned id=N\n" for a buy or sell
        };

        std::optional<cli::CommandResult> send(std::string_view verb, const replay::ReplayRecord& record, std::string assigned);
        std::optional<cli::CommandResult> answer();
        cli::CommandResult book(std::string_view symbol) const;
        cli::CommandResult status() const;

        ShadowBooks& shadows;
        std::optional<PageConnection> standing;
        std::size_t commands = 0;  // client commands sent so far
        std::string rows;
        std::optional<Waiting> waiting;
    };

}  // namespace order_book::wasm
