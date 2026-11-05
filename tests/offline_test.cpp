#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/offline.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/runtime/shard.hpp"

// The offline commands' work (spec #58's D7) on the in-memory storage: verify read-only, with a
// book's check after every command finding a poison command exactly; checkpoint roll; and
// cut-log moving a record and all after it to a cut- file, every byte kept, so a start passes
// it, crashed at every step. A sound book has no poison command, so one is stood up as a check
// that fails once AAPL's book rests five orders, deterministic in the book's state as a real
// one would be. order_book_cli's output and exits are engine_end_to_end_tests'. See
// docs/internals/runtime.md.
namespace {

using namespace order_book;
using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

// AAPL and MSFT on shard 0, IBM on shard 1, of two.
InstrumentDirectory twoShards() {
    return InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("IBM", 1)}, 2);
}

struct Placed {
    std::uint32_t slot;
    replay::Command command;
};

// Buys on alternating books at falling prices, so nothing trades and each rests: slot 0 rests
// its fifth order at the ninth command, position 8.
std::vector<Placed> restingCommandsFor(std::size_t slots, std::size_t count) {
    std::vector<Placed> placed;
    for (std::size_t i = 0; i < count; ++i) {
        replay::AddCommand add{};
        add.orderId = static_cast<OrderId>(i + 1);
        add.side = Side::BUY;
        add.price = static_cast<Price>(200 - i);
        add.quantity = 1;
        placed.push_back(Placed{static_cast<std::uint32_t>(i % slots), add});
    }
    return placed;
}

std::uint64_t smallFiles(const InstrumentDirectory& directory) {
    return leastLogFileBytes(directory) + 7 * replay::logRecordBytes;  // eight records
}

// A shard's log as an engine writes it, after each instrument's empty checkpoint at 0, stopped
// cleanly or left as a crash leaves it.
void writeLog(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shard, const std::vector<Placed>& commands, bool cleanStop) {
    for (const Instrument& instrument : directory.onShard(shard)) {
        publishEmptyCheckpoint(storage, instrument.symbol, directory.shardCount(), shard, 0);
    }
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, smallFiles(directory));
    Ring<LogEntry> ring(4096);
    DurablePosition durable(0);
    LogWriter writer(housekeeping, shard, ring, durable, 0);
    for (const Placed& placed : commands) {
        expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "room in the ring");
    }
    if (cleanStop) {
        expect(ring.tryPush(LogEntry::stop()), "room for the stop");
    }
    while (writer.pass() != 0 || housekeeping.pass() != 0) {
    }
    if (cleanStop) {
        housekeeping.removeUnused();
    }
}

// Thirty commands on shard 0 and three on shard 1, in files of eight records, stopped cleanly.
void writeStore(InMemoryLogStorage& storage, const InstrumentDirectory& directory) {
    writeLog(storage, directory, 0, restingCommandsFor(2, 30), true);
    writeLog(storage, directory, 1, restingCommandsFor(1, 3), true);
}

// The stand-in poison command: a book resting five orders fails.
const BookCheck poison = [](const OrderBook& book) -> std::optional<std::string> {
    if (book.activeOrderCount() >= 5) {
        return "it rests " + std::to_string(book.activeOrderCount()) + " orders";
    }
    return std::nullopt;
};

const std::atomic<bool> running{false};

RecoveredShard recover(InMemoryLogStorage& storage, const InstrumentDirectory& directory, std::uint32_t number) {
    const LogSurvey survey = surveyLog(storage, directory);
    Shard shard(directory, number);
    return recoverShard(storage, storage, directory, survey.shards[number], survey.checkpoints[number], shard, defaultBookCapacity, running);
}

std::vector<replay::ReplayRecord> recordsOf(const InMemoryLogStorage& storage, const std::string& name) {
    const std::vector<std::byte> bytes = storage.read(name, 0);
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::ios::binary);
    replay::CommandLogReader reader(input);
    std::vector<replay::ReplayRecord> records;
    for (;;) {
        replay::ReadResult result = reader.next();
        if (auto* record = std::get_if<replay::ReplayRecord>(&result)) {
            records.push_back(*record);
        } else if (const auto* error = std::get_if<replay::ParseError>(&result)) {
            expect(false, name + " reads to its end: " + error->message);
        } else {
            return records;
        }
    }
}

