#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "order_book/cli/commands.hpp"
#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/network/client_stream.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/wasm/browser_engine.hpp"
#include "order_book/wasm/settings.hpp"
#include "order_book/wasm/shadow_books.hpp"
#include "shadow_check.hpp"

// order_book_wasm, natively and under Emscripten: the shipped settings, the book-image decoder,
// the shadow books following a store's changes and rebuilt from it, ID assignment, and a whole
// BrowserEngine run over its page store and in-page connections. See docs/internals/wasm.md.
namespace {

using namespace order_book;
using namespace order_book::wasm;
using Change = runtime::PageLogStorage::Change;
using replay::Symbol;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function>
void expectThrows(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

Symbol symbol(std::string_view text) { return *Symbol::parse(text); }

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "can read " + path.string());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

replay::AddCommand limit(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner = 0) {
    replay::AddCommand add{id, side, price, quantity};
    add.owner = owner;
    return add;
}

std::vector<std::byte> imageOf(const OrderBook& book) {
    std::vector<std::byte> image(book.imageSize());
    image.resize(book.writeImage(image));
    return image;
}

void testTheSettings() {
    const runtime::EngineSettings settings = browserSettings("list.csv");
    expect(settings.instruments == "list.csv" && settings.shards == 2 && settings.gatewayThreads == 1 && settings.rollWorkers == 1,
           "N = 2, M = 1, one roll worker");
    expect(settings.logFileSize == 64 * 1024 && !settings.keepLog, "64 KiB log files, normal retention");
    expect(settings.commandRing == 1024 && settings.responseRing == 4096 && settings.logRing == 4096, "rings 1,024 / 4,096 / 4,096");
    expect(settings.capacityOrders == runtime::defaultBookCapacity.orders && settings.capacityStops == runtime::defaultBookCapacity.stops &&
               settings.capacityLevels == runtime::defaultBookCapacity.levels,
           "native book capacity");
    expect(pageSocketBuffer == 64 * 1024 && creditWindow == 64 * 1024, "64 KiB connection buffers and W = 64 KiB");
    expect(settings.highWaterMark == 64 * 1024 && settings.hardLimit == 1024 * 1024, "slow clients warned at 64 KiB, cut off at 1 MiB");
    expect(settings.maxConnections == 3, "three connections");
    expect(settings.durability == runtime::Durability::PROCESS, "process durability");
    expect(settings.logDir.empty() && settings.checkpointDir.empty(), "the store is injected, so no directory");
    expect(browserEngineThreads == 8, "2N + M + R + housekeeping + runEngine's own thread = 8");
}

// A book with owners, a partial fill, a last trade, and pending stops of each kind, decoded and
// held to the values that built it, with sequence numbers from the book's own queries.
void testDecodingAnImage() {
    OrderBook book;
    for (const replay::AddCommand& add : {limit(1, Side::BUY, 100, 10, 7), limit(2, Side::BUY, 100, 5), limit(3, Side::BUY, 99, 4, 8),
                                          limit(4, Side::SELL, 105, 6, 9), limit(5, Side::SELL, 101, 3), limit(6, Side::SELL, 100, 4)}) {
        static_cast<void>(replay::executeCommand(book, replay::Command{add}));
    }
    replay::AddCommand stop{7, Side::BUY, 0, 2, OrderType::STOP, TimeInForce::IOC, 103};
    stop.owner = 11;
    replay::AddCommand sellStopLimit{8, Side::SELL, 94, 3, OrderType::STOP_LIMIT, TimeInForce::GTC, 95};
    sellStopLimit.owner = 12;
    sellStopLimit.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    const replay::AddCommand buyStopLimit{9, Side::BUY, 104, 1, OrderType::STOP_LIMIT, TimeInForce::GTC, 102};
    for (const replay::AddCommand& add : {stop, sellStopLimit, buyStopLimit}) {
        static_cast<void>(replay::executeCommand(book, replay::Command{add}));
    }
    expect(book.pendingStopCount() == 3, "three stops are pending");

    const BookView view = decodeBookImage(imageOf(book));
    const auto sequence = [&](OrderId id) { return book.findActiveOrder(id).getSequenceNum(); };
    const std::vector<LevelView> bids{{100, 11, {{1, sequence(1), 10, 6, 7}, {2, sequence(2), 5, 5, 0}}}, {99, 4, {{3, sequence(3), 4, 4, 8}}}};
    const std::vector<LevelView> asks{{101, 3, {{5, sequence(5), 3, 3, 0}}}, {105, 6, {{4, sequence(4), 6, 6, 9}}}};
    expect(view.bids == bids, "the bids, best first, each level's orders in queue order with their owners and the partial fill");
    expect(view.asks == asks, "the asks, best first");
    expect(view.lastTradePrice == Price{100}, "the last trade price");
    expect(view.stops.size() == 3, "every pending stop");
    PendingStopView first{9, view.stops[0].sequence, Side::BUY, OrderType::STOP_LIMIT, 102, 104, 1, TimeInForce::GTC, 0};
    PendingStopView second{7, view.stops[1].sequence, Side::BUY, OrderType::STOP, 103, 0, 2, TimeInForce::IOC, 11};
    PendingStopView third{8, view.stops[2].sequence, Side::SELL, OrderType::STOP_LIMIT, 95, 94, 3, TimeInForce::GTC, 12};
    third.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    expect(view.stops[0] == first && view.stops[1] == second && view.stops[2] == third,
           "buy stops by ascending stop price, then sell stops by descending, every field as submitted");
    expect(view.stops[0].sequence != 0 && view.stops[0].sequence != view.stops[1].sequence, "each stop keeps its own sequence number");
    expect(highestOrderId(view) == 9, "the highest ID resting or pending");

    const BookView empty = decodeBookImage(imageOf(OrderBook{}));
    expect(empty.bids.empty() && empty.asks.empty() && empty.stops.empty() && !empty.lastTradePrice && highestOrderId(empty) == 0,
           "an empty book decodes to nothing, with no last trade");
    std::vector<std::byte> cut = imageOf(book);
    cut.pop_back();
    expectThrows([&] { static_cast<void>(decodeBookImage(cut)); }, "an image cut short is refused");
    expectThrows([&] { static_cast<void>(decodeBookImage(std::vector<std::byte>(10))); }, "bytes shorter than a header are refused");
}

// AAPL on shard 0 and MSFT on shard 1, pinned, as two shards place them.
runtime::InstrumentDirectory twoShards() {
    std::vector<runtime::Instrument> list(2);
    list[0].symbol = symbol("AAPL");
    list[0].shard = 0;
    list[1].symbol = symbol("MSFT");
    list[1].shard = 1;
    return runtime::InstrumentDirectory(list, 2);
}

// A page store whose changes the shadows follow, and log files made in it the engine's way.
class Followed {
public:
    explicit Followed(const runtime::InstrumentDirectory& directory)
        : instruments(directory), shadows(directory), store([this](const Change& change) { shadows.follow(change); }) {}

