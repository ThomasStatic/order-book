#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "child_process.hpp"
#include "order_book/command_log.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/symbol.hpp"

// order_book_cli send against a real order_book_engine, both child processes, compared byte for
// byte with order_book_cli replay of the same file, and then the engine's own log replayed, which
// must give the same books. Takes the engine, the CLI, tests/replay, and tests/engine/samples.
// Threaded, so ThreadSanitizer and Arm run it. See docs/internals/testing.md.
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

// One engine on port 0 with these instruments, stopped by closing its stdin, logging to a log
// directory of its own at process durability and in 1 MiB files unless told otherwise, or to an
// earlier engine's, which it then recovers.
class Engine {
public:
    Engine(std::string named, const std::set<std::string>& symbols, std::uint32_t shards, std::uint32_t gatewayThreads = 1,
           const std::string& durability = "process", const std::string& logFileSize = "1M",
           const std::optional<fs::path>& earlierLogs = std::nullopt)
        : name(std::move(named)), list(fs::temp_directory_path() / ("order_book_end_to_end_" + std::to_string(++started) + ".csv")) {
        logs = earlierLogs ? *earlierLogs : fs::temp_directory_path() / ("order_book_end_to_end_logs_" + std::to_string(started));
        {
            std::ofstream out(list, std::ios::binary);
            out << "symbol,shard,orders,stops,levels\n";
            for (const std::string& symbol : symbols) {
                out << symbol << ",,,,\n";
            }
        }
        if (!earlierLogs) {
            fs::remove_all(logs);
            fs::create_directories(logs);
        }
        const std::vector<std::string> arguments{"--instruments", list.string(), "--shards", std::to_string(shards), "--listen",
                                                 "127.0.0.1:0", "--gateway-threads", std::to_string(gatewayThreads), "--log-dir",
                                                 logs.string(), "--durability", durability, "--log-file-size", logFileSize,
                                                 // so the directory still replays from position 0, as the checks below need
                                                 "--keep-log", "--stop-on-stdin-close"};
        // Windows releases a killed process's file lock in its own time, so a restart may find
        // the lock still held for a moment; nothing else is retried.
        for (int attempt = 0;; ++attempt) {
            child.emplace(engine, arguments);
            if (const std::optional<std::string> line = child->readLine(60s); line && line->starts_with("listening 127.0.0.1:")) {
                address = line->substr(std::string("listening ").size());
                return;
            }
            const ExitStatus status = child->wait(30s);
            const std::string said = child->standardError();
            expect(!status.killed && status.code == 1 && said.find("engine.lock is locked") != std::string::npos && attempt < 100,
                   name + ": the engine said where it listens, with stderr:\n" + said);
            std::this_thread::sleep_for(100ms);
        }
    }

    ~Engine() {
        std::error_code ignored;
        fs::remove(list, ignored);
        fs::remove_all(logs, ignored);
    }

    // As a crash would end it, leaving its log as the crash left it.
    void kill() {
        child->kill();
        child->wait(30s);
    }

    // Everything the engine said, once it has stopped.
    std::string standardError() { return child->standardError(); }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // A clean stop, after every connection has gone. One that rolls many small files to its log's
    // end, flushing each book's checkpoint each time, needs longer on a slow shared runner.
    void stop(std::chrono::seconds allowed = 30s) {
        child->closeStdin();
        const ExitStatus status = child->wait(allowed);
        expect(!status.killed && status.code == 0, name + ": closing stdin stops the engine with exit 0, not " +
                                                       std::to_string(status.code) + ", with stderr:\n" + child->standardError());
    }

