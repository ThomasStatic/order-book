#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine_harness.hpp"
#include "gateway_frames.hpp"
#include "gateway_rig.hpp"
#include "gateway_stepper.hpp"
#include "order_book/little_endian.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/in_memory_socket_set.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

// GatewayThread::pass on the in-memory SocketSet, stepped by one thread with no sleep and no real
// socket. See docs/internals/network.md.
namespace {

using namespace order_book;
using namespace order_book::network;
using namespace order_book::runtime;
using namespace order_book::little_endian;
using order_book::gateway_testing::Bytes;
using order_book::gateway_testing::helloFrame;
using order_book::engine_testing::Client;
using order_book::engine_testing::Received;
using order_book::engine_testing::Rig;
using order_book::engine_testing::ShardActor;
using order_book::engine_testing::refusedForStopping;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(const char* text) {
    return *Symbol::parse(text);
}

Instrument on(const char* symbol, std::uint32_t shard) {
    return Instrument{symbolOf(symbol), shard, std::nullopt, std::nullopt, std::nullopt};
}

replay::AddCommand limit(OrderId id, Side side, Price price, Quantity quantity = 10) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

Bytes commandFrame(Correlation correlation, const char* symbol, const replay::Command& command) {
    Bytes frame(64);
    frame.resize(encodeCommand(correlation, symbolOf(symbol), command, frame));
    expect(!frame.empty(), "the command frame fits");
    return frame;
}

Bytes joined(std::initializer_list<Bytes> frames) {
    Bytes all;
    for (const Bytes& frame : frames) {
        all.insert(all.end(), frame.begin(), frame.end());
    }
    return all;
}

void introduce(Rig& rig, Client& client) {
    client.send(helloFrame());
    rig.gateway->pass();
    expect(client.frames().back().type == FrameType::WELCOME, "the client was welcomed");
}

std::vector<CommandElement> takeElements(Ring<CommandElement>& ring, std::size_t most) {
    std::vector<CommandElement> taken;
    ring.drain(most, [&taken](const CommandElement& element) { taken.push_back(element); });
    return taken;
}

Bytes refusedStream(OrderId count) {
    Bytes stream;
    for (OrderId id = 1; id <= count; ++id) {
        const Bytes frame = commandFrame(id, "ZZZZ", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

void testHelloIsWelcomed() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    client.send(helloFrame(77));
    rig.gateway->pass();

    const std::vector<Received>& frames = client.frames();
    expect(frames.size() == 1 && frames[0].type == FrameType::WELCOME, "a Hello gets one Welcome");
    expect(frames[0].correlation == 77 && frames[0].last, "echoing its correlation, flagged last");
    expect(frames[0].version == wireVersion, "with the version accepted");
}

void testCommandsNeedNotWaitForWelcome() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    client.send(joined({helloFrame(), commandFrame(5, "AAPL", limit(1, Side::BUY, 100))}));
    rig.settle();

    const std::vector<Received>& frames = client.frames();
    expect(frames.size() == 2 && frames[0].type == FrameType::WELCOME, "the Welcome comes first");
    expect(frames[1].correlation == 5 && frames[1].last, "then the answer, under the command's correlation");
    expect(frames[1].event->symbol == symbolOf("AAPL"), "naming its instrument");
    expect(std::holds_alternative<OrderAccepted>(frames[1].event->event), "and it is the book's own answer");
}

void testEachInstrumentReachesItsOwnBook() {
    Rig rig({on("AAPL", 0), on("MSFT", 1), on("GOOG", 1)}, 2, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(joined({commandFrame(1, "MSFT", limit(1, Side::BUY, 100)), commandFrame(2, "GOOG", limit(1, Side::SELL, 200)),
                        commandFrame(3, "AAPL", limit(1, Side::BUY, 300))}));
    rig.settle();

    std::map<Correlation, Symbol> symbols;
    for (const Received& frame : client.frames()) {
        if (frame.event) {
            expect(std::holds_alternative<OrderAccepted>(frame.event->event), "one ID lives on each book");
            symbols.insert({frame.correlation, frame.event->symbol});
        }
    }
    expect(symbols.size() == 3 && symbols.at(1) == symbolOf("MSFT") && symbols.at(2) == symbolOf("GOOG") &&
               symbols.at(3) == symbolOf("AAPL"),
           "each response names the slot's symbol on its shard");
}

void testACascadeComesWholeAndInOrder() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(commandFrame(1, "AAPL", limit(1, Side::SELL, 100, 5)));
    rig.settle();
    client.send(commandFrame(2, "AAPL", limit(2, Side::SELL, 101, 5)));
    rig.settle();
    client.send(commandFrame(3, "AAPL", limit(3, Side::BUY, 101, 10)));
    rig.settle();

    InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard witness(directory, 0);
    witness.process(limit(1, Side::SELL, 100, 5), 0);
    witness.process(limit(2, Side::SELL, 101, 5), 0);
    witness.process(limit(3, Side::BUY, 101, 10), 0);
    const std::span<const BookEvent> expected = witness.result().events();
    expect(expected.size() >= 3, "the command makes more events than the ring has slots");

    std::vector<Received> cascade;
    for (const Received& frame : client.frames()) {
        if (frame.correlation == 3) {
            cascade.push_back(frame);
        }
    }
    expect(cascade.size() == expected.size(), "every event of the cascade arrives");
    for (std::size_t i = 0; i < cascade.size(); ++i) {
        expect(cascade[i].event->event == expected[i], "in the order the book made them");
        expect(cascade[i].last == (i + 1 == cascade.size()), "and only the last is flagged");
    }
    expect(rig.waitsInPlace > 0, "the matching thread really did wait in place");
}

void testEntryAnswersWithoutTheBook() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);

