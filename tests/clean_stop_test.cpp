#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/clean_stop.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/runtime/shard.hpp"

#ifdef _MSC_VER
#include <stdlib.h>
#endif

namespace order_book {

// What the live book holds that no command changes and no check sees.
struct OrderBookTestAccess {
    static OrderBook& book(runtime::Shard& shard, std::uint32_t slot) { return const_cast<OrderBook&>(shard.bookSet.book(slot)); }
    static void setNextTradeId(OrderBook& book, TradeId next) { book.nextTradeId = next; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static auto& index(OrderBook& book) { return book.orderIndex; }
};

}  // namespace order_book

// A clean stop's last three steps as plain calls on the in-memory LogStorage (spec #58's D6): a
// shard's live books checked and imaged as its matching thread does at stop, then the roll
// forward taking the log to its end, comparing each book byte for byte with the live one,
// publishing a book that matches, and saving the live image of one that differs. The engine
// runs the same calls on its threads. See docs/internals/runtime.md.
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

const Symbol aapl = *Symbol::parse("AAPL");
const Symbol msft = *Symbol::parse("MSFT");

struct Placed {
    std::uint32_t slot;
    replay::Command command;
};

// Each book trades within itself, buys and sells alternating on it, with a cancel of every fifth.
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

std::uint64_t fileBytesFor(const InstrumentDirectory& directory, std::uint64_t records) {
    return logFileShape(directory, 0, Durability::MACHINE, defaultLogFileBytes).headerBytes + records * replay::logRecordBytes;
}

using Bytes = std::vector<std::byte>;

Bytes imageOf(const OrderBook& book) {
    Bytes image(book.imageSize());
    static_cast<void>(book.writeImage(image));
    return image;
}

// The same commands on plain books, which nothing restores or logs.
std::vector<Bytes> witnessImages(const std::vector<Placed>& commands, std::size_t count) {
    std::vector<OrderBook> books(2);
    for (std::size_t i = 0; i < count; ++i) {
        static_cast<void>(replay::executeCommand(books[commands[i].slot], commands[i].command));
    }
    return {imageOf(books[0]), imageOf(books[1])};
}

Bytes bytesOf(LogStorage& storage, const std::string& name) {
    const std::unique_ptr<LogFile> file = storage.open(name);
    Bytes bytes(static_cast<std::size_t>(file->size()));
    expect(file->read(0, bytes) == bytes.size(), name + " reads whole");
    return bytes;
}

std::vector<std::string> checkpointNames(const LogStorage& storage) {
    std::vector<std::string> names = storage.names();
    std::erase_if(names, [](const std::string& name) { return !name.starts_with("ckpt-"); });
    return names;
}

// Shard 0 of an engine run as its threads run it, on one thread: an empty book's checkpoint for
// each instrument where the log begins, a live shard taking every command, the log writer and
// housekeeping writing them in files of `records`, and a roller that has rolled to `rolledTo`,
// lagging. stopLog is the clean stop's first two steps, live its third.
class Running {
public:
    Running(LogStorage& logs, const InstrumentDirectory& instruments, const std::vector<Placed>& commands, std::uint64_t records,
            std::uint64_t rolledTo, Durability durability = Durability::MACHINE)
        : storage(logs), directory(instruments), shard(instruments, 0) {
        for (const Instrument& instrument : directory.onShard(0)) {
            publishEmptyCheckpoint(storage, instrument.symbol, directory.shardCount(), 0, 0);
        }
        housekeeping.emplace(storage, directory, durability, fileBytesFor(directory, records));
        writer.emplace(*housekeeping, 0, ring, durable, 0);
        roller.emplace(storage, directory, 0, [this](const std::string& line) { reported.push_back(line); });
        for (const Placed& placed : commands) {
            shard.process(placed.command, placed.slot);
            while (!ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND})) {
                drain();
            }
        }
        drain();
        expect(durable.load() == commands.size(), "every command reached the log");
        if (rolledTo != 0) {
            roller->roll(rolledTo);
        }
    }

    // Steps 1 and 2: the writer writes the rest, cuts, flushes, and closes, and the unused next
    // file goes.
    void stopLog() {
        expect(ring.tryPush(LogEntry::stop()), "room for the stop");
        drain();
        expect(writer->stopped(), "the writer stopped");
        housekeeping->removeUnused();
    }

    LogStorage& storage;
    const InstrumentDirectory& directory;
    Shard shard;
    Ring<LogEntry> ring{4096};
    DurablePosition durable;
    std::optional<Housekeeping> housekeeping;
    std::optional<LogWriter> writer;
    std::optional<ShardRoller> roller;
    std::vector<std::string> reported;

