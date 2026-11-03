#include "benchmark_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

#include "order_book/book_event.hpp"
#include "order_book/cancellation_result.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order.hpp"
#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/tcp.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include <future>
#include <optional>

#include "engine_process.hpp"
#include "interleaver.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/submission_result.hpp"

namespace order_book::benchmark {

namespace {

namespace replay = order_book::replay;

using Clock = std::chrono::steady_clock;

replay::Symbol benchmarkSymbol() {
    return *replay::Symbol::parse("BENCH");
}

std::int64_t nanosecondsBetween(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
}

std::vector<replay::ReplayRecord> recordsOf(const std::vector<BenchmarkCommand>& plain, const std::vector<BehaviorCommand>& behavior,
                                            replay::Symbol symbol) {
    std::vector<replay::ReplayRecord> records;
    records.reserve(plain.size() + behavior.size());
    const auto add = [&](replay::Command command) {
        const std::uint64_t sequence = records.size() + 1;
        records.push_back(replay::ReplayRecord{static_cast<std::size_t>(sequence + 1), sequence, std::move(command), symbol});
    };
    for (const BenchmarkCommand& command : plain) {
        if (command.type == CommandType::SUBMIT) {
            add(replay::AddCommand{command.orderId, command.side, command.price, command.quantity});
        } else {
            add(replay::CancelCommand{command.orderId});
        }
    }
    for (const BehaviorCommand& command : behavior) {
        if (command.type == CommandType::SUBMIT) {
            const OrderRequest& request = command.request;
            add(replay::AddCommand{request.id, request.side, request.price, request.quantity, request.type, request.timeInForce,
                                   request.stopPrice, request.postOnly, request.owner, request.selfTradePrevention});
        } else if (command.type == CommandType::REPLACE) {
            const ReplaceRequest& request = command.replacement;
            add(replay::ReplaceCommand{request.id, request.price, request.quantity, request.postOnly, request.owner,
                                       request.selfTradePrevention});
        } else {
            add(replay::CancelCommand{command.request.id});
        }
    }
    return records;
}

void addOutcome(WorkloadOutcome& total, const WorkloadOutcome& outcome) {
    total.submits += outcome.submits;
    total.cancels += outcome.cancels;
    total.rejections += outcome.rejections;
    total.trades += outcome.trades;
    total.volume += outcome.volume;
    total.finalActiveOrders += outcome.finalActiveOrders;
    total.finalPriceLevels += outcome.finalPriceLevels;
    total.replaces += outcome.replaces;
    total.expirations += outcome.expirations;
    total.preventions += outcome.preventions;
    total.stopsTriggered += outcome.stopsTriggered;
    total.finalPendingStops += outcome.finalPendingStops;
}

// settings.warmups runs, then settings.repetitions measured runs, warm-ups first.
template <typename Run>
std::vector<Measurement> repeated(const RunSettings& settings, Run run) {
    std::vector<Measurement> measurements;
    measurements.reserve(settings.warmups + settings.repetitions);
    for (std::size_t i = 0; i < settings.warmups + settings.repetitions; ++i) {
        Measurement measurement = run();
        measurement.warmup = i < settings.warmups;
        measurement.repetition = measurement.warmup ? i + 1 : i - settings.warmups + 1;
        measurements.push_back(std::move(measurement));
    }
    return measurements;
}

// Sends one command to the book and counts what it did. The sequence number passed to Order
// is a placeholder: the book assigns its own on acceptance.
void execute(OrderBook& book, const BenchmarkCommand& command, WorkloadOutcome& outcome) {
    if (command.type == CommandType::SUBMIT) {
        ++outcome.submits;
        const SubmissionResult result =
            book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side));
        if (result.getStatus() == SubmissionStatus::REJECTED) {
            ++outcome.rejections;
        }
        for (const Trade& trade : result.getGeneratedTrades()) {
            ++outcome.trades;
            outcome.volume += trade.getExecutionQuantity();
        }
    } else {
        ++outcome.cancels;
        if (!book.cancelOrder(command.orderId).isSuccess()) {
            ++outcome.rejections;
        }
    }
}

// The same for a behavior command, counting every event its command made (countEvents),
// the cascade's included.
void execute(OrderBook& book, const BehaviorCommand& command, WorkloadOutcome& outcome) {
    if (command.type == CommandType::SUBMIT) {
        ++outcome.submits;
        const SubmissionResult result = book.submitOrder(command.request);
        if (result.getStatus() == SubmissionStatus::REJECTED) {
            ++outcome.rejections;
        }
        countEvents(result.getEvents(), outcome);
    } else if (command.type == CommandType::REPLACE) {
        ++outcome.replaces;
        const ReplaceResult result = book.replaceOrder(command.replacement);
        if (!result.isSuccess()) {
            ++outcome.rejections;
        }
        countEvents(result.getEvents(), outcome);
    } else {
        ++outcome.cancels;
        if (!book.cancelOrder(command.request.id).isSuccess()) {
            ++outcome.rejections;
        }
    }
}

