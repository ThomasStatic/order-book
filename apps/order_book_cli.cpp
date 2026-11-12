#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include "order_book/book_event.hpp"
#include "order_book/cli/commands.hpp"
#include "order_book/command_log.hpp"
#include "order_book/event_format.hpp"
#include "order_book/network/client_stream.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_inputs.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/offline.hpp"
#include "order_book/symbol.hpp"

namespace {
    namespace replay = order_book::replay;

    namespace cli = order_book::cli;
    using cli::engineFailure;
    using cli::inputError;

    // Writes what a command printed, each stream to its own, and gives its exit status.
    int emit(const cli::CommandResult& result) {
        std::cout << result.out;
        std::cout.flush();
        std::cerr << result.err;
        return result.exit;
    }

    constexpr std::string_view helpText =
        "Replays a CSV command file (docs/replay-format.md) or a binary command log\n"
        "(docs/command-log-format.md), told apart by its first bytes, on an empty order book and\n"
        "prints replay statistics. Never prompts: everything is set on the command line.\n"
        "\n"
        "commands:\n"
        "  replay <path>...   replay the input, then print its statistics; a directory means every\n"
        "                     log file in it\n"
        "  convert <path>...  turn a CSV into a log, or a log into a v3 CSV; several inputs or a\n"
        "                     directory make --out a directory of commands-<symbol>.csv\n"
        "  send <file>        send the file's commands to a running order_book_engine and print\n"
        "                     its answers as replay --events --no-statistics prints the file\n"
        "  checkpoint describe <file>\n"
        "                     print a checkpoint file's two headers (docs/checkpoint-format.md),\n"
        "                     checking nothing past its first bytes\n"
        "  checkpoint verify <file>\n"
        "                     check a checkpoint file's checksum, then rebuild its book from the\n"
        "                     image; print ok, or the first fault's byte offset, code, and message\n"
        "  checkpoint roll    roll an engine's checkpoints forward over its closed log files, as its\n"
        "                     workers do, with no engine running; print the files and books rolled\n"
        "  verify             what an engine's start would make of its log and checkpoints, changing\n"
        "                     nothing: a line per shard (where its replay starts, its log ends, and\n"
        "                     what a start would drop) and per book, then ok\n"
        "  cut-log            move a shard's record at a position, and every one after it, to a\n"
        "                     cut- file beside the log, keeping every byte, so a start passes them\n"
        "  demo               print the events of a short scripted session\n"
        "  help               print this help\n"
        "\n"
        "replay options, in any order before or after the file:\n"
        "  --trades           print each trade, tagged with its command's line, before the statistics\n"
        "  --events           print every event instead, tagged with its command's line; this\n"
        "                     includes the trades\n"
        "  --book             print the final book after the statistics, best price first, then\n"
        "                     the last trade price\n"
        "  --validate MODE    check the book's invariants: none, final (the default), or\n"
        "                     per-command (after every command; slower)\n"
        "  --symbol SYM       let a v1 or v2 file stand in for this one instrument; a file that\n"
        "                     names its own symbols refuses it\n"
        "  --no-statistics    leave out the statistics\n"
        "  --                 treat the next argument as the file, even if it starts with '-'\n"
        "  -h, --help         print this help and exit\n"
        "\n"
        "verify, checkpoint roll, and cut-log options, each given once, with no engine using the\n"
        "directories, whose locks they take:\n"
        "  --log-dir DIR         the engine's log directory; required\n"
        "  --checkpoint-dir DIR  its checkpoint directory, if not the log directory\n"
        "  --validate MODE       verify only: check each book once replayed, final (the default),\n"
        "                        or after every command, per-command, which names a poison command\n"
        "  --shard S, --position P\n"
        "                        cut-log only, required: the shard, and the log position of the\n"
        "                        first record to move, as recovery names it\n"
        "\n"
        "send options, in any order before or after the file:\n"
        "  --connect ADDR:PORT  the engine's IPv4 address and port; required\n"
        "  --symbol SYM         as for replay, and required for a v1 or v2 file\n"
        "  --                   as for replay\n"
        "\n"
        "exit status:\n"
        "  0  replayed to the end, even if the book rejected some commands; or help printed;\n"
        "     for send, every command answered; for checkpoint, the file described or verified,\n"
        "     or the store rolled; for verify, a start would pass, a torn tail it cuts included;\n"
        "     for cut-log, the records moved\n"
        "  1  input error: the file can't be opened or read, or is malformed (what ran before\n"
        "     the error is still printed); for send, also no connection; for checkpoint, not a\n"
        "     checkpoint file, or one verify refuses, or a checkpoint roll couldn't publish; for\n"
        "     verify, a start would stop; for cut-log, refused; and for each store command, a\n"
        "     directory another engine or command is using\n"
        "  2  usage error\n"
        "  3  engine failure: the book threw, or failed an invariant check; for verify, a book\n"
        "     broke, named with its command under --validate per-command; for checkpoint roll, a\n"
        "     rolled book failed its proof; for send, the\n"
        "     connection was lost or the engine sent ProtocolError before every command was\n"
        "     answered, so what ran is unknown (this wins over 1 and 4)\n"
        "  4  send only: every command answered, but at least one was Refused, so that one\n"
        "     certainly did not run and is safe to send again\n"
        "\n"
        "A file that names instruments prints one section per symbol, sorted by symbol bytes,\n"
        "each holding one book's output.\n"
        "\n"
        "examples:\n"
        "  order_book_cli replay orders.csv\n"
        "  order_book_cli replay orders.csv --trades\n"
        "  order_book_cli replay orders.csv --trades --book\n"
        "  order_book_cli replay orders.csv --events --book --validate per-command\n"
        "  order_book_cli send orders.csv --connect 127.0.0.1:9000 --symbol AAPL\n";

