#include "workload.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace order_book::benchmark {

namespace {

constexpr OrderId maxOrderId = std::numeric_limits<OrderId>::max();

// Nothing pulls a book back to its initial size: each command adds or removes about the
// same number of orders whatever the size, so a mix that is not balanced drifts steadily, and
// a balanced one wanders like a random walk. These mixes are tuned so each book keeps its
// intended shape at every size. At seed 42, from 10k to 1M timed commands: small-book holds
// 140-200 orders on about 10 levels (its cap does that), deep-book about 100k on 2000,
// matching-heavy 9k-12k on about 195, and cancellation-heavy about 50k on 500, while
// mostly-resting grows from 1000 to about 860k orders on its 100 levels, as intended.
// A cancellation removes an order a submission had to rest, so a book cannot sustain a
// cancellation rate above 50%. Cancellation-heavy sits at exactly that balance: none of its
// submissions trade, so each rests one order and each cancellation removes one. Deep-book's
// matching does part of its removal instead, so its 35% stays clearly below that.
std::vector<WorkloadConfig> makeWorkloads() {
    std::vector<WorkloadConfig> workloads;

    WorkloadConfig smallBook;
    smallBook.name = "small-book";
    smallBook.objective = "baseline overhead: about 10 levels and 100-200 small orders, a mix of every command";
    smallBook.initialOrders = 100;
    smallBook.targetLevels = 10;
    smallBook.cancelPercent = 35;
    smallBook.uniformPercent = 35;
    smallBook.concentratedPercent = 40;
    smallBook.marketablePercent = 25;
    smallBook.activeOrderCap = 200;
    workloads.push_back(smallBook);

    WorkloadConfig deepBook;
    deepBook.name = "deep-book";
    deepBook.objective = "large working set: about 100k resting orders over 2000 levels, inserts and cancels spread over the band";
    deepBook.initialOrders = 100000;
    deepBook.targetLevels = 2000;
    deepBook.cancelPercent = 35;
    deepBook.uniformPercent = 50;
    deepBook.concentratedPercent = 17;
    deepBook.marketablePercent = 33;
    deepBook.smallPercent = 40;
    deepBook.mediumPercent = 50;
    deepBook.largePercent = 10;
    workloads.push_back(deepBook);

    WorkloadConfig matchingHeavy;
    matchingHeavy.name = "matching-heavy";
    matchingHeavy.objective = "matching efficiency: most submissions marketable and medium to large, so they sweep orders";
    matchingHeavy.initialOrders = 10000;
    matchingHeavy.targetLevels = 200;
    matchingHeavy.cancelPercent = 8;
    matchingHeavy.uniformPercent = 10;
    matchingHeavy.concentratedPercent = 32;
    matchingHeavy.marketablePercent = 58;
    matchingHeavy.throughTicks = 2;
    matchingHeavy.smallPercent = 20;
    matchingHeavy.mediumPercent = 60;
    matchingHeavy.largePercent = 20;
    workloads.push_back(matchingHeavy);

    WorkloadConfig cancellationHeavy;
    cancellationHeavy.name = "cancellation-heavy";
    cancellationHeavy.objective = "order index and removal: half the commands cancel an order anywhere in a 50k-order book, none trade";
    cancellationHeavy.initialOrders = 50000;
    cancellationHeavy.targetLevels = 500;
    cancellationHeavy.cancelPercent = 50;
    cancellationHeavy.uniformPercent = 50;
    cancellationHeavy.concentratedPercent = 50;
    cancellationHeavy.marketablePercent = 0;
    cancellationHeavy.smallPercent = 50;
    cancellationHeavy.mediumPercent = 50;
    workloads.push_back(cancellationHeavy);

    WorkloadConfig mostlyResting;
    mostlyResting.name = "mostly-resting";
    mostlyResting.objective = "insertion and book growth: almost every command rests a new order";
    mostlyResting.initialOrders = 1000;
    mostlyResting.targetLevels = 100;
    mostlyResting.cancelPercent = 5;
    mostlyResting.uniformPercent = 80;
    mostlyResting.concentratedPercent = 18;
    mostlyResting.marketablePercent = 2;
    mostlyResting.smallPercent = 50;
    mostlyResting.mediumPercent = 40;
    mostlyResting.largePercent = 10;
    workloads.push_back(mostlyResting);

    return workloads;
}

// The behavior workloads. Each holds its shape at every size through its floor and cap on
// resting orders (and pending stops), rather than through a balanced mix alone; their realised
// shapes are in benchmarks/README.md's workload table.
std::vector<WorkloadConfig> makeBehaviorWorkloads(const std::vector<WorkloadConfig>& v1) {
    std::vector<WorkloadConfig> workloads;
    const auto behavior = [](std::string name, std::string objective) {
        WorkloadConfig config;
        config.name = std::move(name);
        config.objective = std::move(objective);
        config.family = WorkloadFamily::BEHAVIOR;
        return config;
    };

    WorkloadConfig quoteRefresh = behavior(
        "quote-refresh", "replace-heavy quoting: about 2,000 post-only quotes on 100 levels, 60% replaces, 25% submissions, 15% cancels");
    quoteRefresh.initialOrders = 2000;
    quoteRefresh.targetLevels = 100;
    quoteRefresh.uniformPercent = 30;
    quoteRefresh.concentratedPercent = 70;
    quoteRefresh.marketablePercent = 0;
    quoteRefresh.smallPercent = 60;
    quoteRefresh.mediumPercent = 40;
    quoteRefresh.behavior.submitPercent = 25;
    quoteRefresh.behavior.replacePercent = 60;
    quoteRefresh.behavior.cancelPercent = 15;
    quoteRefresh.behavior.passivePercent = 80;
    quoteRefresh.behavior.marketablePercent = 20;
    quoteRefresh.behavior.postOnlyPercent = 100;
    quoteRefresh.behavior.restingFloor = 1800;
    quoteRefresh.behavior.restingCap = 2200;
    workloads.push_back(quoteRefresh);

    WorkloadConfig pendingStops = behavior(
        "pending-stops", "stop triggering: 10,000 resting orders on 200 levels and about 5,000 pending stops, with cascades");
    pendingStops.initialOrders = 10000;
    pendingStops.targetLevels = 200;
    pendingStops.uniformPercent = 70;
    pendingStops.concentratedPercent = 30;
    pendingStops.marketablePercent = 0;
    pendingStops.throughTicks = 1;
    pendingStops.smallPercent = 40;
    pendingStops.mediumPercent = 50;
    pendingStops.largePercent = 10;
    pendingStops.behavior.submitPercent = 55;
    pendingStops.behavior.stopPercent = 20;
    pendingStops.behavior.cancelStopPercent = 15;
    pendingStops.behavior.cancelPercent = 10;
    pendingStops.behavior.passivePercent = 52;
    pendingStops.behavior.marketablePercent = 30;
    pendingStops.behavior.sweepPercent = 18;
    pendingStops.behavior.preloadStops = 5000;
    pendingStops.behavior.preloadStopSpread = 150;
    pendingStops.behavior.stopReachTicks = 30;
    pendingStops.behavior.sweepTicks = 4;
    pendingStops.behavior.restingFloor = 9000;
    pendingStops.behavior.restingCap = 11000;
    pendingStops.behavior.pendingFloor = 4500;
    pendingStops.behavior.pendingCap = 5500;
    workloads.push_back(pendingStops);

    WorkloadConfig selfTrade = behavior(
        "self-trade", "self-trade prevention: matching-heavy's mix with owners from a pool of 20, and FOKs that walk per order");
    selfTrade.initialOrders = 10000;
    selfTrade.targetLevels = 200;
    selfTrade.uniformPercent = 24;
    selfTrade.concentratedPercent = 76;
    selfTrade.marketablePercent = 0;
    selfTrade.throughTicks = 2;
    selfTrade.smallPercent = 20;
    selfTrade.mediumPercent = 60;
    selfTrade.largePercent = 20;
    selfTrade.behavior.submitPercent = 92;
    selfTrade.behavior.cancelPercent = 8;
    selfTrade.behavior.passivePercent = 42;
    selfTrade.behavior.marketablePercent = 49;
    selfTrade.behavior.fokPercent = 9;
    selfTrade.behavior.ownerPercent = 90;
    selfTrade.behavior.ownerPool = 20;
    selfTrade.behavior.topOwnerPercent = 70;
    selfTrade.behavior.topOwners = 2;
    selfTrade.behavior.cancelRestingPercent = 60;
    selfTrade.behavior.cancelIncomingPercent = 20;
    selfTrade.behavior.restingFloor = 8000;
    selfTrade.behavior.restingCap = 12000;
    workloads.push_back(selfTrade);

    WorkloadConfig immediate = behavior(
        "immediate", "immediate orders: IOC limits, market orders, and FOKs (a third unfillable) against a replenished 10,000-order book");
    immediate.initialOrders = 10000;
    immediate.targetLevels = 200;
    immediate.uniformPercent = 75;
    immediate.concentratedPercent = 25;
    immediate.marketablePercent = 0;
    immediate.throughTicks = 2;
    immediate.smallPercent = 50;
    immediate.mediumPercent = 45;
    immediate.largePercent = 5;
    immediate.behavior.submitPercent = 95;
    immediate.behavior.cancelPercent = 5;
    immediate.behavior.passivePercent = 42;
    immediate.behavior.iocPercent = 26;
    immediate.behavior.marketPercent = 16;
    immediate.behavior.fokPercent = 16;
    immediate.behavior.unfillableFokPercent = 33;
    immediate.behavior.restingFloor = 8000;
    immediate.behavior.restingCap = 12000;
    workloads.push_back(immediate);

    // matching-heavy's exact stream, through the request path: owner 0, then a distinct
    // nonzero owner per order, so comparing the two isolates the owner probe on each match.
    const WorkloadConfig& matchingHeavy = v1[2];
    WorkloadConfig request = matchingHeavy;
    request.name = "matching-heavy-request";
    request.objective = "matching-heavy's exact stream through submitOrder(OrderRequest), owner 0";
    request.family = WorkloadFamily::BEHAVIOR;
    request.behavior.replaysWorkload = matchingHeavy.name;
    workloads.push_back(request);

    WorkloadConfig owned = request;
    owned.name = "matching-heavy-owned";
    owned.objective = "matching-heavy's exact stream through submitOrder(OrderRequest), a distinct owner on every order";
    owned.behavior.ownerPerOrder = true;
    workloads.push_back(owned);

    return workloads;
}

// The book as the generator sees it: price-time priority matching, enough to know each
// side's best price, which orders rest, and what every command will do. Kept apart from
// OrderBook, so a workload never depends on the engine it measures.
class WorkloadModel {
public:
    bool hasBids() const { return !bids.empty(); }
    bool hasAsks() const { return !asks.empty(); }
    Price bestBid() const { return bids.begin()->first; }
    Price bestAsk() const { return asks.begin()->first; }

