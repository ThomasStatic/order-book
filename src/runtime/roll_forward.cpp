#include "order_book/runtime/roll_forward.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
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

    void publishCheckpoint(LogStorage& storage, const CheckpointHeader& header, std::span<const std::byte> image, bool replacing) {
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
            // Proved as the file holds it, then flushed: what reads back must decode to what was
            // meant. The image itself was proved before it was written, and a failure there aborts.
            std::vector<std::byte> back(bytes.size());
            const std::size_t got = file->read(0, back);
            const auto decoded = decodeCheckpoint(std::span<const std::byte>(back).first(got));
            const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
            if (got != bytes.size() || checkpoint == nullptr || !(checkpoint->header == header) ||
                !std::equal(checkpoint->image.begin(), checkpoint->image.end(), image.begin(), image.end())) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot publish " + name + ": " + temporary +
                                                                         " does not read back as written");
            }
            file->flush();
            if (replacing) {
                file->replace(name);
            } else {
                file->rename(name);
            }
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

    void publishEmptyCheckpoint(LogStorage& storage, const Symbol& symbol, std::uint32_t shardCount, std::uint32_t shard,
                                std::uint64_t position) {
        publishCheckpoint(storage, headerFor(symbol, shardCount, shard, position), imageOf(OrderBook{}));
    }

    ShardRoller::ShardRoller(LogStorage& logStorage, const InstrumentDirectory& instruments, std::uint32_t shardIndex, Report reportTo)
        : ShardRoller(logStorage, logStorage, instruments, shardIndex, std::move(reportTo)) {}

    ShardRoller::ShardRoller(LogStorage& logStorage, LogStorage& checkpointStorage, const InstrumentDirectory& instruments,
                             std::uint32_t shardIndex, Report reportTo)
        : logs(logStorage), checkpoints(checkpointStorage), directory(instruments), shard(shardIndex), report(std::move(reportTo)),
          kept(directory.onShard(shardIndex).size()), firstListing(kept.size()), atStop(kept.size()) {
        // Each book's positions, newest first; a .tmp a crash left is removed, as at any start.
        std::vector<std::vector<std::uint64_t>> positions(kept.size());
        for (const std::string& name : checkpoints.names()) {
            const std::optional<CheckpointFileNameParts> parts = parseCheckpointFileName(name);
            if (!parts) {
                continue;
            }
            const std::optional<BookLocation> location = directory.find(parts->symbol);
            if (!location || location->shard != shard) {
                continue;
            }
            if (parts->temporary) {
                checkpoints.remove(name);
                continue;
            }
            positions[location->slot].push_back(parts->position);
        }
        // The two newest that decode as their names say.
        for (std::size_t slot = 0; slot < kept.size(); ++slot) {
            std::sort(positions[slot].rbegin(), positions[slot].rend());
            const Symbol& symbol = directory.onShard(shard)[slot].symbol;
            for (const std::uint64_t position : positions[slot]) {
                const std::vector<std::byte> file = readWhole(checkpoints, checkpointFileName(symbol, position));
                const auto decoded = decodeCheckpoint(file);
                const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                if (checkpoint == nullptr || checkpoint->header.symbol != symbol || checkpoint->header.position != position) {
                    // Its book falls back to an older one and rolls to this position again, which
                    // a file in the way would refuse for ever, since a rename never replaces one.
                    const std::string name = checkpointFileName(symbol, position);
                    checkpoints.remove(name);
                    report("shard " + std::to_string(shard) + ": " + name + " does not decode as its name says; removed, so " +
                           symbol.text() + " rolls from its older checkpoint");
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
                    checkpoints.remove(checkpointFileName(symbol, position));
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
            checkpoints.remove(name);
            held.superseded.reset();
        } catch (const LogStorageError& error) {
            held.superseded.reset();  // left for the next start to remove
            report("shard " + std::to_string(shard) + ": cannot remove " + name + ": " + error.what());
        }
    }

    std::optional<std::uint64_t> ShardRoller::position(std::uint32_t slot) const {
        return kept.at(slot).newest;
    }

    std::optional<std::uint64_t> ShardRoller::retentionFloor() const {
        std::optional<std::uint64_t> floor;
        for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
            // A book's oldest kept checkpoint, which a start falls back to if the newest fails.
            const std::optional<std::uint64_t> oldest = kept[slot].older ? kept[slot].older : standing(slot);
            if (!oldest) {
                return std::nullopt;  // a book standing nowhere yet needs every file
            }
            floor = floor ? std::min(*floor, *oldest) : *oldest;
        }
        return floor;
    }

    void ShardRoller::readLogFiles(std::uint64_t closedEnd) {
        // This shard's files by first position, from their names alone: a file still being
        // written, or made ahead, is never opened here, since Windows lets a file have one handle.
        std::vector<std::pair<std::uint64_t, std::string>> named;
        for (const std::string& name : logs.names()) {
            const std::optional<LogFileNameParts> parts = parseLogFileName(name);
            if (parts && !parts->temporary && parts->shard == shard) {
                named.emplace_back(parts->firstPosition, name);
            }
        }
        std::sort(named.begin(), named.end());
        for (std::size_t i = 0; i + 1 < named.size() && named[i + 1].first <= closedEnd; ++i) {
            const auto& [firstPosition, name] = named[i];
            if (!files.empty() && firstPosition <= files.back().firstPosition) {
                continue;  // known
            }
            files.push_back(describe(name, firstPosition, named[i + 1].first));
        }
    }

    ShardRoller::LogFileInfo ShardRoller::describe(const std::string& name, std::uint64_t firstPosition, std::uint64_t endPosition) {
        const log_reading::FileHeader header = log_reading::readHeader(logs, name);
        const auto* parsed = std::get_if<replay::LogHeader>(&header.parsed);
        if (parsed == nullptr) {
            throw PassAbandoned{"cannot read " + name + "'s header: " + std::get<replay::ParseError>(header.parsed).message};
        }
        LogFileInfo file;
        file.name = name;
        file.firstPosition = firstPosition;
        file.endPosition = endPosition;
        file.headerBytes = header.bytes;
        file.shardCount = parsed->shardCount;
        for (const Symbol& symbol : parsed->symbols) {
            const std::optional<BookLocation> location = directory.find(symbol);
            if (location && location->shard == shard) {
                file.slots.push_back(location->slot);
                // Where a book with no checkpoint stands: the first file that lists it.
                if (!firstListing[location->slot]) {
                    firstListing[location->slot] = firstPosition;
                }
            }
        }
        return file;
    }

    std::optional<ShardRoller::LogFileInfo> ShardRoller::lastFile(std::uint64_t logEnd) {
        // The newest file starting before the end, by name: a file starting at the end holds no
        // record, and one past it was made ahead and never written.
        std::optional<std::pair<std::uint64_t, std::string>> newest;
        for (const std::string& name : logs.names()) {
            const std::optional<LogFileNameParts> parts = parseLogFileName(name);
            if (parts && !parts->temporary && parts->shard == shard && parts->firstPosition < logEnd &&
                (!newest || parts->firstPosition > newest->first)) {
                newest.emplace(parts->firstPosition, name);
            }
        }
        if (!newest || (!files.empty() && newest->first <= files.back().firstPosition)) {
            return std::nullopt;  // none, or known: a roll found it closed
        }
        return describe(newest->second, newest->first, logEnd);
    }

    std::optional<std::uint64_t> ShardRoller::standing(std::uint32_t slot) const {
        return kept[slot].newest ? kept[slot].newest : firstListing[slot];
    }

    bool ShardRoller::standsAt(std::uint32_t slot, const LogFileInfo& file) const {
        return standing(slot) == file.firstPosition;
    }

    void ShardRoller::fallBack(std::uint32_t slot, const std::string& why) {
        // Removed, as a start would remove it, since its book will publish at its position again
        // and a rename never replaces a file.
        try {
            checkpoints.remove(checkpointFileName(directory.onShard(shard)[slot].symbol, *kept[slot].newest));
        } catch (const LogStorageError&) {
            // The next start removes it.
        }
        kept[slot].newest = kept[slot].older;
        kept[slot].older.reset();
        throw PassAbandoned{why + "; " + directory.onShard(shard)[slot].symbol.text() + " falls back to its older checkpoint"};
    }

    void ShardRoller::say(const std::string& lead, const char* what) noexcept {
        try {
            report("shard " + std::to_string(shard) + ": " + lead + what);
        } catch (...) {
            // Memory is short, or the report failed: the counts still hold what happened.
        }
    }

    std::size_t ShardRoller::roll(std::uint64_t closedEnd, const std::atomic<bool>* stopping) {
        std::size_t made = 0;
        rollAbandoned = false;
        try {
            for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
                removeSuperseded(slot);
            }
            readLogFiles(closedEnd);
            std::vector<bool> needs(kept.size());
            for (const LogFileInfo& file : files) {
                bool any = false;
                for (const std::uint32_t slot : file.slots) {
                    needs[slot] = standsAt(slot, file);
                    any = any || needs[slot];
                }
                if (!any) {
                    continue;
                }
                if (stopping != nullptr && stopping->load(std::memory_order_relaxed)) {
                    break;
                }
                pass(file, file.endPosition, needs);
                ++made;
                std::fill(needs.begin(), needs.end(), false);
            }
        } catch (const std::bad_alloc&) {
            ++totals.passesLost;
            say("a roll-forward pass was lost to bad_alloc; the next roll makes it again", "");
        } catch (const PassAbandoned& abandoned) {
            rollAbandoned = true;
            say("roll forward stopped: ", abandoned.why.c_str());
        } catch (const LogStorageError& error) {
            // A read that failed, since a failed publish is caught where it happens: the next
            // roll tries again.
            say("roll forward stopped: ", error.what());
        }
        countBehind();
        totals.passes += made;
        return made;
    }

    void ShardRoller::countBehind() {
        behind = 0;
        for (const LogFileInfo& file : files) {
            // Behind if a book it lists stands before its end.
            const std::uint64_t end = file.endPosition;
            const bool needed = std::any_of(file.slots.begin(), file.slots.end(), [this, end](std::uint32_t slot) {
                const std::optional<std::uint64_t> at = standing(slot);
                return at && *at < end;
            });
            behind += needed ? 1 : 0;
        }
    }

    void ShardRoller::finish(std::uint64_t logEnd, const LiveImages& live) {
        // A pass abandoned for a checkpoint that no longer reads has its book fall back, and is
        // made again at once, as the next roll would make it; each book can fall back only so far.
        for (std::size_t attempt = 0; attempt <= 2 * kept.size(); ++attempt) {
            if (finishOnce(logEnd, live)) {
                break;
            }
        }
        countBehind();
    }

    bool ShardRoller::finishOnce(std::uint64_t logEnd, const LiveImages& live) {
        std::fill(atStop.begin(), atStop.end(), StoppedBook{.outcome = live ? StopOutcome::NOT_ROLLED : StopOutcome::NO_IMAGE});
        // Every closed file first, each published at its own end, as any roll publishes it.
        static_cast<void>(roll(logEnd));
        if (rollAbandoned) {
            return false;
        }
        if (!live) {
            return true;
        }
        try {
            const std::optional<LogFileInfo> last = lastFile(logEnd);
            // What the log rebuilds of each book at its end: the last file rolled, or, for a book
            // already standing there, its newest checkpoint.
            std::vector<std::optional<std::vector<std::byte>>> rolled(kept.size());
            if (last) {
                std::vector<bool> needs(kept.size());
                bool any = false;
                for (const std::uint32_t slot : last->slots) {
                    needs[slot] = standsAt(slot, *last);
                    any = any || needs[slot];
                }
                if (any) {
                    for (Ready& book : imagesAt(*last, logEnd, needs)) {
                        rolled[book.slot] = std::move(book.image);
                    }
                }
            }
            for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
                if (!rolled[slot] && kept[slot].newest == logEnd) {
                    rolled[slot] = newestImage(slot);
                }
            }
            // Then each against its live book, publishing those that match.
            for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
                if (!rolled[slot]) {
                    continue;  // NOT_ROLLED: it stands behind the last file, as a failure left it
                }
                const std::vector<std::byte>& fromLog = *rolled[slot];
                const std::vector<std::byte>& liveImage = (*live)[slot];
                const auto [logAt, liveAt] = std::mismatch(fromLog.begin(), fromLog.end(), liveImage.begin(), liveImage.end());
                StoppedBook& book = atStop[slot];
                if (logAt != fromLog.end() || liveAt != liveImage.end()) {
                    book.outcome = StopOutcome::DIFFERS;
                    book.differsAt = static_cast<std::uint64_t>(logAt - fromLog.begin());
                    // The header is the one it would have been published under.
                    const std::uint32_t shardCount = last ? last->shardCount : directory.shardCount();
                    book.saved = saveLive(slot, shardCount, logEnd, liveImage);
                } else if (kept[slot].newest == logEnd) {
                    book.outcome = StopOutcome::PUBLISHED;
                } else {
                    book.outcome = publish(slot, last->shardCount, logEnd, fromLog) ? StopOutcome::PUBLISHED : StopOutcome::PUBLISH_FAILED;
                }
            }
            if (last) {
                files.push_back(*last);
            }
        } catch (const std::bad_alloc&) {
            ++totals.passesLost;
            say("the clean stop's last roll-forward pass was lost to bad_alloc", "");
        } catch (const PassAbandoned& abandoned) {
            say("the clean stop's last roll forward stopped: ", abandoned.why.c_str());
            return false;
        } catch (const LogStorageError& error) {
            say("the clean stop's last roll forward stopped: ", error.what());
        }
        return true;
    }

    void ShardRoller::pass(const LogFileInfo& file, std::uint64_t end, const std::vector<bool>& needs) {
        for (const Ready& book : imagesAt(file, end, needs)) {
            static_cast<void>(publish(book.slot, file.shardCount, end, book.image));
        }
    }

    std::vector<ShardRoller::Ready> ShardRoller::imagesAt(const LogFileInfo& file, std::uint64_t end, const std::vector<bool>& needs) {
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
                    const std::vector<std::byte> bytes = readWhole(checkpoints, name);
                    const auto decoded = decodeCheckpoint(bytes);
                    const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
                    std::optional<std::variant<OrderBook, RestoreError>> restored;
                    if (checkpoint != nullptr) {
                        restored.emplace(OrderBook::restore(checkpoint->image));
                    }
                    if (!restored || std::holds_alternative<RestoreError>(*restored)) {
                        // Bad since it was read: fall back to the older one, from which the next
                        // roll starts.
                        fallBack(slot, name + " no longer restores");
                    }
                    books[slot].emplace(std::move(std::get<OrderBook>(*restored)));
                }
            }
            return *books[slot];
        };

        const std::uint64_t records = end - file.firstPosition;
        log_reading::LogFileReader reader(logs, file.name, file.headerBytes + records * replay::logRecordBytes);
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
                ready.push_back(Ready{slot, newestImage(slot)});
            } else {
                ready.push_back(Ready{slot, imageOf(OrderBook{})});
            }
        }
        return ready;
    }

    std::vector<std::byte> ShardRoller::newestImage(std::uint32_t slot) {
        const std::string name = checkpointFileName(directory.onShard(shard)[slot].symbol, *kept[slot].newest);
        const std::vector<std::byte> bytes = readWhole(checkpoints, name);
        const auto decoded = decodeCheckpoint(bytes);
        const auto* checkpoint = std::get_if<DecodedCheckpoint>(&decoded);
        if (checkpoint == nullptr) {
            fallBack(slot, name + " no longer decodes");
        }
        return std::vector<std::byte>(checkpoint->image.begin(), checkpoint->image.end());
    }

    bool ShardRoller::publish(std::uint32_t slot, std::uint32_t shardCount, std::uint64_t end, std::span<const std::byte> image) {
        const Symbol& symbol = directory.onShard(shard)[slot].symbol;
        Kept& held = kept[slot];
        try {
            publishCheckpoint(checkpoints, headerFor(symbol, shardCount, shard, end), image);
        } catch (const LogStorageError& error) {
            ++totals.writesFailed;
            report("shard " + std::to_string(shard) + ": checkpoint " + checkpointFileName(symbol, end) + " not published: " +
                   error.what() + "; " + symbol.text() + " stays at its last checkpoint");
            return false;
        }
        ++totals.published;
        // Nothing allocates from here to the removal, so a book published is always one the
        // roller knows; a removal a bad_alloc cuts short stays pending for the next roll. None
        // is pending here: each roll starts by retrying them, and a bad_alloc ends a roll.
        held.superseded = held.older;
        held.older = held.newest;
        held.newest = end;
        removeSuperseded(slot);
        return true;
    }

    bool ShardRoller::saveLive(std::uint32_t slot, std::uint32_t shardCount, std::uint64_t end, std::span<const std::byte> image) {
        const Symbol& symbol = directory.onShard(shard)[slot].symbol;
        const std::string name = liveImageFileName(symbol, end);
        const std::vector<std::byte> bytes = encodeCheckpoint(headerFor(symbol, shardCount, shard, end), image);
        try {
            std::unique_ptr<LogFile> file;
            try {
                file = checkpoints.create(name, Durability::PROCESS);
            } catch (const LogStorageError& error) {
                if (error.kind() != LogStorageError::Kind::EXISTS) {
                    throw;
                }
                // An earlier stop's at the same position, which this one replaces.
                checkpoints.remove(name);
                file = checkpoints.create(name, Durability::PROCESS);
            }
            file->write(0, bytes);
            file->flush();
            file->close();
        } catch (const LogStorageError& error) {
            report("shard " + std::to_string(shard) + ": cannot save " + name + ": " + error.what());
            return false;
        }
        return true;
    }

    RollWorker::RollWorker(LogStorage& storage, const InstrumentDirectory& directory, const ClosedLog& closedLog,
                           std::vector<std::uint32_t> shards, ShardRoller::Report reportTo)
        : RollWorker(storage, storage, directory, closedLog, std::move(shards), std::move(reportTo), {}) {}

    RollWorker::RollWorker(LogStorage& logStorage, LogStorage& checkpointStorage, const InstrumentDirectory& directory,
                           const ClosedLog& closedLog, std::vector<std::uint32_t> shards, ShardRoller::Report reportTo, Rolled rolledTo)
        : closed(closedLog), mine(std::move(shards)), report(reportTo), rolled(std::move(rolledTo)) {
        rollers.reserve(mine.size());
        for (const std::uint32_t shard : mine) {
            rollers.push_back(std::make_unique<ShardRoller>(logStorage, checkpointStorage, directory, shard, reportTo));
        }
    }

    std::size_t RollWorker::pass() {
        std::size_t made = 0;
        for (std::size_t index = 0; index < mine.size() && !closed.stopping().load(std::memory_order_relaxed); ++index) {
            made += rollers[index]->roll(closed.end(mine[index]), &closed.stopping());
            if (rolled) {
                if (const std::optional<std::uint64_t> floor = rollers[index]->retentionFloor()) {
                    rolled(mine[index], *floor);
                }
            }
        }
        return made;
    }

    void RollWorker::run() {
        while (!closed.stopping().load(std::memory_order_relaxed)) {
            // Read before the pass, so a file closed during it wakes the wait at once.
            const std::uint64_t seen = closed.generation();
            try {
                pass();
            } catch (const std::exception& error) {
                // roll catches what it expects; anything else ends this pass, not the engine.
                try {
                    report(std::string("roll forward stopped: ") + error.what());
                } catch (const std::exception&) {
                }
            }
            closed.waitPast(seen);
        }
    }

    void RollWorker::finish(std::span<const ShardAtStop> byShard) {
        for (std::size_t index = 0; index < mine.size(); ++index) {
            const ShardAtStop& stop = byShard[mine[index]];
            try {
                rollers[index]->finish(stop.logEnd, *stop.live);
            } catch (const std::exception& error) {
                // finish throws nothing it expects, so this is a report that failed: its books
                // stay as finish left them, and the other shards still roll.
                try {
                    report(std::string("the clean stop's last roll forward stopped: ") + error.what());
                } catch (const std::exception&) {
                }
            }
        }
    }

}  // namespace order_book::runtime