private:
    void drain() {
        while (writer->pass() != 0 || housekeeping->pass() != 0) {
        }
    }
};

const std::atomic<bool> running{false};

RecoveredShard recoverInto(LogStorage& storage, const InstrumentDirectory& directory, Shard& shard) {
    const LogSurvey survey = surveyLog(storage, directory);
    return recoverShard(storage, storage, directory, survey.shards[0], survey.checkpoints[0], shard, defaultBookCapacity, running);
}

void expectImagesEqual(const Shard& shard, const std::vector<Bytes>& witness, const std::string& what) {
    for (std::uint32_t slot = 0; slot < witness.size(); ++slot) {
        expect(imageOf(shard.books().book(slot)) == witness[slot], what + ": slot " + std::to_string(slot) + "'s image is the witness's");
    }
}

// A checkpoint's image, which must decode as published at this position.
Bytes checkpointImage(LogStorage& storage, const std::string& name, const Symbol& symbol, std::uint64_t position) {
    const Bytes bytes = bytesOf(storage, name);
    const auto decoded = decodeCheckpoint(bytes);
    const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
    expect(checkpoint != nullptr && checkpoint->header.symbol == symbol && checkpoint->header.position == position &&
               checkpoint->header.shardIndex == 0 && checkpoint->header.shardCount == 2,
           name + " decodes as written for its book, shard, and position");
    return Bytes(checkpoint->image.begin(), checkpoint->image.end());
}

void testCapturingLiveImages() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(30);
    Shard shard(directory, 0);
    for (const Placed& placed : commands) {
        shard.process(placed.command, placed.slot);
    }
    const LiveImages live = captureLiveImages(shard);
    expect(live && *live == witnessImages(commands, 30), "each live book's image, by slot, is what the same commands give");
    expect(!captureLiveImages(shard, false), "and the check alone gives none");
}

// Thirty commands in files of eight, rolled to 8 when the stop comes: the stop rolls the files
// at 8 and 16, then the last, short one, to 30, and both books match and publish there.
void testAStopThatMatchesPublishes() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(30);
    InMemoryLogStorage storage;
    Running run(storage, directory, commands, 8, 8);
    expect(run.roller->position(0) == 8 && run.roller->filesBehind() == 0, "rolled to 8 before the stop");
    run.stopLog();
    const LiveImages live = captureLiveImages(run.shard);
    run.roller->finish(30, live);
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        expect(run.roller->stopped(slot) == StoppedBook{.outcome = StopOutcome::PUBLISHED}, "slot " + std::to_string(slot) + " matched and published");
        expect(run.roller->position(slot) == 30, "and stands at the log's end");
        expect(!formatStoppedBook(directory.onShard(0)[slot].symbol, 30, run.roller->stopped(slot)), "and says nothing");
    }
    expect(run.roller->filesBehind() == 0 && run.roller->counts().writesFailed == 0 && run.reported.empty(), "nothing behind, nothing failed");
    expect(checkpointNames(storage) == std::vector<std::string>{"ckpt-AAPL-00000000000000000024.obckpt", "ckpt-AAPL-00000000000000000030.obckpt",
                                                                "ckpt-MSFT-00000000000000000024.obckpt", "ckpt-MSFT-00000000000000000030.obckpt"},
           "the closed files rolled, each book keeping its two newest, and nothing saved as .live");
    const std::vector<Bytes> witness = witnessImages(commands, 30);
    expect(checkpointImage(storage, checkpointFileName(aapl, 30), aapl, 30) == witness[0] &&
               checkpointImage(storage, checkpointFileName(msft, 30), msft, 30) == witness[1],
           "each holds the books the commands give");

    // The next start restores both at the end and replays nothing.
    Shard next(directory, 0);
    const RecoveredShard recovered = recoverInto(storage, directory, next);
    expect(recovered.start == 30 && recovered.replayed == 30 && next.commandsTaken() == 0, "the next start replays nothing");
    expectImagesEqual(next, witness, "restored at the end");

    // A stop with no command since the start finds both books already standing at the end.
    ShardRoller again(storage, directory, 0, [](const std::string&) {});
    again.finish(30, captureLiveImages(next));
    expect(again.stopped(0) == StoppedBook{.outcome = StopOutcome::PUBLISHED} && again.stopped(1) == StoppedBook{.outcome = StopOutcome::PUBLISHED},
           "a book already at the end, matching, counts as published");
    expect(again.counts().published == 0 && checkpointNames(storage).size() == 4, "with nothing written again");
}

