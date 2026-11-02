#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "command_generator.hpp"
#include "engine_harness.hpp"

namespace {

using namespace order_book::engine_testing;
using order_book::randomized::Command;
using order_book::randomized::CommandIntent;
using order_book::reference::Spread;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::vector<Command> generatedFor(const std::vector<EngineCommand>& stream, std::size_t instrument) {
    std::vector<Command> commands;
    for (const EngineCommand& line : stream) {
        if (line.instrument == instrument) {
            commands.push_back(line.generated);
        }
    }
    return commands;
}

EngineSeedConfig smallRun() {
    EngineSeedConfig config;
    config.seed = 7;
    config.instruments = 4;
    config.commands = 2000;
    return config;
}

// A one-instrument run is exactly the single-book harness's stream, so no pin can have moved.
void testOneInstrumentIsTodaysStream() {
    EngineSeedConfig config = smallRun();
    config.instruments = 1;
    config.shards = 1;
    config.profiles = {"balanced"};
    const std::vector<EngineCommand> stream = generateEngineStream(config);
    expect(generatedFor(stream, 0) ==
               order_book::randomized::generateCommands(config.seed, order_book::randomized::findProfile("balanced"), config.commands),
           "instrument 0 uses the run's seed and an unchanged generator");
}

// However the interleaver shares commands out, each instrument draws the same stream.
void testAnInstrumentsCommandsDependOnNoOther() {
    EngineSeedConfig uniform = smallRun();
    EngineSeedConfig skewed = smallRun();
    skewed.spread = Spread::SKEWED;
    const std::vector<EngineCommand> first = generateEngineStream(uniform);
    const std::vector<EngineCommand> second = generateEngineStream(skewed);
    for (std::size_t instrument = 0; instrument < uniform.instruments; ++instrument) {
        const std::vector<Command> left = generatedFor(first, instrument);
        const std::vector<Command> right = generatedFor(second, instrument);
        const std::size_t shared = std::min(left.size(), right.size());
        expect(shared > 0, "every instrument gets commands under both spreads");
        expect(std::equal(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(shared), right.begin()),
               "an instrument's commands do not depend on the interleaving");
    }
}

void testASeedFixesItsStream() {
    const EngineSeedConfig config = smallRun();
    const std::vector<EngineCommand> first = generateEngineStream(config);
    const std::vector<EngineCommand> again = generateEngineStream(config);
    expect(first.size() == config.commands && again.size() == config.commands, "every command is generated");
    for (std::size_t i = 0; i < first.size(); ++i) {
        expect(first[i].instrument == again[i].instrument && first[i].generated == again[i].generated && first[i].fault == again[i].fault,
               "a seed fixes the stream");
    }
}

void testConversion() {
    Command marked = Command::submit(1, order_book::Side::BUY, 100, 5, CommandIntent::MARKED_FILLED);
    marked.overload = order_book::randomized::SubmitOverload::ORDER;
    expect(!convertForEngine(marked), "no entry point can express a marked-filled order");
    Command typed = marked;
    typed.intent = CommandIntent::TYPE_CANNOT_REST;
    expect(!convertForEngine(typed), "or an Order of a type that can't rest");

    Command order = marked;
    order.intent = CommandIntent::NEW_ID;
    const std::optional<Command> converted = convertForEngine(order);
    expect(converted && converted->overload == order_book::randomized::SubmitOverload::REQUEST, "a valid Order becomes a request");
    expect(converted->timeInForce == order_book::TimeInForce::GTC && converted->orderType == order_book::OrderType::LIMIT,
           "its identical GTC limit request");

    Command ownerless = Command::submit(1, order_book::Side::BUY, 100, 5);
    ownerless.selfTradePrevention = order_book::SelfTradePrevention::CANCEL_BOTH;
    expect(convertForEngine(ownerless)->selfTradePrevention == order_book::SelfTradePrevention::CANCEL_RESTING,
           "no owner clears the mode, as the wire does");
    ownerless.owner = 3;
    expect(convertForEngine(ownerless)->selfTradePrevention == order_book::SelfTradePrevention::CANCEL_BOTH, "an owner keeps it");
}

void testARunPassesAndExercisesEverything() {
    const EngineRunResult result = runEngineSeed(smallRun());
    if (result.failure) {
        expect(false, "a clean run failed: " + result.failure->reason);
    }
    expect(result.stats.queued > 1000, "commands reach the shard");
    expect(result.stats.rejectedAtEntry > 0, "the entry check answers some itself");
    expect(result.stats.refused > 0, "bad symbols are refused");
    expect(result.stats.dropped > 0, "what no entry point expresses is dropped");
    expect(result.stats.shardGrowths > 0, "the shard's books really were too small");
}

void testEverySpreadAndShardCountPasses() {
    for (const Spread spread : {Spread::UNIFORM, Spread::SKEWED, Spread::ONE_HOT}) {
        for (const std::uint32_t shards : {1U, 2U, 4U}) {
            EngineSeedConfig config = smallRun();
            config.spread = spread;
            config.shards = shards;
            const EngineRunResult result = runEngineSeed(config);
            expect(result.passed(), "a run with " + std::to_string(shards) + " shards failed: " +
                                        (result.failure ? result.failure->reason : std::string()));
        }
    }
}

// The harness has to be able to fail: a book that drifts from its witness and model is caught.
void testADriftingBookIsCaught() {
    EngineSeedConfig config = smallRun();
    config.plantDivergenceBefore = 50;
    const EngineRunResult result = runEngineSeed(config);
    expect(!result.passed(), "a book with an order nobody else knows of fails the run");
    expect(result.failure->instrument == "I0", "and the failure names the instrument");
}

}  // namespace

int main() {
    testOneInstrumentIsTodaysStream();
    testAnInstrumentsCommandsDependOnNoOther();
    testASeedFixesItsStream();
    testConversion();
    testARunPassesAndExercisesEverything();
    testEverySpreadAndShardCountPasses();
    testADriftingBookIsCaught();
    std::cout << "engine harness tests passed" << std::endl;
    return EXIT_SUCCESS;
}
