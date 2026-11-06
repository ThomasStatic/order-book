#include "scaling_run.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <iterator>
#include <latch>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "order_book/little_endian.hpp"
#include "order_book/network/poll_socket_set.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/xxh64.hpp"
#include "thread_cpu.hpp"

namespace order_book::benchmark {

namespace {

using Clock = std::chrono::steady_clock;

double secondsBetween(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration<double>(stop - start).count();
}

double nanosecondsBetween(Clock::time_point start, Clock::time_point stop) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
}

// The cores a plan names, by a thread's place in the plan's order, or nothing past its end.
class Cores {
public:
    Cores(ThreadPlan plan, std::size_t needed) : plan_(std::move(plan)) {
        if (!plan_.cores.empty() && plan_.cores.size() < needed) {
            throw std::invalid_argument("the plan names " + std::to_string(plan_.cores.size()) + " cores, and this run has " +
                                        std::to_string(needed) + " threads to pin");
        }
    }
    std::optional<unsigned int> at(std::size_t index) const {
        return index < plan_.cores.size() ? std::optional<unsigned int>(plan_.cores[index]) : std::nullopt;
    }
    bool highPriority() const noexcept { return plan_.highPriority; }

private:
    ThreadPlan plan_;
};

// The CPU meters of threads this run doesn't drive itself, the matching threads, gateway threads,
// and log writers, each made on its own thread as it starts, and read from this one either side
// of the timed region.
class Meters {
public:
    void add(ThreadCpuTime meter) {
        const std::lock_guard<std::mutex> held(lock);
        meters.push_back(std::move(meter));
    }
    struct Reading {
        std::vector<std::uint64_t> used;
        Clock::time_point at;
    };
    Reading read() {
        const std::lock_guard<std::mutex> held(lock);
        Reading reading;
        for (const ThreadCpuTime& meter : meters) {
            reading.used.push_back(meter.reading());
        }
        reading.at = Clock::now();
        return reading;
    }

private:
    std::mutex lock;
    std::vector<ThreadCpuTime> meters;
};

// The lowest share of a core: of those a run's own threads took over their own spans, and of what
// the meters used between two readings.
std::optional<double> lowestShare(std::vector<std::optional<double>> shares, const Meters::Reading& before, const Meters::Reading& after) {
    const double span = nanosecondsBetween(before.at, after.at);
    for (std::size_t i = 0; i < before.used.size() && i < after.used.size(); ++i) {
        shares.push_back(ThreadCpuTime::shareOf(after.used[i] - before.used[i], span));
    }
    std::optional<double> lowest;
    for (const std::optional<double>& share : shares) {
        if (!share) {
            return std::nullopt;  // one that can't be measured leaves the run's lowest unknown
        }
        lowest = lowest ? std::min(*lowest, *share) : *share;
    }
    return lowest;
}

// What a thread this run drives saw: its own share over its own span, and when it finished.
struct DrivenThread {
    std::optional<double> share;
    Clock::time_point finished;
    std::exception_ptr failure;
};

// Starts count threads, thread i placed by core firstCore + i and metered, which runs prepare(i),
// waits at a common start, runs body(i), timed to its own finish, and then after(i), untimed; and
// joins them. beforeStart runs on this thread once every one is prepared, just before the start;
// atFinish once every body has finished, while every thread still lives. Rethrows the first
// thread's failure.
template <typename Prepare, typename Body, typename After, typename BeforeStart, typename AtFinish>
std::vector<DrivenThread> runThreads(std::size_t count, const Cores& cores, std::size_t firstCore, Prepare prepare, Body body,
                                     After after, BeforeStart beforeStart, AtFinish atFinish) {
    std::vector<DrivenThread> threads(count);
    std::latch prepared(static_cast<std::ptrdiff_t>(count));
    std::latch finished(static_cast<std::ptrdiff_t>(count));
    std::atomic<bool> go{false};
    std::atomic<bool> release{false};
    std::vector<std::thread> running;
    running.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        running.emplace_back([&, i] {
            DrivenThread& own = threads[i];
            std::optional<ThreadCpuTime> meter;
            try {
                placeCurrentThread(cores.at(firstCore + i), cores.highPriority());
                meter.emplace(ThreadCpuTime::ofCurrentThread());
                prepare(i);
            } catch (...) {
                own.failure = std::current_exception();
            }
            prepared.count_down();
            go.wait(false, std::memory_order_seq_cst);
            if (!own.failure) {
                try {
                    const Clock::time_point begun = Clock::now();
                    const std::uint64_t used = meter->reading();
                    body(i);
                    own.finished = Clock::now();
                    own.share = ThreadCpuTime::shareOf(meter->reading() - used, nanosecondsBetween(begun, own.finished));
                } catch (...) {
                    own.failure = std::current_exception();
                }
            }
            finished.count_down();
            // Alive until every body has finished and this thread has read the meters, so they
            // read live threads.
            release.wait(false, std::memory_order_seq_cst);
            if (!own.failure) {
                try {
                    after(i);
                } catch (...) {
                    own.failure = std::current_exception();
                }
            }
        });
    }
    prepared.wait();
    beforeStart();
    go.store(true, std::memory_order_seq_cst);
    go.notify_all();
    finished.wait();
    atFinish();
    release.store(true, std::memory_order_seq_cst);
    release.notify_all();
    for (std::thread& thread : running) {
        thread.join();
    }
    for (const DrivenThread& thread : threads) {
        if (thread.failure) {
            std::rethrow_exception(thread.failure);
        }
    }
    return threads;
}

