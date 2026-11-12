#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/runtime/shard.hpp"
#include "order_book/xxh64.hpp"

// Recovery from position 0 as plain calls on the in-memory LogStorage: logs written the way the
// engine writes them, by Housekeeping and a LogWriter, stopped cleanly or left as a crash leaves
// them, then surveyed and replayed into a new Shard, whose books must equal a witness that ran
// the same commands. See docs/internals/runtime.md.
namespace {

using namespace order_book;
using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard, std::optional<std::size_t> orders = std::nullopt,
              std::optional<std::size_t> stops = std::nullopt, std::optional<std::size_t> levels = std::nullopt) {
    return Instrument{*Symbol::parse(text), shard, orders, stops, levels};
}

// AAPL and MSFT on shard 0, IBM on shard 1, of two.
InstrumentDirectory twoShards() {
    return InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1)}, 2);
}

struct Placed {
    std::uint32_t slot;
    replay::Command command;
};

// Adds on alternating slots and sides, at prices that cross often enough to trade, then a cancel
// of every fifth order, so the books hold resting orders and the log holds every command kind.
std::vector<Placed> commandsFor(std::size_t slots, std::size_t count) {
    std::vector<Placed> placed;
    for (std::size_t i = 1; i <= count; ++i) {
        const auto slot = static_cast<std::uint32_t>(i % slots);
        if (i % 5 == 0) {
            placed.push_back(Placed{slot, replay::CancelCommand{static_cast<OrderId>(i - 2)}});
            continue;
        }
        replay::AddCommand add{};
        add.orderId = static_cast<OrderId>(i);
        add.side = i % 2 == 0 ? Side::BUY : Side::SELL;
        add.price = static_cast<Price>(100 + (i * 7) % 5);
        add.quantity = static_cast<Quantity>(1 + i % 3);
        placed.push_back(Placed{slot, add});
    }
    return placed;
}

// What a shard's files of this many records each take.
std::uint64_t fileBytesFor(const InstrumentDirectory& directory, std::uint32_t shard, std::uint64_t records) {
    return logFileShape(directory, shard, Durability::MACHINE, defaultLogFileBytes).headerBytes + records * replay::logRecordBytes;
}

// What an engine's first start publishes before its first file: an empty book's checkpoint for
// each of the shard's instruments that has none, at where the log begins.
void publishEmpties(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shard, std::uint64_t position) {
    const std::vector<std::string> names = storage.names();
    for (const Instrument& instrument : directory.onShard(shard)) {
        const bool has = std::any_of(names.begin(), names.end(), [&instrument](const std::string& name) {
            const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
            return parts && !parts->temporary && parts->symbol == instrument.symbol;  // a .tmp holds nothing a start reads
        });
        if (!has) {
            publishEmptyCheckpoint(storage, instrument.symbol, directory.shardCount(), shard, position);
        }
    }
}

// The log's files, a file being made included, not the checkpoints beside them.
std::vector<std::string> logNames(const LogStorage& storage) {
    std::vector<std::string> names = storage.names();
    std::erase_if(names, [](const std::string& name) { return !namedLikeALogFile(name); });
    return names;
}

// A shard's log as the engine writes it: housekeeping makes each file and the next one ahead,
// and the log writer writes every command. A clean stop finishes the last file and removes the
// unused next one; otherwise both stay as a crash leaves them.
void writeLog(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shard, const std::vector<Placed>& commands,
              bool cleanStop, std::uint64_t fileBytes = defaultLogFileBytes, Durability durability = Durability::MACHINE,
              std::uint64_t firstPosition = 0) {
    publishEmpties(storage, directory, shard, firstPosition);
    Housekeeping housekeeping(storage, directory, durability, fileBytes);
    Ring<LogEntry> ring(4096);
    DurablePosition durable(firstPosition);
    LogWriter writer(housekeeping, shard, ring, durable, firstPosition);
    const auto drain = [&] {
        while (writer.pass() != 0 || housekeeping.pass() != 0) {
        }
    };
    for (const Placed& placed : commands) {
        if (!ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND})) {
            drain();
            expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "the writer makes room");
        }
    }
    drain();
    if (cleanStop) {
        expect(ring.tryPush(LogEntry::stop()), "and the stop");
        drain();
    }
    if (cleanStop) {
        expect(writer.stopped(), "the writer stopped");
        housekeeping.removeUnused();
    }
    expect(durable.load() == firstPosition + commands.size(), "every command reached the log");
}

// The same commands on plain books, which nothing restores.
std::vector<OrderBook> witnessOf(std::size_t slots, const std::vector<Placed>& commands) {
    std::vector<OrderBook> books(slots);
    for (const Placed& placed : commands) {
        static_cast<void>(replay::executeCommand(books[placed.slot], placed.command));
    }
    return books;
}

bool sameBook(const OrderBook& left, const OrderBook& right) {
    const BookSnapshot a = left.snapshot(100000);
    const BookSnapshot b = right.snapshot(100000);
    const auto sameLevels = [](const std::vector<PriceLevelSummary>& x, const std::vector<PriceLevelSummary>& y) {
        return std::equal(x.begin(), x.end(), y.begin(), y.end(), [](const PriceLevelSummary& p, const PriceLevelSummary& q) {
            return p.price == q.price && p.totalQuantity == q.totalQuantity && p.orderCount == q.orderCount;
        });
    };
    return sameLevels(a.bids, b.bids) && sameLevels(a.asks, b.asks) && left.activeOrderCount() == right.activeOrderCount() &&
           left.pendingStopCount() == right.pendingStopCount() && left.getLastTradePrice() == right.getLastTradePrice();
}

void expectBooksEqual(const Shard& shard, const std::vector<OrderBook>& witness, const std::string& what) {
    expect(shard.size() == witness.size(), what + ": one book per slot");
    for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
        expect(sameBook(shard.books().book(slot), witness[slot]), what + ": slot " + std::to_string(slot) + "'s book equals the witness");
    }
}

const std::atomic<bool> running{false};

RecoveredShard recover(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t number, Shard& shard,
                       const std::atomic<bool>& stopping = running) {
    const LogSurvey survey = surveyLog(storage, directory);
    return recoverShard(storage, storage, directory, survey.shards.at(number), survey.checkpoints.at(number), shard, defaultBookCapacity, stopping);
}

// The error a call throws, which it must.
RecoveryError errorFrom(const std::function<void()>& call, const std::string& what) {
    try {
        call();
    } catch (const RecoveryError& error) {
        return error;
    }
    expect(false, what + " should have thrown RecoveryError");
    return RecoveryError(RecoveryError::Kind::START_REFUSED, "");
}

void expectError(const std::function<void()>& call, RecoveryError::Kind kind, const std::string& message, const std::string& what) {
    const RecoveryError error = errorFrom(call, what);
    expect(error.kind() == kind, what + ": the kind of refusal, with: " + error.what());
    expect(error.what() == message, what + ": the message, which was:\n" + error.what() + "\nnot:\n" + message);
}

std::vector<std::byte> bytesOf(LogStorage& storage, const std::string& name) {
    const std::unique_ptr<LogFile> file = storage.open(name);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
    expect(file->read(0, bytes) == bytes.size(), name + " reads whole");
    return bytes;
}

// A file of exactly these bytes under this name, replacing any.
void putFile(InMemoryLogStorage& storage, const std::string& name, const std::vector<std::byte>& bytes) {
    storage.remove(name);
    const std::unique_ptr<LogFile> file = storage.create(name, Durability::MACHINE);
    file->write(0, bytes);
    file->close();
}

void testNothingToRecover() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    const LogSurvey survey = surveyLog(storage, directory);
    expect(survey.shards.size() == 2 && survey.shards[0].empty() && survey.shards[1].empty() && survey.temporaries.empty(),
           "an empty directory holds nothing for either shard");
    expect(survey.named.empty(), "and names no symbol");
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
    expect(recovered.replayed == 0 && recovered.tailCutBytes == 0 && recovered.filesRemoved == 0, "and a shard recovers nothing");
    expect(recovered.warmed.size() == 2 && recovered.warmed[0].orders == defaultBookCapacity.orders &&
               recovered.warmed[1].levels == defaultBookCapacity.levels,
           "each book warmed to what it declares");
}

// A cleanly stopped log across several files replays to the witness's books, and is left alone.
void testACleanLogReplays() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 30);
    writeLog(storage, directory, 0, commands, true, fileBytesFor(directory, 0, 8));
    const std::vector<std::string> before = logNames(storage);
    expect(before.size() == 4, "thirty records in files of eight is four files");
    const LogSurvey survey = surveyLog(storage, directory);
    expect(survey.shards[0].size() == 4 && survey.shards[0][3] == LogFileEntry{logFileName(0, 24), 24} && survey.shards[1].empty(),
           "the survey finds shard 0's files in order, each at its first position");
    expect(survey.named == std::vector<Symbol>{*Symbol::parse("AAPL"), *Symbol::parse("MSFT")},
           "and the symbols their headers name, sorted, once each: shard 0's, not IBM");

    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
    expect(recovered.replayed == 30 && shard.nextPosition() == 30, "every record replayed, so the shard goes on from 30");
    expect(recovered.tailCutBytes == 0 && recovered.filesRemoved == 0, "with nothing to cut or remove");
    expectBooksEqual(shard, witnessOf(2, commands), "a clean log");
    expect(logNames(storage) == before, "and every file stays");
    expect(!shard.checkInvariants(), "the shard is sound");
}

