#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "reference_book.hpp"
#include "workload.hpp"

// The behavior workloads: requests, replaces, stops, and owners, generated against and
// predicted by ReferenceBook, the randomized harness's independent model, never an OrderBook.
// The random draws follow workload.cpp's rules: std::mt19937_64's raw output, an unbiased
// below() of our own, and one draw per statement.
namespace order_book::benchmark {

namespace {

using reference::ExpectedOutcome;
using reference::ModelOrder;
using reference::ReferenceBook;

constexpr OrderId maxOrderId = std::numeric_limits<OrderId>::max();
constexpr Price maxPrice = std::numeric_limits<Price>::max();

Price clampPrice(std::int64_t price) {
    return static_cast<Price>(std::clamp<std::int64_t>(price, 1, maxPrice));
}

// Counts what one command did, from the events the model predicts for it. Generated
// commands are never refused, so a refusal is a generator bug.
void count(const BehaviorCommand& command, const ExpectedOutcome& predicted, WorkloadOutcome& outcome,
           const std::string& workload) {
    if (!predicted.success) {
        throw std::logic_error("workload " + workload + ": the model refused a generated command");
    }
    switch (command.type) {
        case CommandType::SUBMIT: ++outcome.submits; break;
        case CommandType::CANCEL: ++outcome.cancels; break;
        case CommandType::REPLACE: ++outcome.replaces; break;
    }
    for (const BookEvent& event : predicted.events) {
        if (const auto* trade = std::get_if<TradeExecuted>(&event)) {
            ++outcome.trades;
            outcome.volume += trade->executionQuantity;
        } else if (std::holds_alternative<OrderExpired>(event)) {
            ++outcome.expirations;
        } else if (std::holds_alternative<SelfTradePrevented>(event)) {
            ++outcome.preventions;
        } else if (std::holds_alternative<StopTriggered>(event)) {
            ++outcome.stopsTriggered;
        }
    }
}

ExpectedOutcome apply(ReferenceBook& model, const BehaviorCommand& command) {
    switch (command.type) {
        case CommandType::SUBMIT: return model.submit(command.request);
        case CommandType::REPLACE: return model.replace(command.replacement);
        case CommandType::CANCEL: break;
    }
    return model.cancel(command.request.id);
}

BehaviorCommand cancelOf(OrderId orderId) {
    BehaviorCommand command;
    command.type = CommandType::CANCEL;
    command.request.id = orderId;
    return command;
}

class BehaviorGenerator {
public:
    BehaviorGenerator(const WorkloadConfig& workloadConfig, std::uint64_t seed)
        : config(workloadConfig), mix(workloadConfig.behavior), random(seed) {
        bidLevels = (config.targetLevels + 1) / 2;
        askLevels = config.targetLevels / 2;
        bandLow = std::int64_t{config.midPrice} - bidLevels;
        bandHigh = std::int64_t{config.midPrice} + askLevels;
    }

    // As workload.cpp's preload: order k at level k mod targetLevels, alternating sides.
    BehaviorCommand preloadOrder(std::size_t k) {
        const auto level = static_cast<Price>(k % config.targetLevels);
        const Side side = level % 2 == 0 ? Side::BUY : Side::SELL;
        const Price offset = level / 2 + 1;
        const Price price = side == Side::BUY ? config.midPrice - offset : config.midPrice + offset;
        BehaviorCommand command = submission(side, price, pickQuantity());
        command.request.postOnly = chance(mix.postOnlyPercent);
        drawOwnership(command.request);
        return command;
    }

    // Stop j of the preload, beyond the touch on alternating sides, spread over
    // preloadStopSpread ticks so no one trade reaches many of them.
    BehaviorCommand preloadStop(std::size_t j, const ReferenceBook& model) {
        const Side side = j % 2 == 0 ? Side::BUY : Side::SELL;
        const std::int64_t touch = touchFor(model, side);
        const auto distance = static_cast<std::int64_t>(1 + below(std::max<std::uint64_t>(mix.preloadStopSpread, 1)));
        return stopAt(side, clampPrice(side == Side::BUY ? touch + distance : touch - distance));
    }

