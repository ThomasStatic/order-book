#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "order_book/cli/commands.hpp"
#include "order_book/network/client_stream.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/network/in_memory_socket_set.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/log_storage.hpp"

// The whole engine in this process, through the injected-parts entry: gateway threads over a
// THREADED InMemorySocketSet, the log and checkpoints in an InMemoryLogStorage, and a stop this
// test sends. It stands in, under Emscripten, for the suites that need TCP or a child process.
// See docs/internals/network.md.
namespace {

using namespace order_book;
using namespace order_book::network;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "can read " + path.string());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A stream the engine writes from its own threads; read once the run has returned.
struct Streams {
    std::ostringstream report;
    std::ostringstream diagnostics;
};

// One engine run on a thread of its own, its gateway threads on members of one network.
class Run {
public:
    Run(const runtime::EngineSettings& runSettings, std::shared_ptr<runtime::LogStorage> storage)
        : settings(runSettings), network(std::make_shared<InMemorySocketSet>(InMemorySocketSet::Mode::THREADED)) {
        EngineParts parts;
        parts.sockets = [this](std::size_t gatewayThread) -> std::shared_ptr<SocketSet> {
            if (gatewayThread == 0) {
                return network;
            }
            return std::make_shared<InMemorySocketSet>(InMemorySocketSet::MemberOf{}, *network);
        };
        parts.logStorage = std::move(storage);
        parts.ready = [this] { ready.set_value(); };
        readyFuture = ready.get_future();
        thread = std::thread([this, moved = std::move(parts)]() mutable {
            exit = runEngine(settings, std::move(moved), stop, streams->report, streams->diagnostics);
            done.set_value();
        });
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    ~Run() {
        if (thread.joinable()) {
            stop.request();
            thread.join();
        }
    }

    void waitUntilReady() {
        const bool readied = readyFuture.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        expect(readied, "the engine became ready, with:\n" + streams->diagnostics.str());
    }

    // Waits for runEngine to return, and joins its thread.
    EngineExit finish() {
        const bool returned = done.get_future().wait_for(std::chrono::seconds(60)) == std::future_status::ready;
        expect(returned, "the run returned");
        thread.join();
        return *exit;
    }

    runtime::EngineSettings settings;
    std::shared_ptr<InMemorySocketSet> network;
    StopSource stop;
    // Kept apart from the run, since a forced stop leaves the engine's threads writing to them.
    std::shared_ptr<Streams> streams = std::make_shared<Streams>();
    std::optional<EngineExit> exit;

private:
    std::promise<void> ready;
    std::future<void> readyFuture;
    std::promise<void> done;
    std::thread thread;
};

// The sample's symbols, with no shard pinned, so each goes where the hash puts it.
runtime::EngineSettings settingsFor(const std::filesystem::path& list) {
    runtime::EngineSettings settings;
    settings.instruments = list.string();
    settings.shards = 2;
    settings.durability = runtime::Durability::PROCESS;
    return settings;
}

// What `order_book_cli send <file>` prints for one connection's session: everything sent, a
// half-close, and every answer read to the end of the engine's stream.
cli::CommandResult send(InMemorySocketSet& network, const std::string& file) {
    const std::vector<std::string_view> args{file, "--connect", "127.0.0.1:9000"};
    std::variant<cli::PreparedSend, cli::CommandResult> prepared = cli::prepareSend(args);
    if (const auto* refused = std::get_if<cli::CommandResult>(&prepared)) {
        expect(false, "the file is sent, not refused: " + refused->err);
    }
    cli::PreparedSend& session = std::get<cli::PreparedSend>(prepared);
    ClientStream stream(std::move(session.commands));
    const SocketId socket = network.connect();
    for (std::span<const std::byte> out = stream.outgoing(); !out.empty(); out = stream.outgoing()) {
        network.clientSend(socket, out);
        stream.wrote(out.size());
    }
    expect(stream.allWritten(), "every command is written");
    network.clientHalfClose(socket);
    while (stream.reading()) {
        const std::vector<std::byte> answer = network.clientReceiveWaiting(socket);
        if (answer.empty()) {
            stream.ended();
        } else {
            stream.received(answer);
        }
    }
    return cli::finishSend(session, stream);
}

// Start, a session whose answers are what replay prints for the same file, a clean stop, and a
// second start that recovers the first's log and checkpoints from the same store.
void testASessionThenACleanStop(const std::filesystem::path& replayDir, const std::filesystem::path& list) {
    const auto storage = std::make_shared<runtime::InMemoryLogStorage>();
    {
        Run run(settingsFor(list), storage);
        run.waitUntilReady();
        const cli::CommandResult sent = send(*run.network, (replayDir / "valid/v3_two_instruments.csv").string());
        expect(sent.exit == 0 && sent.err.empty(), "every command was answered:\n" + sent.err);
        expect(sent.out == readFile(replayDir / "expected/v3_two_instruments_no_statistics.stdout"),
               "the answers print as replay --events --no-statistics prints the file:\n" + sent.out);

        run.stop.request();
        expect(run.finish() == EngineExit::CLEAN_STOP, "a clean stop exits 0:\n" + run.streams->diagnostics.str());
        const std::string report = run.streams->report.str();
        expect(report.starts_with("stopped\n") && report.find("\ntotal: books=4 ") != std::string::npos,
               "the report starts with the stop and counts the four books, with nothing printed at ready:\n" + report);
        expect(!storage->names().empty(), "the log and checkpoints are in the store it was given");
    }

    Run again(settingsFor(list), storage);
    again.waitUntilReady();
    again.stop.request();
    expect(again.finish() == EngineExit::CLEAN_STOP, "the restart stops cleanly too:\n" + again.streams->diagnostics.str());
    // A clean stop publishes every book where its log ends, so each shard's replay begins at its
    // log's end (from= equals replayed=, which is that end), and the two ends hold the file's six
    // records, for the four books.
    const std::string diagnostics = again.streams->diagnostics.str();
    const std::regex recovered("shard [01]: recovered books=([0-9]+) replayed=([0-9]+) tail-cut-bytes=0 from=([0-9]+) fell-back=0");
    std::uint64_t records = 0;
    std::uint64_t books = 0;
    std::size_t shards = 0;
    for (auto line = std::sregex_iterator(diagnostics.begin(), diagnostics.end(), recovered); line != std::sregex_iterator(); ++line) {
        const bool atTheEnd = (*line)[2].str() == (*line)[3].str();
        expect(atTheEnd, "the replay begins at the log's end:\n" + diagnostics);
        books += std::stoull((*line)[1].str());
        records += std::stoull((*line)[2].str());
        ++shards;
    }
    expect(shards == 2 && records == 6 && books == 4, "both shards recovered, six records and four books between them:\n" + diagnostics);
}

// A clean stop held by answers that can't be delivered, then a second stop, which forces it.
// The engine is left running, as a forced stop leaves it, so this case is a run of its own.
void testAForcedStop(const std::filesystem::path& replayDir, const std::filesystem::path& list) {
    runtime::EngineSettings settings = settingsFor(list);
    settings.stopGrace = std::chrono::seconds(600);
    // Each change to the log, which follows every command the engine reads, wakes the wait below.
    struct Changes {
        std::mutex guard;
        std::condition_variable changed;
    };
    auto* changes = new Changes;  // left with the engine's threads, which still tell it
    const auto storage = std::make_shared<runtime::InMemoryLogStorage>();
    storage->watch([changes](const runtime::InMemoryLogStorage::Contents&) {
        const std::lock_guard<std::mutex> held(changes->guard);
        changes->changed.notify_all();
    });
    auto* run = new Run(settings, storage);  // left with its threads
    run->waitUntilReady();
    run->network->limitWrite(0);  // nothing reaches the client: the clean stop waits for it
    std::variant<cli::PreparedSend, cli::CommandResult> prepared =
        cli::prepareSend(std::vector<std::string_view>{(replayDir / "valid/v3_two_instruments.csv").string(), "--connect", "127.0.0.1:9000"});
    expect(std::holds_alternative<cli::PreparedSend>(prepared), "the file is sent");
    ClientStream stream(std::move(std::get<cli::PreparedSend>(prepared).commands));
    const SocketId socket = run->network->connect();
    std::size_t sent = 0;
    for (std::span<const std::byte> out = stream.outgoing(); !out.empty(); out = stream.outgoing()) {
        run->network->clientSend(socket, out);
        stream.wrote(out.size());
        sent += out.size();
    }
    // Every command read, so each one's answer is owed and can't be sent: the engine reads them
    // before it logs the last.
    {
        std::unique_lock<std::mutex> held(changes->guard);
        const bool read = changes->changed.wait_for(held, std::chrono::seconds(60), [&] { return run->network->bytesRead(socket) >= sent; });
        expect(read, "the engine read every command");
    }
    run->stop.request();
    run->stop.request();
    expect(run->finish() == EngineExit::FORCED_STOP, "a second stop during a clean stop forces it, exiting 8:\n" +
                                                         run->streams->diagnostics.str());
    expect(run->streams->report.str().empty(), "and prints no stop report");
}

}  // namespace

int main(int argc, char** argv) {
    expect(argc >= 3, "usage: engine_in_process_tests <tests/replay> <scratch directory> [forced]");
    const std::filesystem::path replayDir = argv[1];
    const std::filesystem::path scratch = argv[2];
    std::filesystem::create_directories(scratch);
    const std::filesystem::path list = scratch / "instruments.csv";
    std::ofstream(list, std::ios::binary) << "symbol,shard,orders,stops,levels\nAAPL,,,,\nBRK.B,,,,\nMSFT,,,,\nZZZ,,,,\n";
    if (argc > 3 && std::string_view(argv[3]) == "forced") {
        testAForcedStop(replayDir, list);
        std::cout << "engine in-process forced stop passed" << std::endl;
        std::quick_exit(EXIT_SUCCESS);  // the forced engine's threads are still running
    }
    testASessionThenACleanStop(replayDir, list);
    std::cout << "engine in-process tests passed" << std::endl;
    return EXIT_SUCCESS;
}