    std::string name;
    std::string address;
    fs::path logs;

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

// After a clean stop, the engine's own log reproduces the books: replay of its log directory prints
// what replay of what was sent prints, exit and diagnostics included unless the input itself was
// damaged, since a damaged file still runs its good prefix but exits 1.
void expectLogReplaysTo(const Engine& stopped, const std::vector<std::string>& fromInput, bool inputIsDamaged = false) {
    const std::vector<std::string> fromLog{"replay", stopped.logs.string(), "--book"};
    const Run logged = run(fromLog);
    const Run replayed = run(fromInput);
    expect(!logged.status.killed && logged.status.code == 0, stopped.name + ": " + describe(fromLog, logged));
    expect(logged.out == replayed.out, stopped.name + ": the log replays to the same books; log:\n" + logged.out + "\ninput:\n" + replayed.out);
    if (!inputIsDamaged) {
        expect(!replayed.status.killed && replayed.status.code == 0 && plain(logged.err) == plain(replayed.err),
               stopped.name + ": and says the same: " + describe(fromInput, replayed));
    }
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
    // The stand-ins' sections, one replay each, in the log's order: by symbol, with a blank line
    // between them, and none for a file with no commands.
    std::vector<std::string> sorted(symbols.begin(), symbols.end());
    std::sort(sorted.begin(), sorted.end());
    std::string sections;
    for (const std::string& symbol : sorted) {
        const std::size_t i = std::stoul(symbol.substr(1));
        const std::vector<std::string> arguments{"replay", standIns[i].string(), "--symbol", symbol, "--book", "--no-statistics"};
        const Run replayed = run(arguments);
        expect(!replayed.status.killed && replayed.status.code == 0, describe(arguments, replayed));
        if (!replayed.out.empty()) {
            sections += (sections.empty() ? "" : "\n") + replayed.out;
        }
    }
    const std::vector<std::string> fromLog{"replay", shared.logs.string(), "--book", "--no-statistics"};
    const Run logged = run(fromLog);
    expect(!logged.status.killed && logged.status.code == 0, describe(fromLog, logged));
    expect(logged.out == sections, "stand-ins: the shared engine's log replays to every stand-in's book; log:\n" + logged.out +
                                       "\ninputs:\n" + sections);

    for (const fs::path& path : named) {
        // A header-only file names nothing, and an engine needs an instrument.
        std::set<std::string> listed = symbolsIn(path);
        listed.insert("UNUSED");
        Engine own(path.stem().string(), listed, 2);
        expectSendMatchesReplay(own, path, std::nullopt);
        own.stop();
        expectLogReplaysTo(own, {"replay", path.string(), "--book"});
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
        expectLogReplaysTo(running, {"replay", file.string(), "--book"});
    }
    // On files of 1 KiB, about 20 records each, so a shard's log is many files, which replay of
    // the directory reads in order, and the clean stop leaves no unused or temporary file.
    {
        Engine running("seven instruments on 2 shards, in 1 KiB files", symbolsIn(file), 2, 1, "process", "1K");
        expectSendMatchesReplay(running, file, std::nullopt);
        running.stop();
        expectLogReplaysTo(running, {"replay", file.string(), "--book"});
        std::size_t files = 0;
        std::size_t checkpoints = 0;
        for (const fs::directory_entry& entry : fs::directory_iterator(running.logs)) {
            const std::string logName = entry.path().filename().string();
            if (logName == "engine.lock") {
                continue;  // empty, and never deleted
            }
            if (logName.starts_with("ckpt-")) {
                // What the roll-forward workers published as files closed: each whole and restoring.
                std::ifstream in(entry.path(), std::ios::binary);
                const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                std::vector<std::byte> bytes(text.size());
                std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
                const std::optional<runtime::CheckpointFault> fault = runtime::verifyCheckpoint(bytes);
                expect(logName.ends_with(".obckpt") && !fault,
                       running.name + ": " + logName + " is a checkpoint that verifies" +
                           (fault ? ": " + runtime::formatCheckpointFault(logName, *fault) : std::string()));
                ++checkpoints;
                continue;
            }
            expect(logName.ends_with(".oblog"), running.name + ": only log files and the lock are left, not " + logName);
            const std::uintmax_t size = fs::file_size(entry.path());
            expect(size <= 1024, running.name + ": " + logName + " is no larger than its file size, at " + std::to_string(size));
            ++files;
        }
        expect(files > 20, running.name + ": the log rolled across many files, " + std::to_string(files));
        expect(checkpoints >= 7, running.name + ": every book has a checkpoint, " + std::to_string(checkpoints));
    }
    // The one run at machine durability, every write through to the drive.
    Engine running("seven instruments on 3 shards and 2 gateway threads", symbolsIn(file), 3, 2, "machine");
    {
        const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(running.address.substr(running.address.rfind(':') + 1)));
        const network::TcpSocket idle = network::TcpSocket::connectLoopback(port);
        expectSendMatchesReplay(running, file, std::nullopt);
    }
    running.stop();
    expectLogReplaysTo(running, {"replay", file.string(), "--book"});
}

