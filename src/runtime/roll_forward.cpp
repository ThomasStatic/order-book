#include "order_book/runtime/roll_forward.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

#include "log_file_reader.hpp"
#include "order_book/command.hpp"
#include "order_book/order_book.hpp"

namespace order_book::runtime {

    namespace {
        constexpr std::string_view prefix = "ckpt-";
        constexpr std::string_view extension = ".obckpt";
        constexpr std::string_view temporarySuffix = ".tmp";
        constexpr std::size_t positionDigits = 20;

        // A pass given up on for something other than memory, which the next roll makes again:
        // a record or a checkpoint that no longer reads. Reported, not counted.
        struct PassAbandoned {
            std::string why;
        };

        std::vector<std::byte> imageOf(const OrderBook& book) {
            std::vector<std::byte> image(book.imageSize());
            static_cast<void>(book.writeImage(image));
            return image;
        }

        std::vector<std::byte> readWhole(LogStorage& storage, const std::string& name) {
            const std::unique_ptr<LogFile> file = storage.open(name);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
            std::size_t got = 0;
            while (got < bytes.size()) {
                const std::size_t chunk = file->read(got, std::span<std::byte>(bytes).subspan(got));
                if (chunk == 0) {
                    break;
                }
                got += chunk;
            }
            bytes.resize(got);
            return bytes;
        }

        CheckpointHeader headerFor(const Symbol& symbol, std::uint32_t shardCount, std::uint32_t shard, std::uint64_t position) {
            CheckpointHeader header;
            header.engineMajor = ORDER_BOOK_VERSION_MAJOR;
            header.engineMinor = ORDER_BOOK_VERSION_MINOR;
            header.enginePatch = ORDER_BOOK_VERSION_PATCH;
            header.shardCount = shardCount;
            header.shardIndex = shard;
            header.position = position;
            header.symbol = symbol;
            return header;
        }
    }  // namespace

    std::string checkpointFileName(const Symbol& symbol, std::uint64_t position) {
        char digits[positionDigits + 1];
        std::snprintf(digits, sizeof(digits), "%020llu", static_cast<unsigned long long>(position));
        return std::string(prefix) + symbol.text() + "-" + digits + std::string(extension);
    }