// A STOP on a file just filled: its last file is full, and the next one, made ahead at the end, is
// gone by the time the roll starts.
void testAStopOnAFullFile() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(32);
    InMemoryLogStorage storage;
    Running run(storage, directory, commands, 8, 0);
    run.stopLog();
    expect(!storage.file(logFileName(0, 32)), "no next file at the end");
    run.roller->finish(32, captureLiveImages(run.shard));
    expect(run.roller->stopped(0).outcome == StopOutcome::PUBLISHED && run.roller->stopped(1).outcome == StopOutcome::PUBLISHED &&
               run.roller->position(0) == 32 && run.roller->position(1) == 32 && run.roller->filesBehind() == 0,
           "every file rolled, the full last one included, and both published at 32");
}

// The named case: what checkInvariants cannot see, the next trade ID, changed on the live book.
// Nothing is published for it, the symbol and the first differing byte are named, the live image
// is saved, and the next start recovers a sound book, which then stops cleanly.
void testALiveBookDriftedFromItsLog() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(30);
    InMemoryLogStorage storage;
    Running run(storage, directory, commands, 8, 24);
    OrderBook& drifting = OrderBookTestAccess::book(run.shard, 0);
    OrderBookTestAccess::setNextTradeId(drifting, OrderBookTestAccess::nextTradeId(drifting) + 2);
    expect(!run.shard.checkInvariants(), "which checkInvariants passes");
    run.stopLog();
    const LiveImages live = captureLiveImages(run.shard);
    run.roller->finish(30, live);

    // The next trade ID is the image's bytes 28 to 31, little-endian (docs/checkpoint-format.md).
    expect(run.roller->stopped(0) == StoppedBook{.outcome = StopOutcome::DIFFERS, .differsAt = 28, .saved = true}, "AAPL differs at byte 28, and its live image is saved");
    expect(formatStoppedBook(aapl, 30, run.roller->stopped(0)) ==
               "clean stop incomplete: AAPL differs at byte 28, saved ckpt-AAPL-00000000000000000030.live; the next start recovers from the log",
           "#34's message");
    expect(run.roller->stopped(1) == StoppedBook{.outcome = StopOutcome::PUBLISHED}, "MSFT, sound, publishes");
    expect(run.roller->position(0) == 24 && !storage.file(checkpointFileName(aapl, 30)), "nothing published for AAPL at the end");
    expect(checkpointImage(storage, liveImageFileName(aapl, 30), aapl, 30) == (*live)[0], "the .live file holds the live image, as a checkpoint");
    expect((*live)[0] != witnessImages(commands, 30)[0], "which is not what the log gives");

    // The log stays the truth: AAPL from its checkpoint at 24 and the last six, MSFT at 30.
    Shard next(directory, 0);
    const RecoveredShard recovered = recoverInto(storage, directory, next);
    expect(recovered.restoredFrom == std::vector<std::uint64_t>{24, 30} && recovered.replayed == 30, "the next start rolls AAPL from 24");
    expectImagesEqual(next, witnessImages(commands, 30), "the next start recovers sound books");
    ShardRoller again(storage, directory, 0, [](const std::string&) {});
    again.finish(30, captureLiveImages(next));
    expect(again.stopped(0) == StoppedBook{.outcome = StopOutcome::PUBLISHED} && again.position(0) == 30, "and its stop publishes AAPL at the end");

    // A second drift at the same end replaces the saved image.
    Shard third(directory, 0);
    recoverInto(storage, directory, third);
    OrderBookTestAccess::setNextTradeId(OrderBookTestAccess::book(third, 1), 900);
    ShardRoller once(storage, directory, 0, [](const std::string&) {});
    const LiveImages drifted = captureLiveImages(third);
    once.finish(30, drifted);
    expect(once.stopped(1).outcome == StopOutcome::DIFFERS && once.stopped(1).saved &&
               checkpointImage(storage, liveImageFileName(msft, 30), msft, 30) == (*drifted)[1],
           "MSFT, drifted after it stood at the end, differs, and its .live is saved");
    OrderBookTestAccess::setNextTradeId(OrderBookTestAccess::book(third, 1), 901);
    const LiveImages driftedAgain = captureLiveImages(third);
    once.finish(30, driftedAgain);
    expect(once.stopped(1).saved && checkpointImage(storage, liveImageFileName(msft, 30), msft, 30) == (*driftedAgain)[1],
           "a later one at the same end replaces it");
}