Measurement runCore(const Workload& workload) {
    Measurement measurement;
    OrderBook book;
    preloadBook(book, workload);

    const Clock::time_point start = Clock::now();
    for (const BenchmarkCommand& command : workload.timed) {
        execute(book, command, measurement.outcome);
    }
    for (const BehaviorCommand& command : workload.behaviorTimed) {
        execute(book, command, measurement.outcome);
    }
    const Clock::time_point stop = Clock::now();

    measurement.elapsedNanoseconds = nanosecondsBetween(start, stop);
    recordFinalBook(book, measurement.outcome);
    requireNoGrowth(book, workload);
    return measurement;
}

replay::ReplayInput readLogInMemory(std::istream& bytes) {
    replay::ReplayInput input;
    input.format = replay::ReplayFormat::V3;
    replay::CommandLogReader reader(bytes);
    while (true) {
        replay::ReadResult result = reader.next();
        if (replay::ReplayRecord* record = std::get_if<replay::ReplayRecord>(&result)) {
            input.records.push_back(*record);
        } else {
            if (std::holds_alternative<replay::ParseError>(result)) {
                input.error = replay::ReplayError{replay::ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt,
                                                  std::get<replay::ParseError>(result).message, std::nullopt};
            }
            return input;
        }
    }
}

Measurement runReplay(const Workload& workload, bool fromLog) {
    Measurement measurement;
    OrderBook book;
    preloadBook(book, workload);
    // Built outside the timed region: the text stands in for a file already read into memory.
    std::istringstream source(fromLog ? renderCommandLog(workload, benchmarkSymbol())
                              : workload.config.family == WorkloadFamily::V1 ? renderReplayCsv(workload.timed)
                                                                             : renderReplayCsv(workload.behaviorTimed));
    replay::ReplayOptions options;
    options.invariantValidation = replay::InvariantValidation::NONE;

    const Clock::time_point start = Clock::now();
    const replay::ReplayInput input = fromLog ? readLogInMemory(source) : replay::readReplayInput(source);
    const replay::ReplayOutcome outcome = replay::ReplayEngine::run(input.records, book, options);
    const replay::ReplayResult* result = std::get_if<replay::ReplayResult>(&outcome);
    const std::string summary = result != nullptr ? replay::formatReplaySummary(*result) : std::string();
    const Clock::time_point stop = Clock::now();

    measurement.elapsedNanoseconds = nanosecondsBetween(start, stop);
    if (input.error || result == nullptr || summary.empty()) {
        throw std::runtime_error("workload " + workload.config.name + ": the replay of its commands failed");
    }
    const replay::ReplayStatistics& statistics = result->statistics;
    measurement.outcome.submits = statistics.addCommands;
    measurement.outcome.cancels = statistics.cancelCommands;
    measurement.outcome.replaces = statistics.replaceCommands;
    measurement.outcome.rejections = statistics.addsRejected + statistics.cancelsRejected + statistics.replacesRejected;
    measurement.outcome.trades = statistics.tradesExecuted;
    measurement.outcome.volume = statistics.volumeTraded;
    measurement.outcome.expirations = statistics.ordersExpired;
    measurement.outcome.preventions = statistics.selfTradesPrevented;
    measurement.outcome.stopsTriggered = statistics.stopsTriggered;
    measurement.outcome.finalActiveOrders = result->finalBook.totalActiveVisibleOrders;
    measurement.outcome.finalPriceLevels = result->finalBook.bids.size() + result->finalBook.asks.size();
    measurement.outcome.finalPendingStops = result->pendingStops;
    requireNoGrowth(book, workload);
    return measurement;
}

}  // namespace

// One event's part of a WorkloadOutcome.
void countResponse(const BookEvent& event, WorkloadOutcome& outcome) {
    if (std::holds_alternative<OrderRejected>(event) || std::holds_alternative<CancelRejected>(event) ||
        std::holds_alternative<ReplaceRejected>(event)) {
        ++outcome.rejections;
    }
    countEvents(std::span<const BookEvent>(&event, 1), outcome);
}

// One command's part of a WorkloadOutcome, from its events alone.
void countCommand(const replay::Command& command, std::span<const BookEvent> events, WorkloadOutcome& outcome) {
    if (std::holds_alternative<replay::AddCommand>(command)) {
        ++outcome.submits;
    } else if (std::holds_alternative<replay::CancelCommand>(command)) {
        ++outcome.cancels;
    } else {
        ++outcome.replaces;
    }
    for (const BookEvent& event : events) {
        countResponse(event, outcome);
    }
}

replay::Symbol instrumentSymbol(std::size_t instrument) {
    return *replay::Symbol::parse("B" + std::to_string(instrument));
}

BookCapacity expectedBookSize(const Workload& workload) {
    return BookCapacity{workload.peak.orders, workload.peak.stops, workload.peak.levels + 1};
}

void requireNoGrowth(const OrderBook& book, const Workload& workload) {
    if (book.capacityGrowths() != 0) {
        throw std::runtime_error("workload " + workload.config.name + ": the book grew past its capacity " +
                                 std::to_string(book.capacityGrowths()) + " times");
    }
}

void preloadBook(OrderBook& book, const Workload& workload) {
    book.reserve(expectedBookSize(workload));
    WorkloadOutcome preloadOutcome;
    for (const BenchmarkCommand& command : workload.preload) {
        execute(book, command, preloadOutcome);
    }
    for (const BehaviorCommand& command : workload.behaviorPreload) {
        execute(book, command, preloadOutcome);
    }
    if (preloadOutcome.rejections != 0 || preloadOutcome.trades != 0) {
        throw std::runtime_error("workload " + workload.config.name + ": the book refused or traded its preload");
    }
}