// A crash leaves the last file's zero tail and the next file made ahead: the next file, with no
// records, goes, even with half a record torn as its first, and the tail is cut, even with half a
// record written in it, each counting the written bytes it drops.
void testATornTailIsCut() {
    enum class Tear { NONE, IN_THE_TAIL, IN_THE_NEXT_FILE };
    for (const Tear tear : {Tear::NONE, Tear::IN_THE_TAIL, Tear::IN_THE_NEXT_FILE}) {
        const std::string what = tear == Tear::NONE ? "a zero tail" : tear == Tear::IN_THE_TAIL ? "a torn record" : "a torn first record";
        InMemoryLogStorage storage;
        const InstrumentDirectory directory = twoShards();
        const std::vector<Placed> commands = commandsFor(2, 6);
        writeLog(storage, directory, 0, commands, false, fileBytesFor(directory, 0, 4));
        const std::string last = logFileName(0, 4);
        const std::uint64_t header = logFileShape(directory, 0, Durability::MACHINE, defaultLogFileBytes).headerBytes;
        expect(logNames(storage) == std::vector<std::string>{logFileName(0, 0), last, logFileName(0, 8)}, what + ": a full file, the last, and the next");
        const std::vector<std::byte> torn(20, std::byte{0xab});
        if (tear == Tear::IN_THE_TAIL) {
            storage.open(last)->write(header + 2 * replay::logRecordBytes, torn);
        } else if (tear == Tear::IN_THE_NEXT_FILE) {
            storage.open(logFileName(0, 8))->write(header, torn);
        }

        Shard shard(directory, 0);
        const RecoveredShard recovered = recover(storage, directory, 0, shard);
        expect(recovered.replayed == 6, what + ": every whole record replays");
        expect(recovered.filesRemoved == 1 && logNames(storage) == std::vector<std::string>{logFileName(0, 0), last},
               what + ": the next file, with no records, is removed");
        // Only what a write reached counts: a zero tail alone drops nothing anyone wrote.
        const std::uint64_t dropped = tear == Tear::NONE ? 0 : torn.size();
        expect(recovered.tailCutBytes == dropped, what + ": " + std::to_string(dropped) + " written bytes dropped, not " +
                                                      std::to_string(recovered.tailCutBytes));
        expect(bytesOf(storage, last).size() == header + 2 * replay::logRecordBytes, what + ": the last file ends on its last record");
        expectBooksEqual(shard, witnessOf(2, commands), what);

        Shard again(directory, 0);
        const RecoveredShard second = recover(storage, directory, 0, again);
        expect(second.replayed == 6 && second.tailCutBytes == 0 && second.filesRemoved == 0, what + ": recovering again cuts nothing");
    }
}

// A shard whose every file is empty, as a run that took no command leaves it, starts over at 0.
void testOnlyEmptyFiles() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    writeLog(storage, directory, 1, {}, false);
    expect(logNames(storage).size() == 2, "a crash before any command leaves the first file and the next");
    Shard shard(directory, 1);
    const RecoveredShard recovered = recover(storage, directory, 1, shard);
    expect(recovered.replayed == 0 && recovered.filesRemoved == 2 && logNames(storage).empty(), "both go, and the shard starts at 0");
}

void testWhatStopsAShard() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 10);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, 4);
    const std::uint64_t header = logFileShape(directory, 0, Durability::MACHINE, defaultLogFileBytes).headerBytes;
    {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        // Cut short and extended again, as no log writer leaves a file it moved past.
        const std::unique_ptr<LogFile> first = storage.open(logFileName(0, 0));
        first->cut(header + 2 * replay::logRecordBytes);
        first->extend(header + 4 * replay::logRecordBytes);
        Shard shard(directory, 0);
        expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 2 file log-0000-00000000000000000000.oblog: TORN_TAIL: the log ends in a torn tail: nothing valid "
                    "follows, in a file before the last, which its log writer flushed whole",
                    "a torn tail in an earlier file");
    }
    {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        storage.remove(logFileName(0, 4));
        Shard shard(directory, 0);
        expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 4 file log-0000-00000000000000000008.oblog: a gap: the file before it ends at position 4, and this "
                    "one starts at 8",
                    "a missing file between two");
    }
    {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        storage.remove(logFileName(0, 0));
        Shard shard(directory, 0);
        expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 0 file log-0000-00000000000000000004.oblog: a gap: the oldest book's checkpoint stands at position 0, "
                    "and the log starts at 4",
                    "a missing first file, which the books' checkpoints at 0 still need");
    }
    {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        std::vector<std::byte> bytes = bytesOf(storage, logFileName(0, 0));
        bytes[9] ^= std::byte{1};
        putFile(storage, logFileName(0, 0), bytes);
        expectError([&] { surveyLog(storage, directory); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 0 file log-0000-00000000000000000000.oblog: CORRUPT_HEADER: the header's checksum does not match",
                    "a damaged header");
    }
    {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        putFile(storage, logFileName(0, 9), bytesOf(storage, logFileName(0, 4)));
        storage.remove(logFileName(0, 4));
        expectError([&] { surveyLog(storage, directory); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 9 file log-0000-00000000000000000009.oblog: its header says shard 0 position 4",
                    "a file under another file's name");
    }
}

std::uint64_t headerBytesOf(const InstrumentDirectory& directory, std::uint32_t shard) {
    return logFileShape(directory, shard, Durability::MACHINE, defaultLogFileBytes).headerBytes;
}

// A header's checksum made right again after an edit, so only the edit is found.
void resealHeader(std::vector<std::byte>& bytes, std::uint64_t headerBytes) {
    const std::size_t at = static_cast<std::size_t>(headerBytes) - sizeof(std::uint64_t);
    std::uint64_t checksum = replay::xxh64(std::span<const std::byte>(bytes).first(at));
    for (std::size_t i = 0; i < sizeof(checksum); ++i, checksum >>= 8) {
        bytes[at + i] = static_cast<std::byte>(checksum & 0xFF);
    }
}

// The same for the record at offset, which chains from the one before it.
void resealRecord(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t previous) {
    auto checksum = static_cast<std::uint32_t>(replay::xxh64(std::span<const std::byte>(bytes).subspan(at + 4, replay::logRecordBytes - 4), previous) &
                                               0xFFFFFFFFULL);
    for (std::size_t i = 0; i < 4; ++i, checksum >>= 8) {
        bytes[at + i] = static_cast<std::byte>(checksum & 0xFF);
    }
}

std::uint32_t storedChecksum(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 4; i-- > 0;) {
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at + i]);
    }
    return value;
}

// The rest of the findings table, each stop with its exact message: header findings stop the
// survey, and a record's own stop its shard.
void testEveryOtherFindingStops() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 10);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, 4);
    const std::uint64_t header = headerBytesOf(directory, 0);
    const std::string first = logFileName(0, 0);
    const std::string where = "shard 0 position 0 file " + first + ": ";
    struct HeaderCase {
        std::string what;
        std::function<void(std::vector<std::byte>&)> edit;
        std::string message;
    };
    const std::vector<HeaderCase> headers{
        {"a newer reader version", [](std::vector<std::byte>& bytes) { bytes[12] = std::byte{2}; },
         "UNSUPPORTED_VERSION: this log needs reader version 2, and this reader is version 1"},
        {"a field this reader doesn't know",
         [header](std::vector<std::byte>& bytes) {
             bytes[30] = std::byte{1};
             resealHeader(bytes, header);
         },
         "UNKNOWN_FIELD: the header uses a field this reader does not know"},
        {"a header naming no symbol",
         [header](std::vector<std::byte>& bytes) {
             bytes[replay::logHeaderBytes] = std::byte{'a'};
             resealHeader(bytes, header);
         },
         "INVALID_SYMBOL: the header names something that is not a symbol"},
    };
    for (const HeaderCase& found : headers) {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        std::vector<std::byte> bytes = bytesOf(storage, first);
        found.edit(bytes);
        putFile(storage, first, bytes);
        expectError([&] { surveyLog(storage, directory); }, RecoveryError::Kind::RECOVERY_STOPPED, where + found.message, found.what);
    }

    // The second record of the first file, damaged but chained, so its fields are what's found.
    struct RecordCase {
        std::string what;
        std::size_t byte;
        std::byte value;
        std::string message;
    };
    const std::vector<RecordCase> records{
        {"a record's field this reader doesn't know", 38, std::byte{1}, "UNKNOWN_FIELD: record uses a field this reader does not know"},
        {"a record naming no symbol", 8, std::byte{'a'}, "INVALID_SYMBOL: not a symbol (1 to 8 of A-Z, 0-9, '.', '-')"},
        {"a record of no command", 4, std::byte{9}, "UNKNOWN_COMMAND: not a command"},
    };
    for (const RecordCase& found : records) {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, true, fileBytes);
        std::vector<std::byte> bytes = bytesOf(storage, first);
        const auto at = static_cast<std::size_t>(header + replay::logRecordBytes);
        bytes[at + found.byte] = found.value;
        resealRecord(bytes, at, storedChecksum(bytes, at - replay::logRecordBytes));
        putFile(storage, first, bytes);
        Shard shard(directory, 0);
        expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 1 file " + first + ": " + found.message, found.what);
    }

    // A damaged record that valid records follow, in a file its log writer flushed whole.
    InMemoryLogStorage storage;
    writeLog(storage, directory, 0, commands, true, fileBytes);
    std::vector<std::byte> bytes = bytesOf(storage, first);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(header + replay::logRecordBytes), replay::logRecordBytes, std::byte{0});
    putFile(storage, first, bytes);
    Shard shard(directory, 0);
    expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                "shard 0 position 1 file " + first +
                    ": CORRUPT_RECORD: a record is damaged, and valid records follow it, in a file before the last, which its log writer "
                    "flushed whole",
                "a hole in an earlier file");
}

