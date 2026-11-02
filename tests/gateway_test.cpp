#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
};

// One gateway thread and every shard, all stepped here. A matching thread that finds its response
// ring full waits in place, and the only thing that can make room is the gateway, so that wait
// runs one gateway pass.
class Rig {
public:
    Rig(std::vector<Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots)
        : directory(std::move(instruments), shardCount) {
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
        gateway.emplace(directory, std::move(links), sockets);
    }

    std::size_t match(std::uint32_t shard = 0) { return shards[shard]->loop->pass(); }

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
};

struct Received {
    FrameType type;
    bool last;
    Correlation correlation;
    std::uint16_t version = 0;
    std::optional<EventFrame> event;
    std::optional<RefusedFrame> refused;
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
            Received item{frame->type, frame->last, frame->correlation, 0, std::nullopt, std::nullopt};
            if (frame->type == FrameType::WELCOME) {
                item.version = decodeVersion(*frame);
            } else if (frame->type == FrameType::REFUSED) {
                item.refused = decodeRefused(*frame);
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
            if (frame.last && frame.type != FrameType::WELCOME) {
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

struct SteppedStream {
    std::vector<Instrument> instruments;
    std::vector<Bytes> bytes;                            // per connection
    std::vector<std::size_t> frames;                     // the frames expected back, per connection
    std::map<std::string, std::vector<Line>> expected;   // from the books, per instrument
    std::map<Correlation, BookEvent> rejectedAtEntry;
    std::set<Correlation> refused;
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

void testSteppedSchedules() {
    SteppedTotals totals;
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        runSchedule(steppedStream(seed, 1000), seed, totals);
    }
    expect(totals.sleeps > 0, "the gateway slept, or the check above checked nothing");
    expect(totals.waitsInPlace > 0, "matching threads did wait on a full response ring");
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
    testSteppedSchedules();
    std::cout << "gateway tests passed" << std::endl;
    return EXIT_SUCCESS;
}