    BehaviorCommand timedCommand(const ReferenceBook& model) {
        const std::uint64_t kind = below(100);
        const std::size_t pending = model.pendingCount();
        const bool stopsLow = mix.pendingFloor > 0 && pending < mix.pendingFloor;
        const bool stopsHigh = mix.pendingCap > 0 && pending > mix.pendingCap;
        std::uint64_t threshold = mix.submitPercent;
        if (kind < threshold) {
            return timedSubmission(model);
        }
        threshold += mix.replacePercent;
        if (kind < threshold) {
            return model.restingIds().empty() ? timedSubmission(model) : timedReplace(model);
        }
        threshold += mix.cancelPercent;
        if (kind < threshold) {
            // Below the floor, a cancel would shrink the book further: rest an order instead.
            if (model.restingIds().empty() || (mix.restingFloor > 0 && model.restingCount() < mix.restingFloor)) {
                return timedSubmission(model);
            }
            return cancelOf(model.restingIds().at(static_cast<std::size_t>(below(model.restingIds().size()))));
        }
        threshold += mix.stopPercent;
        const bool newStop = kind < threshold;
        if ((newStop && !stopsHigh) || (!newStop && (stopsLow || model.pendingIds().empty()))) {
            const Side side = chance(config.buyPercent) ? Side::BUY : Side::SELL;
            return timedStop(model, side);
        }
        return cancelOf(model.pendingIds().at(static_cast<std::size_t>(below(model.pendingIds().size()))));
    }

private:
    std::uint64_t below(std::uint64_t bound) {
        const std::uint64_t limit = (0 - bound) % bound;
        for (;;) {
            const std::uint64_t value = random();
            if (value >= limit) {
                return value % bound;
            }
        }
    }

    bool chance(unsigned int percent) { return below(100) < percent; }

    Quantity pickQuantity() {
        const std::uint64_t draw = below(100);
        QuantityClass quantityClass = QuantityClass::LARGE;
        if (draw < config.smallPercent) {
            quantityClass = QuantityClass::SMALL;
        } else if (draw < config.smallPercent + config.mediumPercent) {
            quantityClass = QuantityClass::MEDIUM;
        }
        const QuantityRange range = quantityRange(quantityClass);
        return range.low + static_cast<Quantity>(below(range.high - range.low + 1));
    }

    void drawOwnership(OrderRequest& request) {
        if (chance(mix.ownerPercent)) {
            const OwnerId pool = chance(mix.topOwnerPercent) ? mix.topOwners : mix.ownerPool;
            request.owner = static_cast<OwnerId>(1 + below(pool));
        }
        const std::uint64_t mode = below(100);
        request.selfTradePrevention = mode < mix.cancelRestingPercent ? SelfTradePrevention::CANCEL_RESTING
                                    : mode < mix.cancelRestingPercent + mix.cancelIncomingPercent
                                        ? SelfTradePrevention::CANCEL_INCOMING
                                        : SelfTradePrevention::CANCEL_BOTH;
    }

    BehaviorCommand submission(Side side, Price price, Quantity quantity) {
        if (nextId == maxOrderId) {
            throw std::length_error("workload " + config.name + " ran out of order IDs");
        }
        BehaviorCommand command;
        command.type = CommandType::SUBMIT;
        command.request = OrderRequest{++nextId, side, price, quantity};
        return command;
    }

    // The side's best, or the band's edge next to midPrice with that side empty.
    std::int64_t ownBest(const ReferenceBook& model, Side side) const {
        const std::optional<Price> best = model.best(side);
        if (best) {
            return *best;
        }
        return side == Side::BUY ? std::int64_t{config.midPrice} - 1 : std::int64_t{config.midPrice} + 1;
    }

    // Where a stop's distance is measured from: the last trade price, or before any trade the
    // opposing best (a buy stop waits above the asks), or midPrice.
    std::int64_t touchFor(const ReferenceBook& model, Side side) const {
        if (const std::optional<Price> last = model.lastTradePrice()) {
            return *last;
        }
        const std::optional<Price> opposing = model.best(side == Side::BUY ? Side::SELL : Side::BUY);
        return opposing ? std::int64_t{*opposing} : std::int64_t{config.midPrice};
    }

