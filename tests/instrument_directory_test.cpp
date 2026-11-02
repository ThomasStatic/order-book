#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/runtime/instruments.hpp"

namespace {

using namespace order_book::runtime;
using Code = InstrumentListErrorCode;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expectThrows(const std::function<void()>& call, const std::string& message) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return;
    }
    expect(false, message);
}

Symbol symbol(std::string_view text) {
    const std::optional<Symbol> parsed = Symbol::parse(text);
    expect(parsed.has_value(), "test symbol should parse: " + std::string(text));
    return *parsed;
}

Instrument unpinned(std::string_view text) {
    return Instrument{symbol(text), std::nullopt, std::nullopt, std::nullopt, std::nullopt};
}

Instrument pinned(std::string_view text, std::uint32_t shard) {
    Instrument instrument = unpinned(text);
    instrument.shard = shard;
    return instrument;
}

std::vector<Instrument> parsed(std::string_view text, std::uint32_t shards = 4) {
    InstrumentListResult result = parseInstrumentList(text, shards);
    if (const InstrumentListError* error = std::get_if<InstrumentListError>(&result)) {
        expect(false, "should parse, but: " + formatInstrumentListError("list", *error));
    }
    return std::get<std::vector<Instrument>>(std::move(result));
}

void testTheDocumentedExample() {
    const std::vector<Instrument> list = parsed("symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,1,100000,10000,2000\n");
    expect(list.size() == 2, "two rows are two instruments");
    expect(list[0] == unpinned("AAPL"), "an empty field means the default");
    expect(list[1].symbol == symbol("MSFT") && list[1].shard == 1u, "shard pins the instrument");
    expect(list[1].orders == 100000u && list[1].stops == 10000u && list[1].levels == 2000u, "capacity columns are read");
}

void testAcceptedForms() {
    const std::string header = "symbol,shard,orders,stops,levels";
    expect(parsed(header + "\nAAPL,0,,,\n")[0].shard == 0u, "shard may be 0, unlike any replay column");
    expect(parsed(header + "\nAAPL,003,0042,,\n")[0].orders == 42u, "leading zeros are ignored");
    expect(parsed(header + "\nAAPL,,,,")[0] == unpinned("AAPL"), "the last line may leave out its ending");
    expect(parsed(header + "\r\nAAPL,,,,\r\nMSFT,,,,\n").size() == 2, "a list may mix CRLF and LF");
    expect(parsed(header + "\nAAPL,,4294967295,,\n")[0].orders == 4294967295u, "a column holds up to its maximum");
    expect(parsed(header + "\nBRK.B,,,,\nES-Z26,,,,\n").size() == 2, "every symbol character is accepted");
}

struct Refusal {
    std::string_view text;
    Code code;
    std::size_t line;
    std::string_view column;
};

