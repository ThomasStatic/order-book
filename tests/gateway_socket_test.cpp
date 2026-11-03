#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "engine/gateway_frames.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/poll_socket_set.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

// The gateway on real loopback sockets and real threads, so this carries the threaded label and
// runs under ThreadSanitizer and on Arm. See docs/internals/network.md.
namespace {

using namespace order_book;
using namespace order_book::network;
using namespace order_book::runtime;
using order_book::gateway_testing::Bytes;
using order_book::gateway_testing::helloFrame;
using order_book::gateway_testing::rawCommandFrame;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbolOf(const char* text) {
    return *Symbol::parse(text);
}

std::vector<Instrument> instruments() {
    return {Instrument{symbolOf("AAPL"), 0, std::nullopt, std::nullopt, std::nullopt},
            Instrument{symbolOf("MSFT"), 0, std::nullopt, std::nullopt, std::nullopt}};
}

// One shard's matching thread and one gateway thread, each building what it owns on itself. The
// gateway has no spin window, so it is asleep almost as soon as nothing is in flight, and a lost
// wake for a connection or a stop hangs a test here; removing the wake's byte does.
class LiveEngine {
public:
    LiveEngine() : directory(instruments(), 1), commands(defaultCommandRingSlots), responses(defaultResponseRingSlots) {
        std::promise<void> ready;
        std::future<void> started = ready.get_future();
        matching = std::thread([this, &ready] {
            Shard shard(directory, 0);
            MatchingLoop loop(shard, doorbell, {RingPair{&commands, &responses}});
            ready.set_value();
            loop.run();
        });
        started.get();
        gateway = std::thread([this] {
            GatewaySettings settings;
            settings.spinWindow = std::chrono::nanoseconds::zero();  // so it sleeps the moment it may
            GatewayThread gatewayThread(directory, {ShardLink{&commands, &responses, &doorbell, nullptr}}, sockets, settings);
            runGateway(gatewayThread, stopRequested);
        });
    }

    ~LiveEngine() { stop(); }

    std::uint16_t port() const noexcept { return sockets.port(); }

    void stop() {
        if (gateway.joinable()) {
            stopRequested.store(true, std::memory_order_seq_cst);
            sockets.wake();
            gateway.join();
            matching.join();
        }
    }

private:
    InstrumentDirectory directory;
    Doorbell doorbell;
    Ring<CommandElement> commands;
    Ring<ResponseRecord> responses;
    PollSocketSet sockets;
    std::atomic<bool> stopRequested{false};
    std::thread matching;
    std::thread gateway;
};

struct Received {
    FrameType type;
    bool last;
    Correlation correlation;
    std::optional<EventFrame> event;
    std::optional<ProtocolErrorFrame> error;
};

// A blocking client: sends everything, half-closes if asked, and reads to the end of the stream.
struct Conversation {
    std::vector<Received> frames;
    ReadStatus ending;
};

Conversation talk(std::uint16_t port, std::span<const std::byte> bytes, bool halfClose) {
    const TcpSocket socket = TcpSocket::connectLoopback(port);
    expect(socket.noDelay(), "a client socket has TCP_NODELAY on");
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::optional<std::size_t> taken = socket.send(bytes.subspan(sent));
        expect(taken.has_value(), "the engine didn't drop the connection while it was being sent to");
        sent += *taken;
    }
    if (halfClose) {
        socket.shutdownSend();
    }

    Conversation conversation{{}, ReadStatus::OPEN};
    FrameReader reader(FrameSource::ENGINE);
    std::array<std::byte, 16 * 1024> buffer;
    for (;;) {
        const ReadResult read = socket.receive(buffer);
        if (read.status != ReadStatus::OPEN) {
            conversation.ending = read.status;
            return conversation;
        }
        reader.feed(std::span<const std::byte>(buffer).first(read.bytes));
        for (FrameResult result = reader.next(); std::holds_alternative<Frame>(result); result = reader.next()) {
            const Frame& frame = std::get<Frame>(result);
            Received item{frame.type, frame.last, frame.correlation, std::nullopt, std::nullopt};
            if (frame.type == FrameType::PROTOCOL_ERROR) {
                item.error = decodeProtocolError(frame);
            } else if (frame.type != FrameType::WELCOME && frame.type != FrameType::REFUSED) {
                item.event = decodeEvent(frame);
            }
            conversation.frames.push_back(item);
        }
    }
}