    // Uniform over its side of the band, or within concentratedTicks behind its own best, in
    // the v1 configuration's proportions; never at or through the opposing best.
    Price passivePrice(const ReferenceBook& model, Side side) {
        const bool buy = side == Side::BUY;
        const unsigned int restingModes = config.uniformPercent + config.concentratedPercent;
        const bool uniform = restingModes > 0 && below(restingModes) < config.uniformPercent;
        std::int64_t price = 0;
        if (uniform) {
            const auto step = static_cast<std::int64_t>(below(buy ? bidLevels : askLevels));
            price = buy ? std::int64_t{config.midPrice} - 1 - step : std::int64_t{config.midPrice} + 1 + step;
        } else {
            const auto behind = static_cast<std::int64_t>(below(std::uint64_t{config.concentratedTicks} + 1));
            price = buy ? ownBest(model, side) - behind : ownBest(model, side) + behind;
        }
        price = std::clamp(price, bandLow, bandHigh);
        const std::optional<Price> opposing = model.best(buy ? Side::SELL : Side::BUY);
        if (opposing) {
            price = buy ? std::min(price, std::int64_t{*opposing} - 1) : std::max(price, std::int64_t{*opposing} + 1);
        }
        return clampPrice(price);
    }

    // At the opposing best or up to `through` ticks past it; passive with that side empty.
    Price marketablePrice(const ReferenceBook& model, Side side, Price through) {
        const std::optional<Price> opposing = model.best(side == Side::BUY ? Side::SELL : Side::BUY);
        if (!opposing) {
            return passivePrice(model, side);
        }
        const auto past = static_cast<std::int64_t>(below(std::uint64_t{through} + 1));
        return clampPrice(side == Side::BUY ? std::int64_t{*opposing} + past : std::int64_t{*opposing} - past);
    }

    BehaviorCommand timedSubmission(const ReferenceBook& model) {
        const Side side = chance(config.buyPercent) ? Side::BUY : Side::SELL;
        const std::uint64_t roll = below(100);
        const std::size_t resting = model.restingCount();
        const bool restingLow = mix.restingFloor > 0 && resting < mix.restingFloor;
        const bool restingHigh = mix.restingCap > 0 && resting > mix.restingCap;
        std::uint64_t threshold = mix.passivePercent;
        // Outside the bounds, the drawn kind gives way: low, the order rests; high, it trades.
        const bool passive = restingLow || (!restingHigh && roll < threshold);
        const Quantity quantity = pickQuantity();
        if (passive) {
            BehaviorCommand command = submission(side, passivePrice(model, side), quantity);
            command.request.postOnly = chance(mix.postOnlyPercent);
            drawOwnership(command.request);
            return command;
        }
        threshold += mix.marketablePercent;
        if (roll < threshold || restingHigh) {
            BehaviorCommand command = submission(side, marketablePrice(model, side, config.throughTicks), quantity);
            drawOwnership(command.request);
            return command;
        }
        threshold += mix.iocPercent;
        if (roll < threshold) {
            BehaviorCommand command = submission(side, marketablePrice(model, side, config.throughTicks), quantity);
            command.request.timeInForce = TimeInForce::IOC;
            drawOwnership(command.request);
            return command;
        }
        threshold += mix.fokPercent;
        if (roll < threshold) {
            BehaviorCommand command = submission(side, marketablePrice(model, side, 0), quantity);
            command.request.timeInForce = TimeInForce::FOK;
            drawOwnership(command.request);
            if (chance(mix.unfillableFokPercent)) {
                // Past what its limit reaches, so it is accepted then wholly expired.
                const OrderRequest& request = command.request;
                const Quantity reachable =
                    model.plan(request.side, request.price, maxQuantity, request.owner, request.selfTradePrevention).filled;
                const std::uint64_t past = std::uint64_t{reachable} + 1 + below(10);
                command.request.quantity = static_cast<Quantity>(std::min<std::uint64_t>(past, maxQuantity));
            }
            return command;
        }
        threshold += mix.marketPercent;
        if (roll < threshold) {
            BehaviorCommand command = submission(side, 0, quantity);
            command.request.type = OrderType::MARKET;
            command.request.timeInForce = TimeInForce::IOC;
            drawOwnership(command.request);
            return command;
        }
        // A sweep: large, and far enough through to move the price across a cluster of stops.
        const QuantityRange large = quantityRange(QuantityClass::LARGE);
        const Quantity size = large.low + static_cast<Quantity>(below(large.high - large.low + 1));
        BehaviorCommand command = submission(side, marketablePrice(model, side, mix.sweepTicks), size);
        command.request.timeInForce = TimeInForce::IOC;
        drawOwnership(command.request);
        return command;
    }