    Bytes badSide = commandFrame(8, "AAPL", limit(9, Side::BUY, 100));
    badSide[13] = std::byte{0x7f};  // the body's side byte
    client.send(joined({commandFrame(7, "ZZZZ", limit(4, Side::BUY, 100)), badSide}));
    rig.gateway->pass();

    expect(!rig.shards[0]->commands.hasItems(), "neither reached a command ring");
    const std::vector<Received>& frames = client.frames();
    expect(frames.size() == 3, "and both were answered in that same pass");
    expect(frames[1].type == FrameType::REFUSED && frames[1].correlation == 7 && frames[1].last, "an unknown symbol is refused");
    expect(frames[1].refused->orderId == 4 && frames[1].refused->code == static_cast<std::uint16_t>(RefusedCode::UNKNOWN_INSTRUMENT),
           "for that reason, naming the order");
    expect(frames[2].correlation == 8 && frames[2].last, "a malformed command gets the book's answer");
    const OrderRejected* rejected = std::get_if<OrderRejected>(&frames[2].event->event);
    expect(rejected != nullptr && rejected->orderId == 9 && rejected->reason == RejectionReason::INVALID_SIDE,
           "with the book's own reason");
    expect(frames[2].event->symbol == symbolOf("AAPL"), "under its symbol");
}

void testOneWritePerConnectionPerPass() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    const std::size_t afterWelcome = rig.sockets.writes(client.socket);

    client.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), commandFrame(2, "AAPL", limit(2, Side::BUY, 99))}));
    rig.gateway->pass();
    expect(rig.sockets.writes(client.socket) == afterWelcome, "nothing to say yet, so no write");
    rig.match();
    rig.gateway->pass();
    expect(rig.sockets.writes(client.socket) == afterWelcome + 1, "two answers go out in one write");
    expect(client.answered() == 2, "both of them");
    rig.gateway->pass();
    expect(rig.sockets.writes(client.socket) == afterWelcome + 1, "and a pass with nothing new writes nothing");
}

void testAShortWriteKeepsTheRest() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), commandFrame(2, "AAPL", limit(2, Side::BUY, 99))}));
    rig.sockets.limitWrite(3);
    rig.settle();
    expect(client.answered() == 2, "a socket taking 3 bytes at a time loses nothing");
}