void recordFinalBook(const OrderBook& book, WorkloadOutcome& outcome) {
    const BookSnapshot snapshot = book.snapshot(std::numeric_limits<unsigned int>::max());
    outcome.finalActiveOrders = book.activeOrderCount();
    outcome.finalPriceLevels = snapshot.bids.size() + snapshot.asks.size();
    outcome.finalPendingStops = book.pendingStopCount();
}

std::string describeOutcome(const WorkloadOutcome& outcome) {
    return "submits=" + std::to_string(outcome.submits) + " cancels=" + std::to_string(outcome.cancels) +
           " replaces=" + std::to_string(outcome.replaces) + " rejections=" + std::to_string(outcome.rejections) +
           " trades=" + std::to_string(outcome.trades) + " volume=" + std::to_string(outcome.volume) +
           " expirations=" + std::to_string(outcome.expirations) + " preventions=" + std::to_string(outcome.preventions) +
           " stops-triggered=" + std::to_string(outcome.stopsTriggered) + " active=" +
           std::to_string(outcome.finalActiveOrders) + " levels=" + std::to_string(outcome.finalPriceLevels) +
           " pending=" + std::to_string(outcome.finalPendingStops);
}

const char* benchmarkKindName(BenchmarkKind kind) {
    switch (kind) {
        case BenchmarkKind::CORE: return "core";
        case BenchmarkKind::REPLAY: return "replay";
        case BenchmarkKind::REPLAY_LOG: return "replay-log";
        case BenchmarkKind::SHARD: return "shard";
        case BenchmarkKind::QUEUE: return "queue";
        case BenchmarkKind::GATEWAY: return "gateway";
        case BenchmarkKind::ENGINE: return "engine";
    }
    return "unknown";
}

BenchmarkKind parseBenchmarkKind(std::string_view name) {
    for (const BenchmarkKind kind : {BenchmarkKind::CORE, BenchmarkKind::REPLAY, BenchmarkKind::REPLAY_LOG, BenchmarkKind::SHARD,
                                     BenchmarkKind::QUEUE, BenchmarkKind::GATEWAY, BenchmarkKind::ENGINE}) {
        if (name == benchmarkKindName(kind)) {
            return kind;
        }
    }
    throw std::invalid_argument("unknown kind '" + std::string(name) + "' (known: core, replay, replay-log, shard, queue, gateway, engine)");
}

double operationsPerSecond(std::size_t commands, std::int64_t elapsedNanoseconds) {
    if (elapsedNanoseconds <= 0) {
        return 0.0;
    }
    return static_cast<double>(commands) * 1e9 / static_cast<double>(elapsedNanoseconds);
}

double Measurement::operationsPerSecond() const {
    return benchmark::operationsPerSecond(commands, elapsedNanoseconds);
}

double Measurement::nanosecondsPerCommand() const {
    return commands == 0 ? 0.0 : static_cast<double>(elapsedNanoseconds) / static_cast<double>(commands);
}

std::vector<Workload> instrumentWorkloads(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed,
                                          std::size_t instruments) {
    if (instruments == 0 || timedCommands < instruments) {
        throw std::invalid_argument("each of " + std::to_string(instruments) + " instruments needs a command");
    }
    std::vector<Workload> workloads;
    workloads.reserve(instruments);
    for (std::size_t i = 0; i < instruments; ++i) {
        workloads.push_back(generateWorkload(config, timedCommands / instruments, seed + i));
    }
    return workloads;
}

WorkloadOutcome totalOutcome(const std::vector<Workload>& workloads) {
    WorkloadOutcome total;
    for (const Workload& workload : workloads) {
        addOutcome(total, workload.expected);
    }
    return total;
}

struct ShardRun::Book {
    runtime::BookLocation location;
    std::vector<replay::ReplayRecord> commands;
    std::size_t next = 0;
    WorkloadOutcome counted;
};

ShardRun::~ShardRun() = default;

ShardRun::ShardRun(const std::vector<Workload>& runWorkloads) : workloads(&runWorkloads) {
    std::vector<runtime::Instrument> instruments;
    for (std::size_t i = 0; i < runWorkloads.size(); ++i) {
        const BookCapacity capacity = expectedBookSize(runWorkloads[i]);
        instruments.push_back(runtime::Instrument{instrumentSymbol(i), 0, capacity.orders, capacity.stops, capacity.levels});
    }
    directory = std::make_unique<runtime::InstrumentDirectory>(instruments, 1);
    shard = std::make_unique<runtime::Shard>(*directory, 0);

    books.reserve(runWorkloads.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < runWorkloads.size(); ++i) {
        const replay::Symbol symbol = instrumentSymbol(i);
        const runtime::BookLocation location = *directory->find(symbol);
        WorkloadOutcome preloaded;
        for (const replay::ReplayRecord& record : preloadRecords(runWorkloads[i], symbol)) {
            shard->process(record.command, location.slot);
            countCommand(record.command, shard->result().events(), preloaded);
        }
        if (preloaded.rejections != 0 || preloaded.trades != 0) {
            throw std::runtime_error("workload " + runWorkloads[i].config.name + ": the book refused or traded its preload");
        }
        books.push_back(Book{location, workloadRecords(runWorkloads[i], symbol), 0, WorkloadOutcome{}});
        total += books.back().commands.size();
    }

    // Each book's commands stay in order, whichever it draws: only the sharing is random.
    std::vector<std::size_t> remaining;
    for (const Book& book : books) {
        remaining.push_back(book.commands.size());
    }
    reference::Interleaver interleaver(runWorkloads.front().seed, books.size(), reference::Spread::UNIFORM);
    order.reserve(total);
    while (order.size() < total) {
        const std::size_t drawn = interleaver.next();
        if (remaining[drawn] != 0) {
            --remaining[drawn];
            order.push_back(static_cast<std::uint32_t>(drawn));
        }
    }
}

