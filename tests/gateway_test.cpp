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
using Bytes = std::vector<std::byte>;

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

struct ShardActor {
    ShardActor(const InstrumentDirectory& directory, std::uint32_t number, std::size_t ringSlots)
        : shard(directory, number), commands(ringSlots), responses(ringSlots) {}

    Shard shard;
    Doorbell doorbell;
    Ring<CommandElement> commands;
    Ring<ResponseRecord> responses;
    std::optional<MatchingLoop> loop;
    bool reportedStop = false;
};

// One gateway thread and every shard, all stepped here. A matching thread that finds its response
// ring full waits in place, and the only thing that can make room is the gateway, so that wait
// runs one gateway pass.
class Rig {
public:
    Rig(std::vector<Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots, GatewaySettings settings = {})
        : directory(std::move(instruments), shardCount) {
        settings.clock = [this] { return now; };
        std::vector<ShardLink> links;
        for (std::uint32_t number = 0; number < shardCount; ++number) {
            shards.push_back(std::make_unique<ShardActor>(directory, number, ringSlots));
            ShardActor& actor = *shards.back();
            links.push_back(ShardLink{&actor.commands, &actor.responses, &actor.doorbell});
            actor.loop.emplace(actor.shard, actor.doorbell, std::vector<RingPair>{RingPair{&actor.commands, &actor.responses}},
                               std::chrono::nanoseconds::zero(), [this] {
                                   ++waitsInPlace;
                                   gateway->pass();
                               });
        }
        gateway.emplace(directory, std::move(links), sockets, std::move(settings));
    }

    // One pass, and a shard with every producer's marker reports that it has stopped, which is
    // all run does then.
    std::size_t match(std::uint32_t shard = 0) {
        ShardActor& actor = *shards[shard];
        const std::size_t taken = actor.loop->pass();
        if (actor.loop->stopped() && !actor.reportedStop) {
            actor.reportedStop = true;
            actor.loop->run();
        }
        return taken;
    }

    // Every actor in turn, far more times than any test here needs.
    void settle() {
        for (int round = 0; round < 512; ++round) {
            gateway->pass();
            for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
                match(shard);
            }
        }
    }

    InstrumentDirectory directory;
    InMemorySocketSet sockets;
    std::vector<std::unique_ptr<ShardActor>> shards;
    std::optional<GatewayThread> gateway;
    std::size_t waitsInPlace = 0;
    std::chrono::steady_clock::time_point now;
};

struct Received {
    FrameType type;
    bool last;
    Correlation correlation;
    std::uint16_t version = 0;
    std::optional<EventFrame> event;
    std::optional<RefusedFrame> refused;
    std::optional<ProtocolErrorFrame> error;
};

class Client {
public:
    explicit Client(InMemorySocketSet& socketSet) : sockets(socketSet), socket(socketSet.connect()), reader(FrameSource::ENGINE) {}

    void send(std::span<const std::byte> bytes) { sockets.clientSend(socket, bytes); }

    // Everything received so far, in order.
    const std::vector<Received>& frames() {
        const Bytes bytes = sockets.clientReceive(socket);
        reader.feed(bytes);
        for (;;) {
            const FrameResult result = reader.next();
            const Frame* frame = std::get_if<Frame>(&result);
            if (frame == nullptr) {
                expect(std::holds_alternative<NeedMore>(result), "the engine sent a frame a client can't read");
                return received;
            }
            Received item{frame->type, frame->last, frame->correlation, 0, std::nullopt, std::nullopt, std::nullopt};
            if (frame->type == FrameType::WELCOME) {
                item.version = decodeVersion(*frame);
            } else if (frame->type == FrameType::REFUSED) {
                item.refused = decodeRefused(*frame);
            } else if (frame->type == FrameType::PROTOCOL_ERROR) {
                item.error = decodeProtocolError(*frame);
            } else {
                item.event = decodeEvent(*frame);
                expect(item.event.has_value(), "an event frame can be decoded");
            }
            received.push_back(item);
        }
    }

    std::size_t answered() {
        std::size_t count = 0;
        for (const Received& frame : frames()) {
            if (frame.last && frame.type != FrameType::WELCOME && frame.type != FrameType::PROTOCOL_ERROR) {
                ++count;
            }
        }
        return count;
    }

    InMemorySocketSet& sockets;
    SocketId socket;

private:
    FrameReader reader;
    std::vector<Received> received;
};

