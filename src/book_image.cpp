#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/little_endian.hpp"
#include "order_book/order_book.hpp"

// The book image, byte for byte as docs/checkpoint-format.md specifies it: writeImage, imageSize,
// and restore, OrderBook members kept apart from the commands.
namespace order_book {

    namespace {
        using little_endian::load32;
        using little_endian::load64;
        using little_endian::load8;
        using little_endian::store32;
        using little_endian::store64;
        using little_endian::store8;

        constexpr std::uint32_t imageVersion = 1;
        constexpr std::uint32_t minimumReaderVersion = 1;
        constexpr std::size_t headerBytes = 64;
        constexpr std::size_t levelBytes = 16;
        constexpr std::size_t orderBytes = 24;
        constexpr std::size_t stopBytes = 32;

        namespace header {
            constexpr std::size_t version = 0;
            constexpr std::size_t minimumReader = 4;
            constexpr std::size_t headerSize = 8;
            constexpr std::size_t levelSize = 12;
            constexpr std::size_t orderSize = 16;
            constexpr std::size_t stopSize = 20;
            constexpr std::size_t nextSequence = 24;
            constexpr std::size_t nextTrade = 28;
            constexpr std::size_t lastTrade = 32;
            constexpr std::size_t bidLevels = 36;
            constexpr std::size_t askLevels = 40;
            constexpr std::size_t orders = 44;
            constexpr std::size_t stops = 48;
            constexpr std::size_t orderIndex = 52;
            constexpr std::size_t stopIndex = 56;
            constexpr std::size_t reserved = 60;
        }  // namespace header

        namespace level {
            constexpr std::size_t price = 0;
            constexpr std::size_t count = 4;
            constexpr std::size_t aggregate = 8;
        }  // namespace level

        namespace order {
            constexpr std::size_t id = 0;
            constexpr std::size_t sequence = 4;
            constexpr std::size_t initial = 8;
            constexpr std::size_t remaining = 12;
            constexpr std::size_t owner = 16;
            constexpr std::size_t fill = 20;
            constexpr std::size_t reserved = 21;
        }  // namespace order

        namespace stop {
            constexpr std::size_t id = 0;
            constexpr std::size_t sequence = 4;
            constexpr std::size_t stopPrice = 8;
            constexpr std::size_t limitPrice = 12;
            constexpr std::size_t quantity = 16;
            constexpr std::size_t owner = 20;
            constexpr std::size_t side = 24;
            constexpr std::size_t type = 25;
            constexpr std::size_t timeInForce = 26;
            constexpr std::size_t selfTradePrevention = 27;
            constexpr std::size_t postOnly = 28;
            constexpr std::size_t reserved = 29;
        }  // namespace stop

        // The format's own byte values, never the C++ enumerators. Each decode returns nothing
        // for a byte outside its values.
        constexpr std::uint8_t sideByte(Side side) noexcept {
            switch(side) {
                case Side::BUY: return 1;
                case Side::SELL: return 2;
            }
            return 0;
        }

        constexpr std::optional<Side> sideOf(std::uint8_t value) noexcept {
            switch(value) {
                case 1: return Side::BUY;
                case 2: return Side::SELL;
                default: return std::nullopt;
            }
        }

        constexpr std::uint8_t orderTypeByte(OrderType type) noexcept {
            switch(type) {
                case OrderType::LIMIT: return 1;
                case OrderType::MARKET: return 2;
                case OrderType::STOP: return 3;
                case OrderType::STOP_LIMIT: return 4;
            }
            return 0;
        }

        constexpr std::optional<OrderType> orderTypeOf(std::uint8_t value) noexcept {
            switch(value) {
                case 1: return OrderType::LIMIT;
                case 2: return OrderType::MARKET;
                case 3: return OrderType::STOP;
                case 4: return OrderType::STOP_LIMIT;
                default: return std::nullopt;
            }
        }

        constexpr std::uint8_t timeInForceByte(TimeInForce timeInForce) noexcept {
            switch(timeInForce) {
                case TimeInForce::GTC: return 1;
                case TimeInForce::IOC: return 2;
                case TimeInForce::FOK: return 3;
            }
            return 0;
        }

