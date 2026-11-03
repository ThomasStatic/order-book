#include "order_book/network/engine.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "order_book/network/gateway_thread.hpp"
#include "order_book/network/poll_socket_set.hpp"
#include "order_book/runtime/abort_report.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"
#include "order_book/runtime/thread_placement.hpp"

namespace order_book::network {

    namespace {

        constexpr std::string_view prefix = "order_book_engine: ";

        // What a start refusal names, rather than a usage error, since the command line parsed.
        struct Refusal {
            std::string cause;
        };

        struct BookLine {
            Symbol symbol;
            std::uint32_t shard;
            BookCapacity capacity;
            std::uint64_t growths;  // since ready
        };

        // One gateway thread's two rings to one shard.
        struct GatewayRings {
            GatewayRings(std::size_t commandSlots, std::size_t responseSlots) : commands(commandSlots), responses(responseSlots) {}

            runtime::Ring<runtime::CommandElement> commands;
            runtime::Ring<runtime::ResponseRecord> responses;
        };

        // One shard: a pair of rings per gateway thread, its matching thread, its log writer if
        // it has a log, and what those threads report once they stop.
        struct ShardRuntime {
            ShardRuntime(std::size_t commandSlots, std::size_t responseSlots, std::size_t gatewayThreads, std::size_t logSlots) {
                rings.reserve(gatewayThreads);
                for (std::size_t gateway = 0; gateway < gatewayThreads; ++gateway) {
                    rings.push_back(std::make_unique<GatewayRings>(commandSlots, responseSlots));
                }
                if (logSlots != 0) {
                    logRing = std::make_unique<runtime::Ring<runtime::LogEntry>>(logSlots);
                }
            }

            runtime::Doorbell doorbell;
            std::vector<std::unique_ptr<GatewayRings>> rings;
            // Here, not on the starting thread's stack, since the matching thread may still be
            // inside set_value when get returns, and must find it alive until it is joined.
            std::promise<void> ready;
            std::thread thread;
            std::vector<BookLine> books;
            std::uint64_t resultGrowths = 0;
            bool loopRan = false;  // the matching thread's: its loop tells the log writer to finish

            // With a log. The matching thread alone writes the ring, the log writer alone reads it
            // and publishes the durable position, and the writer names its file once it made it.
            std::unique_ptr<runtime::Ring<runtime::LogEntry>> logRing;
            runtime::DurablePosition durable;
            runtime::Doorbell writerBell;
            std::promise<void> writerReady;
            std::thread writerThread;
            std::string logName;     // named before the writer starts, so marking it made allocates nothing
            bool logMade = false;    // the writer's, once its file exists
            std::uint64_t logEnd = 0;
        };

        // One gateway thread, its sockets, and how its start and its end went.
        struct GatewayRuntime {
            std::unique_ptr<PollSocketSet> sockets;
            std::thread thread;
            // Here, not on the starting thread's stack, since the gateway thread may still be
            // inside set_value when get returns.
            std::promise<void> ready;
            std::exception_ptr failure;  // written by its thread, read once that thread says so
            bool startFailed = false;    // the starting thread's: it never ran, nor sent a marker
        };

        constexpr std::size_t noGateway = static_cast<std::size_t>(-1);