replay::RawCommand rawAdd(const char* symbol, OrderId id, std::uint8_t side, Price price, Quantity quantity) {
    replay::RawCommand raw{};
    raw.command = replay::rawCommandAdd;
    raw.side = side;
    raw.orderType = 1;    // limit
    raw.timeInForce = 1;  // good till cancelled
    raw.symbol = symbolOf(symbol).bytes();
    raw.orderId = id;
    raw.price = price;
    raw.quantity = quantity;
    return raw;
}

void testLoopbackSocketsWork() {
    PollSocketSet sockets;
    expect(sockets.port() != 0, "port 0 lets the system pick one");
    const TcpSocket client = TcpSocket::connectLoopback(sockets.port());

    std::optional<SocketId> accepted;
    while (!accepted) {
        sockets.wait({}, WaitMode::SLEEP);  // the listener wakes it
        accepted = sockets.accept();
    }
    expect(sockets.noDelay(*accepted), "an accepted connection has TCP_NODELAY on");

    const std::array<std::byte, 3> hello{std::byte{1}, std::byte{2}, std::byte{3}};
    expect(client.send(hello) == hello.size(), "the client sends");
    std::array<std::byte, 16> into{};
    std::size_t got = 0;
    while (got == 0) {
        const std::array<SocketInterest, 1> interest{SocketInterest{*accepted, true, false}};
        sockets.wait(interest, WaitMode::SLEEP);
        got = sockets.read(*accepted, into).bytes;
    }
    expect(got == 3 && into[2] == std::byte{3}, "and the engine side reads it");

    expect(sockets.write(*accepted, std::span<const std::byte>(hello).first(2)) == std::optional<std::size_t>(2), "the engine writes");
    expect(client.receive(into).bytes == 2, "and the client reads it");

    client.shutdownSend();
    ReadResult ended{0, ReadStatus::OPEN};
    while (ended.status == ReadStatus::OPEN) {
        const std::array<SocketInterest, 1> interest{SocketInterest{*accepted, true, false}};
        sockets.wait(interest, WaitMode::SLEEP);
        ended = sockets.read(*accepted, into);
    }
    expect(ended.status == ReadStatus::ENDED, "a half-close reads as ended");
    sockets.shutdownSend(*accepted);
    sockets.close(*accepted);
    expect(client.receive(into).status == ReadStatus::ENDED, "and the client reads to the end of its stream");
}

// A connection released by the listening set is the same connection in a set with no listener,
// which is woken from another thread to take it, as gateway thread 0 hands one on.
void testAConnectionMovesBetweenSets() {
    PollSocketSet listening;
    PollSocketSet other{PollSocketSet::NoListener{}};
    const TcpSocket client = TcpSocket::connectLoopback(listening.port());
    std::optional<SocketId> accepted;
    while (!accepted) {
        listening.wait({}, WaitMode::SLEEP);
        accepted = listening.accept();
    }
    expect(!other.accept().has_value(), "a set with no listener accepts nothing");

    std::atomic<HandedSocket> handed{0};
    std::thread dealer([&] {
        handed.store(listening.release(*accepted), std::memory_order_seq_cst);
        other.wake();
    });
    other.wait({}, WaitMode::SLEEP);  // hangs if the wake is lost, and the CTest timeout fails it
    dealer.join();
    const SocketId adopted = other.adopt(handed.load(std::memory_order_seq_cst));
    expect(other.noDelay(adopted), "it keeps its TCP_NODELAY");

    const std::array<std::byte, 2> bytes{std::byte{7}, std::byte{8}};
    expect(client.send(bytes) == bytes.size(), "the client sends");
    std::array<std::byte, 16> into{};
    std::size_t got = 0;
    while (got == 0) {
        const std::array<SocketInterest, 1> interest{SocketInterest{adopted, true, false}};
        other.wait(interest, WaitMode::SLEEP);
        got = other.read(adopted, into).bytes;
    }
    expect(got == 2 && into[1] == std::byte{8}, "and the adopting set reads it");
    expect(other.write(adopted, std::span<const std::byte>(bytes).first(1)) == std::optional<std::size_t>(1), "and writes");
    expect(client.receive(into).bytes == 1, "to the same client");
    other.close(adopted);
    expect(client.receive(into).status != ReadStatus::OPEN, "and closing it there closes the client's connection");
}