Clock::time_point lastFinish(const std::vector<DrivenThread>& threads) {
    Clock::time_point last{};
    for (const DrivenThread& thread : threads) {
        last = std::max(last, thread.finished);
    }
    return last;
}

std::vector<std::optional<double>> ownShares(const std::vector<DrivenThread>& threads) {
    std::vector<std::optional<double>> shares;
    for (const DrivenThread& thread : threads) {
        shares.push_back(thread.share);
    }
    return shares;
}

// The setup a thread runs first: placed by its core, then metered.
std::function<void()> placedAndMetered(std::optional<unsigned int> core, bool highPriority, Meters& meters) {
    return [core, highPriority, &meters] {
        placeCurrentThread(core, highPriority);
        meters.add(ThreadCpuTime::ofCurrentThread());
    };
}

// The setup a helper thread runs first: placed by its core, never metered, since it sleeps.
std::function<void()> placedOnly(std::optional<unsigned int> core, bool highPriority) {
    return [core, highPriority] { placeCurrentThread(core, highPriority); };
}

void addOutcomes(Measurement& measurement, const std::vector<WorkloadOutcome>& outcomes) {
    for (const WorkloadOutcome& outcome : outcomes) {
        addOutcome(measurement.outcome, outcome);
    }
}

std::size_t commandsOf(const std::vector<Workload>& workloads, const std::vector<std::uint32_t>& instruments) {
    std::size_t commands = 0;
    for (const std::uint32_t instrument : instruments) {
        commands += workloads[instrument].timedCount();
    }
    return commands;
}

// N shard threads, each owning its books: built, checked, and destroyed on it.
void runShards(const std::vector<Workload>& workloads, const RunPlacement& placement, const ScalingSettings& settings,
               Measurement& measurement) {
    const std::size_t shards = settings.shards;
    const Cores cores(settings.plan, shards);
    std::vector<std::unique_ptr<ShardRun>> runs(shards);
    std::vector<std::vector<WorkloadOutcome>> outcomes(shards);
    Clock::time_point start;
    const std::vector<DrivenThread> threads = runThreads(
        shards, cores, 0,
        [&](std::size_t i) {
            runs[i] = std::make_unique<ShardRun>(BookGroup::of(workloads, placement.instrumentsOf[i]), placement.directory,
                                                 static_cast<std::uint32_t>(i), settings.spread);
        },
        [&](std::size_t i) { runs[i]->run(); },
        [&](std::size_t i) {
            runs[i]->check();
            outcomes[i] = runs[i]->outcomes();
            runs[i].reset();
        },
        [&] { start = Clock::now(); }, [] {});
    measurement.elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(lastFinish(threads) - start).count();
    for (std::size_t i = 0; i < shards; ++i) {
        measurement.partRates.push_back(static_cast<double>(commandsOf(workloads, placement.instrumentsOf[i])) /
                                        secondsBetween(start, threads[i].finished));
        addOutcomes(measurement, outcomes[i]);
    }
    measurement.minThreadCpuShare = lowestShare(ownShares(threads), {}, {});
}

