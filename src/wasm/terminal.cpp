#include "order_book/wasm/terminal.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/replay_summary.hpp"

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

    }  // namespace

    std::variant<PageSend, cli::CommandResult> startSend(std::shared_ptr<network::PageSocketSet> sockets, std::span<const std::string_view> args) {
        std::variant<cli::PreparedSend, cli::CommandResult> prepared = cli::prepareSend(args);
        if (auto* refused = std::get_if<cli::CommandResult>(&prepared)) {
            return std::move(*refused);
        }
        cli::PreparedSend& session = std::get<cli::PreparedSend>(prepared);
        const cli::PreparedSend kept = session;
        if (std::optional<PageSend> started = PageSend::start(std::move(sockets), std::move(session))) {
            return std::move(*started);
        }
        return cli::cannotConnect(kept, "Connection refused");
    }

    void Terminal::connect(std::shared_ptr<network::PageSocketSet> sockets) {
        if (waiting) {
            throw std::logic_error("the terminal connects again while a command waits");
        }
        standing.reset();
        if (std::optional<PageConnection> opened = PageConnection::open(std::move(sockets))) {
            standing.emplace(std::move(*opened));
        }
    }

    void Terminal::disconnect() {
        if (!waiting) {
            standing.reset();
        }
        // A waiting command keeps its connection until pump reports it lost.
        else if (standing) {
            standing->stream().ended();
        }
    }

    std::optional<cli::CommandResult> Terminal::run(std::string_view line) {
        const std::vector<std::string_view> words = wordsOf(line);
        if (words.empty()) {
            return cli::CommandResult{};
        }
        const std::string_view verb = words.front();
        if (waiting) {
            return failed(verb, "the last command is still waiting for its answer");
        }
        if (verb == "book") {
            if (words.size() != 2) {
                return failed(verb, "usage: book SYMBOL");
            }
            return book(words[1]);
        }
        if (verb == "status") {
            if (words.size() != 1) {
                return failed(verb, "usage: status");
            }
            return status();
        }

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
            if (const std::optional<std::string> problem = takeOptions(std::span(words).subspan(options), row, stop)) {
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
            if (const std::optional<std::string> problem = takeOptions(std::span(words).subspan(5), row, stop)) {
                return failed(verb, *problem);
            }
        } else {
            return failed(verb, "unknown command");
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
