#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "command.hpp"
#include "command_generator.hpp"
#include "command_replayer.hpp"
#include "failure_report.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "reference_book.hpp"
#include "shrink.hpp"

namespace {

using namespace order_book::randomized;
using order_book::Order;
using order_book::OrderBook;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Callable>
void expectThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }

    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// FNV-1a over the stream's text, one command per line.
std::uint64_t fingerprint(const std::vector<Command>& commands) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const Command& command : commands) {
        for (const char c : formatCommand(command) + "\n") {
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }
    }
    return hash;
}

std::string temporaryPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("order_book_randomized_" + name)).string();
}

// The same seed and profile give the same stream; another seed gives another.
void testSameSeedSameStream() {
    for (const WorkloadProfile& profile : workloadProfiles()) {
        const std::string context = "same seed, same stream (" + profile.name + ")";
        const std::vector<Command> first = generateCommands(99, profile, 1500);
        expect(first.size() == 1500, context + ": should generate every command asked for");
        expect(generateCommands(99, profile, 1500) == first, context + ": a seed should always give the same stream");
        expect(generateCommands(100, profile, 1500) != first, context + ": another seed should give another stream");
        const std::vector<Command> shorter = generateCommands(99, profile, 700);
        expect(std::equal(shorter.begin(), shorter.end(), first.begin()),
               context + ": a shorter run should be a prefix of a longer one");
    }
}

// Pins each profile's output under this generator version. If a change to the generator or a
// profile breaks this, every recorded seed now means a different stream: bump generatorVersion
// and update these values (the failure prints the new ones).
void testGeneratorFingerprints() {
    const std::map<std::string, std::uint64_t> pinned = {
        {"balanced", 5564879061556947044ULL},
        {"aggressive", 8481657866503503821ULL},
        {"passive", 6763976021892944449ULL},
        {"cancel-heavy", 3611834777573128470ULL},
        {"queue", 7739213142561390673ULL},
        {"one-sided", 18072558173000573765ULL},
        {"invalid-heavy", 2685711784982307739ULL},
        {"limits", 8556868042740139041ULL},
        {"self-trade", 8315979448594823842ULL},
        {"stop-cascade", 736736494424531975ULL},
        {"immediate", 15317620537276013285ULL},
    };
    expect(generatorVersion == 8, "the fingerprints below are for generator version 8");
    bool allMatch = true;
    std::string actual;
    for (const WorkloadProfile& profile : workloadProfiles()) {
        const std::uint64_t hash = fingerprint(generateCommands(1, profile, 2000));
        actual += "        {\"" + profile.name + "\", " + std::to_string(hash) + "ULL},\n";
        const auto found = pinned.find(profile.name);
        allMatch = allMatch && found != pinned.end() && found->second == hash;
    }
    expect(allMatch, "generator fingerprints changed; bump generatorVersion and pin:\n" + actual);
}

// Running against the book records exactly the stream the generator gives on its own, since
// generation reads only the model.
void testRunRecordsTheGeneratedStream() {
    const std::string context = "run records the generated stream";
    const WorkloadProfile& profile = findProfile("balanced");
    CommandRecorder recorder(StreamHeader{generatorVersion, profile.name, 5, describeProfile(profile), ""});
    OrderBook book;
    RandomCommandGenerator generator(5, profile);
    const RunResult result = CommandReplayer().run(book, generator, 2000, recorder);
    expect(result.passed(), context + ": the book should pass every check");
    expect(result.stats.commands == 2000, context + ": every command should run");
    expect(recorder.commands() == generateCommands(5, profile, 2000),
           context + ": the recorded stream should equal the one generated without the book");

    OrderBook again;
    expect(CommandReplayer().replay(again, recorder.commands()).passed(), context + ": replaying it should pass too");
}