        std::optional<Refusal> refusedValue(const runtime::EngineSettings& settings) {
            if (settings.shards == 0) {
                return Refusal{"--shards must be at least 1"};
            }
            if (settings.gatewayThreads == 0) {
                return Refusal{"--gateway-threads must be at least 1"};
            }
            for (const auto& [flag, slots] : {std::pair{"--command-ring", settings.commandRing}, std::pair{"--response-ring", settings.responseRing}}) {
                if (!std::has_single_bit(slots)) {
                    return Refusal{std::string(flag) + " must be a power of two, not " + std::to_string(slots)};
                }
            }
            if (settings.maxConnections == 0 || settings.maxConnections > mostConnectionsPerThread) {
                return Refusal{"--max-connections must be from 1 to " + std::to_string(mostConnectionsPerThread) + ", not " +
                               std::to_string(settings.maxConnections)};
            }
            if (!settings.pinMatching.empty() && settings.pinMatching.size() != settings.shards) {
                return Refusal{"--pin-matching needs " + std::to_string(settings.shards) + (settings.shards == 1 ? " core" : " cores") +
                               ", one per shard, not " + std::to_string(settings.pinMatching.size())};
            }
            if (!settings.pinGateway.empty() && settings.pinGateway.size() != settings.gatewayThreads) {
                return Refusal{"--pin-gateway needs " + std::to_string(settings.gatewayThreads) +
                               (settings.gatewayThreads == 1 ? " core" : " cores") + ", one per gateway thread, not " +
                               std::to_string(settings.pinGateway.size())};
            }
            if (!std::has_single_bit(settings.logRing)) {
                return Refusal{"--log-ring must be a power of two, not " + std::to_string(settings.logRing)};
            }
            if (!settings.pinLogWriters.empty() && settings.logDir.empty()) {
                return Refusal{"--pin-log-writers needs --log-dir, since there are no log writers without a log"};
            }
            if (!settings.pinLogWriters.empty() && settings.pinLogWriters.size() != settings.shards) {
                return Refusal{"--pin-log-writers needs " + std::to_string(settings.shards) + (settings.shards == 1 ? " core" : " cores") +
                               ", one per shard, not " + std::to_string(settings.pinLogWriters.size())};
            }
            return std::nullopt;
        }

        // On the thread being placed, before it is ready, so a refusal stops the start.
        void placeThisThread(const std::vector<std::uint32_t>& pins, std::size_t index, runtime::ThreadPriority priority,
                             const std::string& name) {
            if (!pins.empty()) {
                if (const std::optional<std::string> refused = runtime::pinThisThread(pins[index])) {
                    throw Refusal{"cannot pin " + name + " to core " + std::to_string(pins[index]) + ": " + *refused};
                }
            }
            if (const std::optional<std::string> refused = runtime::setThisThreadPriority(priority)) {
                throw Refusal{"--thread-priority high was denied: " + *refused};
            }
        }

        std::chrono::nanoseconds spinWindowOf(const runtime::EngineSettings& settings) {
            return settings.spinWindow == runtime::unlimitedSpinSetting ? runtime::unlimitedSpin : settings.spinWindow;
        }

        class Engine {
        public:
            Engine(const runtime::EngineSettings& engineSettings, StopSource& stopSource, std::ostream& reportStream,
                   std::ostream& diagnosticsStream)
                : settings(engineSettings), stop(stopSource), report(reportStream), diagnostics(diagnosticsStream) {}

            Engine(const Engine&) = delete;
            Engine& operator=(const Engine&) = delete;

            // Whatever started stops, so a failure anywhere in a start leaves nothing running, and
            // a start that never became ready leaves no log behind, since nothing ran. Not reached
            // after a forced stop, whose engine is left to the process's exit.
            ~Engine() {
                stop.onRequest({});
                if (housekeeping && !listening) {
                    // Before the release below, so a start that never became ready makes no next
                    // file: nothing ran, so no log writer waits for one.
                    housekeeping->stop();
                }
                // First, since a gateway thread that is ready waits for every shard to stop, and a
                // shard for a marker from every gateway thread. Nothing else writes these rings.
                for (std::size_t thread = 0; thread < gateways.size(); ++thread) {
                    if (!gateways[thread]->thread.joinable() || gateways[thread]->startFailed) {
                        sendStopMarkers(thread);
                    }
                }
                stopRequested.store(true, std::memory_order_seq_cst);
                releaseGateways();
                wakeGateways();
                for (const std::unique_ptr<GatewayRuntime>& gateway : gateways) {
                    if (gateway->thread.joinable()) {
                        gateway->thread.join();
                    }
                }
                for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                    if (shard->thread.joinable()) {
                        shard->thread.join();
                    }
                }
                joinLogWriters();
                finishHousekeeping();
                if (!listening && logStorage) {
                    for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                        if (shard->logMade) {
                            try {
                                logStorage->remove(shard->logName);
                            } catch (const runtime::LogStorageError&) {
                                // Only headers: the next start is refused on it, and says where.
                            }
                        }
                    }
                }
            }