// A hole in the last file with records, which valid records follow, is a write torn as a machine
// crash met it, if it lies within the 1 MiB one write reaches: it is cut there, valid records
// after it and all. Further back it stops. One write is at most 26,214 records.
void testAHoleNearTheEndIsCut() {
    const InstrumentDirectory directory = twoShards();
    constexpr std::size_t total = 26300;
    const std::vector<Placed> commands = commandsFor(2, total);
    const std::uint64_t header = headerBytesOf(directory, 0);
    const std::string last = logFileName(0, 0);
    // The last, a hole at the first record, which the file's zero first record mustn't hide.
    for (const std::size_t fromTheHole : {std::size_t{26214}, std::size_t{26215}, total}) {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, false, fileBytesFor(directory, 0, total + 10));
        const std::size_t hole = total - fromTheHole;
        const std::uint64_t holeAt = header + hole * replay::logRecordBytes;
        storage.open(last)->write(holeAt, std::vector<std::byte>(replay::logRecordBytes, std::byte{0}));
        Shard shard(directory, 0);
        if (fromTheHole != 26214) {
            expectError([&] { recover(storage, directory, 0, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                        "shard 0 position " + std::to_string(hole) + " file " + last +
                            ": CORRUPT_RECORD: a record is damaged, and valid records follow it, with " +
                            std::to_string(fromTheHole * replay::logRecordBytes) +
                            " bytes of records from it to the end of what was written, more than one write's 1048576",
                        "a hole further back than a write reaches, " + std::to_string(fromTheHole) + " records from the end");
            continue;
        }
        const RecoveredShard recovered = recover(storage, directory, 0, shard);
        expect(recovered.replayed == hole, "a hole 26,214 records from the end is cut there, so 86 replay, not " +
                                               std::to_string(recovered.replayed));
        expect(recovered.tailCutBytes > (fromTheHole - 2) * replay::logRecordBytes && recovered.tailCutBytes <= fromTheHole * replay::logRecordBytes,
               "counting the written bytes dropped, the valid records after it included: " + std::to_string(recovered.tailCutBytes));
        expect(bytesOf(storage, last).size() == holeAt, "the file ends where the hole began");
        expectBooksEqual(shard, witnessOf(2, std::vector<Placed>(commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(hole))),
                         "a cut hole");
    }

    // At a last file's first record, nothing is left in it, so it goes, and the next file can be
    // made where it began.
    InMemoryLogStorage storage;
    const std::vector<Placed> few = commandsFor(2, 11);
    writeLog(storage, directory, 0, few, false, fileBytesFor(directory, 0, 4));
    storage.open(logFileName(0, 8))->write(header, std::vector<std::byte>(replay::logRecordBytes, std::byte{0}));
    Shard shard(directory, 0);
    const RecoveredShard recovered = recover(storage, directory, 0, shard);
    expect(recovered.replayed == 8 && logNames(storage) == std::vector<std::string>{logFileName(0, 0), logFileName(0, 4)},
           "a hole at the first record leaves the file nothing, so it is removed with the next file made ahead");
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(directory, 0, 4));
    expect(housekeeping.first(0, recovered.replayed).name == logFileName(0, 8), "and a new one starts where it began");
}

std::vector<Placed> firstOf(const std::vector<Placed>& commands, std::uint64_t count) {
    return std::vector<Placed>(commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(count));
}

// A machine crash at process durability, on a file of three pages never flushed past its header,
// can lose or tear one page and keep a later one: a hole that valid records follow, which
// recovery cuts, dropping more than a torn record's bytes. Every seed recovers to the witness.
void testAMachineCrashCanLeaveAHole() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 250);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, 300);
    std::size_t holes = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, commands, false, fileBytes, Durability::PROCESS);
        storage.crash(InMemoryLogStorage::Crash::MACHINE, seed);
        Shard shard(directory, 0);
        const RecoveredShard recovered = recover(storage, directory, 0, shard);
        expectBooksEqual(shard, witnessOf(2, firstOf(commands, recovered.replayed)), "seed " + std::to_string(seed));
        holes += recovered.tailCutBytes > replay::logRecordBytes ? 1 : 0;
    }
    expect(holes > 0, "over 20 seeds, some machine crash left a hole that recovery cut");
}

// What the drive held after one change, and how far responses had been released by then.
struct Step {
    InMemoryLogStorage::Contents drive;
    std::uint64_t released = 0;
};

// A crash after one step, then a start on what it left: the books equal the witness at the
// surviving position, which holds every released response's command at machine durability, and
// at process durability for an engine crash. The log then goes on from there with the rest, as
// a client would send them again, stops cleanly, and recovers to the uninterrupted run's books.
// Returns the written bytes the first recovery cut.
std::uint64_t restartAfter(const Step& step, InMemoryLogStorage::Crash kind, std::uint64_t seed, Durability durability,
                           const InstrumentDirectory& directory, const std::vector<Placed>& commands, std::uint64_t fileBytes,
                           const std::string& what) {
    InMemoryLogStorage storage(step.drive);
    storage.crash(kind, seed);
    const std::unique_ptr<StorageLock> held = storage.lock();
    const LogSurvey survey = surveyLog(storage, directory);
    for (const std::string& temporary : survey.temporaries) {
        storage.remove(temporary);
    }
    for (const std::string& temporary : survey.checkpointTemporaries) {
        storage.remove(temporary);
    }
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
    const std::uint64_t survived = recovered.replayed;
    expect(survived <= commands.size(), what + ": no more survives than was written");
    if (durability == Durability::MACHINE || kind == InMemoryLogStorage::Crash::ENGINE) {
        expect(survived >= step.released, what + ": every released response's command survives: " + std::to_string(survived) + " of " +
                                              std::to_string(step.released) + " released");
    }
    expectBooksEqual(shard, witnessOf(2, firstOf(commands, survived)), what + ", at the surviving position " + std::to_string(survived));

    writeLog(storage, directory, 0, std::vector<Placed>(commands.begin() + static_cast<std::ptrdiff_t>(survived), commands.end()), true,
             fileBytes, durability, survived);
    Shard again(directory, 0);
    const RecoveredShard whole = recover(storage, directory, 0, again);
    expect(whole.replayed == commands.size() && whole.tailCutBytes == 0, what + ": after the restart, the whole log recovers");
    expectBooksEqual(again, witnessOf(2, commands), what + ", against the uninterrupted run");
    return recovered.tailCutBytes;
}

// Every step, each crash: an engine crash, and machine crashes of several seeds. At process
// durability the machine crashes must have torn records somewhere, or the cut went untried.
void crashAtEachStep(const std::vector<Step>& steps, Durability durability, const InstrumentDirectory& directory,
                     const std::vector<Placed>& commands, std::uint64_t fileBytes, const std::string& what) {
    expect(steps.size() > 20, what + ": many steps to crash at");
    std::uint64_t torn = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const std::string at = what + " at " + (durability == Durability::MACHINE ? "machine" : "process") + ", step " + std::to_string(i);
        restartAfter(steps[i], InMemoryLogStorage::Crash::ENGINE, 0, durability, directory, commands, fileBytes, at + ", an engine crash");
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            torn += restartAfter(steps[i], InMemoryLogStorage::Crash::MACHINE, seed, durability, directory, commands, fileBytes,
                                 at + ", a machine crash of seed " + std::to_string(seed));
        }
    }
    if (durability == Durability::PROCESS) {
        expect(torn > 0, what + ": some machine crash tore a record");
    }
}

// Writes commands from first on, a few at a time, then stops cleanly: the log writer and
// housekeeping as the engine runs them, so each write, flush, and file made is a step.
void writeInSteps(LogStorage& storage, const InstrumentDirectory& directory, Durability durability, std::uint64_t fileBytes,
                  const std::vector<Placed>& commands, std::size_t first, DurablePosition& durable) {
    constexpr std::size_t perWrite = 7;
    publishEmpties(storage, directory, 0, first);
    Housekeeping housekeeping(storage, directory, durability, fileBytes);
    Ring<LogEntry> ring(16);
    LogWriter writer(housekeeping, 0, ring, durable, first);
    const auto drain = [&] {
        while (writer.pass() != 0 || housekeeping.pass() != 0) {
        }
    };
    for (std::size_t i = first; i < commands.size(); ++i) {
        expect(ring.tryPush(LogEntry{commands[i].command, commands[i].slot, ControlKind::COMMAND}), "room for a write's worth");
        if ((i - first) % perWrite == perWrite - 1) {
            drain();
        }
    }
    expect(ring.tryPush(LogEntry::stop()), "room for the stop");
    drain();
    housekeeping.removeUnused();
    expect(durable.load() == commands.size() && writer.stopped(), "the run wrote everything and stopped");
}