    std::size_t restingCount() const { return restingIds.size(); }
    OrderId restingIdAt(std::size_t position) const { return restingIds.at(position); }
    std::size_t levelCount() const { return bids.size() + asks.size(); }

    // Matches the order against the opposing side, oldest order at the best price first,
    // then rests what is left at its own price.
    void submit(const BenchmarkCommand& command, WorkloadOutcome& outcome) {
        ++outcome.submits;
        Quantity left = command.quantity;
        if (command.side == Side::BUY) {
            left = match(asks, left, [&](Price best) { return best <= command.price; }, outcome);
            if (left > 0) {
                rest(bids, command, left);
            }
        } else {
            left = match(bids, left, [&](Price best) { return best >= command.price; }, outcome);
            if (left > 0) {
                rest(asks, command, left);
            }
        }
    }

    void cancel(OrderId orderId, WorkloadOutcome& outcome) {
        ++outcome.cancels;
        const Resting order = unrest(orderId);
        if (order.side == Side::BUY) {
            leaveLevel(bids, order.price);
        } else {
            leaveLevel(asks, order.price);
        }
    }

private:
    // A level's queue holds IDs in arrival order. A cancelled order's ID stays in it until it
    // reaches the front, and is skipped there; live counts only orders still resting.
    struct Level {
        std::deque<OrderId> queue;
        std::size_t live = 0;
    };

