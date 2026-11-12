#include "order_book/wasm/shadow_books.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

#include "order_book/book_image.hpp"
#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book::wasm {

    namespace {

        // The image's version 1 sizes (docs/checkpoint-format.md).
        constexpr std::size_t imageHeaderBytes = 64;
        constexpr std::size_t levelRecordBytes = 16;
        constexpr std::size_t orderRecordBytes = 24;
        constexpr std::size_t stopRecordBytes = 32;

        std::uint32_t load32(std::span<const std::byte> bytes, std::size_t at) {
            std::uint32_t value = 0;
            for (std::size_t i = 4; i-- > 0;) {
                value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at + i]);
            }
            return value;
        }

        std::uint64_t load64(std::span<const std::byte> bytes, std::size_t at) {
            return std::uint64_t{load32(bytes, at)} | (std::uint64_t{load32(bytes, at + 4)} << 32);
        }

        std::uint8_t load8(std::span<const std::byte> bytes, std::size_t at) { return std::to_integer<std::uint8_t>(bytes[at]); }

        [[noreturn]] void notAnImage(const std::string& why) { throw std::invalid_argument("not a book image: " + why); }

        // A byte the image format numbers from 1, as the enumerator numbered from 0.
        template <typename Enum>
        Enum fromByte(std::uint8_t value, std::uint8_t most, const char* field) {
            if (value == 0 || value > most) {
                notAnImage(std::string(field) + " " + std::to_string(value));
            }
            return static_cast<Enum>(value - 1);
        }

        // Reads a section's records in turn, each at its stated stride, never past the end.
        class Cursor {
        public:
            Cursor(std::span<const std::byte> image, std::size_t start) : bytes(image), at(start) {}

            std::span<const std::byte> next(std::size_t stride, const char* what) {
                if (bytes.size() - at < stride) {
                    notAnImage(std::string("it ends inside ") + what);
                }
                const std::span<const std::byte> record = bytes.subspan(at, stride);
                at += stride;
                return record;
            }

            bool atEnd() const noexcept { return at == bytes.size(); }

        private:
            std::span<const std::byte> bytes;
            std::size_t at;
        };

        std::vector<LevelView> levels(Cursor& cursor, std::uint32_t count, std::uint32_t levelStride, std::uint32_t orderStride) {
            std::vector<LevelView> side;
            side.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::span<const std::byte> level = cursor.next(levelStride, "a level");
                LevelView view{load32(level, 0), load64(level, 8), {}};
                const std::uint32_t orders = load32(level, 4);
                for (std::uint32_t j = 0; j < orders; ++j) {
                    const std::span<const std::byte> order = cursor.next(orderStride, "an order");
                    view.orders.push_back(RestingOrderView{load32(order, 0), load32(order, 4), load32(order, 8), load32(order, 12),
                                                           load32(order, 16)});
                }
                side.push_back(std::move(view));
            }
            return side;
        }

        bool isLogHeader(std::span<const std::byte> bytes) {
            if (bytes.size() < replay::logMagic.size()) {
                return false;
            }
            for (std::size_t i = 0; i < replay::logMagic.size(); ++i) {
                if (std::to_integer<char>(bytes[i]) != replay::logMagic[i]) {
                    return false;
                }
            }
            return true;
        }

        // A header as a write carried it, read through the log's own reader.
        replay::LogHeader readLogHeader(std::span<const std::byte> bytes) {
            std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            replay::CommandLogReader reader(input);
            const replay::ReadResult first = reader.next();
            if (const auto* error = std::get_if<replay::ParseError>(&first)) {
                throw std::runtime_error("its header doesn't read: " + error->message);
            }
            if (reader.header() == nullptr || !std::holds_alternative<replay::EndOfInput>(first)) {
                throw std::runtime_error("its first write isn't its header alone");
            }
            return *reader.header();
        }

        std::vector<std::byte> imageOf(const OrderBook& book) {
            std::vector<std::byte> image(book.imageSize());
            image.resize(book.writeImage(image));
            return image;
        }

    }  // namespace

    BookView decodeBookImage(std::span<const std::byte> image) {
        if (image.size() < imageHeaderBytes) {
            notAnImage("shorter than its header");
        }
        if (load32(image, 4) != 1) {
            notAnImage("minimum reader version " + std::to_string(load32(image, 4)));
        }
        const std::uint32_t headerBytes = load32(image, 8);
        const std::uint32_t levelStride = load32(image, 12);
        const std::uint32_t orderStride = load32(image, 16);
        const std::uint32_t stopStride = load32(image, 20);
        if (headerBytes < imageHeaderBytes || headerBytes > image.size() || levelStride < levelRecordBytes ||
            orderStride < orderRecordBytes || stopStride < stopRecordBytes) {
            notAnImage("its header states sizes this reader can't use");
        }
        BookView book;
        if (const std::uint32_t last = load32(image, 32); last != 0) {
            book.lastTradePrice = last;
        }
        Cursor cursor(image, headerBytes);
        book.bids = levels(cursor, load32(image, 36), levelStride, orderStride);
        book.asks = levels(cursor, load32(image, 40), levelStride, orderStride);
        const std::uint32_t stops = load32(image, 48);
        book.stops.reserve(stops);
        for (std::uint32_t i = 0; i < stops; ++i) {
            const std::span<const std::byte> stop = cursor.next(stopStride, "a stop");
            PendingStopView view;
            view.orderId = load32(stop, 0);
            view.sequence = load32(stop, 4);
            view.stopPrice = load32(stop, 8);
            view.limitPrice = load32(stop, 12);
            view.quantity = load32(stop, 16);
            view.owner = load32(stop, 20);
            view.side = fromByte<Side>(load8(stop, 24), 2, "side");
            view.orderType = fromByte<OrderType>(load8(stop, 25), 4, "order type");
            view.timeInForce = fromByte<TimeInForce>(load8(stop, 26), 3, "time in force");
            view.selfTradePrevention = fromByte<SelfTradePrevention>(load8(stop, 27), 3, "self-trade prevention");
            if (load8(stop, 28) > 1) {
                notAnImage("post-only " + std::to_string(load8(stop, 28)));
            }
            view.postOnly = load8(stop, 28) == 1;
            book.stops.push_back(view);
        }
        std::size_t orders = 0;
        for (const std::vector<LevelView>* side : {&book.bids, &book.asks}) {
            for (const LevelView& level : *side) {
                orders += level.orders.size();
            }
        }
        if (orders != load32(image, 44) || !cursor.atEnd()) {
            notAnImage("its records disagree with its header's counts");
        }
        return book;
    }

    OrderId highestOrderId(const BookView& book) noexcept {
        OrderId highest = 0;
        for (const std::vector<LevelView>* side : {&book.bids, &book.asks}) {
            for (const LevelView& level : *side) {
                for (const RestingOrderView& order : level.orders) {
                    highest = std::max(highest, order.orderId);
                }
            }
        }
        for (const PendingStopView& stop : book.stops) {
            highest = std::max(highest, stop.orderId);
        }
        return highest;
    }

    ShadowBooks::ShadowBooks(const runtime::InstrumentDirectory& instruments, const BookCapacity& capacity)
        : directory(instruments), defaults(capacity) {
        emptyBooks();
    }

    void ShadowBooks::emptyBooks() {
        shadows.clear();
        logged.assign(directory.shardCount(), 0);
        files.clear();
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            std::vector<Shadow>& books = shadows.emplace_back();
            for (const runtime::Instrument& instrument : directory.onShard(shard)) {
                books.push_back(Shadow{});
                books.back().book.reserve(runtime::capacityOf(instrument, defaults));
            }
        }
    }

    void ShadowBooks::rebuild(runtime::LogStorage& store) {
        std::vector<std::vector<Shadow>> rebuilt;
        std::vector<std::uint64_t> counts;
        std::map<std::string, FollowedFile> made;
        try {
            const runtime::LogSurvey survey = runtime::surveyLog(store, directory);
            if (survey.placement) {
                throw std::runtime_error("placement changed: " + survey.placement->what);
            }
            const std::atomic<bool> stopping{false};
            runtime::RecoveryOptions options;
            options.readOnly = true;
            for (std::uint32_t number = 0; number < directory.shardCount(); ++number) {
                runtime::Shard shard(directory, number, defaults);
                runtime::recoverShard(store, store, directory, survey.shards[number], survey.checkpoints[number], shard, defaults, stopping,
                                      options);
                std::vector<Shadow>& books = rebuilt.emplace_back();
                const std::span<const runtime::Instrument> instruments = directory.onShard(number);
                for (std::uint32_t slot = 0; slot < shard.size(); ++slot) {
                    const std::vector<std::byte> image = imageOf(shard.books().book(slot));
                    std::variant<OrderBook, RestoreError> restored = OrderBook::restore(image);
                    if (const auto* error = std::get_if<RestoreError>(&restored)) {
                        throw std::runtime_error(instruments[slot].symbol.text() + "'s image doesn't restore: " + error->message);
                    }
                    books.push_back(Shadow{std::move(std::get<OrderBook>(restored)), highestOrderId(decodeBookImage(image)), 0});
                    books.back().book.reserve(runtime::capacityOf(instruments[slot], defaults));
                }
                counts.push_back(shard.nextPosition());
                // A start opens a new file for each shard, so no file here gets another record.
                for (const runtime::LogFileEntry& entry : survey.shards[number]) {
                    made[entry.name].closed = true;
                }
            }
            for (const std::string& name : survey.temporaries) {
                made[name].closed = true;
            }
        } catch (const std::exception& error) {
            const std::lock_guard held(guard);
            emptyBooks();
            failure = std::string("the shadows can't be rebuilt from the store: ") + error.what();
            return;
        }
        const std::lock_guard held(guard);
        for (std::size_t shard = 0; shard < rebuilt.size(); ++shard) {
            for (std::size_t slot = 0; slot < rebuilt[shard].size(); ++slot) {
                rebuilt[shard][slot].highestAssigned = shadows[shard][slot].highestAssigned;
            }
        }
        shadows = std::move(rebuilt);
        logged = std::move(counts);
        files = std::move(made);
        failure.reset();
    }

    void ShadowBooks::follow(const runtime::PageLogStorage::Change& change) noexcept {
        using Kind = runtime::PageLogStorage::Change::Kind;
        try {
            const std::lock_guard held(guard);
            if (failure) {
                return;
            }
            switch (change.kind) {
                case Kind::CREATE:
                    files.insert_or_assign(change.name, FollowedFile{});
                    break;
                case Kind::WRITE:
                    followWrite(change);
                    break;
                case Kind::CUT:
                    if (const auto file = files.find(change.name); file != files.end() && file->second.hasHeader &&
                                                                   change.size < file->second.recordsStart + file->second.records * replay::logRecordBytes) {
                        fail(change.name + ": cut to " + std::to_string(change.size) + " bytes, before records the shadows ran");
                    }
                    break;
                case Kind::RENAME:
                case Kind::REPLACE: {
                    const auto file = files.find(change.name);
                    if (file == files.end()) {
                        files.erase(change.to);
                        break;
                    }
                    const FollowedFile moved = file->second;
                    files.erase(file);
                    files.insert_or_assign(change.to, moved);
                    break;
                }
                case Kind::REMOVE:
                    files.erase(change.name);
                    break;
                case Kind::EXTEND:
                    break;
            }
        } catch (const std::exception& error) {
            const std::lock_guard held(guard);
            fail(change.name + ": " + error.what());
        } catch (...) {
            const std::lock_guard held(guard);
            fail(change.name + ": an unknown exception");
        }
    }

    void ShadowBooks::followWrite(const runtime::PageLogStorage::Change& change) {
        const auto found = files.find(change.name);
        if (found == files.end()) {
            return;  // nothing the shadows follow: a checkpoint, or a file a start removes
        }
        FollowedFile& file = found->second;
        if (file.closed) {
            fail(change.name + ": written to after the start, though a start always opens a new log file");
            return;
        }
        if (!file.hasHeader) {
            if (change.offset != 0 || !isLogHeader(change.bytes)) {
                files.erase(found);  // not a log file
                return;
            }
            const replay::LogHeader header = readLogHeader(change.bytes);
            if (header.shardCount != directory.shardCount() || header.shardIndex >= directory.shardCount()) {
                fail(change.name + ": its header names shard " + std::to_string(header.shardIndex) + " of " +
                     std::to_string(header.shardCount));
                return;
            }
            file.hasHeader = true;
            file.shard = header.shardIndex;
            file.firstPosition = header.firstPosition;
            file.recordsStart = change.bytes.size();
            file.checksum = header.chainSeed;
            return;
        }
        const std::uint64_t expected = file.recordsStart + file.records * replay::logRecordBytes;
        if (change.offset != expected || change.bytes.size() % replay::logRecordBytes != 0) {
            fail(change.name + ": " + std::to_string(change.bytes.size()) + " bytes written at " + std::to_string(change.offset) +
                 ", where whole records go from " + std::to_string(expected));
            return;
        }
        for (std::size_t at = 0; at < change.bytes.size(); at += replay::logRecordBytes) {
            const std::span<const std::byte> record = change.bytes.subspan(at, replay::logRecordBytes);
            const std::uint64_t position = file.firstPosition + file.records;
            if (position != logged[file.shard]) {
                fail(change.name + ": a record at position " + std::to_string(position) + " of shard " + std::to_string(file.shard) +
                     ", whose log holds " + std::to_string(logged[file.shard]));
                return;
            }
            const replay::ParseResult decoded = replay::decodeLogRecord(record, file.checksum, position, file.records);
            if (const auto* error = std::get_if<replay::ParseError>(&decoded)) {
                fail(change.name + ": the record at position " + std::to_string(position) + " doesn't decode: " + error->message);
                return;
            }
            const replay::ReplayRecord& read = std::get<replay::ReplayRecord>(decoded);
            const std::optional<runtime::BookLocation> location = directory.find(read.symbol);
            if (!location || location->shard != file.shard) {
                fail(change.name + ": the record at position " + std::to_string(position) + " is for " + read.symbol.text() +
                     ", not on shard " + std::to_string(file.shard));
                return;
            }
            Shadow& shadow = shadows[location->shard][location->slot];
            static_cast<void>(replay::executeCommand(shadow.book, read.command));
            if (const auto* add = std::get_if<replay::AddCommand>(&read.command)) {
                shadow.highestSeen = std::max(shadow.highestSeen, add->orderId);
            }
            file.checksum = load32(record, 0);
            ++file.records;
            ++logged[file.shard];
        }
    }

    void ShadowBooks::fail(std::string what) {
        if (!failure) {
            failure = std::move(what);
        }
    }

    std::optional<std::string> ShadowBooks::problem() const {
        const std::lock_guard held(guard);
        return failure;
    }

    const ShadowBooks::Shadow& ShadowBooks::at(const replay::Symbol& symbol) const {
        const std::optional<runtime::BookLocation> location = directory.find(symbol);
        if (!location) {
            throw std::out_of_range(symbol.text() + " is not on the instrument list");
        }
        return shadows[location->shard][location->slot];
    }

    std::vector<std::byte> ShadowBooks::image(const replay::Symbol& symbol) const {
        const std::lock_guard held(guard);
        return imageOf(at(symbol).book);
    }

    BookView ShadowBooks::book(const replay::Symbol& symbol) const { return decodeBookImage(image(symbol)); }

    std::optional<std::uint32_t> ShadowBooks::shardOf(const replay::Symbol& symbol) const noexcept {
        if (const std::optional<runtime::BookLocation> location = directory.find(symbol)) {
            return location->shard;
        }
        return std::nullopt;
    }

    std::uint64_t ShadowBooks::records(std::uint32_t shard) const {
        const std::lock_guard held(guard);
        return logged.at(shard);
    }

    std::optional<OrderId> ShadowBooks::assignOrderId(const replay::Symbol& symbol) {
        const std::optional<runtime::BookLocation> location = directory.find(symbol);
        if (!location) {
            return std::nullopt;
        }
        const std::lock_guard held(guard);
        Shadow& shadow = shadows[location->shard][location->slot];
        const OrderId highest = std::max(shadow.highestSeen, shadow.highestAssigned);
        if (highest == std::numeric_limits<OrderId>::max()) {
            return std::nullopt;
        }
        shadow.highestAssigned = highest + 1;
        return shadow.highestAssigned;
    }

}  // namespace order_book::wasm