        constexpr std::optional<TimeInForce> timeInForceOf(std::uint8_t value) noexcept {
            switch(value) {
                case 1: return TimeInForce::GTC;
                case 2: return TimeInForce::IOC;
                case 3: return TimeInForce::FOK;
                default: return std::nullopt;
            }
        }

        constexpr std::uint8_t selfTradePreventionByte(SelfTradePrevention mode) noexcept {
            switch(mode) {
                case SelfTradePrevention::CANCEL_RESTING: return 1;
                case SelfTradePrevention::CANCEL_INCOMING: return 2;
                case SelfTradePrevention::CANCEL_BOTH: return 3;
            }
            return 0;
        }

        constexpr std::optional<SelfTradePrevention> selfTradePreventionOf(std::uint8_t value) noexcept {
            switch(value) {
                case 1: return SelfTradePrevention::CANCEL_RESTING;
                case 2: return SelfTradePrevention::CANCEL_INCOMING;
                case 3: return SelfTradePrevention::CANCEL_BOTH;
                default: return std::nullopt;
            }
        }

        constexpr std::uint8_t fillStatusByte(FillStatus status) noexcept {
            switch(status) {
                case FillStatus::NONE: return 1;
                case FillStatus::PARTIAL: return 2;
                case FillStatus::FILLED: return 3;
            }
            return 0;
        }

        // FILLED has a byte, so a broken book's image says what it held, but it never rests.
        constexpr std::optional<FillStatus> restingFillStatusOf(std::uint8_t value) noexcept {
            switch(value) {
                case 1: return FillStatus::NONE;
                case 2: return FillStatus::PARTIAL;
                default: return std::nullopt;
            }
        }

        bool allZero(std::span<const std::byte> bytes) noexcept {
            for(const std::byte value : bytes) {
                if(value != std::byte{0}) {
                    return false;
                }
            }
            return true;
        }

        // total += a * b, or false if that overflows.
        bool addProduct(std::uint64_t& total, std::uint64_t a, std::uint64_t b) noexcept {
            constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
            if(a != 0 && b > most / a) {
                return false;
            }
            const std::uint64_t product = a * b;
            if(product > most - total) {
                return false;
            }
            total += product;
            return true;
        }

        RestoreError refusal(RestoreErrorCode code, std::size_t offset, const char* message) {
            return RestoreError{code, offset, message, std::nullopt};
        }

        // What the header says, once checked: each part's stride, and the counts.
        struct Layout {
            std::size_t headerStride = headerBytes;
            std::size_t levelStride = levelBytes;
            std::size_t orderStride = orderBytes;
            std::size_t stopStride = stopBytes;
            std::uint32_t nextSequence = 1;
            std::uint32_t nextTrade = 1;
            std::uint32_t lastTrade = 0;
            std::uint32_t bidLevels = 0;
            std::uint32_t askLevels = 0;
            std::uint32_t orders = 0;
            std::uint32_t stops = 0;
        };