    struct Resting {
        std::size_t position;  // in restingIds
        Quantity remaining;
        Side side;
        Price price;
    };

    template <typename Levels, typename Crosses>
    Quantity match(Levels& levels, Quantity left, Crosses crosses, WorkloadOutcome& outcome) {
        while (left > 0 && !levels.empty() && crosses(levels.begin()->first)) {
            Level& level = levels.begin()->second;
            const auto found = resting.find(level.queue.front());
            if (found == resting.end()) {
                level.queue.pop_front();
                continue;
            }
            const Quantity fill = std::min(left, found->second.remaining);
            left -= fill;
            found->second.remaining -= fill;
            ++outcome.trades;
            outcome.volume += fill;
            if (found->second.remaining == 0) {
                unrest(found->first);
                level.queue.pop_front();
                if (--level.live == 0) {
                    levels.erase(levels.begin());
                }
            }
        }
        return left;
    }

    template <typename Levels>
    void rest(Levels& levels, const BenchmarkCommand& command, Quantity left) {
        Level& level = levels[command.price];
        level.queue.push_back(command.orderId);
        ++level.live;
        resting.emplace(command.orderId, Resting{restingIds.size(), left, command.side, command.price});
        restingIds.push_back(command.orderId);
    }

    template <typename Levels>
    void leaveLevel(Levels& levels, Price price) {
        const auto level = levels.find(price);
        if (--level->second.live == 0) {
            levels.erase(level);
        }
    }