// Files of thirty records, so each spans a few of a drive's sectors and a machine crash can tear
// a record.
constexpr std::uint64_t recordsPerStepFile = 30;

// Rotation: writes of seven commands across files made ahead under a temporary name, renamed,
// and taken when the last is full and flushed, then a clean stop's cut and the removal of the
// unused next file.
void testACrashAtEveryStepOfRotation() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 70);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, recordsPerStepFile);
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        InMemoryLogStorage storage;
        DurablePosition durable;
        std::vector<Step> steps;
        storage.watch([&](const InMemoryLogStorage::Contents& drive) { steps.push_back(Step{drive, durable.load()}); });
        writeInSteps(storage, directory, durability, fileBytes, commands, 0, durable);
        crashAtEachStep(steps, durability, directory, commands, fileBytes, "rotation");
    }
}

// Start: an earlier run's crash left a torn tail, the next file made ahead, and a file being
// made; this start removes the file being made, recovers, cuts and flushes the tail, removes
// the empty file, makes its first file and the next, then writes the rest and stops cleanly.
void testACrashAtEveryStepOfAStart() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 70);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, recordsPerStepFile);
    constexpr std::uint64_t before = 40;
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        InMemoryLogStorage storage;
        writeLog(storage, directory, 0, firstOf(commands, before), false, fileBytes, durability);
        const std::uint64_t header = headerBytesOf(directory, 0);
        storage.open(logFileName(0, 30))->write(header + 10 * replay::logRecordBytes, std::vector<std::byte>(20, std::byte{0xab}));
        putFile(storage, logFileName(0, 60) + ".tmp", {std::byte{1}});
        expect(logNames(storage).size() == 4, "two files with records, the next one made ahead, and one being made");

        DurablePosition durable;
        std::vector<Step> steps;
        storage.watch([&](const InMemoryLogStorage::Contents& drive) { steps.push_back(Step{drive, std::max(before, durable.load())}); });
        {
            const std::unique_ptr<StorageLock> held = storage.lock();
            const LogSurvey survey = surveyLog(storage, directory);
            for (const std::string& temporary : survey.temporaries) {
                storage.remove(temporary);
            }
            Shard shard(directory, 0);
            const RecoveredShard recovered = recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
            expect(recovered.replayed == before && recovered.filesRemoved == 1 && recovered.tailCutBytes == 20,
                   "the start recovers the earlier run's forty, cutting its torn tail");
        }
        writeInSteps(storage, directory, durability, fileBytes, commands, before, durable);
        crashAtEachStep(steps, durability, directory, commands, fileBytes, "a start");
    }
}

// The page store, as the browser keeps it (ADR 0017): the log writer and housekeeping over a
// PageLogStorage at process durability, each change made in the page's own copy as it is told.
// A crash in the browser is the page ending the engine worker, which loses nothing the store was
// told, so after every change a store built from the copy recovers to the witness at what it
// holds, every released response's command among it, then goes on with the rest, as a client
// would send them again, and recovers to the uninterrupted run's books. At the end the copy holds
// exactly the store's files.
std::vector<PageLogStorage::StoredFile> storedFiles(const std::map<std::string, std::vector<std::byte>>& copy) {
    std::vector<PageLogStorage::StoredFile> files;
    for (const auto& [name, bytes] : copy) {
        files.push_back({name, bytes});
    }
    return files;
}

void testAPageStoreRecoversAtEveryStep() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = commandsFor(2, 70);
    const std::uint64_t fileBytes = fileBytesFor(directory, 0, recordsPerStepFile);
    struct PageStep {
        std::vector<PageLogStorage::StoredFile> copy;
        std::uint64_t released = 0;
    };
    std::map<std::string, std::vector<std::byte>> copy;
    DurablePosition durable;
    std::vector<PageStep> steps;
    PageLogStorage storage([&](const PageLogStorage::Change& change) {
        PageLogStorage::apply(copy, change);
        steps.push_back(PageStep{storedFiles(copy), durable.load()});
    });
    writeInSteps(storage, directory, Durability::PROCESS, fileBytes, commands, 0, durable);
    expect(storedFiles(copy) == storage.files(), "the page's copy holds exactly the store's files");
    expect(steps.size() > 20, "the page store: many steps to crash at");

    for (std::size_t i = 0; i < steps.size(); ++i) {
        const std::string what = "the page store, step " + std::to_string(i);
        PageLogStorage rebuilt([](const PageLogStorage::Change&) {}, steps[i].copy);
        expect(rebuilt.files() == steps[i].copy, what + ": a store built from the copy holds its files");
        const std::unique_ptr<StorageLock> held = rebuilt.lock();
        const LogSurvey survey = surveyLog(rebuilt, directory);
        for (const std::string& temporary : survey.temporaries) {
            rebuilt.remove(temporary);
        }
        for (const std::string& temporary : survey.checkpointTemporaries) {
            rebuilt.remove(temporary);
        }
        Shard shard(directory, 0);
        const RecoveredShard recovered =
            recoverShard(rebuilt, rebuilt, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
        const std::uint64_t survived = recovered.replayed;
        expect(survived <= commands.size() && survived >= steps[i].released,
               what + ": every released response's command survives: " + std::to_string(survived) + " of " +
                   std::to_string(steps[i].released) + " released");
        expectBooksEqual(shard, witnessOf(2, firstOf(commands, survived)), what + ", at the surviving position " + std::to_string(survived));

        writeLog(rebuilt, directory, 0, std::vector<Placed>(commands.begin() + static_cast<std::ptrdiff_t>(survived), commands.end()), true,
                 fileBytes, Durability::PROCESS, survived);
        Shard again(directory, 0);
        const RecoveredShard whole = recover(rebuilt, directory, 0, again);
        expect(whole.replayed == commands.size() && whole.tailCutBytes == 0, what + ": after the restart, the whole log recovers");
        expectBooksEqual(again, witnessOf(2, commands), what + ", against the uninterrupted run");
    }
}

// The start's own checks, on the main thread: a symbol off the list, a changed shard count, and
// a moved symbol, refused since every book's checkpoint stands at 0, short of its log's end; a
// new instrument is none of them.
void testWhatRefusesAStart() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    writeLog(storage, directory, 0, commandsFor(2, 4), true);
    writeLog(storage, directory, 1, commandsFor(1, 3), true);

    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0)}, 2)); }, RecoveryError::Kind::START_REFUSED,
                "log-0001-00000000000000000000.oblog names IBM, which is not on the instrument list", "a symbol off the list");
    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1)}, 3)); },
                RecoveryError::Kind::PLACEMENT_CHANGED,
                "the log was written by 2 shards and this start has 3 and the last run did not stop cleanly; restart on the old placement "
                "and stop cleanly first",
                "a changed shard count");
    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 1), on("IBM", 1)}, 2)); },
                RecoveryError::Kind::PLACEMENT_CHANGED,
                "MSFT moved from shard 0 to shard 1 and the last run did not stop cleanly; restart on the old placement and stop cleanly "
                "first",
                "a moved symbol");
    const LogSurvey added = surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1), on("NVDA", 1)}, 2));
    expect(added.shards[1].size() == 1, "a new instrument moves nothing, so the start goes on");

    // Names: a file being made is a temporary, a cut file and anything else are not the log's,
    // and a name only like a log file's refuses the start.
    for (const std::string& other : {std::string("engine.lock"), std::string("cut-0000-00000000000000000001.oblog"), std::string("notes.txt"),
                                     logFileName(0, 4) + ".tmp"}) {
        putFile(storage, other, {std::byte{1}});
    }
    const LogSurvey named = surveyLog(storage, directory);
    expect(named.temporaries == std::vector<std::string>{logFileName(0, 4) + ".tmp"}, "a file left being made is a temporary");
    expect(named.shards[0].size() == 1 && named.shards[1].size() == 1, "and nothing else is the log's");
    putFile(storage, "log-1.oblog", {std::byte{1}});
    expectError([&] { surveyLog(storage, directory); }, RecoveryError::Kind::START_REFUSED,
                "log-1.oblog is named like a log file, but not as an engine names one", "a stray name");
}

