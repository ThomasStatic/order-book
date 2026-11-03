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
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

#include "order_book/command_log.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"

// The log writer as a plain object over the in-memory LogStorage, and the real files under it.
// What it writes is read back through CommandLogReader, never compared with its own encoder.
// Given "abort-write" or "abort-flush" it runs a writer whose storage fails, which must abort
// naming the shard and file, as CTest checks through tests/expect_failure.cmake. See
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

void testFileNames() {
    expect(logFileName(0, 0) == "log-0000-00000000000000000000.oblog", "shard 0's first file");
    expect(logFileName(3, 12) == "log-0003-00000000000000000012.oblog", "four digits of shard and twenty of position");
    expect(logFileName(9999, 18446744073709551615ULL) == "log-9999-18446744073709551615.oblog", "the largest of each");
}

// A new file holds only its header, naming the shard's placement, until anything is written.
void testANewFileHoldsItsPlacement() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    const LogWriter writer(storage, directory, 1, Durability::PROCESS, ring, durable);
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
    expect(durable.load() == 0, "and nothing is durable yet");
}

// Each pass is one write of whatever queued, and the durable position moves past it.
void testAPassWritesWhatQueued() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(storage, directory, 1, Durability::MACHINE, ring, durable);
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
    Ring<LogEntry> ring(32768);
    DurablePosition durable;
    LogWriter writer(storage, directory, 0, Durability::MACHINE, ring, durable);
    for (OrderId id = 1; id <= 30000; ++id) {
        ring.tryPush(entry(replay::CancelCommand{id}, 0));
    }
    expect(writer.pass() == 26214 && durable.load() == 26214, "the first write takes what fits in 1 MiB");
    expect(writer.pass() == 3786 && durable.load() == 30000, "the second the rest");
    const Decoded decoded = decode(fileOf(storage, writer.fileName()).bytes);
    expect(!decoded.error && decoded.records.size() == 30000, "and every record reads back");
}

// A stop writes the rest, flushes, cuts the file to its last record, and closes it.
void testAStopFinishesTheFile() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(storage, directory, 1, Durability::PROCESS, ring, durable);
    const std::size_t headerBytes = fileOf(storage, writer.fileName()).bytes.size();
    ring.tryPush(entry(buy(1, 100, 5), 0));
    ring.tryPush(entry(buy(2, 100, 5), 1));
    expect(writer.pass() == 2 && !writer.stopped(), "records alone don't stop the writer");
    expect(fileOf(storage, writer.fileName()).flushes == 0, "nor flush a file at process level");
    ring.tryPush(entry(replay::CancelCommand{2}, 1));
    ring.tryPush(LogEntry::stop());
    expect(writer.pass() == 1 && writer.stopped(), "the last pass writes the rest and stops");
    expect(durable.load() == 3, "with everything durable");
    const InMemoryLogStorage::File file = fileOf(storage, writer.fileName());
    expect(file.flushes == 1 && file.cuts == 1 && file.closed, "flushed, cut, and closed");
    expect(file.bytes.size() == headerBytes + 3 * replay::logRecordBytes, "the file ends at its last record");
    const Decoded decoded = decode(file.bytes);
    expect(!decoded.error && decoded.records.size() == 3, "and reads back whole, with no torn tail");
    expect(writer.pass() == 0, "a stopped writer writes nothing more");
}

// Old files are never reused, since their stale records still chain.
void testAnExistingFileIsRefused() {
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    const LogWriter first(storage, directory, 0, Durability::MACHINE, ring, durable);
    try {
        const LogWriter second(storage, directory, 0, Durability::MACHINE, ring, durable);
        expect(false, "a second writer can't take the same file");
    } catch (const LogStorageError& error) {
        expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("it is refused as existing: ") + error.what());
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

// The real files at both levels: what was written reads back, a cut shortens the file, an
// existing file is refused, and remove deletes one.
void testRealFiles() {
    const fs::path directory = fs::temp_directory_path() / "order_book_log_writer_tests";
    fs::remove_all(directory);
    fs::create_directories(directory);
    FileLogStorage storage(directory);
    for (const Durability durability : {Durability::MACHINE, Durability::PROCESS}) {
        const std::string name = durability == Durability::MACHINE ? "machine.oblog" : "process.oblog";
        std::unique_ptr<LogFile> file = storage.create(name, durability);
        const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
        file->write(0, bytes);
        file->write(5, std::span<const std::byte>(bytes).first(2));
        file->flush();
        file->cut(6);
        file->close();
        const std::vector<std::byte> expected{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{1}};
        expect(readAll(directory / name) == expected, name + " holds what was written, cut after six bytes");
        expect(storage.describe(name) == (directory / name).string(), "a file is described by its path");
        try {
            static_cast<void>(storage.create(name, durability));
            expect(false, "an existing file is never reused");
        } catch (const LogStorageError& error) {
            expect(error.kind() == LogStorageError::Kind::EXISTS, std::string("and is refused as existing: ") + error.what());
        }
        storage.remove(name);
        expect(!fs::exists(directory / name), "remove deletes it");
    }
    fs::remove_all(directory);
    try {
        static_cast<void>(storage.create("gone.oblog", Durability::MACHINE));
        expect(false, "a file in a missing directory can't be made");
    } catch (const LogStorageError& error) {
        expect(error.kind() == LogStorageError::Kind::FAILED, "and that is a failure, not an existing file");
    }
}

[[noreturn]] void runFailingWriter(bool flush) {
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const InstrumentDirectory directory = twoShards();
    InMemoryLogStorage storage;
    Ring<LogEntry> ring(8);
    DurablePosition durable;
    LogWriter writer(storage, directory, 0, Durability::MACHINE, ring, durable);
    if (flush) {
        storage.failFlushes();
        ring.tryPush(LogEntry::stop());
    } else {
        storage.failWriteAfter(0);
        ring.tryPush(entry(replay::CancelCommand{1}, 0));
    }
    writer.pass();
    std::cerr << "FAILED: a failed log " << (flush ? "flush" : "write") << " should have aborted" << std::endl;
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "abort-write") {
        runFailingWriter(false);
    }
    if (argc == 2 && std::string_view(argv[1]) == "abort-flush") {
        runFailingWriter(true);
    }
    testFileNames();
    testANewFileHoldsItsPlacement();
    testAPassWritesWhatQueued();
    testAWriteIsCappedAtOneMebibyte();
    testAStopFinishesTheFile();
    testAnExistingFileIsRefused();
    testRealFiles();
    std::cout << "log writer tests passed" << std::endl;
    return EXIT_SUCCESS;
}