    // Forgets a resting order, moving the last resting ID into its place, and returns it.
    Resting unrest(OrderId orderId) {
        const auto found = resting.find(orderId);
        if (found == resting.end()) {
            throw std::logic_error("workload model: order " + std::to_string(orderId) + " is not resting");
        }
        const Resting order = found->second;
        const OrderId moved = restingIds.back();
        restingIds[order.position] = moved;
        resting.at(moved).position = order.position;
        restingIds.pop_back();
        resting.erase(orderId);
        return order;
    }

    std::map<Price, Level, std::greater<Price>> bids;
    std::map<Price, Level, std::less<Price>> asks;
    // Every resting order by ID, and the same IDs in a vector, so a cancellation can pick one
    // uniformly. Only the vector's order affects what is drawn, and it depends only on the
    // commands, never on hashing.
    std::unordered_map<OrderId, Resting> resting;
    std::vector<OrderId> restingIds;
};

class WorkloadGenerator {
public:
    WorkloadGenerator(const WorkloadConfig& workloadConfig, std::uint64_t seed)
        : config(workloadConfig), random(seed) {
        bidLevels = (config.targetLevels + 1) / 2;
        askLevels = config.targetLevels / 2;
        bandLow = config.midPrice - bidLevels;
        bandHigh = config.midPrice + askLevels;
    }

    // Order k rests at level k mod targetLevels: even levels are bids below midPrice, odd
    // levels asks above it, each one tick further out than the last of its side.
    BenchmarkCommand preloadOrder(std::size_t k) {
        const auto level = static_cast<Price>(k % config.targetLevels);
        const Side side = level % 2 == 0 ? Side::BUY : Side::SELL;
        const Price offset = level / 2 + 1;
        const Price price = side == Side::BUY ? config.midPrice - offset : config.midPrice + offset;
        const Quantity quantity = pickQuantity();
        return submit(side, price, quantity);
    }

    BenchmarkCommand timedCommand(const WorkloadModel& model) {
        const bool cancel = chance(config.cancelPercent);
        if (cancel && model.restingCount() > 0) {
            const std::size_t position = static_cast<std::size_t>(below(model.restingCount()));
            return BenchmarkCommand{CommandType::CANCEL, model.restingIdAt(position), Side::BUY, 0, 0};
        }
        const Side side = chance(config.buyPercent) ? Side::BUY : Side::SELL;
        const PriceMode drawn = pickPriceMode();
        const bool capped = config.activeOrderCap > 0 && model.restingCount() >= config.activeOrderCap;
        const PriceMode mode = capped ? PriceMode::MARKETABLE : drawn;
        const Quantity quantity = pickQuantity();
        const Price price = pickPrice(model, side, mode);
        return submit(side, price, quantity);
    }

private:
    // Uniform in [0, bound); bound must be positive. Rejects the lowest 2^64 mod bound
    // outputs so every result is equally likely.
    std::uint64_t below(std::uint64_t bound) {
        const std::uint64_t threshold = (0 - bound) % bound;
        for (;;) {
            const std::uint64_t value = random();
            if (value >= threshold) {
                return value % bound;
            }
        }
    }