void testTheStoreGivesItsPlacement() {
    InMemoryLogStorage storage;
    writeStore(storage, twoShards());
    publishEmptyCheckpoint(storage, *Symbol::parse("NEW"), 2, 1, 3);  // a new instrument a start gave a checkpoint
    const InstrumentDirectory found = directoryOfStore(storage, storage);
    expect(found.shardCount() == 2 && found.find(*Symbol::parse("AAPL"))->shard == 0 && found.find(*Symbol::parse("MSFT"))->shard == 0 &&
               found.find(*Symbol::parse("IBM"))->shard == 1 && found.find(*Symbol::parse("NEW"))->shard == 1,
           "every symbol on the shard its newest header or checkpoint names, of the count shard 0's header gives");
    InMemoryLogStorage empty;
    try {
        static_cast<void>(directoryOfStore(empty, empty));
        expect(false, "a store with no log has no placement");
    } catch (const RecoveryError& error) {
        expect(error.kind() == RecoveryError::Kind::START_REFUSED, "refused");
    }
}

void testVerifyReportsAndChangesNothing() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);
    const StoreVerification clean = verifyStore(storage, storage, false);
    expect(clean.outcome == VerifyOutcome::OK && clean.shards.size() == 2, "a clean store verifies: " + clean.failure);
    expect(formatVerifiedShard(clean.shards[0]) ==
               "shard 0: start=0 end=30 files=4 tail-cut-bytes=0 files-dropped=0\nbook AAPL shard=0 checkpoint=0\nbook MSFT shard=0 checkpoint=0\n",
           "a line for the shard, and one for each book:\n" + formatVerifiedShard(clean.shards[0]));
    expect(formatVerifiedShard(clean.shards[1]) == "shard 1: start=0 end=3 files=1 tail-cut-bytes=0 files-dropped=0\nbook IBM shard=1 checkpoint=0\n",
           formatVerifiedShard(clean.shards[1]));

    // A crash's torn tail and the next file made ahead: a start would cut and remove them, and
    // verify only says so.
    InMemoryLogStorage crashed;
    writeLog(crashed, directory, 0, restingCommandsFor(2, 13), false);
    writeLog(crashed, directory, 1, restingCommandsFor(1, 3), true);
    const std::string last = logFileName(0, 8);
    const std::unique_ptr<LogFile> file = crashed.open(last);
    file->write(crashed.read(last, 0).size(), std::vector<std::byte>(17, std::byte{0x2a}));
    file->close();
    const InMemoryLogStorage::Contents before = crashed.contents();
    const StoreVerification torn = verifyStore(crashed, crashed, true);
    expect(torn.outcome == VerifyOutcome::OK, "a torn tail verifies, since a start passes it: " + torn.failure);
    expect(torn.shards[0].end == 13 && torn.shards[0].tailCutBytes == 17 && torn.shards[0].filesDropped == 1,
           "saying what a start would cut and remove: " + formatVerifiedShard(torn.shards[0]));
    const InMemoryLogStorage::Contents after = crashed.contents();
    expect(after.files.size() == before.files.size() && crashed.read(last, 0) == InMemoryLogStorage(before).read(last, 0) &&
               crashed.file(logFileName(0, 16)).has_value(),
           "and touching nothing: the tail stands, and the file made ahead");

    // What a start stops on, verify stops on, with the same words.
    InMemoryLogStorage damaged;
    writeStore(damaged, directory);
    std::vector<std::byte> bytes = damaged.read(logFileName(0, 0), 0);
    bytes[bytes.size() - 3 * replay::logRecordBytes + 12] ^= std::byte{1};
    {
        const std::unique_ptr<LogFile> rewrite = damaged.open(logFileName(0, 0));
        rewrite->write(0, bytes);
        rewrite->close();
    }
    const StoreVerification stopped = verifyStore(damaged, damaged, false);
    expect(stopped.outcome == VerifyOutcome::STOPPED && stopped.failure.find("CORRUPT_RECORD") != std::string::npos &&
               stopped.failure.find("in a file before the last") != std::string::npos,
           "a record that doesn't chain in a closed file stops it: " + stopped.failure);

    // An engine, or another command, holding the lock.
    const std::unique_ptr<StorageLock> held = storage.lock();
    const StoreVerification locked = verifyStore(storage, storage, false);
    expect(locked.outcome == VerifyOutcome::STOPPED && locked.failure.find("locked") != std::string::npos, "a store in use: " + locked.failure);
}

