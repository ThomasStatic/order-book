#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "child_process.hpp"
#include "engine/gateway_frames.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"

// order_book_engine as a real child process, through ChildProcess, which every wait here limits.
// Takes the engine's path. Threaded, so ThreadSanitizer and Arm run it. See
// docs/internals/testing.md.
namespace {

using namespace order_book;
using namespace order_book::network;
using order_book::process_testing::ChildProcess;
using order_book::process_testing::ExitStatus;
using namespace std::chrono_literals;

std::string engine;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// What a stream said, with Windows' text-mode line endings made plain.
std::string plain(std::string text) {
    std::erase(text, '\r');
    return text;
}

std::filesystem::path writeList(const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("order_book_engine_process_test_" + name + ".csv");
    std::ofstream(path, std::ios::binary) << "symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,1,,,\n";
    return path;
}

// A new, empty directory for one engine's log.
std::filesystem::path freshLogDir(const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("order_book_engine_process_test_logs_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<std::string> startArguments(const std::filesystem::path& list, const std::filesystem::path& logs, std::uint16_t port = 0) {
    return {"--instruments", list.string(), "--shards", "2", "--listen", "127.0.0.1:" + std::to_string(port), "--log-dir", logs.string(),
            "--stop-on-stdin-close"};
}

std::uint16_t listeningPort(ChildProcess& child) {
    const std::optional<std::string> line = child.readLine(30s);
    expect(line.has_value(), "the engine said where it listens, with stderr:\n" + child.standardError());
    expect(line->starts_with("listening 127.0.0.1:"), "on the address it was given: " + *line);
    return static_cast<std::uint16_t>(std::stoul(line->substr(line->rfind(':') + 1)));
}

void testAUsageError() {
    ChildProcess child(engine, {"--shards", "2"});
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == 2, "a usage error exits 2, not " + std::to_string(status.code));
    expect(plain(child.standardError()).starts_with("order_book_engine: --instruments is required\nusage: order_book_engine"),
           "naming the problem, then the usage:\n" + child.standardError());
    expect(child.standardOutput().empty(), "and stdout stays empty");
}

void testHelpWinsAnywhere() {
    ChildProcess child(engine, {"--shards", "two", "--help", "--no-such-flag"});
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == 0, "--help exits 0, whatever else the line holds, not " + std::to_string(status.code));
    expect(plain(child.standardOutput()).starts_with("usage: order_book_engine --instruments FILE") &&
               plain(child.standardOutput()).ends_with("       order_book_engine --help\n"),
           "printing the usage on stdout:\n" + child.standardOutput());
    expect(child.standardError().empty(), "and nothing on stderr:\n" + child.standardError());
    expect(ChildProcess(engine, {"-h"}).wait(30s).code == 0, "-h too");
}

// The log is the one way an answer means anything, so a run with none is a usage error.
void testTheLogIsRequired() {
    ChildProcess child(engine, {"--instruments", "list.csv", "--shards", "1", "--listen", "127.0.0.1:0"});
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == 2, "no --log-dir exits 2, not " + std::to_string(status.code));
    expect(plain(child.standardError()).starts_with("order_book_engine: --log-dir is required\nusage: order_book_engine"),
           "naming it:\n" + child.standardError());
}

void testAStartRefusal() {
    const std::filesystem::path logs = freshLogDir("refusal");
    ChildProcess child(engine,
                       {"--instruments", "no/such/list.csv", "--shards", "1", "--listen", "127.0.0.1:0", "--log-dir", logs.string()});
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == 1, "a refused start exits 1");
    expect(plain(child.standardError()).ends_with("order_book_engine: start refused: cannot read no/such/list.csv\n"),
           "naming the cause:\n" + child.standardError());
    std::filesystem::remove_all(logs);
}