    bool chance(unsigned int percent) { return below(100) < percent; }

    PriceMode pickPriceMode() {
        const std::uint64_t draw = below(100);
        if (draw < config.uniformPercent) {
            return PriceMode::UNIFORM;
        }
        if (draw < config.uniformPercent + config.concentratedPercent) {
            return PriceMode::CONCENTRATED;
        }
        return PriceMode::MARKETABLE;
    }

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

    Price pickPrice(const WorkloadModel& model, Side side, PriceMode mode) {
        const bool buy = side == Side::BUY;
        const bool opposingRests = buy ? model.hasAsks() : model.hasBids();
        if (mode == PriceMode::MARKETABLE && opposingRests) {
            const Price through = static_cast<Price>(below(std::uint64_t{config.throughTicks} + 1));
            return buy ? model.bestAsk() + through : model.bestBid() - std::min(through, model.bestBid() - 1);
        }

        // Signed, so a step past either end of the band clamps instead of wrapping.
        std::int64_t price = 0;
        if (mode == PriceMode::UNIFORM) {
            const Price levels = buy ? bidLevels : askLevels;
            const auto step = static_cast<std::int64_t>(below(levels));
            price = buy ? std::int64_t{config.midPrice} - 1 - step : std::int64_t{config.midPrice} + 1 + step;
        } else {
            // CONCENTRATED, or a MARKETABLE order with nothing to trade against.
            const auto behind = static_cast<std::int64_t>(below(std::uint64_t{config.concentratedTicks} + 1));
            const std::int64_t best = ownBest(model, side);
            price = buy ? best - behind : best + behind;
        }
        price = std::clamp<std::int64_t>(price, bandLow, bandHigh);
        // Never at or through the opposing best, so only MARKETABLE orders trade.
        if (buy && model.hasAsks()) {
            price = std::min<std::int64_t>(price, std::int64_t{model.bestAsk()} - 1);
        } else if (!buy && model.hasBids()) {
            price = std::max<std::int64_t>(price, std::int64_t{model.bestBid()} + 1);
        }
        return static_cast<Price>(std::clamp<std::int64_t>(price, 1, std::numeric_limits<Price>::max()));
    }

    // The side's best price or, with that side empty, the band's edge next to midPrice.
    std::int64_t ownBest(const WorkloadModel& model, Side side) const {
        if (side == Side::BUY) {
            return model.hasBids() ? model.bestBid() : std::int64_t{config.midPrice} - 1;
        }
        return model.hasAsks() ? model.bestAsk() : std::int64_t{config.midPrice} + 1;
    }

    // generateWorkload has already checked that every order's ID fits in OrderId.
    BenchmarkCommand submit(Side side, Price price, Quantity quantity) {
        return BenchmarkCommand{CommandType::SUBMIT, ++nextId, side, price, quantity};
    }

    const WorkloadConfig& config;
    std::mt19937_64 random;
    OrderId nextId = 0;
    Price bidLevels = 0;
    Price askLevels = 0;
    std::int64_t bandLow = 0;
    std::int64_t bandHigh = 0;
};

void apply(WorkloadModel& model, const BenchmarkCommand& command, WorkloadOutcome& outcome) {
    if (command.type == CommandType::SUBMIT) {
        model.submit(command, outcome);
    } else {
        model.cancel(command.orderId, outcome);
    }
}

template <typename Value>
void hashValue(std::uint64_t& hash, Value value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (int byte = 0; byte < 8; ++byte) {
        hash = (hash ^ (bits & 0xFF)) * 1099511628211ULL;
        bits >>= 8;
    }
}

void hashCommand(std::uint64_t& hash, const BenchmarkCommand& command) {
    hashValue(hash, command.type == CommandType::SUBMIT ? 0 : 1);
    hashValue(hash, command.orderId);
    hashValue(hash, command.side == Side::BUY ? 0 : 1);
    hashValue(hash, command.price);
    hashValue(hash, command.quantity);
}

void hashCommand(std::uint64_t& hash, const BehaviorCommand& command) {
    const OrderRequest& request = command.request;
    const ReplaceRequest& replacement = command.replacement;
    hashValue(hash, static_cast<int>(command.type));
    hashValue(hash, request.id);
    hashValue(hash, request.side == Side::BUY ? 0 : 1);
    hashValue(hash, request.price);
    hashValue(hash, request.quantity);
    hashValue(hash, static_cast<int>(request.type));
    hashValue(hash, static_cast<int>(request.timeInForce));
    hashValue(hash, request.stopPrice);
    hashValue(hash, request.postOnly ? 1 : 0);
    hashValue(hash, request.owner);
    hashValue(hash, static_cast<int>(request.selfTradePrevention));
    hashValue(hash, replacement.id);
    hashValue(hash, replacement.price);
    hashValue(hash, replacement.quantity);
    hashValue(hash, replacement.postOnly ? 1 : 0);
    hashValue(hash, replacement.owner);
    hashValue(hash, static_cast<int>(replacement.selfTradePrevention));
}

void hashV1Outcome(std::uint64_t& hash, const WorkloadOutcome& expected) {
    hashValue(hash, expected.submits);
    hashValue(hash, expected.cancels);
    hashValue(hash, expected.trades);
    hashValue(hash, expected.volume);
    hashValue(hash, expected.finalActiveOrders);
    hashValue(hash, expected.finalPriceLevels);
}

}  // namespace