    int printHelp() {
        std::cout << cli::usageText() << '\n' << helpText;
        return EXIT_SUCCESS;
    }

    int usage(std::string_view problem) {
        return emit(cli::usage(problem));
    }

    bool isHelpOption(std::string_view arg) {
        return arg == "-h" || arg == "--help";
    }

    // Printing lives here, outside the engine: the book only returns events.
    void printEvents(std::span<const order_book::BookEvent> events) {
        for (const order_book::BookEvent& event : events) {
            std::cout << order_book::formatEvent(event) << '\n';
        }
    }

    int runDemo() {
        using order_book::Order;
        using order_book::Side;

        order_book::OrderBook book;

        printEvents(book.submitOrder(Order(1, 101, 5, 1, Side::SELL)).getEvents());
        printEvents(book.submitOrder(Order(2, 100, 3, 1, Side::SELL)).getEvents());
        printEvents(book.submitOrder(Order(2, 99, 1, 1, Side::BUY)).getEvents());
        printEvents(book.submitOrder(Order(3, 101, 6, 1, Side::BUY)).getEvents());
        printEvents(book.cancelOrder(1).getEvents());
        printEvents(book.cancelOrder(1).getEvents());

        return EXIT_SUCCESS;
    }

    // args are what follows "convert".
    int convertCommand(const std::vector<std::string_view>& args) {
        std::vector<std::filesystem::path> inputs;
        std::optional<std::filesystem::path> out;
        std::optional<replay::Symbol> symbol;
        std::set<std::string_view> given;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (arg.starts_with("--") && !given.insert(arg).second) {
                return usage(cli::repeated(arg));
            }
            if (arg == "--out" || arg == "--symbol") {
                if (i + 1 == args.size()) {
                    return usage(std::string(arg) + " needs a value");
                }
                const std::string_view value = args[++i];
                if (arg == "--out") {
                    out = std::filesystem::path(value);
                } else {
                    symbol = replay::Symbol::parse(value);
                    if (!symbol) {
                        return usage(cli::notASymbol(value));
                    }
                }
            } else if (arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                inputs.emplace_back(arg);
            }
        }
        if (inputs.empty()) {
            return usage("convert needs a file to read");
        }
        if (!out) {
            return usage("convert needs --out");
        }