void ShardRun::run() {
    for (const std::uint32_t index : order) {
        Book& book = books[index];
        const replay::Command& command = book.commands[book.next++].command;
        shard->process(command, book.location.slot);
        countCommand(command, shard->result().events(), book.counted);
    }
}

std::uint32_t ShardRun::slotOf(std::uint32_t book) const {
    return books[book].location.slot;
}

const replay::Command& ShardRun::commandOf(std::uint32_t book, std::size_t index) const {
    return books[book].commands[index].command;
}

std::vector<WorkloadOutcome> ShardRun::outcomes() const {
    std::vector<WorkloadOutcome> counted;
    counted.reserve(books.size());
    for (const Book& book : books) {
        WorkloadOutcome outcome = book.counted;
        recordFinalBook(shard->books().book(book.location.slot), outcome);
        counted.push_back(outcome);
    }
    return counted;
}

void requireOutcomes(const std::vector<WorkloadOutcome>& outcomes, const std::vector<Workload>& workloads, BenchmarkKind kind) {
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (outcomes[i] != workloads[i].expected) {
            throw std::runtime_error("workload " + workloads[i].config.name + " instrument " + std::to_string(i) + " (" +
                                     benchmarkKindName(kind) + "): the book did " + describeOutcome(outcomes[i]) +
                                     " but the workload predicted " + describeOutcome(workloads[i].expected));
        }
    }
}

void ShardRun::check() const {
    for (std::size_t i = 0; i < books.size(); ++i) {
        requireNoGrowth(shard->books().book(books[i].location.slot), (*workloads)[i]);
    }
    requireOutcomes(outcomes(), *workloads, BenchmarkKind::SHARD);
}

MatchingThread::MatchingThread(const std::vector<Workload>& runWorkloads, runtime::Doorbell& doorbell,
                               std::vector<runtime::RingPair> rings, std::function<void()> setUp)
    : workloads(&runWorkloads) {
    std::promise<void> ready;
    std::future<void> started = ready.get_future();
    thread = std::thread([this, &ready, &doorbell, rings = std::move(rings), setUp = std::move(setUp)] {
        // Everything a book's owner must do happens here, its build and destruction included.
        // Whatever can fail before the loop runs fails here, while the constructor is still
        // waiting to hear: once the loop runs, it always sends SHARD_STOPPED.
        std::optional<ShardRun> built;
        std::optional<runtime::MatchingLoop> loop;
        try {
            if (setUp) {
                setUp();
            }
            built.emplace(*workloads);
            loop.emplace(built->onThisThread(), doorbell, rings);
        } catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        run = &*built;
        ready.set_value();
        try {
            loop->run();
            finalShapes = built->outcomes();
            for (std::size_t i = 0; i < workloads->size(); ++i) {
                requireNoGrowth(built->onThisThread().books().book(built->slotOf(static_cast<std::uint32_t>(i))), (*workloads)[i]);
            }
        } catch (...) {
            failure = std::current_exception();
        }
    });
    try {
        started.get();
    } catch (...) {
        thread.join();
        throw;
    }
}

MatchingThread::~MatchingThread() {
    if (thread.joinable()) {
        thread.join();
    }
}

