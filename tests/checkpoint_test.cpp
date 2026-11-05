#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/book_image.hpp"
#include "order_book/event_format.hpp"
#include "order_book/image_header.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"
#include "order_book/replace_request.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/symbol.hpp"
#include "order_book/xxh64.hpp"

// The book image (docs/checkpoint-format.md) through OrderBook's public API: golden samples,
// writeImage(restore(x)) == x, continuation at every command boundary, one refusal per code, a
// byte sweep, and writeImage's span. Expected bytes come from the format document, through
// make_golden.py or the builder below, never from writeImage. Then the checkpoint file that wraps
// an image, through the runtime's codec: golden files, round trips, one refusal per code, every
// bit flipped and every truncation refused, and verify and describe.
namespace {

using namespace order_book;
using Bytes = std::vector<std::byte>;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Bytes imageOf(const OrderBook& book) {
    Bytes image(book.imageSize());
    const std::size_t written = book.writeImage(image);
    expect(written == image.size(), "writeImage fills exactly imageSize bytes: wrote " + std::to_string(written) +
                                        " of " + std::to_string(image.size()));
    return image;
}

std::string describe(const RestoreError& error) {
    std::string text = std::string(restoreErrorCodeName(error.code)) + " at " + std::to_string(error.offset) + ": " +
                       error.message;
    if (error.invariant) {
        text += " [" + formatInvariantResult(*error.invariant) + "]";
    }
    return text;
}

OrderBook restored(std::span<const std::byte> image, const std::string& what) {
    auto result = OrderBook::restore(image);
    if (auto* error = std::get_if<RestoreError>(&result)) {
        expect(false, what + ": restore refused a good image: " + describe(*error));
    }
    return std::move(std::get<OrderBook>(result));
}

RestoreError refused(std::span<const std::byte> image, const std::string& what) {
    auto result = OrderBook::restore(image);
    expect(std::holds_alternative<RestoreError>(result), what + ": restore accepted a bad image");
    return std::get<RestoreError>(std::move(result));
}

void expectRefusal(std::span<const std::byte> image, RestoreErrorCode code, std::size_t offset, const std::string& what) {
    const RestoreError error = refused(image, what);
    expect(error.code == code && error.offset == offset,
           what + ": expected " + restoreErrorCodeName(code) + " at " + std::to_string(offset) + ", got " +
               describe(error));
    expect(!error.message.empty(), what + ": the refusal has a message");
    expect(error.invariant.has_value() == (code == RestoreErrorCode::INVARIANT_VIOLATION),
           what + ": only an invariant violation carries an InvariantResult");
}

// Every reading the public API gives of a book, as text, for the ids that may be live in it.
std::string publicReads(const OrderBook& book, OrderId highestId) {
    std::ostringstream text;
    text << "orders=" << book.activeOrderCount() << " stops=" << book.pendingStopCount() << " last=";
    if (const auto last = book.getLastTradePrice()) {
        text << *last;
    } else {
        text << "none";
    }
    const BookSnapshot snapshot = book.snapshot(1000);
    for (const auto& level : snapshot.bids) {
        text << " bid " << level.price << ':' << level.totalQuantity << '/' << level.orderCount;
    }
    for (const auto& level : snapshot.asks) {
        text << " ask " << level.price << ':' << level.totalQuantity << '/' << level.orderCount;
    }
    if (book.hasBids()) {
        text << " best-bid " << book.getBestBid().getOrderId();
    }
    if (book.hasAsks()) {
        text << " best-ask " << book.getBestAsk().getOrderId();
    }
    for (OrderId id = 1; id <= highestId && id != 0; ++id) {
        if (const auto stop = book.findPendingStop(id)) {
            text << " stop " << id << ' ' << static_cast<int>(stop->side) << ' ' << stop->price << ' ' << stop->quantity
                 << ' ' << static_cast<int>(stop->type) << ' ' << static_cast<int>(stop->timeInForce) << ' '
                 << stop->stopPrice << ' ' << stop->postOnly << ' ' << stop->owner << ' '
                 << static_cast<int>(stop->selfTradePrevention);
            continue;
        }
        try {
            const Order order = book.findActiveOrder(id);
            text << " order " << id << ' ' << order.getSequenceNum() << ' ' << order.getPriceTick() << ' '
                 << order.getInitialQuantity() << ' ' << order.getRemainingQuantity() << ' '
                 << static_cast<int>(order.getFillStatus());
        } catch (const std::invalid_argument&) {
        }
    }
    return text.str();
}

// writeImage(restore(x)) == x, the restored book's public reads equal the original's, and it is
// sound and sized for what it holds.
void expectRoundTrip(const OrderBook& book, OrderId highestId, const std::string& what) {
    const Bytes image = imageOf(book);
    const OrderBook copy = restored(image, what);
    expect(imageOf(copy) == image, what + ": writeImage(restore(x)) == x byte for byte");
    expect(publicReads(copy, highestId) == publicReads(book, highestId),
           what + ": public reads agree\n  original: " + publicReads(book, highestId) +
               "\n  restored: " + publicReads(copy, highestId));
    expect(copy.checkInvariants().isValid(), what + ": the restored book is sound");
    expect(copy.capacityGrowths() == 0, what + ": a restored book reports no growth");
    const BookCapacity held = copy.holding();
    const BookCapacity original = book.holding();
    expect(held.orders == original.orders && held.stops == original.stops && held.levels == original.levels,
           what + ": the restored book holds what the original did");
}

// ---- The format, written from docs/checkpoint-format.md ------------------------------------

// The values these cases use; the format defines the rest.
constexpr std::uint8_t buy = 1, sell = 2;
constexpr std::uint8_t typeStop = 3, typeStopLimit = 4;
constexpr std::uint8_t gtc = 1, ioc = 2;
constexpr std::uint8_t cancelResting = 1, cancelBoth = 3;
constexpr std::uint8_t fillNone = 1, fillPartial = 2, fillFilled = 3;

void put8(Bytes& bytes, std::size_t offset, std::uint8_t value) {
    bytes.at(offset) = static_cast<std::byte>(value);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        put8(bytes, offset + i, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

void put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        put8(bytes, offset + i, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

void append32(Bytes& bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 4);
    put32(bytes, bytes.size() - 4, value);
}

struct ImageCounts {
    std::uint32_t nextSequence = 1;
    std::uint32_t nextTrade = 1;
    std::uint32_t lastTrade = 0;
    std::uint32_t bidLevels = 0;
    std::uint32_t askLevels = 0;
    std::uint32_t orders = 0;
    std::uint32_t stops = 0;
};

Bytes header(const ImageCounts& counts) {
    Bytes bytes;
    for (const std::uint32_t field : {1U, 1U, 64U, 16U, 24U, 32U, counts.nextSequence, counts.nextTrade,
                                      counts.lastTrade, counts.bidLevels, counts.askLevels, counts.orders,
                                      counts.stops, counts.orders, counts.stops, 0U}) {
        append32(bytes, field);
    }
    return bytes;
}

void appendLevel(Bytes& bytes, std::uint32_t price, std::uint32_t orderCount, std::uint64_t aggregate) {
    append32(bytes, price);
    append32(bytes, orderCount);
    bytes.resize(bytes.size() + 8);
    put64(bytes, bytes.size() - 8, aggregate);
}

void appendOrder(Bytes& bytes, std::uint32_t id, std::uint32_t sequence, std::uint32_t initial, std::uint32_t remaining,
                 std::uint32_t owner = 0, std::uint8_t fill = fillNone) {
    for (const std::uint32_t field : {id, sequence, initial, remaining, owner}) {
        append32(bytes, field);
    }
    bytes.push_back(static_cast<std::byte>(fill));
    bytes.resize(bytes.size() + 3);
}

void appendStop(Bytes& bytes, std::uint32_t id, std::uint32_t sequence, std::uint32_t stopPrice, std::uint32_t limit,
                std::uint32_t quantity, std::uint8_t side, std::uint8_t type, std::uint8_t tif, std::uint32_t owner = 0,
                std::uint8_t mode = cancelResting, std::uint8_t postOnly = 0) {
    for (const std::uint32_t field : {id, sequence, stopPrice, limit, quantity, owner}) {
        append32(bytes, field);
    }
    for (const std::uint8_t field : {side, type, tif, mode, postOnly}) {
        bytes.push_back(static_cast<std::byte>(field));
    }
    bytes.resize(bytes.size() + 3);
}

// The base image the refusals change one field of. Offsets: header 0, bid level 100 at 64 with
// order 1 at 80, bid level 99 at 104 with order 2 at 120, ask level 103 at 144 with order 3 at
// 160, buy stop 4 at 184, sell stop 5 at 216, 248 bytes in all.
Bytes baseImage() {
    Bytes bytes = header(ImageCounts{20, 5, 100, 2, 1, 3, 2});
    appendLevel(bytes, 100, 1, 5);
    appendOrder(bytes, 1, 1, 5, 5);
    appendLevel(bytes, 99, 1, 3);
    appendOrder(bytes, 2, 2, 3, 3, 7);
    appendLevel(bytes, 103, 1, 4);
    appendOrder(bytes, 3, 3, 6, 4, 0, fillPartial);
    appendStop(bytes, 4, 4, 105, 0, 1, buy, typeStop, ioc);
    appendStop(bytes, 5, 5, 95, 90, 2, sell, typeStopLimit, gtc, 3, cancelBoth);
    return bytes;
}

// ---- Books built through the public API ---------------------------------------------------

OrderRequest limit(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner = 0) {
    return OrderRequest{id, side, price, quantity, OrderType::LIMIT, TimeInForce::GTC, 0, false, owner};
}

OrderRequest stopRequest(OrderId id, Side side, OrderType type, TimeInForce tif, Price stopPrice, Price price,
                         Quantity quantity, OwnerId owner = 0,
                         SelfTradePrevention mode = SelfTradePrevention::CANCEL_RESTING) {
    return OrderRequest{id, side, price, quantity, type, tif, stopPrice, false, owner, mode};
}

void submit(OrderBook& book, const OrderRequest& request) {
    const SubmissionResult result = book.submitOrder(request);
    expect(result.getStatus() != SubmissionStatus::REJECTED,
           "building a sample: order " + std::to_string(request.id) + " was rejected");
}

// The commands make_golden.py lists each sample's records from.
OrderBook restingSample() {
    OrderBook book;
    book.submitOrder(Order(1, 100, 10, 1, Side::BUY));
    submit(book, limit(2, Side::BUY, 99, 5, 7));
    book.submitOrder(Order(3, 100, 4, 1, Side::BUY));
    submit(book, limit(4, Side::SELL, 103, 8));
    submit(book, OrderRequest{5, Side::SELL, 100, 6, OrderType::LIMIT, TimeInForce::IOC});
    return book;
}

OrderBook stopsSample() {
    OrderBook book;
    book.submitOrder(Order(1, 100, 5, 1, Side::SELL));
    submit(book, limit(2, Side::BUY, 100, 2));
    submit(book, stopRequest(3, Side::BUY, OrderType::STOP, TimeInForce::IOC, 105, 0, 4));
    submit(book, stopRequest(4, Side::BUY, OrderType::STOP_LIMIT, TimeInForce::GTC, 102, 107, 1, 9,
                             SelfTradePrevention::CANCEL_BOTH));
    submit(book, stopRequest(5, Side::SELL, OrderType::STOP, TimeInForce::FOK, 95, 0, 2));
    submit(book, stopRequest(6, Side::SELL, OrderType::STOP_LIMIT, TimeInForce::IOC, 97, 90, 3, 0,
                             SelfTradePrevention::CANCEL_INCOMING));
    submit(book, stopRequest(7, Side::BUY, OrderType::STOP, TimeInForce::IOC, 102, 0, 1));
    return book;
}

OrderBook maximalSample() {
    constexpr unsigned int max = 0xFFFFFFFFU;
    OrderBook book;
    submit(book, limit(max, Side::BUY, max, max, max));
    submit(book, limit(max - 1, Side::BUY, max, max));
    return book;
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(file.good(), "cannot open " + path.string());
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Bytes bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return bytes;
}

void testGoldenSamples(const std::filesystem::path& root) {
    const std::pair<const char*, OrderBook> samples[] = {
        {"empty", OrderBook{}},
        {"resting", restingSample()},
        {"stops", stopsSample()},
        {"maximal", maximalSample()},
    };
    for (const auto& [name, book] : samples) {
        const std::string what = std::string("golden ") + name;
        const Bytes golden = readFile(root / "images" / (std::string(name) + ".image"));
        expect(book.imageSize() == golden.size(), what + ": imageSize is the sample's length");
        expect(imageOf(book) == golden, what + ": writeImage gives the hand-listed bytes");
        const OrderBook copy = restored(golden, what);
        expect(imageOf(copy) == golden, what + ": restoring the sample writes it back");
        expect(publicReads(copy, 10) == publicReads(book, 10) || name == std::string("maximal"),
               what + ": the restored book reads as the one built");
    }
    // What only the image holds: initial quantities, owners, the last trade price, every field
    // of a pending stop.
    const OrderBook stops = restored(readFile(root / "images" / "stops.image"), "golden stops");
    expect(stops.getLastTradePrice() == std::optional<Price>(100), "the last trade price is restored");
    expect(stops.findActiveOrder(1).getInitialQuantity() == 5, "an initial quantity is restored");
    expect(stops.findPendingStop(4) == stopRequest(4, Side::BUY, OrderType::STOP_LIMIT, TimeInForce::GTC, 102, 107, 1,
                                                   9, SelfTradePrevention::CANCEL_BOTH),
           "a pending stop's whole request is restored");
    const OrderBook maximal = restored(readFile(root / "images" / "maximal.image"), "golden maximal");
    expect(maximal.snapshot(1).bids.at(0).totalQuantity == 0x1FFFFFFFEULL, "an aggregate above one quantity");
}

// ---- Round trips -------------------------------------------------------------------------

void testRoundTrips() {
    expectRoundTrip(OrderBook{}, 1, "empty book");
    expectRoundTrip(restingSample(), 10, "both sides, partly filled, an owner");
    expectRoundTrip(stopsSample(), 10, "stops on both sides, mixed types");
    expectRoundTrip(maximalSample(), 0, "maximal values");

    {
        // Partly filled and replaced orders: a replace in place lowers the total and keeps the
        // priority, a replace to another price takes a new sequence number, one that increases
        // goes to the back.
        OrderBook book;
        submit(book, limit(1, Side::SELL, 101, 10, 4));
        submit(book, limit(2, Side::SELL, 101, 6));
        submit(book, limit(3, Side::BUY, 101, 3));
        expect(book.replaceOrder(ReplaceRequest{1, 101, 8, false, 4}).isSuccess(), "replace in place");
        expect(book.replaceOrder(ReplaceRequest{2, 102, 6}).isSuccess(), "replace to a new price");
        submit(book, limit(4, Side::SELL, 101, 2));
        expect(book.replaceOrder(ReplaceRequest{4, 101, 9, false, 5, SelfTradePrevention::CANCEL_BOTH}).isSuccess(),
               "replace with an increase");
        expectRoundTrip(book, 10, "partly filled and replaced orders");
    }
    {
        // Reused IDs: a cancelled, a filled, and an expired order's IDs rest again.
        OrderBook book;
        submit(book, limit(1, Side::BUY, 90, 5));
        book.cancelOrder(1);
        submit(book, limit(1, Side::BUY, 91, 5));
        submit(book, limit(2, Side::SELL, 91, 5));
        submit(book, limit(2, Side::SELL, 95, 1));
        submit(book, OrderRequest{3, Side::BUY, 0, 1, OrderType::MARKET, TimeInForce::IOC});
        submit(book, limit(3, Side::BUY, 92, 7, 2));
        expectRoundTrip(book, 10, "reused IDs");
    }
}

// A queue across a sequence-number wrap, written by hand, since a book reaches it only after
// 2^32 acceptances: order 10 took the last number before the wrap and order 11 the first after.
void testSequenceWrap() {
    Bytes image = header(ImageCounts{2, 1, 0, 1, 0, 2, 0});
    appendLevel(image, 100, 2, 9);
    appendOrder(image, 10, 0xFFFFFFFFU, 4, 4);
    appendOrder(image, 11, 1, 5, 5);
    OrderBook book = restored(image, "a queue across a wrap");
    expect(imageOf(book) == image, "a queue across a wrap round-trips");
    submit(book, limit(12, Side::BUY, 100, 1));
    expect(book.findActiveOrder(12).getSequenceNum() == 2, "numbering carries on from the image");
    const SubmissionResult sale = book.submitOrder(limit(13, Side::SELL, 100, 6));
    const auto& trades = sale.getGeneratedTrades();
    expect(trades.size() == 2 && trades[0].getRestingId() == 10 && trades[1].getRestingId() == 11,
           "queue position, not sequence number, decides who trades first");
    expectRoundTrip(book, 20, "after trading across the wrap");
}

// ---- Continuation ------------------------------------------------------------------------

using Command = std::variant<OrderRequest, ReplaceRequest, OrderId>;

// A seeded script of every kind of command, on a narrow band of prices and a few owners and IDs,
// so it matches, prevents self-trades, triggers stops, cascades, reuses IDs, and is refused.
std::vector<Command> script(std::uint64_t seed, std::size_t length) {
    std::mt19937_64 random(seed);
    const auto below = [&random](std::uint64_t bound) { return static_cast<unsigned int>(random() % bound); };
    std::vector<Command> commands;
    for (std::size_t i = 0; i < length; ++i) {
        const OrderId id = 1 + below(40);
        const Side side = below(2) == 0 ? Side::BUY : Side::SELL;
        const Price price = 95 + below(11);
        const Quantity quantity = 1 + below(8);
        const OwnerId owner = below(4);
        const auto mode = static_cast<SelfTradePrevention>(below(3));
        const unsigned int kind = below(20);
        if (kind < 8) {
            commands.emplace_back(OrderRequest{id, side, price, quantity, OrderType::LIMIT, TimeInForce::GTC, 0,
                                               below(8) == 0, owner, mode});
        } else if (kind < 10) {
            const auto tif = below(2) == 0 ? TimeInForce::IOC : TimeInForce::FOK;
            const bool market = below(3) == 0;
            commands.emplace_back(OrderRequest{id, side, market ? 0 : price, quantity,
                                               market ? OrderType::MARKET : OrderType::LIMIT, tif, 0, false, owner,
                                               mode});
        } else if (kind < 13) {
            const bool stopLimit = below(2) == 0;
            const auto tif = stopLimit ? static_cast<TimeInForce>(below(3))
                                       : (below(2) == 0 ? TimeInForce::IOC : TimeInForce::FOK);
            commands.emplace_back(OrderRequest{id, side, stopLimit ? price : 0, quantity,
                                               stopLimit ? OrderType::STOP_LIMIT : OrderType::STOP, tif,
                                               95 + below(11), false, owner, mode});
        } else if (kind < 16) {
            commands.emplace_back(id);
        } else {
            const Quantity total = 1 + below(12);
            const bool postOnly = below(6) == 0;
            commands.emplace_back(ReplaceRequest{id, price, total, postOnly, owner, mode});
        }
    }
    return commands;
}

// What one command reported, as text, so two runs compare with one ==.
std::string run(OrderBook& book, const Command& command) {
    std::ostringstream text;
    const auto events = [&text](const auto& list) {
        for (const BookEvent& event : list) {
            text << formatEvent(event) << '\n';
        }
    };
    if (const auto* request = std::get_if<OrderRequest>(&command)) {
        const SubmissionResult result = book.submitOrder(*request);
        events(result.getEvents());
        text << "remaining=" << result.getRemainingQuantity() << " fill=" << static_cast<int>(result.getFillStatus());
    } else if (const auto* replace = std::get_if<ReplaceRequest>(&command)) {
        const ReplaceResult result = book.replaceOrder(*replace);
        events(result.getEvents());
        text << "remaining=" << result.getRemainingQuantity() << " fill=" << static_cast<int>(result.getFillStatus());
    } else {
        events(book.cancelOrder(std::get<OrderId>(command)).getEvents());
    }
    return text.str();
}

// At every boundary of each script: image the book, restore it, run the rest on both, and
// compare every result and the final image.
void testContinuation() {
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        const std::vector<Command> commands = script(seed, 200);
        {
            // The script reaches what the image must carry across a boundary.
            OrderBook book;
            std::string all;
            for (const Command& command : commands) {
                all += run(book, command);
            }
            for (const char* event : {"TradeExecuted", "StopTriggered", "SelfTradePrevented", "OrderReplaced",
                                      "OrderExpired", "OrderCancelled", "OrderRejected"}) {
                expect(all.find(event) != std::string::npos,
                       "seed " + std::to_string(seed) + ": the script makes a " + event);
            }
        }
        for (std::size_t boundary = 0; boundary <= commands.size(); ++boundary) {
            const std::string where = "seed " + std::to_string(seed) + " boundary " + std::to_string(boundary);
            OrderBook original;
            for (std::size_t i = 0; i < boundary; ++i) {
                run(original, commands[i]);
            }
            const Bytes image = imageOf(original);
            OrderBook copy = restored(image, where);
            expect(imageOf(copy) == image, where + ": writeImage(restore(x)) == x");
            for (std::size_t i = boundary; i < commands.size(); ++i) {
                const std::string expected = run(original, commands[i]);
                const std::string actual = run(copy, commands[i]);
                expect(actual == expected, where + ", command " + std::to_string(i) + ": the restored book reports\n" +
                                               actual + "\nwhere the original reports\n" + expected);
            }
            expect(imageOf(copy) == imageOf(original), where + ": the final images agree");
        }
    }
}

// ---- Refusals, one case per code ----------------------------------------------------------

void testRefusals() {
    const Bytes base = baseImage();
    restored(base, "the base image");

    {
        const Bytes cut(base.begin(), base.begin() + 63);
        expectRefusal(cut, RestoreErrorCode::TOO_SHORT, 63, "shorter than a header");
        Bytes empty = header(ImageCounts{});
        put32(empty, 8, 80);
        expectRefusal(empty, RestoreErrorCode::TOO_SHORT, 64, "shorter than its stated header");
    }
    {
        Bytes image = base;
        put32(image, 4, 2);
        expectRefusal(image, RestoreErrorCode::UNSUPPORTED_VERSION, 4, "a minimum reader version of 2");
        image = base;
        put32(image, 0, 0);
        expectRefusal(image, RestoreErrorCode::UNSUPPORTED_VERSION, 0, "image version 0");
    }
    {
        Bytes image = base;
        put32(image, 16, 20);
        expectRefusal(image, RestoreErrorCode::SIZE_BELOW_KNOWN, 16, "an order record below 24 bytes");
        image = base;
        put32(image, 8, 60);
        expectRefusal(image, RestoreErrorCode::SIZE_BELOW_KNOWN, 8, "a header below 64 bytes");
    }
    {
        Bytes image = base;
        image.push_back(std::byte{0});
        expectRefusal(image, RestoreErrorCode::LENGTH_MISMATCH, 249, "a trailing byte");
        image = base;
        put32(image, 48, 3);
        put32(image, 56, 3);
        expectRefusal(image, RestoreErrorCode::LENGTH_MISMATCH, 248, "a count the length does not hold");
    }
    {
        Bytes image = base;
        put32(image, 28, 0);
        expectRefusal(image, RestoreErrorCode::ZERO_FIELD, 28, "a zero next trade ID");
        image = base;
        put32(image, 24, 0);
        expectRefusal(image, RestoreErrorCode::ZERO_FIELD, 24, "a zero next sequence number");
        image = base;
        put32(image, 80, 0);
        expectRefusal(image, RestoreErrorCode::ZERO_FIELD, 80, "a zero order ID");
        image = base;
        put32(image, 216, 0);
        expectRefusal(image, RestoreErrorCode::ZERO_FIELD, 216, "a zero stop ID");
    }
    {
        Bytes image = base;
        put8(image, 61, 1);
        expectRefusal(image, RestoreErrorCode::NONZERO_RESERVED, 60, "the header's reserved word");
        image = base;
        put8(image, 102, 1);
        expectRefusal(image, RestoreErrorCode::NONZERO_RESERVED, 101, "an order's reserved bytes");
        image = base;
        put8(image, 247, 1);
        expectRefusal(image, RestoreErrorCode::NONZERO_RESERVED, 245, "a stop's reserved bytes");
    }
    {
        Bytes image = base;
        put32(image, 52, 4);
        expectRefusal(image, RestoreErrorCode::INDEX_SIZE_MISMATCH, 52, "an order index larger than its records");
        image = base;
        put32(image, 56, 1);
        expectRefusal(image, RestoreErrorCode::INDEX_SIZE_MISMATCH, 56, "a stop index smaller than its records");
    }
    {
        Bytes image = base;
        put32(image, 68, 0);
        expectRefusal(image, RestoreErrorCode::EMPTY_LEVEL, 68, "a level with no orders");
    }
    {
        Bytes image = base;
        put32(image, 68, 4);
        expectRefusal(image, RestoreErrorCode::ORDER_COUNT_MISMATCH, 68, "order counts past the orders written");
        // One more order written than the levels hold: the image ends with it, after the stops'
        // place by the counts, so reading stops just after the last level's orders.
        image = base;
        put32(image, 44, 4);
        put32(image, 52, 4);
        appendOrder(image, 6, 6, 1, 1);
        expectRefusal(image, RestoreErrorCode::ORDER_COUNT_MISMATCH, 184, "order counts short of the orders written");
    }
    {
        Bytes image = base;
        put32(image, 104, 101);
        expectRefusal(image, RestoreErrorCode::LEVELS_OUT_OF_ORDER, 104, "a bid level above the one before it");
        image = base;
        put32(image, 104, 100);
        expectRefusal(image, RestoreErrorCode::LEVELS_OUT_OF_ORDER, 104, "two bid levels at one price");
        // Two ask levels, the second at 104 below 103 at 64 + 16 + 24.
        image = header(ImageCounts{10, 1, 0, 0, 2, 2, 0});
        appendLevel(image, 103, 1, 1);
        appendOrder(image, 1, 1, 1, 1);
        appendLevel(image, 102, 1, 1);
        appendOrder(image, 2, 2, 1, 1);
        expectRefusal(image, RestoreErrorCode::LEVELS_OUT_OF_ORDER, 104, "an ask level below the one before it");
    }
    {
        Bytes image = base;
        put64(image, 72, 6);
        expectRefusal(image, RestoreErrorCode::AGGREGATE_MISMATCH, 72, "an aggregate above its orders' sum");
    }
    {
        Bytes image = base;
        put8(image, 100, fillFilled);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 100, "a FILLED order");
        image = base;
        put8(image, 100, 0);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 100, "fill status 0");
        image = base;
        put8(image, 209, 5);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 209, "order type 5");
        image = base;
        put8(image, 240, 3);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 240, "side 3");
        image = base;
        put8(image, 210, 0);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 210, "time in force 0");
        image = base;
        put8(image, 211, 4);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 211, "self-trade prevention 4");
        image = base;
        put8(image, 212, 2);
        expectRefusal(image, RestoreErrorCode::INVALID_ENUMERATOR, 212, "post-only 2");
    }
    {
        Bytes image = base;
        put32(image, 92, 6);
        expectRefusal(image, RestoreErrorCode::REMAINING_ABOVE_INITIAL, 92, "remaining above initial");
    }
    {
        Bytes image = base;
        put8(image, 208, sell);
        put8(image, 240, buy);
        expectRefusal(image, RestoreErrorCode::STOPS_OUT_OF_ORDER, 240, "a buy stop after a sell stop");
        image = Bytes(base.begin(), base.begin() + 216);
        put32(image, 48, 2);
        put32(image, 56, 2);
        appendStop(image, 5, 5, 104, 0, 1, buy, typeStop, ioc);
        expectRefusal(image, RestoreErrorCode::STOPS_OUT_OF_ORDER, 224, "a buy stop below the one before it");
        // Two sell stops, the second at 64 + 32 rising from 95 to 96.
        image = header(ImageCounts{10, 1, 0, 0, 0, 0, 2});
        appendStop(image, 1, 1, 95, 0, 1, sell, typeStop, ioc);
        appendStop(image, 2, 2, 96, 0, 1, sell, typeStop, ioc);
        expectRefusal(image, RestoreErrorCode::STOPS_OUT_OF_ORDER, 104, "a sell stop above the one before it");
    }
    {
        Bytes image = base;
        put32(image, 120, 1);
        expectRefusal(image, RestoreErrorCode::DUPLICATE_ID, 120, "an ID resting twice");
        image = base;
        put32(image, 216, 2);
        expectRefusal(image, RestoreErrorCode::DUPLICATE_ID, 216, "an ID both resting and pending");
        image = base;
        put32(image, 216, 4);
        expectRefusal(image, RestoreErrorCode::DUPLICATE_ID, 216, "an ID pending twice");
    }
    {
        // What checkInvariants finds: restore never matches, so a crossing image is refused.
        const auto violation = [](const Bytes& image, InvariantViolation expected, const std::string& what) {
            expectRefusal(image, RestoreErrorCode::INVARIANT_VIOLATION, 0, what);
            const RestoreError error = refused(image, what);
            expect(error.invariant->violation == expected, what + ": " + describe(error));
        };
        Bytes image = base;
        put32(image, 144, 99);
        violation(image, InvariantViolation::CROSSED_BOOK, "a crossing image");
        image = base;
        put32(image, 192, 100);
        violation(image, InvariantViolation::STOP_PENDING_PAST_TRIGGER, "a stop past its trigger");
        image = base;
        put32(image, 84, 0);
        violation(image, InvariantViolation::MALFORMED_ORDER, "a zero sequence number");
        image = base;
        put32(image, 104, 0);
        violation(image, InvariantViolation::MALFORMED_ORDER, "a level at price 0");
        image = base;
        put32(image, 88, 0);
        put32(image, 92, 0);
        put64(image, 72, 0);
        violation(image, InvariantViolation::ZERO_QUANTITY_ORDER, "an order with nothing left");
    }
}