void testAPassTakesAtMost64Frames() {
    Rig rig({on("AAPL", 0)}, 1, 256);
    Client client(rig.sockets);
    introduce(rig, client);

    Bytes stream;
    for (OrderId id = 1; id <= 100; ++id) {
        const Bytes frame = commandFrame(id, "AAPL", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    client.send(stream);

    rig.gateway->pass();
    expect(rig.match() == 64, "the first pass queues 64 of them");
    expect(!rig.gateway->mayWait(), "frames left in the buffer keep the gateway awake");
    const std::size_t waits = rig.sockets.waits();
    const std::size_t looks = rig.sockets.looks();
    rig.gateway->pass();
    expect(rig.match() == 36, "and the next pass the rest, which were already read");
    expect(rig.sockets.waits() == waits, "with no sleep while frames sat in the buffer");
    expect(rig.sockets.looks() == looks, "nor even a look, so a streaming client pays no system call for it");
}

// Every one of these is answered at entry, so nothing is in flight and nothing is unsent, and only
// knowing there are more frames in the reader stops the thread sleeping on a socket with nothing
// new to say.
void testFramesInTheReaderKeepItAwake() {
    Rig rig({on("AAPL", 0)}, 1, 256);
    Client client(rig.sockets);
    introduce(rig, client);

    Bytes stream;
    for (OrderId id = 1; id <= 100; ++id) {
        const Bytes frame = commandFrame(id, "ZZZZ", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    client.send(stream);
    rig.gateway->pass();
    expect(client.answered() == 64, "the first pass answers 64");
    expect(!rig.gateway->mayWait(), "and the rest still in the reader keep it awake");
    rig.gateway->pass();
    expect(client.answered() == 100, "the next pass answers those");
    expect(rig.gateway->mayWait(), "and only then may it sleep");
}

void testACommandRingsItsShardsDoorbell() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);

    ShardActor& actor = *rig.shards[0];
    actor.doorbell.announceSleep();
    client.send(commandFrame(1, "AAPL", limit(1, Side::BUY, 100)));
    rig.gateway->pass();
    actor.doorbell.wait();  // hangs if nothing rang, and the CTest timeout fails it
    actor.doorbell.cancelSleep();
    expect(rig.match() == 1, "the woken thread finds the command");
}

void testAStreamingClientCantStopTheWaitForEver() {
    Rig rig({on("AAPL", 0)}, 1, 4096);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(refusedStream(64 * 40));
    rig.gateway->pass();  // reads all of it, and answers 64

    std::size_t looks = rig.sockets.looks() + rig.sockets.waits();
    std::size_t passesSkipping = 0;
    for (int pass = 0; pass < 30; ++pass) {
        rig.gateway->pass();
        const std::size_t now = rig.sockets.looks() + rig.sockets.waits();
        if (now == looks) {
            ++passesSkipping;
        } else {
            expect(passesSkipping == mostWaitsSkipped, "the wait is skipped exactly mostWaitsSkipped passes in a row");
            passesSkipping = 0;
            looks = now;
        }
    }
    expect(passesSkipping < mostWaitsSkipped, "and then it looks, though frames are still waiting");
}

void testAConnectionsBudgetIsItsOwn() {
    Rig rig({on("AAPL", 0)}, 1, 256);
    Client busy(rig.sockets);
    Client quiet(rig.sockets);
    introduce(rig, busy);
    introduce(rig, quiet);

    Bytes stream;
    for (OrderId id = 1; id <= 100; ++id) {
        const Bytes frame = commandFrame(id, "AAPL", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    busy.send(stream);
    quiet.send(commandFrame(500, "AAPL", limit(500, Side::BUY, 100)));
    rig.gateway->pass();
    const std::vector<CommandElement> queued = takeElements(rig.shards[0]->commands, 256);
    expect(queued.size() == 65 && queued.back().correlation == 500, "a streaming client doesn't use up another's turn");
}

void testAFullRingParksAndLosesNothing() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client client(rig.sockets);
    introduce(rig, client);

    Bytes stream;
    for (OrderId id = 1; id <= 5; ++id) {
        const Bytes frame = commandFrame(id, "AAPL", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    rig.sockets.limitRead(48 * 3);
    client.send(stream);
    rig.gateway->pass();
    expect(!rig.gateway->mayWait(), "a parked connection keeps the gateway awake");

    const std::size_t reads = rig.sockets.reads(client.socket);
    rig.gateway->pass();
    expect(rig.sockets.reads(client.socket) == reads, "a parked connection is not read");
    expect(client.answered() == 0, "and nothing was refused");

    rig.settle();
    const std::vector<Received>& frames = client.frames();
    expect(client.answered() == 5, "every command is answered once there is room");
    for (std::size_t i = 1; i < frames.size(); ++i) {
        expect(frames[i].type != FrameType::REFUSED && frames[i].correlation == i, "in the order they were sent");
    }
}

void testParkedConnectionsRetryOldestFirst() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client latest(rig.sockets);  // read first in a pass, by its index, so only age can put it last
    Client earliest(rig.sockets);
    Client newcomer(rig.sockets);
    introduce(rig, latest);
    introduce(rig, earliest);
    introduce(rig, newcomer);
    Ring<CommandElement>& ring = rig.shards[0]->commands;

    earliest.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), commandFrame(2, "AAPL", limit(2, Side::BUY, 100)),
                          commandFrame(3, "AAPL", limit(3, Side::BUY, 100))}));
    rig.gateway->pass();
    latest.send(commandFrame(4, "AAPL", limit(4, Side::BUY, 100)));
    rig.gateway->pass();
    expect(takeElements(ring, 1).front().correlation == 1, "the ring held the first two");

    rig.gateway->pass();
    newcomer.send(commandFrame(5, "AAPL", limit(5, Side::BUY, 100)));
    expect(takeElements(ring, 1).front().correlation == 2, "room for one");
    expect(takeElements(ring, 1).front().correlation == 3, "and the oldest parked took it");

    takeElements(ring, 1);
    rig.gateway->pass();
    const std::vector<CommandElement> next = takeElements(ring, 2);
    expect(next.size() == 2 && next[0].correlation == 4, "the next oldest goes next");
    expect(next[1].correlation == 5, "and the newcomer after it");
}

// A matching thread can make room after a pass's retry has failed and before its read. The read
// plays that thread here, and the command it brings must still queue behind the parked one.
void testANewCommandCantPassAParkedOne() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client newcomer(rig.sockets);
    Client parked(rig.sockets);
    introduce(rig, newcomer);
    introduce(rig, parked);
    Ring<CommandElement>& ring = rig.shards[0]->commands;

    parked.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), commandFrame(2, "AAPL", limit(2, Side::BUY, 100)),
                        commandFrame(3, "AAPL", limit(3, Side::BUY, 100))}));
    rig.gateway->pass();
    newcomer.send(commandFrame(4, "AAPL", limit(4, Side::BUY, 100)));

    bool emptied = false;
    rig.sockets.onRead([&](SocketId socket) {
        if (socket == newcomer.socket && !emptied) {
            emptied = true;
            expect(takeElements(ring, 2).size() == 2, "the ring was full, so the retry had failed");
        }
    });
    rig.gateway->pass();
    expect(emptied && !ring.hasItems(), "the new command did not take the room");

    rig.gateway->pass();
    const std::vector<CommandElement> next = takeElements(ring, 2);
    expect(next.size() == 2 && next[0].correlation == 3 && next[1].correlation == 4, "the parked command goes first");
}

void testAParkedConnectionHoldsItsOtherShardToo() {
    Rig rig({on("AAPL", 0), on("MSFT", 1)}, 2, 2);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), commandFrame(2, "AAPL", limit(2, Side::BUY, 100)),
                        commandFrame(3, "AAPL", limit(3, Side::BUY, 100)), commandFrame(4, "MSFT", limit(4, Side::BUY, 100))}));
    rig.gateway->pass();
    rig.gateway->pass();
    expect(!rig.shards[1]->commands.hasItems(), "its stream is ordered, so a later command waits behind the parked one");
    rig.settle();
    expect(client.answered() == 4, "and goes once it can");
}

