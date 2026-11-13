#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/network/client_stream.hpp"
#include "order_book/network/page_socket_set.hpp"
#include "order_book/types.hpp"
#include "order_book/wasm/page_connection.hpp"
#include "order_book/wasm/settings.hpp"
#include "order_book/wasm/shadow_books.hpp"

// The sandbox's standing connection (spec #127, "The engine client"; docs/internals/wasm.md):
// typed commands from the page, each one v3 row checked as the terminal's are and encoded here,
// and their answers decoded, read from the in-page set only while the page's credit window has
// room. Not public API.
namespace order_book::wasm {

    // An order as the page gives it: AddCommand without its ID, in the replay format's words.
    // What is left empty takes the terminal's defaults: LIMIT with a price and MARKET without, or
    // STOP_LIMIT and STOP with a stop price; GTC with a price and IOC without; and CANCEL_RESTING
    // for an owner with no mode.
    struct OrderInput {
        std::string side;  // BUY or SELL
        std::uint64_t quantity = 0;
        std::optional<std::uint64_t> price;
        std::string type;  // LIMIT, MARKET, STOP, or STOP_LIMIT
        std::optional<std::uint64_t> stopPrice;
        std::string timeInForce;  // GTC, IOC, or FOK
        bool postOnly = false;
        std::optional<std::uint64_t> owner;
        std::string selfTradePrevention;  // CANCEL_RESTING, CANCEL_INCOMING, or CANCEL_BOTH
    };

    // A command sent: its correlation on the connection, and a submit's assigned ID.
    struct SandboxSent {
        network::Correlation correlation = 0;
        std::optional<OrderId> orderId;
    };

    // A command's whole answer, or its loss.
    struct SandboxAnswer {
        network::Correlation correlation = 0;
        std::optional<OrderId> orderId;
        std::vector<network::ClientStream::Answer> events;  // each frame, decoded, in order
        std::string text;                                    // what send prints for it, `line N:` its correlation
        std::size_t frameBytes = 0;                          // its frames' bytes, which the page acknowledges
        std::string lost;                                    // why no answer will come, if none did
    };

    class Sandbox {
    public:
        explicit Sandbox(ShadowBooks& books, std::size_t window = creditWindow) : shadows(books), windowBytes(window) {}

        // The engine is ready: the standing connection opens on this run's set, in place of any
        // earlier one, and the window starts empty. Nothing opens if the set takes no connection.
        void connect(std::shared_ptr<network::PageSocketSet> sockets);
        // The run has ended: every command still waiting is lost.
        std::vector<SandboxAnswer> disconnect();

        // Each checked as a replay row and refused with the parser's text, a symbol off the
        // instrument list, or "the engine isn't running"; otherwise sent, its answer to come.
        std::variant<SandboxSent, std::string> submit(std::string_view symbol, const OrderInput& order);
        std::variant<SandboxSent, std::string> cancel(std::string_view symbol, std::uint64_t orderId);
        std::variant<SandboxSent, std::string> replace(std::string_view symbol, std::uint64_t orderId, std::uint64_t price,
                                                       std::uint64_t quantity);

        // On each wake: sends what waits, reads what came while the window has room, and gives
        // each command whose answer is now whole, in the order the commands went, or each
        // command lost if the connection ended.
        std::vector<SandboxAnswer> pump();
        // The page has consumed this many of the frame bytes it was given.
        void consumed(std::size_t bytes) noexcept;
        // What the window lets the next pump read: W, less the frame bytes given and not yet
        // consumed. A pump may finish a frame past it, so the page may hold up to W plus one
        // answer.
        std::size_t room() const noexcept;

    private:
        std::variant<SandboxSent, std::string> send(std::vector<std::string> row);
        std::vector<SandboxAnswer> loseAll(const std::string& why);

        ShadowBooks& shadows;
        std::size_t windowBytes;
        std::optional<PageConnection> standing;
        std::size_t commands = 0;  // sent on this connection, its correlations 1 to commands
        std::map<network::Correlation, std::optional<OrderId>> waiting;
        std::size_t given = 0;         // the frames of answers given on this connection
        std::size_t acknowledged = 0;  // of those, what the page has consumed
    };

}  // namespace order_book::wasm