LaneOptions laneFor(const ScalingSettings& settings, const Cores& cores, std::size_t shard, std::size_t writerCore, Meters& meters) {
    LaneOptions options;
    options.setUpMatchingThread = placedAndMetered(cores.at(shard), cores.highPriority(), meters);
    options.setUpWriter = placedAndMetered(cores.at(writerCore), cores.highPriority(), meters);
    options.setUpHelpers = placedOnly(cores.at(writerCore), cores.highPriority());
    options.durability = settings.durability;
    options.fill = settings.fill;
    options.logParent = settings.logParent;
    options.spinWindow = settings.spinWindow;
    options.rollWorker = settings.rollWorker;
    options.logFileBytes = settings.logFileBytes;
    return options;
}

// N lanes, a matching thread each, driven by M producer threads: shard i by producer i modulo M.
void runQueues(const std::vector<Workload>& workloads, const RunPlacement& placement, const ScalingSettings& settings,
               Measurement& measurement) {
    const std::size_t shards = settings.shards;
    const std::size_t producers = settings.threads;
    const Cores cores(settings.plan, shards + producers);
    Meters meters;
    std::vector<std::unique_ptr<QueueRun>> lanes;
    for (std::size_t i = 0; i < shards; ++i) {
        lanes.push_back(std::make_unique<QueueRun>(BookGroup::of(workloads, placement.instrumentsOf[i]), placement.directory,
                                                   static_cast<std::uint32_t>(i), settings.spread,
                                                   laneFor(settings, cores, i, shards + producers + i, meters)));
    }
    std::vector<Clock::time_point> laneFinished(shards);
    Meters::Reading before;
    Meters::Reading after;
    const std::vector<DrivenThread> threads = runThreads(
        producers, cores, shards, [](std::size_t) {},
        [&](std::size_t producer) {
            std::vector<std::size_t> mine;
            for (std::size_t i = producer; i < shards; i += producers) {
                mine.push_back(i);
            }
            // In turn, a batch each, as one producer feeding several shards would.
            while (!mine.empty()) {
                for (std::size_t k = 0; k < mine.size();) {
                    if (lanes[mine[k]]->step()) {
                        laneFinished[mine[k]] = Clock::now();
                        mine.erase(mine.begin() + static_cast<std::ptrdiff_t>(k));
                    } else {
                        ++k;
                    }
                }
            }
        },
        [](std::size_t) {}, [&] { before = meters.read(); }, [&] { after = meters.read(); });
    // The producers have gone, so this thread is each ring's producer now.
    for (std::size_t i = 0; i < shards; ++i) {
        lanes[i]->finish();
        lanes[i]->check();
        addOutcomes(measurement, lanes[i]->outcomes());
        measurement.rollPasses += lanes[i]->rollPasses();
        measurement.partRates.push_back(static_cast<double>(lanes[i]->commandCount()) / secondsBetween(before.at, laneFinished[i]));
    }
    measurement.elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(lastFinish(threads) - before.at).count();
    measurement.minThreadCpuShare = lowestShare(ownShares(threads), before, after);
}

