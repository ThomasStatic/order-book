#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "order_book/network/entry_check.hpp"
#include "order_book/order_book.hpp"

namespace {

using namespace order_book;
using namespace order_book::network;
using replay::RawCommand;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::array<char, Symbol::size> stored(const char* text) {
    std::array<char, Symbol::size> bytes;
    bytes.fill(' ');
    std::memcpy(bytes.data(), text, std::strlen(text));
    return bytes;
}

runtime::InstrumentDirectory directory() {
    return runtime::InstrumentDirectory(
        {runtime::Instrument{*Symbol::parse("AAPL"), 1, std::nullopt, std::nullopt, std::nullopt},
         runtime::Instrument{*Symbol::parse("MSFT"), 0, std::nullopt, std::nullopt, std::nullopt}},
        2);
}

RawCommand addCommand() {
    RawCommand raw{};
    raw.command = replay::rawCommandAdd;
    raw.side = 1;
    raw.orderType = 1;
    raw.timeInForce = 1;
    raw.symbol = stored("AAPL");
    raw.orderId = 7;
    raw.price = 100;
    raw.quantity = 5;
    return raw;
}

RawCommand cancelCommand(OrderId id) {
    RawCommand raw{};
    raw.command = replay::rawCommandCancel;
    raw.symbol = stored("AAPL");
    raw.orderId = id;
    return raw;
}

EntryDecision check(const RawCommand& raw, bool stopping = false) {
    return checkEntry(CommandFrame{42, raw}, directory(), stopping);
}

void testAPassLocatesTheBook() {
    const EntryDecision decision = check(addCommand());
    const EntryAccepted& accepted = std::get<EntryAccepted>(decision);
    expect(accepted.symbol == *Symbol::parse("AAPL"), "the symbol is named");
    expect(accepted.location == runtime::BookLocation{1, 0}, "and its shard and slot");
    const auto& add = std::get<replay::AddCommand>(accepted.command);
    expect(add.orderId == 7 && add.side == Side::BUY && add.orderType == OrderType::LIMIT && add.timeInForce == TimeInForce::GTC,
           "the command is decoded");
}

void testChecksRunInOrder() {
    RawCommand both = addCommand();
    both.symbol = stored("NOPE");
    both.quantity = 0;
    expect(std::get<EntryRefused>(check(both, true)).code == RefusedCode::UNKNOWN_INSTRUMENT,
           "the symbol comes first, ahead of malformed and stopping");

    RawCommand malformed = addCommand();
    malformed.quantity = 0;
    expect(std::get<EntryRejected>(check(malformed, true)).event == BookEvent{OrderRejected{7, RejectionReason::INVALID_QUANTITY}},
           "malformed comes ahead of stopping");

    expect(std::get<EntryRefused>(check(addCommand(), true)).code == RefusedCode::ENGINE_STOPPING,
           "stopping comes last, so it means this command would have been queued");
    expect(std::get<EntryRefused>(check(cancelCommand(7), true)).code == RefusedCode::ENGINE_STOPPING, "for every command");
}

void testAMalformedSymbolIsAnUnknownInstrument() {
    for (const char* text : {"aapl", "AA PL", "", "A\tPL"}) {
        RawCommand raw = addCommand();
        raw.symbol = stored(text);
        expect(std::get<EntryRefused>(check(raw)).code == RefusedCode::UNKNOWN_INSTRUMENT, "a malformed symbol takes the same path");
    }
    RawCommand zeroes = addCommand();
    zeroes.symbol = {};
    expect(std::get<EntryRefused>(check(zeroes)).code == RefusedCode::UNKNOWN_INSTRUMENT, "so do eight zero bytes");
}

void testACancelOfIdZero() {
    expect(std::get<EntryRejected>(check(cancelCommand(0))).event ==
               BookEvent{CancelRejected{0, RejectionReason::ORDER_NOT_FOUND}},
           "what the book would say");
    expect(std::holds_alternative<EntryAccepted>(check(cancelCommand(1))), "any other ID is queued");
}

void testSelfTradeMode() {
    RawCommand clear = addCommand();
    clear.selfTradePrevention = 3;
    const EntryDecision noOwner = check(clear);
    expect(std::get<replay::AddCommand>(std::get<EntryAccepted>(noOwner).command).selfTradePrevention ==
               SelfTradePrevention::CANCEL_RESTING,
           "no owner clears the mode");

    RawCommand absent = addCommand();
    absent.owner = 9;
    const EntryDecision withOwner = check(absent);
    expect(std::get<replay::AddCommand>(std::get<EntryAccepted>(withOwner).command).selfTradePrevention ==
               SelfTradePrevention::CANCEL_RESTING,
           "an owner with an absent mode takes CANCEL_RESTING");

    RawCommand both = absent;
    both.selfTradePrevention = 3;
    const EntryDecision kept = check(both);
    expect(std::get<replay::AddCommand>(std::get<EntryAccepted>(kept).command).selfTradePrevention ==
               SelfTradePrevention::CANCEL_BOTH,
           "an owner keeps its mode");

    for (const OwnerId owner : {OwnerId{0}, OwnerId{9}}) {
        RawCommand unknown = addCommand();
        unknown.owner = owner;
        unknown.selfTradePrevention = 4;
        expect(std::get<EntryRejected>(check(unknown)).event ==
                   BookEvent{OrderRejected{7, RejectionReason::INVALID_SELF_TRADE_PREVENTION}},
               "an unknown mode is refused whatever the owner");
    }
}

// The book's own answer, from the same bytes read a different way from checkEntry's.
Side sideFrom(std::uint8_t byte) {
    return static_cast<Side>(static_cast<int>(byte) - 1);
}

OrderType typeFrom(std::uint8_t byte) {
    return static_cast<OrderType>(static_cast<int>(byte) - 1);
}

TimeInForce timeInForceFrom(std::uint8_t byte) {
    return static_cast<TimeInForce>(static_cast<int>(byte) - 1);
}

SelfTradePrevention modeFrom(std::uint8_t byte) {
    return byte == 0 ? SelfTradePrevention::CANCEL_RESTING : static_cast<SelfTradePrevention>(static_cast<int>(byte) - 1);
}

BookEvent firstEvent(const auto& events) {
    expect(!events.empty(), "the book answers with an event");
    return *events.begin();
}

// An event on an empty book, so a rejection can only be about the command itself, or, for what
// needs an order, that there is none.
BookEvent askTheBook(const RawCommand& raw) {
    OrderBook book;
    if (raw.command == replay::rawCommandAdd) {
        OrderRequest request{};
        request.id = raw.orderId;
        request.side = sideFrom(raw.side);
        request.price = raw.price;
        request.quantity = raw.quantity;
        request.type = typeFrom(raw.orderType);
        request.timeInForce = timeInForceFrom(raw.timeInForce);
        request.stopPrice = raw.stopPrice;
        request.postOnly = (raw.flags & 1U) != 0;
        request.owner = raw.owner;
        request.selfTradePrevention = modeFrom(raw.selfTradePrevention);
        return firstEvent(book.submitOrder(request).getEvents());
    }
    if (raw.command == replay::rawCommandCancel) {
        return firstEvent(book.cancelOrder(raw.orderId).getEvents());
    }
    ReplaceRequest request{};
    request.id = raw.orderId;
    request.price = raw.price;
    request.quantity = raw.quantity;
    request.postOnly = (raw.flags & 1U) != 0;
    request.owner = raw.owner;
    request.selfTradePrevention = modeFrom(raw.selfTradePrevention);
    return firstEvent(book.replaceOrder(request).getEvents());
}

class Source {
public:
    explicit Source(std::uint64_t seed) : random(seed) {}

