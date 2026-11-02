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
#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/thread_placement.hpp"

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
    settings.pinMatching = {core, core};
    settings.pinGateway = {core};
    expectACleanRun(settings, "a run pinned where it may run starts and stops cleanly");

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

}  // namespace

int main() {
    testACleanStop();
    testAStopBeforeReady();
    testRefusals();
    testPinningAndPriority();
    testTheConnectionLimitIsASetting();
    std::cout << "engine tests passed" << std::endl;
    return EXIT_SUCCESS;
}