void MatchingThread::join() {
    thread.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

std::vector<WorkloadOutcome> MatchingThread::withFinalShapes(std::vector<WorkloadOutcome> counted) const {
    for (std::size_t i = 0; i < counted.size(); ++i) {
        counted[i].finalActiveOrders = finalShapes[i].finalActiveOrders;
        counted[i].finalPriceLevels = finalShapes[i].finalPriceLevels;
        counted[i].finalPendingStops = finalShapes[i].finalPendingStops;
    }
    return counted;
}

QueueRun::QueueRun(const std::vector<Workload>& runWorkloads, std::function<void()> setUpMatchingThread)
    : workloads(&runWorkloads),
      commands(runtime::defaultCommandRingSlots),
      responses(runtime::defaultResponseRingSlots),
      nextCommand(runWorkloads.size(), 0),
      counted(runWorkloads.size()),
      matching(runWorkloads, doorbell, {runtime::RingPair{&commands, &responses}}, std::move(setUpMatchingThread)) {}

QueueRun::~QueueRun() {
    if (matching.joinable()) {
        try {
            finish();
        } catch (const std::exception&) {  // a destructor can't report it; check would have
        }
    }
}

runtime::CommandElement QueueRun::elementFor(std::uint32_t book) {
    // Its correlation names the book, which is all a producer has to match a response to.
    return runtime::CommandElement{book, matching.books().commandOf(book, nextCommand[book]), matching.books().slotOf(book), 0,
                                   runtime::ControlKind::COMMAND};
}

bool QueueRun::trySend(const runtime::CommandElement& element) {
    if (!commands.tryPush(element)) {
        return false;
    }
    const auto book = static_cast<std::uint32_t>(element.correlation);
    countCommand(element.command, {}, counted[book]);
    ++nextCommand[book];
    ++sent;
    return true;
}

void QueueRun::drainResponses() {
    responses.drain(runtime::defaultResponseRingSlots, [this](const runtime::ResponseRecord& record) {
        if (record.kind == runtime::ResponseKind::SHARD_STOPPED) {
            stopReceived = true;
            return;
        }
        countResponse(record.event, counted[static_cast<std::size_t>(record.correlation)]);
        if (record.kind == runtime::ResponseKind::LAST_EVENT) {
            ++answered;
        }
    });
}

void QueueRun::run() {
    const std::vector<std::uint32_t>& sharing = matching.books().sharing();
    while (answered < sharing.size()) {
        std::size_t batch = 0;
        while (batch < runtime::takeBatch && sent < sharing.size() && trySend(elementFor(sharing[sent]))) {
            ++batch;
        }
        if (batch != 0) {
            doorbell.ring();
        }
        drainResponses();
    }
}

void QueueRun::prepare(std::size_t index) {
    held = elementFor(matching.books().sharing()[index]);
}

void QueueRun::sendPrepared() {
    while (!trySend(*held)) {
    }
    doorbell.ring();
}

void QueueRun::awaitResponse() {
    while (answered < sent) {
        drainResponses();
    }
}

void QueueRun::finish() {
    while (!commands.tryPush(runtime::CommandElement::stopMarker())) {
        drainResponses();
    }
    doorbell.ring();
    while (!stopReceived) {
        drainResponses();
    }
    matching.join();
}

std::vector<WorkloadOutcome> QueueRun::outcomes() const {
    return matching.withFinalShapes(counted);
}

void QueueRun::check() const {
    requireOutcomes(outcomes(), *workloads, BenchmarkKind::QUEUE);
}


void sendAll(const network::TcpSocket& socket, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const std::optional<std::size_t> taken = socket.send(bytes);
        if (!taken) {
            throw std::runtime_error("loopback: a connection closed while it was being sent to");
        }
        bytes = bytes.subspan(*taken);
    }
}

std::vector<std::byte> commandFrames(const std::vector<replay::ReplayRecord>& records, replay::Symbol symbol) {
    std::vector<std::byte> bytes(records.size() * commandFrameBytes);
    for (std::size_t i = 0; i < records.size(); ++i) {
        network::encodeCommand(i, symbol, records[i].command, std::span<std::byte>(bytes).subspan(i * commandFrameBytes, commandFrameBytes));
    }
    return bytes;
}

LoopbackConnection connectLoopback(std::uint16_t port, const std::function<bool()>& failed) {
    LoopbackConnection connection;
    connection.socket = network::TcpSocket::connectLoopback(port);
    connection.socket.setNonBlocking();
    std::array<std::byte, 64> hello{};
    sendAll(connection.socket, std::span<const std::byte>(hello).first(network::encodeHello(0, network::wireVersion, hello)));
    for (;;) {  // the Welcome, so the connection has been taken on before anything is timed
        if (failed && failed()) {
            throw std::runtime_error("loopback: the engine failed before a Welcome");
        }
        std::array<std::byte, 64> into{};
        const network::ReadResult read = connection.socket.receive(into);
        if (read.status != network::ReadStatus::OPEN) {
            throw std::runtime_error("loopback: a connection closed before its Welcome");
        }
        connection.reader.feed(std::span<const std::byte>(into).first(read.bytes));
        const network::FrameResult result = connection.reader.next();
        if (const network::Frame* frame = std::get_if<network::Frame>(&result)) {
            if (frame->type != network::FrameType::WELCOME) {
                throw std::runtime_error("loopback: a Hello was not answered with a Welcome");
            }
            return connection;
        }
    }
}

struct LoopbackClients::Connection {
    LoopbackConnection link;
    std::vector<std::byte> timed;  // every timed command's frame, built before the timed region
    std::vector<std::byte> bytes;  // what pump sends: the preload's frames, then timed's
    std::size_t sentBytes = 0;
    std::size_t commands = 0;
    std::size_t answered = 0;
    WorkloadOutcome* tally = nullptr;  // where pump counts what comes back
};

LoopbackClients::~LoopbackClients() = default;
LoopbackClients::LoopbackClients(LoopbackClients&&) noexcept = default;
LoopbackClients& LoopbackClients::operator=(LoopbackClients&&) noexcept = default;

LoopbackClients::LoopbackClients(const std::vector<Workload>& runWorkloads, std::function<bool()> hasFailed)
    : workloads(&runWorkloads), failed(std::move(hasFailed)), connections(runWorkloads.size()), tallies(runWorkloads.size()) {
    for (std::size_t book = 0; book < connections.size(); ++book) {
        const std::vector<replay::ReplayRecord> records = workloadRecords(runWorkloads[book], instrumentSymbol(book));
        connections[book].timed = commandFrames(records, instrumentSymbol(book));
        // Every frame built is sent, or run throws, so it is counted here, untimed.
        for (const replay::ReplayRecord& record : records) {
            countCommand(record.command, {}, tallies[book]);
        }
    }
}

