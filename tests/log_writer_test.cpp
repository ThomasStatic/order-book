#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

#include "order_book/command_log.hpp"
#include "order_book/runtime/doorbell.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"

// The log writer and the housekeeping that makes its files, as plain objects over the in-memory
// LogStorage, then on real threads, then on real files. What is written is read back through
// CommandLogReader, never compared with its own encoder. Given "abort-write", "abort-flush", or
// "abort-next-file" it runs a writer or housekeeping whose storage fails, which must abort naming
// the shard and file, as CTest checks through tests/expect_failure.cmake. See
// docs/internals/runtime.md.
namespace {

using namespace order_book;
using namespace order_book::runtime;
namespace fs = std::filesystem;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

// AAPL and MSFT on shard 1, IBM on shard 0, of two.
InstrumentDirectory twoShards() {
    return InstrumentDirectory({on("MSFT", 1), on("IBM", 0), on("AAPL", 1)}, 2);
}

replay::AddCommand buy(OrderId id, Price price, Quantity quantity) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = Side::BUY;
    command.price = price;
    command.quantity = quantity;
    return command;
}

LogEntry entry(const replay::Command& command, std::uint32_t slot) {
    return LogEntry{command, slot, ControlKind::COMMAND};
}

struct Decoded {
    replay::LogHeader header;
    std::vector<replay::ReplayRecord> records;
    std::optional<replay::ParseError> error;
};

Decoded decode(const std::vector<std::byte>& bytes) {
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), std::ios::binary);
    replay::CommandLogReader reader(input);
    Decoded decoded;
    for (;;) {
        const replay::ReadResult result = reader.next();
        if (const auto* record = std::get_if<replay::ReplayRecord>(&result)) {
            decoded.records.push_back(*record);
        } else {
            if (const auto* error = std::get_if<replay::ParseError>(&result)) {
                decoded.error = *error;
            }
            break;
        }
    }
    if (reader.header() != nullptr) {
        decoded.header = *reader.header();
    }
    return decoded;
}

InMemoryLogStorage::File fileOf(const InMemoryLogStorage& storage, const std::string& name) {
    const std::optional<InMemoryLogStorage::File> file = storage.file(name);
    expect(file.has_value(), "the storage holds " + name);
    return *file;
}

// What a header of this shard takes, its symbols and checksum included: all a new file holds.
std::uint64_t headerBytesOf(std::uint32_t shard) {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, defaultLogFileBytes);
    const MadeLogFile made = housekeeping.first(shard, 0);
    return fileOf(storage, made.name).bytes.size();
}

// Files of this shard that hold exactly records records, with bytes to spare short of one more.
std::uint64_t fileBytesFor(std::uint32_t shard, std::uint64_t records) {
    return headerBytesOf(shard) + records * replay::logRecordBytes + replay::logRecordBytes - 1;
}

void testFileNames() {
    expect(logFileName(0, 0) == "log-0000-00000000000000000000.oblog", "shard 0's first file");
    expect(logFileName(3, 12) == "log-0003-00000000000000000012.oblog", "four digits of shard and twenty of position");
    expect(logFileName(9999, 18446744073709551615ULL) == "log-9999-18446744073709551615.oblog", "the largest of each");
}