    std::uint64_t below(std::uint64_t bound) {
        return random() % bound;
    }

    // Mostly one of a few small values, sometimes anything, so both sides of each check run.
    std::uint8_t byteUpTo(std::uint8_t usual) {
        const std::uint64_t kind = below(20);
        if (kind == 0) {
            return static_cast<std::uint8_t>(below(256));
        }
        return static_cast<std::uint8_t>(below(usual + 1U));
    }

    std::uint32_t amount() {
        const std::uint64_t kind = below(10);
        if (kind == 0) {
            return 0;
        }
        return static_cast<std::uint32_t>(1 + below(1000));
    }

private:
    std::mt19937_64 random;
};

RawCommand randomCommand(Source& source) {
    RawCommand raw{};
    raw.command = static_cast<std::uint8_t>(1 + source.below(3));
    raw.symbol = stored(source.below(4) == 0 ? "MSFT" : "AAPL");
    raw.orderId = source.below(10) == 0 ? 0 : static_cast<OrderId>(1 + source.below(100));
    if (raw.command == replay::rawCommandCancel) {
        return raw;
    }
    raw.price = source.amount();
    raw.quantity = source.amount();
    raw.owner = source.below(2) == 0 ? 0 : static_cast<OwnerId>(1 + source.below(4));
    raw.selfTradePrevention = source.byteUpTo(3);
    raw.flags = source.below(4) == 0 ? replay::rawFlagPostOnly : 0;
    if (raw.command == replay::rawCommandAdd) {
        raw.side = source.byteUpTo(2);
        raw.orderType = source.byteUpTo(4);
        raw.timeInForce = source.byteUpTo(3);
        raw.stopPrice = source.below(2) == 0 ? 0 : source.amount();
    }
    return raw;
}

bool isMalformedAnswer(const BookEvent& event) {
    return std::holds_alternative<OrderRejected>(event) ||
           (std::holds_alternative<ReplaceRejected>(event) &&
            std::get<ReplaceRejected>(event).reason != RejectionReason::ORDER_NOT_FOUND) ||
           (std::holds_alternative<CancelRejected>(event) && std::get<CancelRejected>(event).orderId == 0);
}

// checkEntry refuses exactly what the book would, with the same reason, and what it passes
// encodes to a record the log reader accepts, which gives back the same command.
void testAgreesWithTheBookOnRandomBodies() {
    Source source(20261102);
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (std::size_t i = 0; i < 200000; ++i) {
        const RawCommand raw = randomCommand(source);
        const EntryDecision decision = check(raw);
        const BookEvent answer = askTheBook(raw);
        if (const EntryRejected* rejection = std::get_if<EntryRejected>(&decision)) {
            ++rejected;
            expect(rejection->event == answer, "refused with the book's own event and reason");
            continue;
        }
        ++accepted;
        const EntryAccepted& passed = std::get<EntryAccepted>(decision);
        expect(!isMalformedAnswer(answer), "what passes the book doesn't call malformed");

        std::array<std::byte, replay::logRecordBytes> record{};
        replay::encodeLogRecord(passed.symbol, passed.command, 7, std::span<std::byte, replay::logRecordBytes>(record));
        const replay::ParseResult read = replay::decodeLogRecord(record, 7, 0, 0);
        const replay::ReplayRecord* logged = std::get_if<replay::ReplayRecord>(&read);
        expect(logged != nullptr, "the log reader accepts what passed");
        expect(logged->symbol == passed.symbol && logged->command == passed.command, "and gives back the same command");
    }
    expect(accepted > 10000 && rejected > 10000, "both outcomes were exercised");
}

}  // namespace

int main() {
    testAPassLocatesTheBook();
    testChecksRunInOrder();
    testAMalformedSymbolIsAnUnknownInstrument();
    testACancelOfIdZero();
    testSelfTradeMode();
    testAgreesWithTheBookOnRandomBodies();
    std::cout << "entry check tests passed" << std::endl;
    return EXIT_SUCCESS;
}