// N matching threads with M ring pairs each, M gateway threads each linked to every shard and
// sharing connections as the engine's do, and M client threads.
class ShardedGateway {
public:
    ShardedGateway(const std::vector<Workload>& workloads, const RunPlacement& placement, const ScalingSettings& settings)
        : settings_(settings), cores_(settings.plan, settings.shards + 2 * settings.threads) {
        if (settings.window == 0) {
            throw std::invalid_argument("a gateway run needs a window of at least 1");
        }
        if (settings.threads > workloads.size()) {
            throw std::invalid_argument("a gateway run needs a connection for each of its client threads");
        }
        const std::size_t shards = settings.shards;
        const std::size_t threads = settings.threads;
        // Clients first: nothing here can stop a matching thread if building them throws.
        clientBooks_.resize(threads);
        for (std::uint32_t k = 0; k < workloads.size(); ++k) {
            clientBooks_[k % threads].push_back(k);
        }
        for (std::size_t j = 0; j < threads; ++j) {
            clients_.emplace_back(BookGroup::of(workloads, clientBooks_[j]), [this] { return failed_.load(std::memory_order_seq_cst); });
        }
        for (std::size_t i = 0; i < shards; ++i) {
            auto shard = std::make_unique<ShardLanes>();
            for (std::size_t j = 0; j < threads; ++j) {
                shard->rings.push_back(std::make_unique<Rings>());
            }
            if (settings.durability != RunDurability::NONE) {
                shard->logRing = std::make_unique<LogRing>();
            }
            shard->group = BookGroup::of(workloads, placement.instrumentsOf[i]);
            shards_.push_back(std::move(shard));
        }
        try {
            for (std::size_t i = 0; i < shards; ++i) {
                ShardLanes& shard = *shards_[i];
                std::vector<runtime::RingPair> pairs;
                for (const std::unique_ptr<Rings>& rings : shard.rings) {
                    pairs.push_back(runtime::RingPair{&rings->commands, &rings->responses});
                }
                shard.matching = std::make_unique<MatchingThread>(
                    shard.group, placement.directory, static_cast<std::uint32_t>(i), settings.spread, shard.doorbell, std::move(pairs),
                    placedAndMetered(cores_.at(i), cores_.highPriority(), meters_),
                    shard.logRing ? std::optional<runtime::LogLink>(shard.logRing->link()) : std::nullopt, settings.spinWindow);
                if (shard.logRing) {
                    shard.logFirst = shard.matching->books().firstTimedPosition();
                    shard.logCommands = shard.matching->books().sharing().size();
                    const std::size_t writerCore = shards + 2 * threads + i;
                    std::optional<RunLogWorker> worker;
                    if (settings.rollWorker) {
                        worker = RunLogWorker{preloadedImages(shard.group)};
                    }
                    shard.log = std::make_unique<RunLog>(
                        settings.logParent, settings.durability, *placement.directory, *shard.logRing, shard.logFirst,
                        placedAndMetered(cores_.at(writerCore), cores_.highPriority(), meters_), settings.fill, static_cast<std::uint32_t>(i),
                        std::move(worker), settings.logFileBytes, placedOnly(cores_.at(writerCore), cores_.highPriority()));
                }
            }
            startGateways(*placement.directory);
            for (LoopbackClients& clients : clients_) {
                clients.connect(port_);
            }
        } catch (...) {
            try {
                stop();
            } catch (const std::exception&) {  // the first failure is the one to report
            }
            throw;
        }
    }

    ~ShardedGateway() {
        try {
            stop();
        } catch (const std::exception&) {  // a destructor can't report it; check would have
        }
    }

    ShardedGateway(const ShardedGateway&) = delete;
    ShardedGateway& operator=(const ShardedGateway&) = delete;

    // Each shard's rate is its commands to the last answer any of its books had.
    void run(const std::vector<Workload>& workloads, Measurement& measurement) {
        Meters::Reading before;
        Meters::Reading after;
        const std::vector<DrivenThread> threads = runThreads(
            clients_.size(), cores_, settings_.shards + settings_.threads, [](std::size_t) {},
            [this](std::size_t j) { clients_[j].run(settings_.window); }, [](std::size_t) {}, [&] { before = meters_.read(); },
            [&] { after = meters_.read(); });
        measurement.elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(lastFinish(threads) - before.at).count();
        std::vector<Clock::time_point> finishedAt(workloads.size());
        for (std::size_t j = 0; j < clients_.size(); ++j) {
            const std::vector<Clock::time_point> times = clients_[j].finishTimes();
            for (std::size_t k = 0; k < clientBooks_[j].size(); ++k) {
                finishedAt[clientBooks_[j][k]] = times[k];
            }
        }
        for (const std::unique_ptr<ShardLanes>& shard : shards_) {
            Clock::time_point last{};
            for (const std::uint32_t instrument : shard->group.instruments) {
                last = std::max(last, finishedAt[instrument]);
            }
            measurement.partRates.push_back(static_cast<double>(commandsOf(workloads, shard->group.instruments)) /
                                            secondsBetween(before.at, last));
        }
        measurement.minThreadCpuShare = lowestShare(ownShares(threads), before, after);
    }

