#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "order_book/runtime/matching_loop.hpp"

namespace order_book {

// Befriended by Doorbell, so a test can catch a sleeper at the moment it announces itself.
struct OrderBookTestAccess {
    static bool sleeperAnnounced(const runtime::Doorbell& doorbell) {
        return doorbell.state.load(std::memory_order_seq_cst) == runtime::Doorbell::asleep;
    }
};

}  // namespace order_book

namespace {

using namespace order_book;
using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

replay::Command add(OrderId id, Side side, Price price, Quantity quantity) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

CommandElement element(const replay::Command& command, std::uint64_t correlation, std::uint32_t slot = 0) {
    return CommandElement{correlation, command, slot, 5, ControlKind::COMMAND};
}

std::vector<ResponseRecord> drainAll(Ring<ResponseRecord>& ring) {
    std::vector<ResponseRecord> records;
    while (ring.drain(64, [&records](const ResponseRecord& record) { records.push_back(record); }) != 0) {
    }
    return records;
}

void testAPassRunsCommandsAndSendsTheirEvents() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0);
    Doorbell doorbell;
    Ring<CommandElement> commands(16);
    Ring<ResponseRecord> responses(16);
    MatchingLoop loop(shard, doorbell, {RingPair{&commands, &responses}});

    commands.tryPush(element(add(1, Side::SELL, 100, 5), 11));
    commands.tryPush(element(add(2, Side::BUY, 100, 2), 12));
    expect(loop.pass() == 2, "a pass takes what is there");

    const std::vector<ResponseRecord> records = drainAll(responses);
    expect(records.size() == 3, "one record per event");
    expect(records[0].correlation == 11 && records[0].kind == ResponseKind::LAST_EVENT, "a lone event is the last of its response");
    expect(records[1].correlation == 12 && records[1].kind == ResponseKind::EVENT, "the first of two is not");
    expect(records[2].correlation == 12 && records[2].kind == ResponseKind::LAST_EVENT, "the second is");
    expect(std::holds_alternative<TradeExecuted>(records[2].event), "with the book's own event");
    expect(records[2].connection == 5 && records[2].slot == 0, "a record names its connection and slot");
    expect(shard.commandsTaken() == 2, "the shard took both");
}

void testTakesABatchFromEachRingInTurn() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0, BookCapacity{1024, 8, 8});
    Doorbell doorbell;
    Ring<CommandElement> first(256);
    Ring<CommandElement> second(256);
    Ring<ResponseRecord> firstResponses(1024);
    Ring<ResponseRecord> secondResponses(1024);
    MatchingLoop loop(shard, doorbell, {RingPair{&first, &firstResponses}, RingPair{&second, &secondResponses}});

    for (OrderId id = 1; id <= 100; ++id) {
        first.tryPush(element(add(id, Side::BUY, id, 1), id));
        second.tryPush(element(add(id + 1000, Side::BUY, id, 1), id));
    }
    expect(loop.pass() == 2 * takeBatch, "a pass takes one batch from each ring");
    expect(drainAll(firstResponses).size() == takeBatch, "a ring's responses go back on its own pair");
    expect(drainAll(secondResponses).size() == takeBatch, "both rings");
    expect(loop.pass() == 200 - 2 * takeBatch, "the next pass takes the rest");
}

void testStopNeedsAMarkerFromEveryProducer() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0);
    Doorbell doorbell;
    Ring<CommandElement> first(16);
    Ring<CommandElement> second(16);
    Ring<ResponseRecord> firstResponses(16);
    Ring<ResponseRecord> secondResponses(16);
    MatchingLoop loop(shard, doorbell, {RingPair{&first, &firstResponses}, RingPair{&second, &secondResponses}});

    first.tryPush(CommandElement::stopMarker());
    loop.pass();
    expect(!loop.stopped(), "one producer's marker is not a stop");
    second.tryPush(element(add(1, Side::BUY, 100, 5), 1));
    second.tryPush(CommandElement::stopMarker());
    loop.run();
    expect(loop.stopped(), "every producer's is");
    expect(shard.commandsTaken() == 1, "a marker is not a command, and takes no log position");

    const std::vector<ResponseRecord> onFirst = drainAll(firstResponses);
    const std::vector<ResponseRecord> onSecond = drainAll(secondResponses);
    expect(onFirst.size() == 1 && onFirst[0].kind == ResponseKind::SHARD_STOPPED, "every response ring hears the stop");
    expect(onSecond.size() == 2 && onSecond[1].kind == ResponseKind::SHARD_STOPPED, "after every answer on it");
}

struct Client {
    Ring<CommandElement>& commands;
    Ring<ResponseRecord>& responses;
    Doorbell& doorbell;

    void send(const CommandElement& command) {
        while (!commands.tryPush(command)) {
            std::this_thread::yield();
        }
        doorbell.ring();
    }

    // Everything up to and including the stop, on a shard that is being asked to stop.
    std::vector<ResponseRecord> stopAndCollect() {
        send(CommandElement::stopMarker());
        std::vector<ResponseRecord> records;
        bool stopped = false;
        while (!stopped) {
            responses.drain(64, [&](const ResponseRecord& record) {
                if (record.kind == ResponseKind::SHARD_STOPPED) {
                    stopped = true;
                } else {
                    records.push_back(record);
                }
            });
        }
        return records;
    }
};

