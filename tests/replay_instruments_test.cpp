#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_instruments.hpp"
#include "order_book/replay_summary.hpp"

namespace {

using namespace order_book;
using namespace order_book::replay;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(const char* text) {
    return *Symbol::parse(text);
}

void testGroupingKeepsFileOrderPerSymbol() {
    const std::vector<ReplayRecord> records{
        ReplayRecord{2, 1, CancelCommand{1}, symbolOf("MSFT")},
        ReplayRecord{3, 2, CancelCommand{2}, symbolOf("AAPL")},
        ReplayRecord{4, 3, CancelCommand{3}, symbolOf("MSFT")},
    };
    const std::vector<InstrumentRecords> groups = groupBySymbol(records);
    expect(groups.size() == 2, "one group per symbol");
    expect(groups[0].symbol == symbolOf("AAPL") && groups[1].symbol == symbolOf("MSFT"), "groups sort by symbol");
    expect(groups[1].records.size() == 2 && groups[1].records[0].lineNumber == 2 && groups[1].records[1].lineNumber == 4,
           "each group keeps its file order");
}

void testAStandInNamesRecordsWithNoSymbol() {
    const std::vector<ReplayRecord> records{ReplayRecord{2, 1, CancelCommand{1}}, ReplayRecord{3, 2, CancelCommand{2}}};
    const std::vector<InstrumentRecords> groups = groupBySymbol(records, symbolOf("AAPL"));
    expect(groups.size() == 1 && groups[0].symbol == symbolOf("AAPL") && groups[0].records.size() == 2,
           "a stand-in gathers every record");
}

void testNoRecordsNoSections() {
    expect(groupBySymbol({}).empty(), "no records, no groups");
    const InstrumentsOutcome outcome = replayInstruments({}, ReplayOptions{});
    const auto& replays = std::get<std::vector<InstrumentReplay>>(outcome);
    expect(replays.empty() && formatInstrumentSections(replays, ReplayOutput{}).empty(), "no groups, no sections");
}

// One book per symbol: the same ID on two instruments is two orders, and neither book sees the
// other's commands, however they interleaved in the file.
void testBooksAreSeparate(const std::filesystem::path& root) {
    const ReplayInput input = readReplayFile(root / "valid" / "v3_two_instruments.csv");
    expect(!input.error && input.format == ReplayFormat::V3, "the sample reads as v3");
    const InstrumentsOutcome outcome = replayInstruments(groupBySymbol(input.records), ReplayOptions{});
    const auto& replays = std::get<std::vector<InstrumentReplay>>(outcome);
    expect(replays.size() == 4, "a book for each symbol in a record");
    expect(replays[0].symbol == symbolOf("AAPL") && replays[0].result.statistics.cancelsAccepted == 1,
           "AAPL cancels its own order 1, which MSFT's has the ID of");
    expect(replays[2].symbol == symbolOf("MSFT") && replays[2].result.statistics.tradesExecuted == 1,
           "MSFT trades its own order 1");
    expect(replays[3].symbol == symbolOf("ZZZ") && replays[3].result.statistics.cancelsRejected == 1,
           "ZZZ's book holds nothing for its cancel to find");
}

// A section holds exactly one book's output, so a v3 file of one instrument prints what the same
// commands print as v2 under a heading.
void testASectionIsOneBooksOutput() {
    const std::vector<ReplayRecord> records{ReplayRecord{2, 1, AddCommand{1, Side::BUY, 100, 5}, symbolOf("AAPL")}};
    OrderBook book;
    const ReplayOutcome single = ReplayEngine::run(records, book);
    const ReplayOutput everything{true, true, true, true};
    const std::string body = formatReplayOutput(std::get<ReplayResult>(single), everything);

    const InstrumentsOutcome outcome = replayInstruments(groupBySymbol(records), ReplayOptions{});
    expect(formatInstrumentSections(std::get<std::vector<InstrumentReplay>>(outcome), everything) == "symbol AAPL\n" + body,
           "a section is a heading and one book's output");
}

void testExecuteCommandAgreesWithTheBook() {
    OrderBook direct;
    OrderBook viaCommand;
    OrderBook viaResult;
    CommandResult result;
    result.reserve(16, 32);
    const std::vector<Command> commands{
        AddCommand{1, Side::SELL, 101, 5},
        AddCommand{2, Side::BUY, 101, 2},
        ReplaceCommand{1, 102, 6},
        CancelCommand{1},
        CancelCommand{1},
    };

    const std::vector<std::size_t> eventCounts{1, 2, 1, 1, 1};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const CommandOutcome outcome = executeCommand(viaCommand, commands[i]);
        executeCommand(viaResult, commands[i], result);
        expect(result.events().size() == eventCounts[i], "the out overload reports the command's own events");
        std::visit(
            [&](const auto& held) {
                const auto& events = held.getEvents();
                expect(events.size() == result.events().size(), "both overloads make the same events");
                for (std::size_t e = 0; e < events.size(); ++e) {
                    expect(events[e] == result.events()[e], "both overloads make the same events");
                }
            },
            outcome);
        // What a direct call would have done.
        std::visit(
            [&](const auto& command) {
                using Kind = std::decay_t<decltype(command)>;
                if constexpr (std::is_same_v<Kind, AddCommand>) {
                    direct.submitOrder(toRequest(command));
                } else if constexpr (std::is_same_v<Kind, ReplaceCommand>) {
                    direct.replaceOrder(toRequest(command));
                } else {
                    direct.cancelOrder(command.orderId);
                }
            },
            commands[i]);
    }
    expect(direct.activeOrderCount() == viaCommand.activeOrderCount() && direct.activeOrderCount() == viaResult.activeOrderCount(),
           "the books end alike");
}

void testResultKeepsEachKind() {
    OrderBook book;
    CommandResult result;
    executeCommand(book, Command{AddCommand{1, Side::SELL, 101, 5}}, result);
    expect(result.submission().getStatus() == SubmissionStatus::ACCEPTED, "an add fills the submission");
    executeCommand(book, Command{CancelCommand{7}}, result);
    expect(!result.cancellation().isSuccess(), "a cancel fills the cancellation");
    executeCommand(book, Command{ReplaceCommand{7, 101, 5}}, result);
    expect(!result.replacement().isSuccess(), "a replace fills the replacement");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: replay_instruments_tests <tests/replay directory>\n";
        return EXIT_FAILURE;
    }
    testGroupingKeepsFileOrderPerSymbol();
    testAStandInNamesRecordsWithNoSymbol();
    testNoRecordsNoSections();
    testBooksAreSeparate(argv[1]);
    testASectionIsOneBooksOutput();
    testExecuteCommandAgreesWithTheBook();
    testResultKeepsEachKind();
    std::cout << "replay instruments tests passed\n";
    return EXIT_SUCCESS;
}