// The named case: verify --validate per-command finds a poison command, and cut-log then lets a
// start pass it.
void testAPoisonCommandIsFoundAndCut() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);

    const StoreVerification final = verifyStore(storage, storage, false, poison);
    expect(final.outcome == VerifyOutcome::BROKEN &&
               final.failure == "shard 0 positions 0 to 30 symbol AAPL: replay left the book broken: it rests 15 orders; verify --validate "
                                "per-command finds the command",
           "a final check names only the range: " + final.failure);
    // A start that checked as verify does would stop on it too, and pass it once it is cut.
    const auto startWithTheCheck = [&] {
        const LogSurvey survey = surveyLog(storage, directory);
        Shard shard(directory, 0);
        RecoveryOptions options;
        options.check = poison;
        return recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running, options);
    };
    try {
        static_cast<void>(startWithTheCheck());
        expect(false, "a start stops on the poison command");
    } catch (const RecoveryError& error) {
        expect(error.kind() == RecoveryError::Kind::BOOK_BROKEN, std::string("a book broken: ") + error.what());
    }
    const StoreVerification exact = verifyStore(storage, storage, true, poison);
    expect(exact.outcome == VerifyOutcome::BROKEN &&
               exact.failure == "shard 0 position 8 file log-0000-00000000000000000008.oblog symbol AAPL command 9,ADD,9,BUY,192,1,LIMIT,GTC,,,,,AAPL: "
                                "the book fails its check after it: it rests 5 orders",
           "a check after every command names it exactly: " + exact.failure);

    const LogCut cut = cutLog(storage, storage, 0, 8);
    expect(cut.cut && cut.records == 22 && cut.file == "cut-0000-00000000000000000008.oblog", "the record and the 21 after it moved: " + cut.failure);
    const std::vector<replay::ReplayRecord> moved = recordsOf(storage, cut.file);
    const std::vector<Placed> original = restingCommandsFor(2, 30);
    expect(moved.size() == 22 && moved.front().sequence == 9 && moved.back().sequence == 30, "a valid log of them, from position 8");
    for (std::size_t i = 0; i < moved.size(); ++i) {
        expect(moved[i].command == original[8 + i].command && moved[i].symbol == directory.onShard(0)[original[8 + i].slot].symbol,
               "each command as it was written");
    }
    const StoreVerification after = verifyStore(storage, storage, true, poison);
    expect(after.outcome == VerifyOutcome::OK && after.shards[0].end == 8, "verify passes, the log ending at 8: " + after.failure);
    expect(recover(storage, directory, 0).replayed == 8 && startWithTheCheck().replayed == 8, "and so does a start, checking or not");

    // A verify that stops keeps the shards before it: a check only IBM's three orders fail.
    InMemoryLogStorage fresh;
    writeStore(fresh, directory);
    const StoreVerification partial = verifyStore(fresh, fresh, false, [](const OrderBook& book) -> std::optional<std::string> {
        return book.activeOrderCount() == 3 ? std::optional<std::string>("three") : std::nullopt;
    });
    expect(partial.outcome == VerifyOutcome::BROKEN && partial.shards.size() == 1 && partial.shards[0].shard == 0,
           "shard 0 verified, shard 1 broken: " + partial.failure);
    expect(!storage.file(logFileName(0, 16)) && !storage.file(logFileName(0, 24)), "the files after the record's gone");
}