// Each byte of a small image changed in turn: restore must refuse it or give a sound book, and
// never crash, which ASan and UBSan watch.
void testByteSweep() {
    const Bytes base = baseImage();
    std::size_t accepted = 0;
    for (std::size_t offset = 0; offset < base.size(); ++offset) {
        for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
            Bytes image = base;
            image[offset] ^= static_cast<std::byte>(change);
            auto result = OrderBook::restore(image);
            if (const auto* book = std::get_if<OrderBook>(&result)) {
                ++accepted;
                expect(book->checkInvariants().isValid(),
                       "byte " + std::to_string(offset) + " changed: an accepted image gives a sound book");
                const Bytes again = imageOf(*book);
                restored(again, "byte " + std::to_string(offset) + " changed, written again");
                // writeImage writes version 1, so only a changed version word may write back
                // other bytes.
                expect(offset < 4 || again == image,
                       "byte " + std::to_string(offset) + " changed: an accepted version 1 image writes back the same bytes");
            }
        }
    }
    // Changing a quantity, an owner, or a sequence number that keeps its order leaves a valid
    // book, so some are accepted; anything else is refused.
    expect(accepted > 0 && accepted < base.size() * 3, "the sweep accepts some changes and refuses most");
}

// writeImage writes nothing past its span, and returns 0 for any span too short.
void testShortSpans() {
    const OrderBook book = stopsSample();
    const std::size_t size = book.imageSize();
    constexpr std::size_t guard = 16;
    for (std::size_t length = 0; length < size; ++length) {
        Bytes buffer(length + guard, std::byte{0xAB});
        const std::size_t written = book.writeImage(std::span<std::byte>(buffer.data(), length));
        expect(written == 0, "a span of " + std::to_string(length) + " bytes is too short");
        for (std::size_t i = length; i < buffer.size(); ++i) {
            expect(buffer[i] == std::byte{0xAB}, "nothing written past a span of " + std::to_string(length));
        }
    }
    Bytes roomy(size + guard, std::byte{0xAB});
    expect(book.writeImage(roomy) == size, "a larger span takes the image and says how long it is");
    expect(Bytes(roomy.begin(), roomy.begin() + static_cast<std::ptrdiff_t>(size)) == imageOf(book),
           "the same bytes in a larger span");
}

