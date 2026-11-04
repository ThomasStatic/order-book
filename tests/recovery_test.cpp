#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/recovery.hpp"
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

// A shard's log as the engine writes it: housekeeping makes each file and the next one ahead,
// and the log writer writes every command. A clean stop finishes the last file and removes the
// unused next one; otherwise both stay as a crash leaves them.
void writeLog(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shard, const std::vector<Placed>& commands,
              bool cleanStop, std::uint64_t fileBytes = defaultLogFileBytes, Durability durability = Durability::MACHINE,
              std::uint64_t firstPosition = 0) {
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
    return recoverShard(storage, directory, survey.shards.at(number), shard, defaultBookCapacity, stopping);
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
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, directory, survey.shards[0], shard, defaultBookCapacity, running);
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
    const std::vector<std::string> before = storage.names();
    expect(before.size() == 4, "thirty records in files of eight is four files");
    const LogSurvey survey = surveyLog(storage, directory);
    expect(survey.shards[0].size() == 4 && survey.shards[0][3] == LogFileEntry{logFileName(0, 24), 24} && survey.shards[1].empty(),
           "the survey finds shard 0's files in order, each at its first position");

    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, directory, survey.shards[0], shard, defaultBookCapacity, running);
    expect(recovered.replayed == 30 && shard.nextPosition() == 30, "every record replayed, so the shard goes on from 30");
    expect(recovered.tailCutBytes == 0 && recovered.filesRemoved == 0, "with nothing to cut or remove");
    expectBooksEqual(shard, witnessOf(2, commands), "a clean log");
    expect(storage.names() == before, "and every file stays");
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
        expect(storage.names() == std::vector<std::string>{logFileName(0, 0), last, logFileName(0, 8)}, what + ": a full file, the last, and the next");
        const std::vector<std::byte> torn(20, std::byte{0xab});
        if (tear == Tear::IN_THE_TAIL) {
            storage.open(last)->write(header + 2 * replay::logRecordBytes, torn);
        } else if (tear == Tear::IN_THE_NEXT_FILE) {
            storage.open(logFileName(0, 8))->write(header, torn);
        }

        Shard shard(directory, 0);
        const RecoveredShard recovered = recover(storage, directory, 0, shard);
        expect(recovered.replayed == 6, what + ": every whole record replays");
        expect(recovered.filesRemoved == 1 && storage.names() == std::vector<std::string>{logFileName(0, 0), last},
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
    expect(storage.names().size() == 2, "a crash before any command leaves the first file and the next");
    Shard shard(directory, 1);
    const RecoveredShard recovered = recover(storage, directory, 1, shard);
    expect(recovered.replayed == 0 && recovered.filesRemoved == 2 && storage.names().empty(), "both go, and the shard starts at 0");
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
                    "shard 0 position 0 file log-0000-00000000000000000004.oblog: the log starts at position 4, and this build recovers "
                    "only from position 0",
                    "a missing first file");
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
    expect(recovered.replayed == 8 && storage.names() == std::vector<std::string>{logFileName(0, 0), logFileName(0, 4)},
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
    Shard shard(directory, 0);
    const RecoveredShard recovered = recoverShard(storage, directory, survey.shards[0], shard, defaultBookCapacity, running);
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
        expect(storage.names().size() == 4, "two files with records, the next one made ahead, and one being made");

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
            const RecoveredShard recovered = recoverShard(storage, directory, survey.shards[0], shard, defaultBookCapacity, running);
            expect(recovered.replayed == before && recovered.filesRemoved == 1 && recovered.tailCutBytes == 20,
                   "the start recovers the earlier run's forty, cutting its torn tail");
        }
        writeInSteps(storage, directory, durability, fileBytes, commands, before, durable);
        crashAtEachStep(steps, durability, directory, commands, fileBytes, "a start");
    }
}

// The start's own checks, on the main thread: a symbol off the list, a changed shard count, and
// a moved symbol; a new instrument is none of them.
void testWhatRefusesAStart() {
    InMemoryLogStorage storage;
    const InstrumentDirectory directory = twoShards();
    writeLog(storage, directory, 0, commandsFor(2, 4), true);
    writeLog(storage, directory, 1, commandsFor(1, 3), true);

    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0)}, 2)); }, RecoveryError::Kind::START_REFUSED,
                "log-0001-00000000000000000000.oblog names IBM, which is not on the instrument list", "a symbol off the list");
    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1)}, 3)); },
                RecoveryError::Kind::PLACEMENT_CHANGED,
                "the log was written by 2 shards and this start has 3, and this build can't change placement while a log exists, so start "
                "on the old placement",
                "a changed shard count");
    expectError([&] { surveyLog(storage, InstrumentDirectory({on("AAPL", 0), on("MSFT", 1), on("IBM", 1)}, 2)); },
                RecoveryError::Kind::PLACEMENT_CHANGED,
                "MSFT moved from shard 0 to shard 1, and this build can't change placement while a log exists, so start on the old "
                "placement",
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
        testWhatRefusesAStart();
        testWarming();
        testAStopInterrupts();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: an unexpected exception: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "recovery tests passed" << std::endl;
    return EXIT_SUCCESS;
}