// Saved streams come back exactly, malformed commands included, and bad files are refused
// with the line at fault.
void testStreamRoundTrip() {
    const std::string context = "stream round trip";
    CommandStream stream{StreamHeader{generatorVersion, "invalid-heavy", 42, "some config", "a note"},
                         generateCommands(42, findProfile("invalid-heavy"), 3000)};

    Command oddSide = Command::submit(7, static_cast<Side>(7), 100, 5);
    Command oddType = Command::submit(8, Side::SELL, 100, 5, CommandIntent::UNSUPPORTED_TYPE);
    oddType.orderType = static_cast<order_book::OrderType>(3);
    Command filled = Command::submit(9, Side::BUY, 4294967295U, 4294967295U, CommandIntent::MARKED_FILLED);
    filled.markedFilled = true;
    filled.overload = SubmitOverload::ORDER;
    Command viaOrder = Command::submit(10, Side::SELL, 100, 5, CommandIntent::NEW_ID);
    viaOrder.overload = SubmitOverload::ORDER;
    expect(formatCommand(viaOrder) == "SUBMIT id=10 side=SELL price=100 qty=5 overload=ORDER intent=NEW_ID",
           context + ": the Order overload should print as overload=ORDER");
    expect(formatCommand(Command::submit(10, Side::SELL, 100, 5)) == "SUBMIT id=10 side=SELL price=100 qty=5",
           context + ": the request overload, the default, should not print");
    Command immediate = Command::submit(11, Side::BUY, 100, 5, CommandIntent::NEW_ID);
    immediate.timeInForce = order_book::TimeInForce::IOC;
    expect(formatCommand(immediate) == "SUBMIT id=11 side=BUY price=100 qty=5 tif=IOC intent=NEW_ID",
           context + ": IOC should print as tif=IOC");
    Command marketOrder = Command::submit(14, Side::BUY, 0, 5, CommandIntent::NEW_ID);
    marketOrder.orderType = order_book::OrderType::MARKET;
    marketOrder.timeInForce = order_book::TimeInForce::IOC;
    expect(formatCommand(marketOrder) == "SUBMIT id=14 side=BUY price=0 qty=5 type=MARKET tif=IOC intent=NEW_ID",
           context + ": a market order should print its type by name");
    stream.commands.push_back(marketOrder);
    Command maker = Command::submit(15, Side::SELL, 101, 5, CommandIntent::NEW_ID);
    maker.postOnly = true;
    expect(formatCommand(maker) == "SUBMIT id=15 side=SELL price=101 qty=5 post_only=Y intent=NEW_ID",
           context + ": post-only should print as post_only=Y");
    stream.commands.push_back(maker);
    Command ownedOrder = Command::submit(16, Side::SELL, 101, 5, CommandIntent::NEW_ID);
    ownedOrder.owner = 3;
    ownedOrder.selfTradePrevention = order_book::SelfTradePrevention::CANCEL_BOTH;
    expect(formatCommand(ownedOrder) == "SUBMIT id=16 side=SELL price=101 qty=5 owner=3 stp=CANCEL_BOTH intent=NEW_ID",
           context + ": an owner and a mode other than CANCEL_RESTING should print");
    Command oddMode = Command::submit(17, Side::SELL, 101, 5, CommandIntent::INVALID_STP_MODE);
    oddMode.selfTradePrevention = static_cast<order_book::SelfTradePrevention>(4);
    stream.commands.push_back(ownedOrder);
    stream.commands.push_back(oddMode);
    Command stopLimit = Command::submit(18, Side::BUY, 101, 5, CommandIntent::NEW_ID);
    stopLimit.orderType = order_book::OrderType::STOP_LIMIT;
    stopLimit.stopPrice = 103;
    expect(formatCommand(stopLimit) == "SUBMIT id=18 side=BUY price=101 qty=5 type=STOP_LIMIT stop_price=103 intent=NEW_ID",
           context + ": a stop-limit should print its type and stop price");
    stream.commands.push_back(stopLimit);
    stream.commands.push_back(Command::cancel(18, CommandIntent::CANCEL_PENDING_STOP));
    Command killer = Command::submit(13, Side::SELL, 100, 5, CommandIntent::NEW_ID);
    killer.timeInForce = order_book::TimeInForce::FOK;
    expect(formatCommand(killer) == "SUBMIT id=13 side=SELL price=100 qty=5 tif=FOK intent=NEW_ID",
           context + ": FOK should print as tif=FOK");
    stream.commands.push_back(killer);
    Command oddTimeInForce = Command::submit(12, Side::BUY, 100, 5, CommandIntent::INVALID_TIME_IN_FORCE);
    oddTimeInForce.timeInForce = static_cast<order_book::TimeInForce>(7);
    stream.commands.push_back(immediate);
    stream.commands.push_back(oddTimeInForce);
    stream.commands.push_back(viaOrder);
    stream.commands.push_back(oddSide);
    stream.commands.push_back(oddType);
    stream.commands.push_back(filled);
    stream.commands.push_back(Command::cancel(0));
    stream.commands.push_back(Command::submit(0, Side::BUY, 0, 0, CommandIntent::ZERO_ORDER_ID));

    std::stringstream text;
    writeStream(text, stream);
    expect(readStream(text) == stream, context + ": a written stream should read back identically");

    const std::string path = temporaryPath("round_trip.txt");
    saveStream(path, stream);
    expect(loadStream(path) == stream, context + ": a saved stream should load back identically");
    CommandRecorder recorder(stream.header);
    for (const Command& command : stream.commands) {
        recorder.record(command);
    }
    recorder.save(path, "recorded");
    CommandStream recorded = stream;
    recorded.header.note = "recorded";
    expect(loadStream(path) == recorded, context + ": the recorder should save what it recorded");
    std::filesystem::remove(path);

    for (const std::string bad : {"SUBMIT id=1 side=BUY price=5", "CANCEL id=1 side=BUY", "MODIFY id=1",
                                  "SUBMIT id=1 side=BUY price=5 qty=4294967296", "CANCEL id=x",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 status=PARTIAL", "CANCEL id=1 intent=MAYBE",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 status=FILLED",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 overload=REQUEST", "CANCEL id=1 overload=ORDER",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 tif=IOC overload=ORDER", "CANCEL id=1 tif=IOC",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 post_only=Y overload=ORDER",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 post_only=N",
                                  "SUBMIT id=1 side=BUY price=5 qty=1 owner=2 overload=ORDER", "CANCEL id=1 owner=2"}) {
        expectThrows([&] { parseCommand(bad); }, context + ": should refuse '" + bad + "'");
    }
    std::stringstream noFormat("SUBMIT id=1 side=BUY price=5 qty=1\n");
    expectThrows([&] { readStream(noFormat); }, context + ": a stream needs its format line");
    std::stringstream badLine("format 1\n# comment\nCANCEL id=1\nCANCEL\n");
    try {
        readStream(badLine);
        expect(false, context + ": a stream with a bad command should be refused");
    } catch (const std::invalid_argument& error) {
        expect(contains(error.what(), "line 4"), context + ": the error should name the bad line, got " + error.what());
    }
}