void testAWakeEndsASleep() {
    PollSocketSet sockets;
    for (int round = 0; round < 3; ++round) {
        std::thread waker([&sockets] { sockets.wake(); });
        sockets.wait({}, WaitMode::SLEEP);  // hangs if the wake is lost, and the CTest timeout fails it
        waker.join();
    }
}

// Two clients, one instrument each, at once, against what the same commands do on one thread.
void testTheEngineOverLoopback() {
    const std::array<const char*, 2> symbols{"AAPL", "MSFT"};
    std::array<Bytes, 2> streams;
    std::array<std::map<Correlation, std::vector<BookEvent>>, 2> expected;
    const InstrumentDirectory directory(instruments(), 1);
    Shard witness(directory, 0);
    for (std::size_t client = 0; client < 2; ++client) {
        streams[client] = helloFrame(1);
        for (OrderId id = 1; id <= 200; ++id) {
            // Buys and sells about one price, so plenty trade, with a malformed one now and then.
            const std::uint8_t side = id % 2 == 0 ? 1 : 2;
            const Price price = 100 + id % 7;
            replay::RawCommand raw = rawAdd(symbols[client], id, side, price, 1 + id % 5);
            if (id % 50 == 0) {
                raw.side = 0x7f;
            }
            const Correlation correlation = 1000 + id;
            const Bytes frame = rawCommandFrame(correlation, raw);
            expect(order_book::gateway_testing::roundTrips(frame, raw), "the test's frame is the command");
            streams[client].insert(streams[client].end(), frame.begin(), frame.end());

            const EntryDecision decision = checkEntry(CommandFrame{correlation, raw}, directory, false);
            if (const EntryMalformed* malformed = std::get_if<EntryMalformed>(&decision)) {
                expected[client][correlation].push_back(malformed->event);
            } else {
                const EntryAccepted& accepted = std::get<EntryAccepted>(decision);
                witness.process(accepted.command, accepted.location.slot);
                const std::span<const BookEvent> events = witness.result().events();
                expected[client][correlation].assign(events.begin(), events.end());
            }
        }
    }

    LiveEngine engine;
    std::array<std::optional<Conversation>, 2> conversations;
    std::thread second([&] { conversations[1] = talk(engine.port(), streams[1], true); });
    conversations[0] = talk(engine.port(), streams[0], true);
    second.join();

    for (std::size_t client = 0; client < 2; ++client) {
        const Conversation& conversation = *conversations[client];
        expect(conversation.ending == ReadStatus::ENDED, "a half-closed client reads to a clean end");
        expect(!conversation.frames.empty() && conversation.frames.front().type == FrameType::WELCOME, "it was welcomed");
        std::map<Correlation, std::vector<BookEvent>> answered;
        for (const Received& frame : conversation.frames) {
            if (frame.event) {
                expect(frame.event->symbol == symbolOf(symbols[client]), "only the sender hears");
                answered[frame.correlation].push_back(frame.event->event);
            }
        }
        expect(answered == expected[client], "every answer is the book's own, in order, for client " + std::to_string(client));
    }
}