        // The header's checks, in docs/checkpoint-format.md's order: its size, versions, stated
        // sizes, and length, then its own fields. Fills layout as it goes.
        std::optional<RestoreError> checkImageHeader(std::span<const std::byte> image, Layout& layout) {
            if(image.size() < headerBytes) {
                return refusal(RestoreErrorCode::TOO_SHORT, image.size(), "the image is shorter than a header");
            }
            if(load32(image, header::version) == 0) {
                return refusal(RestoreErrorCode::UNSUPPORTED_VERSION, header::version, "image version 0 does not exist");
            }
            if(load32(image, header::minimumReader) != minimumReaderVersion) {
                return refusal(RestoreErrorCode::UNSUPPORTED_VERSION, header::minimumReader,
                               "the image needs a reader of another version");
            }
            const struct {
                std::size_t offset;
                std::size_t known;
                std::size_t* stride;
                const char* message;
            } sizes[] = {
                {header::headerSize, headerBytes, &layout.headerStride, "the stated header size is below 64 bytes"},
                {header::levelSize, levelBytes, &layout.levelStride, "the stated level record size is below 16 bytes"},
                {header::orderSize, orderBytes, &layout.orderStride, "the stated order record size is below 24 bytes"},
                {header::stopSize, stopBytes, &layout.stopStride, "the stated stop record size is below 32 bytes"},
            };
            for(const auto& size : sizes) {
                const std::uint32_t stated = load32(image, size.offset);
                if(stated < size.known) {
                    return refusal(RestoreErrorCode::SIZE_BELOW_KNOWN, size.offset, size.message);
                }
                *size.stride = stated;
            }
            if(image.size() < layout.headerStride) {
                return refusal(RestoreErrorCode::TOO_SHORT, image.size(), "the image is shorter than its stated header");
            }
            layout.bidLevels = load32(image, header::bidLevels);
            layout.askLevels = load32(image, header::askLevels);
            layout.orders = load32(image, header::orders);
            layout.stops = load32(image, header::stops);
            std::uint64_t length = layout.headerStride;
            const bool fits = addProduct(length, layout.levelStride,
                                         std::uint64_t{layout.bidLevels} + std::uint64_t{layout.askLevels}) &&
                              addProduct(length, layout.orderStride, layout.orders) &&
                              addProduct(length, layout.stopStride, layout.stops);
            if(!fits || length != image.size()) {
                return refusal(RestoreErrorCode::LENGTH_MISMATCH, image.size(),
                               "the image's length is not what its counts give");
            }
            layout.nextSequence = load32(image, header::nextSequence);
            if(layout.nextSequence == 0) {
                return refusal(RestoreErrorCode::ZERO_FIELD, header::nextSequence, "the next sequence number is 0");
            }
            layout.nextTrade = load32(image, header::nextTrade);
            if(layout.nextTrade == 0) {
                return refusal(RestoreErrorCode::ZERO_FIELD, header::nextTrade, "the next trade ID is 0");
            }
            layout.lastTrade = load32(image, header::lastTrade);
            if(load32(image, header::orderIndex) != layout.orders) {
                return refusal(RestoreErrorCode::INDEX_SIZE_MISMATCH, header::orderIndex,
                               "the order index size is not the number of resting orders");
            }
            if(load32(image, header::stopIndex) != layout.stops) {
                return refusal(RestoreErrorCode::INDEX_SIZE_MISMATCH, header::stopIndex,
                               "the pending-stop index size is not the number of pending stops");
            }
            if(load32(image, header::reserved) != 0) {
                return refusal(RestoreErrorCode::NONZERO_RESERVED, header::reserved, "the header's reserved bytes are not 0");
            }
            return std::nullopt;
        }

