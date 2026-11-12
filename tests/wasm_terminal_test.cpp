#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
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
#include "session_transcript.hpp"
#include "wasm_page.hpp"

// The browser terminal behind run(line), natively and under Emscripten: each committed session
// in tests/wasm/sessions played through a Terminal on a BrowserEngine seeded from the shipped
// seed, in a copy of the samples' root, held to its expected transcript and its collected CSV;
// then the session's sends and that CSV sent through send's own code to a fresh seeded engine,
// whose answers must be the transcript's. See docs/internals/wasm.md.
namespace {

using namespace order_book;
using namespace order_book::wasm;
using namespace order_book::wasm_testing;

void expect(bool condition, const std::string& message) { check(condition, message); }

// A committed file's bytes: tests/wasm/sessions is byte-exact (-text), as the samples are.
std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "can read " + path.string());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
}

struct Paths {
    std::filesystem::path instruments;  // src/wasm/instruments.csv
    std::filesystem::path seed;         // src/wasm/seed.csv
    std::filesystem::path sessions;     // tests/wasm/sessions
    std::filesystem::path scratch;
};

// `order_book_cli send ARGS` over the run's connections, to its end, connecting to the browser
// engine's address unless the arguments name one.
cli::CommandResult send(BrowserEngine& engine, Page& page, std::vector<std::string> args) {
    if (std::find(args.begin(), args.end(), "--connect") == args.end()) {
        args.insert(args.end(), {"--connect", "127.0.0.1:9000"});
    }
    const std::vector<std::string_view> views(args.begin(), args.end());
    std::variant<PageSend, cli::CommandResult> started = startSend(engine.sockets(), views);
    if (auto* now = std::get_if<cli::CommandResult>(&started)) {
        return *now;
    }
    PageSend& sending = std::get<PageSend>(started);
    return pumpUntilDone(page, [&] { return sending.pump(); });
}

// A started engine, seeded on its empty store as the engine worker seeds it before ready.
struct Seeded {
    explicit Seeded(const Paths& paths) : engine(paths.instruments.string(), {}, page.callbacks()), running(engine) {
        page.waitUntilReady();
        expect(engine.startedEmpty(), "a first start is on an empty store, so it seeds");
        seeded = send(engine, page, {paths.seed.string()});
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
        Terminal terminal(run.engine.shadows(), run.engine.store());
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

    // After the same seed, the session's sends and then its collected CSV through send print
    // what the terminal printed for them.
    Seeded native(paths);
    expect(native.seeded.out == seededAnswers, name + ": the seed answers the same each time");
    const Sends sends = sendsOf(transcript);
    expect(sends.allBeforeClientCommands, name + ": every send comes before the first client command, so sending them first gives the same books");
    for (const Typed& sent : sends.sends) {
        const cli::CommandResult again = send(native.engine, native.page, std::vector<std::string>(sent.words.begin() + 1, sent.words.end()));
        expect(again.out == sent.out && again.err == sent.err && again.exit == sent.exit,
               name + ": send " + sent.words.at(1) + " prints what the terminal printed:\n" + again.out + again.err);
    }
    const cli::CommandResult sent = send(native.engine, native.page, {(paths.sessions / (name + ".csv")).string()});
    const ClientAnswers expected = clientAnswersOf(transcript);
    expect(sent.exit == (expected.refused ? cli::someRefused : 0), name + ": send exits as the terminal's refusals say:\n" + sent.err);
    expect(sent.err.empty() || sent.exit == cli::someRefused, name + ": send answered every command:\n" + sent.err);
    expect(answersByLine(sent.out) == expected.byLine, name + ": send prints each line's answers as the terminal did:\n" + sent.out);
    expect(native.running.cleanStop() == network::EngineExit::CLEAN_STOP, name + ": the native run stops cleanly");
}

// A start on a store already holding a log doesn't seed, and the terminal goes on counting its
// lines across starts. With no engine running, send can't connect, as native's can't, and verify
// reads the page store, which the engine no longer holds.
void testARestart(const Paths& paths) {
    Page page;
    BrowserEngine engine(paths.instruments.string(), {}, page.callbacks());
    Terminal terminal(engine.shadows(), engine.store());
    const std::optional<cli::CommandResult> early = terminal.run("send valid/v3_two_instruments.csv");
    expect(early && early->exit == cli::inputError && early->out.empty() &&
               early->err == "order_book_cli: cannot connect to 127.0.0.1:9000: Connection refused\n",
           "before the first start, send can't connect");
    {
        Running running(engine);
        page.waitUntilReady();
        expect(engine.startedEmpty(), "the first start is on an empty store, left unseeded here");
        terminal.connect(engine.sockets());
        const std::string first = play(terminal, page, {"buy AAPL 1 90"});
        expect(first == "> buy AAPL 1 90\nassigned id=1\nline 2: OrderAccepted id=1 side=BUY price=90 qty=1\n",
               "an unseeded book's first ID is 1:\n" + first);
        const std::optional<cli::CommandResult> elsewhere = terminal.run("send valid/v3_one_instrument.csv --connect 127.0.0.1:9001");
        expect(elsewhere && elsewhere->exit == cli::inputError &&
                   elsewhere->err == "order_book_cli: cannot connect to 127.0.0.1:9001: Connection refused\n",
               "nothing else listens in the tab, so send to another address can't connect");
        expect(running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
        terminal.disconnect();
    }
    const std::optional<cli::CommandResult> down = terminal.run("send valid/v3_two_instruments.csv");
    expect(down && down->exit == cli::inputError && down->err == early->err, "with the engine down, send can't connect");
    const std::optional<cli::CommandResult> verified = terminal.run("verify");
    expect(verified && verified->exit == 0 && verified->err.empty() && verified->out.ends_with("\nok\n"),
           "with the engine down, verify reads the store:\n" + (verified ? verified->out + verified->err : std::string()));

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
    expect(argc == 4, "usage: wasm_terminal_tests <source directory> <scratch directory> <session>");
    // Both absolute, as CTest gives them: under Emscripten, std::filesystem::absolute takes a
    // Windows path for a relative one.
    const std::filesystem::path source = argv[1];
    const std::string_view which = argv[3];
    Paths paths{source / "src/wasm/instruments.csv", source / "src/wasm/seed.csv", source / "tests/wasm/sessions",
                argv[2]};
    std::filesystem::create_directories(paths.scratch);

    // The samples' root, as the module embeds it: valid/ and malformed/ and nothing else, the
    // working directory, so paths print as typed.
    const std::filesystem::path samples = paths.scratch / ("samples-" + std::string(which));
    std::filesystem::remove_all(samples);
    for (const char* directory : {"valid", "malformed"}) {
        std::filesystem::create_directories(samples / directory);
        std::filesystem::copy(source / "tests/replay" / directory, samples / directory, std::filesystem::copy_options::recursive);
    }
    std::filesystem::current_path(samples);

    if (which == "restart") {
        testARestart(paths);
    } else {
        testASession(paths, std::string(which));
    }
    std::cout << "wasm terminal test " << which << " passed" << std::endl;
    return EXIT_SUCCESS;
}