void testAReusedIndexHearsNothingOfTheDead() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client dead(rig.sockets);
    introduce(rig, dead);
    dead.send(commandFrame(1, "AAPL", limit(1, Side::BUY, 100)));
    rig.gateway->pass();
    const CommandElement deadElement = takeElements(rig.shards[0]->commands, 1).front();

    rig.sockets.clientReset(dead.socket);
    rig.gateway->pass();
    expect(rig.sockets.closed(dead.socket), "a reset connection is closed");
    expect(!rig.gateway->mayWait(), "its command is still in flight");

    Client reuser(rig.sockets);
    introduce(rig, reuser);
    reuser.send(commandFrame(2, "AAPL", limit(2, Side::BUY, 100)));
    rig.gateway->pass();
    const CommandElement reuserElement = takeElements(rig.shards[0]->commands, 1).front();
    expect(connectionIndex(reuserElement.connection) == connectionIndex(deadElement.connection), "the index really was reused");
    expect(reuserElement.connection != deadElement.connection, "under a new generation");

    const BookEvent accepted = OrderAccepted{1, Side::BUY, 100, 10};
    Ring<ResponseRecord>& responses = rig.shards[0]->responses;
    expect(responses.tryPush(ResponseRecord{1, deadElement.connection, 0, accepted, ResponseKind::LAST_EVENT}), "room for it");
    rig.gateway->pass();
    expect(reuser.answered() == 0, "the dead connection's response reached nobody");

    expect(responses.tryPush(ResponseRecord{2, reuserElement.connection, 0, accepted, ResponseKind::LAST_EVENT}), "room for it");
    rig.gateway->pass();
    expect(reuser.answered() == 1, "and its own still arrives");
    expect(rig.gateway->mayWait(), "and with both answered, nothing is in flight");
}

// A gateway with nothing outstanding looks for the spin window before it sleeps.
void testTheSpinWindowComesBeforeSleep() {
    GatewaySettings settings;
    Rig rig({on("AAPL", 0)}, 1, 16);
    settings.clock = [&rig] { return rig.now; };
    settings.spinWindow = std::chrono::microseconds(10);
    InMemorySocketSet sockets;
    GatewayThread gateway(rig.directory, {ShardLink{&rig.shards[0]->commands, &rig.shards[0]->responses, &rig.shards[0]->doorbell}},
                          sockets, settings);

    gateway.pass();
    expect(sockets.waits() == 0 && sockets.looks() == 1, "at first it only looks");
    rig.now += std::chrono::microseconds(10) - std::chrono::nanoseconds(1);
    gateway.pass();
    expect(sockets.waits() == 0, "and still does a moment short of the window");
    rig.now += std::chrono::nanoseconds(1);
    gateway.pass();
    expect(sockets.waits() == 1, "then it sleeps");
    gateway.pass();
    expect(sockets.waits() == 1, "and after waking spins again before the next sleep");

    GatewaySettings never = settings;
    never.spinWindow = unlimitedSpin;
    InMemorySocketSet spinning;
    GatewayThread spinner(rig.directory, {ShardLink{&rig.shards[0]->commands, &rig.shards[0]->responses, &rig.shards[0]->doorbell}},
                          spinning, never);
    spinner.pass();
    rig.now += std::chrono::hours(1);
    spinner.pass();
    expect(spinning.waits() == 0, "an unlimited window never sleeps");
}

// Unsent bytes are a reason to watch a socket, not to stay awake.
void testABackloggedClientLetsTheGatewaySleep() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    rig.sockets.limitWrite(0);
    client.send(refusedStream(1));
    rig.gateway->pass();
    expect(rig.gateway->mayWait(), "with nothing in flight it may sleep");

    bool watchedForWriting = false;
    rig.sockets.onWait([&](std::span<const SocketInterest> interest) {
        watchedForWriting = interest.size() == 1 && interest[0].socket == client.socket && interest[0].write;
    });
    rig.gateway->pass();
    expect(watchedForWriting, "and the wait watches for room to send");
}

void testSleepsOnlyWithNothingInFlight() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    expect(rig.gateway->mayWait(), "with nothing connected there is nothing to do");
    Client client(rig.sockets);
    introduce(rig, client);
    expect(rig.gateway->mayWait(), "a connected, idle client is no reason to spin");

    client.send(commandFrame(1, "AAPL", limit(1, Side::BUY, 100)));
    rig.gateway->pass();
    expect(!rig.gateway->mayWait(), "a command in flight is");
    rig.match();
    rig.gateway->pass();
    expect(client.answered() == 1 && rig.gateway->mayWait(), "until it is answered");
}

struct BadFrame {
    const char* name;
    bool helloFirst;
    Bytes frame;
    ProtocolError code;
    Correlation correlation;  // what the error should echo
};

