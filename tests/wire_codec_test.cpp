#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/network/wire_codec.hpp"

// Counts every allocation, to prove framing makes none per frame.
namespace {
std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
    ++allocations;
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}

// GCC pairs this free with the new it can see, not with the replacement above.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace order_book;
using namespace order_book::network;
using Bytes = std::vector<std::byte>;

constexpr Correlation goldenCorrelation = 0x1122334455667788ULL;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(const char* text) {
    return *Symbol::parse(text);
}

Bytes readFrame(const std::filesystem::path& root, const std::string& name) {
    std::ifstream file(root / "frames" / (name + ".frame"), std::ios::binary);
    expect(file.is_open(), "cannot open the golden frame " + name);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return Bytes(first, first + text.size());
}

template <typename Encode>
Bytes encoded(Encode encode) {
    std::array<std::byte, 64> out{};
    const std::size_t size = encode(std::span<std::byte>(out));
    expect(size != 0, "the frame fits");
    return Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size));
}

struct GoldenEvent {
    const char* file;
    BookEvent event;
    bool last;
};

std::vector<GoldenEvent> goldenEvents() {
    return {
        {"order_accepted", OrderAccepted{7, Side::BUY, 100, 5}, false},
        {"order_rejected", OrderRejected{7, RejectionReason::DUPLICATE_ORDER_ID}, true},
        {"trade_executed", TradeExecuted{7, 3, 100, 2}, false},
        {"order_cancelled", OrderCancelled{7, 5}, true},
        {"cancel_rejected", CancelRejected{7, RejectionReason::ORDER_NOT_FOUND}, true},
        {"order_expired", OrderExpired{7, 4}, true},
        {"self_trade_prevented", SelfTradePrevented{7, 3, 100, 6}, false},
        {"stop_triggered", StopTriggered{7, 96, 97}, false},
        {"order_replaced", OrderReplaced{7, Side::SELL, 101, 6, 4, true}, false},
        {"replace_rejected", ReplaceRejected{7, RejectionReason::REPLACE_QUANTITY_TOO_LOW}, true},
    };
}

replay::AddCommand goldenAdd() {
    replay::AddCommand add{};
    add.orderId = 7;
    add.side = Side::BUY;
    add.price = 100;
    add.quantity = 5;
    add.postOnly = true;
    add.owner = 9;
    add.selfTradePrevention = SelfTradePrevention::CANCEL_INCOMING;
    return add;
}

replay::AddCommand goldenStop() {
    replay::AddCommand add{};
    add.orderId = 8;
    add.side = Side::SELL;
    add.price = 95;
    add.quantity = 3;
    add.orderType = OrderType::STOP_LIMIT;
    add.timeInForce = TimeInForce::IOC;
    add.stopPrice = 96;
    return add;
}

void testEncodersWriteTheGoldenBytes(const std::filesystem::path& root) {
    expect(encoded([](auto out) { return encodeHello(goldenCorrelation, wireVersion, out); }) == readFrame(root, "hello"),
           "Hello");
    expect(encoded([](auto out) { return encodeWelcome(goldenCorrelation, wireVersion, out); }) == readFrame(root, "welcome"),
           "Welcome");
    const auto command = [](const char* symbol, const replay::Command& held) {
        return encoded([&](auto out) { return encodeCommand(goldenCorrelation, symbolOf(symbol), held, out); });
    };
    expect(command("AAPL", goldenAdd()) == readFrame(root, "command_add"), "a Command holding an ADD");
    expect(command("BRK.B", goldenStop()) == readFrame(root, "command_stop"), "a Command holding a stop");
    expect(command("ES-Z26", replay::CancelCommand{77}) == readFrame(root, "command_cancel"), "a Command holding a CANCEL");
    expect(command("AAPL", replay::ReplaceCommand{7, 101, 6, false, 9, SelfTradePrevention::CANCEL_BOTH}) ==
               readFrame(root, "command_replace"),
           "a Command holding a REPLACE");
    expect(encoded([](auto out) {
               return encodeRefused(goldenCorrelation, symbolOf("NOPE").bytes(), 7, RefusedCode::UNKNOWN_INSTRUMENT, out);
           }) == readFrame(root, "refused"),
           "Refused");
    expect(encoded([](auto out) { return encodeProtocolError(goldenCorrelation, ProtocolError::UNEXPECTED_FIELD, out); }) ==
               readFrame(root, "protocol_error"),
           "ProtocolError");
    for (const GoldenEvent& golden : goldenEvents()) {
        expect(encoded([&](auto out) { return encodeEvent(goldenCorrelation, symbolOf("AAPL"), golden.event, golden.last, out); }) ==
                   readFrame(root, golden.file),
               std::string("the event ") + golden.file);
    }
}