void LoopbackClients::connect(std::uint16_t port) {
    for (Connection& connection : connections) {
        connection.link = connectLoopback(port, failed);
    }
}

void LoopbackClients::preload(std::size_t window) {
    std::vector<WorkloadOutcome> preloaded(connections.size());
    answered = 0;
    total = 0;
    for (std::size_t book = 0; book < connections.size(); ++book) {
        Connection& connection = connections[book];
        const std::vector<replay::ReplayRecord> records = preloadRecords((*workloads)[book], instrumentSymbol(book));
        connection.bytes = commandFrames(records, instrumentSymbol(book));
        connection.sentBytes = 0;
        connection.commands = records.size();
        connection.answered = 0;
        connection.tally = &preloaded[book];
        total += records.size();
    }
    pump(window);
    for (std::size_t book = 0; book < connections.size(); ++book) {
        if (preloaded[book].rejections != 0 || preloaded[book].trades != 0) {
            throw std::runtime_error("workload " + (*workloads)[book].config.name + ": the book refused or traded its preload");
        }
    }
}

void LoopbackClients::run(std::size_t window) {
    answered = 0;
    total = 0;
    for (std::size_t book = 0; book < connections.size(); ++book) {
        Connection& connection = connections[book];
        connection.bytes.swap(connection.timed);
        connection.sentBytes = 0;
        connection.commands = connection.bytes.size() / commandFrameBytes;
        connection.answered = 0;
        connection.tally = &tallies[book];
        total += connection.commands;
    }
    pump(window);
}

void LoopbackClients::pump(std::size_t window) {
    while (answered < total) {
        if (failed && failed()) {
            throw std::runtime_error("loopback: the engine failed");
        }
        for (Connection& connection : connections) {
            const std::size_t allowed = std::min(connection.commands, connection.answered + window) * commandFrameBytes;
            if (connection.sentBytes < allowed) {
                const std::optional<std::size_t> taken = connection.link.socket.send(
                    std::span<const std::byte>(connection.bytes).subspan(connection.sentBytes, allowed - connection.sentBytes));
                if (!taken) {
                    throw std::runtime_error("loopback: a connection closed while it was being sent to");
                }
                connection.sentBytes += *taken;
            }
            receive(connection);
        }
    }
}

void LoopbackClients::receive(Connection& connection) {
    std::array<std::byte, 64 * 1024> into;
    const network::ReadResult read = connection.link.socket.receive(into);
    if (read.status != network::ReadStatus::OPEN) {
        throw std::runtime_error("loopback: a connection closed with commands unanswered");
    }
    if (read.bytes != 0) {
        connection.link.reader.feed(std::span<const std::byte>(into).first(read.bytes));
    }
    for (network::FrameResult result = connection.link.reader.next(); std::holds_alternative<network::Frame>(result);
         result = connection.link.reader.next()) {
        const network::Frame& frame = std::get<network::Frame>(result);
        const std::optional<network::EventFrame> event = network::decodeEvent(frame);
        if (!event) {
            throw std::runtime_error("loopback: an answer was not an event");
        }
        countResponse(event->event, *connection.tally);
        if (frame.last) {
            ++connection.answered;
            ++answered;
        }
    }
}

void LoopbackClients::finish() {
    // Each connection half-closes and reads to its end, so every answer is in before a stop.
    for (Connection& connection : connections) {
        if (!connection.link.socket.valid()) {
            continue;  // taken
        }
        connection.link.socket.shutdownSend();
        std::array<std::byte, 1024> into;
        while (!(failed && failed()) && connection.link.socket.receive(into).status == network::ReadStatus::OPEN) {
        }
    }
}

LoopbackConnection LoopbackClients::take(std::size_t book) {
    return std::move(connections[book].link);
}

GatewayRun::GatewayRun(const std::vector<Workload>& runWorkloads, std::size_t commandWindow)
    : workloads(&runWorkloads),
      window(commandWindow),
      commands(runtime::defaultCommandRingSlots),
      responses(runtime::defaultResponseRingSlots),
      sockets(std::make_unique<network::PollSocketSet>()),
      clients(runWorkloads, [this] { return gatewayFailed.load(std::memory_order_seq_cst); }),
      matching((commandWindow == 0 ? throw std::invalid_argument("a gateway run needs a window of at least 1") : runWorkloads), doorbell,
               {runtime::RingPair{&commands, &responses}}) {
    gateway = std::thread([this] {
        try {
            network::GatewayThread gatewayThread(matching.books().instruments(), {network::ShardLink{&commands, &responses, &doorbell}},
                                                 *sockets);
            network::runGateway(gatewayThread, stopRequested);
        } catch (...) {
            gatewayFailure = std::current_exception();
            gatewayFailed.store(true, std::memory_order_seq_cst);
        }
    });
    try {
        clients.connect(sockets->port());
    } catch (...) {
        try {
            stopThreads();
        } catch (const std::exception&) {  // the first failure is the one to report
        }
        finished = true;
        throw;
    }
}

