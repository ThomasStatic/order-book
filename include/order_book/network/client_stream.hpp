#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/symbol.hpp"

// A client's side of one connection, as `order_book_cli send` uses it: bytes in, bytes out, no
// sockets, so a test can play the engine. See docs/internals/network.md.
namespace order_book::network {

    class TcpSocket;

    // One command to send. Its correlation is its line in the file, which is how its answers
    // print, so they read as replay's do.
    struct ClientCommand {
        Correlation correlation;
        Symbol symbol;
        replay::Command command;
    };

    // How a connection ended, from what a script may do next.
    enum class ConnectionEnd {
        ANSWERED,  // every command answered, even if a book rejected some
        REFUSED,   // every command answered, but at least one Refused: those certainly did not run
        LOST,      // the stream ended or broke before every answer, a ProtocolError among them: what ran is unknown
    };

    // Writes Hello, then each command in the order given, and reads the engine's frames in any
    // chunking. Responses are kept until the end, then printed as replay prints them. A command
    // is answered once a frame flagged last comes under its correlation and its symbol.
    class ClientStream {
    public:
        // Correlations must differ from each other and from 0, which Hello takes.
        explicit ClientStream(std::vector<ClientCommand> commands);

        // The bytes to write next, empty once every command is written. Good until wrote.
        std::span<const std::byte> outgoing();
        void wrote(std::size_t bytes);
        // Everything is written, so the caller half-closes.
        bool allWritten() const noexcept;

        // One more command, after those given, for a connection that stays open: the browser
        // terminal's. Its correlation must be above every earlier one's.
        void add(ClientCommand command);

        // What the engine sent.
        void received(std::span<const std::byte> bytes);
        // The engine's stream ended, or the connection failed.
        void ended();
        // Whether to go on reading: false once the stream has ended, or broke on a frame this
        // client can't read, after which nothing more counts.
        bool reading() const noexcept;

        ConnectionEnd end() const noexcept;
        // replay --events --no-statistics's sections: one per symbol answered, sorted by symbol
        // bytes, each line tagged "line N:" with its correlation, sorted by it, and a line's
        // frames in the order they came. A refusal prints in place.
        std::string sections() const;
        // Whether a frame flagged last has come for this correlation's command.
        bool answered(Correlation correlation) const;
        // That command's lines alone, as sections prints them, in the order they came.
        std::string answerLines(Correlation correlation) const;
        // What went wrong, one line each, without a program's prefix.
        const std::vector<std::string>& diagnostics() const noexcept { return problems; }

    private:
        struct Answer {
            Correlation correlation;
            std::size_t command;  // its index in commands
            std::string text;     // formatEvent's, or the refusal's
        };

        static std::string answerLine(const Answer& answer);
        void take(const Frame& frame);
        // The index of the command under this correlation, if one is.
        std::optional<std::size_t> commandOf(Correlation correlation) const;
        void broke(std::string why);
        void fill();

        std::vector<ClientCommand> commands;
        std::vector<std::size_t> byCorrelation;  // command indices, sorted by correlation
        std::vector<bool> isAnswered;            // by command index
        std::size_t unanswered;
        std::vector<Answer> answers;

        std::vector<std::byte> out;
        std::size_t outStart = 0;
        std::size_t nextCommand = 0;
        bool helloQueued = false;

        FrameReader reader{FrameSource::ENGINE};
        bool welcomed = false;
        bool refused = false;
        bool protocolError = false;
        bool broken = false;
        bool streamEnded = false;
        std::vector<std::string> problems;
    };

    // A protocol error's or refusal code's name from docs/wire-protocol.md, or its number for one
    // this release doesn't know.
    std::string protocolErrorName(std::uint16_t code);
    std::string refusedCodeName(std::uint16_t code);

    // Runs stream over a connected socket until the engine's stream ends: one poll loop that
    // reads and writes at once, half-closes once everything is written, and never retries.
    // Throws std::system_error if poll fails.
    void exchange(ClientStream& stream, TcpSocket& socket);

}  // namespace order_book::network
