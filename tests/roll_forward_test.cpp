#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/closed_log.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/roll_forward.hpp"

// Roll forward as plain calls on the in-memory LogStorage (spec #58's D4b): logs written the way
// the engine writes them, rolled by a ShardRoller, whose checkpoints must hold the images of
// books that ran the same commands directly, byte for byte. See docs/internals/runtime.md.
namespace {

using namespace order_book;
using namespace order_book::runtime;
using Bytes = std::vector<std::byte>;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(std::string_view text) {
    const std::optional<Symbol> symbol = Symbol::parse(text);
    expect(symbol.has_value(), "a test symbol parses: " + std::string(text));
    return *symbol;
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{symbolOf(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

// AAPL, MSFT, and QUIET on shard 0, IBM on shard 1, of two. Commands go to AAPL and MSFT only,
// so QUIET is a book with none.
InstrumentDirectory directoryOf() {
    return InstrumentDirectory({on("AAPL", 0), on("MSFT", 0), on("QUIET", 0), on("IBM", 1)}, 2);
}

struct Placed {
    std::uint32_t slot;
    replay::Command command;
};

// Adds on slots 0 and 1, alternating sides, at prices that cross often enough to trade, then a
// cancel of every fifth order, so the books hold resting orders and trades.
std::vector<Placed> commandsFor(std::size_t count) {
    std::vector<Placed> placed;
    for (std::size_t i = 1; i <= count; ++i) {
        // After the first 30, MSFT gets none: a book with resting orders that later passes rewrap.
        const auto slot = static_cast<std::uint32_t>(i <= 30 ? i % 2 : 0);
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

constexpr std::uint64_t recordsPerFile = 25;
constexpr std::size_t commandCount = 100;

std::uint64_t fileBytesFor(const InstrumentDirectory& directory) {
    return logFileShape(directory, 0, Durability::MACHINE, defaultLogFileBytes).headerBytes + recordsPerFile * replay::logRecordBytes;
}

// Shard 0's log as the engine writes it, in files of 25 records, left running: the next file made
// ahead starts at the last position, so every file holding a record is closed once closedEnd
// reaches it.
void writeLog(LogStorage& storage, const InstrumentDirectory& directory, const std::vector<Placed>& commands) {
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(directory));
    Ring<LogEntry> ring(4096);
    DurablePosition durable(0);
    LogWriter writer(housekeeping, 0, ring, durable, 0);
    for (const Placed& placed : commands) {
        expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "room in the log ring");
    }
    while (writer.pass() != 0 || housekeeping.pass() != 0) {
    }
    expect(durable.load() == commands.size(), "every command reached the log");
}

// The images of books that ran the first `count` commands directly, by slot.
std::vector<Bytes> witnessImages(const std::vector<Placed>& commands, std::size_t count) {
    std::vector<OrderBook> books(3);
    for (std::size_t i = 0; i < count; ++i) {
        static_cast<void>(replay::executeCommand(books[commands[i].slot], commands[i].command));
    }
    std::vector<Bytes> images;
    for (const OrderBook& book : books) {
        Bytes image(book.imageSize());
        expect(book.writeImage(image) == image.size(), "a witness image fits");
        images.push_back(std::move(image));
    }
    return images;
}

const char* const symbols[] = {"AAPL", "MSFT", "QUIET"};

// Every published checkpoint of shard 0's books in a store, by symbol and position, decoded.
struct Published {
    CheckpointHeader header;
    Bytes image;
};

std::map<std::pair<std::string, std::uint64_t>, Published> publishedIn(const InMemoryLogStorage& storage) {
    std::map<std::pair<std::string, std::uint64_t>, Published> found;
    for (const std::string& name : storage.names()) {
        const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
        if (!parts || parts->temporary) {
            continue;
        }
        const Bytes file = storage.read(name, 0);
        const std::optional<CheckpointFault> fault = verifyCheckpoint(file);
        expect(!fault, name + " verifies" + (fault ? ": " + formatCheckpointFault(name, *fault) : std::string()));
        const auto decoded = std::get<DecodedCheckpoint>(decodeCheckpoint(file));
        expect(decoded.header.symbol == parts->symbol && decoded.header.position == parts->position,
               name + ": its header names its symbol and position");
        found.emplace(std::pair{parts->symbol.text(), parts->position},
                      Published{decoded.header, Bytes(decoded.image.begin(), decoded.image.end())});
    }
    return found;
}

std::vector<std::string> temporariesIn(const InMemoryLogStorage& storage) {
    std::vector<std::string> left;
    for (const std::string& name : storage.names()) {
        const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
        if (parts && parts->temporary) {
            left.push_back(name);
        }
    }
    return left;
}

// Each published image must be a witness's at its position.
void expectPublishedAreWitnesses(const InMemoryLogStorage& storage, const std::vector<Placed>& commands, const std::string& what) {
    for (const auto& [key, published] : publishedIn(storage)) {
        const auto& [symbol, position] = key;
        const std::vector<Bytes> witness = witnessImages(commands, static_cast<std::size_t>(position));
        const auto slot = static_cast<std::size_t>(std::find(std::begin(symbols), std::end(symbols), symbol) - std::begin(symbols));
        expect(slot < 3, what + ": a checkpoint only for shard 0's books, not " + symbol);
        expect(published.image == witness[slot],
               what + ": " + symbol + "'s checkpoint at " + std::to_string(position) + " is the witness's image");
        expect(published.header.shardCount == 2 && published.header.shardIndex == 0, what + ": shard 0 of 2");
    }
}

// Every book's two kept checkpoints: at `newest` and the file before it.
void expectKeptAt(const InMemoryLogStorage& storage, std::uint64_t newest, const std::string& what) {
    const auto published = publishedIn(storage);
    for (const char* symbol : symbols) {
        expect(published.contains({symbol, newest}) && published.contains({symbol, newest - recordsPerFile}),
               what + ": " + symbol + " keeps checkpoints at " + std::to_string(newest - recordsPerFile) + " and " +
                   std::to_string(newest));
    }
    expect(published.size() == 6, what + ": two per book, older ones removed; found " + std::to_string(published.size()));
}

// Every file a store names, with its bytes.
std::map<std::string, Bytes> filesOf(const InMemoryLogStorage& storage) {
    std::map<std::string, Bytes> files;
    for (const std::string& name : storage.names()) {
        files.emplace(name, storage.read(name, 0));
    }
    return files;
}

ShardRoller rollerOf(LogStorage& storage, const InstrumentDirectory& directory, std::vector<std::string>* reports = nullptr) {
    return ShardRoller(storage, directory, 0, [reports](const std::string& line) {
        if (reports != nullptr) {
            reports->push_back(line);
        }
    });
}

// ---- Names, the proof, and publishing ----------------------------------------------------

void testNames() {
    expect(checkpointFileName(symbolOf("AAPL"), 12) == "ckpt-AAPL-00000000000000000012.obckpt", "a checkpoint's name");
    expect(checkpointFileName(symbolOf("NUL"), 0) == "ckpt-NUL-00000000000000000000.obckpt", "a symbol Windows refuses alone");
    for (const char* text : {"ES-Z26", "BRK.B", "A", "ZZZZZZZZ"}) {
        const Symbol symbol = symbolOf(text);
        const std::string name = checkpointFileName(symbol, 18446744073709551615ULL);
        const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
        expect(parts && parts->symbol == symbol && parts->position == 18446744073709551615ULL && !parts->temporary,
               "a name reads back: " + name);
        const std::optional<CheckpointFileNameParts> temporary = parseCheckpointFileName(name + ".tmp");
        expect(temporary && temporary->symbol == symbol && temporary->temporary, "a .tmp reads as one: " + name);
    }
    for (const char* name : {"ckpt-AAPL-12.obckpt", "ckpt--00000000000000000012.obckpt", "ckpt-aapl-00000000000000000012.obckpt",
                             "ckpt-AAPL-0000000000000000001x.obckpt", "log-0000-00000000000000000000.oblog",
                             "ckpt-AAPL-00000000000000000012.obckpt.tmp.tmp", "ckpt-AAPL-00000000000000000012.oblog",
                             "ckpt-AAPL-99999999999999999999.obckpt", "ckpt-AAPL+00000000000000000012.obckpt"}) {
        expect(!parseCheckpointFileName(name), std::string("not a checkpoint's name: ") + name);
    }
}

void testTheProofRefusesABadImage() {
    const std::vector<Placed> commands = commandsFor(40);
    const Bytes good = witnessImages(commands, commands.size())[0];
    expect(!proveImage(good), "a sound book's image proves");
    expect(!proveImage(witnessImages(commands, 0)[0]), "an empty book's proves");

    Bytes reserved = good;
    reserved[60] = std::byte{1};  // the header's reserved word
    const std::optional<std::string> refused = proveImage(reserved);
    expect(refused && refused->find("NONZERO_RESERVED") != std::string::npos, "an image restore refuses fails the proof");

    // An image restore accepts but writes back otherwise: its header stated 72 bytes, with 8 more
    // after the fields this version knows.
    Bytes longer(good.begin(), good.begin() + 64);
    longer[8] = std::byte{72};
    longer.insert(longer.end(), 8, std::byte{0});
    longer.insert(longer.end(), good.begin() + 64, good.end());
    const std::optional<std::string> rewritten = proveImage(longer);
    expect(rewritten && rewritten->find("differ") != std::string::npos,
           "an image that does not write back the same bytes fails the proof" + (rewritten ? ": " + *rewritten : std::string()));
}

void testPublishing() {
    const std::vector<Placed> commands = commandsFor(40);
    const Bytes image = witnessImages(commands, commands.size())[0];
    CheckpointHeader header;
    header.engineMajor = 2;
    header.engineMinor = 1;
    header.shardCount = 2;
    header.shardIndex = 0;
    header.position = 25;
    header.symbol = symbolOf("AAPL");

    InMemoryLogStorage storage;
    publishCheckpoint(storage, header, image);
    const std::string first = checkpointFileName(header.symbol, 25);
    expect(storage.names() == std::vector<std::string>{first}, "one file, under its name, no .tmp left");
    expect(storage.read(first, 0) == encodeCheckpoint(header, image), "holding the encoded checkpoint");
    const std::optional<InMemoryLogStorage::File> file = storage.file(first);
    expect(file && file->nameOnDrive && file->sizeOnDrive == file->size, "flushed, and its name on the drive");

    header.position = 50;
    publishCheckpoint(storage, header, image);
    const std::vector<std::string> both{first, checkpointFileName(header.symbol, 50)};
    expect(storage.names() == both, "a second checkpoint stands beside the first");

    // A write that fails leaves what was published, and no .tmp.
    storage.failWriteAfter(0);
    header.position = 75;
    bool threw = false;
    try {
        publishCheckpoint(storage, header, image);
    } catch (const LogStorageError&) {
        threw = true;
    }
    expect(threw, "a failed write throws LogStorageError");
    expect(storage.names() == both, "a failed publish keeps what was published and leaves no .tmp");
}

// A store whose files being created read back with their 40th byte changed, as a drive that
// lost a write would: what publishing's read-back is for.
class MisreadingStorage final : public LogStorage {
public:
    explicit MisreadingStorage(LogStorage& inner) : held(inner) {}

    std::unique_ptr<StorageLock> lock() override { return held.lock(); }
    std::vector<std::string> names() const override { return held.names(); }
    std::unique_ptr<LogFile> open(const std::string& name) override { return held.open(name); }
    std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override {
        return std::make_unique<Misread>(held.create(name, durability));
    }
    void remove(const std::string& name) override { held.remove(name); }
    std::string describe(const std::string& name) const override { return held.describe(name); }

private:
    class Misread final : public LogFile {
    public:
        explicit Misread(std::unique_ptr<LogFile> inner) : file(std::move(inner)) {}
        std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
            const std::size_t got = file->read(offset, out);
            if (offset <= 40 && offset + got > 40) {
                out[static_cast<std::size_t>(40 - offset)] ^= std::byte{1};
            }
            return got;
        }
        std::uint64_t size() override { return file->size(); }
        void write(std::uint64_t offset, std::span<const std::byte> bytes) override { file->write(offset, bytes); }
        void flush() override { file->flush(); }
        void extend(std::uint64_t size) override { file->extend(size); }
        void cut(std::uint64_t size) override { file->cut(size); }
        void close() override { file->close(); }
        void rename(const std::string& name) override { file->rename(name); }

    private:
        std::unique_ptr<LogFile> file;
    };

    LogStorage& held;
};

// A store that records every name opened.
class OpenRecordingStorage final : public LogStorage {
public:
    explicit OpenRecordingStorage(LogStorage& inner) : held(inner) {}

    std::unique_ptr<StorageLock> lock() override { return held.lock(); }
    std::vector<std::string> names() const override { return held.names(); }
    std::unique_ptr<LogFile> open(const std::string& name) override {
        opened.push_back(name);
        return held.open(name);
    }
    std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override { return held.create(name, durability); }
    void remove(const std::string& name) override { held.remove(name); }
    std::string describe(const std::string& name) const override { return held.describe(name); }

    std::vector<std::string> opened;

private:
    LogStorage& held;
};

// The log writer's file and the one made ahead are never opened: on Windows a file has one handle.
void testOnlyClosedFilesAreOpened() {
    const InstrumentDirectory directory = directoryOf();
    InMemoryLogStorage storage;
    writeLog(storage, directory, commandsFor(commandCount));
    OpenRecordingStorage recording(storage);
    ShardRoller roller(recording, directory, 0, [](const std::string&) {});
    roller.roll(99);
    for (const std::string& name : recording.opened) {
        expect(name != logFileName(0, 75) && name != logFileName(0, 100), "an open file is never opened: " + name);
    }
    expect(std::find(recording.opened.begin(), recording.opened.end(), logFileName(0, 50)) != recording.opened.end(),
           "a closed one is");
}

void testAReadBackThatDiffersIsNotPublished() {
    CheckpointHeader header;
    header.shardCount = 1;
    header.position = 25;
    header.symbol = symbolOf("AAPL");
    InMemoryLogStorage storage;
    MisreadingStorage misreading(storage);
    bool threw = false;
    try {
        publishCheckpoint(misreading, header, witnessImages(commandsFor(10), 10)[0]);
    } catch (const LogStorageError& error) {
        threw = std::string_view(error.what()).find("does not read back as written") != std::string_view::npos;
    }
    expect(threw, "a .tmp that reads back otherwise is a failed publish");
    expect(storage.names().empty(), "and leaves nothing behind");
}

// A third checkpoint, older than a book's two, which a machine crash undoing a removal leaves,
// goes when a roller is made, as at a start.
void testAnExtraCheckpointGoesAtStart() {
    const InstrumentDirectory directory = directoryOf();
    const std::vector<Placed> commands = commandsFor(commandCount);
    InMemoryLogStorage storage;
    writeLog(storage, directory, commands);
    rollerOf(storage, directory).roll(50);  // AAPL at 25 and 50
    const std::string third = checkpointFileName(symbolOf("AAPL"), 0);
    {
        const std::unique_ptr<LogFile> file = storage.create(third, Durability::PROCESS);
        CheckpointHeader header;
        header.shardCount = 2;
        header.symbol = symbolOf("AAPL");
        file->write(0, encodeCheckpoint(header, witnessImages(commands, 0)[0]));
        file->close();
    }
    const ShardRoller roller = rollerOf(storage, directory);
    expect(roller.position(0) == 50, "the newest stays");
    const std::vector<std::string> names = storage.names();
    expect(std::find(names.begin(), names.end(), third) == names.end(), "the third, older one is removed");
    expect(std::find(names.begin(), names.end(), checkpointFileName(symbolOf("AAPL"), 25)) != names.end(), "the older kept one is not");
}

// ---- Rolling a shard ---------------------------------------------------------------------

void testRollingAShard() {
    const InstrumentDirectory directory = directoryOf();
    const std::vector<Placed> commands = commandsFor(commandCount);
    InMemoryLogStorage storage;
    writeLog(storage, directory, commands);
    const InMemoryLogStorage::Contents logged = storage.contents();

    ShardRoller roller = rollerOf(storage, directory);
    expect(roller.position(0) == std::nullopt, "no book has a checkpoint before the first pass");
    expect(roller.roll(commandCount) == 4, "one pass per closed file");
    expectKeptAt(storage, 100, "rolled at once");
    expectPublishedAreWitnesses(storage, commands, "rolled at once");
    expect(roller.counts().passes == 4 && roller.counts().published == 12 && roller.counts().writesFailed == 0 &&
               roller.counts().passesLost == 0,
           "the counts: 4 passes, 12 checkpoints");
    expect(roller.position(0) == 100 && roller.position(2) == 100 && roller.filesBehind() == 0, "every book at 100");
    expect(roller.roll(commandCount) == 0, "nothing left to roll");
    const auto atOnce = filesOf(storage);

    // A file not yet closed is not rolled: the file at 75 closes once 100 is reached.
    InMemoryLogStorage stepped(logged);
    ShardRoller step = rollerOf(stepped, directory);
    expect(step.roll(99) == 3, "three files closed before 100");
    expect(step.position(1) == 75, "the books stand at the start of the open file");
    expect(step.roll(100) == 1 && step.position(1) == 100, "and the fourth rolls once it closes");
    expect(filesOf(stepped) == atOnce, "file by file gives the same store as all at once");

    // A roller made later, as at a restart, starts from the books' kept checkpoints.
    InMemoryLogStorage restarted(logged);
    expect(rollerOf(restarted, directory).roll(50) == 2, "the first run rolls two files");
    ShardRoller later = rollerOf(restarted, directory);
    expect(later.position(0) == 50, "a later roller reads the kept positions");
    expect(later.roll(100) == 2, "and rolls only what is left");
    expect(filesOf(restarted) == atOnce, "a restart gives the same store");
}

// A checkpoint that fails to publish is counted and reported, its book stays behind, the others
// go on, and a later roll on a store that writes again catches it up.
void testAFailedWriteWaits() {
    const InstrumentDirectory directory = directoryOf();
    const std::vector<Placed> commands = commandsFor(commandCount);
    InMemoryLogStorage storage;
    writeLog(storage, directory, commands);
    InMemoryLogStorage reference(storage.contents());
    rollerOf(reference, directory).roll(commandCount);

    std::vector<std::string> reports;
    storage.failWriteAfter(1);  // AAPL's first checkpoint is written, then nothing more
    ShardRoller roller = rollerOf(storage, directory, &reports);
    roller.roll(commandCount);
    expect(roller.counts().published == 1 && roller.counts().writesFailed == 3,
           "one published; MSFT and QUIET at 25 failed, then AAPL at 50");
    expect(roller.position(0) == 25 && roller.position(1) == std::nullopt && roller.position(2) == std::nullopt,
           "each book stands where its last checkpoint does");
    expect(roller.filesBehind() == 4, "every closed file is still needed by some book");
    expect(reports.size() == 3 && reports[0].find("ckpt-MSFT-00000000000000000025.obckpt") != std::string::npos,
           "each failure is reported, naming the checkpoint" + (reports.empty() ? std::string() : ": " + reports[0]));
    expect(temporariesIn(storage).empty(), "no .tmp is left behind");
    expectPublishedAreWitnesses(storage, commands, "after failed writes");

    InMemoryLogStorage writes(storage.contents());
    ShardRoller after = rollerOf(writes, directory);
    after.roll(commandCount);
    expect(filesOf(writes) == filesOf(reference), "a store that writes again catches up byte for byte");
}

// A crash at every step of publishing leaves every checkpoint a file holds whole and true, and a
// roller made after it catches up to the uninterrupted run byte for byte, removing any .tmp.
void testACrashAtEveryStepOfPublishing() {
    const InstrumentDirectory directory = directoryOf();
    const std::vector<Placed> commands = commandsFor(commandCount);
    InMemoryLogStorage storage;
    writeLog(storage, directory, commands);
    InMemoryLogStorage reference(storage.contents());
    rollerOf(reference, directory).roll(commandCount);

    std::vector<InMemoryLogStorage::Contents> steps;
    storage.watch([&steps](const InMemoryLogStorage::Contents& drive) { steps.push_back(drive); });
    rollerOf(storage, directory).roll(commandCount);
    storage.watch(nullptr);
    expect(steps.size() > 40, "publishing takes many steps: " + std::to_string(steps.size()));

    std::size_t sawTemporary = 0;
    for (std::size_t at = 0; at < steps.size(); ++at) {
        for (const auto& [kind, seed] : {std::pair{InMemoryLogStorage::Crash::ENGINE, 0ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 1ULL},
                                         std::pair{InMemoryLogStorage::Crash::MACHINE, 2ULL}, std::pair{InMemoryLogStorage::Crash::MACHINE, 3ULL}}) {
            const std::string what = "a " + std::string(kind == InMemoryLogStorage::Crash::ENGINE ? "engine" : "machine") +
                                     " crash after step " + std::to_string(at) + " (seed " + std::to_string(seed) + ")";
            InMemoryLogStorage crashed(steps[at]);
            crashed.crash(kind, seed);
            if (!temporariesIn(crashed).empty()) {
                ++sawTemporary;
            }
            expectPublishedAreWitnesses(crashed, commands, what);
            ShardRoller roller = rollerOf(crashed, directory);
            expect(temporariesIn(crashed).empty(), what + ": a new roller removes a leftover .tmp");
            roller.roll(commandCount);
            const auto published = publishedIn(crashed);
            const auto expected = publishedIn(reference);
            for (const char* symbol : symbols) {
                expect(published.contains({symbol, 100}) && published.at({symbol, 100}).image == expected.at({symbol, 100}).image,
                       what + ": " + symbol + " catches up to the uninterrupted run");
            }
            expectPublishedAreWitnesses(crashed, commands, what + ", then rolled");
            ShardRoller(crashed, directory, 0, [](const std::string&) {});  // as the next start
            expect(filesOf(crashed) == filesOf(reference), what + ": then the store is the uninterrupted run's");
        }
    }
    expect(sawTemporary > 0, "some crash left a .tmp to remove");
}

// ---- The worker ------------------------------------------------------------------------------

// The log writer says where it started and where each file it fills ends, once flushed and closed.
void testTheWriterSaysWhatClosed() {
    const InstrumentDirectory directory = directoryOf();
    InMemoryLogStorage storage;
    ClosedLog closed(2);
    Housekeeping housekeeping(storage, directory, Durability::PROCESS, fileBytesFor(directory));
    Ring<LogEntry> ring(4096);
    DurablePosition durable(0);
    LogWriter writer(housekeeping, 0, ring, durable, 0, &closed);
    expect(closed.end(0) == 0 && closed.generation() == 1, "the writer says where it starts");
    const std::vector<Placed> commands = commandsFor(60);
    for (const Placed& placed : commands) {
        expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "room in the log ring");
    }
    while (writer.pass() != 0 || housekeeping.pass() != 0) {
    }
    expect(closed.end(0) == 50 && closed.generation() == 3, "and each full file's end: 25, then 50, with 60 written");
    const std::optional<InMemoryLogStorage::File> second = storage.file(logFileName(0, 25));
    expect(second && second->closed && second->sizeOnDrive == second->size, "a file said closed is flushed and closed");
    expect(closed.end(1) == 0, "another shard's end is its own");
}

// A worker's pass rolls each of its shards to what the log says is closed.
void testAWorkerPassRollsWhatClosed() {
    const InstrumentDirectory directory = directoryOf();
    InMemoryLogStorage storage;
    writeLog(storage, directory, commandsFor(commandCount));
    InMemoryLogStorage reference(storage.contents());
    rollerOf(reference, directory).roll(commandCount);

    ClosedLog closed(2);
    RollWorker worker(storage, directory, closed, {0, 1}, [](const std::string&) {});
    closed.started(0, 60);  // as a writer starting at 60 says: the files before it are closed
    expect(worker.pass() == 2 && worker.roller(0).position(0) == 50, "the files closed by 60");
    closed.closed(0, 100);
    expect(worker.pass() == 2 && worker.roller(0).position(0) == 100 && worker.roller(1).counts().passes == 0, "then the rest");
    expect(filesOf(storage) == filesOf(reference), "the store a roller makes");
}

// A worker on its own thread follows a writer as files close, and stops when told.
void testAWorkerThreadFollowsTheLog() {
    const InstrumentDirectory directory = directoryOf();
    const std::vector<Placed> commands = commandsFor(commandCount);
    InMemoryLogStorage storage;
    InMemoryLogStorage reference;
    writeLog(reference, directory, commands);
    rollerOf(reference, directory).roll(commandCount);

    ClosedLog closed(2);
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(directory));
    Ring<LogEntry> ring(4096);
    DurablePosition durable(0);
    LogWriter writer(housekeeping, 0, ring, durable, 0, &closed);
    RollWorker worker(storage, directory, closed, {0}, [](const std::string&) {});
    std::thread thread([&worker] { worker.run(); });
    for (const Placed& placed : commands) {
        expect(ring.tryPush(LogEntry{placed.command, placed.slot, ControlKind::COMMAND}), "room in the log ring");
        while (writer.pass() != 0 || housekeeping.pass() != 0) {
        }
    }
    const std::string last = checkpointFileName(symbolOf("QUIET"), 100);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const std::vector<std::string> names = storage.names();
        if (std::find(names.begin(), names.end(), last) != names.end() || std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closed.stop();
    thread.join();
    expect(worker.roller(0).position(2) == 100, "the worker rolled every file as it closed");
    std::map<std::string, Bytes> checkpoints;
    for (const auto& [name, bytes] : filesOf(storage)) {
        if (parseCheckpointFileName(name)) {
            checkpoints.emplace(name, bytes);
        }
    }
    std::map<std::string, Bytes> expected;
    for (const auto& [name, bytes] : filesOf(reference)) {
        if (parseCheckpointFileName(name)) {
            expected.emplace(name, bytes);
        }
    }
    expect(checkpoints == expected, "the same checkpoints as a roll after the fact");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "abort-proof") {
#ifdef _MSC_VER
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no dialog to wait on
#endif
        // An image restore refuses, as a broken rolled book would write: the proof must abort.
        Bytes image = witnessImages(commandsFor(10), 10)[0];
        image[60] = std::byte{1};
        requireProvedImage(image, "shard 0 AAPL at 25");
        std::cerr << "requireProvedImage returned\n";
        return EXIT_SUCCESS;  // a mutant that carries on passes expect_failure's exit check only by aborting
    }
    try {
        testNames();
        testTheProofRefusesABadImage();
        testPublishing();
        testAReadBackThatDiffersIsNotPublished();
        testOnlyClosedFilesAreOpened();
        testAnExtraCheckpointGoesAtStart();
        testRollingAShard();
        testAFailedWriteWaits();
        testACrashAtEveryStepOfPublishing();
        testTheWriterSaysWhatClosed();
        testAWorkerPassRollsWhatClosed();
        testAWorkerThreadFollowsTheLog();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "roll_forward_tests passed\n";
    return EXIT_SUCCESS;
}