    BehaviorCommand timedReplace(const ReferenceBook& model) {
        const ModelOrder& order = *model.find(model.restingIds().at(static_cast<std::size_t>(below(model.restingIds().size()))));
        const Quantity executed = order.total - order.open;
        const Side opposing = order.side == Side::BUY ? Side::SELL : Side::BUY;
        const std::uint64_t roll = below(100);
        BehaviorCommand command;
        command.type = CommandType::REPLACE;
        command.replacement = ReplaceRequest{order.id, order.price, order.total, false, order.owner,
                                             SelfTradePrevention::CANCEL_RESTING};
        const unsigned int decrease = mix.decreasePercent;
        const unsigned int reprice = decrease + mix.repricePercent;
        const unsigned int increase = reprice + mix.increasePercent;
        const unsigned int cross = increase + mix.crossPercent;
        // Below the floor, a replace that would take liquidity reprices instead.
        const bool restingLow = mix.restingFloor > 0 && model.restingCount() < mix.restingFloor;
        if (roll < decrease && order.open >= 2) {
            command.replacement.quantity = static_cast<Quantity>(executed + 1 + below(order.open - 1));
        } else if (roll < reprice || (roll < decrease) || (roll >= increase && (!model.best(opposing) || restingLow))) {
            command.replacement.price = passivePrice(model, order.side);
        } else if (roll < increase) {
            const std::uint64_t grown = std::uint64_t{order.total} + 1 + below(10);
            command.replacement.quantity = static_cast<Quantity>(std::min<std::uint64_t>(grown, maxQuantity));
        } else if (roll < cross) {
            command.replacement.price = *model.best(opposing);
        } else {
            command.replacement.price = *model.best(opposing);
            command.replacement.postOnly = true;
        }
        // The instructions are declared again: the same owner, a drawn mode.
        OrderRequest instructions{};
        drawOwnership(instructions);
        command.replacement.selfTradePrevention = instructions.selfTradePrevention;
        return command;
    }

    BehaviorCommand stopAt(Side side, Price stopPrice) {
        BehaviorCommand command = submission(side, 0, pickQuantity());
        OrderRequest& request = command.request;
        request.stopPrice = stopPrice;
        if (chance(mix.stopLimitPercent)) {
            request.type = OrderType::STOP_LIMIT;
            // A limit behind the stop price rests once triggered; one through it trades.
            const bool rests = chance(mix.restingLimitPercent);
            const std::int64_t toward = side == Side::BUY ? 1 : -1;
            const std::int64_t offset = rests ? -2 : std::int64_t{mix.stopReachTicks};
            request.price = clampPrice(std::int64_t{stopPrice} + toward * offset);
        } else {
            request.type = OrderType::STOP;
            request.timeInForce = TimeInForce::IOC;
        }
        drawOwnership(request);
        return command;
    }

    // 1 to stopReachTicks on the untriggered side of the last trade price.
    BehaviorCommand timedStop(const ReferenceBook& model, Side side) {
        const std::int64_t touch = touchFor(model, side);
        const auto distance = static_cast<std::int64_t>(1 + below(std::max<std::uint64_t>(mix.stopReachTicks, 1)));
        return stopAt(side, clampPrice(side == Side::BUY ? touch + distance : touch - distance));
    }

    static constexpr Quantity maxQuantity = std::numeric_limits<Quantity>::max();