// A file under a checkpoint's name, so publishing it meets EXISTS at the rename, for that book alone.
void squat(InMemoryLogStorage& storage, const std::string& name) {
    const std::unique_ptr<LogFile> file = storage.create(name, Durability::MACHINE);
    file->write(0, Bytes(8, std::byte{1}));
    file->close();
}

// Each other way a book goes unpublished, and its message.
void testWhatLeavesABookUnpublished() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(30);
    {
        // No live images: the closed files still roll, and nothing is published at the end.
        InMemoryLogStorage storage;
        Running run(storage, directory, commands, 8, 8);
        run.stopLog();
        run.roller->finish(30, std::nullopt);
        expect(run.roller->stopped(0) == StoppedBook{.outcome = StopOutcome::NO_IMAGE} && run.roller->stopped(1) == StoppedBook{.outcome = StopOutcome::NO_IMAGE},
               "no image, so nothing compared");
        expect(run.roller->position(0) == 24 && run.roller->position(1) == 24 && run.roller->filesBehind() == 0,
               "every closed file rolled, and the last one left to the next start's replay");
        expect(formatStoppedBook(aapl, 30, run.roller->stopped(0)) ==
                   "clean stop incomplete: AAPL's image buffer could not be allocated; the next start recovers from the log",
               "the allocation's message");
    }
    {
        // The last publish fails for AAPL, its name taken: matched, and not published.
        InMemoryLogStorage storage;
        Running run(storage, directory, commands, 8, 24);
        run.stopLog();
        squat(storage, checkpointFileName(aapl, 30));
        run.roller->finish(30, captureLiveImages(run.shard));
        expect(run.roller->stopped(0) == StoppedBook{.outcome = StopOutcome::PUBLISH_FAILED} && run.roller->stopped(1) == StoppedBook{.outcome = StopOutcome::PUBLISHED},
               "AAPL's publish failed, MSFT's didn't");
        expect(run.roller->counts().writesFailed == 1 && run.reported.size() == 1 && run.roller->position(0) == 24,
               "counted, reported, and AAPL left at 24");
        expect(formatStoppedBook(aapl, 30, run.roller->stopped(0)) ==
                   "clean stop incomplete: AAPL's checkpoint at 30 could not be published; the next start recovers from the log",
               "the publish's message");
    }
    {
        // AAPL's publish at 16 fails during the stop's roll, so it never reaches the last file.
        InMemoryLogStorage storage;
        Running run(storage, directory, commands, 8, 8);
        run.stopLog();
        squat(storage, checkpointFileName(aapl, 16));
        run.roller->finish(30, captureLiveImages(run.shard));
        expect(run.roller->stopped(0) == StoppedBook{.outcome = StopOutcome::NOT_ROLLED} && run.roller->stopped(1) == StoppedBook{.outcome = StopOutcome::PUBLISHED},
               "AAPL behind, MSFT at the end");
        expect(run.roller->position(0) == 8 && run.roller->filesBehind() == 3, "AAPL at 8, the files at 8, 16, and 24 behind");
        expect(formatStoppedBook(aapl, 30, run.roller->stopped(0)) ==
                   "clean stop incomplete: AAPL could not be rolled forward to the log's end, 30; the next start recovers from the log",
               "the roll's message");
    }
    expect(formatStoppedBook(aapl, 30, StoppedBook{.outcome = StopOutcome::DIFFERS, .differsAt = 5, .saved = false}) ==
               "clean stop incomplete: AAPL differs at byte 5, and could not save ckpt-AAPL-00000000000000000030.live; the next start "
               "recovers from the log",
           "a .live that could not be saved");
}

struct Step {
    InMemoryLogStorage::Contents drive;
    std::uint64_t released;  // every response with a command below this was released
};