// The balanced profile follows the agreed split: about 70/30 submissions to cancellations,
// 80/20 valid to invalid, 50/50 sides, mostly prices near the touch and small sizes, mostly
// new IDs; and every profile only ever breaks the one thing its intent says.
void testDistributions() {
    for (const WorkloadProfile& profile : workloadProfiles()) {
        const std::string context = "distribution (" + profile.name + ")";
        RandomCommandGenerator generator(2026, profile);
        ReferenceBook model;
        std::map<CommandIntent, std::size_t> intents;
        // Counted only while something rests: with nothing to cancel, a valid cancellation
        // becomes a submission, so an empty book skews the split by design.
        std::size_t commandsWithOrders = 0;
        std::size_t submissionsWithOrders = 0;
        std::size_t validSubmissions = 0;
        std::size_t viaOrder = 0;
        std::size_t ioc = 0;
        std::size_t fok = 0;
        std::size_t markets = 0;
        std::size_t stops = 0;
        std::size_t stopLimits = 0;
        std::size_t requestLimits = 0;
        std::size_t stopLimitIoc = 0;
        std::size_t pastTrigger = 0;
        std::size_t stopsAfterATrade = 0;
        std::size_t postOnly = 0;
        std::size_t ownedRequests = 0;
        std::size_t cancelIncoming = 0;
        std::size_t gtcLimits = 0;
        std::size_t buys = 0;
        std::size_t nearTouch = 0;
        std::size_t large = 0;
        constexpr std::size_t count = 20000;
        for (std::size_t i = 0; i < count; ++i) {
            const Command command = generator.next(model);
            ++intents[command.intent];
            if (!model.restingIds().empty()) {
                ++commandsWithOrders;
                submissionsWithOrders += command.type == CommandType::SUBMIT ? 1 : 0;
            }
            if (command.intent == CommandIntent::NEW_ID || command.intent == CommandIntent::REUSED_ID) {
                ++validSubmissions;
                viaOrder += command.overload == SubmitOverload::ORDER ? 1 : 0;
                buys += command.side == Side::BUY ? 1 : 0;
                large += command.quantity > profile.smallQuantityMax ? 1 : 0;
                using order_book::OrderType;
                using order_book::TimeInForce;
                const bool market = command.orderType == OrderType::MARKET;
                const bool stop = command.orderType == OrderType::STOP;
                const bool stopLimit = command.orderType == OrderType::STOP_LIMIT;
                const bool limit = command.orderType == OrderType::LIMIT;
                const bool viaRequest = command.overload == SubmitOverload::REQUEST;
                markets += market ? 1 : 0;
                stops += stop ? 1 : 0;
                stopLimits += stopLimit ? 1 : 0;
                requestLimits += limit && viaRequest ? 1 : 0;
                ioc += limit && command.timeInForce == TimeInForce::IOC ? 1 : 0;
                fok += limit && command.timeInForce == TimeInForce::FOK ? 1 : 0;
                stopLimitIoc += stopLimit && command.timeInForce == TimeInForce::IOC ? 1 : 0;
                const bool gtcLimit = limit && command.timeInForce == TimeInForce::GTC && viaRequest;
                gtcLimits += gtcLimit ? 1 : 0;
                postOnly += command.postOnly ? 1 : 0;
                if (viaRequest) {
                    ownedRequests += command.owner != 0 ? 1 : 0;
                    cancelIncoming +=
                        command.selfTradePrevention == order_book::SelfTradePrevention::CANCEL_INCOMING ? 1 : 0;
                    expect(command.owner <= profile.ownerPool &&
                               order_book::isValidSelfTradePrevention(command.selfTradePrevention),
                           context + ": a valid request's owner comes from the pool: " + formatCommand(command));
                } else {
                    expect(command.owner == 0 && limit, context + ": an Order is a limit with no owner: " + formatCommand(command));
                }
                expect(!command.postOnly || gtcLimit,
                       context + ": a valid post-only order is a GTC limit request: " + formatCommand(command));
                expect(!(market || stop) || (command.price == 0 && command.timeInForce != TimeInForce::GTC && viaRequest),
                       context + ": a valid market or STOP order has no price and is IOC or FOK: " + formatCommand(command));
                expect((stop || stopLimit) == (command.stopPrice != 0),
                       context + ": exactly the stops carry a stop price: " + formatCommand(command));
                if ((stop || stopLimit) && model.lastTradePrice()) {
                    const std::int64_t last = *model.lastTradePrice();
                    const std::int64_t ahead = (command.side == Side::BUY ? 1 : -1) *
                                               (static_cast<std::int64_t>(command.stopPrice) - last);
                    const std::int64_t near = static_cast<std::int64_t>(profile.stopNearTicks);
                    expect(ahead <= near && ahead >= -near,
                           context + ": a stop price should be near the last trade price: " + formatCommand(command));
                    // Clamped to the price range, a stop near either end may land past its
                    // trigger, so only those away from the ends count toward the share.
                    const std::int64_t top = std::numeric_limits<order_book::Price>::max();
                    if (last > near + 1 && last < top - near - 1) {
                        pastTrigger += ahead <= 0 ? 1 : 0;
                        ++stopsAfterATrade;
                    }
                }
                expect(viaRequest || command.timeInForce == TimeInForce::GTC,
                       context + ": an Order-overload submission has no time in force: " + formatCommand(command));
                expect(command.orderId != 0 && (command.price != 0 || market || stop) && command.quantity != 0 &&
                           command.quantity <= profile.largeQuantityMax && !command.markedFilled &&
                           order_book::isValidOrderType(command.orderType) &&
                           order_book::isValidTimeInForce(command.timeInForce) &&
                           (command.side == Side::BUY || command.side == Side::SELL),
                       context + ": a valid submission should be well formed: " + formatCommand(command));
                expect(command.intent == CommandIntent::NEW_ID || model.removedIds().contains(command.orderId),
                       context + ": a reused ID should be one that was filled, cancelled, or expired");
                if (market || stop) {
                    ++nearTouch;  // no price to be near or far
                }
                const std::int64_t price = command.price;
                std::int64_t distance = std::numeric_limits<std::int64_t>::max();
                for (Side side : {Side::BUY, Side::SELL}) {
                    if (const std::optional<order_book::Price> best = model.best(side)) {
                        distance = std::min(distance, std::abs(price - static_cast<std::int64_t>(*best)));
                    }
                }
                if (distance == std::numeric_limits<std::int64_t>::max()) {
                    distance = std::abs(price - static_cast<std::int64_t>(profile.startPrice));
                }
                const std::int64_t nearBand =
                    static_cast<std::int64_t>(std::max(profile.nearTicks, profile.throughTicks)) + 1;
                nearTouch += !(market || stop) && distance <= nearBand ? 1 : 0;
            }
            if (command.intent != CommandIntent::NEW_ID && command.intent != CommandIntent::REUSED_ID &&
                command.intent != CommandIntent::MARKED_FILLED && command.intent != CommandIntent::TYPE_CANNOT_REST) {
                expect(command.overload == SubmitOverload::REQUEST,
                       context + ": only valid limits and the Order-only faults go through the Order overload: " +
                           formatCommand(command));
            }
            switch (command.intent) {
                case CommandIntent::DUPLICATE_ACTIVE_ID:
                    expect(model.restingIds().contains(command.orderId) || model.pendingIds().contains(command.orderId),
                           context + ": a duplicate should be live");
                    break;
                case CommandIntent::STOP_GTC:
                    expect(command.orderType == order_book::OrderType::STOP && command.price == 0 &&
                               command.stopPrice != 0 && command.timeInForce == order_book::TimeInForce::GTC,
                           context + ": a GTC STOP");
                    break;
                case CommandIntent::POST_ONLY_STOP:
                    expect(command.orderType == order_book::OrderType::STOP_LIMIT && command.postOnly && command.stopPrice != 0,
                           context + ": a post-only stop-limit");
                    break;
                case CommandIntent::MISSING_STOP_PRICE:
                    expect(order_book::isStopType(command.orderType) && command.stopPrice == 0,
                           context + ": a stop without a stop price");
                    break;
                case CommandIntent::STOP_PRICE_ON_LIMIT:
                    expect(!order_book::isStopType(command.orderType) && command.stopPrice != 0,
                           context + ": a stop price on an order that is not a stop");
                    break;
                case CommandIntent::CANCEL_PENDING_STOP:
                    expect(model.pendingIds().contains(command.orderId), context + ": a valid stop cancel should pend");
                    break;
                case CommandIntent::ZERO_ORDER_ID: expect(command.orderId == 0, context + ": zero ID"); break;
                case CommandIntent::ZERO_PRICE: expect(command.price == 0, context + ": zero price"); break;
                case CommandIntent::ZERO_QUANTITY: expect(command.quantity == 0, context + ": zero quantity"); break;
                case CommandIntent::INVALID_SIDE:
                    expect(command.side != Side::BUY && command.side != Side::SELL, context + ": invalid side");
                    break;
                case CommandIntent::UNSUPPORTED_TYPE:
                    expect(!order_book::isValidOrderType(command.orderType) && command.overload == SubmitOverload::REQUEST,
                           context + ": a type that is no enumerator");
                    break;
                case CommandIntent::TYPE_CANNOT_REST:
                    expect(command.orderType != order_book::OrderType::LIMIT && command.overload == SubmitOverload::ORDER,
                           context + ": a type an Order cannot hold, through the Order overload");
                    break;
                case CommandIntent::MARKET_WITH_PRICE:
                    expect(command.orderType == order_book::OrderType::MARKET && command.price != 0 &&
                               command.timeInForce == order_book::TimeInForce::IOC,
                           context + ": a market order with a price");
                    break;
                case CommandIntent::POST_ONLY_NOT_GTC_LIMIT:
                    expect(command.postOnly && (command.orderType == order_book::OrderType::MARKET ||
                                                command.timeInForce != order_book::TimeInForce::GTC),
                           context + ": post-only on a market, IOC, or FOK order");
                    break;
                case CommandIntent::INVALID_STP_MODE:
                    expect(!order_book::isValidSelfTradePrevention(command.selfTradePrevention),
                           context + ": an invalid self-trade prevention mode");
                    break;
                case CommandIntent::MARKET_GTC:
                    expect(command.orderType == order_book::OrderType::MARKET && command.price == 0 &&
                               command.timeInForce == order_book::TimeInForce::GTC,
                           context + ": a GTC market order");
                    break;
                case CommandIntent::MARKED_FILLED:
                    expect(command.markedFilled && command.overload == SubmitOverload::ORDER,
                           context + ": marked filled, through the Order overload");
                    break;
                case CommandIntent::INVALID_TIME_IN_FORCE:
                    expect(!order_book::isValidTimeInForce(command.timeInForce), context + ": invalid time in force");
                    break;
                case CommandIntent::CANCEL_ACTIVE:
                    expect(model.restingIds().contains(command.orderId), context + ": a valid cancel should rest");
                    break;
                case CommandIntent::CANCEL_REMOVED:
                    expect(model.removedIds().contains(command.orderId), context + ": a removed ID");
                    break;
                case CommandIntent::CANCEL_UNKNOWN:
                    expect(!model.restingIds().contains(command.orderId) &&
                               !model.pendingIds().contains(command.orderId) &&
                               !model.removedIds().contains(command.orderId),
                           context + ": an unknown ID should never have been accepted");
                    break;
                case CommandIntent::CANCEL_ZERO_ID: expect(command.orderId == 0, context + ": zero cancel"); break;
                default: break;
            }
            applyCommand(model, command);
        }

        std::size_t invalid = 0;
        for (const auto& [intent, seen] : intents) {
            invalid += isIntentionallyInvalid(intent) ? seen : 0;
        }
        expect(intents.size() == 24 && intents.count(CommandIntent::UNSPECIFIED) == 0,
               context + ": every intent should occur, got " + std::to_string(intents.size()));
        const auto share = [](std::size_t part, std::size_t whole) {
            return 100.0 * static_cast<double>(part) / static_cast<double>(whole);
        };
        const auto near = [](double actual, double target) { return actual > target - 2.5 && actual < target + 2.5; };
        expect(near(share(invalid, count), profile.invalidPercent),
               context + ": invalid share " + std::to_string(share(invalid, count)));
        expect(near(share(viaOrder, validSubmissions), profile.orderOverloadPercent),
               context + ": Order-overload share " + std::to_string(share(viaOrder, validSubmissions)));
        const std::size_t viaRequest = validSubmissions - viaOrder;
        expect(near(share(markets, viaRequest), profile.marketPercent),
               context + ": market share " + std::to_string(share(markets, viaRequest)));
        expect(near(share(stops, viaRequest), profile.stopPercent),
               context + ": STOP share " + std::to_string(share(stops, viaRequest)));
        expect(near(share(stopLimits, viaRequest), profile.stopLimitPercent),
               context + ": STOP_LIMIT share " + std::to_string(share(stopLimits, viaRequest)));
        expect(near(share(ioc, requestLimits), profile.iocPercent),
               context + ": IOC share " + std::to_string(share(ioc, requestLimits)));
        expect(near(share(fok, requestLimits), profile.fokPercent),
               context + ": FOK share " + std::to_string(share(fok, requestLimits)));
        // Smaller samples (about a thousand stops), so a wider band.
        const auto roughly = [](double actual, double target) { return actual > target - 5 && actual < target + 5; };
        expect(roughly(share(stopLimitIoc, stopLimits), profile.stopLimitIocPercent),
               context + ": stop-limit IOC share " + std::to_string(share(stopLimitIoc, stopLimits)));
        // The queue profile trades only at the bottom of the range, so it has no such stops.
        expect(stopsAfterATrade < 200 || roughly(share(pastTrigger, stopsAfterATrade), profile.stopPastTriggerPercent),
               context + ": past-trigger share " + std::to_string(share(pastTrigger, stopsAfterATrade)));
        expect(near(share(ownedRequests, viaRequest), profile.ownerPercent),
               context + ": owner share " + std::to_string(share(ownedRequests, viaRequest)));
        expect(near(share(cancelIncoming, viaRequest), profile.cancelIncomingPercent),
               context + ": CANCEL_INCOMING share " + std::to_string(share(cancelIncoming, viaRequest)));
        expect(near(share(postOnly, gtcLimits), profile.postOnlyPercent),
               context + ": post-only share " + std::to_string(share(postOnly, gtcLimits)));
        expect(near(share(buys, validSubmissions), profile.buyPercent),
               context + ": buy share " + std::to_string(share(buys, validSubmissions)));
        expect(share(nearTouch, validSubmissions) > 100.0 - profile.farPercent - 5,
               context + ": most prices should be near the touch, got " + std::to_string(share(nearTouch, validSubmissions)));
        expect(near(share(large, validSubmissions), profile.largeQuantityPercent),
               context + ": large share " + std::to_string(share(large, validSubmissions)));
        expect(intents[CommandIntent::NEW_ID] > intents[CommandIntent::REUSED_ID],
               context + ": new IDs should outnumber reused ones");
        expect(near(share(submissionsWithOrders, commandsWithOrders), profile.submitPercent),
               context + ": submission share " + std::to_string(share(submissionsWithOrders, commandsWithOrders)));
    }
}