// A command that arrives after a pass found nothing, and whose ring came before the sleeper
// announced itself: only the re-check after the announcement can see it, since nothing will ring
// again. Stepped on one thread, so it lands there every time. Without the re-check this hangs,
// and the CTest timeout fails it.
void testACommandBeforeTheSleepIsSeen() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0);
    Doorbell doorbell;
    Ring<CommandElement> commands(16);
    Ring<ResponseRecord> responses(16);
    MatchingLoop loop(shard, doorbell, {RingPair{&commands, &responses}}, std::chrono::nanoseconds::zero());

    expect(loop.pass() == 0, "the rings start empty");
    commands.tryPush(element(replay::CancelCommand{1}, 1));
    doorbell.ring();
    loop.sleepUntilRung();
    expect(loop.pass() == 1, "the sleep returned at once, and the command is taken");
}

// One command at a time, each sent only once the thread has announced that it is going to sleep,
// so a ring must wake it. Without the notify this hangs, and the CTest timeout fails it. Without
// the wait the producer mostly beats the thread to sleep, and this would pass regardless.
void testARingWakesASleeper() {
    constexpr std::size_t commands = 5000;
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Doorbell doorbell;
    Ring<CommandElement> commandRing(16);
    Ring<ResponseRecord> responseRing(16);
    Client client{commandRing, responseRing, doorbell};

    std::thread matching([&] {
        Shard shard(directory, 0);
        MatchingLoop loop(shard, doorbell, {RingPair{&commandRing, &responseRing}}, std::chrono::nanoseconds::zero());
        loop.run();
    });

    for (std::size_t i = 0; i < commands; ++i) {
        while (!OrderBookTestAccess::sleeperAnnounced(doorbell)) {
            std::this_thread::yield();
        }
        client.send(element(replay::CancelCommand{1}, i));
        bool answered = false;
        while (!answered) {
            responseRing.drain(1, [&answered, i](const ResponseRecord& record) {
                answered = record.correlation == i && record.kind == ResponseKind::LAST_EVENT;
            });
        }
    }
    client.stopAndCollect();
    matching.join();
}

// Two-slot rings, with a response bigger than both: it can only arrive if the matching thread
// waits in place while the producer keeps draining.
void testAFullResponseRingWaitsInPlace() {
    constexpr OrderId resting = 500;
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Doorbell doorbell;
    Ring<CommandElement> commandRing(2);
    Ring<ResponseRecord> responseRing(2);
    Client client{commandRing, responseRing, doorbell};

    std::size_t events = 0;
    const auto collect = [&] {
        responseRing.drain(2, [&events](const ResponseRecord&) { ++events; });
    };

    std::thread matching([&] {
        Shard shard(directory, 0, BookCapacity{1024, 8, 8});
        MatchingLoop loop(shard, doorbell, {RingPair{&commandRing, &responseRing}}, unlimitedSpin);
        loop.run();
    });

    for (OrderId id = 1; id <= resting; ++id) {
        client.send(element(add(id, Side::SELL, 100, 1), id));
        collect();
    }
    client.send(element(add(resting + 1, Side::BUY, 100, resting), 999));
    while (events < resting + resting + 1) {
        collect();
    }
    client.stopAndCollect();
    matching.join();
    expect(events == resting + resting + 1, "every event of a response larger than the ring came through");
}

std::vector<replay::Command> scriptOf(std::size_t commands) {
    std::vector<replay::Command> script;
    script.reserve(commands);
    for (std::size_t i = 0; i < commands; ++i) {
        const auto id = static_cast<OrderId>(i + 1);
        if (i % 5 == 4) {
            script.push_back(replay::CancelCommand{id - 3});
        } else {
            script.push_back(add(id, i % 2 == 0 ? Side::SELL : Side::BUY, static_cast<Price>(100 + i % 7), 3));
        }
    }
    return script;
}

// Real threads must give the bytes one thread gives, and that is what a race would break.
void testAThreadGivesWhatOneThreadGives() {
    const std::vector<replay::Command> script = scriptOf(5000);
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);

    std::vector<BookEvent> direct;
    {
        Shard shard(directory, 0);
        for (const replay::Command& command : script) {
            shard.process(command, 0);
            const std::span<const BookEvent> events = shard.result().events();
            direct.insert(direct.end(), events.begin(), events.end());
        }
    }

    for (const std::chrono::nanoseconds spinWindow : {std::chrono::nanoseconds::zero(), defaultSpinWindow, unlimitedSpin}) {
        Doorbell doorbell;
        Ring<CommandElement> commandRing(8);
        Ring<ResponseRecord> responseRing(8);
        Client client{commandRing, responseRing, doorbell};
        std::thread matching([&] {
            Shard shard(directory, 0);
            MatchingLoop loop(shard, doorbell, {RingPair{&commandRing, &responseRing}}, spinWindow);
            loop.run();
        });

        std::vector<BookEvent> threaded;
        std::size_t sent = 0;
        while (sent < script.size() || threaded.size() < direct.size()) {
            if (sent < script.size() && commandRing.tryPush(element(script[sent], sent))) {
                ++sent;
                doorbell.ring();
            }
            responseRing.drain(8, [&threaded](const ResponseRecord& record) { threaded.push_back(record.event); });
        }
        client.stopAndCollect();
        matching.join();
        expect(threaded == direct, "a matching thread gives the events one thread gives");
    }
}

}  // namespace

int main() {
    testAPassRunsCommandsAndSendsTheirEvents();
    testTakesABatchFromEachRingInTurn();
    testStopNeedsAMarkerFromEveryProducer();
    testACommandBeforeTheSleepIsSeen();
    testARingWakesASleeper();
    testAFullResponseRingWaitsInPlace();
    testAThreadGivesWhatOneThreadGives();
    std::cout << "matching loop tests passed" << std::endl;
    return EXIT_SUCCESS;
}