std::size_t timedCommandCount(BenchmarkSize size) {
    switch (size) {
        case BenchmarkSize::SMOKE: return 10000;
        case BenchmarkSize::STANDARD: return 100000;
        case BenchmarkSize::EXTENDED: return 1000000;
    }
    throw std::invalid_argument("unknown benchmark size");
}

const char* benchmarkSizeName(BenchmarkSize size) {
    switch (size) {
        case BenchmarkSize::SMOKE: return "smoke";
        case BenchmarkSize::STANDARD: return "standard";
        case BenchmarkSize::EXTENDED: return "extended";
    }
    return "unknown";
}

BenchmarkSize parseBenchmarkSize(std::string_view name) {
    for (const BenchmarkSize size : {BenchmarkSize::SMOKE, BenchmarkSize::STANDARD, BenchmarkSize::EXTENDED}) {
        if (name == benchmarkSizeName(size)) {
            return size;
        }
    }
    throw std::invalid_argument("unknown size '" + std::string(name) + "' (known: smoke, standard, extended)");
}

QuantityRange quantityRange(QuantityClass quantityClass) {
    switch (quantityClass) {
        case QuantityClass::SMALL: return {1, 10};
        case QuantityClass::MEDIUM: return {11, 100};
        case QuantityClass::LARGE: return {101, 1000};
    }
    throw std::invalid_argument("unknown quantity class");
}

double WorkloadOutcome::cancellationRate() const {
    const std::size_t commands = submits + cancels;
    return commands == 0 ? 0.0 : static_cast<double>(cancels) / static_cast<double>(commands);
}

void validateWorkloadConfig(const WorkloadConfig& config) {
    const auto fail = [&](const std::string& problem) {
        throw std::invalid_argument("workload " + config.name + ": " + problem);
    };
    if (config.cancelPercent > 100 || config.buyPercent > 100) {
        fail("cancelPercent and buyPercent must be at most 100");
    }
    if (config.family == WorkloadFamily::BEHAVIOR && config.behavior.replaysWorkload.empty()) {
        const BehaviorMix& mix = config.behavior;
        if (mix.submitPercent + mix.replacePercent + mix.cancelPercent + mix.stopPercent + mix.cancelStopPercent != 100) {
            fail("the command mix must sum to 100%");
        }
        if (mix.passivePercent + mix.marketablePercent + mix.iocPercent + mix.fokPercent + mix.marketPercent +
                mix.sweepPercent != 100) {
            fail("the submission mix must sum to 100%");
        }
        if (mix.decreasePercent + mix.repricePercent + mix.increasePercent + mix.crossPercent + mix.takerPercent != 100) {
            fail("the replace mix must sum to 100%");
        }
        if (mix.ownerPool == 0 || mix.topOwners == 0 || mix.topOwners > mix.ownerPool ||
            mix.cancelRestingPercent + mix.cancelIncomingPercent > 100) {
            fail("owners need a pool holding the top owners, and the modes must sum to at most 100%");
        }
    }
    if (config.uniformPercent + config.concentratedPercent + config.marketablePercent != 100) {
        fail("the price modes must sum to 100%");
    }
    if (config.smallPercent + config.mediumPercent + config.largePercent != 100) {
        fail("the quantity classes must sum to 100%");
    }
    if (config.targetLevels < 2) {
        fail("targetLevels must be at least 2, so each side starts with a level");
    }
    if (config.initialOrders < config.targetLevels) {
        fail("initialOrders must be at least targetLevels, so every level starts with an order");
    }
    const std::uint64_t bidLevels = (std::uint64_t{config.targetLevels} + 1) / 2;
    const std::uint64_t askLevels = config.targetLevels / 2;
    if (config.midPrice <= bidLevels ||
        std::uint64_t{config.midPrice} + askLevels + config.throughTicks > std::numeric_limits<Price>::max()) {
        fail("the price band around midPrice must lie between 1 and the largest Price");
    }
}

