#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "order_book/order_book.hpp"
#include "order_book/order_type.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/self_trade_prevention.hpp"
#include "order_book/side.hpp"
#include "order_book/symbol.hpp"
#include "order_book/time_in_force.hpp"
#include "order_book/types.hpp"

// The browser's book view (ADR 0018, docs/internals/wasm.md): a shadow book per instrument,
// running every log record again as it passes to the page store, and what its image says,
// owners and pending stops included. Not public API.
namespace order_book::wasm {

    // A book as its image (docs/checkpoint-format.md) holds it, decoded field by field.
    struct RestingOrderView {
        OrderId orderId = 0;
        SequenceNum sequence = 0;
        Quantity initialQuantity = 0;
        Quantity remainingQuantity = 0;
        OwnerId owner = 0;  // 0 for none

        bool operator==(const RestingOrderView&) const = default;
    };

    struct LevelView {
        Price price = 0;
        std::uint64_t quantity = 0;  // the sum of its orders' remaining quantities
        std::vector<RestingOrderView> orders;  // in queue order

        bool operator==(const LevelView&) const = default;
    };

    struct PendingStopView {
        OrderId orderId = 0;
        SequenceNum sequence = 0;
        Side side = Side::BUY;
        OrderType orderType = OrderType::STOP;
        Price stopPrice = 0;
        Price limitPrice = 0;  // 0 for a STOP, which has none
        Quantity quantity = 0;
        TimeInForce timeInForce = TimeInForce::GTC;
        OwnerId owner = 0;  // 0 for none
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;
        bool postOnly = false;

        bool operator==(const PendingStopView&) const = default;
    };

    struct BookView {
        std::vector<LevelView> bids;  // best (highest) first
        std::vector<LevelView> asks;  // best (lowest) first
        // The buy stops by ascending stop price, then the sell stops by descending stop price,
        // each stop price's in queue order: trigger order.
        std::vector<PendingStopView> stops;
        std::optional<Price> lastTradePrice;

        bool operator==(const BookView&) const = default;
    };

    // Decodes an image writeImage wrote, at version 1's sizes. Throws std::invalid_argument for
    // bytes that aren't one: it reads, and never judges, a book.
    BookView decodeBookImage(std::span<const std::byte> image);

    // The highest order ID resting or pending in it, 0 for none.
    OrderId highestOrderId(const BookView& book) noexcept;

    // One shadow book per instrument of the directory. rebuild reads the store as a start does,
    // read only; follow then takes every change the store reports, and runs each log record a
    // write carries on its instrument's book, as its shard did. Any thread may call any member:
    // one lock guards them all. follow runs under the store's lock and never calls the store.
    class ShadowBooks {
    public:
        // Empty books at the directory's capacities: a store with nothing in it.
        ShadowBooks(const runtime::InstrumentDirectory& directory, const BookCapacity& defaults = runtime::defaultBookCapacity);

        ShadowBooks(const ShadowBooks&) = delete;
        ShadowBooks& operator=(const ShadowBooks&) = delete;

        // Each book as the store's log and checkpoints leave it: surveyLog, then recoverShard for
        // each shard, read only, as verify reads a store. Call it while nothing writes the store.
        // A store recovery refuses leaves every book empty and a problem saying why, since the
        // start that follows refuses it too.
        void rebuild(runtime::LogStorage& store);

        // One change the page store is told of. A write that starts a log file gives its header;
        // a write past it gives records, which must come whole and in order, each checked against
        // the chain and run. Anything else in a log file the shadows follow, or a record that
        // doesn't decode, belongs to its shard, or come in order, is a problem, and the shadows
        // follow nothing after it. Never throws.
        void follow(const runtime::PageLogStorage::Change& change) noexcept;

        // The first thing that went wrong, or nothing while the shadows equal the shards.
        std::optional<std::string> problem() const;

        // The book's image, as writeImage gives it, and decoded. Throws std::out_of_range for a
        // symbol not on the list.
        std::vector<std::byte> image(const replay::Symbol& symbol) const;
        BookView book(const replay::Symbol& symbol) const;

        const runtime::InstrumentDirectory& instruments() const noexcept { return directory; }
        // The instrument's shard, or nothing for a symbol not on the list.
        std::optional<std::uint32_t> shardOf(const replay::Symbol& symbol) const noexcept;
        // How many records the shard's log holds: its next log position.
        std::uint64_t records(std::uint32_t shard) const;

        // The next order ID for the instrument: 1 + the larger of the highest ID its shadow has
        // seen, in a record it ran or a book it rebuilt, and the highest this has given, which it
        // then is. Nothing once no ID is left, or for a symbol not on the list.
        std::optional<OrderId> assignOrderId(const replay::Symbol& symbol);

    private:
        struct Shadow {
            OrderBook book;
            OrderId highestSeen = 0;
            OrderId highestAssigned = 0;
        };
        // A log file the store holds: one made since the last rebuild, followed record by record,
        // or one made before, closed, since a start always opens a new one.
        struct FollowedFile {
            bool closed = false;  // in the store at the rebuild, so never written to again
            bool hasHeader = false;
            std::uint32_t shard = 0;
            std::uint64_t firstPosition = 0;
            std::uint64_t recordsStart = 0;  // the header's whole length
            std::uint64_t records = 0;       // run so far
            std::uint32_t checksum = 0;      // the last record's, or the header's chain seed
        };

        const Shadow& at(const replay::Symbol& symbol) const;
        void emptyBooks();
        void followWrite(const runtime::PageLogStorage::Change& change);
        void fail(std::string what);

        runtime::InstrumentDirectory directory;
        BookCapacity defaults;
        mutable std::mutex guard;
        std::vector<std::vector<Shadow>> shadows;  // by shard, then slot
        std::vector<std::uint64_t> logged;          // by shard: records
        std::map<std::string, FollowedFile> files;  // by name, as the store holds it
        std::optional<std::string> failure;
    };

}  // namespace order_book::wasm