        // Every level and order record, in image order, with the checks that need no index.
        // Leaves position where the stops begin.
        std::optional<RestoreError> checkLevelRecords(std::span<const std::byte> image, const Layout& layout,
                                                std::size_t& position) {
            std::uint64_t ordersRead = 0;
            const auto checkSide = [&](std::uint32_t levels, bool bids) -> std::optional<RestoreError> {
                std::optional<Price> previous;
                for(std::uint32_t index = 0; index < levels; ++index) {
                    const std::size_t at = position;
                    const std::uint32_t count = load32(image, at + level::count);
                    if(count == 0) {
                        return refusal(RestoreErrorCode::EMPTY_LEVEL, at + level::count, "a level holds no orders");
                    }
                    if(count > layout.orders - ordersRead) {
                        return refusal(RestoreErrorCode::ORDER_COUNT_MISMATCH, at + level::count,
                                       "the levels hold more orders than the image has");
                    }
                    const Price price = load32(image, at + level::price);
                    if(previous && (bids ? price >= *previous : price <= *previous)) {
                        return refusal(RestoreErrorCode::LEVELS_OUT_OF_ORDER, at + level::price,
                                       bids ? "a bid level is not below the one before it"
                                            : "an ask level is not above the one before it");
                    }
                    previous = price;
                    position += layout.levelStride;
                    std::uint64_t aggregate = 0;
                    for(std::uint32_t queued = 0; queued < count; ++queued) {
                        const std::size_t record = position;
                        if(load32(image, record + order::id) == 0) {
                            return refusal(RestoreErrorCode::ZERO_FIELD, record + order::id, "an order's ID is 0");
                        }
                        if(!restingFillStatusOf(load8(image, record + order::fill))) {
                            return refusal(RestoreErrorCode::INVALID_ENUMERATOR, record + order::fill,
                                           "an order's fill status is not NONE or PARTIAL");
                        }
                        if(!allZero(image.subspan(record + order::reserved, 3))) {
                            return refusal(RestoreErrorCode::NONZERO_RESERVED, record + order::reserved,
                                           "an order's reserved bytes are not 0");
                        }
                        const Quantity remaining = load32(image, record + order::remaining);
                        if(remaining > load32(image, record + order::initial)) {
                            return refusal(RestoreErrorCode::REMAINING_ABOVE_INITIAL, record + order::remaining,
                                           "an order has more remaining than its initial quantity");
                        }
                        aggregate += remaining;
                        position += layout.orderStride;
                    }
                    if(aggregate != load64(image, at + level::aggregate)) {
                        return refusal(RestoreErrorCode::AGGREGATE_MISMATCH, at + level::aggregate,
                                       "a level's aggregate is not its orders' remaining sum");
                    }
                    ordersRead += count;
                }
                return std::nullopt;
            };
            if(auto error = checkSide(layout.bidLevels, true)) {
                return error;
            }
            if(auto error = checkSide(layout.askLevels, false)) {
                return error;
            }
            if(ordersRead != layout.orders) {
                return refusal(RestoreErrorCode::ORDER_COUNT_MISMATCH, position,
                               "the levels hold fewer orders than the image has");
            }
            return std::nullopt;
        }

        // Every stop record, in image order: buys by ascending stop price, then sells by
        // descending, each byte inside its values.
        std::optional<RestoreError> checkStopRecords(std::span<const std::byte> image, const Layout& layout,
                                               std::size_t position) {
            std::optional<Side> previousSide;
            Price previousPrice = 0;
            for(std::uint32_t index = 0; index < layout.stops; ++index, position += layout.stopStride) {
                if(load32(image, position + stop::id) == 0) {
                    return refusal(RestoreErrorCode::ZERO_FIELD, position + stop::id, "a stop's ID is 0");
                }
                const std::optional<Side> side = sideOf(load8(image, position + stop::side));
                if(!side) {
                    return refusal(RestoreErrorCode::INVALID_ENUMERATOR, position + stop::side, "a stop's side is not BUY or SELL");
                }
                if(*side == Side::BUY && previousSide == Side::SELL) {
                    return refusal(RestoreErrorCode::STOPS_OUT_OF_ORDER, position + stop::side, "a buy stop follows a sell stop");
                }
                const Price stopPrice = load32(image, position + stop::stopPrice);
                if(previousSide == side && (*side == Side::BUY ? stopPrice < previousPrice : stopPrice > previousPrice)) {
                    return refusal(RestoreErrorCode::STOPS_OUT_OF_ORDER, position + stop::stopPrice,
                                   *side == Side::BUY ? "a buy stop's stop price is below the one before it"
                                                      : "a sell stop's stop price is above the one before it");
                }
                previousSide = side;
                previousPrice = stopPrice;
                if(!orderTypeOf(load8(image, position + stop::type))) {
                    return refusal(RestoreErrorCode::INVALID_ENUMERATOR, position + stop::type, "a stop's order type is not one");
                }
                if(!timeInForceOf(load8(image, position + stop::timeInForce))) {
                    return refusal(RestoreErrorCode::INVALID_ENUMERATOR, position + stop::timeInForce,
                                   "a stop's time in force is not one");
                }
                if(!selfTradePreventionOf(load8(image, position + stop::selfTradePrevention))) {
                    return refusal(RestoreErrorCode::INVALID_ENUMERATOR, position + stop::selfTradePrevention,
                                   "a stop's self-trade prevention is not a mode");
                }
                if(load8(image, position + stop::postOnly) > 1) {
                    return refusal(RestoreErrorCode::INVALID_ENUMERATOR, position + stop::postOnly,
                                   "a stop's post-only byte is not 0 or 1");
                }
                if(!allZero(image.subspan(position + stop::reserved, 3))) {
                    return refusal(RestoreErrorCode::NONZERO_RESERVED, position + stop::reserved,
                                   "a stop's reserved bytes are not 0");
                }
            }
            return std::nullopt;
        }

