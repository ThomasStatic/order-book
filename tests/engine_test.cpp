#include <tuple>
#include <iterator>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <streambuf>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "engine/gateway_frames.hpp"
#include "order_book/command_log.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/thread_placement.hpp"
#include "order_book/xxh64.hpp"

// runEngine in this process, on a thread of its own, with a test's StopSource and real loopback
// sockets. Threaded, so ThreadSanitizer and Arm run it. See docs/internals/network.md.
namespace {

using namespace order_book;
using namespace order_book::network;
using namespace std::chrono_literals;
using order_book::gateway_testing::Bytes;
using order_book::gateway_testing::helloFrame;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// An ostream another thread can wait on, for a line to appear.
class WatchedStream : public std::ostream {
public:
    WatchedStream() : std::ostream(&buffer) {}

    std::string text() {
        const std::lock_guard<std::mutex> hold(buffer.guard);
        return buffer.held;
    }

    // Waits until the text holds this, or fails the test after a generous limit.
    std::string waitFor(const std::string& wanted) {
        std::unique_lock<std::mutex> hold(buffer.guard);
        const bool found = buffer.changed.wait_for(hold, 30s, [&] { return buffer.held.find(wanted) != std::string::npos; });
        expect(found, "waited too long for '" + wanted + "', with:\n" + buffer.held);
        return buffer.held;
    }

private:
    struct Buffer : std::streambuf {
        int_type overflow(int_type character) override {
            if (character != traits_type::eof()) {
                const std::lock_guard<std::mutex> hold(guard);
                held.push_back(static_cast<char>(character));
                changed.notify_all();
            }
            return character;
        }
        std::streamsize xsputn(const char* text, std::streamsize count) override {
            const std::lock_guard<std::mutex> hold(guard);
            held.append(text, static_cast<std::size_t>(count));
            changed.notify_all();
            return count;
        }

        std::mutex guard;
        std::condition_variable changed;
        std::string held;
    };

    Buffer buffer;
};

std::filesystem::path writeList(const std::string& name, const std::string& text) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("order_book_engine_test_" + name + ".csv");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

runtime::EngineSettings settingsFor(const std::filesystem::path& list, std::uint32_t shards) {
    runtime::EngineSettings settings;
    settings.instruments = list.string();
    settings.shards = shards;
    settings.listen = runtime::ListenAddress{{127, 0, 0, 1}, 0};
    return settings;
}

// One run on its own thread, so the test can talk to it while it runs.
class Run {
public:
    explicit Run(const runtime::EngineSettings& runSettings) : settings(runSettings) {
        thread = std::thread([this] { exit = runEngine(settings, stop, report, diagnostics); });
    }
    ~Run() {
        if (thread.joinable()) {
            stop.request();
            thread.join();
        }
    }

    std::uint16_t port() {
        const std::string text = report.waitFor("\n");
        const std::size_t colon = text.rfind(':');
        expect(text.starts_with("listening 127.0.0.1:") && colon != std::string::npos, "the first line says where it listens:\n" + text);
        return static_cast<std::uint16_t>(std::stoul(text.substr(colon + 1)));
    }

    EngineExit finish() {
        thread.join();
        return *exit;
    }

    runtime::EngineSettings settings;
    StopSource stop;
    WatchedStream report;
    WatchedStream diagnostics;
    std::optional<EngineExit> exit;
    std::thread thread;
};

replay::AddCommand limit(OrderId id, Side side, Price price) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = 1;
    return command;
}

// Sends everything, half-closes, and reads to the end: every answer's events, in order.
std::vector<BookEvent> talk(std::uint16_t port, const Bytes& stream) {
    const TcpSocket socket = TcpSocket::connectLoopback(port);
    std::span<const std::byte> left(stream);
    while (!left.empty()) {
        const std::optional<std::size_t> taken = socket.send(left);
        expect(taken.has_value(), "the engine took what it was sent");
        left = left.subspan(*taken);
    }
    socket.shutdownSend();
    FrameReader reader(FrameSource::ENGINE);
    std::vector<BookEvent> events;
    std::array<std::byte, 16 * 1024> buffer;
    for (ReadResult read = socket.receive(buffer); read.status == ReadStatus::OPEN; read = socket.receive(buffer)) {
        reader.feed(std::span<const std::byte>(buffer).first(read.bytes));
        for (FrameResult result = reader.next(); std::holds_alternative<Frame>(result); result = reader.next()) {
            if (const std::optional<EventFrame> event = decodeEvent(std::get<Frame>(result))) {
                events.push_back(event->event);
            }
        }
    }
    return events;
}

void testACleanStop() {
    const std::filesystem::path list = writeList("clean", "symbol,shard,orders,stops,levels\nMSFT,1,2,3,4\nAAPL,,,,\n");
    Run run(settingsFor(list, 2));
    const std::uint16_t port = run.port();

    // MSFT's book holds 2 orders before it grows, so 6 resting buys make it grow.
    Bytes stream = helloFrame();
    OrderBook witness;
    witness.reserve(BookCapacity{2, 3, 4});
    for (OrderId id = 1; id <= 6; ++id) {
        Bytes frame(64);
        frame.resize(encodeCommand(id, *Symbol::parse("MSFT"), limit(id, Side::BUY, 100 - id), frame));
        stream.insert(stream.end(), frame.begin(), frame.end());
        OrderRequest request{};
        request.id = id;
        request.side = Side::BUY;
        request.price = 100 - id;
        request.quantity = 1;
        witness.submitOrder(request);
    }
    expect(talk(port, stream).size() == 6, "every command was answered");

    run.stop.request();
    expect(run.finish() == EngineExit::CLEAN_STOP, "a clean stop exits 0");
    expect(witness.capacityGrowths() > 0, "the witness grew, so the report has something to count");
    const std::string report = run.report.text();
    const std::string expected = report.substr(0, report.find('\n') + 1) +
                                 "stopped\n"
                                 "book AAPL shard=" +
                                 std::to_string(runtime::hashedShard(*Symbol::parse("AAPL"), 2)) +
                                 " orders=4096 stops=1024 levels=256 growths=0\n"
                                 "book MSFT shard=1 orders=2 stops=3 levels=4 growths=" +
                                 std::to_string(witness.capacityGrowths()) +
                                 "\n"
                                 "shard 0: result-growths=0\n"
                                 "shard 1: result-growths=0\n"
                                 "total: books=2 capacity-growths=" +
                                 std::to_string(witness.capacityGrowths()) + "\n";
    expect(report == expected, "the stop report, books sorted by symbol, growths since ready:\n" + report);

    const std::string diagnostics = run.diagnostics.text();
    expect(diagnostics == "order_book_engine: " + runtime::formatEngineSettings(run.settings) + "\n",
           "stderr has the run's record and nothing else:\n" + diagnostics);
    std::filesystem::remove(list);
}

