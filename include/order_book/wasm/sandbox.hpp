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

    // An order as the page gives it: AddCommand without its ID, each field the text its replay
    // column takes, so the parser judges a number exactly as it judges a row's (a fraction, a
    // negative, or zero refused with its own message). What is left empty takes the terminal's
    // defaults: LIMIT with a price and MARKET without, or STOP_LIMIT and STOP with a stop price;
    // GTC with a price and IOC without; and CANCEL_RESTING for an owner with no mode.
    struct OrderInput {
        std::string side;  // BUY or SELL
        std::string quantity;
        std::string price;
        std::string type;  // LIMIT, MARKET, STOP, or STOP_LIMIT
        std::string stopPrice;
        std::string timeInForce;  // GTC, IOC, or FOK
        bool postOnly = false;
        std::string owner;
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
        std::variant<SandboxSent, std::string> cancel(std::string_view symbol, std::string_view orderId);
        std::variant<SandboxSent, std::string> replace(std::string_view symbol, std::string_view orderId, std::string_view price,
                                                       std::string_view quantity);

        // On each wake: sends what waits, reads what came while the window has room, and gives
        // each command whose answer is now whole, in the order the commands went, or each
        // command lost if the connection ended.
        std::vector<SandboxAnswer> pump();
        // The page has consumed this many of the frame bytes it was given.
        void consumed(std::size_t bytes) noexcept;
        // What the window lets the next pump read: W, less the bytes read and not yet consumed,
        // whether given or still part of an answer under way. When everything given has been
        // consumed and the window is still full, an answer larger than W is under way, and the
        // next pump reads another W of it, the one way the page can hold more than W.
        std::size_t room() const noexcept;

    private:
        std::variant<SandboxSent, std::string> send(std::vector<std::string> row);
        std::vector<SandboxAnswer> loseAll(const std::string& why);

        ShadowBooks& shadows;
        std::size_t windowBytes;
        std::optional<PageConnection> standing;
        std::size_t commands = 0;  // sent on this connection, its correlations 1 to commands
        std::map<network::Correlation, std::optional<OrderId>> waiting;
        std::size_t read = 0;          // bytes read on this connection, the Welcome first
        std::size_t given = 0;         // of those, the frames of answers given
        std::size_t acknowledged = 0;  // of those, what the page has consumed
    };

}  // namespace order_book::wasm
