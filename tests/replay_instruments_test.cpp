#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_inputs.hpp"
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

// A directory of its own for one test, gone when it ends.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("order-book-" + name)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string fileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(file.is_open(), "cannot open " + path.string());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void testConvertRoundTrips(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-round-trip");
    const std::filesystem::path log = directory.path() / "one.oblog";
    const std::filesystem::path csv = directory.path() / "one.csv";
    const std::filesystem::path logAgain = directory.path() / "two.oblog";

    ConvertOutcome outcome = convertReplayFiles({root / "log" / "every_command.csv"}, log, std::nullopt);
    expect(!outcome.usage && !outcome.refusal && !outcome.error && outcome.records == 10, "a CSV converts to a log");
    expect(isCommandLogFile(log), "the output is a log");
    outcome = convertReplayFiles({log}, csv, std::nullopt);
    expect(!outcome.error && outcome.records == 10, "a log converts to a CSV");
    expect(fileText(csv) == fileText(root / "log" / "every_command.csv"), "canonical CSV to log to CSV is byte-identical");
    outcome = convertReplayFiles({csv}, logAgain, std::nullopt);
    expect(!outcome.error, "and back");
    expect(fileText(logAgain) == fileText(log), "log to CSV to log is byte-identical");
}

void testConvertStandsInASymbol(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-symbol");
    const std::filesystem::path log = directory.path() / "basic.oblog";
    const ConvertOutcome outcome = convertReplayFiles({root / "valid" / "basic.csv"}, log, symbolOf("AAPL"));
    expect(!outcome.usage && !outcome.error, "--symbol names a v1 file's instrument");
    const ReplayInput read = readCommandLogFile(log);
    expect(!read.error && read.records.size() == 4 && read.records[0].symbol == symbolOf("AAPL"), "every record takes it");
}

void testConvertRefuses(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-refuses");
    const std::filesystem::path out = directory.path() / "out.oblog";
    ConvertOutcome outcome = convertReplayFiles({root / "valid" / "basic.csv"}, out, std::nullopt);
    expect(outcome.usage.has_value() && !std::filesystem::exists(out), "a v1 file needs --symbol");
    outcome = convertReplayFiles({root / "valid" / "v3_one_instrument.csv"}, out, symbolOf("AAPL"));
    expect(outcome.usage.has_value() && !std::filesystem::exists(out), "a file that names symbols refuses --symbol");

    outcome = convertReplayFiles({root / "valid" / "v3_one_instrument.csv"}, out, std::nullopt);
    expect(!outcome.usage && !outcome.refusal && std::filesystem::exists(out), "a good conversion writes");
    const std::string first = fileText(out);
    outcome = convertReplayFiles({root / "valid" / "v3_two_instruments.csv"}, out, std::nullopt);
    expect(outcome.refusal.has_value() && fileText(out) == first, "an existing output is refused, never overwritten");
}

// A damaged input's good prefix is still written, and the conversion reports what stopped it.
void testConvertKeepsTheGoodPrefix(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-prefix");
    const std::filesystem::path out = directory.path() / "prefix.csv";
    const ConvertOutcome outcome = convertReplayFiles({root / "log" / "torn_tail.oblog"}, out, std::nullopt);
    expect(outcome.error.has_value() && outcome.records == 2, "the good records are counted, and the error reported");
    expect(fileText(out) == std::string(expectedHeaderV3) + "\n1,ADD,1,SELL,101,5,LIMIT,GTC,,,,,AAPL\n2,CANCEL,1,,,,,,,,,,AAPL\n",
           "the good prefix is written");
}

void testConvertSeveralMakesADirectory(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-several");
    const std::filesystem::path out = directory.path() / "commands";
    const ConvertOutcome outcome = convertReplayFiles({root / "log" / "shards"}, out, std::nullopt);
    expect(!outcome.refusal && !outcome.error && outcome.outputs.size() == 2, "one file per instrument");
    const std::string header = std::string(expectedHeaderV3) + "\n";
    expect(fileText(out / "commands-AAPL.csv") == header + "1,ADD,1,SELL,101,5,LIMIT,GTC,,,,,AAPL\n"
                                                           "2,ADD,2,SELL,102,5,LIMIT,GTC,,,,,AAPL\n"
                                                           "3,CANCEL,1,,,,,,,,,,AAPL\n",
           "an instrument's commands, in order across files, numbered from 1");
    expect(fileText(out / "commands-MSFT.csv") == header + "1,ADD,1,BUY,99,1,LIMIT,GTC,,,,,MSFT\n", "each instrument alone");
}

// A log with no placement at position 0 is a converted file, so what convert writes is the
// golden file itself, and the round trip that starts from binary holds too.
void testBinaryRoundTripIsByteIdentical(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-from-binary");
    const std::filesystem::path golden = root / "log" / "every_command.oblog";
    const std::filesystem::path converted = directory.path() / "converted.oblog";
    const std::filesystem::path csv = directory.path() / "one.csv";
    const std::filesystem::path logAgain = directory.path() / "two.oblog";

    expect(!convertReplayFiles({root / "log" / "every_command.csv"}, converted, std::nullopt).error, "the twin converts");
    expect(fileText(converted) == fileText(golden), "convert writes the golden log exactly");
    expect(!convertReplayFiles({golden}, csv, std::nullopt).error, "the log converts to a CSV");
    expect(!convertReplayFiles({csv}, logAgain, std::nullopt).error, "and back");
    expect(fileText(logAgain) == fileText(golden), "binary to CSV to binary is byte-identical");
}