    const WorkloadConfig& config;
    const BehaviorMix& mix;
    std::mt19937_64 random;
    OrderId nextId = 0;
    Price bidLevels = 0;
    Price askLevels = 0;
    std::int64_t bandLow = 0;
    std::int64_t bandHigh = 0;
};

void finish(Workload& workload, const ReferenceBook& model) {
    workload.expected.finalActiveOrders = model.restingCount();
    workload.expected.finalPriceLevels = model.levelCount();
    workload.expected.finalPendingStops = model.pendingCount();
}

// A v1 workload's exact stream, through the request path: owner 0, or a distinct nonzero owner
// per order (its ID), so nothing self-matches and only the owner probe differs.
Workload replayThroughRequests(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed) {
    // The configuration is a copy of that workload's, so a scaled copy replays a scaled stream.
    WorkloadConfig v1 = config;
    v1.family = WorkloadFamily::V1;
    v1.behavior = BehaviorMix{};
    const Workload original = generateWorkload(v1, timedCommands, seed);
    Workload workload;
    workload.config = config;
    workload.seed = seed;
    const auto convert = [&config](const BenchmarkCommand& command) {
        if (command.type == CommandType::CANCEL) {
            return cancelOf(command.orderId);
        }
        BehaviorCommand converted;
        converted.request = OrderRequest{command.orderId, command.side, command.price, command.quantity};
        converted.request.owner = config.behavior.ownerPerOrder ? command.orderId : 0;
        return converted;
    };
    ReferenceBook model;
    WorkloadOutcome preloadOutcome;
    for (const BenchmarkCommand& command : original.preload) {
        workload.behaviorPreload.push_back(convert(command));
        count(workload.behaviorPreload.back(), apply(model, workload.behaviorPreload.back()), preloadOutcome, config.name);
    }
    workload.preloadResting = model.restingCount();
    for (const BenchmarkCommand& command : original.timed) {
        workload.behaviorTimed.push_back(convert(command));
        count(workload.behaviorTimed.back(), apply(model, workload.behaviorTimed.back()), workload.expected, config.name);
    }
    finish(workload, model);
    // The two models must agree on the stream they both predict.
    WorkloadOutcome v1Fields = workload.expected;
    v1Fields.expirations = 0;
    if (v1Fields != original.expected || workload.expected.expirations != 0 || workload.expected.preventions != 0) {
        throw std::logic_error("workload " + config.name + ": the reference model and the workload model disagree");
    }
    return workload;
}

}  // namespace

Workload generateBehaviorWorkload(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed) {
    validateWorkloadConfig(config);
    if (!config.behavior.replaysWorkload.empty()) {
        return replayThroughRequests(config, timedCommands, seed);
    }
    if (config.initialOrders + config.behavior.preloadStops + timedCommands > maxOrderId) {
        throw std::length_error("workload " + config.name + " needs more order IDs than OrderId holds");
    }
    Workload workload;
    workload.config = config;
    workload.seed = seed;
    BehaviorGenerator generator(workload.config, seed);
    ReferenceBook model;

    WorkloadOutcome preloadOutcome;
    workload.behaviorPreload.reserve(config.initialOrders + config.behavior.preloadStops);
    for (std::size_t k = 0; k < config.initialOrders; ++k) {
        workload.behaviorPreload.push_back(generator.preloadOrder(k));
        count(workload.behaviorPreload.back(), apply(model, workload.behaviorPreload.back()), preloadOutcome, config.name);
    }
    for (std::size_t j = 0; j < config.behavior.preloadStops; ++j) {
        workload.behaviorPreload.push_back(generator.preloadStop(j, model));
        count(workload.behaviorPreload.back(), apply(model, workload.behaviorPreload.back()), preloadOutcome, config.name);
    }
    if (preloadOutcome.trades != 0 || preloadOutcome.expirations != 0 || model.levelCount() != config.targetLevels ||
        model.pendingCount() != config.behavior.preloadStops) {
        throw std::logic_error("workload " + config.name +
                               ": the preload should rest on exactly targetLevels levels, trade nothing, and pend every stop");
    }
    workload.preloadResting = model.restingCount();

    workload.behaviorTimed.reserve(timedCommands);
    for (std::size_t i = 0; i < timedCommands; ++i) {
        workload.behaviorTimed.push_back(generator.timedCommand(model));
        count(workload.behaviorTimed.back(), apply(model, workload.behaviorTimed.back()), workload.expected, config.name);
    }
    finish(workload, model);
    return workload;
}

}  // namespace order_book::benchmark