// Restore reserves each level array for its own levels only, so reserve must still top up the
// others: a restored book with two bid levels and one ask level, reserved for what it holds, takes
// a second ask level without growing.
void testReserveTopsUpARestoredBook() {
    OrderBook book = restored(imageOf(restingSample()), "the resting sample");
    book.reserve(book.holding());
    expect(book.cancelOrder(2).isSuccess(), "a bid level goes, freeing its order and level");
    submit(book, limit(9, Side::SELL, 105, 1));
    const std::uint64_t growths = book.capacityGrowths();
    expect(growths == 0, "a second ask level within what was reserved grew " + std::to_string(growths) + " times");
}

// A restored book is a book: reserve tops it up, it trades, and it reports growth only after.
void testRestoredBookCarriesOn() {
    OrderBook book = restored(imageOf(restingSample()), "the resting sample");
    book.reserve(BookCapacity{64, 8, 16});
    const SubmissionResult sale = book.submitOrder(limit(9, Side::SELL, 99, 12));
    expect(sale.getGeneratedTrades().size() == 3, "the restored book matches as the original would");
    expect(book.checkInvariants().isValid(), "and stays sound");
}

// ---- The checkpoint file, the runtime's wrapper around an image ----------------------------

using runtime::DecodedCheckpoint;
using runtime::CheckpointError;
using runtime::CheckpointErrorCode;
using runtime::CheckpointHeader;

