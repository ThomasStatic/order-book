#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "command.hpp"
#include "reference_book.hpp"

namespace order_book::randomized {

using reference::ExpectedOutcome;
using reference::IdPool;
using reference::ModelLevel;
using reference::ModelOrder;
using reference::ReferenceBook;

// The command's submission as a request, or as the fields of the Order it builds.
OrderRequest requestOf(const Command& command);
ReplaceRequest replaceRequestOf(const Command& command);

// Applies the command to the model through the overload it names, and returns what the engine
// should report for it.
ExpectedOutcome applyCommand(ReferenceBook& model, const Command& command);

// Bump whenever a change to the generator or a built-in profile would make some seed produce
// a different stream: what is drawn, in what order, or how a draw becomes a command. A seed is
// only a reproduction recipe together with this version and the profile; saved streams replay
// exactly whatever the version. randomized_harness_tests pins a fingerprint of each profile's
// output, so a change that forgets to bump this fails there.
inline constexpr unsigned int generatorVersion = 9;

// The shape of a generated workload. Percentages are out of 100.
struct WorkloadProfile {
    std::string name;
    std::string purpose;
    std::size_t defaultCommands = 5000;

    unsigned int submitPercent = 60;      // of commands
    unsigned int replacePercent = 15;     // of commands; the rest are cancellations
    unsigned int invalidPercent = 20;     // of commands, each intentionally invalid
    unsigned int buyPercent = 50;         // of submissions
    unsigned int reuseIdPercent = 10;     // of valid submissions, reusing a filled or cancelled ID
    unsigned int orderOverloadPercent = 10;  // of valid submissions, through submitOrder(Order)
    unsigned int iocPercent = 12;            // of valid request submissions, IOC rather than GTC
    unsigned int fokPercent = 8;             // of valid request submissions, FOK
    // Valid request types, out of 100: MARKET, then STOP, then STOP_LIMIT, the rest LIMIT.
    unsigned int marketPercent = 10;
    unsigned int stopPercent = 8;
    unsigned int stopLimitPercent = 12;
    unsigned int marketIocPercent = 60;      // of market and STOP orders, IOC; the rest FOK
    unsigned int stopLimitIocPercent = 20;   // of STOP_LIMIT orders, IOC; then FOK; the rest GTC
    unsigned int stopLimitFokPercent = 10;
    // A stop price is 1 to stopNearTicks on the untriggered side of the last trade price (above
    // for a buy, below for a sell), or, stopPastTriggerPercent of the time once something has
    // traded, at or past it, so it expires on arrival. Before the first trade the anchor is the
    // touch, or startPrice.
    Price stopNearTicks = 5;
    unsigned int stopPastTriggerPercent = 5;
    unsigned int cancelPendingPercent = 30;  // of valid cancels while stops pend, a pending stop
    unsigned int postOnlyPercent = 10;       // of valid GTC limit requests, post-only

    // Self-trade prevention, on every valid request: ownerPercent have an owner from
    // 1..ownerPool, the rest none. The mode is drawn every time, even with no owner, so that
    // owner 0 ignoring it is exercised: CANCEL_RESTING, CANCEL_INCOMING, then CANCEL_BOTH.
    unsigned int ownerPercent = 50;
    OwnerId ownerPool = 4;
    unsigned int cancelRestingPercent = 50;
    unsigned int cancelIncomingPercent = 25;

    // Prices, relative to the model's current best bid and ask. A marketable order is priced at
    // the opposing best or up to throughTicks through it; a far one sits nearTicks + 1 to
    // nearTicks + farTicks behind its own side's best; any other rests within one tick inside
    // its own best to nearTicks behind it, never crossing. With an empty side, its best is
    // taken as one tick inside the opposing best, or startPrice if both are empty.
    unsigned int marketablePercent = 25;
    unsigned int farPercent = 5;
    Price throughTicks = 3;
    Price nearTicks = 5;
    Price farTicks = 200;
    Price startPrice = 10000;

    // Quantities: mostly 1..smallQuantityMax, sometimes up to largeQuantityMax.
    Quantity smallQuantityMax = 10;
    unsigned int largeQuantityPercent = 5;
    Quantity largeQuantityMax = 1000;

    // New IDs count up from here.
    OrderId firstOrderId = 1;
};

// Every parameter on one line, for reports and saved streams.
std::string describeProfile(const WorkloadProfile& profile);

const std::vector<WorkloadProfile>& workloadProfiles();
// Throws std::invalid_argument, listing the known names, for an unknown one.
const WorkloadProfile& findProfile(std::string_view name);

// Draws commands from a seeded std::mt19937_64. Only the engine's raw output is used, never a
// standard distribution, whose results differ between standard libraries, so a seed gives the
// same stream on every platform. The generator reads only the reference model, never the book
// under test, so a stream does not depend on the engine's behaviour and can be produced
// without running it.
class RandomCommandGenerator {
public:
    RandomCommandGenerator(std::uint64_t seed, WorkloadProfile profile);

    // The next command, given the model's view of the book as it stands before that command.
    // The caller applies the command to the model before asking for another.
    Command next(const ReferenceBook& model);

    const WorkloadProfile& profile() const { return config; }

private:
    // Uniform in [0, bound); bound must be positive.
    std::uint64_t below(std::uint64_t bound);
    bool chance(unsigned int percent) { return below(100) < percent; }

    Command nextSubmit(const ReferenceBook& model, bool invalid);
    Command nextCancel(const ReferenceBook& model, bool invalid);
    Command nextReplace(const ReferenceBook& model, bool invalid);
    Command validSubmit(const ReferenceBook& model);
    // Draws a request's owner, then its mode.
    void drawOwnership(Command& command);
    Price pickStopPrice(const ReferenceBook& model, Side side);
    Price pickPrice(const ReferenceBook& model, Side side);
    Quantity pickQuantity();
    // Throws std::length_error once IDs would pass the largest OrderId.
    OrderId freshId();

    std::mt19937_64 random;
    WorkloadProfile config;
    std::uint64_t nextFreshId;
};

// The first `count` commands a seed produces, run against the model alone.
std::vector<Command> generateCommands(std::uint64_t seed, const WorkloadProfile& profile, std::size_t count);

}  // namespace order_book::randomized