            EngineExit run();
            bool abandoned() const noexcept { return forced; }

        private:
            void startShards();
            void runShard(std::uint32_t number);
            void runLogWriter(std::uint32_t number);
            void joinLogWriters();
            void finishHousekeeping();
            void startGateways();
            void runGatewayThread(std::size_t thread, const GatewaySettings& gatewaySettings);
            void sendStopMarkers(std::size_t thread);
            EngineExit waitForStop();
            void writeStopReport();
            EngineExit forceStop(const std::string& why);
            void releaseGateways() {
                gatewaysReleased.store(true, std::memory_order_seq_cst);
                gatewaysReleased.notify_all();
            }
            void wakeGateways() {
                for (const std::unique_ptr<GatewayRuntime>& gateway : gateways) {
                    if (gateway->sockets) {
                        gateway->sockets->wake();
                    }
                }
            }

            const runtime::EngineSettings& settings;
            StopSource& stop;
            std::ostream& report;
            std::ostream& diagnostics;
            std::unique_ptr<runtime::InstrumentDirectory> directory;
            std::unique_ptr<runtime::LogStorage> logStorage;  // with --log-dir
            std::unique_ptr<runtime::Housekeeping> housekeeping;  // with --log-dir
            std::thread housekeeper;  // once every shard is ready, unpinned, and running once released
            std::vector<std::unique_ptr<ShardRuntime>> shards;
            std::vector<std::unique_ptr<GatewayRuntime>> gateways;  // thread 0's sockets listen
            std::unique_ptr<ConnectionHandOff> handOff;
            std::atomic<bool> gatewaysReleased{false};  // once listening is printed, or to stop
            std::atomic<bool> stopRequested{false};
            std::atomic<std::uint32_t> gatewaysDone{0};
            std::atomic<std::size_t> failedGateway{noGateway};  // the first to fail once running
            std::atomic<std::uint32_t> happenings{0};  // a request or a gateway thread ending, for waitForStop
            bool forced = false;
            bool listening = false;  // once listening is printed
        };

        void Engine::sendStopMarkers(std::size_t thread) {
            // Only for a gateway thread that never started, or failed before it was ready, so it
            // never wrote these rings and never will, and sent no marker of its own.
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                if (shard->thread.joinable()) {
                    while (!shard->rings[thread]->commands.tryPush(runtime::CommandElement::stopMarker())) {
                        std::this_thread::yield();
                    }
                    shard->doorbell.ring();
                }
            }
        }

        void Engine::runShard(std::uint32_t number) {
            // The books are built here, since their thread is the one that builds them.
            std::optional<runtime::Shard> shard;
            std::optional<runtime::MatchingLoop> loop;
            std::vector<std::uint64_t> growthsAtReady;
            ShardRuntime& own = *shards[number];
            try {
                // First, so the books are built on the core they will run on.
                placeThisThread(settings.pinMatching, number, settings.threadPriority, "matching thread " + std::to_string(number));
                const BookCapacity defaults{settings.capacityOrders, settings.capacityStops, settings.capacityLevels};
                shard.emplace(*directory, number, defaults);
                std::vector<runtime::RingPair> rings;
                for (const std::unique_ptr<GatewayRings>& pair : own.rings) {
                    rings.push_back(runtime::RingPair{&pair->commands, &pair->responses});
                }
                std::optional<runtime::LogLink> log;
                if (own.logRing) {
                    shard->watchDurablePosition(own.durable);
                    log = runtime::LogLink{own.logRing.get(), &own.writerBell};
                }
                loop.emplace(*shard, own.doorbell, std::move(rings), spinWindowOf(settings), std::function<void()>{}, log);
                const std::span<const runtime::Instrument> instruments = directory->onShard(number);
                own.books.reserve(instruments.size());
                growthsAtReady.reserve(instruments.size());
                for (std::uint32_t slot = 0; slot < instruments.size(); ++slot) {
                    own.books.push_back(BookLine{instruments[slot].symbol, number, runtime::capacityOf(instruments[slot], defaults), 0});
                    growthsAtReady.push_back(shard->books().book(slot).capacityGrowths());
                }
            } catch (...) {
                own.ready.set_exception(std::current_exception());
                return;
            }
            const runtime::AbortScope abortNamesThisShard(*shard);
            own.loopRan = true;
            own.ready.set_value();
            loop->run();
            // After the stop, with nothing left to allocate for.
            for (std::uint32_t slot = 0; slot < own.books.size(); ++slot) {
                own.books[slot].growths = shard->books().book(slot).capacityGrowths() - growthsAtReady[slot];
            }
            own.resultGrowths = shard->resultGrowths();
        }

        // On its own thread: places itself, then makes its shard's log file, before it is ready,
        // so a refused pin or a file that can't be made refuses the start.
        void Engine::runLogWriter(std::uint32_t number) {
            ShardRuntime& own = *shards[number];
            std::optional<runtime::LogWriter> writer;
            try {
                placeThisThread(settings.pinLogWriters, number, settings.threadPriority, "log writer " + std::to_string(number));
                writer.emplace(*housekeeping, number, *own.logRing, own.durable);
                own.logMade = true;
            } catch (const runtime::LogStorageError& error) {
                own.writerReady.set_exception(std::make_exception_ptr(
                    error.kind() == runtime::LogStorageError::Kind::EXISTS
                        ? Refusal{logStorage->describe(own.logName) +
                                  " already exists: a log from an earlier run, which this build can't recover, so give a directory "
                                  "with no log in it"}
                        : Refusal{error.what()}));
                return;
            } catch (...) {
                own.writerReady.set_exception(std::current_exception());
                return;
            }
            own.writerReady.set_value();
            writer->run(own.writerBell, spinWindowOf(settings));
            own.logEnd = writer->nextPosition();
        }

        // Once every matching thread has been joined, so none still writes a log ring. A shard
        // whose loop never ran never told its writer to finish, and nothing ran on it, so its
        // ring is empty, and the STOP goes in here.
        void Engine::joinLogWriters() {
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                if (!shard->writerThread.joinable()) {
                    continue;
                }
                if (!shard->loopRan) {
                    while (!shard->logRing->tryPush(runtime::LogEntry::stop())) {
                        std::this_thread::yield();
                    }
                    shard->writerBell.ring();
                }
                shard->writerThread.join();
            }
        }

        // Once every log writer has been joined, since one may wait for its next file. A clean
        // stop deletes each next file no writer took, which a failure to delete only reports.
        void Engine::finishHousekeeping() {
            if (housekeeper.joinable()) {
                housekeeping->stop();
                housekeeper.join();
            }
            if (housekeeping) {
                try {
                    housekeeping->removeUnused();
                } catch (const runtime::LogStorageError& error) {
                    diagnostics << prefix << error.what() << std::endl;
                }
            }
        }

        void Engine::startShards() {
            const bool logged = !settings.logDir.empty();
            std::vector<std::future<void>> started;
            std::vector<std::future<void>> writersStarted;
            started.reserve(settings.shards);
            for (std::uint32_t number = 0; number < settings.shards; ++number) {
                shards.push_back(std::make_unique<ShardRuntime>(settings.commandRing, settings.responseRing, settings.gatewayThreads,
                                                                logged ? settings.logRing : 0));
                started.push_back(shards.back()->ready.get_future());
                writersStarted.push_back(shards.back()->writerReady.get_future());
                if (logged) {
                    shards.back()->logName = runtime::logFileName(number, 0);
                }
            }
            std::exception_ptr failure;
            for (std::uint32_t number = 0; number < settings.shards && !failure; ++number) {
                try {
                    if (logged) {
                        shards[number]->writerThread = std::thread([this, number] { runLogWriter(number); });
                    }
                    shards[number]->thread = std::thread([this, number] { runShard(number); });
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            // Every thread that started must be heard from before ready goes out of scope.
            const auto hearFrom = [&failure](const std::thread& thread, std::future<void>& ready) {
                if (thread.joinable()) {
                    try {
                        ready.get();
                    } catch (...) {
                        if (!failure) {
                            failure = std::current_exception();
                        }
                    }
                }
            };
            for (std::uint32_t number = 0; number < settings.shards; ++number) {
                hearFrom(shards[number]->writerThread, writersStarted[number]);
                hearFrom(shards[number]->thread, started[number]);
            }
            if (failure) {
                std::rethrow_exception(failure);
            }
        }

        EngineExit Engine::run() {
            if (const std::optional<Refusal> refusal = refusedValue(settings)) {
                throw *refusal;
            }
            std::ifstream file(settings.instruments, std::ios::binary);
            if (!file) {
                throw Refusal{"cannot read " + settings.instruments};
            }
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            runtime::InstrumentListResult list = runtime::parseInstrumentList(text, settings.shards);
            if (const runtime::InstrumentListError* error = std::get_if<runtime::InstrumentListError>(&list)) {
                throw Refusal{runtime::formatInstrumentListError(settings.instruments, *error)};
            }
            directory = std::make_unique<runtime::InstrumentDirectory>(std::get<std::vector<runtime::Instrument>>(std::move(list)), settings.shards);
            if (!settings.logDir.empty()) {
                std::error_code error;
                if (!std::filesystem::is_directory(settings.logDir, error)) {
                    throw Refusal{"cannot use log directory " + settings.logDir + ": no such directory"};
                }
                // Recovery isn't built yet, so any engine's log here is one this run would lose
                // track of: replay of the directory would then refuse a symbol in two shards.
                std::vector<std::string> earlier;
                for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(settings.logDir)) {
                    const std::string name = entry.path().filename().string();
                    if (name.starts_with("log-") && (name.ends_with(".oblog") || name.ends_with(".oblog.tmp"))) {
                        earlier.push_back(name);
                    }
                }
                if (!earlier.empty()) {
                    std::sort(earlier.begin(), earlier.end());
                    throw Refusal{settings.logDir + " holds " + earlier.front() +
                                  ", a log from an earlier run, which this build can't recover, so give a directory with no log in it"};
                }
                if (const std::uint64_t least = runtime::leastLogFileBytes(*directory); settings.logFileSize < least) {
                    throw Refusal{"--log-file-size must be at least " + std::to_string(least) +
                                  " bytes, the largest shard's header and one record, not " + std::to_string(settings.logFileSize)};
                }
                logStorage = std::make_unique<runtime::FileLogStorage>(settings.logDir);
                housekeeping =
                    std::make_unique<runtime::Housekeeping>(*logStorage, *directory, settings.durability, settings.logFileSize);
            }

            std::vector<SocketSet*> threadSockets;
            for (std::size_t thread = 0; thread < settings.gatewayThreads; ++thread) {
                gateways.push_back(std::make_unique<GatewayRuntime>());
                if (thread == 0) {
                    try {
                        gateways[0]->sockets = std::make_unique<PollSocketSet>(settings.listen.host, settings.listen.port);
                    } catch (const std::system_error& error) {
                        throw Refusal{"cannot listen on " + runtime::formatListenAddress(settings.listen) + ": " + error.what()};
                    }
                } else {
                    gateways[thread]->sockets = std::make_unique<PollSocketSet>(PollSocketSet::NoListener{});
                }
                threadSockets.push_back(gateways[thread]->sockets.get());
            }
            handOff = std::make_unique<ConnectionHandOff>(std::move(threadSockets));
            stop.onRequest([this] {
                stopRequested.store(true, std::memory_order_seq_cst);
                happenings.fetch_add(1, std::memory_order_seq_cst);
                happenings.notify_all();
                wakeGateways();
            });

            startShards();
            if (housekeeping) {
                // Made before ready, so a thread that can't start refuses it, but making nothing
                // until listening is printed, as the gateway threads wait: before ready nothing
                // needs a next file, and a start that ends first stops it before releasing it, so
                // a refused start makes none.
                housekeeper = std::thread([this] {
                    gatewaysReleased.wait(false, std::memory_order_seq_cst);
                    housekeeping->run();
                });
            }
            if (stop.requests() != 0) {
                diagnostics << prefix << "stopped before ready" << std::endl;
                return EngineExit::STOPPED_BEFORE_READY;
            }

            startGateways();
            if (stop.requests() != 0) {
                diagnostics << prefix << "stopped before ready" << std::endl;
                return EngineExit::STOPPED_BEFORE_READY;
            }
            report << "listening "
                   << runtime::formatListenAddress(runtime::ListenAddress{settings.listen.host, gateways[0]->sockets->port()}) << std::endl;
            listening = true;
            releaseGateways();
            return waitForStop();
        }

        // Each gateway thread places itself and is built before anything is printed, so a refused
        // pin or priority, or a failed allocation, refuses the start. Thread 0 starts last and
        // listens only once every other is ready, so no client connects to a start that fails.
        // Each runs only once listening is printed.
        void Engine::startGateways() {
            GatewaySettings gatewaySettings;
            gatewaySettings.maxConnections = settings.maxConnections;
            gatewaySettings.highWaterMark = settings.highWaterMark;
            gatewaySettings.hardLimit = settings.hardLimit;
            gatewaySettings.stopGrace = settings.stopGrace;
            gatewaySettings.spinWindow = spinWindowOf(settings);
            std::vector<std::future<void>> started;
            for (const std::unique_ptr<GatewayRuntime>& gateway : gateways) {
                started.push_back(gateway->ready.get_future());
            }
            for (std::size_t step = 0; step < gateways.size(); ++step) {
                const std::size_t thread = (step + 1) % gateways.size();  // thread 0 last
                gateways[thread]->thread = std::thread([this, thread, gatewaySettings] { runGatewayThread(thread, gatewaySettings); });
                try {
                    started[thread].get();
                } catch (...) {
                    // The destructor sends the markers this thread never will, then joins it.
                    gateways[thread]->startFailed = true;
                    throw;
                }
            }
        }

        void Engine::runGatewayThread(std::size_t thread, const GatewaySettings& gatewaySettings) {
            GatewayRuntime& own = *gateways[thread];
            bool ready = false;
            try {
                placeThisThread(settings.pinGateway, thread, settings.threadPriority,
                                gateways.size() == 1 ? "the gateway thread" : "gateway thread " + std::to_string(thread));
                std::vector<ShardLink> links;
                for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                    links.push_back(ShardLink{&shard->rings[thread]->commands, &shard->rings[thread]->responses, &shard->doorbell,
                                              shard->logRing ? &shard->durable : nullptr});
                }
                GatewayThread gateway(*directory, std::move(links), *own.sockets, gatewaySettings, handOff.get(), thread);
                if (thread == 0) {
                    own.sockets->startListening();
                }
                ready = true;
                own.ready.set_value();
                gatewaysReleased.wait(false, std::memory_order_seq_cst);
                runGateway(gateway, stopRequested);
            } catch (...) {
                own.failure = std::current_exception();
                if (!ready) {
                    own.ready.set_exception(own.failure);
                } else {
                    std::size_t none = noGateway;
                    failedGateway.compare_exchange_strong(none, thread, std::memory_order_seq_cst);
                }
            }
            gatewaysDone.fetch_add(1, std::memory_order_seq_cst);
            happenings.fetch_add(1, std::memory_order_seq_cst);
            happenings.notify_all();
        }

        EngineExit Engine::waitForStop() {
            for (;;) {
                const std::uint32_t seen = happenings.load(std::memory_order_seq_cst);
                if (stop.requests() >= 2) {
                    return forceStop("forced stop");
                }
                if (const std::size_t failed = failedGateway.load(std::memory_order_seq_cst); failed != noGateway) {
                    // The others may wait for ever on its markers, so nothing here waits for them.
                    std::string what = "unknown";
                    try {
                        std::rethrow_exception(gateways[failed]->failure);
                    } catch (const std::exception& error) {
                        what = error.what();
                    } catch (...) {
                    }
                    return forceStop((gateways.size() == 1 ? std::string("gateway thread failed: ")
                                                           : "gateway thread " + std::to_string(failed) + " failed: ") +
                                     what);
                }
                if (gatewaysDone.load(std::memory_order_seq_cst) == gateways.size()) {
                    break;
                }
                happenings.wait(seen, std::memory_order_seq_cst);
            }
            for (const std::unique_ptr<GatewayRuntime>& gateway : gateways) {
                gateway->thread.join();
            }
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                shard->thread.join();
            }
            // Each has written the rest, cut, flushed, and closed its file by now, or is about to.
            joinLogWriters();
            finishHousekeeping();
            writeStopReport();
            return EngineExit::CLEAN_STOP;
        }

        EngineExit Engine::forceStop(const std::string& why) {
            diagnostics << prefix << why << std::endl;
            // Whatever is still running is left to the process's exit: nothing here waits for it.
            forced = true;
            stop.onRequest({});
            for (const std::unique_ptr<GatewayRuntime>& gateway : gateways) {
                if (gateway->thread.joinable()) {
                    gateway->thread.detach();
                }
            }
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                if (shard->thread.joinable()) {
                    shard->thread.detach();
                }
                if (shard->writerThread.joinable()) {
                    shard->writerThread.detach();
                }
            }
            if (housekeeper.joinable()) {
                housekeeper.detach();  // its next files stay, headers with no records, for recovery
            }
            return EngineExit::FORCED_STOP;
        }

        void Engine::writeStopReport() {
            std::vector<BookLine> books;
            std::uint64_t growths = 0;
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                books.insert(books.end(), shard->books.begin(), shard->books.end());
            }
            std::sort(books.begin(), books.end(), [](const BookLine& left, const BookLine& right) { return left.symbol < right.symbol; });
            std::ostringstream text;
            text << "stopped\n";
            for (const BookLine& book : books) {
                text << "book " << book.symbol.text() << " shard=" << book.shard << " orders=" << book.capacity.orders
                     << " stops=" << book.capacity.stops << " levels=" << book.capacity.levels << " growths=" << book.growths << '\n';
                growths += book.growths;
            }
            for (std::uint32_t number = 0; number < shards.size(); ++number) {
                text << "shard " << number << ":";
                if (shards[number]->logRing) {
                    text << " log-end=" << shards[number]->logEnd;
                }
                text << " result-growths=" << shards[number]->resultGrowths << '\n';
            }
            text << "total: books=" << books.size() << " capacity-growths=" << growths << '\n';
            report << text.str() << std::flush;
        }
    }  // namespace

    EngineExit runEngine(const runtime::EngineSettings& settings, StopSource& stop, std::ostream& report, std::ostream& diagnostics) {
        std::unique_ptr<Engine> engine;
        try {
            // Inside the try, since even the record's line allocates.
            diagnostics << prefix << runtime::formatEngineSettings(settings) << std::endl;
            engine = std::make_unique<Engine>(settings, stop, report, diagnostics);
            const EngineExit exit = engine->run();
            if (engine->abandoned()) {
                static_cast<void>(engine.release());  // its threads still use it
            }
            return exit;
        } catch (const Refusal& refusal) {
            engine.reset();
            diagnostics << prefix << "start refused: " << refusal.cause << std::endl;
        } catch (const std::bad_alloc&) {
            engine.reset();
            diagnostics << prefix << "start refused: not enough memory" << std::endl;
        } catch (const std::exception& error) {
            engine.reset();
            diagnostics << prefix << "start refused: " << error.what() << std::endl;
        }
        return EngineExit::START_REFUSED;
    }

}  // namespace order_book::network