    // Half-closes every connection and reads it to its end, stops every thread, and checks every
    // book against its prediction and every log against what it must hold.
    void finishAndCheck(const std::vector<Workload>& workloads, Measurement& measurement) {
        for (LoopbackClients& clients : clients_) {
            clients.finish();
        }
        stop();
        std::vector<WorkloadOutcome> byInstrument(workloads.size());
        for (std::size_t j = 0; j < clients_.size(); ++j) {
            for (std::size_t k = 0; k < clientBooks_[j].size(); ++k) {
                byInstrument[clientBooks_[j][k]] = clients_[j].counted()[k];
            }
        }
        for (const std::unique_ptr<ShardLanes>& shard : shards_) {
            std::vector<WorkloadOutcome> counted;
            for (const std::uint32_t instrument : shard->group.instruments) {
                counted.push_back(byInstrument[instrument]);
            }
            const std::vector<WorkloadOutcome> outcomes = shard->matching->withFinalShapes(counted);
            requireOutcomes(outcomes, shard->group, BenchmarkKind::GATEWAY);
            if (shard->log) {
                measurement.rollPasses += shard->log->rollPasses();
                if (shard->logEnd - shard->logFirst != shard->logCommands) {
                    throw std::runtime_error("gateway: a shard's log holds " + std::to_string(shard->logEnd - shard->logFirst) +
                                             " records, not its " + std::to_string(shard->logCommands) + " commands");
                }
            }
            addOutcomes(measurement, outcomes);
        }
    }

private:
    struct Rings {
        runtime::Ring<runtime::CommandElement> commands{runtime::defaultCommandRingSlots};
        runtime::Ring<runtime::ResponseRecord> responses{runtime::defaultResponseRingSlots};
    };
    // One shard as the gateway threads see it: a ring pair for each.
    struct ShardLanes {
        runtime::Doorbell doorbell;
        std::vector<std::unique_ptr<Rings>> rings;  // by gateway thread
        std::unique_ptr<LogRing> logRing;
        BookGroup group;
        std::unique_ptr<MatchingThread> matching;
        std::unique_ptr<RunLog> log;
        std::uint64_t logFirst = 0;
        std::size_t logCommands = 0;
        std::uint64_t logEnd = 0;
    };
    struct Gateway {
        std::unique_ptr<network::PollSocketSet> sockets;
        std::thread thread;
        std::exception_ptr startFailure;  // written before the start's count-down, so read after it
        std::exception_ptr failure;       // read only once the thread is joined
        std::atomic<bool> failed{false};  // set last by a thread that threw, which then touches no ring
        std::atomic<bool> done{false};
    };

    void startGateways(const runtime::InstrumentDirectory& directory) {
        const std::size_t threads = settings_.threads;
        std::vector<network::SocketSet*> sockets;
        for (std::size_t j = 0; j < threads; ++j) {
            auto gateway = std::make_unique<Gateway>();
            gateway->sockets = j == 0 ? std::make_unique<network::PollSocketSet>()
                                      : std::make_unique<network::PollSocketSet>(network::PollSocketSet::NoListener{});
            sockets.push_back(gateway->sockets.get());
            gateways_.push_back(std::move(gateway));
        }
        port_ = gateways_[0]->sockets->port();
        handOff_ = std::make_unique<network::ConnectionHandOff>(std::move(sockets));
        std::latch built(static_cast<std::ptrdiff_t>(threads));
        for (std::size_t j = 0; j < threads; ++j) {
            gateways_[j]->thread = std::thread([this, j, &directory, &built] {
                Gateway& own = *gateways_[j];
                bool counted = false;
                try {
                    placedAndMetered(cores_.at(settings_.shards + j), cores_.highPriority(), meters_)();
                    std::vector<network::ShardLink> links;
                    for (const std::unique_ptr<ShardLanes>& shard : shards_) {
                        links.push_back(network::ShardLink{&shard->rings[j]->commands, &shard->rings[j]->responses, &shard->doorbell,
                                                           shard->logRing ? &shard->logRing->durable : nullptr});
                    }
                    network::GatewaySettings gatewaySettings;
                    gatewaySettings.spinWindow = settings_.spinWindow;
                    gatewaySettings.framesPerPass = settings_.framesPerPass;
                    network::GatewayThread gateway(directory, std::move(links), *own.sockets, gatewaySettings, handOff_.get(), j);
                    counted = true;
                    built.count_down();
                    // As the engine's: none runs until every one is built, since thread 0 hands
                    // connections to the others.
                    released_.wait(false, std::memory_order_seq_cst);
                    network::runGateway(gateway, stopRequested_);
                } catch (...) {
                    if (counted) {
                        own.failure = std::current_exception();
                    } else {
                        own.startFailure = std::current_exception();
                    }
                    failed_.store(true, std::memory_order_seq_cst);
                    own.failed.store(true, std::memory_order_seq_cst);
                    if (!counted) {
                        built.count_down();
                    }
                }
                own.done.store(true, std::memory_order_seq_cst);
            });
        }
        built.wait();
        released_.store(true, std::memory_order_seq_cst);
        released_.notify_all();
        for (const std::unique_ptr<Gateway>& gateway : gateways_) {
            if (gateway->startFailure) {
                std::rethrow_exception(gateway->startFailure);
            }
        }
    }