void GatewayRun::stopThreads() {
    stopRequested.store(true, std::memory_order_seq_cst);
    sockets->wake();
    gateway.join();
    if (gatewayFailure) {
        // The gateway thread has gone, so this one may write its ring and stop the shard itself.
        while (!commands.tryPush(runtime::CommandElement::stopMarker())) {
            std::this_thread::yield();
        }
        doorbell.ring();
    }
    try {
        matching.join();
    } catch (...) {
        if (!gatewayFailure) {
            throw;
        }
    }
    if (gatewayFailure) {
        std::rethrow_exception(gatewayFailure);
    }
}

GatewayRun::~GatewayRun() {
    if (!finished) {
        try {
            finish();
        } catch (const std::exception&) {  // a destructor can't report it; check would have
        }
    }
}

void GatewayRun::run() {
    clients.run(window);
}

void GatewayRun::finish() {
    if (finished) {
        return;
    }
    finished = true;
    clients.finish();
    stopThreads();
}

std::vector<WorkloadOutcome> GatewayRun::outcomes() const {
    return matching.withFinalShapes(clients.counted());
}

void GatewayRun::check() const {
    requireOutcomes(outcomes(), *workloads, BenchmarkKind::GATEWAY);
}

EngineRun::EngineRun(const std::vector<Workload>& runWorkloads, std::size_t commandWindow, const std::filesystem::path& enginePath,
                     std::size_t shards)
    : workloads(&runWorkloads),
      window(commandWindow == 0 ? throw std::invalid_argument("an engine run needs a window of at least 1") : commandWindow),
      engine(std::make_unique<EngineProcess>(enginePath, runWorkloads, EngineOptions{shards, {}})),
      clients(runWorkloads, {}) {
    clients.connect(engine->port());
    clients.preload(window);
}

EngineRun::~EngineRun() = default;

void EngineRun::run() {
    clients.run(window);
}

void EngineRun::finish() {
    if (finished) {
        return;
    }
    finished = true;
    clients.finish();
    engine->stop();
}

WorkloadOutcome withPredictedShape(WorkloadOutcome counted, const Workload& workload) {
    counted.finalActiveOrders = workload.expected.finalActiveOrders;
    counted.finalPriceLevels = workload.expected.finalPriceLevels;
    counted.finalPendingStops = workload.expected.finalPendingStops;
    return counted;
}

std::vector<WorkloadOutcome> EngineRun::outcomes() const {
    std::vector<WorkloadOutcome> counted = clients.counted();
    for (std::size_t i = 0; i < counted.size(); ++i) {
        counted[i] = withPredictedShape(counted[i], (*workloads)[i]);
    }
    return counted;
}

void EngineRun::check() const {
    requireOutcomes(outcomes(), *workloads, BenchmarkKind::ENGINE);
}

Measurement runInstrumentsOnce(const std::vector<Workload>& workloads, BenchmarkKind kind, std::size_t window) {
    RunSettings settings;
    settings.window = window;
    return runInstrumentsOnce(workloads, kind, settings);
}

Measurement runInstrumentsOnce(const std::vector<Workload>& workloads, BenchmarkKind kind, const RunSettings& settings) {
    Measurement measurement;
    std::vector<WorkloadOutcome> outcomes;
    if (kind == BenchmarkKind::ENGINE) {
        EngineRun run(workloads, settings.window, settings.engine, settings.shards);
        const Clock::time_point start = Clock::now();
        run.run();
        measurement.elapsedNanoseconds = nanosecondsBetween(start, Clock::now());
        run.finish();
        run.check();
        outcomes = run.outcomes();
        measurement.window = settings.window;
        measurement.shards = settings.shards;
    } else if (kind == BenchmarkKind::GATEWAY) {
        GatewayRun run(workloads, settings.window);
        const Clock::time_point start = Clock::now();
        run.run();
        measurement.elapsedNanoseconds = nanosecondsBetween(start, Clock::now());
        run.finish();
        run.check();
        outcomes = run.outcomes();
        measurement.window = settings.window;
    } else if (kind == BenchmarkKind::SHARD) {
        ShardRun run(workloads);
        const Clock::time_point start = Clock::now();
        run.run();
        measurement.elapsedNanoseconds = nanosecondsBetween(start, Clock::now());
        run.check();
        outcomes = run.outcomes();
    } else {
        QueueRun run(workloads);
        const Clock::time_point start = Clock::now();
        run.run();
        measurement.elapsedNanoseconds = nanosecondsBetween(start, Clock::now());
        run.finish();
        run.check();
        outcomes = run.outcomes();
    }
    measurement.workload = workloads.front().config.name;
    measurement.kind = kind;
    measurement.instruments = workloads.size();
    for (const Workload& workload : workloads) {
        measurement.commands += workload.timedCount();
    }
    for (const WorkloadOutcome& outcome : outcomes) {
        addOutcome(measurement.outcome, outcome);
    }
    return measurement;
}

std::vector<Measurement> runInstrumentsRepeated(const std::vector<Workload>& workloads, BenchmarkKind kind,
                                                const RunSettings& settings) {
    return repeated(settings, [&workloads, kind, &settings] { return runInstrumentsOnce(workloads, kind, settings); });
}