// A file holds its header and as many whole records as fit, so a full one ends on a record.
void testAFileHoldsWholeRecords() {
    const InstrumentDirectory directory = twoShards();
    const std::uint64_t header1 = headerBytesOf(1);
    const std::uint64_t header0 = headerBytesOf(0);
    expect(header1 == header0 + 8, "a header grows by a symbol's 8 bytes: " + std::to_string(header0) + " and " + std::to_string(header1));
    const LogFileShape shape = logFileShape(directory, 1, Durability::PROCESS, header1 + 3 * replay::logRecordBytes + 39);
    expect(shape.headerBytes == header1 && shape.records == 3, "three records, and 39 bytes too few for a fourth");
    expect(shape.shard == 1 && shape.durability == Durability::PROCESS, "the shard's, at its level");
    expect(logFileShape(directory, 1, Durability::MACHINE, header1 + replay::logRecordBytes).records == 1, "the smallest file holds one");
    expect(leastLogFileBytes(directory) == header1 + replay::logRecordBytes, "which is the largest header and one record");
    try {
        static_cast<void>(logFileShape(directory, 1, Durability::MACHINE, header1 + replay::logRecordBytes - 1));
        expect(false, "a file with no room for a record is refused");
    } catch (const std::invalid_argument&) {
    }
    try {
        InMemoryLogStorage unused;
        const Housekeeping housekeeping(unused, directory, Durability::MACHINE, header0 + replay::logRecordBytes);
        expect(false, "housekeeping is refused a size one shard can't use, though another can");
    } catch (const std::invalid_argument&) {
    }
    expect(defaultLogFileBytes == 256 * 1024 * 1024, "256 MiB by default");
}

// A file is made under a temporary name, header written, only extended, flushed, then renamed.
void testMakingAFile() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    const LogFileShape shape = logFileShape(directory, 1, Durability::MACHINE, fileBytesFor(1, 3));
    const MadeLogFile made = makeLogFile(storage, shape, 12);
    expect(made.name == "log-0001-00000000000000000012.oblog" && made.firstPosition == 12, "named for its first position");
    expect(storage.names() == std::vector<std::string>{made.name}, "under that name alone, with no temporary one left");
    const InMemoryLogStorage::File file = fileOf(storage, made.name);
    expect(file.writes == 1 && file.extends == 1 && file.flushes == 1 && file.durability == Durability::MACHINE,
           "its header written once, extended, and flushed, at its level");
    expect(file.renames == 1, "then renamed from its temporary name");
    expect(file.size == shape.headerBytes + 3 * replay::logRecordBytes, "extended to its last record exactly");
    expect(file.bytes.size() == shape.headerBytes, "with nothing past the header written");
    const Decoded decoded = decode(file.bytes);
    expect(!decoded.error && decoded.header.firstPosition == 12 && decoded.header.shardIndex == 1, "a header naming its position");
    expect(made.headerBytes == shape.headerBytes && made.records == 3, "the made file knows where its records go");

    try {
        static_cast<void>(makeLogFile(storage, shape, 12));
        expect(false, "a name already taken is never reused");
    } catch (const LogStorageError& error) {
        expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("it is refused as existing: ") + error.what());
    }
    expect(storage.names() == std::vector<std::string>{made.name}, "and the refusal leaves no temporary file");

    for (const bool failCreate : {true, false}) {
        InMemoryLogStorage failing;
        if (failCreate) {
            failing.failCreateAfter(0);
        } else {
            failing.failWriteAfter(0);
        }
        try {
            static_cast<void>(makeLogFile(failing, shape, 0));
            expect(false, "a failing storage fails the make");
        } catch (const LogStorageError& error) {
            expect(error.kind() == LogStorageError::Kind::FAILED, "as a failure");
        }
        expect(failing.names().empty(), std::string("a failed ") + (failCreate ? "create" : "header write") + " leaves no file");
    }
}

// A new file holds only its header, naming the shard's placement, until anything is written.
void testANewFileHoldsItsPlacement() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::PROCESS, defaultLogFileBytes);
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    const LogWriter writer(housekeeping, 1, ring, durable);
    expect(writer.fileName() == "log-0001-00000000000000000000.oblog", "the file is named for its shard and first position");
    const InMemoryLogStorage::File file = fileOf(storage, writer.fileName());
    expect(file.durability == Durability::PROCESS, "with the durability asked for");
    const Decoded decoded = decode(file.bytes);
    expect(!decoded.error, "a header alone reads as an empty log");
    expect(decoded.records.empty(), "with no records");
    expect(decoded.header.shardCount == 2 && decoded.header.shardIndex == 1, "naming the shard count and index");
    expect(decoded.header.symbols == std::vector<Symbol>{*Symbol::parse("AAPL"), *Symbol::parse("MSFT")}, "and the shard's symbols, sorted");
    expect(decoded.header.firstPosition == 0, "starting at position 0");
    expect(decoded.header.engineMajor == 2, "with the engine's version");
    expect(file.size == file.bytes.size() + (defaultLogFileBytes - file.bytes.size()) / replay::logRecordBytes * replay::logRecordBytes,
           "extended to 256 MiB's worth of whole records");
    expect(durable.load() == 0, "and nothing is durable yet");
}

