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
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

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

        // One shard: its rings, its matching thread, and what that thread reports once it stops.
        struct ShardRuntime {
            ShardRuntime(std::size_t commandSlots, std::size_t responseSlots) : commands(commandSlots), responses(responseSlots) {}

            runtime::Doorbell doorbell;
            runtime::Ring<runtime::CommandElement> commands;
            runtime::Ring<runtime::ResponseRecord> responses;
            // Here, not on the starting thread's stack, since the matching thread may still be
            // inside set_value when get returns, and must find it alive until it is joined.
            std::promise<void> ready;
            std::thread thread;
            std::vector<BookLine> books;
            std::uint64_t resultGrowths = 0;
        };

        std::optional<Refusal> refusedValue(const runtime::EngineSettings& settings) {
            if (settings.shards == 0) {
                return Refusal{"--shards must be at least 1"};
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
            return std::nullopt;
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
                if (gateway.joinable()) {
                    stopRequested.store(true, std::memory_order_seq_cst);
                    sockets->wake();
                    gateway.join();
                    if (gatewayFailure) {
                        sendStopMarkers();
                    }
                } else {
                    sendStopMarkers();
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
            void sendStopMarkers();
            EngineExit waitForStop();
            void writeStopReport();
            EngineExit forceStop(const std::string& why);

            const runtime::EngineSettings& settings;
            StopSource& stop;
            std::ostream& report;
            std::ostream& diagnostics;
            std::unique_ptr<runtime::InstrumentDirectory> directory;
            std::unique_ptr<PollSocketSet> sockets;
            std::vector<std::unique_ptr<ShardRuntime>> shards;
            std::thread gateway;
            std::exception_ptr gatewayFailure;
            std::atomic<bool> stopRequested{false};
            std::atomic<bool> gatewayDone{false};
            std::atomic<std::uint32_t> happenings{0};  // a request or the gateway ending, for waitForStop
            bool forced = false;
        };

        void Engine::sendStopMarkers() {
            // Only while no gateway thread writes these rings: before it starts, or after it failed.
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                if (shard->thread.joinable()) {
                    while (!shard->commands.tryPush(runtime::CommandElement::stopMarker())) {
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
                const BookCapacity defaults{settings.capacityOrders, settings.capacityStops, settings.capacityLevels};
                shard.emplace(*directory, number, defaults);
                loop.emplace(*shard, own.doorbell, std::vector<runtime::RingPair>{runtime::RingPair{&own.commands, &own.responses}},
                             spinWindowOf(settings));
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
                shards.push_back(std::make_unique<ShardRuntime>(settings.commandRing, settings.responseRing));
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

            try {
                sockets = std::make_unique<PollSocketSet>(settings.listen.host, settings.listen.port);
            } catch (const std::system_error& error) {
                throw Refusal{"cannot listen on " + runtime::formatListenAddress(settings.listen) + ": " + error.what()};
            }
            stop.onRequest([this] {
                stopRequested.store(true, std::memory_order_seq_cst);
                happenings.fetch_add(1, std::memory_order_seq_cst);
                happenings.notify_all();
                sockets->wake();
            });

            startShards();
            if (stop.requests() != 0) {
                diagnostics << prefix << "stopped before ready" << std::endl;
                return EngineExit::STOPPED_BEFORE_READY;
            }

            sockets->startListening();
            report << "listening " << runtime::formatListenAddress(runtime::ListenAddress{settings.listen.host, sockets->port()}) << std::endl;

            GatewaySettings gatewaySettings;
            gatewaySettings.maxConnections = settings.maxConnections;
            gatewaySettings.highWaterMark = settings.highWaterMark;
            gatewaySettings.hardLimit = settings.hardLimit;
            gatewaySettings.stopGrace = settings.stopGrace;
            gatewaySettings.spinWindow = spinWindowOf(settings);
            std::vector<ShardLink> links;
            for (const std::unique_ptr<ShardRuntime>& shard : shards) {
                links.push_back(ShardLink{&shard->commands, &shard->responses, &shard->doorbell});
            }
            gateway = std::thread([this, links = std::move(links), gatewaySettings = std::move(gatewaySettings)] {
                try {
                    GatewayThread gatewayThread(*directory, links, *sockets, gatewaySettings);
                    runGateway(gatewayThread, stopRequested);
                } catch (...) {
                    gatewayFailure = std::current_exception();
                }
                gatewayDone.store(true, std::memory_order_seq_cst);
                happenings.fetch_add(1, std::memory_order_seq_cst);
                happenings.notify_all();
            });
            return waitForStop();
        }

        EngineExit Engine::waitForStop() {
            for (;;) {
                const std::uint32_t seen = happenings.load(std::memory_order_seq_cst);
                if (stop.requests() >= 2) {
                    return forceStop("forced stop");
                }
                if (gatewayDone.load(std::memory_order_seq_cst)) {
                    break;
                }
                happenings.wait(seen, std::memory_order_seq_cst);
            }
            gateway.join();
            if (gatewayFailure) {
                std::string what = "unknown";
                try {
                    std::rethrow_exception(gatewayFailure);
                } catch (const std::exception& error) {
                    what = error.what();
                } catch (...) {
                }
                sendStopMarkers();
                return forceStop("gateway thread failed: " + what);
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
            if (gateway.joinable()) {
                gateway.detach();
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