    // A gateway thread that never ran or has failed sends no more stop markers, and every shard
    // waits for one on each of its rings: this thread sends them, as the engine does, and keeps
    // doing so for any that fail while the rest stop, since a running gateway thread waits for
    // every shard to stop. A failed one may have sent some already; a second marker only stops
    // the shard sooner, on a run that fails anyway.
    void markStopped(std::vector<bool>& marked) {
        for (std::size_t j = 0; j < settings_.threads; ++j) {
            const bool dead = j >= gateways_.size() || !gateways_[j]->thread.joinable() || gateways_[j]->failed.load(std::memory_order_seq_cst);
            if (marked[j] || !dead) {
                continue;
            }
            marked[j] = true;
            for (const std::unique_ptr<ShardLanes>& shard : shards_) {
                if (!shard->matching) {
                    continue;
                }
                while (!shard->rings[j]->commands.tryPush(runtime::CommandElement::stopMarker())) {
                    std::this_thread::yield();
                }
                shard->doorbell.ring();
            }
        }
    }

    // Stops the gateway threads, then the matching threads and their logs, and rethrows what any
    // threw, the gateways' first.
    void stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        std::vector<bool> marked(settings_.threads, false);
        markStopped(marked);
        stopRequested_.store(true, std::memory_order_seq_cst);
        released_.store(true, std::memory_order_seq_cst);
        released_.notify_all();
        for (const std::unique_ptr<Gateway>& gateway : gateways_) {
            gateway->sockets->wake();
        }
        for (;;) {
            bool running = false;
            for (const std::unique_ptr<Gateway>& gateway : gateways_) {
                running = running || (gateway->thread.joinable() && !gateway->done.load(std::memory_order_seq_cst));
            }
            if (!running) {
                break;
            }
            markStopped(marked);
            std::this_thread::yield();
        }
        for (const std::unique_ptr<Gateway>& gateway : gateways_) {
            if (gateway->thread.joinable()) {
                gateway->thread.join();
            }
        }
        markStopped(marked);
        std::exception_ptr later;
        for (const std::unique_ptr<ShardLanes>& shard : shards_) {
            if (!shard->matching) {
                continue;
            }
            try {
                shard->matching->join();
            } catch (...) {
                if (!later) {
                    later = std::current_exception();
                }
            }
            if (shard->log) {
                try {
                    shard->logEnd = shard->log->finish();
                } catch (...) {
                    if (!later) {
                        later = std::current_exception();
                    }
                }
            }
        }
        for (const std::unique_ptr<Gateway>& gateway : gateways_) {
            if (gateway->failure) {
                std::rethrow_exception(gateway->failure);
            }
            if (gateway->startFailure) {
                std::rethrow_exception(gateway->startFailure);
            }
        }
        if (later) {
            std::rethrow_exception(later);
        }
    }

    ScalingSettings settings_;
    Cores cores_;
    Meters meters_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> released_{false};
    std::vector<std::vector<std::uint32_t>> clientBooks_;
    std::vector<LoopbackClients> clients_;
    std::vector<std::unique_ptr<ShardLanes>> shards_;
    std::vector<std::unique_ptr<Gateway>> gateways_;
    std::unique_ptr<network::ConnectionHandOff> handOff_;
    std::uint16_t port_ = 0;
    bool stopped_ = false;
};

}  // namespace

const char* placementName(Placement placement) {
    switch (placement) {
        case Placement::EVEN: return "even";
        case Placement::HASH: return "hash";
        case Placement::HOT_ALONE: return "hot-alone";
    }
    return "even";
}

Placement parsePlacement(std::string_view name) {
    if (name == "even") {
        return Placement::EVEN;
    }
    if (name == "hash") {
        return Placement::HASH;
    }
    if (name == "hot-alone") {
        return Placement::HOT_ALONE;
    }
    throw std::invalid_argument("unknown placement '" + std::string(name) + "' (known: even, hash, hot-alone)");
}

const char* spreadName(reference::Spread spread) {
    switch (spread) {
        case reference::Spread::UNIFORM: return "uniform";
        case reference::Spread::SKEWED: return "skewed";
        case reference::Spread::ONE_HOT: return "one-hot";
    }
    return "uniform";
}