// Every profile passes on the engine and reaches what it is meant to stress.
void testProfilesExerciseTheBook() {
    for (const WorkloadProfile& profile : workloadProfiles()) {
        const std::string context = "profile coverage (" + profile.name + ")";
        std::uint64_t largestAggregate = 0;
        unsigned int longestQueue = 0;
        const CommandReplayer replayer([&](const OrderBook& book, const Command&) -> std::optional<std::string> {
            const order_book::BookSnapshot snapshot = book.snapshot(std::numeric_limits<unsigned int>::max());
            for (const auto* levels : {&snapshot.bids, &snapshot.asks}) {
                for (const order_book::PriceLevelSummary& level : *levels) {
                    largestAggregate = std::max(largestAggregate, level.totalQuantity);
                    longestQueue = std::max(longestQueue, level.orderCount);
                }
            }
            return std::nullopt;
        });
        CommandRecorder recorder;
        OrderBook book;
        RandomCommandGenerator generator(11, profile);
        const RunResult result = replayer.run(book, generator, 3000, recorder);
        expect(result.passed(), context + ": should pass, but failed with " +
                                    (result.failure ? result.failure->reason : std::string()));
        const RunStats& stats = result.stats;
        expect(stats.trades > 0 && stats.cancelled > 0 && stats.rejected > 0 && stats.cancelsRejected > 0 &&
                   stats.expired > 0,
               context + ": should trade, cancel, expire, and refuse submissions and cancellations: " + stats.summary());
        if (profile.name == "passive") {
            expect(stats.peakLevels > 200, context + ": should build a deep book: " + stats.summary());
        } else if (profile.name == "queue") {
            expect(longestQueue >= 30, context + ": should queue many orders at one price, got " +
                                           std::to_string(longestQueue));
        } else if (profile.name == "aggressive") {
            expect(stats.mostTradesInOneSubmission >= 5, context + ": some orders should sweep several makers: " +
                                                             stats.summary());
        } else if (profile.name == "limits") {
            expect(largestAggregate > std::numeric_limits<order_book::Quantity>::max(),
                   context + ": a level should hold more than one Quantity in total");
        }
    }
}

