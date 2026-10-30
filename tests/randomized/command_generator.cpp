#include "command_generator.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>

namespace order_book::randomized {

namespace {

constexpr std::int64_t maxPrice = std::numeric_limits<Price>::max();
constexpr OrderId maxOrderId = std::numeric_limits<OrderId>::max();

Price clampPrice(std::int64_t price) {
    return static_cast<Price>(std::clamp<std::int64_t>(price, 1, maxPrice));
}

std::vector<WorkloadProfile> makeProfiles() {
    std::vector<WorkloadProfile> profiles;

    WorkloadProfile balanced;
    balanced.name = "balanced";
    balanced.purpose = "the default mix: 70/30 submit/cancel, 20% invalid, prices near the touch, mostly small sizes";
    profiles.push_back(balanced);

    WorkloadProfile aggressive = balanced;
    aggressive.name = "aggressive";
    aggressive.purpose = "mostly marketable and often large, so orders sweep several levels and leave remainders";
    aggressive.submitPercent = 80;
    aggressive.marketablePercent = 60;
    aggressive.throughTicks = 10;
    aggressive.largeQuantityPercent = 20;
    aggressive.largeQuantityMax = 5000;
    profiles.push_back(aggressive);

    WorkloadProfile passive = balanced;
    passive.name = "passive";
    passive.purpose = "few marketable orders spread over a wide range, building a deep book of many levels";
    passive.submitPercent = 80;
    passive.marketablePercent = 5;
    passive.farPercent = 15;
    passive.nearTicks = 20;
    passive.farTicks = 2000;
    passive.marketPercent = 2;  // a market order sweeps the depth this profile is for
    passive.postOnlyPercent = 30;
    profiles.push_back(passive);

    WorkloadProfile cancelHeavy = balanced;
    cancelHeavy.name = "cancel-heavy";
    cancelHeavy.purpose = "more cancellations than submissions, churning levels in and out of the book";
    cancelHeavy.submitPercent = 45;
    cancelHeavy.marketablePercent = 15;
    profiles.push_back(cancelHeavy);

    WorkloadProfile queue = balanced;
    queue.name = "queue";
    queue.purpose = "prices packed into a few ticks at the bottom of the range, so long FIFO queues form at price 1";
    queue.submitPercent = 75;
    queue.marketablePercent = 15;
    queue.farPercent = 0;
    queue.throughTicks = 0;
    queue.nearTicks = 1;
    queue.startPrice = 2;
    profiles.push_back(queue);

    WorkloadProfile oneSided = balanced;
    oneSided.name = "one-sided";
    oneSided.purpose = "85% buys, so bids pile up while asks are repeatedly emptied and rebuilt";
    oneSided.buyPercent = 85;
    oneSided.marketablePercent = 30;
    profiles.push_back(oneSided);

    WorkloadProfile invalidHeavy = balanced;
    invalidHeavy.name = "invalid-heavy";
    invalidHeavy.purpose = "half the commands intentionally invalid, and frequent reuse of filled and cancelled IDs";
    invalidHeavy.invalidPercent = 50;
    invalidHeavy.reuseIdPercent = 30;
    profiles.push_back(invalidHeavy);

    WorkloadProfile limits = balanced;
    limits.name = "limits";
    limits.purpose = "prices, IDs, and quantities at the top of their range, so level totals exceed one Quantity";
    limits.startPrice = std::numeric_limits<Price>::max() - 2;
    limits.largeQuantityPercent = 30;
    limits.largeQuantityMax = std::numeric_limits<Quantity>::max();
    limits.firstOrderId = maxOrderId - 1000000;
    profiles.push_back(limits);

    WorkloadProfile selfTrade = balanced;
    selfTrade.name = "self-trade";
    selfTrade.purpose = "nearly every order owned, by one of two owners, and often marketable, so matches are prevented";
    selfTrade.ownerPercent = 95;
    selfTrade.ownerPool = 2;
    selfTrade.marketablePercent = 50;
    profiles.push_back(selfTrade);

    WorkloadProfile immediate = balanced;
    immediate.name = "immediate";
    immediate.purpose = "market, IOC, and FOK orders 60% of submissions, often large, so FOKs fail and remainders expire";
    immediate.marketPercent = 25;
    immediate.iocPercent = 27;
    immediate.fokPercent = 20;
    immediate.marketablePercent = 40;
    immediate.largeQuantityPercent = 30;
    immediate.largeQuantityMax = 60;
    profiles.push_back(immediate);

    return profiles;
}

}  // namespace

OrderRequest requestOf(const Command& command) {
    return OrderRequest{command.orderId, command.side, command.price, command.quantity, command.orderType,
                        command.timeInForce, command.postOnly, command.owner, command.selfTradePrevention};
}

ExpectedOutcome applyCommand(ReferenceBook& model, const Command& command) {
    if (command.type == CommandType::CANCEL) {
        return model.cancel(command.orderId);
    }
    if (command.overload == SubmitOverload::ORDER) {
        return model.submitOrder(requestOf(command), command.markedFilled);
    }
    return model.submit(requestOf(command));
}

std::string describeProfile(const WorkloadProfile& profile) {
    return "submit=" + std::to_string(profile.submitPercent) + "% invalid=" + std::to_string(profile.invalidPercent) +
           "% buy=" + std::to_string(profile.buyPercent) + "% reuse=" + std::to_string(profile.reuseIdPercent) +
           "% order-overload=" + std::to_string(profile.orderOverloadPercent) +
           "% ioc=" + std::to_string(profile.iocPercent) + "% fok=" + std::to_string(profile.fokPercent) +
           "% market=" + std::to_string(profile.marketPercent) + "% (ioc " + std::to_string(profile.marketIocPercent) +
           "%) post-only=" + std::to_string(profile.postOnlyPercent) + "% owner=" +
           std::to_string(profile.ownerPercent) + "% of " + std::to_string(profile.ownerPool) + " (resting " +
           std::to_string(profile.cancelRestingPercent) + "%, incoming " + std::to_string(profile.cancelIncomingPercent) +
           "%)" +
           " marketable=" + std::to_string(profile.marketablePercent) + "% far=" + std::to_string(profile.farPercent) +
           "% through=" + std::to_string(profile.throughTicks) + " near=" + std::to_string(profile.nearTicks) +
           " farTicks=" + std::to_string(profile.farTicks) + " start=" + std::to_string(profile.startPrice) +
           " small<=" + std::to_string(profile.smallQuantityMax) + " large=" +
           std::to_string(profile.largeQuantityPercent) + "%<=" + std::to_string(profile.largeQuantityMax) +
           " firstId=" + std::to_string(profile.firstOrderId);
}

const std::vector<WorkloadProfile>& workloadProfiles() {
    static const std::vector<WorkloadProfile> profiles = makeProfiles();
    return profiles;
}

const WorkloadProfile& findProfile(std::string_view name) {
    std::string known;
    for (const WorkloadProfile& profile : workloadProfiles()) {
        if (profile.name == name) {
            return profile;
        }
        known += (known.empty() ? "" : ", ") + profile.name;
    }
    throw std::invalid_argument("unknown profile '" + std::string(name) + "' (known: " + known + ")");
}

RandomCommandGenerator::RandomCommandGenerator(std::uint64_t seed, WorkloadProfile profile)
    : random(seed), config(std::move(profile)), nextFreshId(config.firstOrderId) {
    if (config.smallQuantityMax == 0 || config.largeQuantityMax < config.smallQuantityMax || config.firstOrderId == 0 ||
        config.ownerPool == 0) {
        throw std::invalid_argument("profile " + config.name + " cannot generate valid orders");
    }
}

std::uint64_t RandomCommandGenerator::below(std::uint64_t bound) {
    // Rejects the lowest 2^64 mod bound outputs so every result is equally likely.
    const std::uint64_t threshold = (0 - bound) % bound;
    for (;;) {
        const std::uint64_t value = random();
        if (value >= threshold) {
            return value % bound;
        }
    }
}

Command RandomCommandGenerator::next(const ReferenceBook& model) {
    const bool submit = chance(config.submitPercent);
    const bool invalid = chance(config.invalidPercent);
    return submit ? nextSubmit(model, invalid) : nextCancel(model, invalid);
}

Command RandomCommandGenerator::validSubmit(const ReferenceBook& model) {
    const IdPool& removed = model.removedIds();
    const bool reuse = !removed.empty() && chance(config.reuseIdPercent);
    const OrderId orderId = reuse ? removed.at(below(removed.size())) : freshId();
    const Side side = chance(config.buyPercent) ? Side::BUY : Side::SELL;
    const Price price = pickPrice(model, side);
    Command command = Command::submit(orderId, side, price, pickQuantity(),
                                      reuse ? CommandIntent::REUSED_ID : CommandIntent::NEW_ID);
    if (chance(config.orderOverloadPercent)) {
        command.overload = SubmitOverload::ORDER;
        return command;
    }
    if (chance(config.marketPercent)) {
        command.orderType = OrderType::MARKET;
        command.price = 0;
        command.timeInForce = chance(config.marketIocPercent) ? TimeInForce::IOC : TimeInForce::FOK;
        drawOwnership(command);
        return command;
    }
    const std::uint64_t timeInForce = below(100);
    if (timeInForce < config.iocPercent) {
        command.timeInForce = TimeInForce::IOC;
    } else if (timeInForce < config.iocPercent + config.fokPercent) {
        command.timeInForce = TimeInForce::FOK;
    } else if (chance(config.postOnlyPercent)) {
        command.postOnly = true;
    }
    drawOwnership(command);
    return command;
}

void RandomCommandGenerator::drawOwnership(Command& command) {
    if (chance(config.ownerPercent)) {
        command.owner = static_cast<OwnerId>(1 + below(config.ownerPool));
    }
    const std::uint64_t mode = below(100);
    command.selfTradePrevention = mode < config.cancelRestingPercent ? SelfTradePrevention::CANCEL_RESTING
                                : mode < config.cancelRestingPercent + config.cancelIncomingPercent
                                    ? SelfTradePrevention::CANCEL_INCOMING
                                    : SelfTradePrevention::CANCEL_BOTH;
}

// An otherwise ordinary new order with exactly one thing wrong, chosen among the faults
// possible now (a duplicate needs a resting order).
Command RandomCommandGenerator::nextSubmit(const ReferenceBook& model, bool invalid) {
    if (!invalid) {
        return validSubmit(model);
    }
    constexpr CommandIntent faults[] = {CommandIntent::ZERO_ORDER_ID,    CommandIntent::ZERO_PRICE,
                                        CommandIntent::ZERO_QUANTITY,    CommandIntent::INVALID_SIDE,
                                        CommandIntent::UNSUPPORTED_TYPE, CommandIntent::MARKED_FILLED,
                                        CommandIntent::INVALID_TIME_IN_FORCE, CommandIntent::MARKET_WITH_PRICE,
                                        CommandIntent::MARKET_GTC,       CommandIntent::TYPE_CANNOT_REST,
                                        CommandIntent::POST_ONLY_NOT_GTC_LIMIT, CommandIntent::INVALID_STP_MODE,
                                        CommandIntent::DUPLICATE_ACTIVE_ID};
    const IdPool& resting = model.restingIds();
    const std::size_t choices = std::size(faults) - (resting.empty() ? 1 : 0);
    const CommandIntent fault = faults[below(choices)];

    const Side side = chance(config.buyPercent) ? Side::BUY : Side::SELL;
    // Drawn in separate statements: as two arguments of one call their order would be up to
    // the compiler. Quantity first is the order the pinned fingerprints were recorded in.
    const Quantity quantity = pickQuantity();
    const Price price = pickPrice(model, side);
    Command command = Command::submit(0, side, price, quantity, fault);
    switch (fault) {
        case CommandIntent::ZERO_ORDER_ID:
            break;
        case CommandIntent::DUPLICATE_ACTIVE_ID:
            command.orderId = resting.at(below(resting.size()));
            break;
        default:
            command.orderId = freshId();
            break;
    }
    switch (fault) {
        case CommandIntent::ZERO_PRICE: command.price = 0; break;
        case CommandIntent::ZERO_QUANTITY: command.quantity = 0; break;
        case CommandIntent::INVALID_SIDE: command.side = static_cast<Side>(2 + below(6)); break;
        // Past every enumerator, including those still to come.
        case CommandIntent::UNSUPPORTED_TYPE: command.orderType = static_cast<OrderType>(4 + below(6)); break;
        case CommandIntent::MARKET_WITH_PRICE:
            command.orderType = OrderType::MARKET;
            command.timeInForce = TimeInForce::IOC;
            break;
        case CommandIntent::MARKET_GTC:
            command.orderType = OrderType::MARKET;
            command.price = 0;
            break;
        case CommandIntent::POST_ONLY_NOT_GTC_LIMIT: {
            command.postOnly = true;
            const std::uint64_t which = below(3);
            if (which == 0) {
                command.orderType = OrderType::MARKET;
                command.price = 0;
                command.timeInForce = TimeInForce::IOC;
            } else {
                command.timeInForce = which == 1 ? TimeInForce::IOC : TimeInForce::FOK;
            }
            break;
        }
        case CommandIntent::INVALID_STP_MODE:
            if (chance(config.ownerPercent)) {
                command.owner = static_cast<OwnerId>(1 + below(config.ownerPool));
            }
            command.selfTradePrevention = static_cast<SelfTradePrevention>(3 + below(6));
            break;
        // A type an Order refuses since it cannot rest, among the request's enumerators to come.
        case CommandIntent::TYPE_CANNOT_REST:
            command.orderType = static_cast<OrderType>(1 + below(3));
            command.overload = SubmitOverload::ORDER;
            break;
        case CommandIntent::MARKED_FILLED:
            command.markedFilled = true;
            command.overload = SubmitOverload::ORDER;
            break;
        // Past every enumerator, including those still to come.
        case CommandIntent::INVALID_TIME_IN_FORCE: command.timeInForce = static_cast<TimeInForce>(3 + below(6)); break;
        default: break;
    }
    return command;
}

Command RandomCommandGenerator::nextCancel(const ReferenceBook& model, bool invalid) {
    const IdPool& resting = model.restingIds();
    if (!invalid) {
        if (resting.empty()) {
            // Nothing rests to cancel; submit instead, so the command stays valid.
            return validSubmit(model);
        }
        return Command::cancel(resting.at(below(resting.size())), CommandIntent::CANCEL_ACTIVE);
    }

    const IdPool& removed = model.removedIds();
    constexpr CommandIntent faults[] = {CommandIntent::CANCEL_UNKNOWN, CommandIntent::CANCEL_ZERO_ID,
                                        CommandIntent::CANCEL_REMOVED};
    const std::size_t choices = std::size(faults) - (removed.empty() ? 1 : 0);
    const CommandIntent fault = faults[below(choices)];
    switch (fault) {
        case CommandIntent::CANCEL_UNKNOWN: {
            // At or past the next new ID, so never issued so far.
            const std::uint64_t unknown = std::min<std::uint64_t>(nextFreshId + below(1000), maxOrderId);
            return Command::cancel(static_cast<OrderId>(unknown), fault);
        }
        case CommandIntent::CANCEL_ZERO_ID:
            return Command::cancel(0, fault);
        default:
            return Command::cancel(removed.at(below(removed.size())), fault);
    }
}

Price RandomCommandGenerator::pickPrice(const ReferenceBook& model, Side side) {
    const Side opposingSide = side == Side::BUY ? Side::SELL : Side::BUY;
    const std::optional<Price> own = model.best(side);
    const std::optional<Price> opposing = model.best(opposingSide);
    // +1 is toward the opposing side: up for a buy, down for a sell.
    const std::int64_t toward = side == Side::BUY ? 1 : -1;
    const std::int64_t anchor = own        ? static_cast<std::int64_t>(*own)
                                : opposing ? static_cast<std::int64_t>(*opposing) - toward
                                           : static_cast<std::int64_t>(config.startPrice);

    const std::uint64_t roll = below(100);
    if (roll < config.marketablePercent) {
        if (opposing) {
            const auto through = static_cast<std::int64_t>(below(std::uint64_t{config.throughTicks} + 1));
            return clampPrice(static_cast<std::int64_t>(*opposing) + toward * through);
        }
        // Nothing to trade against; rest near the touch instead.
    } else if (roll < config.marketablePercent + config.farPercent) {
        const auto behind = static_cast<std::int64_t>(std::uint64_t{config.nearTicks} + 1 +
                                                      below(std::max<std::uint64_t>(config.farTicks, 1)));
        return clampPrice(anchor - toward * behind);
    }

    // From one tick inside the own best to nearTicks behind it, without crossing.
    const auto behind = static_cast<std::int64_t>(below(std::uint64_t{config.nearTicks} + 2));
    std::int64_t price = anchor + toward - toward * behind;
    if (opposing) {
        const std::int64_t lastPassive = static_cast<std::int64_t>(*opposing) - toward;
        price = side == Side::BUY ? std::min(price, lastPassive) : std::max(price, lastPassive);
    }
    return clampPrice(price);
}

Quantity RandomCommandGenerator::pickQuantity() {
    if (config.largeQuantityMax > config.smallQuantityMax && chance(config.largeQuantityPercent)) {
        const std::uint64_t span = std::uint64_t{config.largeQuantityMax} - config.smallQuantityMax;
        return static_cast<Quantity>(config.smallQuantityMax + 1 + below(span));
    }
    return static_cast<Quantity>(1 + below(config.smallQuantityMax));
}

OrderId RandomCommandGenerator::freshId() {
    if (nextFreshId > maxOrderId) {
        throw std::length_error("profile " + config.name + " ran out of new order IDs");
    }
    return static_cast<OrderId>(nextFreshId++);
}

std::vector<Command> generateCommands(std::uint64_t seed, const WorkloadProfile& profile, std::size_t count) {
    RandomCommandGenerator generator(seed, profile);
    ReferenceBook model;
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(generator.next(model));
        applyCommand(model, commands.back());
    }
    return commands;
}

}  // namespace order_book::randomized