struct FrameSize {
    const char* name;
    std::size_t size;
};

void testFramesHaveTheirSizes(const std::filesystem::path& root) {
    const std::vector<FrameSize> sizes = {
        {"hello", 24},           {"welcome", 24},         {"command_add", 48},          {"command_cancel", 48},
        {"refused", 28},         {"protocol_error", 16},  {"order_accepted", 36},       {"order_rejected", 28},
        {"trade_executed", 36},  {"order_cancelled", 28}, {"cancel_rejected", 28},      {"order_expired", 28},
        {"stop_triggered", 32},  {"order_replaced", 40},  {"self_trade_prevented", 36}, {"replace_rejected", 28},
    };
    for (const auto& [name, size] : sizes) {
        expect(readFrame(root, name).size() == size, std::string("the size of ") + name);
    }
}

void testACommandFrameIsARecordsBody(const std::filesystem::path& root) {
    const Bytes frame = readFrame(root, "command_add");
    std::array<std::byte, replay::logRecordBytes> record{};
    replay::encodeLogRecord(symbolOf("AAPL"), goldenAdd(), 0, std::span<std::byte, replay::logRecordBytes>(record));
    expect(std::equal(frame.begin() + frameHeaderBytes, frame.end(), record.begin() + 4), "bytes 4 to 39 of a log record, unchanged");
}

void testEveryEventRoundTrips() {
    FrameReader reader(FrameSource::ENGINE);
    const Symbol symbol = symbolOf("BF-B");
    for (const GoldenEvent& golden : goldenEvents()) {
        const Bytes frame = encoded([&](auto out) { return encodeEvent(5, symbol, golden.event, golden.last, out); });
        reader.feed(frame);
        const FrameResult result = reader.next();
        const Frame* read = std::get_if<Frame>(&result);
        expect(read != nullptr, "the frame reads");
        expect(read->correlation == 5 && read->last == golden.last, "the header reads back");
        const std::optional<EventFrame> event = decodeEvent(*read);
        expect(event && event->symbol == symbol && event->event == golden.event, "every event round trips to an equal BookEvent");
    }
}

struct ReasonCode {
    RejectionReason reason;
    std::uint16_t code;
};

void testEveryReasonRoundTrips() {
    // Pinned: a value here is part of the protocol, and is never reused.
    const std::vector<ReasonCode> table = {
        {RejectionReason::DUPLICATE_ORDER_ID, 1},
        {RejectionReason::ORDER_NOT_FOUND, 2},
        {RejectionReason::INVALID_ORDER_ID, 3},
        {RejectionReason::INVALID_PRICE, 4},
        {RejectionReason::INVALID_QUANTITY, 5},
        {RejectionReason::INVALID_SIDE, 6},
        {RejectionReason::UNSUPPORTED_ORDER_TYPE, 7},
        {RejectionReason::ORDER_NOT_ACTIVE, 8},
        {RejectionReason::WOULD_CROSS_BOOK, 9},
        {RejectionReason::PRICE_NOT_ALLOWED, 10},
        {RejectionReason::INVALID_TIME_IN_FORCE, 11},
        {RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, 12},
        {RejectionReason::INVALID_STOP_PRICE, 13},
        {RejectionReason::STOP_PRICE_NOT_ALLOWED, 14},
        {RejectionReason::POST_ONLY_NOT_ALLOWED, 15},
        {RejectionReason::INVALID_SELF_TRADE_PREVENTION, 16},
        {RejectionReason::REPLACE_QUANTITY_TOO_LOW, 17},
        {RejectionReason::ORDER_NOT_REPLACEABLE, 18},
    };
    for (const auto& [reason, code] : table) {
        expect(reasonCode(reason) == code, std::string("the code of ") + rejectionReasonName(reason));
        expect(reasonFromCode(code) == reason, std::string("the reason of ") + rejectionReasonName(reason));
    }
    expect(!reasonFromCode(0) && !reasonFromCode(19), "a code this release doesn't know names no reason");
    expect(static_cast<int>(RefusedCode::UNKNOWN_INSTRUMENT) == 1 && static_cast<int>(RefusedCode::ENGINE_STOPPING) == 2,
           "refused codes are pinned");
    expect(static_cast<int>(ProtocolError::BAD_LENGTH) == 1 && static_cast<int>(ProtocolError::BAD_MAGIC) == 2 &&
               static_cast<int>(ProtocolError::UNKNOWN_FRAME_TYPE) == 3 && static_cast<int>(ProtocolError::HELLO_REQUIRED) == 4 &&
               static_cast<int>(ProtocolError::DUPLICATE_HELLO) == 5 && static_cast<int>(ProtocolError::UNSUPPORTED_VERSION) == 6 &&
               static_cast<int>(ProtocolError::UNKNOWN_COMMAND) == 7 && static_cast<int>(ProtocolError::UNEXPECTED_FIELD) == 8 &&
               static_cast<int>(ProtocolError::TOO_MANY_CONNECTIONS) == 9,
           "protocol error codes are pinned");
}

