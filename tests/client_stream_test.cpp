#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "order_book/network/client_stream.hpp"
#include "order_book/network/wire_codec.hpp"

// ClientStream on its own: this file plays the engine with the wire encoders, so every byte the
// client writes and every chunking of what it reads is in the test's hands. See
// docs/internals/network.md.
namespace {

using namespace order_book;
using namespace order_book::network;
using Bytes = std::vector<std::byte>;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbol(const char* text) {
    return *Symbol::parse(text);
}

replay::AddCommand add(OrderId id, Side side, Price price, Quantity quantity) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

replay::CancelCommand cancel(OrderId id) {
    replay::CancelCommand command{};
    command.orderId = id;
    return command;
}

void append(Bytes& bytes, std::size_t size, const std::array<std::byte, 64>& frame) {
    bytes.insert(bytes.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
}

Bytes welcome(std::uint16_t version = wireVersion) {
    std::array<std::byte, 64> frame{};
    Bytes bytes;
    append(bytes, encodeWelcome(0, version, frame), frame);
    return bytes;
}

Bytes event(Correlation correlation, const char* text, const BookEvent& what, bool last) {
    std::array<std::byte, 64> frame{};
    Bytes bytes;
    append(bytes, encodeEvent(correlation, symbol(text), what, last, frame), frame);
    return bytes;
}

Bytes refused(Correlation correlation, const char* text, OrderId id, RefusedCode code) {
    std::array<std::byte, 64> frame{};
    Bytes bytes;
    append(bytes, encodeRefused(correlation, symbol(text).bytes(), id, code, frame), frame);
    return bytes;
}

Bytes protocolError(Correlation correlation, ProtocolError code) {
    std::array<std::byte, 64> frame{};
    Bytes bytes;
    append(bytes, encodeProtocolError(correlation, code, frame), frame);
    return bytes;
}

Bytes joined(std::initializer_list<Bytes> parts) {
    Bytes bytes;
    for (const Bytes& part : parts) {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return bytes;
}

// Everything the client writes, taking at most chunk bytes a write.
Bytes drain(ClientStream& stream, std::size_t chunk = 1 << 20) {
    Bytes written;
    for (std::span<const std::byte> next = stream.outgoing(); !next.empty(); next = stream.outgoing()) {
        const std::size_t taken = std::min(chunk, next.size());
        written.insert(written.end(), next.begin(), next.begin() + static_cast<std::ptrdiff_t>(taken));
        stream.wrote(taken);
    }
    return written;
}

std::vector<ClientCommand> twoInstruments() {
    return {
        {2, symbol("MSFT"), add(1, Side::SELL, 101, 5)},
        {3, symbol("AAPL"), add(1, Side::BUY, 99, 4)},
        {4, symbol("MSFT"), add(2, Side::BUY, 101, 2)},
        {5, symbol("AAPL"), cancel(1)},
    };
}

void testItWritesHelloThenEachCommandUnderItsCorrelation() {
    ClientStream stream(twoInstruments());
    expect(!stream.allWritten(), "nothing is written before the first write");
    const Bytes written = drain(stream, 7);
    expect(stream.allWritten(), "and everything is written once outgoing runs dry");

    std::array<std::byte, 64> frame{};
    Bytes expected;
    append(expected, encodeHello(0, wireVersion, frame), frame);
    for (const ClientCommand& command : twoInstruments()) {
        append(expected, encodeCommand(command.correlation, command.symbol, command.command, frame), frame);
    }
    expect(written == expected, "Hello under correlation 0, then each command in order, under its own");
}

// The first four lines of valid/v3_two_instruments.csv, answered out of line order across
// instruments, as a sharded engine may.
Bytes answersOutOfOrder() {
    return joined({
        welcome(),
        event(3, "AAPL", OrderAccepted{1, Side::BUY, 99, 4}, true),
        event(5, "AAPL", OrderCancelled{1, 4}, true),
        event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
        event(4, "MSFT", OrderAccepted{2, Side::BUY, 101, 2}, false),
        event(4, "MSFT", TradeExecuted{2, 1, 101, 2}, true),
    });
}

void testAnswersPrintInReplaysSections() {
    ClientStream stream(twoInstruments());
    drain(stream);
    stream.received(answersOutOfOrder());
    expect(stream.reading(), "the stream is still read until it ends");
    stream.ended();
    expect(!stream.reading(), "and not after");
    expect(stream.end() == ConnectionEnd::ANSWERED, "every command answered");
    expect(stream.diagnostics().empty(), "with nothing to say on stderr");
    expect(stream.sections() ==
               "symbol AAPL\n"
               "line 3: OrderAccepted id=1 side=BUY price=99 qty=4\n"
               "line 5: OrderCancelled id=1 qty=4\n"
               "\n"
               "symbol MSFT\n"
               "line 2: OrderAccepted id=1 side=SELL price=101 qty=5\n"
               "line 4: OrderAccepted id=2 side=BUY price=101 qty=2\n"
               "line 4: TradeExecuted incoming=2 resting=1 price=101 qty=2\n",
           "sorted by symbol then line, a line's frames in the order they came:\n" + stream.sections());
}

// Whatever the chunking, the same sections.
void testEveryChunkingReadsTheSame() {
    const Bytes stream = answersOutOfOrder();
    ClientStream whole(twoInstruments());
    drain(whole);
    whole.received(stream);
    whole.ended();
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        ClientStream chunked(twoInstruments());
        drain(chunked);
        chunked.received(std::span<const std::byte>(stream).first(split));
        chunked.received(std::span<const std::byte>(stream).subspan(split));
        chunked.ended();
        expect(chunked.end() == ConnectionEnd::ANSWERED && chunked.sections() == whole.sections(),
               "split at byte " + std::to_string(split));
    }
    ClientStream bytewise(twoInstruments());
    drain(bytewise);
    for (const std::byte b : stream) {
        bytewise.received(std::span<const std::byte>(&b, 1));
    }
    bytewise.ended();
    expect(bytewise.sections() == whole.sections(), "and a byte at a time");
}

// A Refused frame overtakes earlier answers, yet prints in its line's place.
void testARefusalPrintsInPlace() {
    std::vector<ClientCommand> commands = twoInstruments();
    commands.push_back({6, symbol("ZZZ"), cancel(9)});
    commands.push_back({7, symbol("AAPL"), add(3, Side::BUY, 98, 1)});
    ClientStream stream(commands);
    drain(stream);
    stream.received(joined({
        welcome(),
        refused(6, "ZZZ", 9, RefusedCode::UNKNOWN_INSTRUMENT),
        refused(7, "AAPL", 3, RefusedCode::ENGINE_STOPPING),
        event(3, "AAPL", OrderAccepted{1, Side::BUY, 99, 4}, true),
        event(5, "AAPL", OrderCancelled{1, 4}, true),
        event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
        event(4, "MSFT", OrderAccepted{2, Side::BUY, 101, 2}, true),
    }));
    stream.ended();
    expect(stream.end() == ConnectionEnd::REFUSED, "every command answered, some refused, is REFUSED");
    expect(stream.sections() ==
               "symbol AAPL\n"
               "line 3: OrderAccepted id=1 side=BUY price=99 qty=4\n"
               "line 5: OrderCancelled id=1 qty=4\n"
               "line 7: Refused symbol=AAPL id=3 code=ENGINE_STOPPING\n"
               "\n"
               "symbol MSFT\n"
               "line 2: OrderAccepted id=1 side=SELL price=101 qty=5\n"
               "line 4: OrderAccepted id=2 side=BUY price=101 qty=2\n"
               "\n"
               "symbol ZZZ\n"
               "line 6: Refused symbol=ZZZ id=9 code=UNKNOWN_INSTRUMENT\n",
           "each refusal in its own line's place:\n" + stream.sections());
    expect(stream.diagnostics().empty(), "a refusal belongs to a line, not to stderr");
}

ClientStream answeredWith(const std::vector<ClientCommand>& commands, const Bytes& stream) {
    ClientStream client(commands);
    drain(client);
    client.received(stream);
    client.ended();
    return client;
}

std::string lines(const std::vector<std::string>& diagnostics) {
    std::string text;
    for (const std::string& line : diagnostics) {
        text += line + '\n';
    }
    return text;
}

void testAStreamEndingEarlyIsLost() {
    const ClientStream stream = answeredWith(twoInstruments(), joined({
        welcome(),
        event(3, "AAPL", OrderAccepted{1, Side::BUY, 99, 4}, true),
        event(4, "MSFT", OrderAccepted{2, Side::BUY, 101, 2}, false),
    }));
    expect(stream.end() == ConnectionEnd::LOST, "a stream that ends before every answer is LOST");
    expect(lines(stream.diagnostics()) == "3 of 4 commands unanswered: what ran is unknown\n",
           "saying how many went unanswered:\n" + lines(stream.diagnostics()));
    expect(stream.sections() ==
               "symbol AAPL\n"
               "line 3: OrderAccepted id=1 side=BUY price=99 qty=4\n"
               "\n"
               "symbol MSFT\n"
               "line 4: OrderAccepted id=2 side=BUY price=101 qty=2\n",
           "and printing what did come:\n" + stream.sections());
}

// 3 wins over 4: a refusal says that command didn't run, but nothing is known of the rest.
void testALossWinsOverARefusal() {
    const ClientStream stream = answeredWith(twoInstruments(), joined({
        welcome(),
        refused(2, "MSFT", 1, RefusedCode::ENGINE_STOPPING),
    }));
    expect(stream.end() == ConnectionEnd::LOST, "a refusal and then a loss is LOST, not REFUSED");
}

// #34: exit 3 is a ProtocolError "before every answer came". One after them all still goes to
// stderr, but everything that ran is known.
void testAProtocolErrorGoesToTheDiagnosticsAndIsLostBeforeEveryAnswer() {
    const ClientStream afterEveryAnswer = answeredWith(twoInstruments(), joined({
        welcome(),
        event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
        event(3, "AAPL", OrderAccepted{1, Side::BUY, 99, 4}, true),
        event(4, "MSFT", OrderAccepted{2, Side::BUY, 101, 2}, true),
        event(5, "AAPL", OrderCancelled{1, 4}, true),
        protocolError(5, ProtocolError::UNEXPECTED_FIELD),
    }));
    expect(afterEveryAnswer.end() == ConnectionEnd::ANSWERED, "a ProtocolError after every answer loses nothing");
    expect(lines(afterEveryAnswer.diagnostics()) == "the engine sent ProtocolError UNEXPECTED_FIELD at line 5\n",
           "but still says so, naming its code and line:\n" + lines(afterEveryAnswer.diagnostics()));

    const ClientStream beforeOne = answeredWith(twoInstruments(), joined({
        welcome(),
        event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
        event(3, "AAPL", OrderAccepted{1, Side::BUY, 99, 4}, true),
        event(4, "MSFT", OrderAccepted{2, Side::BUY, 101, 2}, true),
        protocolError(5, ProtocolError::UNEXPECTED_FIELD),
    }));
    expect(beforeOne.end() == ConnectionEnd::LOST, "one before an answer is LOST");

    const ClientStream atNoLine = answeredWith(twoInstruments(), joined({
        welcome(),
        protocolError(0, ProtocolError::TOO_MANY_CONNECTIONS),
    }));
    expect(atNoLine.end() == ConnectionEnd::LOST, "a connection turned away is LOST");
    expect(lines(atNoLine.diagnostics()) ==
               "the engine sent ProtocolError TOO_MANY_CONNECTIONS\n4 of 4 commands unanswered: what ran is unknown\n",
           "and correlation 0 names no line:\n" + lines(atNoLine.diagnostics()));
    expect(atNoLine.sections().empty(), "a ProtocolError prints in no section");

    const ClientStream afterIt = answeredWith(twoInstruments(), joined({
        welcome(),
        protocolError(0, ProtocolError::TOO_MANY_CONNECTIONS),
        event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
    }));
    expect(afterIt.sections().empty(), "nothing after a ProtocolError counts, since it is the last thing said");
}

// The engine broke the protocol: stop reading, since nothing after can be trusted.
void testAFrameThisClientCantReadBreaksTheStream() {
    struct Case {
        const char* name;
        Bytes stream;
        std::string diagnostic;
    };
    Bytes shortFrame{std::byte{5}, std::byte{0}};
    Bytes unknownReason = event(2, "MSFT", OrderRejected{1, RejectionReason::INVALID_PRICE}, true);
    unknownReason[24] = std::byte{0xff};  // the reason's low byte: a code no release has
    Bytes secondLast = event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true);
    const std::vector<Case> cases = {
        {"a length under a header", joined({welcome(), shortFrame}), "the engine sent a frame this client can't read: BAD_LENGTH"},
        {"an answer before Welcome", event(2, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true),
         "the engine answered before its Welcome"},
        {"another version", welcome(2), "the engine sent a frame this client can't read: UNSUPPORTED_VERSION"},
        {"a second Welcome", joined({welcome(), welcome()}), "the engine sent a second Welcome"},
        {"a line never sent", joined({welcome(), event(9, "MSFT", OrderAccepted{1, Side::SELL, 101, 5}, true)}),
         "the engine answered line 9, which awaits no answer"},
        {"a line answered twice", joined({welcome(), secondLast, secondLast}), "the engine answered line 2, which awaits no answer"},
        {"a reason it doesn't know", joined({welcome(), unknownReason}), "the engine sent a frame this client can't read at line 2"},
        {"an event under another symbol", joined({welcome(), event(2, "AAPL", OrderAccepted{1, Side::SELL, 101, 5}, true)}),
         "the engine answered line 2 under another symbol than MSFT"},
        {"a refusal under another symbol", joined({welcome(), refused(2, "AAPL", 1, RefusedCode::UNKNOWN_INSTRUMENT)}),
         "the engine answered line 2 under another symbol than MSFT"},
    };
    for (const Case& broken : cases) {
        ClientStream stream(twoInstruments());
        drain(stream);
        stream.received(broken.stream);
        expect(!stream.reading(), std::string(broken.name) + " stops the reading");
        stream.received(answersOutOfOrder());
        stream.ended();
        expect(stream.end() == ConnectionEnd::LOST, std::string(broken.name) + " is LOST");
        expect(!stream.diagnostics().empty() && stream.diagnostics().front() == broken.diagnostic,
               std::string(broken.name) + " says so first:\n" + lines(stream.diagnostics()));
    }
}

void testAFrameTypeItDoesntKnowIsSkipped() {
    Bytes unknown(16, std::byte{0});
    unknown[0] = std::byte{16};
    unknown[2] = std::byte{6};  // free for frames that are not events
    unknown[3] = std::byte{1};
    unknown[4] = std::byte{2};
    const ClientStream stream = answeredWith(twoInstruments(), joined({unknown, answersOutOfOrder()}));
    expect(stream.end() == ConnectionEnd::ANSWERED && stream.diagnostics().empty(), "a frame type this client doesn't know is skipped");
}

void testNoCommandsStillNeedsAWelcome() {
    const ClientStream welcomed = answeredWith({}, welcome());
    expect(welcomed.end() == ConnectionEnd::ANSWERED && welcomed.sections().empty() && welcomed.diagnostics().empty(),
           "a file with no commands, welcomed, is answered and prints nothing");
    const ClientStream turnedAway = answeredWith({}, Bytes{});
    expect(turnedAway.end() == ConnectionEnd::LOST, "but a stream that ends with no Welcome is LOST");
    expect(lines(turnedAway.diagnostics()) == "the engine closed the connection before its Welcome\n",
           "saying so:\n" + lines(turnedAway.diagnostics()));
}

}  // namespace

int main() {
    testAStreamEndingEarlyIsLost();
    testALossWinsOverARefusal();
    testAProtocolErrorGoesToTheDiagnosticsAndIsLostBeforeEveryAnswer();
    testAFrameThisClientCantReadBreaksTheStream();
    testAFrameTypeItDoesntKnowIsSkipped();
    testNoCommandsStillNeedsAWelcome();
    testItWritesHelloThenEachCommandUnderItsCorrelation();
    testAnswersPrintInReplaysSections();
    testEveryChunkingReadsTheSame();
    testARefusalPrintsInPlace();
    std::cout << "client stream tests passed" << std::endl;
    return EXIT_SUCCESS;
}
