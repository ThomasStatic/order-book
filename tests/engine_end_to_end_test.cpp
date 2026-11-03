#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "child_process.hpp"
#include "order_book/command_log.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/symbol.hpp"

// order_book_cli send against a real order_book_engine, both child processes, compared byte for
// byte with order_book_cli replay of the same file. Takes the engine, the CLI, tests/replay, and
// tests/engine/samples. Threaded, so ThreadSanitizer and Arm run it. See docs/internals/testing.md.
namespace {

using namespace order_book;
using order_book::process_testing::ChildProcess;
using order_book::process_testing::ExitStatus;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

std::string engine;
std::string cli;
fs::path replaySamples;
fs::path engineSamples;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// What a stream said, with Windows' text-mode line endings made plain. The CLI writes binary.
std::string plain(std::string text) {
    std::erase(text, '\r');
    return text;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every symbol a file names, for an instrument list that takes them all.
std::set<std::string> symbolsIn(const fs::path& path) {
    std::set<std::string> symbols;
    for (const replay::ReplayRecord& record : replay::readReplayOrLogFile(path).records) {
        symbols.insert(record.symbol.text());
    }
    return symbols;
}

struct Run {
    ExitStatus status;
    std::string out;
    std::string err;
};

Run run(const std::vector<std::string>& arguments) {
    ChildProcess child(cli, arguments);
    const ExitStatus status = child.wait(60s);
    return Run{status, child.standardOutput(), child.standardError()};
}

std::string describe(const std::vector<std::string>& arguments, const Run& result) {
    std::string text = "order_book_cli";
    for (const std::string& argument : arguments) {
        text += ' ' + argument;
    }
    return text + (result.status.killed ? " was killed" : " exited " + std::to_string(result.status.code)) + ", with stdout:\n" +
           result.out + "\nand stderr:\n" + result.err;
}

// One engine on port 0 with these instruments, stopped by closing its stdin.
class Engine {
public:
    Engine(std::string named, const std::set<std::string>& symbols, std::uint32_t shards, std::uint32_t gatewayThreads = 1)
        : name(std::move(named)), list(fs::temp_directory_path() / ("order_book_end_to_end_" + std::to_string(++started) + ".csv")) {
        {
            std::ofstream out(list, std::ios::binary);
            out << "symbol,shard,orders,stops,levels\n";
            for (const std::string& symbol : symbols) {
                out << symbol << ",,,,\n";
            }
        }
        child.emplace(engine, std::vector<std::string>{"--instruments", list.string(), "--shards", std::to_string(shards), "--listen",
                                                       "127.0.0.1:0", "--gateway-threads", std::to_string(gatewayThreads),
                                                       "--stop-on-stdin-close"});
        const std::optional<std::string> line = child->readLine(30s);
        expect(line && line->starts_with("listening 127.0.0.1:"),
               name + ": the engine said where it listens, with stderr:\n" + child->standardError());
        address = line->substr(std::string("listening ").size());
    }

    ~Engine() {
        std::error_code ignored;
        fs::remove(list, ignored);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // A clean stop, after every connection has gone.
    void stop() {
        child->closeStdin();
        const ExitStatus status = child->wait(30s);
        expect(!status.killed && status.code == 0, name + ": closing stdin stops the engine with exit 0, not " +
                                                       std::to_string(status.code) + ", with stderr:\n" + child->standardError());
    }

    std::string name;
    std::string address;

private:
    static inline int started = 0;
    fs::path list;
    std::optional<ChildProcess> child;
};

// send prints exactly what replay --events --no-statistics prints, and exits as it does.
void expectSendMatchesReplay(const Engine& running, const fs::path& file, const std::optional<std::string>& symbol,
                             int exit = 0) {
    const std::string name = running.name + ": " + file.filename().string();
    std::vector<std::string> sendArguments{"send", file.string(), "--connect", running.address};
    std::vector<std::string> replayArguments{"replay", file.string(), "--events", "--no-statistics"};
    if (symbol) {
        for (std::vector<std::string>* arguments : {&sendArguments, &replayArguments}) {
            arguments->push_back("--symbol");
            arguments->push_back(*symbol);
        }
    }
    const Run sent = run(sendArguments);
    const Run replayed = run(replayArguments);
    expect(!sent.status.killed && sent.status.code == exit, name + ": " + describe(sendArguments, sent));
    expect(!replayed.status.killed && replayed.status.code == exit, name + ": " + describe(replayArguments, replayed));
    expect(sent.out == replayed.out, name + ": send prints replay's bytes; send:\n" + sent.out + "\nreplay:\n" + replayed.out);
    expect(plain(sent.err) == plain(replayed.err), name + ": and the same diagnostics; send:\n" + sent.err + "\nreplay:\n" + replayed.err);
}

// Every valid sample. A v1 or v2 file stands in for one instrument, each its own, so they share
// an engine, with two gateway threads, so thread 0 hands every other send's connection on; a v3
// file names its own, which may collide, so each has one.
void testEveryValidSample() {
    std::vector<fs::path> standIns;
    std::vector<fs::path> named;
    for (const fs::directory_entry& entry : fs::directory_iterator(replaySamples / "valid")) {
        const fs::path& path = entry.path();
        (replay::readReplayOrLogFile(path).format == replay::ReplayFormat::V3 ? named : standIns).push_back(path);
    }
    std::sort(standIns.begin(), standIns.end());
    std::sort(named.begin(), named.end());
    expect(standIns.size() >= 15 && named.size() >= 3, "the samples are where they should be");

    std::set<std::string> symbols;
    for (std::size_t i = 0; i < standIns.size(); ++i) {
        symbols.insert("S" + std::to_string(i));
    }
    Engine shared("stand-ins", symbols, 3, 2);
    for (std::size_t i = 0; i < standIns.size(); ++i) {
        expectSendMatchesReplay(shared, standIns[i], "S" + std::to_string(i));
    }
    shared.stop();

    for (const fs::path& path : named) {
        // A header-only file names nothing, and an engine needs an instrument.
        std::set<std::string> listed = symbolsIn(path);
        listed.insert("UNUSED");
        Engine own(path.stem().string(), listed, 2);
        expectSendMatchesReplay(own, path, std::nullopt);
        own.stop();
    }
}

// Seven instruments interleaved, on one shard and across three, where answers from different
// shards overtake each other on the wire, and across three with two gateway threads, where an idle
// connection made first takes thread 0, so send's is handed to thread 1.
void testManyInstrumentsAtOneAndThreeShards() {
    const fs::path file = engineSamples / "seven_instruments.csv";
    expect(symbolsIn(file).size() == 7, "the sample names seven instruments");
    for (const std::uint32_t shards : {1U, 3U}) {
        Engine running("seven instruments on " + std::to_string(shards), symbolsIn(file), shards);
        expectSendMatchesReplay(running, file, std::nullopt);
        running.stop();
    }
    Engine running("seven instruments on 3 shards and 2 gateway threads", symbolsIn(file), 3, 2);
    {
        const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(running.address.substr(running.address.rfind(':') + 1)));
        const network::TcpSocket idle = network::TcpSocket::connectLoopback(port);
        expectSendMatchesReplay(running, file, std::nullopt);
    }
    running.stop();
}

// A good prefix still runs, then the file's diagnostic, and exit 1.
void testAMalformedFileSendsItsPrefix() {
    const fs::path file = replaySamples / "malformed" / "symbol_lower_case.csv";
    Engine running("a malformed file", {"AAPL"}, 1);
    expectSendMatchesReplay(running, file, std::nullopt, 1);
    running.stop();
}

// A log is sniffed as replay sniffs it, so its records go out as its CSV twin's would.
void testALogSendsItsRecords() {
    const fs::path file = replaySamples / "log" / "every_command.oblog";
    Engine running("a log", symbolsIn(file), 2);
    expectSendMatchesReplay(running, file, std::nullopt);
    running.stop();
}

// The one case replay can't stand for: a command the engine refuses prints in place, and exits 4.
void testASymbolOffTheListExits4() {
    const fs::path file = replaySamples / "valid" / "v3_two_instruments.csv";
    Engine running("off the list", {"AAPL", "BRK.B", "MSFT"}, 2);
    const std::vector<std::string> arguments{"send", file.string(), "--connect", running.address};
    const Run sent = run(arguments);
    expect(!sent.status.killed && sent.status.code == 4, "a symbol off the list exits 4: " + describe(arguments, sent));
    expect(sent.out == readFile(engineSamples / "off_the_list.stdout"), "printing the refusal in place:\n" + sent.out);
    expect(sent.err.empty(), "with nothing on stderr: " + sent.err);
    running.stop();
}

void testNoEngineExits1() {
    std::uint16_t port = 0;
    {
        const network::TcpSocket listener = network::TcpSocket::listenLoopback(0);
        port = listener.localPort();
    }  // closed, so nothing listens there
    const std::vector<std::string> arguments{"send", (replaySamples / "valid" / "basic.csv").string(), "--symbol", "AAPL",
                                             "--connect", "127.0.0.1:" + std::to_string(port)};
    const Run sent = run(arguments);
    expect(!sent.status.killed && sent.status.code == 1, "no engine exits 1: " + describe(arguments, sent));
    expect(plain(sent.err).starts_with("order_book_cli: cannot connect to 127.0.0.1:" + std::to_string(port) + ": "),
           "naming the address: " + sent.err);
    expect(sent.out.empty(), "and printing nothing");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: engine_end_to_end_tests ENGINE CLI REPLAY_SAMPLES ENGINE_SAMPLES" << std::endl;
        return EXIT_FAILURE;
    }
    engine = argv[1];
    cli = argv[2];
    replaySamples = argv[3];
    engineSamples = argv[4];
    testEveryValidSample();
    testManyInstrumentsAtOneAndThreeShards();
    testAMalformedFileSendsItsPrefix();
    testALogSendsItsRecords();
    testASymbolOffTheListExits4();
    testNoEngineExits1();
    std::cout << "engine end-to-end tests passed" << std::endl;
    return EXIT_SUCCESS;
}