// What each profile is for, judged on the model after each command of a 2000-command run, so a
// profile tuned into uselessness fails loudly. Each target needs to happen only once.
struct ProfileTarget {
    const char* profile;
    const char* behavior;
    // Given the model after the command, the command, and the model's prediction for it.
    std::function<bool(const ReferenceBook&, const Command&, const order_book::reference::ExpectedOutcome&)> hit;
};

std::size_t tradesIn(const order_book::reference::ExpectedOutcome& outcome) {
    return static_cast<std::size_t>(std::count_if(outcome.events.begin(), outcome.events.end(), [](const auto& event) {
        return std::holds_alternative<order_book::TradeExecuted>(event);
    }));
}

std::size_t longestQueue(const ReferenceBook& model) {
    std::size_t longest = 0;
    for (const auto& [price, queue] : model.bidQueues()) {
        longest = std::max(longest, queue.size());
    }
    for (const auto& [price, queue] : model.askQueues()) {
        longest = std::max(longest, queue.size());
    }
    return longest;
}

void testProfilesHitTheirTargets() {
    const std::vector<ProfileTarget> targets = {
        {"balanced", "a valid limit through submitOrder(Order) that trades",
         [](const ReferenceBook&, const Command& command, const auto& outcome) {
             return command.overload == SubmitOverload::ORDER && outcome.success && tradesIn(outcome) > 0;
         }},
        {"aggressive", "one submission sweeping five makers",
         [](const ReferenceBook&, const Command&, const auto& outcome) { return tradesIn(outcome) >= 5; }},
        {"passive", "a book of 100 levels, and a post-only order that would have taken",
         [expiredPostOnly = false](const ReferenceBook& model, const Command& command, const auto& outcome) mutable {
             expiredPostOnly = expiredPostOnly || (command.postOnly && outcome.success && outcome.events.size() == 2 &&
                                                   std::holds_alternative<order_book::OrderExpired>(outcome.events.back()));
             return expiredPostOnly && model.levelCount() >= 100;
         }},
        {"cancel-heavy", "a cancellation that empties the book",
         [](const ReferenceBook& model, const Command& command, const auto& outcome) {
             return command.type == CommandType::CANCEL && outcome.success && model.restingCount() == 0;
         }},
        {"queue", "twenty orders queued at one price",
         [](const ReferenceBook& model, const Command&, const auto&) { return longestQueue(model) >= 20; }},
        {"one-sided", "a buy that empties the ask side",
         [](const ReferenceBook& model, const Command& command, const auto& outcome) {
             return command.type == CommandType::SUBMIT && command.side == Side::BUY && tradesIn(outcome) > 0 &&
                    !model.best(Side::SELL);
         }},
        {"invalid-heavy", "a reused ID accepted",
         [](const ReferenceBook&, const Command& command, const auto& outcome) {
             return command.intent == CommandIntent::REUSED_ID && outcome.success;
         }},
        {"self-trade", "a prevented self-trade under each mode",
         [modes = std::set<order_book::SelfTradePrevention>{}](const ReferenceBook&, const Command& command,
                                                               const auto& outcome) mutable {
             for (const auto& event : outcome.events) {
                 if (std::holds_alternative<order_book::SelfTradePrevented>(event)) {
                     modes.insert(command.selfTradePrevention);
                 }
             }
             return modes.size() == 3;
         }},
        {"stop-cascade", "one command running a cascade of three stops",
         [](const ReferenceBook&, const Command&, const auto& outcome) {
             return std::count_if(outcome.events.begin(), outcome.events.end(), [](const auto& event) {
                        return std::holds_alternative<order_book::StopTriggered>(event);
                    }) >= 3;
         }},
        {"immediate", "a market order that trades and expires the rest",
         [](const ReferenceBook&, const Command& command, const auto& outcome) {
             return command.orderType == order_book::OrderType::MARKET && tradesIn(outcome) > 0 &&
                    std::holds_alternative<order_book::OrderExpired>(outcome.events.back());
         }},
        {"limits", "a level holding more than one Quantity in total",
         [](const ReferenceBook& model, const Command&, const auto&) {
             for (Side side : {Side::BUY, Side::SELL}) {
                 for (const order_book::reference::ModelLevel& level : model.levels(side)) {
                     if (level.totalQuantity > std::numeric_limits<order_book::Quantity>::max()) {
                         return true;
                     }
                 }
             }
             return false;
         }},
    };
    expect(targets.size() == workloadProfiles().size(), "every profile should have a target behavior");
    for (const ProfileTarget& target : targets) {
        const WorkloadProfile& profile = findProfile(target.profile);
        RandomCommandGenerator generator(1, profile);
        ReferenceBook model;
        bool hit = false;
        for (std::size_t i = 0; i < 2000 && !hit; ++i) {
            const Command command = generator.next(model);
            const order_book::reference::ExpectedOutcome outcome = applyCommand(model, command);
            hit = target.hit(model, command, outcome);
        }
        expect(hit, std::string("profile ") + target.profile + " should reach its target within 2000 commands: " +
                        target.behavior);
    }
}