// Each pass is one write of whatever queued, and the durable position moves past it.
void testAPassWritesWhatQueued() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, defaultLogFileBytes);
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(housekeeping, 1, ring, durable);
    const std::size_t headerWrites = fileOf(storage, writer.fileName()).writes;

    expect(writer.pass() == 0, "an empty ring writes nothing");
    expect(fileOf(storage, writer.fileName()).writes == headerWrites, "and makes no write");

    ring.tryPush(entry(buy(1, 100, 5), 0));
    ring.tryPush(entry(replay::CancelCommand{1}, 1));
    ring.tryPush(entry(buy(2, 101, 7), 0));
    expect(writer.pass() == 3, "a pass writes all three");
    expect(fileOf(storage, writer.fileName()).writes == headerWrites + 1, "in one write");
    expect(durable.load() == 3, "and positions 0 to 2 are durable");
    expect(writer.nextPosition() == 3, "the next record is position 3");

    ring.tryPush(entry(replay::CancelCommand{2}, 0));
    expect(writer.pass() == 1 && durable.load() == 4, "the next pass carries on");

    const Decoded decoded = decode(fileOf(storage, writer.fileName()).bytes);
    expect(!decoded.error, "the records chain from the header: " + (decoded.error ? decoded.error->message : std::string()));
    expect(decoded.records.size() == 4, "four records");
    const Symbol aapl = *Symbol::parse("AAPL");
    const Symbol msft = *Symbol::parse("MSFT");
    expect(decoded.records[0].symbol == aapl && decoded.records[0].command == replay::Command(buy(1, 100, 5)), "slot 0 is AAPL");
    expect(decoded.records[1].symbol == msft && decoded.records[1].command == replay::Command(replay::CancelCommand{1}), "slot 1 is MSFT");
    expect(decoded.records[2].command == replay::Command(buy(2, 101, 7)), "in the order taken");
    expect(decoded.records[3].sequence == 4, "the fourth at position 3, read as sequence 4");
}

// One write is at most 1 MiB, so 26,214 records of 40 bytes.
void testAWriteIsCappedAtOneMebibyte() {
    expect(LogWriter::mostRecordsPerWrite == 26214, "1 MiB holds 26,214 whole records");
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, defaultLogFileBytes);
    Ring<LogEntry> ring(32768);
    DurablePosition durable;
    LogWriter writer(housekeeping, 0, ring, durable);
    for (OrderId id = 1; id <= 30000; ++id) {
        ring.tryPush(entry(replay::CancelCommand{id}, 0));
    }
    expect(writer.pass() == 26214 && durable.load() == 26214, "the first write takes what fits in 1 MiB");
    expect(writer.pass() == 3786 && durable.load() == 30000, "the second the rest");
    const Decoded decoded = decode(fileOf(storage, writer.fileName()).bytes);
    expect(!decoded.error && decoded.records.size() == 30000, "and every record reads back");
}