void testWhatCutLogRefuses() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);
    ShardRoller(storage, directory, 0, [](const std::string&) {}).roll(24);  // checkpoints at 8, 16, 24
    const LogCut past = cutLog(storage, storage, 0, 20);
    expect(!past.cut && past.failure.starts_with("ckpt-") && past.failure.find("stands past position 20") != std::string::npos,
           "a checkpoint that holds the records: " + past.failure);
    const LogCut at = cutLog(storage, storage, 0, 24);
    expect(at.cut && at.records == 6, "one at the position holds none of them: " + at.failure);
    const LogCut done = cutLog(storage, storage, 0, 24);
    expect(done.cut && done.records == 6 && done.file == at.file, "run again once done, it says what it moved: " + done.failure);
    const LogCut beyond = cutLog(storage, storage, 0, 25);
    expect(!beyond.cut && beyond.failure == "shard 0 has no record at position 25 to cut", "nothing there: " + beyond.failure);
    const LogCut again = cutLog(storage, storage, 1, 99);
    expect(!again.cut && again.failure == "shard 1 has no record at position 99 to cut", again.failure);
    const LogCut nowhere = cutLog(storage, storage, 3, 0);
    expect(!nowhere.cut && nowhere.failure == "shard 3 has no log file holding position 0", nowhere.failure);
    InMemoryLogStorage elsewhere;  // a checkpoint store holding none of the shard's books
    const LogCut wrongStore = cutLog(storage, elsewhere, 0, 20);
    expect(!wrongStore.cut && wrongStore.failure == "no checkpoint of shard 0's books is in the checkpoint directory; if they are in another, "
                                                    "name it with --checkpoint-dir",
           wrongStore.failure);
}

// A hole that valid records follow, the damage a start stops on, is cut with every record from
// the position on: the hole and those after it copied as they were, unsealed.
void testACutKeepsEveryRecordPastAHole() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);
    const std::string name = logFileName(0, 8);
    const std::vector<std::byte> original = storage.read(name, 0);
    const std::size_t header = original.size() - 8 * replay::logRecordBytes;
    {
        const std::unique_ptr<LogFile> file = storage.open(name);
        file->write(header + 2 * replay::logRecordBytes, std::vector<std::byte>(replay::logRecordBytes, std::byte{0}));  // position 10
        file->close();
    }
    const StoreVerification stopped = verifyStore(storage, storage, false);
    expect(stopped.outcome == VerifyOutcome::STOPPED && stopped.failure.find("shard 0 position 10") != std::string::npos,
           "a start stops on the hole: " + stopped.failure);
    const LogCut cut = cutLog(storage, storage, 0, 10);
    expect(cut.cut && cut.records == 20, "the hole and the 19 records after it moved: " + cut.failure);
    const std::vector<std::byte> moved = storage.read(cut.file, 0);
    const std::size_t cutHeader = moved.size() - 20 * replay::logRecordBytes;
    expect(std::equal(moved.begin() + static_cast<std::ptrdiff_t>(cutHeader + replay::logRecordBytes),
                      moved.begin() + static_cast<std::ptrdiff_t>(cutHeader + 6 * replay::logRecordBytes),
                      original.begin() + static_cast<std::ptrdiff_t>(header + 3 * replay::logRecordBytes)),
           "the records after the hole, in its file, byte for byte");
    expect(verifyStore(storage, storage, false).outcome == VerifyOutcome::OK && recover(storage, directory, 0).replayed == 10,
           "and the log ends at 10");
}