    // The shard's next file, made as housekeeping makes one: created as .tmp, its header
    // written, extended, renamed.
    runtime::MadeLogFile file(std::uint32_t shard, std::uint64_t firstPosition) {
        return runtime::makeLogFile(store, runtime::logFileShape(instruments, shard, runtime::Durability::PROCESS, 4096), firstPosition);
    }

    // These records written at once at the file's next record, chained from its last.
    void write(runtime::MadeLogFile& made, const std::vector<std::pair<Symbol, replay::Command>>& records) {
        std::vector<std::byte> bytes(records.size() * replay::logRecordBytes);
        for (std::size_t i = 0; i < records.size(); ++i) {
            made.chainSeed = replay::encodeLogRecord(records[i].first, records[i].second, made.chainSeed,
                                                     std::span<std::byte>(bytes).subspan(i * replay::logRecordBytes).first<replay::logRecordBytes>());
        }
        made.file->write(made.headerBytes + written[made.name] * replay::logRecordBytes, bytes);
        written[made.name] += records.size();
    }

    const runtime::InstrumentDirectory& instruments;
    ShadowBooks shadows;
    runtime::PageLogStorage store;
    std::map<std::string, std::uint64_t> written;
};

void testFollowingTheLog() {
    const runtime::InstrumentDirectory directory = twoShards();
    Followed followed(directory);
    expect(followed.shadows.assignOrderId(symbol("AAPL")) == OrderId{1} && followed.shadows.assignOrderId(symbol("AAPL")) == OrderId{2},
           "with nothing seen, IDs count from 1");
    expect(!followed.shadows.assignOrderId(symbol("ZZZ")), "no ID for a symbol off the list");

    runtime::MadeLogFile aapl = followed.file(0, 0);
    followed.write(aapl, {{symbol("AAPL"), limit(10, Side::BUY, 99, 5, 3)}, {symbol("AAPL"), limit(11, Side::SELL, 101, 2)}});
    followed.write(aapl, {{symbol("AAPL"), limit(12, Side::SELL, 99, 1)}});
    runtime::MadeLogFile msft = followed.file(1, 0);
    followed.write(msft, {{symbol("MSFT"), limit(4, Side::SELL, 50, 1)}, {symbol("MSFT"), replay::CancelCommand{4}}});

    expect(!followed.shadows.problem(), "the log is followed: " + followed.shadows.problem().value_or(""));
    expect(followed.shadows.records(0) == 3 && followed.shadows.records(1) == 2, "each shard's record count");
    expect(followed.shadows.shardOf(symbol("AAPL")) == 0U && followed.shadows.shardOf(symbol("MSFT")) == 1U && !followed.shadows.shardOf(symbol("ZZZ")),
           "each instrument's shard");
    const BookView book = followed.shadows.book(symbol("AAPL"));
    expect(book.bids == std::vector<LevelView>{{99, 4, {{10, book.bids.at(0).orders.at(0).sequence, 5, 4, 3}}}} &&
               book.asks.size() == 1 && book.asks[0].price == 101 && book.lastTradePrice == Price{99},
           "each record ran on its book, as its shard runs it");
    expect(followed.shadows.book(symbol("MSFT")).asks.empty(), "the cancel ran too");
    expect(followed.shadows.assignOrderId(symbol("AAPL")) == OrderId{13}, "1 + the highest seen, past the highest assigned");
    expect(followed.shadows.assignOrderId(symbol("MSFT")) == OrderId{5}, "a cancelled order's ID was still seen");

    // A start reads the same store into the same books, from the empty books' checkpoints at the
    // log's start, as an engine's first start publishes them, though checkpoints aren't followed.
    for (const auto& [name, shard] : {std::pair{"AAPL", 0U}, std::pair{"MSFT", 1U}}) {
        runtime::CheckpointHeader header;
        header.shardCount = 2;
        header.shardIndex = shard;
        header.symbol = symbol(name);
        runtime::publishCheckpoint(followed.store, header, imageOf(OrderBook{}));
    }
    expect(!followed.shadows.problem(), "a checkpoint's changes are no log's: " + followed.shadows.problem().value_or(""));
    ShadowBooks rebuilt(directory);
    rebuilt.rebuild(followed.store);
    expect(!rebuilt.problem(), "the store rebuilds: " + rebuilt.problem().value_or(""));
    for (const char* name : {"AAPL", "MSFT"}) {
        expect(rebuilt.image(symbol(name)) == followed.shadows.image(symbol(name)), std::string(name) + " rebuilds to the book followed");
    }
    expect(rebuilt.records(0) == 3 && rebuilt.records(1) == 2, "and to the same record counts");
    expect(rebuilt.assignOrderId(symbol("AAPL")) == OrderId{12}, "after a rebuild, 1 + the highest its book holds");

    // A file made before the rebuild never gets another record, since a start opens a new one.
    rebuilt.follow(Change{Change::Kind::WRITE, aapl.name, "", aapl.headerBytes + 3 * replay::logRecordBytes, {}, 0});
    expect(rebuilt.problem().has_value(), "a write to a file made before the start is a problem");

    followed.write(aapl, {{symbol("AAPL"), limit(std::numeric_limits<OrderId>::max(), Side::BUY, 1, 1)}});
    expect(!followed.shadows.assignOrderId(symbol("AAPL")), "no ID is left past the largest");
}

// Each way a write can disagree with its shard's log stops the shadows, which say why.
void testWhatTheShadowsRefuse() {
    const runtime::InstrumentDirectory directory = twoShards();
    {
        Followed followed(directory);
        runtime::MadeLogFile made = followed.file(0, 0);
        followed.write(made, {{symbol("MSFT"), limit(1, Side::BUY, 1, 1)}});
        expect(followed.shadows.problem().value_or("").find("not on shard 0") != std::string::npos, "a record for another shard's instrument");
    }
    {
        Followed followed(directory);
        runtime::MadeLogFile made = followed.file(0, 0);
        followed.written[made.name] = 1;
        followed.write(made, {{symbol("AAPL"), limit(1, Side::BUY, 1, 1)}});
        expect(followed.shadows.problem().value_or("").find("where whole records go") != std::string::npos, "a record past a gap");
    }
    {
        Followed followed(directory);
        runtime::MadeLogFile made = followed.file(0, 0);
        made.chainSeed ^= 1;
        followed.write(made, {{symbol("AAPL"), limit(1, Side::BUY, 1, 1)}});
        expect(followed.shadows.problem().value_or("").find("doesn't decode") != std::string::npos, "a record off the chain");
    }
    {
        Followed followed(directory);
        runtime::MadeLogFile made = followed.file(0, 5);
        followed.write(made, {{symbol("AAPL"), limit(1, Side::BUY, 1, 1)}});
        expect(followed.shadows.problem().value_or("").find("whose log holds 0") != std::string::npos, "a file starting past the log's end");
        followed.shadows.follow(Change{Change::Kind::CREATE, "later", "", 0, {}, 0});
        expect(followed.shadows.problem().value_or("").find("whose log holds 0") != std::string::npos, "the first problem is kept");
    }
    {
        // A log whose header names a symbol off the list, which a start refuses too.
        std::vector<runtime::Instrument> list(1);
        list[0].symbol = symbol("ZZZ");
        const runtime::InstrumentDirectory other(list, 2);
        Followed followed(other);
        runtime::MadeLogFile made = followed.file(runtime::hashedShard(symbol("ZZZ"), 2), 0);
        followed.write(made, {{symbol("ZZZ"), limit(1, Side::BUY, 1, 1)}});
        ShadowBooks shadows(directory);
        shadows.rebuild(followed.store);
        expect(shadows.problem().value_or("").starts_with("the shadows can't be rebuilt from the store: "), "a store a start refuses");
        expect(shadows.book(symbol("AAPL")).bids.empty(), "leaves every book empty");
    }
}

// What the engine worker would see: changes, wakes, and ready, under the engine's threads.
class Page {
public:
    BrowserCallbacks callbacks() {
        BrowserCallbacks told;
        told.wake = [this] {
            const std::lock_guard held(guard);
            ++wakes;
            woken.notify_all();
        };
        told.stored = [this](const Change& change) {
            runtime::PageLogStorage::apply(copy, change);
            ++changes;
        };
        told.ready = [this] {
            const std::lock_guard held(guard);
            readied = true;
            woken.notify_all();
        };
        return told;
    }