void testAStopBeforeReady() {
    const std::filesystem::path list = writeList("early", "symbol,shard,orders,stops,levels\nAAPL,,,,\n");
    StopSource stop;
    stop.request();
    WatchedStream report;
    WatchedStream diagnostics;
    expect(runEngine(settingsFor(list, 1), stop, report, diagnostics) == EngineExit::STOPPED_BEFORE_READY, "a stop before ready exits 6");
    expect(report.text().empty(), "nothing was ever listening");
    expect(diagnostics.text().ends_with("order_book_engine: stopped before ready\n"), "and says so");
    std::filesystem::remove(list);
}

EngineExit refusedWith(const runtime::EngineSettings& settings, const std::string& cause) {
    StopSource stop;
    WatchedStream report;
    WatchedStream diagnostics;
    const EngineExit exit = runEngine(settings, stop, report, diagnostics);
    const std::string text = diagnostics.text();
    expect(text.ends_with("order_book_engine: start refused: " + cause + "\n"), "refused, naming the cause:\n" + text);
    expect(report.text().empty(), "and never listening");
    return exit;
}

void testRefusals() {
    const std::filesystem::path good = writeList("good", "symbol,shard,orders,stops,levels\nAAPL,,,,\n");
    const std::filesystem::path bad = writeList("bad", "symbol,shard,orders,stops,levels\nAAPL,5,,,\n");

    runtime::EngineSettings settings = settingsFor(std::filesystem::path("no/such/list.csv"), 1);
    expect(refusedWith(settings, "cannot read no/such/list.csv") == EngineExit::START_REFUSED, "a list that can't be read exits 1");

    settings = settingsFor(bad, 2);
    expect(refusedWith(settings, bad.string() + ":2: shard: shard 5 is not below the shard count 2") == EngineExit::START_REFUSED,
           "a list that doesn't parse names its line");

    settings = settingsFor(good, 1);
    settings.commandRing = 1000;
    expect(refusedWith(settings, "--command-ring must be a power of two, not 1000") == EngineExit::START_REFUSED, "a ring");
    settings = settingsFor(good, 1);
    settings.maxConnections = 0;
    expect(refusedWith(settings, "--max-connections must be from 1 to 65536, not 0") == EngineExit::START_REFUSED, "a connection limit");
    settings = settingsFor(good, 0);
    expect(refusedWith(settings, "--shards must be at least 1") == EngineExit::START_REFUSED, "no shards");
    settings = settingsFor(good, 1);
    settings.gatewayThreads = 0;
    expect(refusedWith(settings, "--gateway-threads must be at least 1") == EngineExit::START_REFUSED, "no gateway thread");
    settings = settingsFor(good, 1);
    settings.rollWorkers = 0;
    expect(refusedWith(settings, "--roll-workers must be from 1 to the 1 shard, not 0") == EngineExit::START_REFUSED, "no roll worker");
    settings = settingsFor(good, 2);
    settings.rollWorkers = 3;
    expect(refusedWith(settings, "--roll-workers must be from 1 to the 2 shards, not 3") == EngineExit::START_REFUSED,
           "more roll workers than shards, each of which one worker rolls");

    const TcpSocket taken = TcpSocket::listenLoopback(0);
    settings = settingsFor(good, 1);
    settings.listen.port = taken.localPort();
    StopSource stop;
    WatchedStream report;
    WatchedStream diagnostics;
    expect(runEngine(settings, stop, report, diagnostics) == EngineExit::START_REFUSED, "a busy port exits 1");
    expect(diagnostics.text().find("order_book_engine: start refused: cannot listen on 127.0.0.1:" + std::to_string(taken.localPort()) + ": ") !=
               std::string::npos,
           "naming the address:\n" + diagnostics.text());

    std::filesystem::remove(good);
    std::filesystem::remove(bad);
}

// The first core a scratch thread can pin to, so a run pinned there can start.
std::uint32_t aCoreThisProcessMayUse() {
    std::uint32_t found = 0;
    std::thread([&found] {
        while (runtime::pinThisThread(found)) {
            expect(++found < 4096, "this process may run on some core");
        }
    }).join();
    return found;
}

void expectACleanRun(const runtime::EngineSettings& settings, const std::string& what) {
    Run run(settings);
    run.port();
    run.stop.request();
    expect(run.finish() == EngineExit::CLEAN_STOP, what);
}