Measurement runOnce(const Workload& workload, BenchmarkKind kind) {
    if (sharesInstruments(kind)) {
        return runInstrumentsOnce({workload}, kind);
    }
    Measurement measurement = kind == BenchmarkKind::CORE ? runCore(workload) : runReplay(workload, kind == BenchmarkKind::REPLAY_LOG);
    measurement.workload = workload.config.name;
    measurement.kind = kind;
    measurement.commands = workload.timedCount();
    if (measurement.outcome != workload.expected) {
        throw std::runtime_error("workload " + workload.config.name + " (" + benchmarkKindName(kind) +
                                 "): the book did " + describeOutcome(measurement.outcome) +
                                 " but the workload predicted " + describeOutcome(workload.expected));
    }
    return measurement;
}

std::vector<Measurement> runRepeated(const Workload& workload, BenchmarkKind kind, const RunSettings& settings) {
    return repeated(settings, [&workload, kind] { return runOnce(workload, kind); });
}

std::vector<replay::ReplayRecord> workloadRecords(const Workload& workload, replay::Symbol symbol) {
    return recordsOf(workload.timed, workload.behaviorTimed, symbol);
}

std::vector<replay::ReplayRecord> preloadRecords(const Workload& workload, replay::Symbol symbol) {
    return recordsOf(workload.preload, workload.behaviorPreload, symbol);
}

std::string renderCommandLog(const Workload& workload, replay::Symbol symbol) {
    const std::vector<std::byte> header = replay::encodeLogHeader(replay::LogHeader{});
    std::uint32_t checksum = replay::logChainSeed(header);
    std::string bytes(reinterpret_cast<const char*>(header.data()), header.size());
    bytes.reserve(bytes.size() + workload.timedCount() * replay::logRecordBytes);
    for (const replay::ReplayRecord& record : workloadRecords(workload, symbol)) {
        std::array<std::byte, replay::logRecordBytes> encoded;
        checksum = replay::encodeLogRecord(record.symbol, record.command, checksum, encoded);
        bytes.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    }
    return bytes;
}

std::string renderReplayCsv(const std::vector<BenchmarkCommand>& commands) {
    std::string csv(order_book::replay::expectedHeader);
    csv += '\n';
    std::uint64_t sequence = 0;
    for (const BenchmarkCommand& command : commands) {
        csv += std::to_string(++sequence);
        if (command.type == CommandType::SUBMIT) {
            csv += ",ADD,";
            csv += std::to_string(command.orderId);
            csv += command.side == Side::BUY ? ",BUY," : ",SELL,";
            csv += std::to_string(command.price);
            csv += ',';
            csv += std::to_string(command.quantity);
        } else {
            csv += ",CANCEL,";
            csv += std::to_string(command.orderId);
            csv += ",,,";
        }
        csv += '\n';
    }
    return csv;
}

namespace {

const char* typeKeyword(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::STOP: return "STOP";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
    }
    return "LIMIT";
}

const char* timeInForceKeyword(TimeInForce timeInForce) {
    switch (timeInForce) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "GTC";
}

const char* modeKeyword(SelfTradePrevention mode) {
    switch (mode) {
        case SelfTradePrevention::CANCEL_RESTING: return "CANCEL_RESTING";
        case SelfTradePrevention::CANCEL_INCOMING: return "CANCEL_INCOMING";
        case SelfTradePrevention::CANCEL_BOTH: return "CANCEL_BOTH";
    }
    return "CANCEL_RESTING";
}

// post_only, owner, and self_trade_prevention, the mode only with an owner (the format
// requires that, and with no owner the book ignores the mode anyway).
void appendInstructions(std::string& csv, bool postOnly, OwnerId owner, SelfTradePrevention mode) {
    csv += postOnly ? ",Y," : ",,";
    if (owner != 0) {
        csv += std::to_string(owner);
        csv += ',';
        csv += modeKeyword(mode);
    } else {
        csv += ',';
    }
}

}  // namespace

std::string renderReplayCsv(const std::vector<BehaviorCommand>& commands) {
    std::string csv(order_book::replay::expectedHeaderV2);
    csv += '\n';
    std::uint64_t sequence = 0;
    for (const BehaviorCommand& command : commands) {
        csv += std::to_string(++sequence);
        if (command.type == CommandType::SUBMIT) {
            const OrderRequest& request = command.request;
            csv += ",ADD,";
            csv += std::to_string(request.id);
            csv += request.side == Side::BUY ? ",BUY," : ",SELL,";
            if (request.price != 0) {
                csv += std::to_string(request.price);
            }
            csv += ',';
            csv += std::to_string(request.quantity);
            csv += ',';
            csv += typeKeyword(request.type);
            csv += ',';
            csv += timeInForceKeyword(request.timeInForce);
            csv += ',';
            if (request.stopPrice != 0) {
                csv += std::to_string(request.stopPrice);
            }
            appendInstructions(csv, request.postOnly, request.owner, request.selfTradePrevention);
        } else if (command.type == CommandType::REPLACE) {
            const ReplaceRequest& replacement = command.replacement;
            csv += ",REPLACE,";
            csv += std::to_string(replacement.id);
            csv += ",,";
            csv += std::to_string(replacement.price);
            csv += ',';
            csv += std::to_string(replacement.quantity);
            csv += ",,,";
            appendInstructions(csv, replacement.postOnly, replacement.owner, replacement.selfTradePrevention);
        } else {
            csv += ",CANCEL,";
            csv += std::to_string(command.request.id);
            csv += ",,,,,,,,,";
        }
        csv += '\n';
    }
    return csv;
}

}  // namespace order_book::benchmark
