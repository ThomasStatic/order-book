#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/cli/commands.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/wasm/browser_engine.hpp"
#include "order_book/wasm/page_connection.hpp"
#include "order_book/wasm/terminal.hpp"
#include "wasm_page.hpp"

// The browser terminal behind run(line), natively and under Emscripten: each committed session
// in tests/wasm/sessions played through a Terminal on a BrowserEngine seeded from the shipped
// seed, held to its expected transcript and its collected CSV; then that CSV sent through send's
// own code to a fresh seeded engine, whose answers must be the transcript's, line for line. See
// docs/internals/wasm.md.
namespace {

using namespace order_book;
using namespace order_book::wasm;
using namespace order_book::wasm_testing;

void expect(bool condition, const std::string& message) { check(condition, message); }

// A committed file's text, with any CR a Windows checkout added dropped.
std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "can read " + path.string());
    std::string text(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
    std::erase(text, '\r');
    return text;
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        lines.push_back(line);
    }
    return lines;
}

struct Paths {
    std::filesystem::path instruments;  // src/wasm/instruments.csv
    std::filesystem::path seed;         // src/wasm/seed.csv
    std::filesystem::path sessions;     // tests/wasm/sessions
    std::filesystem::path scratch;
};

// `order_book_cli send FILE` over the run's connections, to its end.
cli::CommandResult sendFile(BrowserEngine& engine, Page& page, const std::filesystem::path& file) {
    const std::string path = file.string();
    const std::vector<std::string_view> args{path, "--connect", "127.0.0.1:9000"};
    std::variant<PageSend, cli::CommandResult> started = startSend(engine.sockets(), args);
    if (auto* refused = std::get_if<cli::CommandResult>(&started)) {
        expect(false, "the file is sent: " + refused->err);
    }
    PageSend& sending = std::get<PageSend>(started);
    return pumpUntilDone(page, [&] { return sending.pump(); });
}

// A started engine, seeded on its empty store as the engine worker seeds it before ready.
struct Seeded {
    explicit Seeded(const Paths& paths) : engine(paths.instruments.string(), {}, page.callbacks()), running(engine) {
        page.waitUntilReady();
        expect(engine.startedEmpty(), "a first start is on an empty store, so it seeds");
        seeded = sendFile(engine, page, paths.seed);
        expect(seeded.exit == 0 && seeded.err.empty(), "the seed is answered whole:\n" + seeded.err);
    }

    Page page;
    BrowserEngine engine;
    Running running;
    cli::CommandResult seeded;
};

// The session's transcript: each line typed, after "> ", then what it printed, its stderr lines
// after "! ", and "[exit N]" when not 0.
std::string play(Terminal& terminal, Page& page, const std::vector<std::string>& typed) {
    std::string transcript;
    for (const std::string& line : typed) {
        transcript += "> " + line + "\n";
        std::optional<cli::CommandResult> now = terminal.run(line);
        const cli::CommandResult result = now ? *now : pumpUntilDone(page, [&] { return terminal.pump(); });
        transcript += result.out;
        for (const std::string& error : linesOf(result.err)) {
            transcript += "! " + error + "\n";
        }
        if (result.exit != 0) {
            transcript += "[exit " + std::to_string(result.exit) + "]\n";
        }
    }
    return transcript;
}

// Each `line N:` line, by N, in the order printed.
std::map<std::string, std::vector<std::string>> answersByLine(const std::string& text) {
    std::map<std::string, std::vector<std::string>> byLine;
    for (const std::string& line : linesOf(text)) {
        if (line.starts_with("line ")) {
            byLine[line.substr(0, line.find(':'))].push_back(line);
        }
    }
    return byLine;
}

// Writes what came beside the scratch directory, so a changed session can be looked at.
void expectFile(const std::string& actual, const std::filesystem::path& expected, const Paths& paths) {
    if (actual != readFile(expected)) {
        const std::filesystem::path written = paths.scratch / expected.filename();
        std::ofstream(written, std::ios::binary) << actual;
        expect(false, expected.string() + " differs from what the session printed, written to " + written.string());
    }
}