    void waitUntilReady() {
        std::unique_lock held(guard);
        expect(woken.wait_for(held, std::chrono::seconds(60), [this] { return readied; }), "the engine became ready");
        readied = false;
    }

    // Sleeps until the next wake after `seen`, and gives the count.
    std::size_t waitPast(std::size_t seen) {
        std::unique_lock held(guard);
        expect(woken.wait_for(held, std::chrono::seconds(60), [&] { return wakes > seen; }), "the connections woke the page");
        return wakes;
    }

    std::size_t wakesSoFar() {
        const std::lock_guard held(guard);
        return wakes;
    }

    // Under the store's lock, which every change is told under.
    std::map<std::string, std::vector<std::byte>> copy;
    std::size_t changes = 0;

private:
    std::mutex guard;
    std::condition_variable woken;
    std::size_t wakes = 0;
    bool readied = false;
};

// `order_book_cli send <file>` over the in-page connections: everything written as the buffer
// takes it, a half-close, and every answer read to the end, sleeping between wakes.
cli::CommandResult send(network::PageSocketSet& sockets, Page& page, const std::string& file) {
    const std::vector<std::string_view> args{file, "--connect", "127.0.0.1:9000"};
    std::variant<cli::PreparedSend, cli::CommandResult> prepared = cli::prepareSend(args);
    if (const auto* refused = std::get_if<cli::CommandResult>(&prepared)) {
        expect(false, "the file is sent, not refused: " + refused->err);
    }
    cli::PreparedSend& session = std::get<cli::PreparedSend>(prepared);
    network::ClientStream stream(std::move(session.commands));
    const std::optional<network::SocketId> socket = sockets.clientConnect();
    expect(socket.has_value(), "the engine takes a connection");
    std::size_t seen = page.wakesSoFar();
    bool halfClosed = false;
    while (stream.reading()) {
        for (std::span<const std::byte> out = stream.outgoing(); !out.empty(); out = stream.outgoing()) {
            const std::size_t taken = sockets.clientSend(*socket, out);
            stream.wrote(taken);
            if (taken < out.size()) {
                break;
            }
        }
        if (stream.allWritten() && !halfClosed) {
            sockets.clientHalfClose(*socket);
            halfClosed = true;
        }
        const std::vector<std::byte> answer = sockets.clientReceive(*socket);
        if (!answer.empty()) {
            stream.received(answer);
        } else if (sockets.clientEnd(*socket) != network::PageSocketSet::ClientEnd::OPEN) {
            stream.ended();
        } else {
            seen = page.waitPast(seen);
        }
    }
    sockets.clientRelease(*socket);
    return cli::finishSend(session, stream);
}

// One BrowserEngine run on a thread of its own, as the engine worker starts the entry.
class Running {
public:
    explicit Running(BrowserEngine& browser) : engine(browser) {
        thread = std::thread([this] { exit = engine.run(stop, report, diagnostics); });
    }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    ~Running() {
        if (thread.joinable()) {
            stop.request();
            thread.join();
        }
    }

