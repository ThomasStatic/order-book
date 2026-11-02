#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "child_process.hpp"
#include "engine/gateway_frames.hpp"
#include "engine_handlers.hpp"
#include "order_book/invariants.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/abort_report.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/shard.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Real signals sent to a real order_book_engine, and the abort handler apps/ installs, in a child
// of this test. On Windows the stop is Ctrl-Break through this process's console, and the test
// reports Skipped if it can't get one. Takes the engine's path. See docs/internals/testing.md.

// Counts every allocation, to show the abort line needs none.
namespace {
std::atomic<std::size_t> allocations{0};
}

void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace {

using namespace order_book;
using namespace order_book::network;
using order_book::process_testing::ChildOptions;
using order_book::process_testing::ChildProcess;
using order_book::process_testing::ExitStatus;
using namespace std::chrono_literals;

constexpr int skipped = 77;
std::string engine;
std::string self;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string plain(std::string text) {
    std::erase(text, '\r');
    return text;
}

std::string described(const ExitStatus& status) {
    return status.killed ? "killed by the wait" : "exit " + std::to_string(status.code);
}

// What an abort ends with: SIGABRT on Linux, and status 3 from the Windows runtime's abort.
#ifdef _WIN32
constexpr int abortStatus = 3;
#else
constexpr int abortStatus = 128 + SIGABRT;
#endif

runtime::InstrumentDirectory directoryOf(const std::string& list, std::uint32_t shards) {
    runtime::InstrumentListResult parsed = runtime::parseInstrumentList(list, shards);
    expect(std::holds_alternative<std::vector<runtime::Instrument>>(parsed), "the test's list parses");
    return runtime::InstrumentDirectory(std::get<std::vector<runtime::Instrument>>(std::move(parsed)), shards);
}

replay::CancelCommand cancel(OrderId id) {
    replay::CancelCommand command{};
    command.orderId = id;
    return command;
}

// The child half of the abort tests: a shard, named for the handler, takes three commands on
// MSFT, and then this thread aborts.
[[noreturn]] void abortOnAShard() {
    engine_app::installAbortHandler();
    const runtime::InstrumentDirectory directory = directoryOf("symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,0,,,\n", 1);
    runtime::Shard shard(directory, 0);
    const std::uint32_t slot = directory.find(*Symbol::parse("MSFT"))->slot;
    const runtime::AbortScope abortNamesThisShard(shard);
    for (OrderId id = 1; id <= 3; ++id) {
        shard.process(cancel(id), slot);
    }
    std::abort();
}

[[noreturn]] void abortElsewhere() {
    engine_app::installAbortHandler();
    std::thread([] { std::abort(); }).join();
    std::abort();
}

std::string abortLineOnAShard() {
    return invariantChecksAfterCommands ? "order_book_engine: aborted: shard 0 symbol MSFT position 2\n" : "order_book_engine: aborted\n";
}

void testTheAbortLineAllocatesNothing() {
    std::array<char, 160> buffer{};
    const std::size_t before = allocations.load();
    const std::size_t size = runtime::describeAbort(buffer);
    const std::size_t made = allocations.load() - before;  // before any message is built
    expect(made == 0, "the abort line for a thread with no shard allocates nothing, not " + std::to_string(made));
    expect(std::string_view(buffer.data(), size) == "order_book_engine: aborted\n", "and names none");

    const runtime::InstrumentDirectory directory = directoryOf("symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,0,,,\n", 1);
    runtime::Shard shard(directory, 0);
    const std::uint32_t slot = directory.find(*Symbol::parse("MSFT"))->slot;
    for (OrderId id = 1; id <= 3; ++id) {
        shard.process(cancel(id), slot);
    }
    {
        const runtime::AbortScope abortNamesThisShard(shard);
        const std::size_t again = allocations.load();
        const std::size_t written = runtime::describeAbort(buffer);
        const std::size_t madeOnAShard = allocations.load() - again;
        expect(madeOnAShard == 0, "nor for a thread running a shard, not " + std::to_string(madeOnAShard));
        expect(std::string_view(buffer.data(), written) == abortLineOnAShard(),
               "which names where it stood in a checked build: " + std::string(buffer.data(), written));
        std::array<char, 10> small{};
        expect(runtime::describeAbort(small) == small.size() && std::string_view(small.data(), small.size()) == "order_book",
               "cut to fit a smaller buffer");
    }
    expect(runtime::describeAbort(buffer) == std::string_view("order_book_engine: aborted\n").size(), "and forgotten when its scope ends");
}

void testTheAbortHandler() {
    for (const auto& [mode, line] : {std::pair{std::string("--abort-on-a-shard"), abortLineOnAShard()},
                                     std::pair{std::string("--abort-elsewhere"), std::string("order_book_engine: aborted\n")}}) {
        ChildProcess child(self, {mode});
        const ExitStatus status = child.wait(30s);
        expect(!status.killed && status.code == abortStatus,
               mode + ": the process dies as an abort does, not " + described(status) + ", with stderr:\n" + child.standardError());
        expect(plain(child.standardError()).ends_with(line), mode + ": after printing the abort line:\n" + child.standardError());
    }
}

std::filesystem::path writeList(const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("order_book_engine_signal_test_" + name + ".csv");
    std::ofstream(path, std::ios::binary) << "symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,1,,,\n";
    return path;
}

std::vector<std::string> startArguments(const std::filesystem::path& list, std::vector<std::string> more = {}) {
    std::vector<std::string> arguments{"--instruments", list.string(), "--shards", "2", "--listen", "127.0.0.1:0"};
    arguments.insert(arguments.end(), more.begin(), more.end());
    return arguments;
}

std::uint16_t listeningPort(ChildProcess& child) {
    const std::optional<std::string> line = child.readLine(30s);
    expect(line.has_value() && line->starts_with("listening 127.0.0.1:"), "the engine listens, with stderr:\n" + child.standardError());
    return static_cast<std::uint16_t>(std::stoul(line->substr(line->rfind(':') + 1)));
}

// One stop, as an operator sends it.
#ifdef _WIN32
constexpr std::array stopSignals{0};
const char* nameOf(int) {
    return "Ctrl-Break";
}
void sendStop(ChildProcess& child, int) {
    expect(child.sendCtrlBreak(), "Ctrl-Break was sent");
}
#else
constexpr std::array stopSignals{SIGINT, SIGTERM, SIGHUP};
const char* nameOf(int signal) {
    return signal == SIGINT ? "SIGINT" : signal == SIGTERM ? "SIGTERM" : "SIGHUP";
}
void sendStop(ChildProcess& child, int signal) {
    expect(child.sendSignal(signal), std::string(nameOf(signal)) + " was sent");
}
#endif

// The engine itself installs the handler: a SIGABRT from outside prints its line and ends the
// process as an abort would. Windows has no way to send one, so this is POSIX only.
void testTheEngineInstallsItsAbortHandler() {
#ifndef _WIN32
    const std::filesystem::path list = writeList("abort");
    ChildProcess child(engine, startArguments(list));
    listeningPort(child);
    expect(child.sendSignal(SIGABRT), "SIGABRT was sent");
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == abortStatus, "the engine dies by SIGABRT, not " + described(status));
    // Whichever thread the system picked: a matching thread's line names its shard too.
    const std::string said = child.standardError();
    const std::size_t lastLine = said.rfind('\n', said.size() - 2);
    expect(said.ends_with("\n") && said.compare(lastLine + 1, 26, "order_book_engine: aborted") == 0,
           "after printing the abort line:\n" + said);
    std::filesystem::remove(list);
#endif
}