// A crash at every step of a cut, as an engine and a machine with two seeds: run again, it
// finishes, every moved record in the cut file and none left in the log.
void testACrashAtEveryStepOfACut() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);
    std::vector<InMemoryLogStorage::Contents> steps{storage.contents()};
    storage.watch([&steps](const InMemoryLogStorage::Contents& drive) { steps.push_back(drive); });
    expect(cutLog(storage, storage, 0, 8).cut, "the cut");
    storage.watch(nullptr);
    expect(steps.size() > 5, "a cut is many steps: " + std::to_string(steps.size()));
    const std::vector<Placed> original = restingCommandsFor(2, 30);
    for (std::size_t at = 0; at < steps.size(); ++at) {
        for (const auto& [kind, seed] : {std::pair{InMemoryLogStorage::Crash::ENGINE, 0ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 1ULL},
                                         std::pair{InMemoryLogStorage::Crash::MACHINE, 2ULL}}) {
            const std::string what = "a crash at step " + std::to_string(at) + ", " + (kind == InMemoryLogStorage::Crash::ENGINE ? "engine" : "machine");
            InMemoryLogStorage crashed(steps[at]);
            crashed.crash(kind, seed);
            const LogCut again = cutLog(crashed, crashed, 0, 8);
            expect(again.cut || again.failure == "shard 0 has no record at position 8 to cut", what + ": run again, it cuts or finds it cut: " + again.failure);
            expect(recover(crashed, directory, 0).replayed == 8, what + ": the log ends at 8");
            const std::vector<replay::ReplayRecord> moved = recordsOf(crashed, "cut-0000-00000000000000000008.oblog");
            expect(moved.size() == 22, what + ": every moved record is in the cut file");
            for (std::size_t i = 0; i < moved.size(); ++i) {
                expect(moved[i].command == original[8 + i].command, what + ": as written");
            }
        }
    }
}

void testCheckpointRoll() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    writeStore(storage, directory);
    const StoreRoll rolled = rollStore(storage, storage);
    expect(rolled.outcome == RollOutcome::OK && rolled.files == 3 && rolled.books == 6,
           "shard 0's three closed files, each publishing both books; shard 1's one file isn't closed: " + rolled.failure);
    expect(ShardRoller(storage, directory, 0, [](const std::string&) {}).position(0) == 24, "AAPL stands at 24");
    const StoreRoll again = rollStore(storage, storage);
    expect(again.outcome == RollOutcome::OK && again.files == 0 && again.books == 0, "a second roll has nothing to do");

    InMemoryLogStorage fresh;
    writeStore(fresh, directory);
    const StoreRoll failed = rollStore(fresh, fresh, poison);
    expect(failed.outcome == RollOutcome::PROOF_FAILED &&
               failed.failure == "shard 0 AAPL at 16: the image proof failed: it rests 8 orders",
           "a rolled book that fails its proof: " + failed.failure);
    expect(ShardRoller(fresh, directory, 0, [](const std::string&) {}).position(0) == 8, "nothing published for it past the pass before");

    // Checkpoints that can't be published: each book's at 8, the first pass's, after which both
    // stand behind and need no later file.
    InMemoryLogStorage full;
    writeStore(full, directory);
    full.failCreateAfter(0);
    const StoreRoll unpublished = rollStore(full, full);
    expect(unpublished.outcome == RollOutcome::FAILED && unpublished.failure == "2 checkpoints were not published, which the next roll makes again" &&
               unpublished.reported.size() == 2,
           "a write that failed exits 1: " + unpublished.failure);

    // A closed file the roll can't read: reported, and exit 1, not a quiet success.
    InMemoryLogStorage damaged;
    writeStore(damaged, directory);
    std::vector<std::byte> bytes = damaged.read(logFileName(0, 0), 0);
    bytes[bytes.size() - 4 * replay::logRecordBytes + 12] ^= std::byte{1};
    {
        const std::unique_ptr<LogFile> file = damaged.open(logFileName(0, 0));
        file->write(0, bytes);
        file->close();
    }
    const StoreRoll stopped = rollStore(damaged, damaged);
    expect(stopped.outcome == RollOutcome::FAILED && stopped.failure.starts_with("the roll stopped") && !stopped.reported.empty(),
           "a roll that stopped: " + stopped.failure);

    const std::unique_ptr<StorageLock> held = storage.lock();
    const StoreRoll locked = rollStore(storage, storage);
    expect(locked.outcome == RollOutcome::FAILED && locked.failure.find("locked") != std::string::npos, "a store in use: " + locked.failure);
}

}  // namespace

int main() {
    try {
        testTheStoreGivesItsPlacement();
        testVerifyReportsAndChangesNothing();
        testAPoisonCommandIsFoundAndCut();
        testWhatCutLogRefuses();
        testACutKeepsEveryRecordPastAHole();
        testACrashAtEveryStepOfACut();
        testCheckpointRoll();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: threw " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "offline tests passed" << std::endl;
    return EXIT_SUCCESS;
}