std::vector<BadFrame> badFrames() {
    const auto headerOnly = [](std::uint16_t length, std::uint8_t type, Correlation correlation) {
        Bytes frame(12);
        store16(frame, 0, length);
        store8(frame, 2, type);
        store64(frame, 4, correlation);
        return frame;
    };
    Bytes badMagic = helloFrame(21);
    badMagic[12] = std::byte{0};
    Bytes badVersion(64);
    badVersion.resize(encodeHello(22, static_cast<std::uint16_t>(wireVersion + 1), badVersion));
    Bytes unknownCommand = commandFrame(23, "AAPL", limit(50, Side::BUY, 100));
    unknownCommand[12] = std::byte{9};
    Bytes unexpectedField = commandFrame(24, "AAPL", limit(51, Side::BUY, 100));
    unexpectedField[3] = std::byte{2};  // a flag no frame from a client may set

    return {
        {"a length under a header", true, headerOnly(5, 3, 20), ProtocolError::BAD_LENGTH, 0},
        {"bad magic", false, badMagic, ProtocolError::BAD_MAGIC, 21},
        {"an unknown frame type", true, headerOnly(12, 99, 25), ProtocolError::UNKNOWN_FRAME_TYPE, 25},
        {"a command before Hello", false, commandFrame(26, "AAPL", limit(52, Side::BUY, 100)), ProtocolError::HELLO_REQUIRED, 26},
        {"a second Hello", true, helloFrame(27), ProtocolError::DUPLICATE_HELLO, 27},
        {"a version it can't speak", false, badVersion, ProtocolError::UNSUPPORTED_VERSION, 22},
        {"an unknown command", true, unknownCommand, ProtocolError::UNKNOWN_COMMAND, 23},
        {"a field that must be zero", true, unexpectedField, ProtocolError::UNEXPECTED_FIELD, 24},
    };
}

// Answers what came before it, says why once, closes, and never touches what came after.
void testEveryProtocolErrorIsAnsweredThenCloses() {
    for (const BadFrame& bad : badFrames()) {
        Rig rig({on("AAPL", 0)}, 1, 16);
        Client client(rig.sockets);
        Bytes stream;
        if (bad.helloFirst) {
            stream = joined({helloFrame(), commandFrame(9, "AAPL", limit(1, Side::BUY, 100))});
        }
        stream.insert(stream.end(), bad.frame.begin(), bad.frame.end());
        const Bytes after = commandFrame(10, "AAPL", limit(2, Side::BUY, 100));
        stream.insert(stream.end(), after.begin(), after.end());
        client.send(stream);
        rig.settle();

        const std::string name = bad.name;
        const std::vector<Received>& frames = client.frames();
        expect(!frames.empty() && frames.back().type == FrameType::PROTOCOL_ERROR, name + " ends with a ProtocolError");
        expect(frames.back().last && frames.back().correlation == bad.correlation, name + " echoes the offending frame's correlation");
        expect(frames.back().error->code == static_cast<std::uint16_t>(bad.code), name + " gets its own code");
        expect(frames.back().error->engineVersion == wireVersion, name + " says what the engine speaks");
        expect(std::count_if(frames.begin(), frames.end(), [](const Received& f) { return f.error.has_value(); }) == 1, name + " says so once");
        expect(client.answered() == (bad.helloFirst ? 1U : 0U), name + " answers the command before it, and not the one after");
        expect(rig.sockets.sendShutDown(client.socket) && rig.sockets.closed(client.socket), name + " closes");
    }
}

void testAProtocolErrorWaitsForWhatIsInFlight() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(joined({commandFrame(1, "AAPL", limit(1, Side::BUY, 100)), helloFrame(2)}));
    rig.gateway->pass();
    rig.gateway->pass();
    expect(client.frames().size() == 1 && !rig.sockets.closed(client.socket), "nothing is said while the command is out");
    const std::size_t reads = rig.sockets.reads(client.socket);
    rig.gateway->pass();
    expect(rig.sockets.reads(client.socket) == reads, "and it has stopped reading");

    rig.match();
    rig.gateway->pass();
    const std::vector<Received>& frames = client.frames();
    expect(frames.size() == 3 && frames[1].correlation == 1, "the answer comes first");
    expect(frames[2].type == FrameType::PROTOCOL_ERROR, "the error after it");
    expect(rig.sockets.closed(client.socket), "and then it closes");
}

void testAHalfCloseIsAnsweredInFull() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client client(rig.sockets);
    introduce(rig, client);
    Bytes stream;
    for (OrderId id = 1; id <= 5; ++id) {
        const Bytes frame = commandFrame(id, "AAPL", limit(id, Side::BUY, 100));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    client.send(stream);
    rig.sockets.clientHalfClose(client.socket);
    rig.gateway->pass();
    expect(!rig.sockets.closed(client.socket), "it isn't closed with commands still to run");

    rig.settle();
    expect(client.answered() == 5, "every command is answered, parked ones too");
    const std::vector<Received>& frames = client.frames();
    expect(frames.back().type != FrameType::PROTOCOL_ERROR, "with no error frame");
    expect(rig.sockets.sendShutDown(client.socket) && rig.sockets.closed(client.socket), "and it closes after that");
}