// Each book warms to the larger of what it declares and 1.5 times what it holds, each apart.
void testWarming() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory({on("AAPL", 0, 4, 1, 2), on("MSFT", 0), on("IBM", 1)}, 2);
    std::vector<Placed> commands;
    for (OrderId id = 1; id <= 10; ++id) {
        replay::AddCommand add{};
        add.orderId = id;
        add.side = Side::BUY;
        add.price = static_cast<Price>(100 + id);
        add.quantity = 1;
        commands.push_back(Placed{0, add});
    }
    writeLog(storage, directory, 0, commands, true);
    Shard shard(directory, 0);
    const RecoveredShard recovered = recover(storage, directory, 0, shard);
    expect(recovered.warmed.size() == 2, "a capacity for each book");
    const BookCapacity aapl = recovered.warmed[0];
    expect(aapl.orders == 15 && aapl.stops == 1 && aapl.levels == 15,
           "AAPL holds 10 orders on 10 levels, so 15 of each, and keeps the 1 stop it declares, not " + std::to_string(aapl.orders) +
               ", " + std::to_string(aapl.stops) + ", " + std::to_string(aapl.levels));
    const BookCapacity msft = recovered.warmed[1];
    expect(msft.orders == defaultBookCapacity.orders && msft.stops == defaultBookCapacity.stops &&
               msft.levels == defaultBookCapacity.levels,
           "MSFT holds nothing, so what it declares");
    const std::uint64_t growths = shard.books().book(0).capacityGrowths();
    replay::AddCommand more{};
    more.orderId = 11;
    more.side = Side::BUY;
    more.price = 1;
    more.quantity = 1;
    shard.process(more, 0);
    expect(shard.books().book(0).capacityGrowths() == growths, "so a book warmed past what it holds takes more without growing");
}

void testAStopInterrupts() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    writeLog(storage, directory, 0, commandsFor(2, 10), true);
    const std::atomic<bool> stopping{true};
    Shard shard(directory, 0);
    bool interrupted = false;
    try {
        recover(storage, directory, 0, shard, stopping);
    } catch (const RecoveryInterrupted&) {
        interrupted = true;
    }
    expect(interrupted, "a stop during replay ends the recovery");
}

}  // namespace


// ---- Recovery from checkpoints (D5) ---------------------------------------------------------

// Shard 0's log of thirty commands in files of eight, left running, then rolled: each book keeps
// checkpoints at 16 and 24, and the file at 24, still being written, holds the last six.
// Each book trades within itself, buys and sells alternating on it, so a record run twice where it
// should be skipped re-adds an order that had traded away, which the image shows.
std::vector<Placed> tradingCommandsFor(std::size_t count) {
    std::vector<Placed> placed;
    for (std::size_t i = 1; i <= count; ++i) {
        const auto slot = static_cast<std::uint32_t>(i % 2);
        if (i % 5 == 0) {
            placed.push_back(Placed{slot, replay::CancelCommand{static_cast<OrderId>(i - 2)}});
            continue;
        }
        replay::AddCommand add{};
        add.orderId = static_cast<OrderId>(i);
        add.side = (i / 2) % 2 == 0 ? Side::BUY : Side::SELL;
        add.price = static_cast<Price>(100 + (i * 7) % 3);
        add.quantity = static_cast<Quantity>(1 + i % 3);
        placed.push_back(Placed{slot, add});
    }
    return placed;
}

struct Checkpointed {
    InMemoryLogStorage storage;
    std::vector<Placed> commands = tradingCommandsFor(30);
};

void checkpointed(Checkpointed& store, const InstrumentDirectory& directory) {
    writeLog(store.storage, directory, 0, store.commands, false, fileBytesFor(directory, 0, 8));
    ShardRoller roller(store.storage, directory, 0, [](const std::string&) {});
    expect(roller.roll(store.commands.size()) == 3 && roller.position(0) == 24 && roller.position(1) == 24,
           "the three closed files rolled: each book at 24");
}

void damage(InMemoryLogStorage& storage, const std::string& name) {
    std::vector<std::byte> bytes = bytesOf(storage, name);
    bytes[bytes.size() / 2] ^= std::byte{1};
    putFile(storage, name, bytes);
}

// Byte for byte, so a command run twice shows even where the book refused it, in the sequence
// numbers and trade IDs the image holds.
void expectImagesEqual(const Shard& shard, const std::vector<OrderBook>& witness, const std::string& what) {
    for (std::uint32_t slot = 0; slot < witness.size(); ++slot) {
        const OrderBook& book = shard.books().book(slot);
        std::vector<std::byte> mine(book.imageSize());
        std::vector<std::byte> theirs(witness[slot].imageSize());
        static_cast<void>(book.writeImage(mine));
        static_cast<void>(witness[slot].writeImage(theirs));
        expect(mine == theirs, what + ": slot " + std::to_string(slot) + "'s image is the witness's");
    }
}

RecoveredShard recoverInto(LogStorage& storage, const InstrumentDirectory& directory, Shard& shard) {
    const LogSurvey survey = surveyLog(storage, directory);
    return recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
}

void testBooksRestoreFromCheckpoints() {
    const InstrumentDirectory directory = twoShards();
    Checkpointed store;
    checkpointed(store, directory);
    const LogSurvey survey = surveyLog(store.storage, directory);
    expect(survey.checkpoints[0][0] == std::vector<std::uint64_t>{24, 16} && survey.checkpoints[0][1] == std::vector<std::uint64_t>{24, 16},
           "the survey finds each book's checkpoints, newest first");
    expect(survey.newBooks[0].empty() && survey.newBooks[1] == std::vector<std::uint32_t>{0}, "and IBM, named nowhere, new");
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverInto(store.storage, directory, shard);
    expect(recovered.start == 24 && recovered.restoredFrom == std::vector<std::uint64_t>{24, 24} && recovered.fellBack == 0,
           "each book from its newest, so the replay starts at 24");
    expect(recovered.replayed == 30 && shard.commandsTaken() == 6, "and takes the last six");
    expectBooksEqual(shard, witnessOf(2, store.commands), "restored and replayed");
    expectImagesEqual(shard, witnessOf(2, store.commands), "restored and replayed");
    expect(!shard.checkInvariants(), "the shard is sound");

    // A missing file before the oldest book's checkpoint is not needed.
    store.storage.remove(logFileName(0, 0));
    Shard without(directory, 0);
    expectBooksEqual(without, witnessOf(2, {}), "a new shard holds nothing");
    recoverInto(store.storage, directory, without);
    expectBooksEqual(without, witnessOf(2, store.commands), "a file no book needs, gone, changes nothing");
}

// The fallback DAMAGE gives: AAPL's newest doesn't decode, so it restores from 16, and the shard
// replays from there, skipping MSFT's records before its 24.
void testBooksOnOneShardAtDifferentPositions() {
    const InstrumentDirectory directory = twoShards();
    Checkpointed store;
    checkpointed(store, directory);
    damage(store.storage, checkpointFileName(*Symbol::parse("AAPL"), 24));
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverInto(store.storage, directory, shard);
    expect(recovered.restoredFrom == std::vector<std::uint64_t>{16, 24} && recovered.start == 16 && recovered.fellBack == 1,
           "AAPL from its older checkpoint, MSFT from its newest");
    expect(recovered.replayed == 30 && shard.commandsTaken() == 14, "the replay takes all fourteen from 16, running some");
    expectBooksEqual(shard, witnessOf(2, store.commands), "books at different positions recover");
    expectImagesEqual(shard, witnessOf(2, store.commands), "books at different positions recover");
    expect(!shard.checkInvariants(), "and the shard is sound");

    // With the file at 16 gone, the fallback needs it: a gap.
    store.storage.remove(logFileName(0, 16));
    Shard gap(directory, 0);
    expectError([&] { recoverInto(store.storage, directory, gap); }, RecoveryError::Kind::RECOVERY_STOPPED,
                "shard 0 position 16 file log-0000-00000000000000000024.oblog: a gap: the file before it ends at position 16, and this one "
                "starts at 24",
                "a file the oldest book needs, gone");
}

