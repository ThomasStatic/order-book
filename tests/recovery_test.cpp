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
              bool cleanStop, std::uint64_t fileBytes = defaultLogFileBytes) {
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytes);
    Ring<LogEntry> ring(4096);
    DurablePosition durable;
    LogWriter writer(housekeeping, shard, ring, durable);
    for (const Placed& placed : commands) {
        expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "the test's log ring holds every command");
    }
    if (cleanStop) {
        expect(ring.tryPush(LogEntry::stop()), "and the stop");
    }
    while (writer.pass() != 0 || housekeeping.pass() != 0) {
    }
    if (cleanStop) {
        expect(writer.stopped(), "the writer stopped");
        housekeeping.removeUnused();
    }
    expect(durable.load() == commands.size(), "every command reached the log");
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