replay::Symbol symbolOf(std::string_view text) {
    const std::optional<replay::Symbol> symbol = replay::Symbol::parse(text);
    expect(symbol.has_value(), "a test symbol parses: " + std::string(text));
    return *symbol;
}

CheckpointHeader headerOf(std::string_view symbol, std::uint64_t position, std::uint32_t shardCount,
                          std::uint32_t shardIndex, std::uint16_t major = 2, std::uint16_t minor = 1,
                          std::uint16_t patch = 0) {
    CheckpointHeader header;
    header.engineMajor = major;
    header.engineMinor = minor;
    header.enginePatch = patch;
    header.shardCount = shardCount;
    header.shardIndex = shardIndex;
    header.position = position;
    header.symbol = symbolOf(symbol);
    return header;
}

std::string describe(const CheckpointError& error) {
    return std::string(runtime::checkpointErrorCodeName(error.code)) + " at " + std::to_string(error.offset) + ": " +
           error.message;
}

DecodedCheckpoint decoded(std::span<const std::byte> file, const std::string& what) {
    auto result = runtime::decodeCheckpoint(file);
    if (const auto* error = std::get_if<CheckpointError>(&result)) {
        expect(false, what + ": decodeCheckpoint refused a good file: " + describe(*error));
    }
    return std::get<DecodedCheckpoint>(result);
}