// The named crash points: a crash at every step of a clean stop, the log's cut, flush, and
// close, the unused next file's removal, every checkpoint the last roll publishes and every one
// it supersedes, and a drifted book's .live, as an engine and a machine with three seeds. The
// next start recovers the books the surviving commands give, losing no released response's.
void testACrashAtEveryStepOfACleanStop() {
    const InstrumentDirectory directory = twoShards();
    const std::vector<Placed> commands = tradingCommandsFor(30);
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        for (const bool drift : {false, true}) {
            const std::string what = std::string(durability == Durability::MACHINE ? "machine" : "process") + (drift ? ", MSFT drifted" : "");
            InMemoryLogStorage storage;
            // Leaves the last six records unflushed at process durability.
            Running run(storage, directory, commands, 8, 8, durability);
            if (drift) {
                OrderBookTestAccess::setNextTradeId(OrderBookTestAccess::book(run.shard, 1), 77);
            }
            std::vector<Step> steps{Step{storage.contents(), run.durable.load()}};
            storage.watch([&](const InMemoryLogStorage::Contents& drive) { steps.push_back(Step{drive, run.durable.load()}); });
            run.stopLog();
            run.roller->finish(30, captureLiveImages(run.shard));
            storage.watch(nullptr);
            expect(run.roller->stopped(0).outcome == StopOutcome::PUBLISHED &&
                       run.roller->stopped(1).outcome == (drift ? StopOutcome::DIFFERS : StopOutcome::PUBLISHED),
                   what + ": the stop does what it should uncrashed");
            expect(steps.size() > 20, what + ": many steps to crash at: " + std::to_string(steps.size()));
            for (std::size_t at = 0; at < steps.size(); ++at) {
                for (const auto& [kind, seed] : {std::pair{InMemoryLogStorage::Crash::ENGINE, 0ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 1ULL},
                                                 std::pair{InMemoryLogStorage::Crash::MACHINE, 2ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 3ULL}}) {
                    const std::string crash = what + ", step " + std::to_string(at) +
                                              (kind == InMemoryLogStorage::Crash::ENGINE ? ", an engine crash" : ", a machine crash of seed " + std::to_string(seed));
                    InMemoryLogStorage crashed(steps[at].drive);
                    crashed.crash(kind, seed);
                    Shard shard(directory, 0);
                    const RecoveredShard recovered = recoverInto(crashed, directory, shard);
                    const std::uint64_t survived = recovered.replayed;
                    expect(survived <= commands.size(), crash + ": no more survives than was written");
                    if (durability == Durability::MACHINE || kind == InMemoryLogStorage::Crash::ENGINE) {
                        expect(survived >= steps[at].released, crash + ": every released response's command survives");
                    }
                    expectImagesEqual(shard, witnessImages(commands, survived), crash + ", at the surviving position " + std::to_string(survived));
                }
            }
        }
    }
}

// The named case: what only the live book holds. A clean stop's first two steps run, then an
// index entry is broken with no command after it, its price or its level handle, which leaves
// the image as it was, so no comparison can see it: checkInvariants at step 3 aborts.
int abortOnABrokenIndex(bool levelHandle) {
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no dialog to wait on
#endif
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Running run(storage, directory, tradingCommandsFor(30), 8, 8);
    run.stopLog();
    OrderBook& book = OrderBookTestAccess::book(run.shard, 0);
    const Bytes before = imageOf(book);
    auto& index = OrderBookTestAccess::index(book);
    expect(index.begin() != index.end(), "AAPL has a resting order");
    if (levelHandle) {
        (*index.begin()).second.level = nullptr;
    } else {
        (*index.begin()).second.tickPrice += 1;
    }
    expect(imageOf(book) == before, "the broken index leaves the image as it was");
    static_cast<void>(captureLiveImages(run.shard));
    std::cerr << "captureLiveImages returned\n";
    return EXIT_SUCCESS;  // a mutant that carries on passes expect_failure's exit check only by aborting
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "abort-index" || std::string_view(argv[1]) == "abort-level-handle")) {
        return abortOnABrokenIndex(std::string_view(argv[1]) == "abort-level-handle");
    }
    try {
        testCapturingLiveImages();
        testAStopThatMatchesPublishes();
        testAStopOnAFullFile();
        testALiveBookDriftedFromItsLog();
        testWhatLeavesABookUnpublished();
        testACrashAtEveryStepOfACleanStop();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: threw " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "clean_stop_tests passed" << std::endl;
    return EXIT_SUCCESS;
}