// replay refuses a log that doesn't start at 0, since its books would start wrong. Conversion
// runs no book, so any one log converts, keeping its commands.
void testOneLogConvertsFromAnywhere(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-placed");
    ConvertOutcome outcome = convertReplayFiles({root / "log" / "placement.oblog"}, directory.path() / "placed.csv", std::nullopt);
    expect(!outcome.refusal && !outcome.error && outcome.records == 2, "a log with a placement and a first position converts");
    outcome = convertReplayFiles({root / "log" / "shards" / "shard0-part2.oblog"}, directory.path() / "tail.csv", std::nullopt);
    expect(!outcome.refusal && !outcome.error && outcome.records == 1, "so does a shard's later file");
    outcome = convertReplayFiles({root / "log" / "shards" / "shard0-part1.oblog", root / "log" / "gap" / "shard0-part3.oblog"},
                                 directory.path() / "several", std::nullopt);
    expect(outcome.refusal.has_value() && outcome.outputs.empty(), "several logs with a gap would lose commands");
}

// With nothing good to keep, a file would only block the retry.
void testNoGoodPrefixWritesNothing(const std::filesystem::path& root) {
    const TemporaryDirectory directory("convert-nothing");
    const std::filesystem::path out = directory.path() / "out.oblog";
    const ConvertOutcome outcome = convertReplayFiles({root / "no_such_file.csv"}, out, std::nullopt);
    expect(outcome.error.has_value() && outcome.outputs.empty(), "a missing input is an error with no output");
    expect(!std::filesystem::exists(out), "and writes no file");
}

// Cut records stay valid logs: left out of a directory, and readable when named.
void testCutRecordsAndFilesBeingMade(const std::filesystem::path& root) {
    const std::filesystem::path cut = root / "log" / "cut";
    const std::vector<std::filesystem::path> expanded = expandInputs({cut});
    expect(expanded.size() == 1 && expanded[0].filename() == "log-0000-00000000000000000000.oblog",
           "a directory leaves out cut records and files being made");
    const ReplayInputs named = readReplayInputs({cut / "cut-0000-00000000000000000002.oblog"});
    expect(!named.refusal && named.files.size() == 1 && named.files[0].input.records.size() == 1,
           "cut records read directly, though they don't start at position 0");
}

void testMergedErrorsNameTheirOwnFile(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "log" / "every_command.csv";
    const LoadedInput first{path, readReplayFile(path)};
    const LoadedInput second{path, readReplayFile(path)};
    const MergedRecords merged = mergeRecords({&first, &second});
    expect(merged.origins.size() == 20, "every record keeps its origin");

    // Index 11 is the second file's second record, which is on line 3.
    const ReplayError atARecord{ReplayErrorCode::INVARIANT_VIOLATION, 11, "broken", std::nullopt};
    expect(formatMergedError(atARecord, merged) == path.generic_string() + ":3: engine failure: broken",
           "a diagnostic names the record's file and line, not its index");
    const ReplayError atNoRecord{ReplayErrorCode::INVARIANT_VIOLATION, std::nullopt, "broken", std::nullopt};
    expect(formatMergedError(atNoRecord, merged) == "several inputs: engine failure: broken",
           "an error with no line names no file");
}

// Whatever order they are named in, an engine's logs run by shard then position.
void testEngineLogsRunInOrder(const std::filesystem::path& root) {
    const std::filesystem::path shards = root / "log" / "shards";
    const ReplayInputs inputs =
        readReplayInputs({shards / "shard0-part2.oblog", shards / "a-shard1.oblog", shards / "shard0-part1.oblog"});
    expect(!inputs.refusal && inputs.files.size() == 3, "all three read");
    expect(inputs.files[0].path.filename() == "shard0-part1.oblog", "shard 0 first, from position 0");
    expect(inputs.files[1].path.filename() == "shard0-part2.oblog", "then shard 0's next file");
    expect(inputs.files[2].path.filename() == "a-shard1.oblog", "then shard 1");
    expect(expandInputs({shards}).size() == 3, "a directory means every log in it");
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
    testConvertRoundTrips(argv[1]);
    testConvertStandsInASymbol(argv[1]);
    testConvertRefuses(argv[1]);
    testConvertKeepsTheGoodPrefix(argv[1]);
    testConvertSeveralMakesADirectory(argv[1]);
    testEngineLogsRunInOrder(argv[1]);
    testBinaryRoundTripIsByteIdentical(argv[1]);
    testOneLogConvertsFromAnywhere(argv[1]);
    testNoGoodPrefixWritesNothing(argv[1]);
    testCutRecordsAndFilesBeingMade(argv[1]);
    testMergedErrorsNameTheirOwnFile(argv[1]);
    std::cout << "replay instruments tests passed\n";
    return EXIT_SUCCESS;
}