// What a reader saw: comparable whatever chunks it came in.
struct Seen {
    std::uint8_t type;
    bool last;
    Correlation correlation;
    Bytes body;

    bool operator==(const Seen&) const = default;
};

struct Outcome {
    std::vector<Seen> frames;
    std::optional<FrameError> error;

    bool operator==(const Outcome&) const = default;
};

void drain(FrameReader& reader, Outcome& outcome) {
    while (!outcome.error) {
        const FrameResult result = reader.next();
        if (const Frame* frame = std::get_if<Frame>(&result)) {
            static_cast<void>(decodeEvent(*frame));
            outcome.frames.push_back(
                Seen{static_cast<std::uint8_t>(frame->type), frame->last, frame->correlation, Bytes(frame->body.begin(), frame->body.end())});
        } else if (const FrameError* error = std::get_if<FrameError>(&result)) {
            outcome.error = *error;
        } else {
            return;
        }
    }
}

Outcome readInChunks(FrameSource source, std::span<const std::byte> stream, std::size_t chunk) {
    FrameReader reader(source);
    Outcome outcome;
    for (std::size_t start = 0; start < stream.size(); start += chunk) {
        reader.feed(stream.subspan(start, std::min(chunk, stream.size() - start)));
        drain(reader, outcome);
    }
    return outcome;
}

Outcome readSplitAt(FrameSource source, std::span<const std::byte> stream, std::size_t split) {
    FrameReader reader(source);
    Outcome outcome;
    reader.feed(stream.first(split));
    drain(reader, outcome);
    reader.feed(stream.subspan(split));
    drain(reader, outcome);
    return outcome;
}