reference::Spread parseSpread(std::string_view name) {
    if (name == "uniform") {
        return reference::Spread::UNIFORM;
    }
    if (name == "skewed") {
        return reference::Spread::SKEWED;
    }
    throw std::invalid_argument("unknown spread '" + std::string(name) + "' (known: uniform, skewed)");
}

std::vector<std::size_t> commandShares(std::size_t total, std::size_t instruments, reference::Spread spread) {
    if (instruments == 0 || total < instruments) {
        throw std::invalid_argument("each of " + std::to_string(instruments) + " instruments needs a command");
    }
    std::vector<std::size_t> shares(instruments, total / instruments);
    if (spread == reference::Spread::SKEWED) {
        // One each, then the rest by Zipf weight, rounded down, which never gives more than there
        // is; what rounding leaves goes to the top instrument, which stays the largest.
        double harmonic = 0.0;
        for (std::size_t k = 0; k < instruments; ++k) {
            harmonic += 1.0 / static_cast<double>(k + 1);
        }
        const std::size_t rest = total - instruments;
        std::size_t given = 0;
        for (std::size_t k = 0; k < instruments; ++k) {
            shares[k] = 1 + static_cast<std::size_t>(static_cast<double>(rest) / (static_cast<double>(k + 1) * harmonic));
            given += shares[k];
        }
        if (given > total) {  // only a floating rounding up could
            throw std::logic_error("skewed shares overran the total");
        }
        shares[0] += total - given;
    } else {
        shares[0] += total - (total / instruments) * instruments;
    }
    return shares;
}

std::vector<Workload> spreadWorkloads(const WorkloadConfig& config, std::size_t total, std::uint64_t seed, std::size_t instruments,
                                      reference::Spread spread) {
    const std::vector<std::size_t> shares = commandShares(total, instruments, spread);
    std::vector<Workload> workloads;
    workloads.reserve(instruments);
    for (std::size_t i = 0; i < instruments; ++i) {
        workloads.push_back(generateWorkload(config, shares[i], seed + i));
    }
    return workloads;
}

std::vector<Workload> mixedWorkloads(std::size_t total, std::uint64_t seed, std::size_t instruments, std::size_t shards) {
    static const char* const recipe[] = {"deep-book", "deep-book", "pending-stops", "pending-stops",
                                         "immediate", "immediate", "quote-refresh", "quote-refresh"};
    if (instruments == 0 || total < instruments || shards == 0) {
        throw std::invalid_argument("each of " + std::to_string(instruments) + " instruments needs a command, and a run a shard");
    }
    std::vector<Workload> workloads;
    workloads.reserve(instruments);
    for (std::size_t i = 0; i < instruments; ++i) {
        const std::size_t share = total / instruments + (i == 0 ? total % instruments : 0);
        // By the instrument's place on its shard under even placement, so every shard's eight
        // hold two of each.
        workloads.push_back(generateWorkload(findWorkload(recipe[(i / shards) % std::size(recipe)]), share, seed + i));
    }
    return workloads;
}

std::uint64_t runFingerprint(const std::vector<Workload>& workloads, reference::Spread spread, std::size_t shards, Placement placement) {
    std::vector<std::byte> bytes(8 * (workloads.size() + 4));
    little_endian::store64(bytes, 0, workloads.size());
    little_endian::store64(bytes, 8, static_cast<std::uint64_t>(spread));
    little_endian::store64(bytes, 16, shards);
    little_endian::store64(bytes, 24, static_cast<std::uint64_t>(placement));
    for (std::size_t i = 0; i < workloads.size(); ++i) {
        little_endian::store64(bytes, 8 * (i + 4), workloadFingerprint(workloads[i]));
    }
    return replay::xxh64(bytes);
}