bool sameBytes(std::span<const std::byte> left, std::span<const std::byte> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

// The files make_golden.py writes from the format document, each with the image it wraps.
struct GoldenFile {
    const char* name;
    const char* image;
    CheckpointHeader header;
};

std::vector<GoldenFile> goldenFiles() {
    return {
        {"resting", "resting", headerOf("AAPL", 12345, 4, 2)},
        {"empty", "empty", headerOf("BRK.B", 0, 1, 0)},
        {"maximal", "maximal", headerOf("ZZZZZZZZ", 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFU, 0xFFFFFFFEU, 0xFFFF, 0xFFFF, 0xFFFF)},
    };
}

void testCheckpointGoldenFiles(const std::filesystem::path& root) {
    for (const GoldenFile& golden : goldenFiles()) {
        const std::string what = std::string("golden file ") + golden.name;
        const Bytes image = readFile(root / "images" / (std::string(golden.image) + ".image"));
        const Bytes file = readFile(root / "files" / (std::string(golden.name) + ".obckpt"));
        expect(runtime::encodeCheckpoint(golden.header, image) == file, what + ": encodeCheckpoint gives the hand-listed bytes");
        const DecodedCheckpoint checkpoint = decoded(file, what);
        expect(checkpoint.header == golden.header, what + ": decoding gives the header written");
        expect(sameBytes(checkpoint.image, image), what + ": decoding gives the image written");
    }
}

CheckpointError refusedFile(std::span<const std::byte> file, const std::string& what) {
    auto result = runtime::decodeCheckpoint(file);
    expect(std::holds_alternative<CheckpointError>(result), what + ": decodeCheckpoint accepted a bad file");
    return std::get<CheckpointError>(std::move(result));
}

void expectFileRefusal(std::span<const std::byte> file, CheckpointErrorCode code, std::size_t offset, const std::string& what) {
    const CheckpointError error = refusedFile(file, what);
    expect(error.code == code && error.offset == offset,
           what + ": expected " + runtime::checkpointErrorCodeName(code) + " at " + std::to_string(offset) + ", got " +
               describe(error));
    expect(!error.message.empty(), what + ": the refusal has a message");
}

// The same bytes with the checksum written again over everything before it, so a fault stands
// behind a checksum that passes. XXH64 itself is held to the specification's vectors in
// command_log_tests.
Bytes sealed(Bytes file) {
    const std::size_t body = file.size() - runtime::checkpointChecksumBytes;
    put64(file, body, replay::xxh64(std::span<const std::byte>(file).first(body)));
    return file;
}

Bytes with32(Bytes file, std::size_t offset, std::uint32_t value) {
    put32(file, offset, value);
    return sealed(std::move(file));
}

Bytes with64(Bytes file, std::size_t offset, std::uint64_t value) {
    put64(file, offset, value);
    return sealed(std::move(file));
}

// The resting golden file's image followed by these bytes as sections, its checksum resealed.
Bytes withTrailing(const Bytes& file, const Bytes& sections) {
    Bytes out(file.begin(), file.end() - static_cast<std::ptrdiff_t>(runtime::checkpointChecksumBytes));
    out.insert(out.end(), sections.begin(), sections.end());
    out.resize(out.size() + runtime::checkpointChecksumBytes);
    return sealed(std::move(out));
}

Bytes sectionBytes(std::uint32_t type, std::uint32_t reserved, std::uint64_t length, std::size_t payload) {
    Bytes bytes(16 + payload, std::byte{0x5A});
    put32(bytes, 0, type);
    put32(bytes, 4, reserved);
    put64(bytes, 8, length);
    return bytes;
}

void testCheckpointRefusals(const std::filesystem::path& root) {
    const Bytes good = readFile(root / "files" / "resting.obckpt");
    const std::size_t size = good.size();  // 280: a 64-byte header, a 208-byte image, the checksum
    const std::size_t checksumAt = size - 8;
    const std::size_t imageEnd = 64 + 208;

    expectFileRefusal({}, CheckpointErrorCode::BAD_MAGIC, 0, "an empty file");
    expectFileRefusal(std::span<const std::byte>(good).first(7), CheckpointErrorCode::BAD_MAGIC, 0, "7 bytes of the magic");
    {
        Bytes textMode = good;  // a text-mode copy turns \r\n into \n
        textMode.erase(textMode.begin() + 6);
        expectFileRefusal(textMode, CheckpointErrorCode::BAD_MAGIC, 0, "a text-mode copy");
    }
    expectFileRefusal(std::span<const std::byte>(good).first(71), CheckpointErrorCode::TOO_SHORT, 71,
                      "one byte short of a header and a checksum");

    expectFileRefusal(readFile(root / "files" / "bad_checksum.obckpt"), CheckpointErrorCode::CHECKSUM_MISMATCH, checksumAt,
                      "an image byte changed");
    expectFileRefusal(std::span<const std::byte>(good).first(size - 1), CheckpointErrorCode::CHECKSUM_MISMATCH, checksumAt - 1,
                      "the last byte cut off");
    {
        Bytes lengthened = good;  // a wrong length field fails the checksum before it is read
        put64(lengthened, 56, 209);
        expectFileRefusal(lengthened, CheckpointErrorCode::CHECKSUM_MISMATCH, checksumAt, "a length field changed alone");
    }

    expectFileRefusal(with32(good, 8, 0), CheckpointErrorCode::UNSUPPORTED_VERSION, 8, "format version 0");
    expectFileRefusal(with32(good, 12, 2), CheckpointErrorCode::UNSUPPORTED_VERSION, 12, "minimum reader version 2");
    expectFileRefusal(with32(good, 12, 0), CheckpointErrorCode::UNSUPPORTED_VERSION, 12, "minimum reader version 0");
    expectFileRefusal(with32(with32(good, 8, 2), 12, 2), CheckpointErrorCode::UNSUPPORTED_VERSION, 12,
                      "format 2 with minimum reader version 2");

    expectFileRefusal(with32(good, 16, 63), CheckpointErrorCode::SIZE_BELOW_KNOWN, 16, "a header of 63 bytes");

    expectFileRefusal(with32(good, 16, 273), CheckpointErrorCode::LENGTH_MISMATCH, 16, "a header past the checksum");
    expectFileRefusal(with64(good, 56, 209), CheckpointErrorCode::LENGTH_MISMATCH, 56, "an image past the checksum");
    expectFileRefusal(with64(good, 56, 0xFFFFFFFFFFFFFFFFULL), CheckpointErrorCode::LENGTH_MISMATCH, 56,
                      "an image length that would wrap");
    expectFileRefusal(with64(good, 56, 207), CheckpointErrorCode::LENGTH_MISMATCH, imageEnd - 1,
                      "one byte after the image, too few for a section");
    expectFileRefusal(withTrailing(good, sectionBytes(1, 0, 3, 2)), CheckpointErrorCode::LENGTH_MISMATCH, imageEnd + 8,
                      "a section past the checksum");
    expectFileRefusal(withTrailing(good, Bytes(15)), CheckpointErrorCode::LENGTH_MISMATCH, imageEnd,
                      "15 bytes after the image");

    {
        Bytes reserved = good;
        put8(reserved, 26, 1);
        expectFileRefusal(sealed(reserved), CheckpointErrorCode::NONZERO_RESERVED, 26, "the first reserved field");
    }
    {
        Bytes reserved = good;
        put8(reserved, 27, 0x80);
        expectFileRefusal(sealed(reserved), CheckpointErrorCode::NONZERO_RESERVED, 26, "the first reserved field's top byte");
    }
    expectFileRefusal(with32(good, 36, 1), CheckpointErrorCode::NONZERO_RESERVED, 36, "the second reserved field");
    expectFileRefusal(withTrailing(good, sectionBytes(1, 1, 0, 0)), CheckpointErrorCode::NONZERO_RESERVED, imageEnd + 4,
                      "a section's reserved field");

    expectFileRefusal(with32(good, 28, 0), CheckpointErrorCode::INVALID_SHARD, 28, "a shard count of 0");
    expectFileRefusal(with32(good, 32, 4), CheckpointErrorCode::INVALID_SHARD, 32, "a shard index equal to the count");

    expectFileRefusal(with64(good, 48, 0), CheckpointErrorCode::INVALID_SYMBOL, 48, "a symbol of zero bytes");
    {
        Bytes lower = good;
        put8(lower, 48, static_cast<std::uint8_t>('a'));
        expectFileRefusal(sealed(lower), CheckpointErrorCode::INVALID_SYMBOL, 48, "a lower-case symbol");
    }

    // The checks run in the order the format gives, so the first fault is the one reported.
    {
        Bytes several = good;
        put32(several, 8, 0);
        put32(several, 28, 0);
        put64(several, 48, 0);
        expectFileRefusal(sealed(several), CheckpointErrorCode::UNSUPPORTED_VERSION, 8, "the version before the fields");
    }
    {
        Bytes several = good;
        put64(several, 56, 209);
        put32(several, 36, 1);
        expectFileRefusal(sealed(several), CheckpointErrorCode::LENGTH_MISMATCH, 56, "the length before the fields");
    }

    // A later writer's file: format 2 readable by 1, a longer header, and sections of types this
    // reader does not know, which it skips.
    const Bytes extendedFile = readFile(root / "files" / "extended.obckpt");  // the image is a view into it
    const DecodedCheckpoint extended = decoded(extendedFile, "golden file extended");
    expect(extended.header == headerOf("ES-Z26", 77, 2, 1), "golden file extended: decoding gives its header");
    expect(sameBytes(extended.image, readFile(root / "images" / "stops.image")),
           "golden file extended: the image is found past the longer header, and the sections skipped");
    decoded(withTrailing(good, sectionBytes(0xFFFFFFFFU, 0, 0, 0)), "an empty section of an unknown type");
}

// decodeCheckpoint(encodeCheckpoint(h, x)) gives h and x back, for books' own images and for
// every header field at its extremes, and the image still restores to the same bytes.
void testCheckpointRoundTrips() {
    const std::pair<OrderBook, CheckpointHeader> cases[] = {
        {OrderBook{}, headerOf("A", 0, 1, 0, 0, 0, 0)},
        {restingSample(), headerOf("MSFT", 1, 2, 1)},
        {stopsSample(), headerOf("BRK.B", 0x8000000000000000ULL, 0xFFFFFFFFU, 0)},
        {maximalSample(), headerOf("ES-Z26", 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFU, 0xFFFFFFFEU, 0xFFFF, 0xFFFF, 0xFFFF)},
    };
    for (const auto& [book, header] : cases) {
        const std::string what = "round trip of " + header.symbol.text();
        const Bytes image = imageOf(book);
        const Bytes file = runtime::encodeCheckpoint(header, image);
        expect(file.size() == 64 + image.size() + 8, what + ": a header, the image, and a checksum");
        const DecodedCheckpoint checkpoint = decoded(file, what);
        expect(checkpoint.header == header, what + ": the header comes back");
        expect(sameBytes(checkpoint.image, image), what + ": the image comes back");
        expect(imageOf(restored(checkpoint.image, what)) == image, what + ": and restores to the same bytes");
    }
}

// Any one bit changed, anywhere, and any length cut off, is refused, and by the check that should
// catch it: the magic's for its 8 bytes, the checksum's for every byte after them, since the
// checksum covers every byte before it and is read before any field.
void testCheckpointDamage(const std::filesystem::path& root) {
    for (const char* name : {"empty", "resting"}) {
        const Bytes good = readFile(root / "files" / (std::string(name) + ".obckpt"));
        const std::size_t checksumAt = good.size() - 8;
        for (std::size_t offset = 0; offset < good.size(); ++offset) {
            for (unsigned int bit = 0; bit < 8; ++bit) {
                Bytes flipped = good;
                flipped[offset] ^= static_cast<std::byte>(1U << bit);
                const std::string what = std::string(name) + ": bit " + std::to_string(bit) + " of byte " + std::to_string(offset);
                if (offset < 8) {
                    expectFileRefusal(flipped, CheckpointErrorCode::BAD_MAGIC, 0, what);
                } else {
                    expectFileRefusal(flipped, CheckpointErrorCode::CHECKSUM_MISMATCH, checksumAt, what);
                }
            }
        }
        for (std::size_t length = 0; length < good.size(); ++length) {
            const std::string what = std::string(name) + ": cut to " + std::to_string(length) + " bytes";
            const std::span<const std::byte> cut = std::span<const std::byte>(good).first(length);
            if (length < 8) {
                expectFileRefusal(cut, CheckpointErrorCode::BAD_MAGIC, 0, what);
            } else if (length < 72) {
                expectFileRefusal(cut, CheckpointErrorCode::TOO_SHORT, length, what);
            } else {
                expectFileRefusal(cut, CheckpointErrorCode::CHECKSUM_MISMATCH, length - 8, what);
            }
        }
    }
}

// verify runs the wrapper's checks, then restore, and reports a fault at its offset in the file.
void testVerifyCheckpoint(const std::filesystem::path& root) {
    const auto fault = [&root](const char* name) {
        return runtime::verifyCheckpoint(readFile(root / "files" / (std::string(name) + ".obckpt")));
    };
    for (const char* name : {"resting", "empty", "maximal", "extended"}) {
        const std::optional<runtime::CheckpointFault> found = fault(name);
        expect(!found, std::string("verify passes golden file ") + name + (found ? ": " + found->message : ""));
    }

    const std::optional<runtime::CheckpointFault> checksum = fault("bad_checksum");
    expect(checksum && checksum->code == runtime::CheckpointFault::Code{CheckpointErrorCode::CHECKSUM_MISMATCH} &&
               checksum->offset == 272,
           "verify reports the wrapper's fault");
    expect(checksum && runtime::formatCheckpointFault("x.obckpt", *checksum) ==
                           "x.obckpt: 272: CHECKSUM_MISMATCH: the checksum is not that of the bytes before it",
           "a fault prints as path, offset, code, and message");

    // The image's reserved header word, at 60 in the image, is 124 in a file whose header is 64.
    const std::optional<runtime::CheckpointFault> image = fault("bad_image");
    expect(image && image->code == runtime::CheckpointFault::Code{RestoreErrorCode::NONZERO_RESERVED} && image->offset == 124,
           "verify reports restore's fault at its offset in the file");
    expect(image && runtime::formatCheckpointFault("bad_image.obckpt", *image) ==
                        "bad_image.obckpt: 124: NONZERO_RESERVED: the header's reserved bytes are not 0",
           "restore's fault prints with restore's code and message");
    // And at 132 behind a 72-byte header.
    const std::optional<runtime::CheckpointFault> extended =
        runtime::verifyCheckpoint(with32(readFile(root / "files" / "extended.obckpt"), 72 + 60, 1));
    expect(extended && extended->code == runtime::CheckpointFault::Code{RestoreErrorCode::NONZERO_RESERVED} &&
               extended->offset == 132,
           "restore's offset is moved by the stated header size");
}

// describe reads both headers and checks nothing past the magic: a damaged file still describes.
void testDescribeCheckpoint(const std::filesystem::path& root) {
    const std::string resting =
        "file-bytes: 280\n"
        "format-version: 1\n"
        "minimum-reader-version: 1\n"
        "header-bytes: 64\n"
        "engine-version: 2.1.0\n"
        "reserved-at-26: 0\n"
        "shard-count: 4\n"
        "shard-index: 2\n"
        "reserved-at-36: 0\n"
        "position: 12345\n"
        "symbol: AAPL\n"
        "image-bytes: 208\n"
        "image-version: 1\n"
        "image-minimum-reader-version: 1\n"
        "image-header-bytes: 64\n"
        "level-record-bytes: 16\n"
        "order-record-bytes: 24\n"
        "stop-record-bytes: 32\n"
        "next-sequence-number: 6\n"
        "next-trade-id: 2\n"
        "last-trade-price: 100\n"
        "bid-levels: 2\n"
        "ask-levels: 1\n"
        "orders: 4\n"
        "stops: 0\n"
        "order-index-size: 4\n"
        "stop-index-size: 0\n"
        "image-reserved: 0\n"
        "checksum: not checked\n";
    const auto describedText = [](std::span<const std::byte> file, const std::string& what) {
        auto result = runtime::describeCheckpoint(file);
        if (const auto* error = std::get_if<CheckpointError>(&result)) {
            expect(false, what + ": describe refused: " + describe(*error));
        }
        return runtime::formatCheckpointDescription(std::get<runtime::CheckpointDescription>(result));
    };
    const Bytes good = readFile(root / "files" / "resting.obckpt");
    const std::string text = describedText(good, "describe resting");
    expect(text == resting, "describe resting prints both headers:\n" + text);

    // Damage past the magic is printed, not judged.
    std::string changed = resting;
    changed.replace(changed.find("last-trade-price: 100"), 21, "last-trade-price: 101");
    expect(describedText(readFile(root / "files" / "bad_checksum.obckpt"), "describe bad_checksum") == changed,
           "a bad checksum still describes");
    {
        Bytes odd = good;
        put64(odd, 48, 0x000000006C707041ULL);  // "Appl" then zeros: not a symbol
        const std::string oddText = describedText(odd, "describe a bad symbol");
        expect(oddText.find("symbol: not a symbol: 41 70 70 6c 00 00 00 00\n") != std::string::npos,
               "a symbol that is not one prints as its bytes:\n" + oddText);
    }
    {
        const std::string cut = describedText(std::span<const std::byte>(good).first(127), "describe a cut file");
        expect(cut.find("file-bytes: 127\n") == 0 && cut.find("image-version") == std::string::npos &&
                   cut.find("image-header: past the end of the file\nchecksum: not checked\n") != std::string::npos,
               "an image header past the end is said to be:\n" + cut);
        const std::string whole = describedText(std::span<const std::byte>(good).first(128), "describe a file cut after the image header");
        expect(whole.find("stop-index-size: 0\n") != std::string::npos, "64 bytes of image header describe");
    }

    const auto refusal = [](std::span<const std::byte> file) {
        auto result = runtime::describeCheckpoint(file);
        expect(std::holds_alternative<CheckpointError>(result), "describe refuses");
        return std::get<CheckpointError>(result);
    };
    // The image header is read where the stated header size puts it.
    {
        auto extended = runtime::describeCheckpoint(readFile(root / "files" / "extended.obckpt"));
        const auto* description = std::get_if<runtime::CheckpointDescription>(&extended);
        expect(description && description->headerBytes == 72 && description->image &&
                   description->image == readImageHeader(readFile(root / "images" / "stops.image")),
               "describe reads the image header past a 72-byte header");
    }

    const CheckpointError notOne = refusal(readFile(root / "images" / "resting.image"));
    expect(notOne.code == CheckpointErrorCode::BAD_MAGIC && notOne.offset == 0, "describe refuses another kind of file");
    const CheckpointError shortFile = refusal(std::span<const std::byte>(good).first(63));
    expect(shortFile.code == CheckpointErrorCode::TOO_SHORT && shortFile.offset == 63, "describe needs a whole header");
}

// The image's header as stored, checking nothing, for describe.
void testReadImageHeader(const std::filesystem::path& root) {
    const Bytes image = readFile(root / "images" / "stops.image");
    const std::optional<ImageHeader> header = readImageHeader(image);
    ImageHeader expected;
    expected.imageVersion = 1;
    expected.minimumReaderVersion = 1;
    expected.headerBytes = 64;
    expected.levelRecordBytes = 16;
    expected.orderRecordBytes = 24;
    expected.stopRecordBytes = 32;
    expected.nextSequenceNumber = 8;
    expected.nextTradeId = 2;
    expected.lastTradePrice = 100;
    expected.bidLevels = 0;
    expected.askLevels = 1;
    expected.orders = 1;
    expected.stops = 5;
    expected.orderIndexSize = 1;
    expected.stopIndexSize = 5;
    expected.reserved = 0;
    expect(header == expected, "readImageHeader gives the stops sample's fields");
    expect(!readImageHeader(std::span<const std::byte>(image).first(63)), "fewer than 64 bytes hold no header");
    Bytes odd(64, std::byte{0xFF});
    expect(readImageHeader(odd) && readImageHeader(odd)->reserved == 0xFFFFFFFFU, "and it checks nothing");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: checkpoint_tests <tests/checkpoint directory>\n";
        return EXIT_FAILURE;
    }
    try {
        testGoldenSamples(argv[1]);
        testRoundTrips();
        testSequenceWrap();
        testRefusals();
        testByteSweep();
        testShortSpans();
        testRestoredBookCarriesOn();
        testReserveTopsUpARestoredBook();
        testContinuation();
        testCheckpointGoldenFiles(argv[1]);
        testCheckpointRefusals(argv[1]);
        testCheckpointRoundTrips();
        testCheckpointDamage(argv[1]);
        testReadImageHeader(argv[1]);
        testVerifyCheckpoint(argv[1]);
        testDescribeCheckpoint(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "checkpoint_tests passed\n";
    return EXIT_SUCCESS;
}