// A fault planted as an extra property (no level may queue three orders) is caught at the
// right command, the recorder holds exactly the executed prefix, the prefix reproduces it,
// shrinking finds the three submissions that cause it, and the report says all of that.
void testPlantedFaultIsReportedPreservedAndShrunk() {
    const std::string context = "planted fault";
    const CommandReplayer replayer([](const OrderBook& book, const Command&) -> std::optional<std::string> {
        const order_book::BookSnapshot snapshot = book.snapshot(std::numeric_limits<unsigned int>::max());
        for (const auto* levels : {&snapshot.bids, &snapshot.asks}) {
            for (const order_book::PriceLevelSummary& level : *levels) {
                if (level.orderCount >= 3) {
                    return "the level at " + std::to_string(level.price) + " queues " +
                           std::to_string(level.orderCount) + " orders";
                }
            }
        }
        return std::nullopt;
    });
    const WorkloadProfile& profile = findProfile("balanced");
    const StreamHeader header{generatorVersion, profile.name, 1, describeProfile(profile), ""};
    CommandRecorder recorder(header);
    OrderBook book;
    RandomCommandGenerator generator(1, profile);
    const RunResult result = replayer.run(book, generator, 5000, recorder);

    expect(!result.passed(), context + ": the planted fault should be found");
    const Failure& failure = *result.failure;
    expect(failure.type == FailureType::PROPERTY_VIOLATION && !failure.violation,
           context + ": it should be reported as a property violation");
    expect(failure.commandIndex > 3, context + ": it should take a few commands to reach");
    expect(recorder.commands().size() == failure.commandIndex + 1 && recorder.commands().back() == failure.command,
           context + ": the recorder should stop at the failing command");
    expect(result.stats.commands == failure.commandIndex + 1, context + ": nothing should run after the failure");
    expect(contains(failure.reason, "queues 3 orders"), context + ": the reason should come from the property");
    expect(contains(failure.bookState, "bids") && contains(failure.bookState, "model"),
           context + ": the state should show the book and the model");

    OrderBook replayed;
    const RunResult again = replayer.replay(replayed, recorder.commands());
    expect(again.failure && again.failure->commandIndex == failure.commandIndex && again.failure->sameKindAs(failure),
           context + ": replaying the prefix should fail at the same command the same way");
    OrderBook beforeIt;
    const std::vector<Command> withoutLast(recorder.commands().begin(), recorder.commands().end() - 1);
    expect(replayer.replay(beforeIt, withoutLast).passed(), context + ": everything before it should pass");

    const ShrinkResult shrunk = shrinkFailingStream(recorder.commands(), failure, replayer);
    expect(shrunk.complete, context + ": shrinking should finish within its budget");
    expect(shrunk.commands.size() == 3, context + ": three submissions should be the whole story, got " +
                                            std::to_string(shrunk.commands.size()));
    for (const Command& command : shrunk.commands) {
        expect(command.type == CommandType::SUBMIT && command.side == shrunk.commands[0].side &&
                   command.price == shrunk.commands[0].price,
               context + ": they should be submissions to one level: " + formatCommand(command));
    }
    OrderBook minimal;
    const RunResult minimalRun = replayer.replay(minimal, shrunk.commands);
    expect(minimalRun.failure && minimalRun.failure->sameKindAs(failure) && minimalRun.failure->commandIndex == 2,
           context + ": the shrunk stream should fail the same way at its last command");
    for (std::size_t drop = 0; drop < shrunk.commands.size(); ++drop) {
        std::vector<Command> smaller = shrunk.commands;
        smaller.erase(smaller.begin() + static_cast<std::ptrdiff_t>(drop));
        OrderBook check;
        expect(replayer.replay(check, smaller).passed(), context + ": dropping any one command should lose it");
    }

    const std::string path = temporaryPath("planted.txt");
    CommandStream saved{header, shrunk.commands};
    saveStream(path, saved);
    OrderBook fromFile;
    const RunResult loaded = replayer.replay(fromFile, loadStream(path).commands);
    expect(loaded.failure && loaded.failure->sameKindAs(failure), context + ": the saved stream should still fail");
    std::filesystem::remove(path);

    const RunOrigin origin{"randomized_tests", 1, profile.name, describeProfile(profile), 5000, ""};
    PreservedStreams preserved;
    preserved.prefixPath = "out/prefix.txt";
    preserved.minimizedPath = "out/minimized.txt";
    preserved.shrunk = shrunk;
    const std::string report = formatFailureReport(origin, failure, recorder.commands(), preserved);
    const std::string count = std::to_string(failure.commandIndex + 1);
    const std::vector<std::string> expectedParts = {
        "RANDOMIZED TEST FAILURE",
        "Seed:          1",
        "Profile:       balanced (generator version " + std::to_string(generatorVersion) + ")",
        "Command index: " + std::to_string(failure.commandIndex) + " (0-based; " + count +
            " commands executed of 5000 planned)",
        "Command:       " + formatCommand(failure.command),
        "Failure:       PROPERTY_VIOLATION",
        "queues 3 orders",
        "   <-- failed",
        "Executed prefix:  out/prefix.txt (" + count + " commands)",
        "Minimized stream: out/minimized.txt (3 commands",
        "randomized_tests --replay out/minimized.txt",
        "randomized_tests --seed 1 --profile balanced --commands " + count};
    for (const std::string& part : expectedParts) {
        expect(contains(report, part), context + ": the report should contain '" + part + "':\n" + report);
    }
}