void testPastTheLimitAConnectionIsTurnedAway() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    std::vector<Client> clients;
    clients.reserve(connectionsPerThread + 1);
    for (std::uint32_t i = 0; i <= connectionsPerThread; ++i) {
        clients.emplace_back(rig.sockets);
    }
    rig.gateway->pass();

    Client& extra = clients.back();
    const std::vector<Received>& frames = extra.frames();
    expect(frames.size() == 1 && frames[0].type == FrameType::PROTOCOL_ERROR && frames[0].correlation == 0, "one past the limit hears why");
    expect(frames[0].error->code == static_cast<std::uint16_t>(ProtocolError::TOO_MANY_CONNECTIONS), "with the right code");
    expect(rig.sockets.closed(extra.socket), "and is closed");

    Client& lastInside = clients[connectionsPerThread - 1];
    introduce(rig, lastInside);
    expect(!rig.sockets.closed(lastInside.socket), "the one before it was taken on");
}

void testABackloggedConnectionIsNotRead() {
    GatewaySettings settings;
    settings.highWaterMark = 100;
    Rig rig({on("AAPL", 0)}, 1, 16, settings);
    Client client(rig.sockets);
    introduce(rig, client);

    rig.sockets.limitWrite(0);
    client.send(refusedStream(100));
    rig.gateway->pass();
    expect(client.answered() == 0, "the client takes nothing");
    const std::size_t reads = rig.sockets.reads(client.socket);
    rig.gateway->pass();
    rig.gateway->pass();
    expect(rig.sockets.reads(client.socket) == reads, "past the high-water mark it isn't read");
    expect(!rig.sockets.closed(client.socket), "and under the hard limit it just waits");

    rig.sockets.limitWrite(std::numeric_limits<std::size_t>::max());
    rig.settle();
    expect(client.answered() == 100, "once it drains, it is read again");
}

void testAtTheHighWaterMarkAConnectionIsStillRead() {
    GatewaySettings settings;
    settings.highWaterMark = 28;  // exactly one Refused frame
    Rig rig({on("AAPL", 0)}, 1, 16, settings);
    Client client(rig.sockets);
    introduce(rig, client);

    rig.sockets.limitWrite(0);
    client.send(refusedStream(1));
    rig.gateway->pass();
    const std::size_t reads = rig.sockets.reads(client.socket);
    rig.gateway->pass();
    expect(rig.sockets.reads(client.socket) == reads + 1, "exactly at the mark isn't past it");
}

void testPastTheHardLimitAConnectionCloses() {
    expect(GatewaySettings{}.highWaterMark == 1024 * 1024 && GatewaySettings{}.hardLimit == 16 * 1024 * 1024, "the defaults are 1 MiB and 16 MiB");

    GatewaySettings settings;
    settings.hardLimit = 1000;
    Rig rig({on("AAPL", 0)}, 1, 16, settings);
    Client client(rig.sockets);
    introduce(rig, client);
    rig.sockets.limitWrite(0);
    client.send(refusedStream(36));  // 1,008 bytes of Refused frames
    rig.gateway->pass();
    expect(rig.sockets.closed(client.socket), "it closes");
    expect(!rig.sockets.sendShutDown(client.socket), "without pretending it was answered");
}

void testAtTheHardLimitAConnectionStaysOpen() {
    GatewaySettings settings;
    settings.hardLimit = 1008;
    Rig rig({on("AAPL", 0)}, 1, 16, settings);
    Client client(rig.sockets);
    introduce(rig, client);
    rig.sockets.limitWrite(0);
    client.send(refusedStream(36));
    rig.gateway->pass();
    expect(!rig.sockets.closed(client.socket), "exactly at the limit isn't past it");
}

// Through a second connection, so what the first left behind is read from the books themselves.
void expectResting(Rig& rig, OrderId id, bool resting, const std::string& message) {
    Client looker(rig.sockets);
    introduce(rig, looker);
    looker.send(commandFrame(1000 + id, "AAPL", replay::CancelCommand{id}));
    rig.settle();
    const std::vector<Received>& frames = looker.frames();
    const bool cancelled = std::holds_alternative<OrderCancelled>(frames.back().event->event);
    expect(cancelled == resting, message);
}

void testAClosedConnectionsCommandsRunUnheardAndItsParkedOneIsDropped() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client client(rig.sockets);
    introduce(rig, client);

    rig.sockets.limitWrite(0);
    client.send(joined({refusedStream(1), commandFrame(11, "AAPL", limit(1, Side::BUY, 100)), commandFrame(12, "AAPL", limit(2, Side::BUY, 99)),
                        commandFrame(13, "AAPL", limit(3, Side::BUY, 98))}));
    rig.gateway->pass();
    rig.sockets.clientReset(client.socket);
    rig.gateway->pass();
    expect(rig.sockets.closed(client.socket), "a failed write closes a parked connection, which nobody is reading");
    expect(!rig.gateway->mayWait(), "its queued commands are still in flight");

    rig.sockets.limitWrite(std::numeric_limits<std::size_t>::max());
    rig.settle();
    expect(rig.gateway->mayWait(), "they ran, and nobody heard");
    expectResting(rig, 1, true, "a queued command still runs");
    expectResting(rig, 2, true, "and the next");
    expectResting(rig, 3, false, "a parked one is dropped");

    Client next(rig.sockets);
    introduce(rig, next);
    next.send(commandFrame(14, "AAPL", limit(4, Side::BUY, 97)));
    rig.gateway->pass();
    expect(rig.shards[0]->commands.hasItems(), "and with nobody really parked, a new command goes straight to the ring");
}

