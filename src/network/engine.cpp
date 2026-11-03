#include "order_book/network/engine.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <exception>
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

        // One shard: a pair of rings per gateway thread, its matching thread, and what that thread
        // reports once it stops.
        struct ShardRuntime {
            ShardRuntime(std::size_t commandSlots, std::size_t responseSlots, std::size_t gatewayThreads) {
                rings.reserve(gatewayThreads);
                for (std::size_t gateway = 0; gateway < gatewayThreads; ++gateway) {
                    rings.push_back(std::make_unique<GatewayRings>(commandSlots, responseSlots));
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

            // Whatever started stops, so a failure anywhere in a start leaves nothing running. Not
            // reached after a forced stop, whose engine is left to the process's exit.
            ~Engine() {
                stop.onRequest({});
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
            }

            EngineExit run();
            bool abandoned() const noexcept { return forced; }

        private:
            void startShards();
            void runShard(std::uint32_t number);
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
            std::vector<std::unique_ptr<ShardRuntime>> shards;
            std::vector<std::unique_ptr<GatewayRuntime>> gateways;  // thread 0's sockets listen
            std::unique_ptr<ConnectionHandOff> handOff;
            std::atomic<bool> gatewaysReleased{false};  // once listening is printed, or to stop
            std::atomic<bool> stopRequested{false};
            std::atomic<std::uint32_t> gatewaysDone{0};
            std::atomic<std::size_t> failedGateway{noGateway};  // the first to fail once running
            std::atomic<std::uint32_t> happenings{0};  // a request or a gateway thread ending, for waitForStop
            bool forced = false;
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
                loop.emplace(*shard, own.doorbell, std::move(rings), spinWindowOf(settings));
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
            own.ready.set_value();
            loop->run();
            // After the stop, with nothing left to allocate for.
            for (std::uint32_t slot = 0; slot < own.books.size(); ++slot) {
                own.books[slot].growths = shard->books().book(slot).capacityGrowths() - growthsAtReady[slot];
            }
            own.resultGrowths = shard->resultGrowths();
        }

        void Engine::startShards() {
            std::vector<std::future<void>> started;
            started.reserve(settings.shards);
            for (std::uint32_t number = 0; number < settings.shards; ++number) {
                shards.push_back(std::make_unique<ShardRuntime>(settings.commandRing, settings.responseRing, settings.gatewayThreads));
                started.push_back(shards.back()->ready.get_future());
            }
            std::exception_ptr failure;
            for (std::uint32_t number = 0; number < settings.shards && !failure; ++number) {
                try {
                    shards[number]->thread = std::thread([this, number] { runShard(number); });
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            // Every thread that started must be heard from before ready goes out of scope.
            for (std::uint32_t number = 0; number < settings.shards; ++number) {
                if (shards[number]->thread.joinable()) {
                    try {
                        started[number].get();
                    } catch (...) {
                        if (!failure) {
                            failure = std::current_exception();
                        }
                    }
                }
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
                    links.push_back(ShardLink{&shard->rings[thread]->commands, &shard->rings[thread]->responses, &shard->doorbell});
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
                text << "shard " << number << ": result-growths=" << shards[number]->resultGrowths << '\n';
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