Bytes concatenated(const std::filesystem::path& root, const std::vector<std::string>& names) {
    Bytes stream;
    for (const std::string& name : names) {
        const Bytes frame = readFrame(root, name);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

void checkEverySplit(FrameSource source, const Bytes& stream, const std::string& what) {
    const Outcome whole = readInChunks(source, stream, stream.size());
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        expect(readSplitAt(source, stream, split) == whole, what + " reads the same split at byte " + std::to_string(split));
    }
    for (const std::size_t chunk : std::array<std::size_t, 5>{1, 2, 3, 7, 13}) {
        expect(readInChunks(source, stream, chunk) == whole, what + " reads the same in chunks of " + std::to_string(chunk));
    }
}

void testEverySplitPoint(const std::filesystem::path& root) {
    const Bytes commands = concatenated(root, {"hello", "command_add", "command_stop", "command_cancel", "command_replace"});
    const Outcome read = readInChunks(FrameSource::CLIENT, commands, commands.size());
    expect(read.frames.size() == 5 && !read.error, "the client's stream reads whole");
    checkEverySplit(FrameSource::CLIENT, commands, "a client's stream");

    std::vector<std::string> responses = {"welcome", "refused", "protocol_error"};
    for (const GoldenEvent& golden : goldenEvents()) {
        responses.emplace_back(golden.file);
    }
    const Bytes engine = concatenated(root, responses);
    expect(readInChunks(FrameSource::ENGINE, engine, engine.size()).frames.size() == 13, "the engine's stream reads whole");
    checkEverySplit(FrameSource::ENGINE, engine, "an engine's stream");

    Bytes broken = commands;
    broken[broken.size() - 36] = std::byte{9};  // the last Command's command byte
    const Outcome failed = readInChunks(FrameSource::CLIENT, broken, broken.size());
    expect(failed.frames.size() == 4 && failed.error && failed.error->code == ProtocolError::UNKNOWN_COMMAND,
           "frames before an error still read");
    checkEverySplit(FrameSource::CLIENT, broken, "a stream with an error");
}

void put16(Bytes& frame, std::size_t offset, std::uint16_t value) {  // little-endian
    frame[offset] = static_cast<std::byte>(value & 0xFFU);
    frame[offset + 1] = static_cast<std::byte>(value >> 8);
}

void expectError(const Bytes& stream, ProtocolError code, const std::string& what, Correlation correlation = goldenCorrelation) {
    const Outcome outcome = readInChunks(FrameSource::CLIENT, stream, stream.size());
    expect(outcome.error.has_value(), what + " is an error");
    expect(outcome.error->code == code, what + " has the wrong code");
    expect(outcome.error->correlation == correlation, what + " names the wrong correlation");
    checkEverySplit(FrameSource::CLIENT, stream, what);
}

Bytes after(const Bytes& hello, const Bytes& frame) {
    Bytes stream = hello;
    stream.insert(stream.end(), frame.begin(), frame.end());
    return stream;
}

void testOneCasePerProtocolError(const std::filesystem::path& root) {
    const Bytes hello = readFrame(root, "hello");
    const Bytes add = readFrame(root, "command_add");
    const Bytes cancel = readFrame(root, "command_cancel");
    const Bytes replace = readFrame(root, "command_replace");

    Bytes tooShortToBeAFrame = hello;
    put16(tooShortToBeAFrame, 0, 11);
    expectError(tooShortToBeAFrame, ProtocolError::BAD_LENGTH, "a length under the header", 0);
    FrameReader shortLength(FrameSource::CLIENT);
    shortLength.feed(std::span<const std::byte>(tooShortToBeAFrame).first(2));
    expect(std::get<FrameError>(shortLength.next()) == FrameError{ProtocolError::BAD_LENGTH, 0},
           "a length under the header fails at once, with no correlation to wait for");
    Bytes longCommand = add;
    longCommand.push_back(std::byte{0});
    put16(longCommand, 0, 49);
    expectError(after(hello, longCommand), ProtocolError::BAD_LENGTH, "the engine's exact length");

    Bytes wrongMagic = hello;
    wrongMagic[12] = std::byte{'X'};
    expectError(wrongMagic, ProtocolError::BAD_MAGIC, "a wrong magic");

    Bytes unknownType = add;
    unknownType[2] = std::byte{6};
    expectError(after(hello, unknownType), ProtocolError::UNKNOWN_FRAME_TYPE, "a type the engine doesn't take");
    expectError(after(hello, readFrame(root, "welcome")), ProtocolError::UNKNOWN_FRAME_TYPE, "an engine's frame");

    expectError(add, ProtocolError::HELLO_REQUIRED, "a command before Hello");
    expectError(after(hello, hello), ProtocolError::DUPLICATE_HELLO, "a second Hello");

    Bytes newVersion = hello;
    put16(newVersion, 20, 2);
    expectError(newVersion, ProtocolError::UNSUPPORTED_VERSION, "a version past this one");
    Bytes noVersion = hello;
    put16(noVersion, 20, 0);
    expectError(noVersion, ProtocolError::UNSUPPORTED_VERSION, "version 0");

    Bytes noCommand = add;
    noCommand[12] = std::byte{0};
    expectError(after(hello, noCommand), ProtocolError::UNKNOWN_COMMAND, "command byte 0");

    Bytes helloReserved = hello;
    helloReserved[23] = std::byte{1};
    expectError(helloReserved, ProtocolError::UNEXPECTED_FIELD, "Hello's zero bytes");
    Bytes flagged = add;
    flagged[3] = std::byte{1};
    expectError(after(hello, flagged), ProtocolError::UNEXPECTED_FIELD, "flags on a client's frame");
    Bytes reserved = add;
    reserved[47] = std::byte{1};
    expectError(after(hello, reserved), ProtocolError::UNEXPECTED_FIELD, "a command's reserved bytes");
    Bytes unknownFlag = add;
    unknownFlag[44] = std::byte{3};
    expectError(after(hello, unknownFlag), ProtocolError::UNEXPECTED_FIELD, "a flag this version doesn't know");
    for (const std::size_t offset : std::array<std::size_t, 10>{13, 14, 15, 28, 32, 36, 40, 44, 45, 39}) {
        Bytes used = cancel;
        used[offset] = std::byte{1};
        expectError(after(hello, used), ProtocolError::UNEXPECTED_FIELD, "a field a CANCEL leaves unused");
    }
    for (const std::size_t offset : std::array<std::size_t, 4>{13, 14, 15, 36}) {
        Bytes used = replace;
        used[offset] = std::byte{1};
        expectError(after(hello, used), ProtocolError::UNEXPECTED_FIELD, "a field a REPLACE leaves unused");
    }

    const Bytes tooMany = encoded([](auto out) { return encodeProtocolError(5, ProtocolError::TOO_MANY_CONNECTIONS, out); });
    FrameReader client(FrameSource::ENGINE);
    client.feed(tooMany);
    const FrameResult result = client.next();
    const Frame& frame = std::get<Frame>(result);
    expect(frame.type == FrameType::PROTOCOL_ERROR && frame.correlation == 5 && frame.last, "the gateway's own error reads");
    expect(decodeProtocolError(frame) == ProtocolErrorFrame{9, wireVersion}, "with its code and the engine's version");

    FrameReader stuck(FrameSource::CLIENT);
    stuck.feed(add);
    const FrameResult first = stuck.next();
    stuck.feed(hello);
    const FrameResult second = stuck.next();
    expect(std::get<FrameError>(first) == std::get<FrameError>(second), "after an error, every later call returns it");
}

void testHeadersAreJudgedBeforeTheirBodies(const std::filesystem::path& root) {
    Bytes unknown = readFrame(root, "command_add");
    unknown[2] = std::byte{6};
    FrameReader reader(FrameSource::CLIENT);
    reader.feed(readFrame(root, "hello"));
    expect(std::holds_alternative<Frame>(reader.next()), "Hello reads");
    reader.feed(std::span<const std::byte>(unknown).first(frameHeaderBytes));
    expect(std::holds_alternative<FrameError>(reader.next()), "no one waits for a body that will never be accepted");
}

// A client skips a frame it doesn't know and trailing bytes, by the length.
void testAClientSkipsWhatItDoesNotKnow(const std::filesystem::path& root) {
    Bytes unknown = readFrame(root, "order_cancelled");
    unknown[2] = std::byte{200};
    Bytes longer = readFrame(root, "order_cancelled");
    longer.insert(longer.end(), 4, std::byte{0xFF});
    put16(longer, 0, static_cast<std::uint16_t>(longer.size()));

    Bytes stream = unknown;
    stream.insert(stream.end(), longer.begin(), longer.end());
    checkEverySplit(FrameSource::ENGINE, stream, "a stream with what a client skips");
    FrameReader reader(FrameSource::ENGINE);
    reader.feed(stream);
    const FrameResult result = reader.next();
    const Frame& frame = std::get<Frame>(result);
    expect(frame.type == FrameType::ORDER_CANCELLED, "an unknown type is skipped");
    expect(decodeEvent(frame)->event == BookEvent{OrderCancelled{7, 5}}, "trailing bytes are ignored");
    expect(std::holds_alternative<NeedMore>(reader.next()), "and nothing is left");

    Bytes tooShort = readFrame(root, "order_cancelled");
    tooShort.pop_back();
    put16(tooShort, 0, static_cast<std::uint16_t>(tooShort.size()));
    checkEverySplit(FrameSource::ENGINE, tooShort, "a frame shorter than its type");
    FrameReader strict(FrameSource::ENGINE);
    strict.feed(tooShort);
    expect(std::get<FrameError>(strict.next()).code == ProtocolError::BAD_LENGTH, "a frame shorter than its type is an error");

    Bytes unknownReason = readFrame(root, "order_rejected");
    put16(unknownReason, 24, 999);
    FrameReader future(FrameSource::ENGINE);
    future.feed(unknownReason);
    const FrameResult read = future.next();
    expect(!decodeEvent(std::get<Frame>(read)), "a reason this release doesn't know decodes to nothing, not a wrong reason");
}

void testEncodersRefuseASmallSpan() {
    std::array<std::byte, 8> small{};
    expect(encodeHello(1, 1, small) == 0, "Hello does not fit");
    expect(encodeCommand(1, symbolOf("A"), replay::CancelCommand{1}, small) == 0, "Command does not fit");
    expect(encodeRefused(1, symbolOf("A").bytes(), 1, RefusedCode::ENGINE_STOPPING, small) == 0, "Refused does not fit");
    expect(encodeProtocolError(1, ProtocolError::BAD_LENGTH, small) == 0, "ProtocolError does not fit");
    expect(encodeEvent(1, symbolOf("A"), OrderCancelled{1, 1}, true, small) == 0, "an event does not fit");
    expect(std::all_of(small.begin(), small.end(), [](std::byte value) { return value == std::byte{0}; }), "and nothing is written");
}

// Every byte of every frame through every value, on both readers. The assertion is the sweep
// finishing clean under ASan and UBSan.
void testByteSweep(const std::filesystem::path& root) {
    const std::vector<std::string> names = {"hello",  "command_add",    "command_cancel", "command_replace",
                                            "refused", "protocol_error", "order_replaced", "order_rejected"};
    std::size_t reads = 0;
    for (const std::string& name : names) {
        const Bytes original = readFrame(root, name);
        const Bytes hello = readFrame(root, "hello");
        for (std::size_t offset = 0; offset < original.size(); ++offset) {
            for (unsigned int value = 0; value < 256; ++value) {
                Bytes frame = original;
                frame[offset] = static_cast<std::byte>(value);
                for (const FrameSource source : {FrameSource::CLIENT, FrameSource::ENGINE}) {
                    FrameReader reader(source);
                    if (source == FrameSource::CLIENT && name != "hello") {
                        reader.feed(hello);
                    }
                    reader.feed(frame);
                    Outcome outcome;
                    drain(reader, outcome);
                    reads += outcome.frames.size() + 1;
                }
            }
        }
    }
    expect(reads != 0, "the sweep ran");
}

void testNothingAllocatesPerFrame(const std::filesystem::path& root) {
    Bytes stream = readFrame(root, "hello");
    for (int i = 0; i < 16; ++i) {
        const Bytes frame = readFrame(root, "command_add");
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    FrameReader reader(FrameSource::CLIENT);
    std::array<std::byte, maxEventFrameBytes> out{};
    const Symbol symbol = symbolOf("AAPL");
    const BookEvent event = OrderAccepted{1, Side::BUY, 100, 5};

    const auto pass = [&] {
        reader.feed(stream);
        std::size_t frames = 0;
        while (std::holds_alternative<Frame>(reader.next())) {
            ++frames;
            expect(encodeEvent(1, symbol, event, true, out) == 36, "the event encodes");
        }
        return frames;
    };
    expect(pass() == 17, "the warm-up reads every frame");

    // The Hello was only for the first pass; a later one is all commands.
    const Bytes commands(stream.begin() + static_cast<std::ptrdiff_t>(helloFrameBytes), stream.end());
    std::size_t written = 0;
    const std::size_t before = allocations;
    for (int i = 0; i < 100; ++i) {
        reader.feed(commands);
        while (std::holds_alternative<Frame>(reader.next())) {
            written += encodeEvent(1, symbol, event, true, out);
        }
    }
    const std::size_t during = allocations - before;
    expect(during == 0, "a warm reader and the encoders allocate nothing per frame");
    expect(written == 100 * 16 * 36, "every frame was read, and every event encoded");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: wire_codec_tests <tests/wire directory>\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path root = argv[1];
    testEncodersWriteTheGoldenBytes(root);
    testFramesHaveTheirSizes(root);
    testACommandFrameIsARecordsBody(root);
    testEveryEventRoundTrips();
    testEveryReasonRoundTrips();
    testEverySplitPoint(root);
    testOneCasePerProtocolError(root);
    testHeadersAreJudgedBeforeTheirBodies(root);
    testAClientSkipsWhatItDoesNotKnow(root);
    testEncodersRefuseASmallSpan();
    testByteSweep(root);
    testNothingAllocatesPerFrame(root);
    std::cout << "wire codec tests passed" << std::endl;
    return EXIT_SUCCESS;
}