// A stop writes the rest, cuts the file to its last record, flushes, and closes it.
void testAStopFinishesTheFile() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::PROCESS, defaultLogFileBytes);
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(housekeeping, 1, ring, durable);
    const InMemoryLogStorage::File made = fileOf(storage, writer.fileName());
    const std::size_t headerBytes = made.bytes.size();
    ring.tryPush(entry(buy(1, 100, 5), 0));
    ring.tryPush(entry(buy(2, 100, 5), 1));
    expect(writer.pass() == 2 && !writer.stopped(), "records alone don't stop the writer");
    expect(fileOf(storage, writer.fileName()).flushes == made.flushes, "nor flush a file at process level");
    ring.tryPush(entry(replay::CancelCommand{2}, 1));
    ring.tryPush(LogEntry::stop());
    expect(writer.pass() == 1 && writer.stopped(), "the last pass writes the rest and stops");
    expect(durable.load() == 3, "with everything durable");
    const InMemoryLogStorage::File file = fileOf(storage, writer.fileName());
    expect(file.flushes == made.flushes + 1 && file.cuts == 1 && file.closed, "cut, flushed, and closed");
    expect(file.size == headerBytes + 3 * replay::logRecordBytes && file.bytes.size() == file.size,
           "the file ends at its last record, with no zero tail");
    const Decoded decoded = decode(file.bytes);
    expect(!decoded.error && decoded.records.size() == 3, "and reads back whole, with no torn tail");
    expect(writer.pass() == 0, "a stopped writer writes nothing more");
}

// Old files are never reused, since their stale records still chain.
void testAnExistingFileIsRefused() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, defaultLogFileBytes);
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    const LogWriter first(housekeeping, 0, ring, durable);
    try {
        const LogWriter second(housekeeping, 0, ring, durable);
        expect(false, "a second writer can't take the same file");
    } catch (const LogStorageError& error) {
        expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("it is refused as existing: ") + error.what());
    }
}

// Housekeeping keeps one next file made per shard whose log has started, and taking it asks for
// the one after.
void testHousekeepingKeepsOneNextFile() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(0, 4));
    expect(housekeeping.pass() == 0, "nothing is wanted before a log starts");
    const MadeLogFile first = housekeeping.first(0, 100);
    expect(first.name == logFileName(0, 100) && first.records == 4, "the first file, of four records");
    expect(!housekeeping.takeNext(0), "and no next file until housekeeping makes one");
    expect(housekeeping.pass() == 1, "which one pass does, for the one shard started");
    expect(housekeeping.pass() == 0, "and only one");
    std::optional<MadeLogFile> next = housekeeping.takeNext(0);
    expect(next && next->name == logFileName(0, 104) && next->firstPosition == 104, "starting where the first file's room ends");
    expect(!housekeeping.takeNext(0), "taken once");
    expect(housekeeping.pass() == 1, "taking it asks for the one after");
    housekeeping.removeUnused();
    expect(storage.names() == std::vector<std::string>{logFileName(0, 100), logFileName(0, 104)},
           "and removing the unused one leaves the files taken: " + std::to_string(storage.names().size()));
    expect(housekeeping.pass() == 1, "a removed next file is wanted again, so only a stopped engine removes it");
}