// A real engine, a real client, and closing stdin to stop it.
void testACleanStopOverStdin() {
    const std::filesystem::path list = writeList("clean");
    const std::filesystem::path logs = freshLogDir("clean");
    ChildProcess child(engine, startArguments(list, logs));
    const std::uint16_t port = listeningPort(child);

    const TcpSocket client = TcpSocket::connectLoopback(port);
    gateway_testing::Bytes stream = gateway_testing::helloFrame();
    replay::AddCommand add{};
    add.orderId = 1;
    add.side = Side::BUY;
    add.price = 100;
    add.quantity = 5;
    std::array<std::byte, 64> frame{};
    const std::size_t size = encodeCommand(7, *Symbol::parse("AAPL"), add, frame);
    stream.insert(stream.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
    expect(client.send(stream) == stream.size(), "the client sends Hello and one command");
    client.shutdownSend();
    FrameReader reader(FrameSource::ENGINE);
    std::vector<std::byte> kept;
    std::array<std::byte, 4096> buffer;
    for (ReadResult read = client.receive(buffer); read.status == ReadStatus::OPEN; read = client.receive(buffer)) {
        kept.insert(kept.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read.bytes));
    }
    reader.feed(kept);
    FrameResult welcome = reader.next();
    FrameResult answer = reader.next();
    expect(std::holds_alternative<Frame>(answer) && std::get<Frame>(answer).correlation == 7 && std::get<Frame>(answer).last,
           "and is answered, under its correlation");
    static_cast<void>(welcome);

    child.closeStdin();
    const ExitStatus status = child.wait(30s);
    expect(!status.killed && status.code == 0, "closing stdin stops it cleanly, with exit 0, not " + std::to_string(status.code));
    const std::string report = plain(child.standardOutput());
    expect(report.find("\nstopped\nbook AAPL shard=") != std::string::npos && report.ends_with("total: books=2 capacity-growths=0\n"),
           "then prints its stop report:\n" + report);
    expect(child.standardError().starts_with("order_book_engine: --instruments "), "and its record went to stderr first");
    std::filesystem::remove(list);
    std::filesystem::remove_all(logs);
}

void testABusyPort() {
    const std::filesystem::path list = writeList("busy");
    const std::filesystem::path firstLogs = freshLogDir("busy_first");
    const std::filesystem::path secondLogs = freshLogDir("busy_second");
    ChildProcess first(engine, startArguments(list, firstLogs));
    const std::uint16_t port = listeningPort(first);
    ChildProcess second(engine, startArguments(list, secondLogs, port));
    const ExitStatus refused = second.wait(30s);
    const std::string how = refused.killed ? "killed" : "exit " + std::to_string(refused.code);
    expect(!refused.killed && refused.code == 1, "a second engine on the same port is refused, not " + how + ", with stdout:\n" +
                                                     second.standardOutput() + "\nand stderr:\n" + second.standardError());
    expect(second.standardError().find("start refused: cannot listen on 127.0.0.1:" + std::to_string(port)) != std::string::npos,
           "naming the address:\n" + second.standardError());
    first.closeStdin();
    expect(first.wait(30s).code == 0, "and the first still stops cleanly");
    std::filesystem::remove(list);
    std::filesystem::remove_all(firstLogs);
    std::filesystem::remove_all(secondLogs);
}

// Nothing stops this engine, so the wait's limit has to.
void testAWaitThatRunsOutKillsTheChild() {
    const std::filesystem::path list = writeList("forever");
    const std::filesystem::path logs = freshLogDir("forever");
    ChildProcess child(engine, {"--instruments", list.string(), "--shards", "2", "--listen", "127.0.0.1:0", "--log-dir", logs.string()});
    listeningPort(child);
    const ExitStatus status = child.wait(200ms);
    expect(status.killed, "a wait that runs out kills the child");
    std::filesystem::remove(list);
    std::filesystem::remove_all(logs);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: engine_process_tests ENGINE" << std::endl;
        return EXIT_FAILURE;
    }
    engine = argv[1];
    testAUsageError();
    testHelpWinsAnywhere();
    testTheLogIsRequired();
    testAStartRefusal();
    testACleanStopOverStdin();
    testABusyPort();
    testAWaitThatRunsOutKillsTheChild();
    std::cout << "engine process tests passed" << std::endl;
    return EXIT_SUCCESS;
}