const std::vector<WorkloadConfig>& builtInWorkloads() {
    static const std::vector<WorkloadConfig> workloads = makeWorkloads();
    return workloads;
}

const std::vector<WorkloadConfig>& behaviorWorkloads() {
    static const std::vector<WorkloadConfig> workloads = makeBehaviorWorkloads(builtInWorkloads());
    return workloads;
}

const WorkloadConfig& findWorkload(std::string_view name) {
    std::string known;
    for (const auto* family : {&builtInWorkloads(), &behaviorWorkloads()}) {
        for (const WorkloadConfig& config : *family) {
            if (config.name == name) {
                return config;
            }
            known += (known.empty() ? "" : ", ") + config.name;
        }
    }
    throw std::invalid_argument("unknown workload '" + std::string(name) + "' (known: " + known + ")");
}

Workload generateWorkload(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed) {
    if (config.family == WorkloadFamily::BEHAVIOR) {
        return generateBehaviorWorkload(config, timedCommands, seed);
    }
    validateWorkloadConfig(config);
    if (config.initialOrders + timedCommands > maxOrderId) {
        throw std::length_error("workload " + config.name + " needs more order IDs than OrderId holds");
    }

    Workload workload;
    workload.config = config;
    workload.seed = seed;
    WorkloadGenerator generator(workload.config, seed);
    WorkloadModel model;

    WorkloadOutcome preloadOutcome;
    workload.preload.reserve(config.initialOrders);
    for (std::size_t k = 0; k < config.initialOrders; ++k) {
        workload.preload.push_back(generator.preloadOrder(k));
        apply(model, workload.preload.back(), preloadOutcome);
    }
    if (preloadOutcome.trades != 0 || model.levelCount() != config.targetLevels) {
        throw std::logic_error("workload " + config.name + ": the preload should rest on exactly targetLevels levels");
    }

    workload.timed.reserve(timedCommands);
    for (std::size_t i = 0; i < timedCommands; ++i) {
        workload.timed.push_back(generator.timedCommand(model));
        apply(model, workload.timed.back(), workload.expected);
    }
    workload.expected.finalActiveOrders = model.restingCount();
    workload.expected.finalPriceLevels = model.levelCount();
    workload.preloadResting = workload.preload.size();
    return workload;
}

std::uint64_t workloadFingerprintV2(const Workload& workload) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const BenchmarkCommand& command : workload.preload) {
        hashCommand(hash, command);
    }
    for (const BenchmarkCommand& command : workload.timed) {
        hashCommand(hash, command);
    }
    hashV1Outcome(hash, workload.expected);
    return hash;
}

std::uint64_t workloadFingerprint(const Workload& workload) {
    std::uint64_t hash = workloadFingerprintV2(workload);
    for (const BehaviorCommand& command : workload.behaviorPreload) {
        hashCommand(hash, command);
    }
    for (const BehaviorCommand& command : workload.behaviorTimed) {
        hashCommand(hash, command);
    }
    const WorkloadOutcome& expected = workload.expected;
    hashValue(hash, expected.replaces);
    hashValue(hash, expected.expirations);
    hashValue(hash, expected.preventions);
    hashValue(hash, expected.stopsTriggered);
    hashValue(hash, expected.finalPendingStops);
    return hash;
}

}  // namespace order_book::benchmark