// A writer that fills a file flushes and closes it at once, and goes on in the next file, which
// chains from its own header; with no next file made yet, it waits.
void testAWriterRollsAcrossFiles() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(1, 3));
    Ring<LogEntry> ring(16);
    DurablePosition durable;
    LogWriter writer(housekeeping, 1, ring, durable);
    housekeeping.pass();
    for (OrderId id = 1; id <= 7; ++id) {
        ring.tryPush(entry(buy(id, 100, 1), static_cast<std::uint32_t>(id % 2)));
    }
    const std::string firstName = logFileName(1, 0);
    expect(writer.pass() == 3 && durable.load() == 3, "a pass writes no more than its file has room for");
    const InMemoryLogStorage::File full = fileOf(storage, firstName);
    expect(full.closed && full.flushes == 2, "and flushes and closes the file it filled, before any write to the next");
    expect(full.bytes.size() == full.size, "which it filled to its end");

    expect(writer.pass() == 3 && durable.load() == 6, "the next pass takes the next file and fills it");
    expect(writer.fileName() == logFileName(1, 3), "named for position 3");
    expect(writer.pass() == 0 && writer.waitingForFile() && durable.load() == 6, "then waits, since no file after it is made");
    housekeeping.pass();
    expect(writer.pass() == 1 && !writer.waitingForFile() && durable.load() == 7, "and goes on once housekeeping makes one");

    ring.tryPush(LogEntry::stop());
    writer.pass();
    expect(writer.stopped(), "a stop finishes the last file");
    housekeeping.pass();
    housekeeping.removeUnused();
    const std::vector<std::string> names{logFileName(1, 0), logFileName(1, 3), logFileName(1, 6)};
    expect(storage.names() == names, "leaving three files, and none unused");
    std::uint64_t position = 0;
    for (const std::string& name : names) {
        const InMemoryLogStorage::File file = fileOf(storage, name);
        expect(file.closed && file.bytes.size() == file.size, name + " is closed with no zero tail");
        const Decoded decoded = decode(file.bytes);
        expect(!decoded.error, name + " reads back whole, chained from its own header: " + (decoded.error ? decoded.error->message : ""));
        expect(decoded.header.firstPosition == position, name + " starts where the last ended");
        for (const replay::ReplayRecord& record : decoded.records) {
            expect(record.sequence == ++position, name + " holds the next positions");
            expect(record.command == replay::Command(buy(static_cast<OrderId>(position), 100, 1)), name + " holds the next commands");
        }
    }
    expect(position == 7, "every record, once");
}

// A stop on a file just filled has nothing to finish, and needs no next file.
void testAStopOnAFullFile() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::PROCESS, fileBytesFor(0, 2));
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(housekeeping, 0, ring, durable);
    housekeeping.pass();
    ring.tryPush(entry(replay::CancelCommand{1}, 0));
    ring.tryPush(entry(replay::CancelCommand{2}, 0));
    ring.tryPush(LogEntry::stop());
    expect(writer.pass() == 2 && !writer.stopped(), "the file fills before the stop is read");
    expect(writer.pass() == 0 && writer.stopped() && !writer.waitingForFile(), "then the stop finishes, taking no next file");
    const InMemoryLogStorage::File file = fileOf(storage, logFileName(0, 0));
    expect(file.closed && file.cuts == 0 && file.flushes == 2, "the full file was flushed once, at process level too, and never cut");
    expect(storage.names().size() == 2, "the next file is still there");
    housekeeping.removeUnused();
    expect(storage.names() == std::vector<std::string>{logFileName(0, 0)}, "until the clean stop removes it");
}

// The writer and housekeeping each on a thread of its own: the writer sleeps on its doorbell or
// waits for its next file, and housekeeping sleeps until a file is taken.
void testRealThreads() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, fileBytesFor(1, 5));
    Ring<LogEntry> ring(4);
    DurablePosition durable;
    Doorbell doorbell;
    LogWriter writer(housekeeping, 1, ring, durable);
    std::thread housekeeper([&housekeeping] { housekeeping.run(); });
    std::thread writing([&] { writer.run(doorbell, std::chrono::nanoseconds::zero()); });
    constexpr OrderId commands = 2000;
    for (OrderId id = 1; id <= commands; ++id) {
        while (!ring.tryPush(entry(replay::CancelCommand{id}, static_cast<std::uint32_t>(id % 2)))) {
            doorbell.ring();
            std::this_thread::yield();
        }
        doorbell.ring();
    }
    while (!ring.tryPush(LogEntry::stop())) {
        doorbell.ring();
        std::this_thread::yield();
    }
    doorbell.ring();
    writing.join();
    housekeeping.stop();
    housekeeper.join();
    housekeeping.removeUnused();
    expect(durable.load() == commands, "every command is durable");
    const std::vector<std::string> names = storage.names();
    expect(names.size() == commands / 5, "in files of five: " + std::to_string(names.size()));
    std::uint64_t position = 0;
    for (const std::string& name : names) {
        const InMemoryLogStorage::File file = fileOf(storage, name);
        const Decoded decoded = decode(file.bytes);
        expect(file.closed && !decoded.error && decoded.header.firstPosition == position && decoded.records.size() == 5,
               name + " is whole and starts where the last ended");
        for (const replay::ReplayRecord& record : decoded.records) {
            expect(record.command == replay::Command(replay::CancelCommand{static_cast<OrderId>(++position)}), "in order");
        }
    }
}