void testEachStopSignalStopsCleanly() {
    const std::filesystem::path list = writeList("clean");
    for (const int signal : stopSignals) {
        ChildProcess child(engine, startArguments(list), ChildOptions{true});
        listeningPort(child);
        sendStop(child, signal);
        const ExitStatus status = child.wait(30s);
        expect(!status.killed && status.code == 0, std::string(nameOf(signal)) + " stops cleanly with exit 0, not " + described(status) +
                                                       ", with stderr:\n" + child.standardError());
        expect(plain(child.standardOutput()).ends_with("\ntotal: books=2 capacity-growths=0\n"),
               std::string(nameOf(signal)) + " then prints the stop report:\n" + child.standardOutput());
    }
    std::filesystem::remove(list);
}

// A client that sends and never reads, so the engine has answers it can't deliver and its stop
// waits out the grace, 60 s here: the second stop must end it long before.
void testASecondStopForcesIt() {
    const std::filesystem::path list = writeList("forced");
    ChildProcess child(engine, startArguments(list, {"--stop-grace", "60s"}), ChildOptions{true});
    const TcpSocket client = TcpSocket::connectLoopback(listeningPort(child));

    std::vector<std::byte> chunk = gateway_testing::helloFrame();
    std::vector<std::byte> rest;
    for (OrderId id = 1; rest.size() < 64 * 1024; ++id) {
        std::array<std::byte, 64> frame{};
        const std::size_t size = encodeCommand(id, *Symbol::parse(id % 2 == 0 ? "AAPL" : "MSFT"), cancel(id), frame);
        rest.insert(rest.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
    }
    std::atomic<std::uint64_t> sent{0};
    std::thread sender([&] {
        std::span<const std::byte> left(chunk);
        for (;;) {
            if (left.empty()) {
                left = rest;
            }
            const std::optional<std::size_t> taken = client.send(left);
            if (!taken) {
                return;  // the engine is gone
            }
            left = left.subspan(*taken);
            sent.fetch_add(*taken);
        }
    });

    // Once the engine stops reading, the client's sends stall.
    std::uint64_t last = 0;
    auto unchangedSince = std::chrono::steady_clock::now();
    const auto giveUp = unchangedSince + 60s;
    while (std::chrono::steady_clock::now() - unchangedSince < 1500ms) {
        expect(std::chrono::steady_clock::now() < giveUp, "the engine stopped reading a client that never reads");
        std::this_thread::sleep_for(100ms);
        if (const std::uint64_t now = sent.load(); now != last) {
            last = now;
            unchangedSince = std::chrono::steady_clock::now();
        }
    }

    sendStop(child, stopSignals[0]);
    expect(child.stillRunningAfter(2s), "the first stop waits for the answers it owes, with stderr:\n" + child.standardError());
    sendStop(child, stopSignals[stopSignals.size() - 1]);
    const ExitStatus status = child.wait(20s);
    expect(!status.killed && status.code == 8, "a second stop exits at once with 8, not " + described(status) + ", with stderr:\n" +
                                                   child.standardError());
    expect(plain(child.standardError()).ends_with("order_book_engine: forced stop\n"), "saying so:\n" + child.standardError());
    expect(plain(child.standardOutput()).find("stopped") == std::string::npos, "with no stop report:\n" + child.standardOutput());
    sender.join();
    std::filesystem::remove(list);
}

// A console this process shares with the engine, for Ctrl-Break: the one it has, or a new one.
bool haveAConsole() {
#ifdef _WIN32
    return AllocConsole() || GetLastError() == ERROR_ACCESS_DENIED;
#else
    return true;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    self = argv[0];
    if (argc == 2 && std::string_view(argv[1]) == "--abort-on-a-shard") {
        abortOnAShard();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--abort-elsewhere") {
        abortElsewhere();
    }
    if (argc != 2) {
        std::cerr << "usage: engine_signal_tests ENGINE" << std::endl;
        return EXIT_FAILURE;
    }
    engine = argv[1];
    testTheAbortLineAllocatesNothing();
    testTheAbortHandler();
    testTheEngineInstallsItsAbortHandler();
    if (!haveAConsole()) {
        std::cout << "SKIPPED: no console to send Ctrl-Break through" << std::endl;
        return skipped;
    }
    testEachStopSignalStopsCleanly();
    testASecondStopForcesIt();
    std::cout << "engine signal tests passed" << std::endl;
    return EXIT_SUCCESS;
}