void testClosingAConnectionLeavesItsRestingOrders() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(commandFrame(1, "AAPL", limit(1, Side::BUY, 100)));
    rig.settle();
    rig.sockets.clientHalfClose(client.socket);
    rig.settle();
    expect(rig.sockets.closed(client.socket), "the connection is gone");
    expectResting(rig, 1, true, "and its order isn't");
}

void testAStopRefusesWhatItCanNoLongerQueue() {
    Rig rig({on("AAPL", 0)}, 1, 2);
    Client client(rig.sockets);
    introduce(rig, client);
    client.send(joined({commandFrame(11, "AAPL", limit(1, Side::BUY, 100)), commandFrame(12, "AAPL", limit(2, Side::BUY, 99)),
                        commandFrame(13, "AAPL", limit(3, Side::BUY, 98))}));
    rig.gateway->pass();  // 11 and 12 are queued, and 13 is parked

    rig.gateway->beginStop();
    expect(rig.sockets.listenerClosed(), "the listener closes first");
    client.send(commandFrame(14, "AAPL", limit(4, Side::BUY, 97)));
    Client late(rig.sockets);
    rig.settle();

    std::map<Correlation, std::vector<Received>> byCommand;
    for (const Received& frame : client.frames()) {
        byCommand[frame.correlation].push_back(frame);
    }
    expect(byCommand[11].size() == 1 && byCommand[11][0].event && byCommand[12].size() == 1 && byCommand[12][0].event,
           "what was queued gets its full answer");
    expect(byCommand[13].size() == 1 && refusedForStopping(byCommand[13][0]), "a parked command is refused with the retry code");
    expect(byCommand[13][0].refused->orderId == 3 && byCommand[13][0].refused->symbol == symbolOf("AAPL").bytes(), "naming it");
    expect(byCommand[14].size() == 1 && refusedForStopping(byCommand[14][0]), "and so is a command read after the stop");

    expect(rig.sockets.reads(late.socket) == 0, "a late connection is never taken on");
    expect(rig.sockets.closed(client.socket) && rig.sockets.sendShutDown(client.socket), "everything flushed, connections close");
    expect(rig.gateway->finished(), "and the gateway is done");
}

void testAStopStillAnswersWhatEntryAnswers() {
    Rig rig({on("AAPL", 0)}, 1, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    rig.gateway->beginStop();
    Bytes badSide = commandFrame(2, "AAPL", limit(2, Side::BUY, 100));
    badSide[13] = std::byte{0x7f};
    client.send(joined({commandFrame(1, "ZZZZ", limit(1, Side::BUY, 100)), badSide}));
    rig.gateway->pass();
    const std::vector<Received>& frames = client.frames();
    expect(frames.size() == 3, "both are answered");
    expect(frames[1].refused->code == static_cast<std::uint16_t>(RefusedCode::UNKNOWN_INSTRUMENT), "an unknown symbol stays unknown");
    expect(frames[2].event.has_value(), "and a malformed command stays malformed, since stopping comes last");
}

void testNothingIsClosedUntilEveryShardHasStopped() {
    Rig rig({on("AAPL", 0), on("MSFT", 1)}, 2, 16);
    Client client(rig.sockets);
    introduce(rig, client);
    rig.gateway->beginStop();
    expect(!rig.gateway->mayWait(), "with nothing in flight it still may not sleep, since the shards have yet to answer");
    rig.gateway->pass();
    rig.match(0);
    rig.gateway->pass();
    expect(!rig.gateway->finished() && !rig.sockets.closed(client.socket), "one shard stopping isn't enough");

    client.send(commandFrame(1, "AAPL", limit(1, Side::BUY, 100)));
    rig.gateway->pass();
    expect(refusedForStopping(client.frames().back()), "and meanwhile the gateway reads, to refuse");

    rig.match(1);
    rig.gateway->pass();
    expect(rig.gateway->finished() && rig.sockets.closed(client.socket), "both have, so it flushes and closes");
}

void testABackloggedConnectionStaysUnreadThroughAStop() {
    GatewaySettings settings;
    settings.highWaterMark = 100;
    Rig rig({on("AAPL", 0)}, 1, 16, settings);
    Client client(rig.sockets);
    introduce(rig, client);

    rig.sockets.limitWrite(0);
    client.send(refusedStream(100));
    rig.gateway->pass();
    const std::size_t reads = rig.sockets.reads(client.socket);
    rig.gateway->beginStop();
    rig.gateway->pass();
    rig.gateway->pass();
    expect(rig.sockets.reads(client.socket) == reads, "a stop doesn't start reading it");
}

void testTheStopGraceBoundsTheFlush() {
    expect(GatewaySettings{}.stopGrace == std::chrono::seconds(5), "the default grace is 5 s");

    Rig rig({on("AAPL", 0)}, 1, 16);
    Client stalled(rig.sockets);
    introduce(rig, stalled);
    rig.sockets.limitWrite(0);
    stalled.send(refusedStream(1));
    rig.gateway->pass();

    rig.gateway->beginStop();
    rig.settle();
    expect(!rig.sockets.closed(stalled.socket), "every shard has stopped, and it gets the grace to read");
    expect(!rig.gateway->finished(), "so the gateway isn't done");

    const std::size_t reads = rig.sockets.reads(stalled.socket);
    stalled.send(commandFrame(9, "AAPL", limit(9, Side::BUY, 100)));
    rig.gateway->pass();
    expect(rig.sockets.reads(stalled.socket) == reads, "the flush reads nothing more, so anything unread gets no answer");

    rig.now += std::chrono::seconds(5) - std::chrono::nanoseconds(1);
    rig.gateway->pass();
    expect(!rig.sockets.closed(stalled.socket), "a moment short of the grace it is still open");

    rig.now += std::chrono::nanoseconds(1);
    rig.gateway->pass();
    expect(rig.sockets.closed(stalled.socket) && rig.gateway->finished(), "and at the grace it closes anyway");
}

// A shard waits for a marker from every gateway thread, and tells each of them.
void testAShardStopsOnceEveryGatewayThreadHasSaidSo() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0);
    Doorbell doorbell;
    Ring<CommandElement> firstCommands(16);
    Ring<ResponseRecord> firstResponses(16);
    Ring<CommandElement> secondCommands(16);
    Ring<ResponseRecord> secondResponses(16);
    MatchingLoop loop(shard, doorbell, {RingPair{&firstCommands, &firstResponses}, RingPair{&secondCommands, &secondResponses}});
    InMemorySocketSet firstSockets;
    InMemorySocketSet secondSockets;
    GatewayThread first(directory, {ShardLink{&firstCommands, &firstResponses, &doorbell}}, firstSockets);
    GatewayThread second(directory, {ShardLink{&secondCommands, &secondResponses, &doorbell}}, secondSockets);

    first.beginStop();
    first.pass();
    loop.pass();
    expect(!loop.stopped(), "one gateway thread's marker isn't a stop");

    second.beginStop();
    second.pass();
    loop.pass();
    expect(loop.stopped(), "every gateway thread's is");
    loop.run();  // with every marker in, this only reports
    first.pass();
    second.pass();
    expect(first.finished() && second.finished(), "and both heard, on their own rings");
}