std::vector<std::byte> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return bytes;
}

// The real files at both levels: what was written reads back, an extended file reads as zeros
// past its writes, a cut shortens the file, a rename moves an open file and never over another,
// an existing file is refused, and remove deletes one.
void testRealFiles() {
    const fs::path directory = fs::temp_directory_path() / "order_book_log_writer_tests";
    fs::remove_all(directory);
    fs::create_directories(directory);
    FileLogStorage storage(directory);
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        const std::string name = durability == Durability::MACHINE ? "machine.oblog" : "process.oblog";
        const std::string moved = "moved-" + name;
        std::unique_ptr<LogFile> file = storage.create(name, durability);
        const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
        file->write(0, bytes);
        file->extend(64 * 1024);
        file->write(5, std::span<const std::byte>(bytes).first(2));
        file->flush();
        file->rename(moved);
        expect(!fs::exists(directory / name) && fs::exists(directory / moved), name + " moves while open");
        file->write(7, std::span<const std::byte>(bytes).first(1));
        file->close();
        try {
            file->write(0, bytes);
            expect(false, "a closed file can't be written");
        } catch (const LogStorageError& error) {
            expect(std::string(error.what()).find((directory / moved).string()) != std::string::npos,
                   std::string("and a failure names the file by its new name: ") + error.what());
        }
        std::vector<std::byte> extended = readAll(directory / moved);
        expect(extended.size() == 64 * 1024, name + " is as long as it was extended to: " + std::to_string(extended.size()));
        const std::vector<std::byte> written{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{1}, std::byte{2},
                                             std::byte{1}};
        expect(std::vector<std::byte>(extended.begin(), extended.begin() + 8) == written, name + " holds what was written, after the move too");
        bool zeros = true;
        for (std::size_t i = 8; i < extended.size(); ++i) {
            zeros = zeros && extended[i] == std::byte{0};
        }
        expect(zeros, name + " reads as zeros past what was written");

        file = storage.create(name, durability);
        file->write(0, bytes);
        file->cut(3);
        try {
            file->rename(moved);
            expect(false, "a rename never replaces a file");
        } catch (const LogStorageError& error) {
            expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("it is refused as existing: ") + error.what());
        }
        file->close();
        expect(readAll(directory / name) == std::vector<std::byte>(bytes.begin(), bytes.begin() + 3), name + " is cut after three bytes");
        expect(readAll(directory / moved).size() == 64 * 1024, "and leaves both files as they were");
        expect(storage.describe(name) == (directory / name).string(), "a file is described by its path");
        try {
            static_cast<void>(storage.create(name, durability));
            expect(false, "an existing file is never reused");
        } catch (const LogStorageError& error) {
            expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("and is refused as existing: ") + error.what());
        }
        storage.remove(name);
        storage.remove(moved);
        expect(!fs::exists(directory / name) && !fs::exists(directory / moved), "remove deletes each");
    }
    fs::remove_all(directory);
    try {
        static_cast<void>(storage.create("gone.oblog", Durability::MACHINE));
        expect(false, "a file in a missing directory can't be made");
    } catch (const LogStorageError& error) {
        expect(error.kind() == LogStorageError::Kind::FAILED, "and that is a failure, not an existing file");
    }
}