RunPlacement placeInstruments(const std::vector<Workload>& workloads, std::size_t shards, Placement placement) {
    if (shards == 0) {
        throw std::invalid_argument("a run needs a shard");
    }
    if (placement == Placement::HOT_ALONE && shards < 2) {
        throw std::invalid_argument("hot-alone needs a second shard for the rest");
    }
    std::vector<runtime::Instrument> instruments;
    for (std::size_t i = 0; i < workloads.size(); ++i) {
        const BookCapacity capacity = expectedBookSize(workloads[i]);
        std::optional<std::uint32_t> shard;
        if (placement == Placement::EVEN) {
            shard = static_cast<std::uint32_t>(i % shards);
        } else if (placement == Placement::HOT_ALONE) {
            shard = static_cast<std::uint32_t>(i == 0 ? 0 : 1 + (i - 1) % (shards - 1));
        }
        instruments.push_back(runtime::Instrument{instrumentSymbol(i), shard, capacity.orders, capacity.stops, capacity.levels});
    }
    RunPlacement result;
    result.directory = std::make_shared<const runtime::InstrumentDirectory>(instruments, static_cast<std::uint32_t>(shards));
    result.instrumentsOf.resize(shards);
    for (std::size_t i = 0; i < workloads.size(); ++i) {
        const std::optional<runtime::BookLocation> location = result.directory->find(instrumentSymbol(i));
        result.instrumentsOf[location->shard].push_back(static_cast<std::uint32_t>(i));
    }
    for (std::size_t shard = 0; shard < shards; ++shard) {
        if (result.instrumentsOf[shard].empty()) {
            throw std::invalid_argument("shard " + std::to_string(shard) + " of " + std::to_string(shards) + " would hold no book under " +
                                        placementName(placement) + " placement");
        }
    }
    return result;
}

Measurement runScaledOnce(const std::vector<Workload>& workloads, BenchmarkKind kind, const ScalingSettings& settings) {
    if (kind != BenchmarkKind::SHARD && kind != BenchmarkKind::QUEUE && kind != BenchmarkKind::GATEWAY) {
        throw std::invalid_argument(std::string("a scaled run is shard, queue, or gateway, not ") + benchmarkKindName(kind));
    }
    if (settings.threads == 0 || (kind == BenchmarkKind::QUEUE && settings.threads > settings.shards)) {
        throw std::invalid_argument("a queue run needs between 1 and N producers, and a gateway run at least one gateway thread");
    }
    if (kind == BenchmarkKind::SHARD && settings.durability != RunDurability::NONE) {
        throw std::invalid_argument("a shard run has no log");
    }
    if (settings.rollWorker && settings.durability == RunDurability::NONE) {
        throw std::invalid_argument("a roll-forward worker needs a log");
    }
    // Calibrated now, on this thread, rather than lazily on the first to finish while others run.
    static_cast<void>(ThreadCpuTime::shareOf(0, 1.0));
    const RunPlacement placement = placeInstruments(workloads, settings.shards, settings.placement);
    Measurement measurement;
    if (kind == BenchmarkKind::SHARD) {
        runShards(workloads, placement, settings, measurement);
    } else if (kind == BenchmarkKind::QUEUE) {
        runQueues(workloads, placement, settings, measurement);
        measurement.producers = settings.threads;
        measurement.durability = settings.durability;
    } else {
        ShardedGateway gateway(workloads, placement, settings);
        gateway.run(workloads, measurement);
        gateway.finishAndCheck(workloads, measurement);
        measurement.gatewayThreads = settings.threads;
        measurement.window = settings.window;
        measurement.connections = workloads.size();
        measurement.durability = settings.durability;
    }
    measurement.workload = workloads.front().config.name;
    measurement.kind = kind;
    measurement.instruments = workloads.size();
    measurement.shards = settings.shards;
    measurement.spread = spreadName(settings.spread);
    measurement.placement = placementName(settings.placement);
    measurement.pinned = !settings.plan.cores.empty();
    measurement.highPriority = settings.plan.highPriority;
    measurement.layout = settings.plan.layout;
    measurement.spinWindow = kind == BenchmarkKind::SHARD ? std::nullopt : std::optional<std::chrono::nanoseconds>(settings.spinWindow);
    measurement.fingerprint = runFingerprint(workloads, settings.spread, settings.shards, settings.placement);
    for (const Workload& workload : workloads) {
        measurement.commands += workload.timedCount();
        const BookCapacity capacity = expectedBookSize(workload);
        measurement.reservedStops += capacity.stops;
        measurement.reservedLevels += capacity.levels;
    }
    return measurement;
}

std::vector<Measurement> runScaledRepeated(const std::vector<Workload>& workloads, BenchmarkKind kind, const ScalingSettings& scaling,
                                           const RunSettings& settings) {
    std::vector<Measurement> measurements;
    for (std::size_t i = 0; i < settings.warmups + settings.repetitions; ++i) {
        Measurement measurement = runScaledOnce(workloads, kind, scaling);
        measurement.warmup = i < settings.warmups;
        measurement.repetition = measurement.warmup ? i + 1 : i - settings.warmups + 1;
        measurements.push_back(std::move(measurement));
    }
    return measurements;
}

}  // namespace order_book::benchmark