// 100 seeded schedules each way, through the harness's stepper, which engine_randomized_tests
// --stepper also runs, so a failing seed here reproduces there.
void testSteppedSchedules() {
    order_book::engine_testing::StepperResult totals;
    for (const bool stop : {false, true}) {
        for (std::uint64_t seed = 1; seed <= 100; ++seed) {
            order_book::engine_testing::StepperConfig config;
            config.seed = seed;
            config.stop = stop;
            const order_book::engine_testing::StepperResult result = order_book::engine_testing::runStepperSchedule(config);
            expect(result.passed(), (stop ? "stopped" : "unstopped") + std::string(" schedule, seed ") + std::to_string(seed) + ": " +
                                        result.failure.value_or("") + "\nreproduce with: engine_randomized_tests --stepper " +
                                        (stop ? "stop" : "run") + " --seed " + std::to_string(seed));
            totals.sleeps += result.sleeps;
            totals.waitsInPlace += result.waitsInPlace;
            totals.commandsRun += result.commandsRun;
            totals.commandsRefused += result.commandsRefused;
        }
    }
    expect(totals.sleeps > 0, "the gateway slept, or the check of each sleep checked nothing");
    expect(totals.waitsInPlace > 0, "matching threads did wait on a full response ring");
    expect(totals.commandsRun > 0 && totals.commandsRefused > 0, "stops landed mid-stream, or the stopped checks checked little");
}

}  // namespace

int main() {
    testHelloIsWelcomed();
    testCommandsNeedNotWaitForWelcome();
    testEachInstrumentReachesItsOwnBook();
    testACascadeComesWholeAndInOrder();
    testEntryAnswersWithoutTheBook();
    testOneWritePerConnectionPerPass();
    testAShortWriteKeepsTheRest();
    testAPassTakesAtMost64Frames();
    testFramesInTheReaderKeepItAwake();
    testACommandRingsItsShardsDoorbell();
    testAStreamingClientCantStopTheWaitForEver();
    testAConnectionsBudgetIsItsOwn();
    testAFullRingParksAndLosesNothing();
    testParkedConnectionsRetryOldestFirst();
    testANewCommandCantPassAParkedOne();
    testAParkedConnectionHoldsItsOtherShardToo();
    testAReusedIndexHearsNothingOfTheDead();
    testSleepsOnlyWithNothingInFlight();
    testTheSpinWindowComesBeforeSleep();
    testABackloggedClientLetsTheGatewaySleep();
    testEveryProtocolErrorIsAnsweredThenCloses();
    testAProtocolErrorWaitsForWhatIsInFlight();
    testAHalfCloseIsAnsweredInFull();
    testPastTheLimitAConnectionIsTurnedAway();
    testABackloggedConnectionIsNotRead();
    testAtTheHighWaterMarkAConnectionIsStillRead();
    testPastTheHardLimitAConnectionCloses();
    testAtTheHardLimitAConnectionStaysOpen();
    testAClosedConnectionsCommandsRunUnheardAndItsParkedOneIsDropped();
    testClosingAConnectionLeavesItsRestingOrders();
    testAStopRefusesWhatItCanNoLongerQueue();
    testAStopStillAnswersWhatEntryAnswers();
    testNothingIsClosedUntilEveryShardHasStopped();
    testABackloggedConnectionStaysUnreadThroughAStop();
    testTheStopGraceBoundsTheFlush();
    testAShardStopsOnceEveryGatewayThreadHasSaidSo();
    testSteppedSchedules();
    std::cout << "gateway tests passed" << std::endl;
    return EXIT_SUCCESS;
}
