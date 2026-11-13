#include "order_book/wasm/terminal.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/wasm/settings.hpp"

namespace order_book::wasm {

    namespace {

        // The row's columns, in replayHeader's order.
        enum Column : std::size_t { SEQUENCE, COMMAND, ORDER_ID, SIDE, PRICE, QUANTITY, ORDER_TYPE, TIME_IN_FORCE, STOP_PRICE, POST_ONLY, OWNER, STP, SYMBOL, COLUMNS };

        std::vector<std::string_view> wordsOf(std::string_view line) {
            std::vector<std::string_view> words;
            std::size_t at = 0;
            while (at < line.size()) {
                while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) != 0) {
                    ++at;
                }
                const std::size_t start = at;
                while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) == 0) {
                    ++at;
                }
                if (at > start) {
                    words.push_back(line.substr(start, at - start));
                }
            }
            return words;
        }

        cli::CommandResult failed(std::string_view verb, std::string_view what, int exit = cli::usageError) {
            return cli::CommandResult{"", std::string(verb) + ": " + std::string(what) + "\n", exit};
        }

        // A keyword as the replay format spells it: upper case, with _ for -.
        std::string keyword(std::string_view text) {
            std::string out(text);
            for (char& character : out) {
                character = character == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
            return out;
        }

        bool isOption(std::string_view word) { return word == "post-only" || word.find('=') != std::string_view::npos; }

        // The words from `from` on, as key=value options and the post-only flag, into the row.
        // What a key may hold is the replay parser's to judge. Returns what was wrong, if anything.
        std::optional<std::string> takeOptions(std::span<const std::string_view> words, std::vector<std::string>& row, bool& stop) {
            for (const std::string_view word : words) {
                std::size_t column = 0;
                std::string value;
                if (word == "post-only") {
                    column = POST_ONLY;
                    value = "Y";
                } else if (const std::size_t equals = word.find('='); equals != std::string_view::npos) {
                    const std::string_view key = word.substr(0, equals);
                    const std::string_view given = word.substr(equals + 1);
                    if (key == "stop") {
                        column = STOP_PRICE;
                        stop = true;
                        value = std::string(given);
                    } else if (key == "tif") {
                        column = TIME_IN_FORCE;
                        value = keyword(given);
                    } else if (key == "owner") {
                        column = OWNER;
                        value = std::string(given);
                    } else if (key == "stp") {
                        column = STP;
                        value = keyword(given);
                    } else {
                        return "unknown option " + std::string(word);
                    }
                    if (value.empty()) {
                        return std::string(word) + " needs a value";
                    }
                } else {
                    return "unexpected " + std::string(word);
                }
                if (!row[column].empty()) {
                    return std::string(word) + " is given twice";
                }
                row[column] = std::move(value);
            }
            return std::nullopt;
        }

        const char* const orderUsage = "usage: buy|sell SYMBOL QUANTITY [PRICE] [stop=PRICE] [tif=gtc|ioc|fok] [post-only] [owner=N] [stp=MODE]";
        const char* const replaceUsage = "usage: replace SYMBOL ID PRICE QUANTITY [post-only] [owner=N] [stp=MODE]";
        const char* const verifyUsage = "usage: verify [--validate final|per-command]";

        constexpr std::string_view helpText =
            "commands, all run in this tab:\n"
            "  buy|sell SYMBOL QUANTITY [PRICE] [stop=PRICE] [tif=gtc|ioc|fok] [post-only] [owner=N] [stp=MODE]\n"
            "                     an order, LIMIT with a price and MARKET without, a stop with stop=; prints its ID\n"
            "  cancel SYMBOL ID\n"
            "  replace SYMBOL ID PRICE QUANTITY [post-only] [owner=N] [stp=MODE]\n"
            "  book SYMBOL        the book, as replay --book prints it\n"
            "  status             each shard's instruments, and how many records its log holds\n"
            "  send FILE [--symbol SYMBOL]\n"
            "                     order_book_cli send: a sample's commands to the running engine\n"
            "  replay FILE... [--trades] [--events] [--book] [--symbol SYMBOL] [--no-statistics]\n"
            "                     order_book_cli replay: a sample on fresh books, not the running engine\n"
            "  verify [--validate final|per-command]\n"
            "                     order_book_cli verify: what a start would recover from the log; not while it runs\n"
            "  ls [DIRECTORY]     the sample files, in valid/ and malformed/\n"
            "  cat FILE           a sample file\n"
            "  start              start the engine, recovering what its log holds\n"
            "  stop               stop it cleanly; a second stop forces it\n"
            "  crash              kill it without warning; it stays down until start\n"
            "  reset              wipe its log and start again from the seed\n"
            "  clear | help       clear the screen, or print this\n"
            "MODE is cancel-resting, cancel-incoming, or cancel-both.\n";

        bool isCliCommand(std::string_view word) { return word == "send" || word == "replay" || word == "verify"; }

        bool isClientCommand(std::string_view word) { return word == "buy" || word == "sell" || word == "cancel" || word == "replace"; }

        // A path under the samples' root, which is the working directory: relative, with no ..
        bool isSamplePath(const std::filesystem::path& path) {
            if (path.empty() || path.has_root_name() || path.has_root_directory()) {
                return false;
            }
            return std::none_of(path.begin(), path.end(), [](const std::filesystem::path& part) { return part == ".."; });
        }

        cli::CommandResult listSamples(std::span<const std::string_view> args) {
            if (args.size() > 1) {
                return failed("ls", "usage: ls [DIRECTORY]");
            }
            const std::string typed = args.empty() ? std::string(".") : std::string(args.front());
            const cli::CommandResult missing = failed("ls", "no such sample directory: " + typed, cli::inputError);
            const std::filesystem::path directory(typed);
            std::error_code error;
            if (!isSamplePath(directory) || !std::filesystem::is_directory(directory, error)) {
                return missing;
            }
            std::vector<std::string> names;
            for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
                std::error_code kind;
                names.push_back(entry->path().filename().string() + (entry->is_directory(kind) ? "/" : ""));
            }
            if (error) {
                return missing;
            }
            std::sort(names.begin(), names.end());
            std::string text;
            for (const std::string& name : names) {
                text += name + "\n";
            }
            return cli::CommandResult{text, "", 0};
        }

        cli::CommandResult showSample(std::span<const std::string_view> args) {
            if (args.size() != 1) {
                return failed("cat", "usage: cat FILE");
            }
            const cli::CommandResult missing = failed("cat", "no such sample file: " + std::string(args.front()), cli::inputError);
            const std::filesystem::path file(args.front());
            std::error_code error;
            if (!isSamplePath(file) || !std::filesystem::is_regular_file(file, error)) {
                return missing;
            }
            std::ifstream in(file, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in.bad()) {
                return missing;
            }
            return cli::CommandResult{std::move(bytes), "", 0};
        }

    }  // namespace

    std::variant<PageSend, cli::CommandResult> startSend(std::shared_ptr<network::PageSocketSet> sockets, std::span<const std::string_view> args) {
        std::variant<cli::PreparedSend, cli::CommandResult> prepared = cli::prepareSend(args);
        if (auto* refused = std::get_if<cli::CommandResult>(&prepared)) {
            return std::move(*refused);
        }
        cli::PreparedSend& session = std::get<cli::PreparedSend>(prepared);
        if (session.address != browserListen) {
            return cli::cannotConnect(session, "Connection refused");  // nothing else is listening in the tab
        }
        const cli::PreparedSend kept = session;
        if (std::optional<PageSend> started = PageSend::start(std::move(sockets), std::move(session))) {
            return std::move(*started);
        }
        return cli::cannotConnect(kept, "Connection refused");
    }

    void Terminal::connect(std::shared_ptr<network::PageSocketSet> set) {
        if (waiting || sending) {
            throw std::logic_error("the terminal connects again while a command waits");
        }
        sockets = set;
        standing.reset();
        if (std::optional<PageConnection> opened = PageConnection::open(std::move(set))) {
            standing.emplace(std::move(*opened));
        }
    }

    void Terminal::disconnect() {
        sockets.reset();
        if (sending) {
            sending->abandon();
        }
        if (!waiting) {
            standing.reset();
        }
        // A waiting command keeps its connection until pump reports it lost.
        else if (standing) {
            standing->stream().ended();
        }
    }

    std::optional<cli::CommandResult> Terminal::run(std::string_view line) {
        std::vector<std::string_view> words = wordsOf(line);
        if (words.empty()) {
            return cli::CommandResult{};
        }
        // As a recorded session types it, with the program's name.
        if (words.size() > 1 && words.front() == "order_book_cli" && isCliCommand(words[1])) {
            words.erase(words.begin());
        }
        const std::string_view verb = words.front();
        if (waiting || sending) {
            return failed(verb, "the last command is still waiting for its answer");
        }
        const std::span<const std::string_view> args = std::span<const std::string_view>(words).subspan(1);
        if (verb == "book") {
            if (args.size() != 1) {
                return failed(verb, "usage: book SYMBOL");
            }
            return book(args.front());
        }
        if (verb == "status" || verb == "help") {
            if (!args.empty()) {
                return failed(verb, "usage: " + std::string(verb));
            }
            return verb == "status" ? status() : help();
        }
        if (verb == "send") {
            return sendFile(args);
        }
        if (verb == "replay") {
            return cli::replay(args);
        }
        if (verb == "verify") {
            return verify(args);
        }
        if (verb == "ls") {
            return listSamples(args);
        }
        if (verb == "cat") {
            return showSample(args);
        }
        if (isClientCommand(verb)) {
            return clientCommand(verb, words);
        }
        return failed(verb, "unknown command; help lists them");
    }

    std::optional<cli::CommandResult> Terminal::clientCommand(std::string_view verb, std::span<const std::string_view> words) {
        std::vector<std::string> row(COLUMNS);
        const std::size_t sequence = commands + 1;
        row[SEQUENCE] = std::to_string(sequence);
        bool stop = false;
        if (verb == "buy" || verb == "sell") {
            if (words.size() < 3) {
                return failed(verb, orderUsage);
            }
            row[COMMAND] = "ADD";
            row[SIDE] = verb == "buy" ? "BUY" : "SELL";
            row[ORDER_ID] = "1";  // a stand-in for the check: the ID is assigned once the row passes
            row[SYMBOL] = std::string(words[1]);
            row[QUANTITY] = std::string(words[2]);
            std::size_t options = 3;
            if (words.size() > 3 && !isOption(words[3])) {
                row[PRICE] = std::string(words[3]);
                options = 4;
            }
            if (const std::optional<std::string> problem = takeOptions(words.subspan(options), row, stop)) {
                return failed(verb, *problem);
            }
            const bool priced = !row[PRICE].empty();
            row[ORDER_TYPE] = stop ? (priced ? "STOP_LIMIT" : "STOP") : (priced ? "LIMIT" : "MARKET");
            if (row[TIME_IN_FORCE].empty()) {
                row[TIME_IN_FORCE] = priced ? "GTC" : "IOC";
            }
        } else if (verb == "cancel") {
            if (words.size() != 3) {
                return failed(verb, "usage: cancel SYMBOL ID");
            }
            row[COMMAND] = "CANCEL";
            row[SYMBOL] = std::string(words[1]);
            row[ORDER_ID] = std::string(words[2]);
        } else if (verb == "replace") {
            if (words.size() < 5) {
                return failed(verb, replaceUsage);
            }
            row[COMMAND] = "REPLACE";
            row[SYMBOL] = std::string(words[1]);
            row[ORDER_ID] = std::string(words[2]);
            row[PRICE] = std::string(words[3]);
            row[QUANTITY] = std::string(words[4]);
            if (const std::optional<std::string> problem = takeOptions(words.subspan(5), row, stop)) {
                return failed(verb, *problem);
            }
        } else {
            throw std::logic_error("not a client command: " + std::string(verb));
        }
        if (!row[OWNER].empty() && row[STP].empty()) {
            row[STP] = "CANCEL_RESTING";
        }

        const replay::ParseResult parsed = replay::parseRow(replay::CsvRow{sequence + 1, std::move(row)}, replay::ReplayFormat::V3,
                                                            sequence > 1 ? std::optional<std::uint64_t>(sequence - 1) : std::nullopt);
        if (const auto* error = std::get_if<replay::ParseError>(&parsed)) {
            return failed(verb, (error->column.empty() ? std::string() : error->column + ": ") + error->message);
        }
        replay::ReplayRecord record = std::get<replay::ReplayRecord>(parsed);
        // The list is fixed, so a symbol off it is refused here for every client command alike,
        // and takes no line.
        if (!shadows.shardOf(record.symbol)) {
            return failed(verb, record.symbol.text() + " is not on the instrument list");
        }
        if (standing) {
            standing->pump();
        }
        if (!standing || standing->done()) {
            standing.reset();
            return failed(verb, "the engine isn't running", cli::inputError);  // as send with no connection
        }
        std::string assigned;
        if (auto* add = std::get_if<replay::AddCommand>(&record.command)) {
            const std::optional<OrderId> id = shadows.assignOrderId(record.symbol);
            if (!id) {
                return failed(verb, "no order ID is left for " + record.symbol.text());
            }
            add->orderId = *id;
            assigned = "assigned id=" + std::to_string(*id) + "\n";
        }
        return send(verb, record, std::move(assigned));
    }

    std::optional<cli::CommandResult> Terminal::send(std::string_view verb, const replay::ReplayRecord& record, std::string assigned) {
        ++commands;
        rows += replay::formatReplayRow(record) + '\n';
        const auto correlation = static_cast<network::Correlation>(record.lineNumber);
        standing->add(network::ClientCommand{correlation, record.symbol, record.command});
        waiting = Waiting{std::string(verb), correlation, std::move(assigned)};
        standing->pump();
        return answer();
    }

    std::optional<cli::CommandResult> Terminal::pump() {
        if (sending) {
            return sent();
        }
        if (standing) {
            standing->pump();
        }
        return answer();
    }

    std::optional<cli::CommandResult> Terminal::answer() {
        if (!waiting) {
            return std::nullopt;
        }
        const network::ClientStream& stream = standing->stream();
        cli::CommandResult result;
        result.out = waiting->assigned + stream.answerLines(waiting->correlation);
        if (stream.answered(waiting->correlation)) {
            const bool refused = result.out.find(": Refused symbol=") != std::string::npos;
            result.exit = refused ? cli::someRefused : 0;
        } else if (standing->done()) {
            result.err = waiting->verb + ": the connection was lost before line " + std::to_string(waiting->correlation) +
                         " was answered: what ran is unknown\n";
            for (const std::string& problem : stream.diagnostics()) {
                result.err += waiting->verb + ": " + problem + "\n";
            }
            result.exit = cli::connectionLost;
            standing.reset();
        } else {
            return std::nullopt;
        }
        waiting.reset();
        return result;
    }

    std::optional<cli::CommandResult> Terminal::sendFile(std::span<const std::string_view> args) {
        std::vector<std::string_view> given(args.begin(), args.end());
        // send needs --connect, and the only address in the tab is the engine's.
        const std::string nominal = runtime::formatListenAddress(browserListen);
        if (std::find(given.begin(), given.end(), "--connect") == given.end()) {
            given.push_back("--connect");
            given.push_back(nominal);
        }
        std::variant<PageSend, cli::CommandResult> started = startSend(sockets, given);
        if (auto* result = std::get_if<cli::CommandResult>(&started)) {
            return std::move(*result);
        }
        sending.emplace(std::move(std::get<PageSend>(started)));
        return sent();
    }

    std::optional<cli::CommandResult> Terminal::sent() {
        std::optional<cli::CommandResult> result = sending->pump();
        if (result) {
            sending.reset();
        }
        return result;
    }

    cli::CommandResult Terminal::verify(std::span<const std::string_view> args) {
        std::optional<std::string_view> mode;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] != "--validate") {
                return failed("verify", "unexpected " + std::string(args[i]) + "; " + verifyUsage);
            }
            if (mode) {
                return failed("verify", "--validate is given twice");
            }
            if (i + 1 == args.size()) {
                return failed("verify", "--validate needs a mode: final or per-command");
            }
            mode = args[++i];
        }
        if (mode && *mode != "final" && *mode != "per-command") {
            return failed("verify", "unknown --validate mode '" + std::string(*mode) + "'; expected final or per-command");
        }
        // The engine, while it runs, holds the store's lock, so this is refused as `... is locked`.
        return cli::verifyStores(store, store, mode == "per-command");
    }

    cli::CommandResult Terminal::help() const {
        std::string text(helpText);
        text += "instruments:";
        const runtime::InstrumentDirectory& directory = shadows.instruments();
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            for (const runtime::Instrument& instrument : directory.onShard(shard)) {
                text += " " + instrument.symbol.text();
            }
        }
        return cli::CommandResult{text + "\n", "", 0};
    }

    cli::CommandResult Terminal::book(std::string_view text) const {
        const std::optional<replay::Symbol> symbol = replay::Symbol::parse(text);
        if (!symbol || !shadows.shardOf(*symbol)) {
            return failed("book", std::string(text) + " is not on the instrument list");
        }
        std::variant<OrderBook, RestoreError> restored = OrderBook::restore(shadows.image(*symbol));
        if (const auto* error = std::get_if<RestoreError>(&restored)) {
            return failed("book", "the shadow's image doesn't restore: " + error->message, cli::engineFailure);
        }
        const OrderBook& book = std::get<OrderBook>(restored);
        replay::ReplayResult result;
        result.finalBook = book.snapshot(std::numeric_limits<unsigned int>::max());
        result.lastTradePrice = book.getLastTradePrice();
        return cli::CommandResult{replay::formatFinalBook(result), "", 0};
    }

    cli::CommandResult Terminal::status() const {
        const runtime::InstrumentDirectory& directory = shadows.instruments();
        std::string text;
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            text += "shard " + std::to_string(shard) + ":";
            for (const runtime::Instrument& instrument : directory.onShard(shard)) {
                text += " " + instrument.symbol.text();
            }
            text += " records=" + std::to_string(shadows.records(shard)) + "\n";
        }
        return cli::CommandResult{text, "", 0};
    }

}  // namespace order_book::wasm