// A good prefix still runs, then the file's diagnostic, and exit 1.
void testAMalformedFileSendsItsPrefix() {
    const fs::path file = replaySamples / "malformed" / "symbol_lower_case.csv";
    Engine running("a malformed file", {"AAPL"}, 1);
    expectSendMatchesReplay(running, file, std::nullopt, 1);
    running.stop();
    expectLogReplaysTo(running, {"replay", file.string(), "--book"}, true);
}

// A log is sniffed as replay sniffs it, so its records go out as its CSV twin's would.
void testALogSendsItsRecords() {
    const fs::path file = replaySamples / "log" / "every_command.oblog";
    Engine running("a log", symbolsIn(file), 2);
    expectSendMatchesReplay(running, file, std::nullopt);
    running.stop();
    expectLogReplaysTo(running, {"replay", file.string(), "--book"});
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

// A file of the sample's rows again and again, each pass's order IDs past the last pass's, so
// every pass trades as the first did, and a stream long enough to kill an engine in the middle of.
// Returns how many commands it holds.
std::uint64_t writeRepeated(const fs::path& sample, int passes, const fs::path& out) {
    std::ifstream in(sample, std::ios::binary);
    std::string header;
    std::getline(in, header);
    std::vector<std::string> rows;
    for (std::string row; std::getline(in, row);) {
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (!row.empty()) {
            rows.push_back(row);
        }
    }
    if (!header.empty() && header.back() == '\r') {
        header.pop_back();
    }
    std::ofstream written(out, std::ios::binary);
    written << header << '\n';
    std::uint64_t sequence = 1;
    for (int pass = 0; pass < passes; ++pass) {
        for (const std::string& row : rows) {
            const std::size_t first = row.find(',');
            const std::size_t second = row.find(',', first + 1);
            const std::size_t third = row.find(',', second + 1);
            const std::uint64_t id = std::stoull(row.substr(second + 1, third - second - 1)) + static_cast<std::uint64_t>(pass) * 1'000'000;
            written << sequence++ << row.substr(first, second - first + 1) << id << row.substr(third) << '\n';
        }
    }
    return sequence - 1;
}

// Every log file in a directory, in shard then position order, as recovery reads them.
std::vector<fs::path> logFilesIn(const fs::path& directory) {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("log-") && name.ends_with(".oblog")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool aRecordIsLogged(const fs::path& directory) {
    for (const fs::path& file : logFilesIn(directory)) {
        if (!replay::readCommandLogFile(file).records.empty()) {
            return true;
        }
    }
    return false;
}

// replay --events or send's output: a section per symbol, each line with its line number.
struct Answer {
    std::size_t line;
    std::string text;
};
using Sections = std::map<std::string, std::vector<Answer>>;

Sections sectionsOf(const std::string& output) {
    Sections sections;
    std::vector<Answer>* current = nullptr;
    std::size_t start = 0;
    while (start < output.size()) {
        const std::size_t end = output.find('\n', start);
        const std::string line = output.substr(start, end - start);
        start = end == std::string::npos ? output.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("symbol ")) {
            current = &sections[line.substr(7)];
            continue;
        }
        const std::size_t colon = line.find(": ");
        expect(current != nullptr && line.starts_with("line ") && colon != std::string::npos, "an answer line: " + line);
        current->push_back(Answer{std::stoul(line.substr(5, colon - 5)), line.substr(colon + 2)});
    }
    return sections;
}

std::string formatSections(const Sections& sections) {
    std::string text;
    for (const auto& [symbol, answers] : sections) {
        if (answers.empty()) {
            continue;
        }
        text += (text.empty() ? "symbol " : "\nsymbol ") + symbol + '\n';
        for (const Answer& answer : answers) {
            text += "line " + std::to_string(answer.line) + ": " + answer.text + '\n';
        }
    }
    return text;
}

// A child engine killed in the middle of a stream restarts on its own log, recovers what
// survived, and goes on: its answers to a second stream are a single-threaded replay's of the
// survivors and then that stream, and its log replays to the same books. When the kill lands is
// chance, but every check holds wherever it lands. At machine durability, every answer the first
// stream's client heard is one the survivors give too.
void testAKilledEngineRestarts() {
    const fs::path sample = engineSamples / "seven_instruments.csv";
    const fs::path first = fs::temp_directory_path() / "order_book_end_to_end_killed_first.csv";
    const std::uint64_t commands = writeRepeated(sample, 300, first);
    Engine killed("killed mid-stream", symbolsIn(sample), 2, 1, "machine");
    std::string heard;
    {
        ChildProcess sender(cli, {"send", first.string(), "--connect", killed.address});
        for (int waited = 0; !aRecordIsLogged(killed.logs); ++waited) {
            expect(waited < 12000, "the engine logged a command");
            std::this_thread::sleep_for(5ms);
        }
        killed.kill();
        const ExitStatus status = sender.wait(60s);
        expect(!status.killed && (status.code == 3 || status.code == 0), "send loses its connection, unless it was all answered first");
        heard = sender.standardOutput();
    }

    // What survived, as recovery reads it: each shard's files, each up to its first error.
    std::vector<replay::ReplayRecord> survivors;
    for (const fs::path& file : logFilesIn(killed.logs)) {
        const replay::ReplayInput input = replay::readCommandLogFile(file);
        expect(!input.error || input.error->parseError->code == replay::ParseErrorCode::TORN_TAIL,
               "a killed engine's files end, if anywhere, in a torn tail: " + file.filename().string());
        survivors.insert(survivors.end(), input.records.begin(), input.records.end());
    }
    expect(!survivors.empty(), "something survived");
    // Chance, so a log can show where a failing run's kill landed.
    std::cout << "the kill left " << survivors.size() << " of " << commands << " commands in the log" << std::endl;

    Engine restarted("restarted", symbolsIn(sample), 2, 1, "machine", "1M", killed.logs);
    const std::vector<std::string> sendArguments{"send", sample.string(), "--connect", restarted.address};
    const Run sent = run(sendArguments);
    expect(!sent.status.killed && sent.status.code == 0, "the restarted engine answers a second stream: " + describe(sendArguments, sent));
    restarted.stop();
    const std::string said = plain(restarted.standardError());
    std::uint64_t replayedRecords = 0;
    for (std::uint32_t shard = 0; shard < 2; ++shard) {
        const std::string label = "order_book_engine: shard " + std::to_string(shard) + ": recovered books=";
        const std::size_t at = said.find(label);
        expect(at != std::string::npos, "each shard says what it recovered:\n" + said);
        const std::size_t count = said.find(" replayed=", at);
        const std::size_t cut = said.find(" tail-cut-bytes=", count);
        expect(count != std::string::npos && cut != std::string::npos && said.find('\n', cut) != std::string::npos,
               "naming what it replayed and the bytes it cut:\n" + said);
        replayedRecords += std::stoull(said.substr(count + 10, cut - count - 10));
    }
    expect(replayedRecords == survivors.size(), "the shards replayed every survivor, " + std::to_string(replayedRecords) + " of " +
                                             std::to_string(survivors.size()));

    // The survivors, then the second stream, as one file.
    const fs::path combined = fs::temp_directory_path() / "order_book_end_to_end_killed_combined.csv";
    {
        std::ifstream in(sample, std::ios::binary);
        std::string header;
        std::getline(in, header);
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        std::ofstream out(combined, std::ios::binary);
        out << header << '\n';
        std::uint64_t sequence = 1;
        for (replay::ReplayRecord record : survivors) {
            record.sequence = sequence++;
            out << replay::formatReplayRow(record) << '\n';
        }
        for (replay::ReplayRecord record : replay::readReplayFile(sample).records) {
            record.sequence = sequence++;
            out << replay::formatReplayRow(record) << '\n';
        }
    }
    const std::vector<std::string> replayArguments{"replay", combined.string(), "--events", "--no-statistics"};
    const Run replayed = run(replayArguments);
    expect(!replayed.status.killed && replayed.status.code == 0, describe(replayArguments, replayed));
    const std::size_t lastSurvivorLine = survivors.size() + 1;
    Sections afterRestart;
    Sections beforeKill;
    for (const auto& [symbol, answers] : sectionsOf(replayed.out)) {
        for (const Answer& answer : answers) {
            if (answer.line > lastSurvivorLine) {
                afterRestart[symbol].push_back(Answer{answer.line - survivors.size(), answer.text});
            } else {
                beforeKill[symbol].push_back(answer);
            }
        }
    }
    expect(sent.out == formatSections(afterRestart), "the restarted engine answers as a replay of its log and the second stream; send:\n" +
                                                          sent.out + "\nreplay:\n" + formatSections(afterRestart));
    for (const auto& [symbol, answers] : sectionsOf(heard)) {
        const std::vector<Answer>& survived = beforeKill[symbol];
        expect(answers.size() <= survived.size(), symbol + ": every answer the client heard is a survivor's");
        for (std::size_t i = 0; i < answers.size(); ++i) {
            expect(answers[i].text == survived[i].text, symbol + ": heard " + answers[i].text + ", where the survivors give " + survived[i].text);
        }
    }
    expectLogReplaysTo(restarted, {"replay", combined.string(), "--book"});
    fs::remove(first);
    fs::remove(combined);
}

// The offline commands (D7) on a real engine's store, one case per command and exit a store can
// give. Exit 3, a book broken or a proof failed, needs a broken book, which no file a correct
// build accepts can hold, since restore refuses one that fails checkInvariants and no command
// breaks a sound book: offline_tests stands one up through a check. Then a record damaged
// mid-log, which stops a start with exit 5: verify names it, cut-log moves it and everything
// after it to a cut- file, every byte kept, and the engine starts on what is left.
void testOfflineCommands() {
    const fs::path sample = engineSamples / "seven_instruments.csv";
    const fs::path input = fs::temp_directory_path() / "order_book_end_to_end_offline.csv";
    const std::uint64_t commands = writeRepeated(sample, 20, input);
    const std::set<std::string> symbols = symbolsIn(sample);
    Engine store("offline", symbols, 1, 1, "process", "4K");
    const std::string logs = store.logs.string();
    // The start's empty-book checkpoints at 0, before any roll supersedes them.
    std::map<std::string, std::string> empties;
    for (const fs::directory_entry& entry : fs::directory_iterator(store.logs)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("ckpt-")) {
            std::ifstream in(entry.path(), std::ios::binary);
            empties[name] = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }
    expect(empties.size() == symbols.size(), "each instrument's empty checkpoint, published at the start");
    const auto expectRun = [](const std::vector<std::string>& arguments, int code, const std::string& what) {
        const Run result = run(arguments);
        expect(!result.status.killed && result.status.code == code, what + ": " + describe(arguments, result));
        return result;
    };
    {
        const std::vector<std::string> sendArguments{"send", input.string(), "--connect", store.address};
        expectRun(sendArguments, 0, "every command answered");
        // In use, each command is refused, changing nothing.
        for (const std::vector<std::string>& arguments :
             {std::vector<std::string>{"verify", "--log-dir", logs}, std::vector<std::string>{"checkpoint", "roll", "--log-dir", logs},
              std::vector<std::string>{"cut-log", "--log-dir", logs, "--shard", "0", "--position", "0"}}) {
            const Run refused = expectRun(arguments, 1, "a store an engine is using");
            expect(plain(refused.err).find("engine.lock is locked") != std::string::npos && refused.out.empty(),
                   "naming the lock: " + refused.err);
        }
    }
    store.stop(180s);  // seven books' checkpoints on every one of the 4K files
    const std::vector<fs::path> files = logFilesIn(store.logs);
    expect(files.size() > 3, "the log spans several files: " + std::to_string(files.size()));

    // verify: a line per shard, one per book, then ok, every book at the log's end after a clean stop.
    const Run verified = expectRun({"verify", "--log-dir", logs}, 0, "a store stopped cleanly verifies");
    std::string expected = "shard 0: start=" + std::to_string(commands) + " end=" + std::to_string(commands) + " files=" +
                           std::to_string(files.size()) + " tail-cut-bytes=0 files-dropped=0\n";
    for (const std::string& symbol : symbols) {
        expected += "book " + symbol + " shard=0 checkpoint=" + std::to_string(commands) + "\n";
    }
    expect(verified.out == expected + "ok\n", "verify's lines:\n" + verified.out + "\nnot:\n" + expected + "ok\n");

    // checkpoint roll: with only the start's empty checkpoints, as a crash before any roll leaves
    // them, it rolls every closed file, each publishing every book.
    const auto keepOnlyTheEmpty = [&store, &empties] {
        std::vector<fs::path> checkpoints;
        for (const fs::directory_entry& entry : fs::directory_iterator(store.logs)) {
            if (entry.path().filename().string().starts_with("ckpt-")) {
                checkpoints.push_back(entry.path());
            }
        }
        for (const fs::path& checkpoint : checkpoints) {
            fs::remove(checkpoint);
        }
        for (const auto& [name, bytes] : empties) {
            std::ofstream(store.logs / name, std::ios::binary) << bytes;
        }
    };
    keepOnlyTheEmpty();
    const Run rolled = expectRun({"checkpoint", "roll", "--log-dir", logs}, 0, "an offline roll");
    expect(rolled.out == "rolled files=" + std::to_string(files.size() - 1) + " books=" + std::to_string((files.size() - 1) * symbols.size()) + "\n",
           "every closed file, each publishing all seven books: " + rolled.out);
    const Run again = expectRun({"checkpoint", "roll", "--log-dir", logs}, 0, "a second roll");
    expect(again.out == "rolled files=0 books=0\n", "has nothing to do: " + again.out);

    // The roll's checkpoints hold what a cut at 5 would move, so cut-log refuses.
    const Run refused = expectRun({"cut-log", "--log-dir", logs, "--shard", "0", "--position", "5"}, 1, "a checkpoint past the cut");
    expect(plain(refused.err).find("stands past position 5") != std::string::npos, "naming it: " + refused.err);

    // A record damaged in the first file, which its writer flushed whole, with only the empty
    // checkpoints, as after a crash before any roll: a start reads it, and stops, exit 5.
    keepOnlyTheEmpty();
    const std::uint64_t damagedAt = 5;
    {
        std::ifstream in(files[0], std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::istringstream header(bytes, std::ios::binary);
        replay::CommandLogReader reader(header);
        static_cast<void>(reader.next());
        const replay::LogHeader& read = *reader.header();
        const std::size_t at = read.headerBytes + read.symbols.size() * replay::Symbol::size + sizeof(std::uint64_t) + damagedAt * replay::logRecordBytes + 12;
        bytes[at] = static_cast<char>(bytes[at] ^ 0x10);
        std::ofstream(files[0], std::ios::binary | std::ios::trunc) << bytes;
    }
    const fs::path list = fs::temp_directory_path() / "order_book_end_to_end_offline_list.csv";
    {
        std::ofstream out(list, std::ios::binary);
        out << "symbol,shard,orders,stops,levels\n";
        for (const std::string& symbol : symbols) {
            out << symbol << ",,,,\n";
        }
    }
    {
        ChildProcess start(engine, {"--instruments", list.string(), "--shards", "1", "--listen", "127.0.0.1:0",
                                                                       "--log-dir", logs, "--durability", "process", "--log-file-size", "4K",
                                                                       "--keep-log"});
        const ExitStatus status = start.wait(60s);
        expect(!status.killed && status.code == 5 && plain(start.standardError()).find("recovery stopped: shard 0 position 5 file " +
                                                                                       files[0].filename().string() + ": CORRUPT_RECORD") != std::string::npos,
               "a start stops on it, exit 5, not " + std::to_string(status.code) + (status.killed ? " (killed)" : "") + ", with stderr:\n" +
                   start.standardError());
    }
    const Run found = expectRun({"verify", "--log-dir", logs, "--validate", "per-command"}, 1, "verify stops where a start would");
    expect(plain(found.err).starts_with("order_book_cli: verify: shard 0 position 5 file " + files[0].filename().string() + ": CORRUPT_RECORD"),
           "naming the record: " + found.err);

    const Run cut =expectRun({"cut-log", "--log-dir", logs, "--shard", "0", "--position", "5"}, 0, "cut-log");
    const std::string cutName = "cut-0000-00000000000000000005.oblog";
    expect(cut.out == "moved records=" + std::to_string(commands - damagedAt) + " shard=0 to " + cutName + "\n", "every record from 5 on: " + cut.out);
    expect(logFilesIn(store.logs).size() == 1, "the log is its first file, cut at the record");
    expect(fs::file_size(store.logs / cutName) > (commands - damagedAt) * replay::logRecordBytes, "the cut file holds every moved record");

    // The engine starts on what is left, the log ending at 5.
    Engine restarted("offline restarted", symbols, 1, 1, "process", "4K", store.logs);
    restarted.stop(180s);
    expect(plain(restarted.standardError()).find("shard 0: recovered books=7 replayed=5 ") != std::string::npos,
           "the start passes the cut, its log ending at 5:\n" + restarted.standardError());
    expectRun({"verify", "--log-dir", logs}, 0, "and verifies");

    const Run missing = expectRun({"verify", "--log-dir", logs + "-missing"}, 1, "no such directory");
    expect(plain(missing.err) == "order_book_cli: cannot use log directory " + logs + "-missing: no such directory\n", missing.err);
    fs::remove(input);
    fs::remove(list);
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
    testAKilledEngineRestarts();
    testOfflineCommands();
    testNoEngineExits1();
    std::cout << "engine end-to-end tests passed" << std::endl;
    return EXIT_SUCCESS;
}