    network::EngineExit cleanStop() {
        stop.request();
        thread.join();
        return *exit;
    }

    BrowserEngine& engine;
    network::StopSource stop;
    std::ostringstream report;
    std::ostringstream diagnostics;
    std::optional<network::EngineExit> exit;

private:
    std::thread thread;
};

// The shadows hold what the shards held at the clean stop.
void expectTheShadowsEqualTheShards(const BrowserEngine& engine, const std::vector<runtime::PageLogStorage::StoredFile>& files) {
    const std::optional<std::string> difference = wasm_testing::shadowsDifferFromTheStop(
        engine.shadows(), {symbol("AAPL"), symbol("BRK.B"), symbol("MSFT"), symbol("ZZZ")}, files);
    expect(!difference, "each shadow equals its shard's book: " + difference.value_or(""));
}

// The whole engine in the browser's shape: a session over the in-page connections printing what
// replay prints, every store change handed on so the page's copy holds the store, a clean stop
// whose books equal the shadows, then a new engine from that copy, as after a crash, recovering
// them.
void testABrowserEngineRun(const std::filesystem::path& replayDir, const std::filesystem::path& list) {
    Page page;
    BrowserEngine engine(list.string(), {}, page.callbacks());
    expect(engine.settings() == browserSettings(list.string()), "a run starts with the shipped settings");
    expect(engine.sockets() == nullptr, "no connections before the first run");
    {
        Running running(engine);
        page.waitUntilReady();
        const std::shared_ptr<network::PageSocketSet> sockets = engine.sockets();
        expect(sockets != nullptr, "the run's connections are there by its ready");
        const cli::CommandResult sent = send(*sockets, page, (replayDir / "valid/v3_two_instruments.csv").string());
        expect(sent.exit == 0 && sent.err.empty(), "every command was answered:\n" + sent.err);
        expect(sent.out == readFile(replayDir / "expected/v3_two_instruments_no_statistics.stdout"),
               "the answers print as replay --events --no-statistics prints the file:\n" + sent.out);
        expect(engine.shadows().records(0) + engine.shadows().records(1) == 6, "the shadows ran the file's six records");
        // Every answer waited for its log write, which the shadows ran first.
        const BookView msft = engine.shadows().book(symbol("MSFT"));
        expect(msft.asks.size() == 1 && msft.asks[0].price == 101 && msft.asks[0].orders.size() == 1 && msft.asks[0].orders[0].orderId == 1 &&
                   msft.asks[0].orders[0].remainingQuantity == 3 && msft.lastTradePrice == Price{101} && msft.bids.empty(),
               "MSFT's sell rests partly filled by the buy");
        const BookView brk = engine.shadows().book(symbol("BRK.B"));
        expect(brk.bids.size() == 1 && brk.bids[0].price == 98 && brk.asks.empty(), "BRK.B's buy rests");
        expect(engine.shadows().book(symbol("AAPL")).bids.empty(), "AAPL's buy was cancelled");
        expect(engine.shadows().assignOrderId(symbol("MSFT")) == OrderId{3}, "MSFT's next ID is past the 2 its log holds");
        expect(running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop exits 0:\n" + running.diagnostics.str());
    }
    expect(!engine.shadows().problem(), "the shadows followed the whole run: " + engine.shadows().problem().value_or(""));
    const std::vector<runtime::PageLogStorage::StoredFile> files = engine.store().files();
    expect(page.changes > 0 && runtime::PageLogStorage::storedFiles(page.copy) == files, "the page's copy holds the store");
    expectTheShadowsEqualTheShards(engine, files);

    // A new engine worker is given the page's copy and recovers from it.
    Page next;
    BrowserEngine recovered(list.string(), runtime::PageLogStorage::storedFiles(page.copy), next.callbacks());
    Running again(recovered);
    next.waitUntilReady();
    for (const char* name : {"AAPL", "BRK.B", "MSFT", "ZZZ"}) {
        expect(recovered.shadows().image(symbol(name)) == engine.shadows().image(symbol(name)), std::string(name) + " is rebuilt from the copy");
    }
    expect(recovered.shadows().records(0) == engine.shadows().records(0) && recovered.shadows().records(1) == engine.shadows().records(1),
           "and so are the record counts");
    expect(again.cleanStop() == network::EngineExit::CLEAN_STOP, "the recovered engine stops cleanly:\n" + again.diagnostics.str());
    expect(!recovered.shadows().problem(), "and its shadows followed it: " + recovered.shadows().problem().value_or(""));
    expectTheShadowsEqualTheShards(recovered, recovered.store().files());
}

}  // namespace

int main(int argc, char** argv) {
    expect(argc == 3, "usage: order_book_wasm_tests <tests/replay> <scratch directory>");
    const std::filesystem::path replayDir = argv[1];
    const std::filesystem::path scratch = argv[2];
    std::filesystem::create_directories(scratch);
    const std::filesystem::path list = scratch / "instruments.csv";
    std::ofstream(list, std::ios::binary) << "symbol,shard,orders,stops,levels\nAAPL,,,,\nBRK.B,,,,\nMSFT,,,,\nZZZ,,,,\n";

    testTheSettings();
    testDecodingAnImage();
    testFollowingTheLog();
    testWhatTheShadowsRefuse();
    testABrowserEngineRun(replayDir, list);
    std::cout << "order_book_wasm tests passed" << std::endl;
    return EXIT_SUCCESS;
}