void testRefusals() {
    const std::vector<Refusal> refusals = {
        {"", Code::MISSING_HEADER, 1, ""},
        {"AAPL,,,,\n", Code::INVALID_HEADER, 1, ""},
        {"symbol,shard,orders,stops\nAAPL,,,\n", Code::INVALID_HEADER, 1, ""},
        {"symbol,shard,orders,stops,levels,\nAAPL,,,,,\n", Code::INVALID_HEADER, 1, ""},
        {"Symbol,shard,orders,stops,levels\nAAPL,,,,\n", Code::INVALID_HEADER, 1, ""},
        {" symbol,shard,orders,stops,levels\nAAPL,,,,\n", Code::INVALID_HEADER, 1, ""},
        {"symbol,shard,orders,stops,levels\n", Code::NO_INSTRUMENTS, 1, ""},
        {"symbol,shard,orders,stops,levels", Code::NO_INSTRUMENTS, 1, ""},
        {"symbol,shard,orders,stops,levels\n\nAAPL,,,,\n", Code::EMPTY_RECORD, 2, ""},
        {"symbol,shard,orders,stops,levels\nAAPL,,,,\n\n", Code::EMPTY_RECORD, 3, ""},
        {"symbol,shard,orders,stops,levels\n\"AAPL\",,,,\n", Code::QUOTE_CHARACTER, 2, ""},
        {"symbol,shard,orders,stops,levels\nAAPL,,,\n", Code::WRONG_COLUMN_COUNT, 2, ""},
        {"symbol,shard,orders,stops,levels\nAAPL,,,,,\n", Code::WRONG_COLUMN_COUNT, 2, ""},
        {"symbol,shard,orders,stops,levels\n# a comment\n", Code::WRONG_COLUMN_COUNT, 2, ""},
        {"symbol,shard,orders,stops,levels\naapl,,,,\n", Code::INVALID_SYMBOL, 2, "symbol"},
        {"symbol,shard,orders,stops,levels\n,,,,\n", Code::INVALID_SYMBOL, 2, "symbol"},
        {"symbol,shard,orders,stops,levels\nABCDEFGHI,,,,\n", Code::INVALID_SYMBOL, 2, "symbol"},
        {"symbol,shard,orders,stops,levels\nAAPL ,,,,\n", Code::INVALID_SYMBOL, 2, "symbol"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,,,,\nAAPL,1,,,\n", Code::DUPLICATE_SYMBOL, 4, "symbol"},
        {"symbol,shard,orders,stops,levels\nAAPL,-1,,,\n", Code::INVALID_NUMBER, 2, "shard"},
        {"symbol,shard,orders,stops,levels\nAAPL,,+5,,\n", Code::INVALID_NUMBER, 2, "orders"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,1e3,\n", Code::INVALID_NUMBER, 2, "stops"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,, 5\n", Code::INVALID_NUMBER, 2, "levels"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,,5\r", Code::INVALID_NUMBER, 2, "levels"},
        {"symbol,shard,orders,stops,levels\nAAPL,,4294967296,,\n", Code::NUMBER_OUT_OF_RANGE, 2, "orders"},
        {"symbol,shard,orders,stops,levels\nAAPL,99999999999999999999999,,,\n", Code::NUMBER_OUT_OF_RANGE, 2, "shard"},
        {"symbol,shard,orders,stops,levels\nAAPL,,0,,\n", Code::INVALID_VALUE, 2, "orders"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,000,\n", Code::INVALID_VALUE, 2, "stops"},
        {"symbol,shard,orders,stops,levels\nAAPL,,,,0\n", Code::INVALID_VALUE, 2, "levels"},
        {"symbol,shard,orders,stops,levels\nAAPL,4,,,\n", Code::SHARD_OUT_OF_RANGE, 2, "shard"},
    };
    for (const Refusal& refusal : refusals) {
        const InstrumentListResult result = parseInstrumentList(refusal.text, 4);
        const InstrumentListError* error = std::get_if<InstrumentListError>(&result);
        expect(error != nullptr, "should be refused: " + std::string(refusal.text));
        expect(error->code == refusal.code, "wrong code for: " + std::string(refusal.text));
        expect(error->lineNumber == refusal.line, "wrong line for: " + std::string(refusal.text));
        expect(error->column == refusal.column, "wrong column for: " + std::string(refusal.text));
    }
    expect(std::holds_alternative<std::vector<Instrument>>(
               parseInstrumentList("symbol,shard,orders,stops,levels\nAAPL,3,,,\n", 4)),
           "the last shard is in range");
    expectThrows([] { (void)parseInstrumentList("symbol,shard,orders,stops,levels\nAAPL,,,,\n", 0); }, "no shards is a bug");
}

void testErrorFormat() {
    const InstrumentListError field = std::get<InstrumentListError>(
        parseInstrumentList("symbol,shard,orders,stops,levels\nAAPL,9,,,\n", 4));
    expect(formatInstrumentListError("list.csv", field) == "list.csv:2: shard: shard 9 is not below the shard count 4",
           "errors read <path>:<line>: <column>: <message>");
    const InstrumentListError record = std::get<InstrumentListError>(
        parseInstrumentList("symbol,shard,orders,stops,levels\n\n", 4));
    expect(formatInstrumentListError("list.csv", record) == "list.csv:2: a blank line", "a record-level error has no column");
}

void testHashIsSpecified() {
    // From a separate reading of FNV-1a's definition, over the space-padded 8 bytes.
    expect(symbolHash(symbol("AAPL")) == 0xe6277f8d5fc5493bull, "hash of AAPL");
    expect(symbolHash(symbol("MSFT")) == 0x5d4202d7c8b8a9e3ull, "hash of MSFT");
    expect(symbolHash(symbol("BRK.B")) == 0xe7d1f5bc4e2020feull, "hash of BRK.B");
    expect(symbolHash(symbol("ABCDEFGH")) == 0xb8c5303eddd8136dull, "hash of eight characters");
}

struct SamplePlacement {
    std::string_view symbol;
    std::uint32_t shards;
    std::uint32_t shard;
};

void testSamplePlacements() {
    // Changing any of these moves instruments under existing logs.
    const std::vector<SamplePlacement> samples = {
        {"AAPL", 1, 0},  {"AAPL", 2, 1},  {"AAPL", 4, 3},  {"AAPL", 7, 6},   {"AAPL", 16, 11},
        {"MSFT", 2, 1},  {"MSFT", 4, 3},  {"MSFT", 7, 1},  {"MSFT", 16, 3},
        {"GOOG", 2, 1},  {"GOOG", 4, 1},  {"GOOG", 7, 0},  {"GOOG", 16, 1},
        {"BRK.B", 2, 0}, {"BRK.B", 4, 2}, {"BRK.B", 7, 1}, {"BRK.B", 16, 14},
        {"BF-B", 3, 0},  {"BF-B", 4, 2},  {"X", 4, 1},     {"X", 16, 13},
        {"12345678", 3, 0}, {"12345678", 7, 4},
    };
    for (const SamplePlacement& sample : samples) {
        expect(hashedShard(symbol(sample.symbol), sample.shards) == sample.shard,
               "placement of " + std::string(sample.symbol) + " on " + std::to_string(sample.shards) + " shards");
    }
}

void testAddingInstrumentsMovesNothing() {
    const std::vector<std::string_view> first = {"AAPL", "MSFT", "GOOG", "BRK.B"};
    std::vector<Instrument> instruments;
    for (const std::string_view text : first) {
        instruments.push_back(unpinned(text));
    }
    instruments.push_back(pinned("HOT", 2));
    const InstrumentDirectory before(instruments, 4);

    for (const std::string_view text : {"AMD", "X", "BF-B", "ZZZZZZZZ", "A"}) {
        instruments.push_back(unpinned(text));
    }
    const InstrumentDirectory after(instruments, 4);

    for (const std::string_view text : {"AAPL", "MSFT", "GOOG", "BRK.B", "HOT"}) {
        expect(before.find(symbol(text))->shard == after.find(symbol(text))->shard,
               "adding instruments must not change the shard of " + std::string(text));
    }
}

void testDirectoryMapsSymbols() {
    const std::vector<Instrument> instruments = {pinned("MSFT", 1), pinned("AAPL", 1), pinned("GOOG", 0), pinned("AMD", 1)};
    const InstrumentDirectory directory(instruments, 3);

    expect(directory.shardCount() == 3 && directory.size() == 4, "the directory's sizes");
    expect(directory.find(symbol("GOOG")) == BookLocation{0, 0}, "a shard's only instrument is slot 0");
    expect(directory.find(symbol("AAPL")) == BookLocation{1, 0}, "slots are in symbol order");
    expect(directory.find(symbol("AMD")) == BookLocation{1, 1}, "slots are in symbol order");
    expect(directory.find(symbol("MSFT")) == BookLocation{1, 2}, "slots are in symbol order");
    expect(!directory.find(symbol("TSLA")).has_value(), "a symbol not on the list is not found");
    expect(!directory.find(Symbol{}).has_value(), "the absence of a symbol is not found");

    expect(directory.onShard(0).size() == 1 && directory.onShard(0)[0].symbol == symbol("GOOG"), "shard 0's instruments");
    expect(directory.onShard(1).size() == 3 && directory.onShard(1)[2].symbol == symbol("MSFT"), "shard 1's instruments, by slot");
    expect(directory.onShard(2).empty(), "a shard may hold no instruments");
    expect(directory.onShard(1)[0] == pinned("AAPL", 1), "a slot keeps its list row");
}

void testEverySlotMapsBack() {
    std::vector<Instrument> instruments;
    for (const char first : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
        for (const char second : std::string_view("ABCDEFGH")) {
            instruments.push_back(unpinned(std::string{first, second}));
        }
    }
    const InstrumentDirectory directory(instruments, 7);
    std::size_t total = 0;
    for (std::uint32_t shard = 0; shard < 7; ++shard) {
        const std::span<const Instrument> onShard = directory.onShard(shard);
        total += onShard.size();
        for (std::size_t slot = 0; slot < onShard.size(); ++slot) {
            expect(directory.find(onShard[slot].symbol) == BookLocation{shard, static_cast<std::uint32_t>(slot)},
                   "every slot maps back to its shard and slot");
            expect(hashedShard(onShard[slot].symbol, 7) == shard, "an unpinned instrument sits on its hashed shard");
        }
    }
    expect(total == instruments.size(), "every instrument is on exactly one shard");
}

void testPinsBeatTheHash() {
    expect(hashedShard(symbol("AAPL"), 4) == 3, "AAPL's hash would put it on shard 3");
    const InstrumentDirectory directory({pinned("AAPL", 0)}, 4);
    expect(directory.find(symbol("AAPL")) == BookLocation{0, 0}, "a pin replaces the hash");
}

void testLineOrderMeansNothing() {
    const std::vector<Instrument> one = parsed("symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,,5,,\nGOOG,,,,\nAMD,1,,,\n");
    const std::vector<Instrument> other = parsed("symbol,shard,orders,stops,levels\nAMD,1,,,\nGOOG,,,,\nAAPL,,,,\nMSFT,,5,,\n");
    const InstrumentDirectory left(one, 4);
    const InstrumentDirectory right(other, 4);
    for (std::uint32_t shard = 0; shard < 4; ++shard) {
        const std::span<const Instrument> a = left.onShard(shard);
        const std::span<const Instrument> b = right.onShard(shard);
        expect(a.size() == b.size(), "the same list gives the same shard sizes");
        for (std::size_t slot = 0; slot < a.size(); ++slot) {
            expect(a[slot] == b[slot], "the same list gives the same slots");
        }
    }
}

void testDirectoryRefusesWhatTheParserRefuses() {
    expectThrows([] { InstrumentDirectory({}, 4); }, "no instruments");
    expectThrows([] { InstrumentDirectory({unpinned("AAPL")}, 0); }, "no shards");
    expectThrows([] { InstrumentDirectory({unpinned("AAPL"), pinned("AAPL", 1)}, 4); }, "a repeated symbol");
    expectThrows([] { InstrumentDirectory({pinned("AAPL", 4)}, 4); }, "a pin past the last shard");
    expectThrows([] { InstrumentDirectory({Instrument{}}, 4); }, "an empty symbol");
}

void testCapacity() {
    const order_book::BookCapacity defaults{10, 20, 30};
    const order_book::BookCapacity plain = capacityOf(unpinned("AAPL"), defaults);
    expect(plain.orders == 10 && plain.stops == 20 && plain.levels == 30, "an empty column takes the default");

    Instrument partial = unpinned("AAPL");
    partial.stops = 7;
    const order_book::BookCapacity overridden = capacityOf(partial, defaults);
    expect(overridden.orders == 10 && overridden.stops == 7 && overridden.levels == 30, "each column overrides on its own");

    expect(defaultBookCapacity.orders == 4096 && defaultBookCapacity.stops == 1024 && defaultBookCapacity.levels == 256,
           "the documented defaults");
}

}  // namespace

int main() {
    testTheDocumentedExample();
    testAcceptedForms();
    testRefusals();
    testErrorFormat();
    testHashIsSpecified();
    testSamplePlacements();
    testAddingInstrumentsMovesNothing();
    testDirectoryMapsSymbols();
    testEverySlotMapsBack();
    testPinsBeatTheHash();
    testLineOrderMeansNothing();
    testDirectoryRefusesWhatTheParserRefuses();
    testCapacity();
    std::cout << "instrument directory tests passed" << std::endl;
    return EXIT_SUCCESS;
}