// A log writer's real files: an engine that never stopped cleanly leaves an extended file that
// reads as its records and then a torn tail, which recovery cuts, and a clean stop leaves files
// that read whole, the full one included.
void testRealLogFiles() {
    const fs::path path = fs::temp_directory_path() / "order_book_log_writer_tests_logs";
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        for (const bool stop : {false, true}) {
            fs::remove_all(path);
            fs::create_directories(path);
            const InstrumentDirectory directory = twoShards();
            FileLogStorage storage(path);
            Housekeeping housekeeping(storage, directory, durability, fileBytesFor(0, 4));
            std::string current;
            {
                Ring<LogEntry> ring(16);
                DurablePosition durable;
                LogWriter writer(housekeeping, 0, ring, durable);
                housekeeping.pass();
                for (OrderId id = 1; id <= 6; ++id) {
                    ring.tryPush(entry(replay::CancelCommand{id}, 0));
                }
                while (writer.pass() != 0) {
                }
                expect(durable.load() == 6, "six records written, across two files");
                if (stop) {
                    ring.tryPush(LogEntry::stop());
                    writer.pass();
                    expect(writer.stopped(), "and stopped");
                }
                current = writer.fileName();
            }  // a writer left unstopped closes its file as a crash would leave it
            housekeeping.removeUnused();
            const std::vector<std::byte> fullFile = readAll(path / logFileName(0, 0));
            const Decoded full = decode(fullFile);
            expect(!full.error && full.records.size() == 4, "a full file reads whole, ending on its last record");
            const std::vector<std::byte> bytes = readAll(path / current);
            const Decoded last = decode(bytes);
            expect(last.header.firstPosition == 4 && last.records.size() == 2, "the last file holds the other two");
            if (stop) {
                expect(!last.error && bytes.size() == headerBytesOf(0) + 2 * replay::logRecordBytes,
                       "cut to its last record by the clean stop, so it reads with no error");
            } else {
                expect(bytes.size() == headerBytesOf(0) + 4 * replay::logRecordBytes, "extended to four records");
                bool zeros = true;
                for (std::size_t i = static_cast<std::size_t>(headerBytesOf(0)) + 2 * replay::logRecordBytes; i < bytes.size(); ++i) {
                    zeros = zeros && bytes[i] == std::byte{0};
                }
                expect(zeros, "reading as zeros past its writes");
                expect(last.error && last.error->code == replay::ParseErrorCode::TORN_TAIL,
                       "which reads as a torn tail: " + (last.error ? last.error->message : std::string("no error")));
            }
            std::size_t files = 0;
            for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
                ++files;
                expect(entry.path().extension() == ".oblog", "no temporary file is left: " + entry.path().filename().string());
            }
            expect(files == 2, "and no unused next file");
        }
    }
    fs::remove_all(path);
}

[[noreturn]] void runFailingWriter(std::string_view what) {
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Housekeeping housekeeping(storage, directory, Durability::MACHINE, defaultLogFileBytes);
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(housekeeping, 0, ring, durable);
    if (what == "next-file") {
        storage.failCreateAfter(0);
        housekeeping.pass();
    } else if (what == "flush") {
        storage.failFlushes();
        ring.tryPush(LogEntry::stop());
    } else {
        storage.failWriteAfter(0);
        ring.tryPush(entry(replay::CancelCommand{1}, 0));
    }
    writer.pass();
    // Exit 0, which expect_failure.cmake refuses, since any other failure would pass for the
    // abort: a failure that printed its message and carried on would look the same.
    std::cerr << "FAILED: a failed " << what << " should have aborted" << std::endl;
    std::exit(EXIT_SUCCESS);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]).starts_with("abort-")) {
        runFailingWriter(std::string_view(argv[1]).substr(6));
    }
    testFileNames();
    testAFileHoldsWholeRecords();
    testMakingAFile();
    testANewFileHoldsItsPlacement();
    testAPassWritesWhatQueued();
    testAWriteIsCappedAtOneMebibyte();
    testAStopFinishesTheFile();
    testAnExistingFileIsRefused();
    testHousekeepingKeepsOneNextFile();
    testAWriterRollsAcrossFiles();
    testAStopOnAFullFile();
    testRealThreads();
    testRealFiles();
    testRealLogFiles();
    std::cout << "log writer tests passed" << std::endl;
    return EXIT_SUCCESS;
}