void testPinningAndPriority() {
    const std::filesystem::path list = writeList("placed", "symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,1,,,\n");
    const std::uint32_t core = aCoreThisProcessMayUse();

    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.pinMatching = {core};
    expect(refusedWith(settings, "--pin-matching needs 2 cores, one per shard, not 1") == EngineExit::START_REFUSED,
           "a core for each shard");
    settings = settingsFor(list, 2);
    settings.pinGateway = {core, core};
    expect(refusedWith(settings, "--pin-gateway needs 1 core, one per gateway thread, not 2") == EngineExit::START_REFUSED,
           "and for each gateway thread");

    // Past what any build can pin, so the refusal comes from the thread that tried.
    settings = settingsFor(list, 2);
    settings.pinMatching = {core, 4'000'000'000u};
    const std::string past = *runtime::pinThisThread(4'000'000'000u);
    expect(refusedWith(settings, "cannot pin matching thread 1 to core 4000000000: " + past) == EngineExit::START_REFUSED,
           "a core one shard's thread can't take refuses the start, naming that shard");
    settings = settingsFor(list, 2);
    settings.pinGateway = {4'000'000'000u};
    expect(refusedWith(settings, "cannot pin the gateway thread to core 4000000000: " + past) == EngineExit::START_REFUSED,
           "and the gateway's");
    settings = settingsFor(list, 2);
    settings.gatewayThreads = 3;
    settings.pinGateway = {core};
    expect(refusedWith(settings, "--pin-gateway needs 3 cores, one per gateway thread, not 1") == EngineExit::START_REFUSED,
           "a core for each of several gateway threads");
    settings.pinGateway = {core, core, 4'000'000'000u};
    expect(refusedWith(settings, "cannot pin gateway thread 2 to core 4000000000: " + past) == EngineExit::START_REFUSED,
           "naming the one that couldn't take it");

    settings = settingsFor(list, 2);
    settings.pinMatching = {core, core};
    settings.pinGateway = {core};
    expectACleanRun(settings, "a run pinned where it may run starts and stops cleanly");
    settings.gatewayThreads = 2;
    settings.pinGateway = {core, core};
    expectACleanRun(settings, "and so does one with two gateway threads, each pinned");

    // Whether this process may raise a thread is the system's to say, so a scratch thread asks it.
    std::optional<std::string> denied;
    std::thread([&denied] { denied = runtime::setThisThreadPriority(runtime::ThreadPriority::HIGH); }).join();
    settings = settingsFor(list, 2);
    settings.threadPriority = runtime::ThreadPriority::HIGH;
    if (denied) {
        expect(refusedWith(settings, "--thread-priority high was denied: " + *denied) == EngineExit::START_REFUSED,
               "a denied priority refuses the start, so no record claims it");
    } else {
        expectACleanRun(settings, "a priority the system grants starts and stops cleanly");
    }
    std::filesystem::remove(list);
}

// A fresh, empty log directory.
std::filesystem::path logDirectory(const std::string& name) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("order_book_engine_test_logs_" + name);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<std::string> filesIn(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// The log files alone, without the lock file, which a start that took the lock leaves for good.
// Everything a run left but the lock.
std::vector<std::string> storeIn(const std::filesystem::path& directory) {
    std::vector<std::string> names = filesIn(directory);
    std::erase(names, "engine.lock");
    return names;
}

// The log files alone.
std::vector<std::string> logsIn(const std::filesystem::path& directory) {
    std::vector<std::string> names = storeIn(directory);
    std::erase_if(names, [](const std::string& name) { return !name.ends_with(".oblog"); });
    return names;
}

// The checkpoints alone.
std::vector<std::string> checkpointsIn(const std::filesystem::path& directory) {
    std::vector<std::string> names = storeIn(directory);
    std::erase_if(names, [](const std::string& name) { return !name.starts_with("ckpt-"); });
    return names;
}

EngineExit exitsWith(const runtime::EngineSettings& settings, const std::string& line) {
    StopSource stop;
    WatchedStream report;
    WatchedStream diagnostics;
    const EngineExit exit = runEngine(settings, stop, report, diagnostics);
    const std::string text = diagnostics.text();
    expect(text.ends_with("order_book_engine: " + line + "\n"), "ending with '" + line + "':\n" + text);
    expect(report.text().empty(), "and never listening");
    return exit;
}

std::string bytesOf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Bytes commandsFor(const char* symbol, OrderId count) {
    Bytes stream;
    for (OrderId id = 1; id <= count; ++id) {
        Bytes frame(64);
        frame.resize(encodeCommand(id, *Symbol::parse(symbol), limit(id, Side::BUY, 100), frame));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

// With a log directory, each shard writes one log, named for its shard and first position, that
// holds every command it ran, and the stop report gives where each ends. A second run on the
// same directory recovers the books from it and goes on where each log ended, in a new file.
void testALoggedRun() {
    const std::filesystem::path list = writeList("logged", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,2,3,4\n");
    const std::filesystem::path logs = logDirectory("logged");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.logDir = logs.string();
    settings.durability = runtime::Durability::PROCESS;
    {
        Run run(settings);
        const std::uint16_t port = run.port();
        Bytes stream = helloFrame();
        for (const Bytes& more : {commandsFor("MSFT", 5), commandsFor("AAPL", 3)}) {
            stream.insert(stream.end(), more.begin(), more.end());
        }
        expect(talk(port, stream).size() == 8, "every command was answered");
        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "a logged run stops cleanly");
        const std::string report = run.report.text();
        expect(report.find("shard 0: log-end=3 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\nshard 1: log-end=5 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\n") != std::string::npos,
               "each shard's line gives where its log ends:\n" + report);
    }
    const std::vector<std::string> expected{"log-0000-00000000000000000000.oblog", "log-0001-00000000000000000000.oblog"};
    expect(logsIn(logs) == expected, "one log per shard, named for its first position");
    expect(checkpointsIn(logs) == std::vector<std::string>{"ckpt-AAPL-00000000000000000000.obckpt", "ckpt-AAPL-00000000000000000003.obckpt",
                                                          "ckpt-MSFT-00000000000000000000.obckpt", "ckpt-MSFT-00000000000000000005.obckpt"},
           "and each new instrument's empty-book checkpoint, where the log began, and the clean stop's, where it ends");
    expect(filesIn(logs).size() == 7 && std::filesystem::file_size(logs / "engine.lock") == 0, "and the lock file, empty");
    for (const auto& [name, symbol, count] : {std::tuple{expected[0], "AAPL", 3U}, std::tuple{expected[1], "MSFT", 5U}}) {
        const replay::ReplayInput input = replay::readCommandLogFile(logs / name);
        expect(!input.error, name + " reads to its end, with no torn tail");
        expect(input.records.size() == count, name + " holds every command its shard ran");
        for (const replay::ReplayRecord& record : input.records) {
            expect(record.symbol == *Symbol::parse(symbol), name + " holds only its shard's instruments");
        }
    }

    const std::string before = bytesOf(logs / expected[0]);
    {
        Run run(settings);
        const std::uint16_t port = run.port();
        // MSFT's orders 1 to 5 rest from the first run, so cancelling 3 finds it, and AAPL's 3 too.
        Bytes stream = helloFrame();
        std::uint64_t correlation = 10;
        for (const auto& [symbol, id] : {std::pair{"MSFT", OrderId{3}}, std::pair{"AAPL", OrderId{3}}, std::pair{"AAPL", OrderId{4}}}) {
            Bytes frame(64);
            frame.resize(encodeCommand(correlation++, *Symbol::parse(symbol), replay::CancelCommand{id}, frame));
            stream.insert(stream.end(), frame.begin(), frame.end());
        }
        const std::vector<BookEvent> events = talk(port, stream);
        // Two shards answer in either order.
        const auto cancelled = std::count_if(events.begin(), events.end(), [](const BookEvent& event) {
            return std::holds_alternative<OrderCancelled>(event);
        });
        const auto rejected = std::count_if(events.begin(), events.end(), [](const BookEvent& event) {
            return std::holds_alternative<CancelRejected>(event);
        });
        expect(events.size() == 3 && cancelled == 2 && rejected == 1,
               "the second run holds the first run's books: orders 3 rest, and AAPL never had a 4");
        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "a recovered run stops cleanly");
        const std::string diagnostics = run.diagnostics.text();
        // Each book from the clean stop's checkpoint at its log's end, so nothing replays.
        expect(diagnostics.find("order_book_engine: shard 0: recovered books=1 replayed=3 tail-cut-bytes=0 from=3 fell-back=0\n"
                                "order_book_engine: shard 1: recovered books=1 replayed=5 tail-cut-bytes=0 from=5 fell-back=0\n") !=
                   std::string::npos,
               "each shard says what it recovered, in shard order:\n" + diagnostics);
        const std::string report = run.report.text();
        // The clean stop rolls each log to its end, so nothing is behind.
        expect(report.find("shard 0: log-end=5 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\n"
                           "shard 1: log-end=6 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\n") != std::string::npos,
               "and each log goes on from where it ended:\n" + report);
        // MSFT declares room for 2 orders and holds 5 on 1 level, so recovery warms it to 8
        // orders, its own 3 stops, and its own 4 levels, more than 1.5 times its 1.
        expect(report.find("book AAPL shard=0 orders=4096 stops=1024 levels=256 growths=0\n"
                           "book MSFT shard=1 orders=8 stops=3 levels=4 growths=0\n") != std::string::npos,
               "and the stop report gives what recovery warmed each book to:\n" + report);
    }
    expect(logsIn(logs) == std::vector<std::string>{expected[0], "log-0000-00000000000000000003.oblog", expected[1],
                                                    "log-0001-00000000000000000005.oblog"},
           "each shard's next run is in a new file, named for where its log ended");
    expect(bytesOf(logs / expected[0]) == before, "leaving the first run's files as they were");

    // A third run adds IBM on shard 1, whose log ended at 6: a new instrument, named in no log
    // header and with no checkpoint, gets an empty book's where that shard's log ends.
    const std::filesystem::path added = writeList("logged-added", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,2,3,4\nIBM,1,,,\n");
    settings.instruments = added.string();
    {
        Run run(settings);
        static_cast<void>(run.port());  // once listening, so every shard published what it had to
        run.stop.request();
        const EngineExit exit = run.finish();
        expect(exit == EngineExit::CLEAN_STOP,
               "a run with a new instrument stops cleanly, exit " + std::to_string(static_cast<int>(exit)) + ":\n" + run.diagnostics.text());
    }
    std::vector<std::string> ibm = checkpointsIn(logs);
    std::erase_if(ibm, [](const std::string& name) { return !name.starts_with("ckpt-IBM-"); });
    expect(ibm == std::vector<std::string>{"ckpt-IBM-00000000000000000006.obckpt"}, "IBM's one checkpoint, at shard 1's log end");
    const std::string held = bytesOf(logs / ibm[0]);
    std::vector<std::byte> bytes(held.size());
    std::transform(held.begin(), held.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto decoded = runtime::decodeCheckpoint(bytes);
    const auto* checkpoint = std::get_if<runtime::DecodedCheckpoint>(&decoded);
    expect(checkpoint != nullptr && checkpoint->header.shardIndex == 1 && checkpoint->header.shardCount == 2,
           "of shard 1 of 2");
    const OrderBook empty;
    std::vector<std::byte> emptyImage(empty.imageSize());
    static_cast<void>(empty.writeImage(emptyImage));
    expect(checkpoint != nullptr && std::equal(checkpoint->image.begin(), checkpoint->image.end(), emptyImage.begin(), emptyImage.end()),
           "an empty book's image");
    std::filesystem::remove_all(logs);
    std::filesystem::remove(list);
    std::filesystem::remove(added);
}

// The lock, and the start's own refusals: another shard count or a moved symbol exits 4, and a
// damaged log exits 5, each before anything listens, changing nothing.
void testRecoveryRefusals() {
    const std::filesystem::path list = writeList("recovery", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,,,\n");
    const std::filesystem::path logs = logDirectory("recovery");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.logDir = logs.string();
    {
        Run run(settings);
        Bytes stream = helloFrame();
        const Bytes commands = commandsFor("MSFT", 2);
        stream.insert(stream.end(), commands.begin(), commands.end());
        expect(talk(run.port(), stream).size() == 2, "every command was answered");
        // While it runs, the directory is locked.
        const std::string lockFile = (logs / "engine.lock").string();
        expect(refusedWith(settings, lockFile + " is locked: another engine or command is using " + logs.string()) ==
                   EngineExit::START_REFUSED,
               "a second engine on a directory in use is refused");
        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "and the first stops cleanly");
    }
    const std::vector<std::string> before = filesIn(logs);
    const std::string placement = "and this build can't change placement while a log exists, so start on the old placement";
    runtime::EngineSettings three = settingsFor(list, 3);
    three.logDir = logs.string();
    expect(exitsWith(three, "start refused: placement changed: the log was written by 2 shards and this start has 3, " + placement) ==
               EngineExit::PLACEMENT_CHANGED,
           "another shard count exits 4");
    const std::filesystem::path moved = writeList("recovery_moved", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,0,,,\n");
    runtime::EngineSettings movedSettings = settingsFor(moved, 2);
    movedSettings.logDir = logs.string();
    expect(exitsWith(movedSettings, "start refused: placement changed: MSFT moved from shard 1 to shard 0, " + placement) ==
               EngineExit::PLACEMENT_CHANGED,
           "a moved symbol exits 4");
    const std::filesystem::path off = writeList("recovery_off", "symbol,shard,orders,stops,levels\nAAPL,0,,,\n");
    runtime::EngineSettings offSettings = settingsFor(off, 2);
    offSettings.logDir = logs.string();
    expect(refusedWith(offSettings, "log-0001-00000000000000000000.oblog names MSFT, which is not on the instrument list") ==
               EngineExit::START_REFUSED,
           "a symbol off the list exits 1");
    expect(filesIn(logs) == before, "and none of them changes the directory");

    const std::string aapl = bytesOf(logs / "log-0000-00000000000000000000.oblog");
    std::string damaged = aapl;
    damaged[9] = static_cast<char>(damaged[9] ^ 1);
    std::ofstream(logs / "log-0000-00000000000000000000.oblog", std::ios::binary | std::ios::trunc) << damaged;
    expect(exitsWith(settings, "recovery stopped: shard 0 position 0 file log-0000-00000000000000000000.oblog: CORRUPT_HEADER: the "
                               "header's checksum does not match") == EngineExit::RECOVERY_STOPPED,
           "a damaged log exits 5");
    expect(filesIn(logs) == before, "changing nothing");

    // From a matching thread's read of the log too: MSFT's second record names no command, under
    // a checksum that chains, so it is the record and not a tear that is found, at its own
    // position, though it lies before MSFT's checkpoint at the log's end and only its chain is read.
    std::ofstream(logs / "log-0000-00000000000000000000.oblog", std::ios::binary | std::ios::trunc) << aapl;
    std::string msft = bytesOf(logs / "log-0001-00000000000000000000.oblog");
    const std::size_t second = msft.size() - replay::logRecordBytes;
    msft[second + 4] = static_cast<char>(9);
    std::uint32_t previous = 0;
    for (std::size_t i = 4; i-- > 0;) {
        previous = (previous << 8) | std::uint32_t{static_cast<unsigned char>(msft[second - replay::logRecordBytes + i])};
    }
    auto checksum = static_cast<std::uint32_t>(
        replay::xxh64(std::as_bytes(std::span<const char>(msft.data() + second + 4, replay::logRecordBytes - 4)), previous) & 0xFFFFFFFFULL);
    for (std::size_t i = 0; i < 4; ++i, checksum >>= 8) {
        msft[second + i] = static_cast<char>(checksum & 0xFF);
    }
    std::ofstream(logs / "log-0001-00000000000000000000.oblog", std::ios::binary | std::ios::trunc) << msft;
    expect(exitsWith(settings, "recovery stopped: shard 1 position 1 file log-0001-00000000000000000000.oblog: UNKNOWN_COMMAND: not a "
                               "command") == EngineExit::RECOVERY_STOPPED,
           "a record recovery can't replay exits 5 from its shard's thread");
    std::filesystem::remove_all(logs);
    for (const std::filesystem::path& path : {list, moved, off}) {
        std::filesystem::remove(path);
    }
}

// A shard's log rolls across files of --log-file-size, each starting where the last ended, every
// one but the last full, and a clean stop leaves no unused next file and no temporary one.
void testALoggedRunAcrossFiles() {
    const std::filesystem::path list = writeList("logged_files", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,,,\n");
    const std::filesystem::path logs = logDirectory("logged_files");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.logDir = logs.string();
    settings.keepLog = true;  // every file read back below, whatever the workers have rolled
    const runtime::InstrumentDirectory directory(
        {runtime::Instrument{*Symbol::parse("AAPL"), 0, std::nullopt, std::nullopt, std::nullopt},
         runtime::Instrument{*Symbol::parse("MSFT"), 1, std::nullopt, std::nullopt, std::nullopt}},
        2);
    settings.logFileSize = static_cast<std::size_t>(runtime::leastLogFileBytes(directory)) + replay::logRecordBytes;  // two records
    {
        Run run(settings);
        Bytes stream = helloFrame();
        for (const Bytes& more : {commandsFor("MSFT", 5), commandsFor("AAPL", 4)}) {
            stream.insert(stream.end(), more.begin(), more.end());
        }
        expect(talk(run.port(), stream).size() == 9, "every command was answered");
        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "a run across files stops cleanly");
        const std::string report = run.report.text();
        // The clean stop rolls whatever the worker had not reached, so nothing is behind.
        expect(report.find("shard 0: log-end=4 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\n"
                           "shard 1: log-end=5 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0\n") != std::string::npos,
               "each shard's line gives where its log ends:\n" + report);
    }
    const std::vector<std::string> expected{"log-0000-00000000000000000000.oblog", "log-0000-00000000000000000002.oblog",
                                            "log-0001-00000000000000000000.oblog", "log-0001-00000000000000000002.oblog",
                                            "log-0001-00000000000000000004.oblog"};
    expect(logsIn(logs) == expected, "files of two records, and no unused next file, which shard 0's full last file had");
    for (const auto& [name, symbol, first, count] :
         {std::tuple{expected[0], "AAPL", 0U, 2U}, std::tuple{expected[1], "AAPL", 2U, 2U}, std::tuple{expected[2], "MSFT", 0U, 2U},
          std::tuple{expected[3], "MSFT", 2U, 2U}, std::tuple{expected[4], "MSFT", 4U, 1U}}) {
        const replay::ReplayInput input = replay::readCommandLogFile(logs / name);
        expect(!input.error, name + " reads to its end, with no torn tail");
        expect(!input.records.empty() && input.records.front().sequence == first + 1, name + " starts where the last file ended");
        expect(input.records.size() == count, name + " holds its share of its shard's commands");
        for (const replay::ReplayRecord& record : input.records) {
            expect(record.symbol == *Symbol::parse(symbol) && record.sequence > first && record.sequence <= first + count,
                   name + " holds its own positions, of its shard's instruments");
        }
    }
    std::filesystem::remove_all(logs);
    std::filesystem::remove(list);
}

// A clean stop that can't publish a book exits 7, naming it, and still prints its stop report;
// the next start recovers that book from the log. AAPL's checkpoint at its log's end can't be
// published, its name taken by a directory, so the rename meets a file already there.
void testACleanStopThatLeavesABookUnpublished() {
    const std::filesystem::path list = writeList("unpublished", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,0,,,\n");
    const std::filesystem::path logs = logDirectory("unpublished");
    runtime::EngineSettings settings = settingsFor(list, 1);
    settings.logDir = logs.string();
    settings.durability = runtime::Durability::PROCESS;
    const std::filesystem::path squatter = logs / "ckpt-AAPL-00000000000000000005.obckpt";
    {
        Run run(settings);
        Bytes stream = helloFrame();
        for (const Bytes& more : {commandsFor("AAPL", 3), commandsFor("MSFT", 2)}) {
            stream.insert(stream.end(), more.begin(), more.end());
        }
        expect(talk(run.port(), stream).size() == 5, "every command was answered");
        std::filesystem::create_directory(squatter);
        run.stop.request();
        expect(run.finish() == EngineExit::NOT_PUBLISHED, "a clean stop with a book unpublished exits 7:\n" + run.diagnostics.text());
        const std::string diagnostics = run.diagnostics.text();
        expect(diagnostics.find("order_book_engine: clean stop incomplete: AAPL's checkpoint at 5 could not be published; the next start "
                                "recovers from the log\n") != std::string::npos,
               "naming the book:\n" + diagnostics);
        expect(diagnostics.find("clean stop incomplete: MSFT") == std::string::npos, "and only that book:\n" + diagnostics);
        expect(run.report.text().find("stopped\n") != std::string::npos &&
                   run.report.text().find("shard 0: log-end=5 result-growths=0 checkpoint-writes-failed=1 passes-lost=0 files-behind=1\n") !=
                       std::string::npos,
               "the stop report still comes, counting the failed write and the file behind:\n" + run.report.text());
    }
    std::vector<std::string> checkpoints = checkpointsIn(logs);
    expect(checkpoints == std::vector<std::string>{"ckpt-AAPL-00000000000000000000.obckpt", squatter.filename().string(),
                                                   "ckpt-MSFT-00000000000000000000.obckpt", "ckpt-MSFT-00000000000000000005.obckpt"},
           "MSFT published at the end, AAPL left at its empty checkpoint");
    std::filesystem::remove(squatter);
    {
        Run run(settings);
        static_cast<void>(run.port());
        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "the next start recovers and stops cleanly:\n" + run.diagnostics.text());
        expect(run.diagnostics.text().find("shard 0: recovered books=2 replayed=5 tail-cut-bytes=0 from=0 fell-back=0") != std::string::npos,
               "AAPL from its checkpoint at 0, through the log:\n" + run.diagnostics.text());
    }
    expect(std::filesystem::exists(logs / "ckpt-AAPL-00000000000000000005.obckpt"), "and its stop publishes AAPL at the end");
    std::filesystem::remove_all(logs);
    std::filesystem::remove(list);
}

// What the log rebuilds differs from the live book: through the engine, exit 7, the symbol and
// first differing byte named, and the live image saved as .live. With no hook into a live book,
// it is the checkpoint the roll starts from that is changed, once the engine is ready: AAPL's
// empty book at 0, rewritten with a next trade ID of 5, which no command reaches, since none
// trades. The image's bytes 28 to 31 hold it (docs/checkpoint-format.md).
void testACleanStopThatFindsADifference() {
    const std::filesystem::path list = writeList("differs", "symbol,shard,orders,stops,levels\nAAPL,0,,,\n");
    const std::filesystem::path logs = logDirectory("differs");
    runtime::EngineSettings settings = settingsFor(list, 1);
    settings.logDir = logs.string();
    settings.durability = runtime::Durability::PROCESS;
    Run run(settings);
    const std::uint16_t port = run.port();
    const std::filesystem::path empty = logs / "ckpt-AAPL-00000000000000000000.obckpt";
    const std::string held = bytesOf(empty);
    std::vector<std::byte> bytes(held.size());
    std::transform(held.begin(), held.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto decoded = runtime::decodeCheckpoint(bytes);
    const auto* checkpoint = std::get_if<runtime::DecodedCheckpoint>(&decoded);
    expect(checkpoint != nullptr, "the start's empty checkpoint decodes");
    std::vector<std::byte> image(checkpoint->image.begin(), checkpoint->image.end());
    image[28] = std::byte{5};
    const std::vector<std::byte> changed = runtime::encodeCheckpoint(checkpoint->header, image);
    std::ofstream(empty, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(changed.data()), static_cast<std::streamsize>(changed.size()));

    Bytes stream = helloFrame();
    const Bytes commands = commandsFor("AAPL", 3);  // buys at one price: nothing trades
    stream.insert(stream.end(), commands.begin(), commands.end());
    expect(talk(port, stream).size() == 3, "every command was answered");
    run.stop.request();
    expect(run.finish() == EngineExit::NOT_PUBLISHED, "a book that differs exits 7:\n" + run.diagnostics.text());
    expect(run.diagnostics.text().find("order_book_engine: clean stop incomplete: AAPL differs at byte 28, saved "
                                       "ckpt-AAPL-00000000000000000003.live; the next start recovers from the log\n") != std::string::npos,
           "naming the symbol, the byte, and the file:\n" + run.diagnostics.text());
    expect(run.report.text().find("stopped\n") != std::string::npos, "the stop report still prints");
    expect(!std::filesystem::exists(logs / "ckpt-AAPL-00000000000000000003.obckpt"), "nothing published for AAPL at the end");
    const std::string live = bytesOf(logs / "ckpt-AAPL-00000000000000000003.live");
    std::vector<std::byte> liveBytes(live.size());
    std::transform(live.begin(), live.end(), liveBytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto saved = runtime::decodeCheckpoint(liveBytes);
    const auto* savedCheckpoint = std::get_if<runtime::DecodedCheckpoint>(&saved);
    expect(savedCheckpoint != nullptr && savedCheckpoint->header.position == 3 && savedCheckpoint->image.size() > 31 &&
               savedCheckpoint->image[28] == std::byte{1},
           "the .live file is a checkpoint at 3 of the live book, whose next trade ID is still 1");
    std::filesystem::remove_all(logs);
    std::filesystem::remove(list);
}

// Every log setting that can't run refuses the start, and a start refused or stopped after its
// logs were made leaves the directory as it found it.
// --checkpoint-dir: checkpoints, and a lock of their own, in another directory, from which a
// restart recovers; the log's directory holds only the log.
void testACheckpointDirectory() {
    const std::filesystem::path list = writeList("checkpoint_dir", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,,,\n");
    const std::filesystem::path logs = logDirectory("checkpoint_dir_log");
    const std::filesystem::path checkpoints = logDirectory("checkpoint_dir_ckpt");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.logDir = logs.string();
    settings.checkpointDir = checkpoints.string();
    for (int run = 0; run < 2; ++run) {
        Run engine(settings);
        Bytes stream = helloFrame();
        const Bytes more = commandsFor("AAPL", 2);
        stream.insert(stream.end(), more.begin(), more.end());
        expect(talk(engine.port(), stream).size() == 2, "every command was answered");
        engine.stop.request();
        expect(engine.finish() == EngineExit::CLEAN_STOP, "a run with a checkpoint directory stops cleanly:\n" + engine.diagnostics.text());
        if (run == 1) {
            expect(engine.diagnostics.text().find("shard 0: recovered books=1 replayed=2 tail-cut-bytes=0 from=2 fell-back=0") != std::string::npos,
                   "the restart restores from the checkpoint directory:\n" + engine.diagnostics.text());
        }
    }
    expect(checkpointsIn(logs).empty() && !logsIn(logs).empty(), "the log's directory holds only the log");
    // The restart's worker may have rolled the first run's closed file too, so each book has one or two.
    const std::vector<std::string> held = checkpointsIn(checkpoints);
    for (const char* symbol : {"ckpt-AAPL-", "ckpt-MSFT-"}) {
        expect(std::any_of(held.begin(), held.end(), [symbol](const std::string& name) { return name.starts_with(symbol); }),
               std::string("and the checkpoint directory the checkpoints: ") + symbol);
    }
    expect(std::filesystem::exists(checkpoints / "engine.lock"), "with a lock of its own");

    // The log's own directory, named again, shares its lock: one directory, one lock.
    {
        // In a store of its own, since the log above keeps its checkpoints elsewhere.
        const std::filesystem::path both = logDirectory("checkpoint_dir_same");
        runtime::EngineSettings shared = settings;
        shared.logDir = both.string();
        shared.checkpointDir = both.string();
        for (int run = 0; run < 2; ++run) {
            Run same(shared);
            static_cast<void>(same.port());
            same.stop.request();
            expect(same.finish() == EngineExit::CLEAN_STOP,
                   "a checkpoint directory that is the log's takes no second lock:\n" + same.diagnostics.text());
        }
        expect(!checkpointsIn(both).empty(), "and holds the checkpoints beside the log");
        std::filesystem::remove_all(both);
    }
    // A checkpoint directory another holds refuses the start.
    settings.checkpointDir = checkpoints.string();
    {
        runtime::FileLogStorage other(checkpoints.string());
        const std::unique_ptr<runtime::StorageLock> otherLock = other.lock();
        StopSource stop;
        WatchedStream report;
        WatchedStream diagnostics;
        expect(runEngine(settings, stop, report, diagnostics) == EngineExit::START_REFUSED &&
                   diagnostics.text().find(" is locked: another engine or command is using ") != std::string::npos,
               "a locked checkpoint directory refuses the start:\n" + diagnostics.text());
    }
    {
        runtime::EngineSettings logless = settingsFor(list, 2);
        logless.checkpointDir = checkpoints.string();
        expect(refusedWith(logless, "--checkpoint-dir needs --log-dir, since there are no checkpoints without a log") == EngineExit::START_REFUSED,
               "a checkpoint directory with no log is refused");
    }
    settings.checkpointDir = (checkpoints / "missing").string();
    expect(refusedWith(settings, "cannot use checkpoint directory " + settings.checkpointDir + ": no such directory") ==
               EngineExit::START_REFUSED,
           "a checkpoint directory that doesn't exist refuses the start");
    std::filesystem::remove_all(logs);
    std::filesystem::remove_all(checkpoints);
    std::filesystem::remove(list);
}

void testLogRefusals() {
    const std::filesystem::path list = writeList("log_refusals", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,,,\n");
    const std::filesystem::path logs = logDirectory("refusals");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.logDir = (logs / "missing").string();
    expect(refusedWith(settings, "cannot use log directory " + settings.logDir + ": no such directory") == EngineExit::START_REFUSED,
           "a log directory must exist");
    settings.logDir = logs.string();
    settings.logRing = 1000;
    expect(refusedWith(settings, "--log-ring must be a power of two, not 1000") == EngineExit::START_REFUSED, "the log ring is a ring");
    settings.logRing = runtime::defaultLogRingRecords;
    const runtime::InstrumentDirectory directory(
        {runtime::Instrument{*Symbol::parse("AAPL"), 0, std::nullopt, std::nullopt, std::nullopt},
         runtime::Instrument{*Symbol::parse("MSFT"), 1, std::nullopt, std::nullopt, std::nullopt}},
        2);
    const std::uint64_t least = runtime::leastLogFileBytes(directory);
    settings.logFileSize = static_cast<std::size_t>(least - 1);
    expect(refusedWith(settings, "--log-file-size must be at least " + std::to_string(least) +
                                     " bytes, the largest shard's header and one record, not " + std::to_string(least - 1)) ==
               EngineExit::START_REFUSED,
           "a log file must hold a record");
    expect(filesIn(logs).empty(), "and that refusal makes no file, not even the lock's");
    settings.logFileSize = runtime::defaultLogFileBytes;
    // A file being made when an engine crashed held nothing anyone was told of, so it goes.
    std::ofstream(logs / "log-0000-00000000000000000002.oblog.tmp").put('x');
    expectACleanRun(settings, "a file left being made doesn't stop a start");
    expect(logsIn(logs) == std::vector<std::string>{"log-0000-00000000000000000000.oblog", "log-0001-00000000000000000000.oblog"},
           "and is gone");
    std::filesystem::remove_all(logs);
    std::filesystem::create_directories(logs);
    settings = settingsFor(list, 2);
    settings.pinLogWriters = {0, 0};
    expect(refusedWith(settings, "--pin-log-writers needs --log-dir, since there are no log writers without a log") ==
               EngineExit::START_REFUSED,
           "log writers pin only with a log");
    settings.logDir = logs.string();
    settings.pinLogWriters = {0};
    expect(refusedWith(settings, "--pin-log-writers needs 2 cores, one per shard, not 1") == EngineExit::START_REFUSED,
           "a core for each shard's log writer");

    settings.pinLogWriters = {aCoreThisProcessMayUse(), 4'000'000'000u};
    const std::string past = *runtime::pinThisThread(4'000'000'000u);
    expect(refusedWith(settings, "cannot pin log writer 1 to core 4000000000: " + past) == EngineExit::START_REFUSED,
           "a core one log writer can't take refuses the start, naming it");
    expect(filesIn(logs) == std::vector<std::string>{"engine.lock"}, "and the log shard 0 made is gone, leaving only the lock file");

    settings.pinLogWriters.clear();
    StopSource stop;
    stop.request();
    WatchedStream report;
    WatchedStream diagnostics;
    expect(runEngine(settings, stop, report, diagnostics) == EngineExit::STOPPED_BEFORE_READY, "a logged stop before ready exits 6");
    expect(storeIn(logs).empty(), "and leaves no log or checkpoint, since nothing ran");
    std::filesystem::remove_all(logs);
    std::filesystem::remove(list);
}

void testTheConnectionLimitIsASetting() {
    const std::filesystem::path list = writeList("limit", "symbol,shard,orders,stops,levels\nAAPL,,,,\n");
    runtime::EngineSettings settings = settingsFor(list, 1);
    settings.maxConnections = 1;
    Run run(settings);
    const std::uint16_t port = run.port();
    const TcpSocket first = TcpSocket::connectLoopback(port);
    const Bytes hello = helloFrame();
    expect(first.send(hello) == hello.size(), "the first says Hello");
    std::array<std::byte, 64> welcome{};
    expect(first.receive(welcome).bytes == 24, "and is welcomed");

    const TcpSocket second = TcpSocket::connectLoopback(port);
    FrameReader reader(FrameSource::ENGINE);
    std::array<std::byte, 64> buffer{};
    for (ReadResult read = second.receive(buffer); read.status == ReadStatus::OPEN; read = second.receive(buffer)) {
        reader.feed(std::span<const std::byte>(buffer).first(read.bytes));
    }
    const FrameResult result = reader.next();
    expect(std::holds_alternative<Frame>(result) && std::get<Frame>(result).type == FrameType::PROTOCOL_ERROR,
           "the second hears it is one too many");
    expect(decodeProtocolError(std::get<Frame>(result)).code == static_cast<std::uint16_t>(ProtocolError::TOO_MANY_CONNECTIONS), "saying so");
    run.stop.request();
    expect(run.finish() == EngineExit::CLEAN_STOP, "and the run still stops cleanly");
    std::filesystem::remove(list);
}

// Thread 0 deals connections in turn, so with two gateway threads and a limit of one each, the
// first two are taken on, one per thread, and the third is one too many on thread 0. Each thread
// answers its own connection's commands.
void testSeveralGatewayThreads() {
    const std::filesystem::path list = writeList("threads", "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,,,\n");
    runtime::EngineSettings settings = settingsFor(list, 2);
    settings.gatewayThreads = 2;
    settings.maxConnections = 1;
    Run run(settings);
    const std::uint16_t port = run.port();

    std::vector<TcpSocket> held;
    for (const char* symbol : {"AAPL", "MSFT"}) {
        held.push_back(TcpSocket::connectLoopback(port));
        Bytes stream = helloFrame();
        Bytes frame(64);
        frame.resize(encodeCommand(1, *Symbol::parse(symbol), limit(1, Side::BUY, 100), frame));
        stream.insert(stream.end(), frame.begin(), frame.end());
        expect(held.back().send(stream) == stream.size(), "each sends Hello and a command");

        FrameReader reader(FrameSource::ENGINE);
        std::array<std::byte, 256> buffer{};
        std::optional<EventFrame> answer;
        while (!answer) {
            const ReadResult read = held.back().receive(buffer);
            expect(read.status == ReadStatus::OPEN, std::string("the connection stays open until answered: ") + symbol);
            reader.feed(std::span<const std::byte>(buffer).first(read.bytes));
            for (FrameResult result = reader.next(); std::holds_alternative<Frame>(result) && !answer; result = reader.next()) {
                answer = decodeEvent(std::get<Frame>(result));
            }
        }
        expect(answer->symbol == *Symbol::parse(symbol) && std::holds_alternative<OrderAccepted>(answer->event),
               std::string("and is answered by its own thread: ") + symbol);
    }

    const TcpSocket third = TcpSocket::connectLoopback(port);
    FrameReader reader(FrameSource::ENGINE);
    std::array<std::byte, 64> buffer{};
    for (ReadResult read = third.receive(buffer); read.status == ReadStatus::OPEN; read = third.receive(buffer)) {
        reader.feed(std::span<const std::byte>(buffer).first(read.bytes));
    }
    const FrameResult refused = reader.next();
    expect(std::holds_alternative<Frame>(refused) &&
               decodeProtocolError(std::get<Frame>(refused)).code == static_cast<std::uint16_t>(ProtocolError::TOO_MANY_CONNECTIONS),
           "the third is thread 0's second, one too many, though thread 1 has room");

    held.clear();
    run.stop.request();
    expect(run.finish() == EngineExit::CLEAN_STOP, "and every gateway thread stops cleanly");
    expect(run.report.text().ends_with("total: books=2 capacity-growths=0\n"), "with the stop report:\n" + run.report.text());
    std::filesystem::remove(list);
}

}  // namespace

int main() {
    testALoggedRun();
    testALoggedRunAcrossFiles();
    testACleanStopThatLeavesABookUnpublished();
    testACleanStopThatFindsADifference();
    testACheckpointDirectory();
    testLogRefusals();
    testRecoveryRefusals();
    testACleanStop();
    testAStopBeforeReady();
    testRefusals();
    testPinningAndPriority();
    testTheConnectionLimitIsASetting();
    testSeveralGatewayThreads();
    std::cout << "engine tests passed" << std::endl;
    return EXIT_SUCCESS;
}