void testPastTheLimitOverLoopback() {
    LiveEngine engine;
    std::vector<TcpSocket> inside;
    for (std::uint32_t i = 0; i < connectionsPerThread; ++i) {
        inside.push_back(TcpSocket::connectLoopback(engine.port()));
        const Bytes hello = helloFrame(1);
        expect(inside.back().send(hello) == hello.size(), "each sends its Hello");
        std::array<std::byte, 64> welcome{};
        expect(inside.back().receive(welcome).bytes == 24, "and is welcomed, so it was taken on");
    }
    const Conversation turnedAway = talk(engine.port(), {}, false);
    expect(turnedAway.frames.size() == 1 && turnedAway.frames[0].error.has_value(), "one more hears a protocol error");
    expect(turnedAway.frames[0].error->code == static_cast<std::uint16_t>(ProtocolError::TOO_MANY_CONNECTIONS), "saying why");
    expect(turnedAway.ending == ReadStatus::ENDED, "and is closed cleanly");
}

// A client like send's: it writes a whole file, half-closes, and reads to the end, here with a bad
// frame near the start and far more after it than one read takes. The engine stops reading at the
// bad frame, and closing with the rest unread would reset the connection, which could cost the
// client the frame saying why. So the socket lingers, discarding, until the client is done.
void testAProtocolErrorReachesAClientStillSending() {
    LiveEngine engine;
    Bytes stream = helloFrame(1);
    Bytes unknownType(12);
    little_endian::store16(unknownType, 0, 12);
    little_endian::store8(unknownType, 2, 99);
    little_endian::store64(unknownType, 4, 77);
    stream.insert(stream.end(), unknownType.begin(), unknownType.end());
    stream.resize(stream.size() + 1024 * 1024, std::byte{0x5a});

    const TcpSocket socket = TcpSocket::connectLoopback(engine.port());
    std::thread sender([&socket, &stream] {
        std::span<const std::byte> left(stream);
        while (!left.empty()) {
            const std::optional<std::size_t> taken = socket.send(left);
            if (!taken) {
                return;  // the test below says what went wrong
            }
            left = left.subspan(*taken);
        }
        socket.shutdownSend();
    });
    FrameReader reader(FrameSource::ENGINE);
    std::vector<std::byte> kept;
    std::array<std::byte, 16 * 1024> buffer;
    ReadResult read{0, ReadStatus::OPEN};
    while ((read = socket.receive(buffer)).status == ReadStatus::OPEN) {
        kept.insert(kept.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read.bytes));
    }
    sender.join();
    reader.feed(kept);
    std::vector<Correlation> errors;
    for (FrameResult result = reader.next(); std::holds_alternative<Frame>(result); result = reader.next()) {
        const Frame& frame = std::get<Frame>(result);
        if (frame.type == FrameType::PROTOCOL_ERROR) {
            errors.push_back(frame.correlation);
        }
    }
    expect(errors.size() == 1 && errors[0] == 77, "the client hears the error, for its frame");
    expect(read.status == ReadStatus::ENDED, "and the stream ends cleanly, not with a reset");
}

// An idle connection and a sleeping gateway: the stop's wake is all that ends them.
void testAStopWakesASleepingGateway() {
    std::optional<LiveEngine> engine;
    engine.emplace();
    const TcpSocket idle = TcpSocket::connectLoopback(engine->port());
    const Bytes hello = helloFrame(1);
    expect(idle.send(hello) == hello.size(), "the client says Hello");
    std::array<std::byte, 64> buffer{};
    expect(idle.receive(buffer).bytes == 24, "and is welcomed");

    engine->stop();  // hangs if the wake doesn't reach the sleeping thread
    expect(idle.receive(buffer).status == ReadStatus::ENDED, "the stop closes the idle connection cleanly");
}

}  // namespace

int main() {
    testLoopbackSocketsWork();
    testAConnectionMovesBetweenSets();
    testAWakeEndsASleep();
    testTheEngineOverLoopback();
    testPastTheLimitOverLoopback();
    testAProtocolErrorReachesAClientStillSending();
    testAStopWakesASleepingGateway();
    std::cout << "gateway socket tests passed" << std::endl;
    return EXIT_SUCCESS;
}