// The book is changed behind the model's back (by a submission the model never sees, which
// keeps the book itself sound) and the next command's comparison catches it: by its events
// when it touches the changed order, by the book's levels when it does not.
void testDivergenceFromTheModelIsCaught() {
    const std::string context = "divergence from the model";
    auto drainAfterSecond = [](const OrderBook& book, const Command& command) -> std::optional<std::string> {
        if (command.orderId == 2) {
            // The book is never const itself; the property just receives it read-only. Buys 3
            // of order 1, the oldest ask at 100.
            const_cast<OrderBook&>(book).submitOrder(Order(99, 100, 3, 1, Side::BUY));
        }
        return std::nullopt;
    };
    const CommandReplayer replayer(drainAfterSecond);
    const std::vector<Command> setUp = {Command::submit(1, Side::SELL, 100, 5),
                                        Command::submit(2, Side::SELL, 100, 5)};

    std::vector<Command> touching = setUp;
    touching.push_back(Command::cancel(1));
    OrderBook first;
    const RunResult outcome = replayer.replay(first, touching);
    expect(outcome.failure && outcome.failure->type == FailureType::OUTCOME_MISMATCH &&
               outcome.failure->commandIndex == 2,
           context + ": cancelling the drained order should report different events");
    expect(contains(outcome.failure->reason, "OrderCancelled id=1 qty=5") &&
               contains(outcome.failure->reason, "OrderCancelled id=1 qty=2"),
           context + ": the reason should show both events:\n" + outcome.failure->reason);

    std::vector<Command> elsewhere = setUp;
    elsewhere.push_back(Command::submit(3, Side::BUY, 90, 1));
    OrderBook second;
    const RunResult state = replayer.replay(second, elsewhere);
    expect(state.failure && state.failure->type == FailureType::BOOK_STATE_MISMATCH &&
               state.failure->commandIndex == 2,
           context + ": an unrelated command should still find the levels differ");
    expect(contains(state.failure->reason,
                    "asks level 0 at 100, queue position 0: the book has #1 open 2, the model #1 open 5"),
           context + ": the reason should name the order that differs:\n" + state.failure->reason);
}

// The model starts empty, so the book must too.
void testReplayNeedsAnEmptyBook() {
    OrderBook book;
    book.submitOrder(Order(1, 100, 5, 1, Side::BUY));
    expectThrows([&] { CommandReplayer().replay(book, {}); }, "replay should refuse a book that is not empty");
}

}  // namespace

int main() {
    testSameSeedSameStream();
    testGeneratorFingerprints();
    testRunRecordsTheGeneratedStream();
    testStreamRoundTrip();
    testDistributions();
    testProfilesExerciseTheBook();
    testProfilesHitTheirTargets();
    testPlantedFaultIsReportedPreservedAndShrunk();
    testDivergenceFromTheModelIsCaught();
    testReplayNeedsAnEmptyBook();

    std::cout << "randomized harness tests passed" << std::endl;
    return EXIT_SUCCESS;
}