        const replay::ConvertOutcome outcome = replay::convertReplayFiles(inputs, *out, symbol);
        if (outcome.usage) {
            return usage(*outcome.usage);
        }
        if (outcome.refusal) {
            std::cerr << "order_book_cli: " << *outcome.refusal << '\n';
            return inputError;
        }
        if (outcome.error) {
            std::cerr << replay::formatReplayError(*outcome.error, outcome.errorPath.generic_string()) << '\n';
            return inputError;
        }
        return EXIT_SUCCESS;
    }

    // args are what follows "send". The command decides everything before connecting and prints
    // what the ended stream holds; connecting and the poll loop are this program's.
    int sendCommand(const std::vector<std::string_view>& args) {
        std::variant<cli::PreparedSend, cli::CommandResult> prepared = cli::prepareSend(args);
        if (const auto* refused = std::get_if<cli::CommandResult>(&prepared)) {
            return emit(*refused);
        }
        cli::PreparedSend& send = std::get<cli::PreparedSend>(prepared);

        order_book::network::TcpSocket socket;
        try {
            socket = order_book::network::TcpSocket::connectIpv4(send.address.host, send.address.port);
        } catch (const std::system_error& error) {
            return emit(cli::cannotConnect(send, error.what()));
        }
        order_book::network::ClientStream stream(std::move(send.commands));
        std::string earlier;
        try {
            order_book::network::exchange(stream, socket);
        } catch (const std::system_error& error) {
            // poll failed: the connection is over, and what came still prints.
            earlier = "order_book_cli: " + std::string(error.what()) + '\n';
            stream.ended();
        }
        return emit(cli::finishSend(send, stream, std::move(earlier)));
    }

    // The whole file, or nothing if it can't be opened or read.
    std::optional<std::vector<std::byte>> readWholeFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad()) {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(text.size());
        if (!text.empty()) {  // an empty vector's data() may be null, which memcpy must not get
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return bytes;
    }

    // args are what follows "checkpoint roll".
    int checkpointRollCommand(const std::vector<std::string_view>& args) {
        namespace runtime = order_book::runtime;
        constexpr std::array<std::string_view, 1> allowed{"--checkpoint-dir"};
        std::variant<cli::StoreFlags, cli::CommandResult> parsed = cli::parseStoreFlags("checkpoint roll", args, allowed);
        if (const auto* refused = std::get_if<cli::CommandResult>(&parsed)) {
            return emit(*refused);
        }
        std::variant<cli::Stores, cli::CommandResult> opened = cli::openStores(std::get<cli::StoreFlags>(parsed));
        if (const auto* refused = std::get_if<cli::CommandResult>(&opened)) {
            return emit(*refused);
        }
        cli::Stores& stores = std::get<cli::Stores>(opened);
        const runtime::StoreRoll rolled = runtime::rollStore(*stores.logs, stores.checkpointStore());
        for (const std::string& line : rolled.reported) {
            std::cerr << "order_book_cli: checkpoint roll: " << line << '\n';
        }
        if (rolled.outcome != runtime::RollOutcome::OK) {
            std::cerr << "order_book_cli: checkpoint roll: " << rolled.failure << '\n';
            return rolled.outcome == runtime::RollOutcome::PROOF_FAILED ? engineFailure : inputError;
        }
        std::cout << "rolled files=" << rolled.files << " books=" << rolled.books << '\n';
        return EXIT_SUCCESS;
    }

    // A whole number, digits only, as the engine's flags take them.
    std::optional<std::uint64_t> parseCount(std::string_view text, std::uint64_t most) {
        if (text.empty()) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (const char digit : text) {
            if (digit < '0' || digit > '9' || value > (most - static_cast<std::uint64_t>(digit - '0')) / 10) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint64_t>(digit - '0');
        }
        return value;
    }

    // args are what follows "cut-log".
    int cutLogCommand(const std::vector<std::string_view>& args) {
        namespace runtime = order_book::runtime;
        constexpr std::array<std::string_view, 3> allowed{"--checkpoint-dir", "--shard", "--position"};
        std::variant<cli::StoreFlags, cli::CommandResult> parsed = cli::parseStoreFlags("cut-log", args, allowed);
        if (const auto* refused = std::get_if<cli::CommandResult>(&parsed)) {
            return emit(*refused);
        }
        const cli::StoreFlags& flags = std::get<cli::StoreFlags>(parsed);
        for (const char* needed : {"--shard", "--position"}) {
            if (!flags.values.contains(needed)) {
                return usage(std::string("cut-log needs ") + needed + (std::string_view(needed) == "--shard" ? " S" : " P"));
            }
        }
        const std::optional<std::uint64_t> shard = parseCount(flags.values.at("--shard"), 0xFFFFFFFFULL);
        if (!shard) {
            return usage("--shard needs a shard number, not '" + flags.values.at("--shard") + "'");
        }
        const std::optional<std::uint64_t> position = parseCount(flags.values.at("--position"), UINT64_MAX);
        if (!position) {
            return usage("--position needs a log position, not '" + flags.values.at("--position") + "'");
        }
        std::variant<cli::Stores, cli::CommandResult> opened = cli::openStores(flags);
        if (const auto* refused = std::get_if<cli::CommandResult>(&opened)) {
            return emit(*refused);
        }
        cli::Stores& stores = std::get<cli::Stores>(opened);
        const runtime::LogCut cut = runtime::cutLog(*stores.logs, stores.checkpointStore(), static_cast<std::uint32_t>(*shard), *position);
        if (!cut.cut) {
            std::cerr << "order_book_cli: cut-log: " << cut.failure << '\n';
            return inputError;
        }
        std::cout << "moved records=" << cut.records << " shard=" << *shard << " to " << cut.file << '\n';
        return EXIT_SUCCESS;
    }

    // args are what follows "checkpoint". describe and verify read one file whole, with no lock:
    // a published checkpoint never changes. roll works on a whole store, locked.
    int checkpointCommand(const std::vector<std::string_view>& args) {
        namespace runtime = order_book::runtime;
        if (args.empty()) {
            return usage("checkpoint needs a command: describe, verify, or roll");
        }
        const std::string_view command = args[0];
        if (command == "roll") {
            return checkpointRollCommand(std::vector<std::string_view>(args.begin() + 1, args.end()));
        }
        if (command != "describe" && command != "verify") {
            return usage("unknown checkpoint command '" + std::string(command) + "'; expected describe, verify, or roll");
        }
        std::vector<std::string_view> files;
        bool optionsEnded = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (!optionsEnded && arg == "--") {
                optionsEnded = true;
            } else if (!optionsEnded && arg.starts_with('-')) {
                return usage("unknown option '" + std::string(arg) + "'");
            } else {
                files.push_back(arg);
            }
        }
        const std::string name = "checkpoint " + std::string(command);
        if (files.empty()) {
            return usage(name + " needs a file to read");
        }
        if (files.size() > 1) {
            return usage(name + " takes one file, not " + std::to_string(files.size()));
        }

        const std::string path(files[0]);
        const std::optional<std::vector<std::byte>> file = readWholeFile(path);
        if (!file) {
            std::cerr << path << ": cannot open checkpoint file\n";
            return inputError;
        }
        if (command == "describe") {
            const auto described = runtime::describeCheckpoint(*file);
            if (const auto* error = std::get_if<runtime::CheckpointError>(&described)) {
                std::cerr << runtime::formatCheckpointFault(path, {error->code, error->offset, error->message}) << '\n';
                return inputError;
            }
            std::cout << runtime::formatCheckpointDescription(std::get<runtime::CheckpointDescription>(described));
            return EXIT_SUCCESS;
        }
        if (const std::optional<runtime::CheckpointFault> fault = runtime::verifyCheckpoint(*file)) {
            std::cerr << runtime::formatCheckpointFault(path, *fault) << '\n';
            return inputError;
        }
        std::cout << "ok\n";
        return EXIT_SUCCESS;
    }
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Write '\n' as is, not as CRLF, so a replay prints the same bytes on every platform.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage("missing command");
    }
    // A help option anywhere before "--" wins over everything else on the line.
    for (const std::string_view arg : args) {
        if (arg == "--") {
            break;
        }
        if (isHelpOption(arg)) {
            return printHelp();
        }
    }

    const std::string_view command = args[0];
    const std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (command == "replay") {
        return emit(cli::replay(rest));
    }
    if (command == "convert") {
        return convertCommand(rest);
    }
    if (command == "send") {
        return sendCommand(rest);
    }
    if (command == "checkpoint") {
        return checkpointCommand(rest);
    }
    if (command == "verify") {
        return emit(cli::verify(rest));
    }
    if (command == "cut-log") {
        return cutLogCommand(rest);
    }
    if (command == "demo" || command == "help") {
        if (!rest.empty()) {
            return usage("unexpected argument '" + std::string(rest[0]) + "'; "
                         + std::string(command) + " takes none");
        }
        return command == "demo" ? runDemo() : printHelp();
    }
    return usage("unknown command '" + std::string(command) + "'");
}