void testASession(const Paths& paths, const std::string& name) {
    const std::vector<std::string> typed = linesOf(readFile(paths.sessions / (name + ".txt")));
    std::string transcript;
    std::string collected;
    std::string seededAnswers;
    {
        Seeded run(paths);
        seededAnswers = run.seeded.out;
        Terminal terminal(run.engine.shadows());
        terminal.connect(run.engine.sockets());
        transcript = play(terminal, run.page, typed);
        collected = terminal.collected();
        expect(run.running.cleanStop() == network::EngineExit::CLEAN_STOP, name + ": a clean stop:\n" + run.running.diagnostics.str());
        terminal.disconnect();
        const std::optional<cli::CommandResult> after = terminal.run("buy AAPL 1 100");
        expect(after && after->exit == cli::inputError && after->err == "buy: the engine isn't running\n",
               name + ": once the engine stops, nothing answers");
    }
    expectFile(transcript, paths.sessions / (name + ".expected"), paths);
    expectFile(collected, paths.sessions / (name + ".csv"), paths);

    // The collected CSV through send, after the same seed, answers each line as the terminal did.
    Seeded native(paths);
    expect(native.seeded.out == seededAnswers, name + ": the seed answers the same each time");
    const cli::CommandResult sent = sendFile(native.engine, native.page, paths.sessions / (name + ".csv"));
    expect(sent.err.empty() || sent.exit == cli::someRefused, name + ": send answered every command:\n" + sent.err);
    expect(answersByLine(sent.out) == answersByLine(transcript),
           name + ": send prints each line's answers as the terminal did:\n" + sent.out);
    expect(native.running.cleanStop() == network::EngineExit::CLEAN_STOP, name + ": the native run stops cleanly");
}

// A start on a store already holding a log doesn't seed, and the terminal goes on counting its
// lines across starts.
void testARestart(const Paths& paths) {
    Page page;
    BrowserEngine engine(paths.instruments.string(), {}, page.callbacks());
    Terminal terminal(engine.shadows());
    {
        Running running(engine);
        page.waitUntilReady();
        expect(engine.startedEmpty(), "the first start is on an empty store, left unseeded here");
        terminal.connect(engine.sockets());
        const std::string first = play(terminal, page, {"buy AAPL 1 90"});
        expect(first == "> buy AAPL 1 90\nassigned id=1\nline 2: OrderAccepted id=1 side=BUY price=90 qty=1\n",
               "an unseeded book's first ID is 1:\n" + first);
        expect(running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
        terminal.disconnect();
    }
    Running again(engine);
    page.waitUntilReady();
    expect(!engine.startedEmpty(), "a start on a store with a log doesn't seed");
    terminal.connect(engine.sockets());
    const std::string second = play(terminal, page, {"buy AAPL 1 90"});
    expect(second == "> buy AAPL 1 90\nassigned id=2\nline 3: OrderAccepted id=2 side=BUY price=90 qty=1\n",
           "the restarted terminal counts on, and its ID is past what the store holds:\n" + second);
    expect(again.cleanStop() == network::EngineExit::CLEAN_STOP, "the restart stops cleanly");
}

}  // namespace

int main(int argc, char** argv) {
    expect(argc == 4, "usage: wasm_terminal_tests <source directory> <scratch directory> <session>...");
    const std::filesystem::path source = argv[1];
    Paths paths{source / "src/wasm/instruments.csv", source / "src/wasm/seed.csv", source / "tests/wasm/sessions", argv[2]};
    std::filesystem::create_directories(paths.scratch);
    const std::string_view which = argv[3];
    if (which == "restart") {
        testARestart(paths);
    } else {
        testASession(paths, std::string(which));
    }
    std::cout << "wasm terminal test " << which << " passed" << std::endl;
    return EXIT_SUCCESS;
}