        // A checked stop record as the request it was accepted with.
        OrderRequest stopRequestAt(std::span<const std::byte> image, std::size_t at) noexcept {
            OrderRequest request{};
            request.id = load32(image, at + stop::id);
            request.side = *sideOf(load8(image, at + stop::side));
            request.price = load32(image, at + stop::limitPrice);
            request.quantity = load32(image, at + stop::quantity);
            request.type = *orderTypeOf(load8(image, at + stop::type));
            request.timeInForce = *timeInForceOf(load8(image, at + stop::timeInForce));
            request.stopPrice = load32(image, at + stop::stopPrice);
            request.postOnly = load8(image, at + stop::postOnly) == 1;
            request.owner = load32(image, at + stop::owner);
            request.selfTradePrevention = *selfTradePreventionOf(load8(image, at + stop::selfTradePrevention));
            return request;
        }
    }  // namespace

    std::size_t OrderBook::imageSize() const noexcept {
        return headerBytes + levelBytes * (bids.size() + asks.size()) + orderBytes * orderIndex.size() +
               stopBytes * pendingIndex.size();
    }

    std::size_t OrderBook::writeImage(std::span<std::byte> out) const noexcept {
        if(out.size() < headerBytes) {
            return 0;
        }
        std::size_t at = headerBytes;
        std::size_t ordersWritten = 0;
        std::size_t stopsWritten = 0;
        // Each level's room is checked before it is written, against its own queue's length, so
        // nothing reaches past out whatever the counts elsewhere say.
        const auto writeLevels = [&](const auto& levels) noexcept {
            for(const auto& [price, priceLevel] : levels) {
                const std::size_t count = priceLevel.orders.size();
                const std::size_t room = out.size() - at;
                if(room < levelBytes || (room - levelBytes) / orderBytes < count) {
                    return false;
                }
                store32(out, at + level::price, price);
                store32(out, at + level::count, static_cast<std::uint32_t>(count));
                store64(out, at + level::aggregate, priceLevel.quantity);
                at += levelBytes;
                for(const RestingOrder& resting : priceLevel.orders) {
                    store32(out, at + order::id, resting.orderId);
                    store32(out, at + order::sequence, resting.sequenceNum);
                    store32(out, at + order::initial, resting.initialQuantity);
                    store32(out, at + order::remaining, resting.remainingQuantity);
                    store32(out, at + order::owner, resting.owner);
                    store8(out, at + order::fill, fillStatusByte(resting.fillStatus));
                    store8(out, at + order::reserved, 0);
                    store8(out, at + order::reserved + 1, 0);
                    store8(out, at + order::reserved + 2, 0);
                    at += orderBytes;
                }
                ordersWritten += count;
            }
            return true;
        };
        const auto writeStops = [&](const auto& levels) noexcept {
            for(const auto& [stopPrice, stopLevel] : levels) {
                const std::size_t count = stopLevel.stops.size();
                if((out.size() - at) / stopBytes < count) {
                    return false;
                }
                for(const PendingStop& pending : stopLevel.stops) {
                    const OrderRequest& request = pending.request;
                    store32(out, at + stop::id, request.id);
                    store32(out, at + stop::sequence, pending.sequenceNum);
                    store32(out, at + stop::stopPrice, request.stopPrice);
                    store32(out, at + stop::limitPrice, request.price);
                    store32(out, at + stop::quantity, request.quantity);
                    store32(out, at + stop::owner, request.owner);
                    store8(out, at + stop::side, sideByte(request.side));
                    store8(out, at + stop::type, orderTypeByte(request.type));
                    store8(out, at + stop::timeInForce, timeInForceByte(request.timeInForce));
                    store8(out, at + stop::selfTradePrevention, selfTradePreventionByte(request.selfTradePrevention));
                    store8(out, at + stop::postOnly, request.postOnly ? 1 : 0);
                    store8(out, at + stop::reserved, 0);
                    store8(out, at + stop::reserved + 1, 0);
                    store8(out, at + stop::reserved + 2, 0);
                    at += stopBytes;
                }
                stopsWritten += count;
            }
            return true;
        };
        if(!writeLevels(bids) || !writeLevels(asks) || !writeStops(buyStops) || !writeStops(sellStops)) {
            return 0;
        }
        // The header last, once the counts are known.
        const std::uint32_t fields[] = {
            imageVersion,
            minimumReaderVersion,
            static_cast<std::uint32_t>(headerBytes),
            static_cast<std::uint32_t>(levelBytes),
            static_cast<std::uint32_t>(orderBytes),
            static_cast<std::uint32_t>(stopBytes),
            nextSequenceNum,
            nextTradeId,
            lastTradePrice.value_or(0),
            static_cast<std::uint32_t>(bids.size()),
            static_cast<std::uint32_t>(asks.size()),
            static_cast<std::uint32_t>(ordersWritten),
            static_cast<std::uint32_t>(stopsWritten),
            static_cast<std::uint32_t>(orderIndex.size()),
            static_cast<std::uint32_t>(pendingIndex.size()),
            0,
        };
        for(std::size_t field = 0; field < std::size(fields); ++field) {
            store32(out, field * 4, fields[field]);
        }
        return at;
    }

