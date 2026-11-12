#pragma once

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/network/client_stream.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/log_storage.hpp"

// order_book_cli_commands (docs/internals/replay.md): the CLI's send, replay, and verify, which
// the native CLI and the browser's terminal both run. Each takes the arguments that follow its
// name and returns what it printed and its exit status rather than printing. Not public API.
namespace order_book::cli {

    // What a command printed on each stream, and its exit status.
    struct CommandResult {
        std::string out;
        std::string err;
        int exit = 0;
    };

    // Exit statuses: a file that replays to the end exits 0, even when the book rejected some
    // commands, and so does --help.
    constexpr int inputError = 1;     // the file can't be opened or read, or is malformed
    constexpr int usageError = 2;
    constexpr int engineFailure = 3;  // the book threw, or failed an invariant check
    // send's own: 1 is also no connection, and 3 a connection lost before every answer came.
    constexpr int connectionLost = 3;
    constexpr int someRefused = 4;  // every command answered, some refused: safe to send again

    // The CLI's usage lines, which every usage error prints after its problem.
    std::string_view usageText();

    // One line saying what was wrong, then the usage, on stderr, exiting 2.
    CommandResult usage(std::string_view problem);
    std::string repeated(std::string_view option);
    std::string notASymbol(std::string_view text);

    CommandResult replay(std::span<const std::string_view> args);

    // send, split so the caller drives the connection: prepareSend decides everything before
    // connecting, then the caller connects and runs a ClientStream of the prepared commands to
    // its end, natively over TCP's poll loop. cannotConnect is what a connection that never
    // opened prints, and finishSend what the ended stream does; earlier is stderr's text from
    // the connection itself, printed before the stream's own diagnostics.
    struct PreparedSend {
        std::string path;
        replay::ReplayInput input;
        runtime::ListenAddress address;
        std::vector<network::ClientCommand> commands;
    };
    std::variant<PreparedSend, CommandResult> prepareSend(std::span<const std::string_view> args);
    CommandResult cannotConnect(const PreparedSend& send, std::string_view reason);
    CommandResult finishSend(const PreparedSend& send, const network::ClientStream& stream, std::string earlier = {});

    // A store command's flags, each `--flag value` and given once: --log-dir always, and those
    // in allowed. Nothing else, and no file.
    struct StoreFlags {
        std::map<std::string, std::string, std::less<>> values;
    };
    std::variant<StoreFlags, CommandResult> parseStoreFlags(std::string_view command, std::span<const std::string_view> args,
                                                            std::span<const std::string_view> allowed);

    // The store's two directories, the checkpoints' the log's unless --checkpoint-dir names
    // another.
    struct Stores {
        std::unique_ptr<runtime::FileLogStorage> logs;
        std::unique_ptr<runtime::FileLogStorage> checkpoints;  // null when the log's
        runtime::LogStorage& checkpointStore() { return checkpoints ? *checkpoints : *logs; }
    };
    std::variant<Stores, CommandResult> openStores(const StoreFlags& flags);

    // verify on directories, as the CLI takes them, and verifyStores on any storage, which is how
    // the browser verifies its page store.
    CommandResult verify(std::span<const std::string_view> args);
    CommandResult verifyStores(runtime::LogStorage& logs, runtime::LogStorage& checkpoints, bool perCommand);

}  // namespace order_book::cli
