#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/xxh64.hpp"

namespace {
long allocations = 0;
}

void* operator new(std::size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace order_book;
using namespace order_book::replay;
using Bytes = std::vector<std::byte>;
using Code = ParseErrorCode;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(std::string_view text) {
    return *Symbol::parse(text);
}

const Symbol aapl = symbolOf("AAPL");

struct Entry {
    Symbol symbol;
    Command command;
};

// A log built through the codec under test; the goldens prove the codec, independently.
Bytes makeLog(const LogHeader& header, const std::vector<Entry>& entries) {
    Bytes bytes = encodeLogHeader(header);
    std::uint32_t checksum = logChainSeed(bytes);
    for (const Entry& entry : entries) {
        std::array<std::byte, logRecordBytes> record;
        checksum = encodeLogRecord(entry.symbol, entry.command, checksum, record);
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

std::size_t recordsStart(const LogHeader& header = {}) {
    return encodeLogHeader(header).size();
}

// Puts a record's checksum right after a change, so the check under test is the one reached.
void rechain(Bytes& bytes, std::size_t start = recordsStart(), std::size_t recordBytes = logRecordBytes) {
    std::uint32_t checksum = 0;
    std::memcpy(&checksum, bytes.data() + start - sizeof(std::uint64_t), sizeof(checksum));
    for (std::size_t at = start; at + recordBytes <= bytes.size(); at += recordBytes) {
        checksum = static_cast<std::uint32_t>(xxh64(std::span<const std::byte>(bytes).subspan(at + 4, recordBytes - 4), checksum));
        std::memcpy(bytes.data() + at, &checksum, sizeof(checksum));
    }
}

struct ReadAll {
    std::vector<ReplayRecord> records;
    std::optional<ParseError> error;
    LogHeader header;
};

ReadAll readAll(const Bytes& bytes, std::size_t chunk = CommandLogReader::defaultChunkCapacity) {
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    CommandLogReader reader(input, chunk);
    ReadAll outcome;
    while (true) {
        ReadResult result = reader.next();
        if (ReplayRecord* record = std::get_if<ReplayRecord>(&result)) {
            outcome.records.push_back(*record);
        } else {
            if (ParseError* error = std::get_if<ParseError>(&result)) {
                outcome.error = *error;
            }
            break;
        }
    }
    if (reader.header() != nullptr) {
        outcome.header = *reader.header();
    }
    return outcome;
}

void expectError(const ReadAll& outcome, Code code, std::size_t line, const std::string& column, const std::string& context) {
    expect(outcome.error.has_value(), context + ": expected an error");
    expect(outcome.error->code == code, context + ": got " + std::string(parseErrorCodeName(outcome.error->code)));
    expect(outcome.error->lineNumber == line, context + ": line " + std::to_string(outcome.error->lineNumber));
    expect(outcome.error->column == column, context + ": column " + outcome.error->column);
}

std::vector<Entry> threeCommands() {
    return {Entry{aapl, AddCommand{1, Side::SELL, 101, 5}}, Entry{aapl, AddCommand{2, Side::BUY, 100, 4}},
            Entry{aapl, CancelCommand{1}}};
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(file.is_open(), "cannot open " + path.string());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Bytes bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

// From the reference implementation, not from the code under test.
void testXxh64Vectors() {
    const auto hash = [](std::string_view text, std::uint64_t seed = 0) {
        return xxh64(std::as_bytes(std::span<const char>(text.data(), text.size())), seed);
    };
    expect(hash("") == 0xEF46DB3751D8E999ULL, "the empty input");
    expect(hash("", 1) == 0xD5AFBA1336A3BE4BULL, "the empty input, seeded");
    expect(hash("a") == 0xD24EC4F1A98C6E5BULL, "one byte");
    expect(hash("abc") == 0x44BC2CF5AD770999ULL, "three bytes");
    expect(hash("message digest") == 0x066ED728FCEEB3BEULL, "fourteen bytes");
    expect(hash("abcdefghijklmnopqrstuvwxyz") == 0xCFE1F278FA89835CULL, "past 8 bytes, short of the long path");
    expect(hash("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == 0xAAA46907D3047814ULL, "the long path");
    expect(hash("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == 0xE04A477F19EE145DULL,
           "several long rounds");
    expect(hash("abcdefghijklmnopqrstuvwxyz0123456789", 12345) == 0x1F903AC69608D409ULL, "the long path, seeded");
}

// The golden records, listed by hand as make_golden.py lists them.
std::vector<Entry> everyCommand() {
    AddCommand postOnly{1, Side::BUY, 100, 3};
    postOnly.postOnly = true;
    AddCommand ioc{2, Side::BUY, 101, 2, OrderType::LIMIT, TimeInForce::IOC};
    ioc.owner = 7;
    ioc.selfTradePrevention = SelfTradePrevention::CANCEL_INCOMING;
    const AddCommand market{3, Side::BUY, 0, 1, OrderType::MARKET, TimeInForce::FOK};
    AddCommand stop{4, Side::SELL, 0, 2, OrderType::STOP, TimeInForce::IOC};
    stop.stopPrice = 90;
    AddCommand stopLimit{5, Side::BUY, 120, 2, OrderType::STOP_LIMIT, TimeInForce::GTC};
    stopLimit.stopPrice = 110;
    stopLimit.owner = 7;
    stopLimit.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    return {Entry{aapl, AddCommand{1, Side::SELL, 101, 5}},
            Entry{symbolOf("BRK.B"), postOnly},
            Entry{aapl, ioc},
            Entry{aapl, market},
            Entry{aapl, stop},
            Entry{aapl, stopLimit},
            Entry{aapl, ReplaceCommand{1, 102, 6, false, 9, SelfTradePrevention::CANCEL_RESTING}},
            Entry{symbolOf("BRK.B"), ReplaceCommand{1, 99, 3, true}},
            Entry{aapl, CancelCommand{5}},
            Entry{symbolOf("ES-Z26"), CancelCommand{77}}};
}

LogHeader goldenHeader() {
    LogHeader header;
    header.engineMajor = 2;
    header.engineMinor = 1;
    return header;
}

void testGoldenFiles(const std::filesystem::path& root) {
    const Bytes every = readFile(root / "log" / "every_command.oblog");
    expect(makeLog(goldenHeader(), everyCommand()) == every, "the encoder writes the golden log byte for byte");
    const ReadAll read = readAll(every);
    expect(!read.error, "the golden log reads to its end");
    expect(read.records.size() == 10, "every record reads");
    for (std::size_t i = 0; i < read.records.size(); ++i) {
        expect(read.records[i].command == everyCommand()[i].command, "the golden commands read back");
        expect(read.records[i].symbol == everyCommand()[i].symbol, "the golden symbols read back");
        expect(read.records[i].sequence == i + 1, "sequence is position + 1");
        expect(read.records[i].lineNumber == i + 2, "line is the twin's");
    }

    LogHeader placement = goldenHeader();
    placement.firstPosition = 1000;
    placement.shardCount = 4;
    placement.shardIndex = 2;
    placement.symbols = {aapl, symbolOf("MSFT")};
    const Bytes placed = readFile(root / "log" / "placement.oblog");
    const std::vector<Entry> entries{Entry{aapl, AddCommand{1, Side::SELL, 101, 5}}, Entry{aapl, CancelCommand{1}}};
    expect(makeLog(placement, entries) == placed, "the encoder writes a placement byte for byte");
    const ReadAll readPlaced = readAll(placed);
    expect(!readPlaced.error && readPlaced.records.size() == 2, "a placed log reads");
    expect(readPlaced.header.firstPosition == 1000 && readPlaced.header.shardCount == 4 && readPlaced.header.shardIndex == 2,
           "the header reads back");
    expect(readPlaced.header.symbols == placement.symbols && readPlaced.header.engineMinor == 1, "and its symbols");
    expect(readPlaced.records[1].sequence == 1002 && readPlaced.records[1].lineNumber == 3,
           "position counts from the header, and the line from the file");

    const Bytes empty = readFile(root / "log" / "header_only.oblog");
    expect(makeLog(goldenHeader(), {}) == empty, "a header alone is byte for byte");
    expect(!readAll(empty).error && readAll(empty).records.empty(), "a header alone is a clean end");
}

// Every valid sample, as a log: the same commands, and rows that parse back to them.
void testEveryCsvSampleRoundTrips(const std::filesystem::path& root) {
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "valid")) {
        const ReplayInput input = readReplayFile(entry.path());
        expect(!input.error, "sample reads: " + entry.path().string());
        std::vector<Entry> entries;
        for (const ReplayRecord& record : input.records) {
            entries.push_back(Entry{record.symbol.empty() ? aapl : record.symbol, record.command});
        }
        const ReadAll read = readAll(makeLog({}, entries));
        expect(!read.error, "log reads: " + entry.path().string());
        expect(read.records.size() == entries.size(), "every record: " + entry.path().string());

        std::string csv = std::string(expectedHeaderV3) + '\n';
        for (std::size_t i = 0; i < entries.size(); ++i) {
            expect(read.records[i].command == entries[i].command, "same command: " + entry.path().string());
            csv += formatReplayRow(read.records[i]) + '\n';
        }
        // Log to CSV to log is byte-identical, for a log with no placement at position 0.
        std::istringstream text(csv);
        const ReplayInput again = readReplayInput(text);
        expect(!again.error, "a converted row parses: " + entry.path().string());
        std::vector<Entry> reparsed;
        for (const ReplayRecord& record : again.records) {
            reparsed.push_back(Entry{record.symbol, record.command});
        }
        expect(makeLog({}, reparsed) == makeLog({}, entries), "log to CSV to log is byte-identical: " + entry.path().string());
        ++files;
    }
    expect(files >= 20, "the samples were found");
}

void testFormatReplayRow() {
    AddCommand stop{4, Side::SELL, 0, 2, OrderType::STOP, TimeInForce::IOC};
    stop.stopPrice = 90;
    stop.owner = 7;
    stop.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    expect(formatReplayRow(ReplayRecord{9, 8, stop, aapl}) == "8,ADD,4,SELL,,2,STOP,IOC,90,,7,CANCEL_BOTH,AAPL", "an ADD's row");
    expect(formatReplayRow(ReplayRecord{9, 8, CancelCommand{3}, aapl}) == "8,CANCEL,3,,,,,,,,,,AAPL", "a CANCEL's row");
    expect(formatReplayRow(ReplayRecord{9, 8, ReplaceCommand{3, 101, 6, true}, aapl}) == "8,REPLACE,3,,101,6,,,,Y,,,AAPL",
           "a REPLACE's row");
}

// One record, with one byte set and the chain repaired, must give exactly this.
struct FieldCase {
    std::size_t offset;
    std::uint8_t value;
    Code code;
    const char* column;
    const char* name;
};

void testOneCasePerCode() {
    const std::vector<Entry> one{Entry{aapl, AddCommand{1, Side::SELL, 101, 5}}};
    const std::vector<FieldCase> cases{
        {4, 0, Code::UNKNOWN_COMMAND, "command", "command 0"},
        {4, 4, Code::UNKNOWN_COMMAND, "command", "an unknown command"},
        {5, 0, Code::MISSING_REQUIRED_FIELD, "side", "an absent side"},
        {5, 3, Code::INVALID_SIDE, "side", "an unknown side"},
        {6, 0, Code::MISSING_REQUIRED_FIELD, "order_type", "an absent type"},
        {6, 5, Code::INVALID_ORDER_TYPE, "order_type", "an unknown type"},
        {7, 0, Code::MISSING_REQUIRED_FIELD, "time_in_force", "an absent time in force"},
        {7, 4, Code::INVALID_TIME_IN_FORCE, "time_in_force", "an unknown time in force"},
        {8, 'a', Code::INVALID_SYMBOL, "symbol", "a lower-case symbol"},
        {16, 0, Code::INVALID_VALUE, "order_id", "order 0"},  // bytes 17 to 19 are zero already
        {20, 0, Code::MISSING_REQUIRED_FIELD, "price", "an absent price"},
        {24, 0, Code::MISSING_REQUIRED_FIELD, "quantity", "an absent quantity"},
        {28, 1, Code::UNEXPECTED_FIELD, "stop_price", "a stop price on a limit order"},
        {37, 1, Code::UNEXPECTED_FIELD, "self_trade_prevention", "a mode with no owner"},
        {36, 2, Code::UNKNOWN_FIELD, "", "an unknown flag"},
        {38, 1, Code::UNKNOWN_FIELD, "", "a reserved byte"},
        {39, 1, Code::UNKNOWN_FIELD, "", "the other reserved byte"},
    };
    for (const FieldCase& field : cases) {
        Bytes bytes = makeLog({}, one);
        bytes[recordsStart() + field.offset] = static_cast<std::byte>(field.value);
        rechain(bytes);
        expectError(readAll(bytes), field.code, 2, field.column, field.name);
    }
}

void testCombinations() {
    const auto fails = [](AddCommand add, const std::function<void(Bytes&)>& change, Code code, const char* column, const char* name) {
        Bytes bytes = makeLog({}, {Entry{aapl, add}});
        change(bytes);
        rechain(bytes);
        expectError(readAll(bytes), code, 2, column, name);
    };
    const std::size_t at = recordsStart();
    fails(AddCommand{1, Side::BUY, 100, 5}, [at](Bytes& b) { b[at + 6] = std::byte{2}; b[at + 20] = std::byte{0}; },
          Code::INVALID_COMBINATION, "time_in_force", "a MARKET order with GTC");
    fails(AddCommand{1, Side::BUY, 100, 5, OrderType::LIMIT, TimeInForce::IOC}, [at](Bytes& b) { b[at + 36] = std::byte{1}; },
          Code::INVALID_COMBINATION, "post_only", "post-only on an IOC");
    fails(AddCommand{1, Side::BUY, 0, 5, OrderType::MARKET, TimeInForce::IOC}, [at](Bytes& b) { b[at + 20] = std::byte{9}; },
          Code::UNEXPECTED_FIELD, "price", "a price on a market order");
    fails(AddCommand{1, Side::BUY, 100, 5}, [at](Bytes& b) { b[at + 6] = std::byte{4}; },
          Code::MISSING_REQUIRED_FIELD, "stop_price", "a stop limit with no stop");
    fails(AddCommand{1, Side::BUY, 100, 5}, [at](Bytes& b) { b[at + 32] = std::byte{7}; },
          Code::MISSING_REQUIRED_FIELD, "self_trade_prevention", "an owner with no mode");
    fails(AddCommand{1, Side::BUY, 100, 5}, [at](Bytes& b) { b[at + 32] = std::byte{7}; b[at + 37] = std::byte{4}; },
          Code::INVALID_SELF_TRADE_PREVENTION, "self_trade_prevention", "an unknown mode");

    Bytes cancel = makeLog({}, {Entry{aapl, CancelCommand{1}}});
    cancel[at + 20] = std::byte{1};
    rechain(cancel);
    expectError(readAll(cancel), Code::UNEXPECTED_FIELD, 2, "price", "a price on a cancel");

    Bytes replace = makeLog({}, {Entry{aapl, ReplaceCommand{1, 100, 5}}});
    replace[at + 5] = std::byte{1};
    rechain(replace);
    expectError(readAll(replace), Code::UNEXPECTED_FIELD, 2, "side", "a side on a replace");
}

void testHeaderErrors() {
    const Bytes log = makeLog({}, threeCommands());
    const auto expectHeaderError = [](const Bytes& bytes, Code code, const std::string& context) {
        const ReadAll outcome = readAll(bytes);
        expectError(outcome, code, 1, "", context);
        expect(outcome.records.empty(), context + ": nothing runs");
    };
    for (std::size_t cut = 0; cut < recordsStart(); ++cut) {
        expectHeaderError(Bytes(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(cut)), Code::CORRUPT_HEADER, "a cut header");
    }
    Bytes magic = log;
    magic[0] = std::byte{'X'};
    expectHeaderError(magic, Code::CORRUPT_HEADER, "the wrong magic");
    Bytes flipped = log;
    flipped[33] ^= std::byte{1};
    expectHeaderError(flipped, Code::CORRUPT_HEADER, "a flipped header bit");

    LogHeader newer;
    newer.minimumReaderVersion = logFormatVersion + 1;
    expectHeaderError(encodeLogHeader(newer), Code::UNSUPPORTED_VERSION, "a newer minimum reader");
    LogHeader tooSmall;
    tooSmall.recordBytes = 39;
    expectHeaderError(encodeLogHeader(tooSmall), Code::CORRUPT_HEADER, "a record smaller than the format's");

    // A header whose checksum is right, with a reserved byte set.
    Bytes reserved = encodeLogHeader({});
    reserved[52] = std::byte{1};
    const std::uint64_t fixed = xxh64(std::span<const std::byte>(reserved).first(logHeaderBytes));
    std::memcpy(reserved.data() + logHeaderBytes, &fixed, sizeof(fixed));
    expectHeaderError(reserved, Code::UNKNOWN_FIELD, "a reserved header byte");
}

void testEveryCutOfTheTail() {
    const Bytes log = makeLog({}, threeCommands());
    const std::size_t start = recordsStart();
    for (std::size_t cut = start; cut <= log.size(); ++cut) {
        const ReadAll outcome = readAll(Bytes(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(cut)));
        const std::size_t whole = (cut - start) / logRecordBytes;
        expect(outcome.records.size() == whole, "a cut keeps every whole record");
        if ((cut - start) % logRecordBytes == 0) {
            expect(!outcome.error, "a cut on a boundary looks like a clean end");
        } else {
            expectError(outcome, Code::TORN_TAIL, whole + 2, "", "a cut inside a record");
        }
    }
}

void testZeroFillAndStaleTails() {
    Bytes zeros = makeLog({}, threeCommands());
    zeros.resize(zeros.size() + logRecordBytes * 5, std::byte{0});
    ReadAll outcome = readAll(zeros);
    expect(outcome.records.size() == 3, "the good prefix reads");
    expectError(outcome, Code::TORN_TAIL, 5, "", "a zero-filled tail");

    // A valid record of another chain, left behind.
    Bytes stale = makeLog({}, threeCommands());
    LogHeader other;
    other.firstPosition = 500;
    const Bytes otherLog = makeLog(other, {Entry{aapl, CancelCommand{9}}});
    stale.insert(stale.end(), otherLog.end() - logRecordBytes, otherLog.end());
    outcome = readAll(stale);
    expect(outcome.records.size() == 3, "the good prefix reads");
    expectError(outcome, Code::TORN_TAIL, 5, "", "a stale record");
}

void testTheChainCatchesMovedRecords() {
    const std::size_t start = recordsStart();
    const Bytes log = makeLog({}, {Entry{aapl, CancelCommand{1}}, Entry{aapl, CancelCommand{2}}, Entry{aapl, CancelCommand{3}},
                                   Entry{aapl, CancelCommand{4}}, Entry{aapl, CancelCommand{5}}});
    const auto recordAt = [&](std::size_t index) { return log.begin() + static_cast<std::ptrdiff_t>(start + index * logRecordBytes); };

    Bytes dropped(log.begin(), recordAt(1));
    dropped.insert(dropped.end(), recordAt(2), log.end());
    ReadAll outcome = readAll(dropped);
    expect(outcome.records.size() == 1, "a dropped record stops the read there");
    expectError(outcome, Code::CORRUPT_RECORD, 3, "", "a dropped record");

    Bytes duplicated(log.begin(), recordAt(2));
    duplicated.insert(duplicated.end(), recordAt(1), log.end());
    outcome = readAll(duplicated);
    expect(outcome.records.size() == 2, "a duplicated record stops the read there");
    expectError(outcome, Code::CORRUPT_RECORD, 4, "", "a duplicated record");

    Bytes swapped = log;
    std::swap_ranges(swapped.begin() + static_cast<std::ptrdiff_t>(start + logRecordBytes),
                     swapped.begin() + static_cast<std::ptrdiff_t>(start + 2 * logRecordBytes),
                     swapped.begin() + static_cast<std::ptrdiff_t>(start + 2 * logRecordBytes));
    outcome = readAll(swapped);
    expect(outcome.records.size() == 1, "swapped records stop the read there");
    expectError(outcome, Code::CORRUPT_RECORD, 3, "", "swapped records");

    Bytes flipped = log;
    flipped[start + logRecordBytes + 20] ^= std::byte{1};
    outcome = readAll(flipped);
    expectError(outcome, Code::CORRUPT_RECORD, 3, "", "a flipped bit with good records after it");

    Bytes hole = log;
    std::fill(hole.begin() + static_cast<std::ptrdiff_t>(start + logRecordBytes),
              hole.begin() + static_cast<std::ptrdiff_t>(start + 2 * logRecordBytes), std::byte{0});
    outcome = readAll(hole);
    expectError(outcome, Code::CORRUPT_RECORD, 3, "", "a hole with good records after it");
}

void testTinyChunks() {
    const Bytes log = makeLog({}, everyCommand());
    const ReadAll whole = readAll(log);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{39},
                                    std::size_t{40}, std::size_t{41}, std::size_t{72}}) {
        const ReadAll small = readAll(log, chunk);
        expect(!small.error && small.records == whole.records, "any chunk size reads the same");
        Bytes torn = log;
        torn.resize(torn.size() - 3);
        expectError(readAll(torn, chunk), Code::TORN_TAIL, 11, "", "a torn tail at any chunk size");
    }
}

// Changing any byte is refused or stays a sound command. It must never crash, under ASan and
// UBSan above all.
void testByteSweep() {
    const Bytes log = makeLog(goldenHeader(), everyCommand());
    std::uint32_t state = 1;
    for (std::size_t at = 0; at < log.size(); ++at) {
        for (int round = 0; round < 3; ++round) {
            state = state * 1664525U + 1013904223U;
            Bytes changed = log;
            changed[at] ^= static_cast<std::byte>(1U + (state >> 24) % 255U);
            const ReadAll outcome = readAll(changed);
            expect(outcome.error.has_value(), "a changed byte fails its chain at offset " + std::to_string(at));
        }
    }
}

std::string sideText(std::uint8_t value) {
    return value == 0 ? "" : value == 1 ? "BUY" : value == 2 ? "SELL" : "X";
}
std::string typeText(std::uint8_t value) {
    constexpr std::array<const char*, 5> names{"", "LIMIT", "MARKET", "STOP", "STOP_LIMIT"};
    return value < names.size() ? names[value] : "X";
}
std::string timeText(std::uint8_t value) {
    constexpr std::array<const char*, 4> names{"", "GTC", "IOC", "FOK"};
    return value < names.size() ? names[value] : "X";
}
std::string modeText(std::uint8_t value) {
    constexpr std::array<const char*, 4> names{"", "CANCEL_RESTING", "CANCEL_INCOMING", "CANCEL_BOTH"};
    return value < names.size() ? names[value] : "X";
}
std::string numberText(std::uint32_t value) {
    return value == 0 ? "" : std::to_string(value);
}

// Random fields with valid checksums: a record decodes exactly when its v3 row parses, to the
// same command. That pins the two check lists together without sharing code.
void testCsvEquivalence() {
    std::uint32_t state = 12345;
    const auto draw = [&state](std::uint32_t bound) {
        state = state * 1664525U + 1013904223U;
        return (state >> 8) % bound;
    };
    const Bytes empty = encodeLogHeader({});
    const std::uint32_t seed = logChainSeed(empty);
    std::size_t accepted = 0;
    for (int trial = 0; trial < 200000; ++trial) {
        const auto command = static_cast<std::uint8_t>(1 + draw(3));
        // Each field is usually valid for its command, and otherwise anything, so that plenty
        // of records pass and each check is still reached alone.
        const bool add = command == 1;
        const bool replace = command == 3;
        const bool wild = draw(3) == 0;
        const auto byteOr = [&](std::uint8_t usual, std::uint32_t bound) {
            return wild && draw(3) == 0 ? static_cast<std::uint8_t>(draw(bound)) : usual;
        };
        const auto type = byteOr(add ? static_cast<std::uint8_t>(1 + draw(4)) : 0, 6);
        const bool priceless = type == 2 || type == 3;
        const bool stopping = type == 3 || type == 4;
        const auto time = byteOr(add ? static_cast<std::uint8_t>(priceless ? 2 + draw(2) : 1 + draw(3)) : 0, 5);
        const auto side = byteOr(add ? static_cast<std::uint8_t>(1 + draw(2)) : 0, 4);
        const std::uint32_t price = (wild && draw(3) == 0 ? draw(2) == 0 : (replace || (add && !priceless))) ? 100 : 0;
        const std::uint32_t quantity = (wild && draw(3) == 0 ? draw(2) == 0 : (add || replace)) ? 5 : 0;
        const std::uint32_t stop = (wild && draw(3) == 0 ? draw(2) == 0 : (add && stopping)) ? 90 : 0;
        const bool canOwn = add || replace;
        const std::uint32_t owner = (wild && draw(3) == 0 ? draw(2) == 0 : (canOwn && draw(2) == 0)) ? 7 : 0;
        const bool postOnly = wild && draw(3) == 0 ? draw(2) == 0 : (replace && draw(4) == 0) || (add && type == 1 && time == 1 && draw(4) == 0);
        const auto mode = byteOr(owner != 0 ? static_cast<std::uint8_t>(1 + draw(3)) : 0, 5);

        Bytes record(logRecordBytes, std::byte{0});
        record[4] = static_cast<std::byte>(command);
        record[5] = static_cast<std::byte>(side);
        record[6] = static_cast<std::byte>(type);
        record[7] = static_cast<std::byte>(time);
        std::memcpy(record.data() + 8, "AAPL    ", 8);
        record[16] = std::byte{1};
        std::memcpy(record.data() + 20, &price, 4);
        std::memcpy(record.data() + 24, &quantity, 4);
        std::memcpy(record.data() + 28, &stop, 4);
        std::memcpy(record.data() + 32, &owner, 4);
        record[36] = postOnly ? std::byte{1} : std::byte{0};
        record[37] = static_cast<std::byte>(mode);
        const auto checksum = static_cast<std::uint32_t>(xxh64(std::span<const std::byte>(record).subspan(4), seed));
        std::memcpy(record.data(), &checksum, 4);

        const char* names[] = {"", "ADD", "CANCEL", "REPLACE"};
        const CsvRow row = splitRecord("1," + std::string(names[command]) + ",1," + sideText(side) + ',' + numberText(price) +
                                           ',' + numberText(quantity) + ',' + typeText(type) + ',' + timeText(time) + ',' +
                                           numberText(stop) + ',' + (postOnly ? "Y" : "") + ',' + numberText(owner) + ',' +
                                           modeText(mode) + ",AAPL",
                                       2);
        const ParseResult fromRow = parseRow(row, ReplayFormat::V3, std::nullopt);
        const ParseResult fromLog = decodeLogRecord(record, seed, 0, 0);
        const auto* rowRecord = std::get_if<ReplayRecord>(&fromRow);
        const auto* logRecord = std::get_if<ReplayRecord>(&fromLog);
        expect((rowRecord != nullptr) == (logRecord != nullptr), "a record decodes exactly when its row parses: " + row.fields[1]);
        if (rowRecord != nullptr) {
            expect(rowRecord->command == logRecord->command && rowRecord->symbol == logRecord->symbol, "to the same command");
            ++accepted;
        } else {
            expect(std::get<ParseError>(fromRow).column == std::get<ParseError>(fromLog).column,
                   "the same column is at fault: " + std::get<ParseError>(fromRow).column + " and " +
                       std::get<ParseError>(fromLog).column);
        }
    }
    expect(accepted > 50000 && accepted < 180000, "the draws reach valid and refused records alike: " + std::to_string(accepted));
}

// Records longer than this reader knows: zero tails read, and a nonzero one stops there.
void testExtension() {
    LogHeader header;
    header.recordBytes = logRecordBytes + 8;
    const std::vector<std::byte> head = encodeLogHeader(header);
    const std::size_t start = head.size();
    Bytes bytes = head;
    for (const Entry& entry : threeCommands()) {
        std::array<std::byte, logRecordBytes> record;
        encodeLogRecord(entry.symbol, entry.command, 0, record);
        bytes.insert(bytes.end(), record.begin(), record.end());
        bytes.insert(bytes.end(), 8, std::byte{0});
    }
    rechain(bytes, start, header.recordBytes);
    ReadAll outcome = readAll(bytes);
    expect(!outcome.error && outcome.records.size() == 3, "a longer record with a zero tail reads");

    bytes[start + 48 + logRecordBytes + 3] = std::byte{1};
    rechain(bytes, start, header.recordBytes);
    outcome = readAll(bytes);
    expect(outcome.records.size() == 1, "records before it read");
    expectError(outcome, Code::UNKNOWN_FIELD, 3, "", "a record that uses a field this reader lacks");

    LogHeader longer;
    longer.headerBytes = logHeaderBytes + 16;
    const ReadAll longerHeader = readAll(encodeLogHeader(longer));
    expect(!longerHeader.error, "a longer header with zeros in it reads");
}

void testAllocation() {
#if !defined(_ITERATOR_DEBUG_LEVEL) || _ITERATOR_DEBUG_LEVEL == 0
    std::array<std::byte, logRecordBytes> record;
    const Command command = AddCommand{1, Side::SELL, 101, 5};
    long before = allocations;
    std::uint32_t checksum = 0;
    for (int i = 0; i < 1000; ++i) {
        checksum = encodeLogRecord(aapl, command, checksum, record);
    }
    long made = allocations - before;
    expect(made == 0, "encodeLogRecord never allocates");

    std::vector<Entry> entries;
    for (OrderId id = 1; id <= 1000; ++id) {
        entries.push_back(Entry{aapl, CancelCommand{id}});
    }
    const Bytes log = makeLog({}, entries);
    std::istringstream input(std::string(reinterpret_cast<const char*>(log.data()), log.size()));
    CommandLogReader reader(input);
    reader.next();  // the header and first chunk
    before = allocations;
    for (int i = 0; i < 998; ++i) {
        reader.next();
    }
    made = allocations - before;
    expect(made == 0, "next allocates nothing per record, made " + std::to_string(made));
#endif
    expect(sizeof(ReadResult) > 0, "the reader's result is a value");
}

void testSniffing(const std::filesystem::path& root) {
    expect(isCommandLogFile(root / "log" / "every_command.oblog"), "a log is told by its first bytes");
    expect(!isCommandLogFile(root / "valid" / "basic.csv"), "a CSV is not a log");
    expect(!isCommandLogFile(root / "no_such_file"), "a missing file is not a log");
    const ReplayInput log = readReplayOrLogFile(root / "log" / "every_command.oblog");
    expect(!log.error && log.records.size() == 10 && log.format == ReplayFormat::V3, "the log path reads a log");
    const ReplayInput csv = readReplayOrLogFile(root / "valid" / "basic.csv");
    expect(!csv.error && csv.records.size() == 4 && csv.format == ReplayFormat::V1, "the same call reads a CSV");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: command_log_tests <tests/replay directory>\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path root = argv[1];
    testXxh64Vectors();
    testGoldenFiles(root);
    testEveryCsvSampleRoundTrips(root);
    testFormatReplayRow();
    testOneCasePerCode();
    testCombinations();
    testHeaderErrors();
    testEveryCutOfTheTail();
    testZeroFillAndStaleTails();
    testTheChainCatchesMovedRecords();
    testTinyChunks();
    testByteSweep();
    testCsvEquivalence();
    testExtension();
    testAllocation();
    testSniffing(root);
    std::cout << "command log tests passed\n";
    return EXIT_SUCCESS;
}