    std::variant<OrderBook, RestoreError> OrderBook::restore(std::span<const std::byte> image) {
        // First every check the bytes alone can make, so a bad image allocates nothing.
        Layout layout;
        if(auto error = checkImageHeader(image, layout)) {
            return std::move(*error);
        }
        std::size_t stopsStart = layout.headerStride;
        if(auto error = checkLevelRecords(image, layout, stopsStart)) {
            return std::move(*error);
        }
        if(auto error = checkStopRecords(image, layout, stopsStart)) {
            return std::move(*error);
        }

        // Where each level and stop level starts. Levels are created worst first, since a level
        // array keeps its best at the back, so building best first would shift every level.
        struct LevelStart {
            std::size_t offset;
            Price price;
        };
        std::vector<LevelStart> levelStarts;
        levelStarts.reserve(std::size_t{layout.bidLevels} + layout.askLevels);
        std::size_t position = layout.headerStride;
        for(std::uint32_t index = 0; index < layout.bidLevels + layout.askLevels; ++index) {
            levelStarts.push_back(LevelStart{position, load32(image, position + level::price)});
            position += layout.levelStride + std::size_t{load32(image, position + level::count)} * layout.orderStride;
        }
        struct StopRun {
            std::size_t first;  // the index of its first stop record
            Side side;
            Price stopPrice;
        };
        std::vector<StopRun> stopRuns;
        std::size_t buyStopLevels = 0;
        for(std::uint32_t index = 0; index < layout.stops; ++index) {
            const std::size_t at = stopsStart + std::size_t{index} * layout.stopStride;
            const Side side = *sideOf(load8(image, at + stop::side));
            const Price stopPrice = load32(image, at + stop::stopPrice);
            if(stopRuns.empty() || stopRuns.back().side != side || stopRuns.back().stopPrice != stopPrice) {
                stopRuns.push_back(StopRun{index, side, stopPrice});
                buyStopLevels += side == Side::BUY ? 1 : 0;
            }
        }

        OrderBook book;
        NodePool* const pool = book.nodePool.get();
        book.orderIndex.reserve(layout.orders);
        book.pendingIndex.reserve(layout.stops);
        book.bids.reserve(layout.bidLevels);
        book.asks.reserve(layout.askLevels);
        book.buyStops.reserve(buyStopLevels);
        book.sellStops.reserve(stopRuns.size() - buyStopLevels);
        if(layout.stops > 0) {
            book.triggeredStops = std::make_unique<StopQueue>(PoolAllocator<PendingStop>(pool));
        }

        std::vector<PriceLevel*> levels(levelStarts.size());
        for(std::size_t index = levelStarts.size(); index-- > 0;) {
            const Price price = levelStarts[index].price;
            levels[index] = index < layout.bidLevels ? &book.bids.try_emplace(price, price, Side::BUY, pool).first->second
                                                     : &book.asks.try_emplace(price, price, Side::SELL, pool).first->second;
        }
        std::vector<StopLevel*> stopLevels(stopRuns.size());
        for(std::size_t index = stopRuns.size(); index-- > 0;) {
            const StopRun& run = stopRuns[index];
            stopLevels[index] = run.side == Side::BUY
                                    ? &book.buyStops.try_emplace(run.stopPrice, run.stopPrice, Side::BUY, pool).first->second
                                    : &book.sellStops.try_emplace(run.stopPrice, run.stopPrice, Side::SELL, pool).first->second;
        }

        // Then every record into its level, in image order, so a duplicate is reported where it
        // appears second.
        const auto duplicate = [&book](OrderId id) {
            return book.orderIndex.find(id) != book.orderIndex.end() || book.pendingIndex.find(id) != book.pendingIndex.end();
        };
        for(std::size_t index = 0; index < levelStarts.size(); ++index) {
            PriceLevel& priceLevel = *levels[index];
            const std::size_t at = levelStarts[index].offset;
            const std::uint32_t count = load32(image, at + level::count);
            for(std::uint32_t queued = 0; queued < count; ++queued) {
                const std::size_t record = at + layout.levelStride + std::size_t{queued} * layout.orderStride;
                const OrderId id = load32(image, record + order::id);
                if(duplicate(id)) {
                    return refusal(RestoreErrorCode::DUPLICATE_ID, record + order::id, "an ID is live twice");
                }
                // Built with placeholders and then filled in field by field, so a field Order
                // would refuse reaches checkInvariants as a malformed order instead of throwing.
                Order resting(1, 1, 1, 1, priceLevel.side);
                resting.orderId = id;
                resting.sequenceNum = load32(image, record + order::sequence);
                resting.priceTick = priceLevel.tickPrice;
                resting.initialQuantity = load32(image, record + order::initial);
                resting.remainingQuantity = load32(image, record + order::remaining);
                resting.fillStatus = *restingFillStatusOf(load8(image, record + order::fill));
                const OrderLocation location = priceLevel.addOrder(resting, load32(image, record + order::owner));
                book.orderIndex.emplace(id, location);
            }
        }
        for(std::size_t run = 0; run < stopRuns.size(); ++run) {
            StopLevel& stopLevel = *stopLevels[run];
            const std::size_t end = run + 1 < stopRuns.size() ? stopRuns[run + 1].first : layout.stops;
            for(std::size_t index = stopRuns[run].first; index < end; ++index) {
                const std::size_t record = stopsStart + index * layout.stopStride;
                const OrderRequest request = stopRequestAt(image, record);
                if(duplicate(request.id)) {
                    return refusal(RestoreErrorCode::DUPLICATE_ID, record + stop::id, "an ID is live twice");
                }
                stopLevel.stops.push_back(PendingStop{request, load32(image, record + stop::sequence)});
                book.pendingIndex.emplace(
                    request.id, StopLocation(stopLevel.stopPrice, stopLevel.side, std::prev(stopLevel.stops.end()), &stopLevel));
            }
        }

        book.nextSequenceNum = layout.nextSequence;
        book.nextTradeId = layout.nextTrade;
        if(layout.lastTrade != 0) {
            book.lastTradePrice = layout.lastTrade;
        }
        // Sized for the image: what restore built is not a growth. Its indexes hold what it holds,
        // but each level array only its own levels, so no spare level is claimed and a later
        // reserve tops every array up.
        book.reservedCapacity = BookCapacity{layout.orders, layout.stops, 0};
        book.reservedChunks = pool->chunksTaken();

        // In every build, whatever ORDER_BOOK_INVARIANT_CHECKS says.
        const InvariantResult checked = book.checkInvariants();
        if(!checked.isValid()) {
            return RestoreError{RestoreErrorCode::INVARIANT_VIOLATION, 0,
                                "the rebuilt book fails checkInvariants: " + formatInvariantResult(checked), checked};
        }
        return std::variant<OrderBook, RestoreError>(std::in_place_type<OrderBook>, std::move(book));
    }

}  // namespace order_book