    std::optional<CheckpointFileNameParts> parseCheckpointFileName(std::string_view name) {
        CheckpointFileNameParts parts{};
        parts.temporary = name.ends_with(temporarySuffix);
        if (parts.temporary) {
            name.remove_suffix(temporarySuffix.size());
        }
        if (!name.starts_with(prefix) || !name.ends_with(extension)) {
            return std::nullopt;
        }
        name.remove_prefix(prefix.size());
        name.remove_suffix(extension.size());
        if (name.size() < positionDigits + 2 || name[name.size() - positionDigits - 1] != '-') {
            return std::nullopt;
        }
        const std::string_view digits = name.substr(name.size() - positionDigits);
        std::uint64_t position = 0;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            const auto value = static_cast<std::uint64_t>(digit - '0');
            if (position > (std::numeric_limits<std::uint64_t>::max() - value) / 10) {
                return std::nullopt;
            }
            position = position * 10 + value;
        }
        const std::optional<Symbol> symbol = Symbol::parse(name.substr(0, name.size() - positionDigits - 1));
        if (!symbol) {
            return std::nullopt;
        }
        parts.symbol = *symbol;
        parts.position = position;
        return parts;
    }

    std::optional<std::string> proveImage(std::span<const std::byte> image) {
        std::variant<OrderBook, RestoreError> restored = OrderBook::restore(image);
        if (const auto* error = std::get_if<RestoreError>(&restored)) {
            return std::string("restore refused it: ") + restoreErrorCodeName(error->code) + " at " +
                   std::to_string(error->offset) + ": " + error->message;
        }
        const OrderBook& book = std::get<OrderBook>(restored);
        const InvariantResult invariants = book.checkInvariants();
        if (!invariants.isValid()) {
            return "the restored book fails checkInvariants: " + formatInvariantResult(invariants);
        }
        const std::vector<std::byte> again = imageOf(book);
        if (!std::equal(image.begin(), image.end(), again.begin(), again.end())) {
            const auto first = std::mismatch(image.begin(), image.end(), again.begin(), again.end());
            return "the image written again differs from it at byte " + std::to_string(first.first - image.begin()) + ", " +
                   std::to_string(again.size()) + " bytes against " + std::to_string(image.size());
        }
        return std::nullopt;
    }

    void requireProvedImage(std::span<const std::byte> image, std::string_view what) {
        if (const std::optional<std::string> failure = proveImage(image)) {
            std::fprintf(stderr, "order book runtime: roll forward: %.*s: the image proof failed: %s\n", static_cast<int>(what.size()),
                         what.data(), failure->c_str());
            std::fflush(stderr);
            std::abort();
        }
    }

    void publishCheckpoint(LogStorage& storage, const CheckpointHeader& header, std::span<const std::byte> image) {
        const std::string name = checkpointFileName(header.symbol, header.position);
        const std::string temporary = name + std::string(temporarySuffix);
        const std::vector<std::byte> bytes = encodeCheckpoint(header, image);
        std::unique_ptr<LogFile> file;
        try {
            file = storage.create(temporary, Durability::PROCESS);
        } catch (const LogStorageError& error) {
            if (error.kind() != LogStorageError::Kind::EXISTS) {
                throw;
            }
            // Only this engine, which holds the directory's lock, makes a .tmp here, so one
            // standing is its own from a publish an exception cut short.
            storage.remove(temporary);
            file = storage.create(temporary, Durability::PROCESS);
        }
        try {
            file->write(0, bytes);
            file->flush();
            // Proved as the file holds it: what reads back must decode to what was meant.
            std::vector<std::byte> back(bytes.size());
            const std::size_t got = file->read(0, back);
            const auto decoded = decodeCheckpoint(std::span<const std::byte>(back).first(got));
            const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
            if (got != bytes.size() || checkpoint == nullptr || !(checkpoint->header == header) ||
                !std::equal(checkpoint->image.begin(), checkpoint->image.end(), image.begin(), image.end())) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot publish " + name + ": " + temporary +
                                                                         " does not read back as written");
            }
            file->rename(name);
        } catch (...) {
            try {
                file->close();
                storage.remove(temporary);
            } catch (const LogStorageError&) {
                // The start removes a leftover .tmp.
            }
            throw;
        }
        // Published once renamed: a close that fails changes nothing the drive holds.
        try {
            file->close();
        } catch (...) {
        }
    }

    ShardRoller::ShardRoller(LogStorage& logStorage, const InstrumentDirectory& instruments, std::uint32_t shardIndex, Report reportTo)
        : storage(logStorage), directory(instruments), shard(shardIndex), report(std::move(reportTo)),
          kept(directory.onShard(shardIndex).size()), firstListing(kept.size()) {
        // Each book's positions, newest first; a .tmp a crash left is removed, as at any start.
        std::vector<std::vector<std::uint64_t>> positions(kept.size());
        for (const std::string& name : storage.names()) {
            const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
            if (!parts) {
                continue;
            }
            const std::optional<BookLocation> location = directory.find(parts->symbol);
            if (!location || location->shard != shard) {
                continue;
            }
            if (parts->temporary) {
                storage.remove(name);
                continue;
            }
            positions[location->slot].push_back(parts->position);
        }
        // The two newest that decode as their names say.
        for (std::size_t slot = 0; slot < kept.size(); ++slot) {
            std::sort(positions[slot].rbegin(), positions[slot].rend());
            const Symbol& symbol = directory.onShard(shard)[slot].symbol;
            for (const std::uint64_t position : positions[slot]) {
                const std::vector<std::byte> file = readWhole(storage, checkpointFileName(symbol, position));
                const auto decoded = decodeCheckpoint(file);
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                if (checkpoint == nullptr || checkpoint->header.symbol != symbol || checkpoint->header.position != position) {
                    continue;
                }
                (kept[slot].newest ? kept[slot].older : kept[slot].newest) = position;
                if (kept[slot].older) {
                    break;
                }
            }
            // Two are kept: anything older, which a removal a crash undid can leave, goes.
            for (const std::uint64_t position : positions[slot]) {
                if (kept[slot].older && position < *kept[slot].older) {
                    storage.remove(checkpointFileName(symbol, position));
                }
            }
        }
    }

    void ShardRoller::removeSuperseded(std::uint32_t slot) {
        Kept& held = kept[slot];
        if (!held.superseded) {
            return;
        }
        const std::string name = checkpointFileName(directory.onShard(shard)[slot].symbol, *held.superseded);
        try {
            storage.remove(name);
            held.superseded.reset();
        } catch (const LogStorageError& error) {
            held.superseded.reset();  // left for the next start to remove
            report("shard " + std::to_string(shard) + ": cannot remove " + name + ": " + error.what());
        }
    }

    std::optional<std::uint64_t> ShardRoller::position(std::uint32_t slot) const {
        return kept.at(slot).newest;
    }

    void ShardRoller::readLogFiles() {
        for (const std::string& name : storage.names()) {
            const std::optional<LogFileNameParts> parts = parseLogFileName(name);
            if (!parts || parts->temporary || parts->shard != shard) {
                continue;
            }
            const bool known = std::any_of(files.begin(), files.end(), [&name](const LogFileInfo& file) { return file.name == name; });
            if (known) {
                continue;
            }
            const log_reading::FileHeader header = log_reading::readHeader(storage, name);
            const auto* parsed = std::get_if<replay::LogHeader>(&header.parsed);
            if (parsed == nullptr) {
                throw PassAbandoned{"cannot read " + name + "'s header: " + std::get<replay::ParseError>(header.parsed).message};
            }
            LogFileInfo file;
            file.name = name;
            file.firstPosition = parsed->firstPosition;
            file.headerBytes = header.bytes;
            file.shardCount = parsed->shardCount;
            for (const Symbol& symbol : parsed->symbols) {
                const std::optional<BookLocation> location = directory.find(symbol);
                if (location && location->shard == shard) {
                    file.slots.push_back(location->slot);
                }
            }
            const auto at = std::lower_bound(files.begin(), files.end(), file.firstPosition,
                                             [](const LogFileInfo& held, std::uint64_t first) { return held.firstPosition < first; });
            files.insert(at, std::move(file));
        }
        // Where a book with no checkpoint stands: the first file that lists it.
        for (std::size_t slot = 0; slot < firstListing.size(); ++slot) {
            firstListing[slot].reset();
            for (const LogFileInfo& file : files) {
                if (std::find(file.slots.begin(), file.slots.end(), slot) != file.slots.end()) {
                    firstListing[slot] = file.firstPosition;
                    break;
                }
            }
        }
    }

    bool ShardRoller::standsAt(std::uint32_t slot, const LogFileInfo& file) const {
        const std::optional<std::uint64_t> at = kept[slot].newest ? kept[slot].newest : firstListing[slot];
        return at == file.firstPosition;
    }

    std::size_t ShardRoller::roll(std::uint64_t closedEnd) {
        std::size_t made = 0;
        try {
            for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
                removeSuperseded(slot);
            }
            readLogFiles();
            std::vector<bool> needs(kept.size());
            for (std::size_t i = 0; i + 1 < files.size() && files[i + 1].firstPosition <= closedEnd; ++i) {
                const LogFileInfo& file = files[i];
                bool any = false;
                for (const std::uint32_t slot : file.slots) {
                    needs[slot] = standsAt(slot, file);
                    any = any || needs[slot];
                }
                if (!any) {
                    continue;
                }
                pass(file, files[i + 1].firstPosition, needs);
                ++made;
                std::fill(needs.begin(), needs.end(), false);
            }
        } catch (const std::bad_alloc&) {
            ++totals.passesLost;
            try {
                report("shard " + std::to_string(shard) + ": a roll-forward pass was lost to bad_alloc; the next roll makes it again");
            } catch (const std::bad_alloc&) {
                // Counted, if not said: memory is still short.
            }
        } catch (const PassAbandoned& abandoned) {
            report("shard " + std::to_string(shard) + ": roll forward stopped: " + abandoned.why);
        }
        behind = 0;
        for (std::size_t i = 0; i + 1 < files.size() && files[i + 1].firstPosition <= closedEnd; ++i) {
            // Behind if a book it lists stands before its end.
            const std::uint64_t end = files[i + 1].firstPosition;
            const bool needed = std::any_of(files[i].slots.begin(), files[i].slots.end(), [this, end](std::uint32_t slot) {
                const std::optional<std::uint64_t> at = kept[slot].newest ? kept[slot].newest : firstListing[slot];
                return at && *at < end;
            });
            behind += needed ? 1 : 0;
        }
        totals.passes += made;
        return made;
    }

    void ShardRoller::pass(const LogFileInfo& file, std::uint64_t end, const std::vector<bool>& needs) {
        const std::span<const Instrument> instruments = directory.onShard(shard);
        // The books with commands in this file, restored on first use from their newest
        // checkpoint, or empty when they have none.
        std::vector<std::optional<OrderBook>> books(kept.size());
        const auto restoreOnFirstUse = [&](std::uint32_t slot) -> OrderBook& {
            if (!books[slot]) {
                if (!kept[slot].newest) {
                    books[slot].emplace();
                } else {
                    const std::string name = checkpointFileName(instruments[slot].symbol, *kept[slot].newest);
                    const std::vector<std::byte> bytes = readWhole(storage, name);
                    const auto decoded = decodeCheckpoint(bytes);
                    const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                    std::optional<std::variant<OrderBook, RestoreError>> restored;
                    if (checkpoint != nullptr) {
                        restored.emplace(OrderBook::restore(checkpoint->image));
                    }
                    if (!restored || std::holds_alternative<RestoreError>(*restored)) {
                        // Bad since it was read: fall back to the older one, from which the next
                        // roll starts.
                        kept[slot].newest = kept[slot].older;
                        kept[slot].older.reset();
                        throw PassAbandoned{name + " no longer restores; " + instruments[slot].symbol.text() +
                                            " falls back to its older checkpoint"};
                    }
                    books[slot].emplace(std::move(std::get<OrderBook>(*restored)));
                }
            }
            return *books[slot];
        };

        const std::uint64_t records = end - file.firstPosition;
        log_reading::LogFileReader reader(storage, file.name, file.headerBytes + records * replay::logRecordBytes);
        std::uint64_t recordsRead = 0;
        for (;;) {
            replay::ReadResult result = reader.next();
            if (std::holds_alternative<replay::EndOfInput>(result)) {
                break;
            }
            if (const auto* error = std::get_if<replay::ParseError>(&result)) {
                throw PassAbandoned{"cannot read " + file.name + ": " + error->message};
            }
            const replay::ReplayRecord& record = std::get<replay::ReplayRecord>(result);
            ++recordsRead;
            const std::optional<BookLocation> location = directory.find(record.symbol);
            if (!location || location->shard != shard || !needs[location->slot]) {
                continue;
            }
            static_cast<void>(replay::executeCommand(restoreOnFirstUse(location->slot), record.command));
        }
        if (recordsRead != records) {
            throw PassAbandoned{file.name + " holds " + std::to_string(recordsRead) + " records, not " + std::to_string(records)};
        }

        // Every image first, proved, then publishing, so a bad_alloc publishes nothing.
        struct Ready {
            std::uint32_t slot;
            std::vector<std::byte> image;
        };
        std::vector<Ready> ready;
        ready.reserve(file.slots.size());
        for (const std::uint32_t slot : file.slots) {
            if (!needs[slot]) {
                continue;
            }
            const Symbol& symbol = instruments[slot].symbol;
            if (books[slot]) {
                std::vector<std::byte> image = imageOf(*books[slot]);
                books[slot].reset();
                requireProvedImage(image, "shard " + std::to_string(shard) + " " + symbol.text() + " at " + std::to_string(end));
                ready.push_back(Ready{slot, std::move(image)});
            } else if (kept[slot].newest) {
                // No command: a new wrapper around the same image, with no restore.
                const std::string name = checkpointFileName(symbol, *kept[slot].newest);
                const std::vector<std::byte> bytes = readWhole(storage, name);
                const auto decoded = decodeCheckpoint(bytes);
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                if (checkpoint == nullptr) {
                    kept[slot].newest = kept[slot].older;
                    kept[slot].older.reset();
                    throw PassAbandoned{name + " no longer decodes; " + symbol.text() + " falls back to its older checkpoint"};
                }
                ready.push_back(Ready{slot, std::vector<std::byte>(checkpoint->image.begin(), checkpoint->image.end())});
            } else {
                ready.push_back(Ready{slot, imageOf(OrderBook{})});
            }
        }

        for (const Ready& book : ready) {
            const Symbol& symbol = instruments[book.slot].symbol;
            Kept& held = kept[book.slot];
            try {
                publishCheckpoint(storage, headerFor(symbol, file.shardCount, shard, end), book.image);
            } catch (const LogStorageError& error) {
                ++totals.writesFailed;
                report("shard " + std::to_string(shard) + ": checkpoint " + checkpointFileName(symbol, end) + " not published: " +
                       error.what() + "; " + symbol.text() + " stays at its last checkpoint");
                continue;
            }
            ++totals.published;
            // Nothing allocates from here to the removal, so a book published is always one the
            // roller knows; a removal a bad_alloc cuts short stays pending for the next roll. None
            // is pending here: each roll starts by retrying them, and a bad_alloc ends a roll.
            held.superseded = held.older;
            held.older = held.newest;
            held.newest = end;
            removeSuperseded(book.slot);
        }
    }

}  // namespace order_book::runtime