void testWhatCheckpointsStop() {
    const InstrumentDirectory directory = twoShards();
    {
        Checkpointed store;
        checkpointed(store, directory);
        store.storage.remove(checkpointFileName(*Symbol::parse("MSFT"), 16));
        store.storage.remove(checkpointFileName(*Symbol::parse("MSFT"), 24));
        expectError([&] { surveyLog(store.storage, directory); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 symbol MSFT: a log file's header names it, and no checkpoint holds it: a lost checkpoint", "a lost checkpoint");
    }
    {
        Checkpointed store;
        checkpointed(store, directory);
        publishEmptyCheckpoint(store.storage, *Symbol::parse("ZZZ"), 2, 0, 0);
        expectError([&] { surveyLog(store.storage, directory); }, RecoveryError::Kind::START_REFUSED,
                    "ckpt-ZZZ-00000000000000000000.obckpt names ZZZ, which is not on the instrument list", "a checkpoint off the list");
    }
    {
        Checkpointed store;
        checkpointed(store, directory);
        publishEmptyCheckpoint(store.storage, *Symbol::parse("AAPL"), 2, 0, 40);
        Shard shard(directory, 0);
        expectError([&] { recoverInto(store.storage, directory, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 40 symbol AAPL: its checkpoint stands past the log's end, 30: the disk lost data it had confirmed",
                    "a checkpoint past the log's end");
        // Even one that doesn't decode: its name says the disk had the log that far.
        damage(store.storage, checkpointFileName(*Symbol::parse("AAPL"), 40));
        Shard damaged(directory, 0);
        expectError([&] { recoverInto(store.storage, directory, damaged); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 40 symbol AAPL: its checkpoint stands past the log's end, 30: the disk lost data it had confirmed",
                    "a damaged checkpoint past the log's end");
    }
    {
        // Only an empty file left, starting past the books: the log between is gone.
        Checkpointed store;
        checkpointed(store, directory);
        for (const std::string& name : logNames(store.storage)) {
            store.storage.remove(name);
        }
        Housekeeping housekeeping(store.storage, directory, Durability::MACHINE, fileBytesFor(directory, 0, 8));
        static_cast<void>(housekeeping.first(0, 100));
        Shard shard(directory, 0);
        expectError([&] { recoverInto(store.storage, directory, shard); }, RecoveryError::Kind::RECOVERY_STOPPED,
                    "shard 0 position 24 file log-0000-00000000000000000100.oblog: a gap: the oldest book's checkpoint stands at position 24, "
                    "and the log starts at 100",
                    "an empty file past the books, and nothing between");
    }
    {
        Checkpointed store;
        checkpointed(store, directory);
        damage(store.storage, checkpointFileName(*Symbol::parse("AAPL"), 24));
        damage(store.storage, checkpointFileName(*Symbol::parse("AAPL"), 16));
        Shard shard(directory, 0);
        const RecoveryError error = errorFrom([&] { recoverInto(store.storage, directory, shard); }, "no checkpoint restores");
        const std::string text = error.what();
        expect(error.kind() == RecoveryError::Kind::RECOVERY_STOPPED &&
                   text.starts_with("shard 0 symbol AAPL: no checkpoint restores: ckpt-AAPL-00000000000000000024.obckpt: CHECKSUM_MISMATCH at ") &&
                   text.find("; ckpt-AAPL-00000000000000000016.obckpt: CHECKSUM_MISMATCH at ") != std::string::npos,
               "neither checkpoint restores, and each says why: " + text);
    }
    {
        Checkpointed store;
        checkpointed(store, directory);
        const std::string name = checkpointFileName(*Symbol::parse("AAPL"), 24);
        const std::vector<std::byte> bytes = bytesOf(store.storage, name);
        const auto decoded = std::get<DecodedCheckpoint>(decodeCheckpoint(bytes));
        CheckpointHeader moved = decoded.header;
        moved.shardCount = 3;
        putFile(store.storage, name, encodeCheckpoint(moved, decoded.image));
        Shard shard(directory, 0);
        expectError([&] { recoverInto(store.storage, directory, shard); }, RecoveryError::Kind::PLACEMENT_CHANGED,
                    "ckpt-AAPL-00000000000000000024.obckpt was written by shard 0 of 3, and this start places AAPL on shard 0 of 2, though "
                    "the logs' headers name this placement: a placement change was cut short, so start on the placement that checkpoint "
                    "names to finish it",
                    "a checkpoint under another placement");
    }
}

// An instrument added to the list, named in no header and with no checkpoint, is new: an empty
// book, which the log writer then gives a checkpoint.
void testANewInstrument() {
    const InstrumentDirectory directory = twoShards();
    Checkpointed store;
    checkpointed(store, directory);
    const InstrumentDirectory added({on("AAPL", 0), on("MSFT", 0), on("NEW", 0), on("IBM", 1)}, 2);
    const LogSurvey survey = surveyLog(store.storage, added);
    expect(survey.newBooks[0] == std::vector<std::uint32_t>{2} && survey.checkpoints[0][2].empty(), "NEW, in slot 2, is new");
    Shard shard(added, 0);
    const RecoveredShard recovered =
        recoverShard(store.storage, store.storage, added, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
    expect(recovered.restoredFrom == std::vector<std::uint64_t>{24, 24, 24} && shard.books().book(2).activeOrderCount() == 0,
           "an empty book, standing where the replay starts");
}

// Retention: housekeeping deletes each closed file that ends at or before the oldest checkpoint
// any book keeps, and a crash at every step of deleting them still recovers the whole run.
void testRetention() {
    const InstrumentDirectory directory = twoShards();
    {
        Checkpointed store;
        checkpointed(store, directory);
        ShardRoller roller(store.storage, directory, 0, [](const std::string&) {});
        expect(roller.retentionFloor() == 16, "each book's older checkpoint stands at 16");
        Housekeeping keeping(store.storage, directory, Durability::MACHINE, fileBytesFor(directory, 0, 8));
        keeping.keepEveryFile();
        keeping.retain(0, 16);
        expect(keeping.pass() == 0 && keeping.filesDeleted() == 0, "with --keep-log nothing goes");
    }
    Checkpointed store;
    checkpointed(store, directory);
    const InMemoryLogStorage::Contents before = store.storage.contents();
    std::vector<InMemoryLogStorage::Contents> steps;
    store.storage.watch([&steps](const InMemoryLogStorage::Contents& drive) { steps.push_back(drive); });
    Housekeeping housekeeping(store.storage, directory, Durability::MACHINE, fileBytesFor(directory, 0, 8));
    housekeeping.retain(0, 16);
    expect(housekeeping.pass() == 2 && housekeeping.filesDeleted() == 2, "the files at 0 and 8 go");
    store.storage.watch(nullptr);
    expect(logNames(store.storage) == std::vector<std::string>{logFileName(0, 16), logFileName(0, 24), logFileName(0, 32)},
           "the file the older checkpoints need stays, with the one being written and the one made ahead");
    housekeeping.retain(0, 8);
    expect(housekeeping.pass() == 0, "a lower floor changes nothing");
    expect(steps.size() == 2, "each deletion is a step");

    steps.insert(steps.begin(), before);

    // And a whole roll with its retention, crashed at every step: each checkpoint published, the
    // superseded one removed, and each log file deleted, interleaved as a worker and the
    // housekeeping thread leave them.
    {
        InMemoryLogStorage rolled;
        writeLog(rolled, directory, 0, store.commands, false, fileBytesFor(directory, 0, 8));
        rolled.watch([&steps](const InMemoryLogStorage::Contents& drive) { steps.push_back(drive); });
        ShardRoller roller(rolled, directory, 0, [](const std::string&) {});
        Housekeeping trimming(rolled, directory, Durability::MACHINE, fileBytesFor(directory, 0, 8));
        for (const std::uint64_t closedEnd : {8U, 16U, 24U, 30U}) {
            roller.roll(closedEnd);
            trimming.retain(0, *roller.retentionFloor());
            trimming.pass();
        }
        rolled.watch(nullptr);
        expect(trimming.filesDeleted() == 2 && roller.position(0) == 24, "rolled to 24, retention deleting the files at 0 and 8");
    }
    expect(steps.size() > 30, "the whole roll and its retention are many steps: " + std::to_string(steps.size()));
    for (std::size_t at = 0; at < steps.size(); ++at) {
        for (const auto& [kind, seed] : {std::pair{InMemoryLogStorage::Crash::ENGINE, 0ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 1ULL},
                                         std::pair{InMemoryLogStorage::Crash::MACHINE, 2ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 3ULL}}) {
            InMemoryLogStorage crashed(steps[at]);
            crashed.crash(kind, seed);
            Shard shard(directory, 0);
            recoverInto(crashed, directory, shard);
            expectBooksEqual(shard, witnessOf(2, store.commands), "a crash after deletion step " + std::to_string(at) + " recovers the whole run");
            // And the fallback still has what it needs, where AAPL has an older checkpoint to fall to.
            std::vector<std::uint64_t> aapl;
            for (const std::string& name : crashed.names()) {
                const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
                if (parts && !parts->temporary && parts->symbol == *Symbol::parse("AAPL")) {
                    aapl.push_back(parts->position);
                }
            }
            if (aapl.size() < 2) {
                continue;
            }
            damage(crashed, checkpointFileName(*Symbol::parse("AAPL"), *std::max_element(aapl.begin(), aapl.end())));
            Shard fallback(directory, 0);
            recoverInto(crashed, directory, fallback);
            expectImagesEqual(fallback, witnessOf(2, store.commands), "and so does AAPL's fallback to its older checkpoint");
        }
    }
}

// ---- Placement changes (D6b) ----------------------------------------------------------------

// AAPL and MSFT on shard 0 with four commands, IBM on shard 1 with three, each stopped cleanly:
// the log cut and closed, then the clean stop's last roll publishing every book at its log's end.
struct StoppedCleanly {
    InMemoryLogStorage storage;
    std::vector<Placed> zero = commandsFor(2, 4);
    std::vector<Placed> one = commandsFor(1, 3);
};

std::vector<std::byte> imageBytes(const OrderBook& book) {
    std::vector<std::byte> image(book.imageSize());
    static_cast<void>(book.writeImage(image));
    return image;
}

void stopCleanly(StoppedCleanly& store, const InstrumentDirectory& directory) {
    writeLog(store.storage, directory, 0, store.zero, true);
    writeLog(store.storage, directory, 1, store.one, true);
    for (const auto& [shard, commands, slots] : {std::tuple{0U, &store.zero, std::size_t{2}}, std::tuple{1U, &store.one, std::size_t{1}}}) {
        std::vector<std::vector<std::byte>> images;
        for (const OrderBook& book : witnessOf(slots, *commands)) {
            images.push_back(imageBytes(book));
        }
        ShardRoller roller(store.storage, directory, shard, [](const std::string&) {});
        roller.finish(commands->size(), images);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            expect(roller.stopped(slot).outcome == StopOutcome::PUBLISHED, "the clean stop publishes every book at its log's end");
        }
    }
}

// Each instrument's image, by symbol, as the clean stop left it: what any later start must hold.
std::map<std::string, std::vector<std::byte>> imagesBySymbol(const StoppedCleanly& store) {
    const std::vector<OrderBook> zero = witnessOf(2, store.zero);
    const std::vector<OrderBook> one = witnessOf(1, store.one);
    return {{"AAPL", imageBytes(zero[0])}, {"MSFT", imageBytes(zero[1])}, {"IBM", imageBytes(one[0])}};
}

// A start as the engine makes it: the survey, the change if any, the survey again, then every
// shard recovered, whose books must be the clean stop's.
std::vector<RecoveredShard> startOn(InMemoryLogStorage& storage, const InstrumentDirectory& directory,
                                    const std::map<std::string, std::vector<std::byte>>& expected, const std::string& what) {
    LogSurvey survey = surveyLog(storage, directory);
    for (const std::string& temporary : survey.checkpointTemporaries) {
        storage.remove(temporary);
    }
    if (survey.placement) {
        changePlacement(storage, *survey.placement);
        survey = surveyLog(storage, directory);
        expect(survey.placement && survey.placement->rewraps.empty() && survey.placement->dropped.empty(),
               what + ": the survey again finds the change made");
    }
    std::vector<RecoveredShard> recovered;
    for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
        Shard books(directory, shard);
        recovered.push_back(recoverShard(storage, storage, directory, survey.shards[shard], survey.checkpoints[shard], books,
                                         defaultBookCapacity, running));
        for (std::uint32_t slot = 0; slot < books.size(); ++slot) {
            const std::string symbol = books.books().symbol(slot).text();
            expect(imageBytes(books.books().book(slot)) == expected.at(symbol), what + ": " + symbol + " is the clean stop's book");
        }
        expect(!books.checkInvariants(), what + ": shard " + std::to_string(shard) + " is sound");
    }
    return recovered;
}

CheckpointHeader headerOf(InMemoryLogStorage& storage, const std::string& name) {
    const std::vector<std::byte> bytes = bytesOf(storage, name);
    return std::get<DecodedCheckpoint>(decodeCheckpoint(bytes)).header;
}

std::vector<std::string> checkpointsOf(const InMemoryLogStorage& storage) {
    std::vector<std::string> names = storage.names();
    std::erase_if(names, [](const std::string& name) { return !name.starts_with("ckpt-"); });
    return names;
}

// Log files of 64 records, so a restart's look at a trailing file reads a few KiB of zeros, not
// the default 256 MiB, which a sanitizer build crawls through at every step of a crash test.
std::uint64_t smallFiles(const InstrumentDirectory& directory) {
    return leastLogFileBytes(directory) + 63 * replay::logRecordBytes;
}

// After a clean stop every book stands at its log's end, where the next run's first file begins,
// so retention keeps the file before it: a crash that loses that first file's records, all of
// them or with a torn first record, as a machine crash at process durability can, must still find
// the log reaching the books, from the file that holds its last record.
void testRetentionKeepsTheLastRecord() {
    const InstrumentDirectory directory = twoShards();
    for (const bool torn : {true, false}) {
        StoppedCleanly store;
        stopCleanly(store, directory);
        // The next run's first file, at 4, and the one made ahead, retention at every book's 4.
        writeLog(store.storage, directory, 0, commandsFor(2, 3), false, smallFiles(directory), Durability::PROCESS, 4);
        Housekeeping trimming(store.storage, directory, Durability::PROCESS, smallFiles(directory));
        trimming.retain(0, 4);
        static_cast<void>(trimming.pass());
        expect(store.storage.file(logFileName(0, 0)).has_value() && trimming.filesDeleted() == 0,
               "the file before the writer's stays, holding the log's last record");
        const std::uint64_t header = headerBytesOf(directory, 0);
        const std::unique_ptr<LogFile> file = store.storage.open(logFileName(0, 4));
        file->write(header, std::vector<std::byte>(3 * replay::logRecordBytes, std::byte{0}));
        if (torn) {
            file->write(header, std::vector<std::byte>(7, std::byte{0x3c}));  // a first record torn as it was written
        }
        file->close();
        Shard shard(directory, 0);
        const RecoveredShard recovered = recoverInto(store.storage, directory, shard);
        expect(recovered.replayed == 4 && recovered.start == 4, "the log reaches the books, 4: " + std::to_string(recovered.replayed));
        const std::map<std::string, std::vector<std::byte>> expected = imagesBySymbol(store);
        expect(imageBytes(shard.books().book(0)) == expected.at("AAPL") && imageBytes(shard.books().book(1)) == expected.at("MSFT"),
               "and the books are the clean stop's");
    }
}

// The named case "placement change refused at start": one case per refusal, each exit 4 with
// #34's message.
void testAPlacementChangeRefused() {
    const InstrumentDirectory directory = twoShards();
    const InstrumentDirectory moved({on("AAPL", 0), on("MSFT", 1), on("IBM", 1)}, 2);
    const std::string refused = "MSFT moved from shard 0 to shard 1 and the last run did not stop cleanly; restart on the old placement "
                                "and stop cleanly first";
    {
        // A book whose checkpoint stands short of its log's end: a crash, or a stop that exited 7.
        StoppedCleanly store;
        writeLog(store.storage, directory, 0, store.zero, true);
        writeLog(store.storage, directory, 1, store.one, true);
        expectError([&] { surveyLog(store.storage, moved); }, RecoveryError::Kind::PLACEMENT_CHANGED, refused, "a tail not empty");
    }
    {
        // A crash after a clean stop that left records past the checkpoints.
        StoppedCleanly store;
        stopCleanly(store, directory);
        writeLog(store.storage, directory, 1, commandsFor(1, 2), false, smallFiles(directory), Durability::MACHINE, 3);
        expectError([&] { surveyLog(store.storage, moved); }, RecoveryError::Kind::PLACEMENT_CHANGED, refused, "records past a checkpoint");
    }
    {
        // A torn tail on a shard whose books stand at its last record.
        StoppedCleanly store;
        stopCleanly(store, directory);
        const std::string last = logFileName(0, 0);
        const std::uint64_t at = bytesOf(store.storage, last).size();
        const std::unique_ptr<LogFile> file = store.storage.open(last);
        file->write(at, std::vector<std::byte>(replay::logRecordBytes / 2, std::byte{0x5a}));
        file->close();
        expectError([&] { surveyLog(store.storage, moved); }, RecoveryError::Kind::PLACEMENT_CHANGED, refused, "a torn tail");
    }
    {
        // A newest checkpoint that no longer decodes says nothing of where its tail ends.
        StoppedCleanly store;
        stopCleanly(store, directory);
        damage(store.storage, checkpointFileName(*Symbol::parse("IBM"), 3));
        expectError([&] { surveyLog(store.storage, moved); }, RecoveryError::Kind::PLACEMENT_CHANGED, refused, "a damaged checkpoint");
    }
    {
        // A changed shard count names the counts.
        StoppedCleanly store;
        writeLog(store.storage, directory, 0, store.zero, true);
        writeLog(store.storage, directory, 1, store.one, true);
        expectError([&] { surveyLog(store.storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1)}, 3)); },
                    RecoveryError::Kind::PLACEMENT_CHANGED,
                    "the log was written by 2 shards and this start has 3 and the last run did not stop cleanly; restart on the old "
                    "placement and stop cleanly first",
                    "a changed count, the tail not empty");
    }
}

// After a clean stop a change goes through: each moved book rewrapped at its new shard's log end,
// every older checkpoint dropped, and every shard recovering the clean stop's books.
void testAPlacementChangeAfterACleanStop() {
    const InstrumentDirectory directory = twoShards();
    {
        // MSFT moves from shard 0, whose log ends at 4, to shard 1, whose ends at 3.
        StoppedCleanly store;
        stopCleanly(store, directory);
        const InstrumentDirectory moved({on("AAPL", 0), on("MSFT", 1), on("IBM", 1)}, 2);
        const LogSurvey survey = surveyLog(store.storage, moved);
        expect(survey.placement && survey.placement->what == "MSFT moved from shard 0 to shard 1", "the survey names the move");
        const std::vector<Rewrap>& rewraps = survey.placement->rewraps;
        expect(rewraps.size() == 1 && rewraps[0].symbol == *Symbol::parse("MSFT") && rewraps[0].from == 4 && rewraps[0].fromShard == 0 &&
                   rewraps[0].fromShards == 2 && rewraps[0].toShard == 1 && rewraps[0].toShards == 2 && rewraps[0].to == 3,
               "MSFT is rewrapped from 4 on shard 0 to 3 on shard 1");
        expect(survey.placement->dropped == std::vector<std::string>{"ckpt-AAPL-00000000000000000000.obckpt", "ckpt-MSFT-00000000000000000000.obckpt"},
               "MSFT's checkpoint at 0 is dropped, and AAPL's, on the shard MSFT left, whose log after it holds MSFT's records");
        const std::vector<RecoveredShard> recovered = startOn(store.storage, moved, imagesBySymbol(store), "a moved symbol");
        expect(recovered[0].start == 4 && recovered[0].replayed == 4 && recovered[1].start == 3 && recovered[1].replayed == 3,
               "each shard starts at its own log's end and replays nothing");
        expect(checkpointsOf(store.storage) == std::vector<std::string>{"ckpt-AAPL-00000000000000000004.obckpt",
                                                                         "ckpt-IBM-00000000000000000000.obckpt",
                                                                         "ckpt-IBM-00000000000000000003.obckpt",
                                                                         "ckpt-MSFT-00000000000000000003.obckpt"},
               "IBM keeps its older checkpoint, on a shard that only gained a book");
        const CheckpointHeader header = headerOf(store.storage, "ckpt-MSFT-00000000000000000003.obckpt");
        expect(header.shardIndex == 1 && header.shardCount == 2 && header.position == 3, "MSFT's under shard 1 at 3");
        // The new run's first files carry the new placement, and then nothing has changed.
        writeLog(store.storage, moved, 0, {}, true, smallFiles(moved), Durability::MACHINE, 4);
        writeLog(store.storage, moved, 1, {}, true, smallFiles(moved), Durability::MACHINE, 3);
        expect(!surveyLog(store.storage, moved).placement, "the next start finds no change");
        // And IBM can still fall back to its older one: shard 1's log from 0 holds IBM's records alone.
        damage(store.storage, "ckpt-IBM-00000000000000000003.obckpt");
        const std::vector<RecoveredShard> fallen = startOn(store.storage, moved, imagesBySymbol(store), "IBM's fall-back after the move");
        expect(fallen[1].start == 0 && fallen[1].fellBack == 1, "IBM restores from 0 and replays shard 1's log");
    }
    {
        // IBM leaves shard 1 for shard 0, so shard 1 holds no book: it reads its log past IBM's
        // records and starts at its end, then and at every later start.
        StoppedCleanly store;
        stopCleanly(store, directory);
        const InstrumentDirectory gathered({on("AAPL", 0), on("MSFT", 0), on("IBM", 0)}, 2);
        const std::vector<RecoveredShard> recovered = startOn(store.storage, gathered, imagesBySymbol(store), "a shard left with no book");
        expect(recovered[1].start == 3 && recovered[1].replayed == 3 && recovered[0].start == 4,
               "shard 1, with no book, starts at its log's end, 3");
        writeLog(store.storage, gathered, 0, {}, true, smallFiles(gathered), Durability::MACHINE, 4);
        writeLog(store.storage, gathered, 1, {}, true, smallFiles(gathered), Durability::MACHINE, 3);
        expect(!surveyLog(store.storage, gathered).placement, "the next start finds no change");
        const std::vector<RecoveredShard> again = startOn(store.storage, gathered, imagesBySymbol(store), "the next start");
        expect(again[1].start == 3 && again[1].replayed == 3, "and shard 1 starts at its end again");
    }
    {
        // Three shards: AAPL stays on shard 0 at its own log's end, so its rewrap replaces a file of
        // the same name; MSFT goes to shard 1, at 3; IBM to shard 2, which has no log, at 0.
        StoppedCleanly store;
        stopCleanly(store, directory);
        const InstrumentDirectory three({on("AAPL", 0), on("MSFT", 1), on("IBM", 2)}, 3);
        const LogSurvey survey = surveyLog(store.storage, three);
        expect(survey.placement && survey.placement->what == "the log was written by 2 shards and this start has 3" &&
                   survey.placement->rewraps.size() == 3,
               "every book is rewrapped under the new count");
        startOn(store.storage, three, imagesBySymbol(store), "three shards");
        const std::vector<std::tuple<std::string, std::uint32_t, std::uint64_t>> placed{
            {"ckpt-AAPL-00000000000000000004.obckpt", 0, 4}, {"ckpt-IBM-00000000000000000000.obckpt", 2, 0}, {"ckpt-MSFT-00000000000000000003.obckpt", 1, 3}};
        expect(checkpointsOf(store.storage).size() == 3, "one checkpoint a book");
        for (const auto& [name, shard, position] : placed) {
            const CheckpointHeader header = headerOf(store.storage, name);
            expect(header.shardCount == 3 && header.shardIndex == shard && header.position == position, name + " under its new shard of 3");
        }
    }
    {
        // One shard: IBM leaves shard 1, whose log no shard writes any more and none recovers.
        StoppedCleanly store;
        stopCleanly(store, directory);
        const InstrumentDirectory one({on("AAPL", 0), on("MSFT", 0), on("IBM", 0)}, 1);
        const std::vector<RecoveredShard> recovered = startOn(store.storage, one, imagesBySymbol(store), "one shard");
        expect(recovered.size() == 1 && recovered[0].start == 4, "one shard, from its log's end");
        expect(store.storage.file(logFileName(1, 0)).has_value(), "shard 1's log stays, unread");
        expect(headerOf(store.storage, "ckpt-IBM-00000000000000000004.obckpt").shardIndex == 0, "IBM rewrapped at shard 0's end");
        writeLog(store.storage, one, 0, {}, true, smallFiles(one), Durability::MACHINE, 4);
        const LogSurvey next = surveyLog(store.storage, one);
        expect(!next.placement && next.shards.size() == 1, "and the next start finds no change, the old log set aside");
    }
}

// A crash at every step of a start that changes placement, as an engine and a machine with three
// seeds: the change itself, then each shard's recovery and its log writer's first file under the
// new placement, so some shards' newest headers are new and others' old. The next start finds
// the same change with less to do, or none, and recovers the clean stop's books.
void testACrashAtEveryStepOfAPlacementChange() {
    const InstrumentDirectory directory = twoShards();
    StoppedCleanly store;
    stopCleanly(store, directory);
    const std::map<std::string, std::vector<std::byte>> expected = imagesBySymbol(store);
    const InstrumentDirectory three({on("AAPL", 0), on("MSFT", 1), on("IBM", 2)}, 3);
    std::vector<InMemoryLogStorage::Contents> steps{store.storage.contents()};
    store.storage.watch([&steps](const InMemoryLogStorage::Contents& drive) { steps.push_back(drive); });
    std::size_t changed = 0;  // the step that ends the change itself
    {
        changePlacement(store.storage, *surveyLog(store.storage, three).placement);
        changed = steps.size() - 1;
        const LogSurvey survey = surveyLog(store.storage, three);
        Housekeeping housekeeping(store.storage, three, Durability::MACHINE, smallFiles(three));
        for (std::uint32_t shard = 0; shard < three.shardCount(); ++shard) {
            Shard books(three, shard);
            const RecoveredShard recovered =
                recoverShard(store.storage, store.storage, three, survey.shards[shard], survey.checkpoints[shard], books, defaultBookCapacity, running);
            static_cast<void>(housekeeping.first(shard, recovered.replayed));
        }
    }
    store.storage.watch(nullptr);
    expect(steps.size() > 15, "a start that changes placement is many steps: " + std::to_string(steps.size()));
    for (std::size_t at = 0; at < steps.size(); ++at) {
        for (const auto& [kind, seed] : {std::pair{InMemoryLogStorage::Crash::ENGINE, 0ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 1ULL},
                                         std::pair{InMemoryLogStorage::Crash::MACHINE, 2ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 3ULL}}) {
            InMemoryLogStorage crashed(steps[at]);
            crashed.crash(kind, seed);
            startOn(crashed, three, expected, "a crash at step " + std::to_string(at) + " of the change");
        }
    }

    // Cut short once every book is rewrapped, and before any new log file: the logs' headers still
    // name the old placement, so a start on it finds no change, and is refused at the first
    // rewrapped checkpoint, sent on to the new one, which finishes the change.
    InMemoryLogStorage cutShort(steps[changed]);
    cutShort.crash(InMemoryLogStorage::Crash::ENGINE, 0);
    const LogSurvey old = surveyLog(cutShort, directory);
    expect(!old.placement, "the old placement's start sees no change");
    Shard zero(directory, 0);
    expectError([&] { recoverShard(cutShort, cutShort, directory, old.shards[0], old.checkpoints[0], zero, defaultBookCapacity, running); },
                RecoveryError::Kind::PLACEMENT_CHANGED,
                "ckpt-AAPL-00000000000000000004.obckpt was written by shard 0 of 3, and this start places AAPL on shard 0 of 2, though the "
                "logs' headers name this placement: a placement change was cut short, so start on the placement that checkpoint names to "
                "finish it",
                "a start on the old placement after a change cut short");
    startOn(cutShort, three, expected, "the start on the new placement, after one on the old");
}

int main() {
    // A recovery that throws where none should is a failure to report, not a crash.
    try {
        testNothingToRecover();
        testACleanLogReplays();
        testATornTailIsCut();
        testOnlyEmptyFiles();
        testWhatStopsAShard();
        testEveryOtherFindingStops();
        testAHoleNearTheEndIsCut();
        testAMachineCrashCanLeaveAHole();
        testACrashAtEveryStepOfRotation();
        testACrashAtEveryStepOfAStart();
        testAPageStoreRecoversAtEveryStep();
        testWhatRefusesAStart();
        testWarming();
        testAStopInterrupts();
        testBooksRestoreFromCheckpoints();
        testBooksOnOneShardAtDifferentPositions();
        testWhatCheckpointsStop();
        testANewInstrument();
        testRetention();
        testRetentionKeepsTheLastRecord();
        testAPlacementChangeRefused();
        testAPlacementChangeAfterACleanStop();
        testACrashAtEveryStepOfAPlacementChange();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: an unexpected exception: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "recovery tests passed" << std::endl;
    return EXIT_SUCCESS;
}