Bytes helloFrame(Correlation correlation = 1) {
    Bytes frame(64);
    frame.resize(encodeHello(correlation, wireVersion, frame));
    return frame;
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

bool refusedForStopping(const Received& frame) {
    return frame.refused && frame.refused->code == static_cast<std::uint16_t>(RefusedCode::ENGINE_STOPPING);
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
    rig.gateway->pass();
    expect(rig.match() == 36, "and the next pass the rest, which were already read");
    expect(rig.sockets.waits() == waits, "with no sleep while frames sat in the buffer");
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

void testSleepsOnlyWithNothingOutstanding() {
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

// The seeded stepper: one thread plays each actor, choosing the next from a seeded source, with
// rings of capacity 2, reads and writes split anywhere. One connection owns each instrument, so
// its book's order is one stream's order and the answers must equal a single-threaded run. What
// the entry check answers itself may overtake what is queued, so those match by correlation.
struct Line {
    Correlation correlation;
    BookEvent event;
    bool last;

    bool operator==(const Line&) const = default;
};

struct StreamCommand {
    Correlation correlation;
    std::size_t connection;
    EntryAccepted accepted;
};

struct SteppedStream {
    std::vector<Instrument> instruments;
    std::vector<Bytes> bytes;                            // per connection
    std::vector<std::size_t> frames;                     // the frames expected back, per connection
    std::map<std::string, std::vector<Line>> expected;   // from the books, per instrument
    std::map<Correlation, BookEvent> rejectedAtEntry;
    std::set<Correlation> refused;
    std::vector<StreamCommand> commands;                 // those that passed the entry check
};

constexpr std::uint32_t steppedShards = 2;
constexpr std::size_t steppedConnections = 3;
constexpr std::size_t steppedInstruments = 6;

Bytes rawCommandFrame(Correlation correlation, const replay::RawCommand& raw) {
    Bytes frame(12 + replay::commandBodyBytes);
    const std::span<std::byte> out(frame);
    store16(out, 0, static_cast<std::uint16_t>(frame.size()));
    store8(out, 2, static_cast<std::uint8_t>(FrameType::COMMAND));
    store64(out, 4, correlation);
    const std::span<std::byte> body = out.subspan(12);
    store8(body, 0, raw.command);
    store8(body, 1, raw.side);
    store8(body, 2, raw.orderType);
    store8(body, 3, raw.timeInForce);
    std::memcpy(body.data() + 4, raw.symbol.data(), raw.symbol.size());
    store32(body, 12, raw.orderId);
    store32(body, 16, raw.price);
    store32(body, 20, raw.quantity);
    store32(body, 24, raw.stopPrice);
    store32(body, 28, raw.owner);
    store8(body, 32, raw.flags);
    store8(body, 33, raw.selfTradePrevention);
    store16(body, 34, raw.reserved);
    expect(replay::unpackCommandBody(std::span<const std::byte, replay::commandBodyBytes>(body)) == raw, "the test's frame is the command");
    return frame;
}

SteppedStream steppedStream(std::uint64_t seed, std::size_t commands) {
    engine_testing::EngineSeedConfig config;
    config.seed = seed;
    config.instruments = steppedInstruments;
    config.shards = steppedShards;
    config.commands = commands;

    SteppedStream stream;
    std::vector<Symbol> symbols;
    for (std::size_t i = 0; i < steppedInstruments; ++i) {
        symbols.push_back(symbolOf(("SYM" + std::to_string(i)).c_str()));
        stream.instruments.push_back(Instrument{symbols.back(), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }
    const InstrumentDirectory directory(stream.instruments, steppedShards);
    std::vector<std::unique_ptr<Shard>> witnesses;
    for (std::uint32_t shard = 0; shard < steppedShards; ++shard) {
        witnesses.push_back(std::make_unique<Shard>(directory, shard));
    }

    stream.bytes.resize(steppedConnections);
    stream.frames.assign(steppedConnections, 1);  // each gets a Welcome
    for (Bytes& bytes : stream.bytes) {
        bytes = helloFrame();
    }

    const std::vector<engine_testing::EngineCommand> lines = engine_testing::generateEngineStream(config);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const engine_testing::EngineCommand& line = lines[index];
        if (!line.converted) {
            continue;
        }
        const Correlation correlation = index + 1;
        const std::array<char, Symbol::size> stored =
            line.fault == engine_testing::SymbolFault::NONE ? symbols[line.instrument].bytes() : symbolOf("ZZZZ").bytes();
        const replay::RawCommand raw = engine_testing::rawCommandOf(*line.converted, stored);
        const std::size_t connection = line.instrument % steppedConnections;
        const Bytes frame = rawCommandFrame(correlation, raw);
        stream.bytes[connection].insert(stream.bytes[connection].end(), frame.begin(), frame.end());

        const std::string name = symbols[line.instrument].text();
        const EntryDecision decision = checkEntry(CommandFrame{correlation, raw}, directory, false);
        if (std::holds_alternative<EntryRefused>(decision)) {
            stream.refused.insert(correlation);
            ++stream.frames[connection];
        } else if (const EntryMalformed* malformed = std::get_if<EntryMalformed>(&decision)) {
            stream.rejectedAtEntry.insert({correlation, malformed->event});
            ++stream.frames[connection];
        } else {
            const EntryAccepted& accepted = std::get<EntryAccepted>(decision);
            stream.commands.push_back(StreamCommand{correlation, connection, accepted});
            Shard& witness = *witnesses[accepted.location.shard];
            witness.process(accepted.command, accepted.location.slot);
            const std::span<const BookEvent> events = witness.result().events();
            for (std::size_t i = 0; i < events.size(); ++i) {
                stream.expected[name].push_back(Line{correlation, events[i], i + 1 == events.size()});
                ++stream.frames[connection];
            }
        }
    }
    return stream;
}

struct SteppedTotals {
    std::size_t sleeps = 0;
    std::size_t waitsInPlace = 0;
    std::size_t commandsRun = 0;
    std::size_t commandsRefused = 0;
};

// Sleeping is only safe with every command read so far answered, counted here from the bytes the
// gateway took and the frames the client holds, and from nothing the gateway says about itself.
void checkNothingOutstanding(Rig& rig, std::vector<Client>& clients) {
    for (const std::unique_ptr<ShardActor>& shard : rig.shards) {
        expect(!shard->commands.hasItems() && !shard->responses.hasItems(), "a gateway slept with a ring holding work");
    }
    for (Client& client : clients) {
        const std::size_t taken = rig.sockets.bytesRead(client.socket);
        const std::size_t commandsRead = taken < helloFrame().size() ? 0 : (taken - helloFrame().size()) / 48;
        expect(client.answered() == commandsRead, "a gateway slept with a command in flight");
    }
}

void runSchedule(const SteppedStream& stream, std::uint64_t seed, SteppedTotals& totals) {
    Rig rig(stream.instruments, steppedShards, 2);
    std::mt19937_64 random(seed);
    std::vector<Client> clients;
    clients.reserve(steppedConnections);
    for (std::size_t i = 0; i < steppedConnections; ++i) {
        clients.emplace_back(rig.sockets);
    }
    rig.sockets.onWait([&] {
        checkNothingOutstanding(rig, clients);
        ++totals.sleeps;
    });

    std::vector<std::size_t> sent(steppedConnections, 0);
    const auto finished = [&] {
        for (std::size_t i = 0; i < steppedConnections; ++i) {
            if (clients[i].frames().size() != stream.frames[i]) {
                return false;
            }
        }
        return true;
    };

    std::size_t steps = 0;
    while (!finished()) {
        expect(++steps < 40'000'000, "the stepped run stopped making progress");
        for (int actor = 0; actor < 64; ++actor) {
            const std::uint64_t choice = random();
            switch (choice % 3) {
                case 0: {
                    const std::size_t connection = (choice >> 8) % steppedConnections;
                    const Bytes& bytes = stream.bytes[connection];
                    const std::size_t left = bytes.size() - sent[connection];
                    const std::size_t chunk = std::min<std::size_t>(left, 1 + (choice >> 16) % 150);
                    clients[connection].send(std::span<const std::byte>(bytes).subspan(sent[connection], chunk));
                    sent[connection] += chunk;
                    break;
                }
                case 1:
                    rig.sockets.limitRead(1 + (choice >> 8) % 200);
                    rig.sockets.limitWrite(1 + (choice >> 16) % 200);
                    rig.gateway->pass();
                    break;
                default:
                    rig.match(static_cast<std::uint32_t>((choice >> 8) % steppedShards));
                    break;
            }
        }
    }

    std::map<std::string, std::vector<Line>> answered;
    std::map<Correlation, BookEvent> rejected;
    std::set<Correlation> refused;
    for (Client& client : clients) {
        for (const Received& frame : client.frames()) {
            if (frame.type == FrameType::REFUSED) {
                refused.insert(frame.correlation);
            } else if (frame.event && stream.rejectedAtEntry.contains(frame.correlation)) {
                expect(frame.last, "an entry rejection is one whole answer");
                rejected.insert({frame.correlation, frame.event->event});
            } else if (frame.event) {
                answered[frame.event->symbol.text()].push_back(Line{frame.correlation, frame.event->event, frame.last});
            }
        }
    }
    expect(answered == stream.expected, "every instrument's answers equal the single-threaded run's, seed " + std::to_string(seed));
    expect(rejected == stream.rejectedAtEntry, "and so do the rejections at entry, seed " + std::to_string(seed));
    expect(refused == stream.refused, "and the refusals, seed " + std::to_string(seed));
    totals.waitsInPlace += rig.waitsInPlace;
}

// A stop lands at a chosen step. What it must leave: every command is answered in full, refused
// with the retry code, or was never read, and for one connection they fall in that order, so what
// ran is what a single-threaded run of those same commands does.
void runStoppedSchedule(const SteppedStream& stream, std::uint64_t seed, SteppedTotals& totals) {
    Rig rig(stream.instruments, steppedShards, 2);
    std::mt19937_64 random(seed);
    std::vector<Client> clients;
    clients.reserve(steppedConnections);
    for (std::size_t i = 0; i < steppedConnections; ++i) {
        clients.emplace_back(rig.sockets);
    }

    std::vector<std::size_t> sent(steppedConnections, 0);
    const std::uint64_t stopAt = random() % 3000;
    std::size_t steps = 0;
    while (!rig.gateway->finished()) {
        expect(++steps < 40'000'000, "the stopped run stopped making progress");
        if (steps == stopAt) {
            rig.gateway->beginStop();
        }
        const std::uint64_t choice = random();
        if (choice % 3 == 0) {
            const std::size_t connection = (choice >> 8) % steppedConnections;
            const Bytes& bytes = stream.bytes[connection];
            const std::size_t chunk = std::min<std::size_t>(bytes.size() - sent[connection], 1 + (choice >> 16) % 150);
            clients[connection].send(std::span<const std::byte>(bytes).subspan(sent[connection], chunk));
            sent[connection] += chunk;
        } else if (choice % 3 == 1) {
            rig.sockets.limitRead(1 + (choice >> 8) % 200);
            rig.sockets.limitWrite(1 + (choice >> 16) % 200);
            rig.gateway->pass();
        } else {
            rig.match(static_cast<std::uint32_t>((choice >> 8) % steppedShards));
        }
    }

    std::map<std::string, std::vector<Line>> answered;
    std::set<Correlation> ran;
    std::set<Correlation> refusedByStop;
    for (Client& client : clients) {
        for (const Received& frame : client.frames()) {
            if (refusedForStopping(frame)) {
                refusedByStop.insert(frame.correlation);
            } else if (frame.event && !stream.rejectedAtEntry.contains(frame.correlation)) {
                ran.insert(frame.correlation);
                answered[frame.event->symbol.text()].push_back(Line{frame.correlation, frame.event->event, frame.last});
            }
        }
        expect(rig.sockets.closed(client.socket), "a finished gateway has closed every connection");
    }

    const InstrumentDirectory directory(stream.instruments, steppedShards);
    std::vector<std::unique_ptr<Shard>> witnesses;
    for (std::uint32_t shard = 0; shard < steppedShards; ++shard) {
        witnesses.push_back(std::make_unique<Shard>(directory, shard));
    }
    std::map<std::string, std::vector<Line>> expected;
    std::vector<bool> connectionCut(steppedConnections, false);
    for (const StreamCommand& command : stream.commands) {
        const bool didRun = ran.contains(command.correlation);
        expect(!(didRun && refusedByStop.contains(command.correlation)), "no command both ran and was refused");
        expect(!(didRun && connectionCut[command.connection]), "nothing ran after a connection's first command that didn't");
        if (!didRun) {
            connectionCut[command.connection] = true;
            continue;
        }
        Shard& witness = *witnesses[command.accepted.location.shard];
        witness.process(command.accepted.command, command.accepted.location.slot);
        const std::span<const BookEvent> events = witness.result().events();
        for (std::size_t i = 0; i < events.size(); ++i) {
            expected[command.accepted.symbol.text()].push_back(Line{command.correlation, events[i], i + 1 == events.size()});
        }
    }
    expect(answered == expected, "what ran equals a single-threaded run of those commands, seed " + std::to_string(seed));
    totals.commandsRun += ran.size();
    totals.commandsRefused += refusedByStop.size();
}

void testSteppedSchedules() {
    SteppedTotals totals;
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        runSchedule(steppedStream(seed, 1000), seed, totals);
    }
    expect(totals.sleeps > 0, "the gateway slept, or the check above checked nothing");
    expect(totals.waitsInPlace > 0, "matching threads did wait on a full response ring");
}

void testSteppedStops() {
    SteppedTotals totals;
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        runStoppedSchedule(steppedStream(seed, 1000), seed, totals);
    }
    expect(totals.commandsRun > 0 && totals.commandsRefused > 0, "stops landed mid-stream, or the check above checked little");
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
    testAConnectionsBudgetIsItsOwn();
    testAFullRingParksAndLosesNothing();
    testParkedConnectionsRetryOldestFirst();
    testANewCommandCantPassAParkedOne();
    testAParkedConnectionHoldsItsOtherShardToo();
    testAReusedIndexHearsNothingOfTheDead();
    testSleepsOnlyWithNothingOutstanding();
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
    testSteppedStops();
    std::cout << "gateway tests passed" << std::endl;
    return EXIT_SUCCESS;
}
