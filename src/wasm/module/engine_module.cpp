#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/threading.h>
#include <emscripten/val.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/cli/commands.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/wasm/browser_engine.hpp"
#include "order_book/wasm/page_connection.hpp"
#include "order_book/wasm/settings.hpp"
#include "order_book/wasm/sandbox.hpp"
#include "order_book/wasm/terminal.hpp"

// order_book_wasm_module (spec #127, "The module and the engine worker"; docs/internals/wasm.md):
// the embind class the engine worker drives and the EM_JS glue that tells it to look. Everything
// the engine's threads report goes into one queue in the order it happened: page-store changes,
// lines, ready, wakes, and the run's end. The worker takes that queue on its own thread, which
// never blocks, and posts what it gives in order, so the page never sees an answer before the
// log write it waited for.
namespace order_book::wasm {

    namespace {

        // Where the module embeds what ships with it (CMakeLists.txt, --embed-file).
        constexpr const char* instrumentListPath = "/engine/instruments.csv";
        constexpr const char* seedPath = "/engine/seed.csv";
        constexpr const char* samplesRoot = "/samples";

        using emscripten::val;
        using runtime::PageLogStorage;

        std::string_view exitName(network::EngineExit exit) {
            switch (exit) {
                case network::EngineExit::CLEAN_STOP: return "CLEAN_STOP";
                case network::EngineExit::START_REFUSED: return "START_REFUSED";
                case network::EngineExit::PLACEMENT_CHANGED: return "PLACEMENT_CHANGED";
                case network::EngineExit::RECOVERY_STOPPED: return "RECOVERY_STOPPED";
                case network::EngineExit::STOPPED_BEFORE_READY: return "STOPPED_BEFORE_READY";
                case network::EngineExit::NOT_PUBLISHED: return "NOT_PUBLISHED";
                case network::EngineExit::FORCED_STOP: return "FORCED_STOP";
            }
            return "UNKNOWN";
        }

        // A copy of bytes in the module's memory as a Uint8Array of its own, which can be
        // transferred: the module's memory is shared, and a view of it can't leave the worker.
        val copyOut(std::span<const std::byte> bytes) {
            const val view(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data())));
            return val::global("Uint8Array").new_(view);
        }

        std::vector<std::byte> copyIn(const val& array) {
            std::vector<std::byte> bytes(array["length"].as<std::size_t>());
            val view(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<unsigned char*>(bytes.data())));
            view.call<void>("set", array);
            return bytes;
        }

        val resultOf(const cli::CommandResult& result) {
            val out = val::object();
            out.set("stdout", result.out);
            out.set("stderr", result.err);
            out.set("exit", result.exit);
            return out;
        }

        const char* sideName(Side side) { return side == Side::BUY ? "BUY" : "SELL"; }

        const char* orderTypeName(OrderType type) {
            switch (type) {
                case OrderType::LIMIT: return "LIMIT";
                case OrderType::MARKET: return "MARKET";
                case OrderType::STOP: return "STOP";
                case OrderType::STOP_LIMIT: return "STOP_LIMIT";
            }
            return "UNKNOWN";
        }

        const char* timeInForceName(TimeInForce timeInForce) {
            switch (timeInForce) {
                case TimeInForce::GTC: return "GTC";
                case TimeInForce::IOC: return "IOC";
                case TimeInForce::FOK: return "FOK";
            }
            return "UNKNOWN";
        }

        const char* modeName(SelfTradePrevention mode) {
            switch (mode) {
                case SelfTradePrevention::CANCEL_RESTING: return "CANCEL_RESTING";
                case SelfTradePrevention::CANCEL_INCOMING: return "CANCEL_INCOMING";
                case SelfTradePrevention::CANCEL_BOTH: return "CANCEL_BOTH";
            }
            return "UNKNOWN";
        }

        // A count as the page's number; every field fits in 2^53.
        double jsNumber(std::uint64_t value) { return static_cast<double>(value); }

        // One answer frame as the page's AnswerEvent: its type, its fields, and its text.
        val eventOf(const network::ClientStream::Answer& frame) {
            val posted = val::object();
            posted.set("text", frame.text);
            if (const auto* refusal = std::get_if<network::RefusedFrame>(&frame.decoded)) {
                posted.set("type", std::string("Refused"));
                posted.set("orderId", jsNumber(refusal->orderId));
                posted.set("code", network::refusedCodeName(refusal->code));
                return posted;
            }
            std::visit(
                [&posted](const auto& event) {
                    using Event = std::decay_t<decltype(event)>;
                    if constexpr (std::is_same_v<Event, OrderAccepted>) {
                        posted.set("type", std::string("OrderAccepted"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("side", std::string(sideName(event.side)));
                        posted.set("price", jsNumber(event.price));
                        posted.set("quantity", jsNumber(event.quantity));
                    } else if constexpr (std::is_same_v<Event, OrderRejected> || std::is_same_v<Event, CancelRejected> ||
                                         std::is_same_v<Event, ReplaceRejected>) {
                        posted.set("type", std::string(std::is_same_v<Event, OrderRejected>    ? "OrderRejected"
                                                       : std::is_same_v<Event, CancelRejected> ? "CancelRejected"
                                                                                                : "ReplaceRejected"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("reason", std::string(rejectionReasonName(event.reason)));
                    } else if constexpr (std::is_same_v<Event, TradeExecuted>) {
                        posted.set("type", std::string("TradeExecuted"));
                        posted.set("incomingOrderId", jsNumber(event.incomingOrderId));
                        posted.set("restingOrderId", jsNumber(event.restingOrderId));
                        posted.set("price", jsNumber(event.executionPrice));
                        posted.set("quantity", jsNumber(event.executionQuantity));
                    } else if constexpr (std::is_same_v<Event, OrderCancelled>) {
                        posted.set("type", std::string("OrderCancelled"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("quantity", jsNumber(event.cancelledQuantity));
                    } else if constexpr (std::is_same_v<Event, OrderExpired>) {
                        posted.set("type", std::string("OrderExpired"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("quantity", jsNumber(event.expiredQuantity));
                    } else if constexpr (std::is_same_v<Event, SelfTradePrevented>) {
                        posted.set("type", std::string("SelfTradePrevented"));
                        posted.set("incomingOrderId", jsNumber(event.incomingOrderId));
                        posted.set("restingOrderId", jsNumber(event.restingOrderId));
                        posted.set("price", jsNumber(event.price));
                        posted.set("quantity", jsNumber(event.cancelledQuantity));
                    } else if constexpr (std::is_same_v<Event, StopTriggered>) {
                        posted.set("type", std::string("StopTriggered"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("stopPrice", jsNumber(event.stopPrice));
                        posted.set("tradePrice", jsNumber(event.tradePrice));
                    } else {
                        static_assert(std::is_same_v<Event, OrderReplaced>, "every BookEvent alternative is an AnswerEvent");
                        posted.set("type", std::string("OrderReplaced"));
                        posted.set("orderId", jsNumber(event.orderId));
                        posted.set("side", std::string(sideName(event.side)));
                        posted.set("price", jsNumber(event.price));
                        posted.set("quantity", jsNumber(event.quantity));
                        posted.set("openQuantity", jsNumber(event.openQuantity));
                        posted.set("priorityKept", event.priorityKept);
                    }
                },
                std::get<BookEvent>(frame.decoded));
            return posted;
        }

        // A sandbox command's answer, or its loss, as the worker matches it to its request.
        val sandboxAnswerOf(const SandboxAnswer& answer) {
            val posted = val::object();
            posted.set("type", std::string("sandboxAnswer"));
            posted.set("correlation", static_cast<double>(answer.correlation));
            if (answer.orderId) {
                posted.set("orderId", jsNumber(*answer.orderId));
            }
            val events = val::array();
            for (const network::ClientStream::Answer& frame : answer.events) {
                events.call<void>("push", eventOf(frame));
            }
            posted.set("events", events);
            posted.set("text", answer.text);
            posted.set("frameBytes", static_cast<double>(answer.frameBytes));
            if (!answer.lost.empty()) {
                posted.set("lost", answer.lost);
            }
            return posted;
        }

        // A field as the text its replay column takes, as JavaScript's String() writes it, so the
        // parser judges it exactly as it would a row's: 100.7, -5, NaN, or "ten" each refused with
        // the parser's own message, never rounded. Nothing for a field left out.
        std::string textOf(const val& field) {
            if (field.isUndefined() || field.isNull()) {
                return {};
            }
            return val::global("String")(field).as<std::string>();
        }

        // What the sandbox sends, as the page's OrderInput gives it.
        OrderInput orderOf(const val& order) {
            OrderInput input;
            if (order.isUndefined() || order.isNull()) {
                return input;
            }
            input.side = textOf(order["side"]);
            input.quantity = textOf(order["quantity"]);
            input.price = textOf(order["price"]);
            input.type = textOf(order["type"]);
            input.stopPrice = textOf(order["stopPrice"]);
            input.timeInForce = textOf(order["timeInForce"]);
            input.postOnly = order["postOnly"].isTrue();
            input.owner = textOf(order["owner"]);
            input.selfTradePrevention = textOf(order["selfTradePrevention"]);
            return input;
        }

        // Bytes the page says it consumed: a whole count, at most what any window could hold.
        std::size_t bytesOf(double bytes) {
            if (!(bytes > 0)) {
                return 0;
            }
            return bytes >= 4294967295.0 ? static_cast<std::size_t>(4294967295U) : static_cast<std::size_t>(bytes);
        }

        // What the engine reported, in order, as the worker takes it.
        struct StoreChange {
            PageLogStorage::Change::Kind kind;
            std::string name;
            std::string to;
            std::uint64_t offset;
            std::vector<std::byte> bytes;
            std::uint64_t size;
        };
        struct Line {
            std::string stream;  // "report" or "diagnostics"
            std::string text;
        };
        struct Ready {};
        struct Wake {};
        struct Exited {
            network::EngineExit exit;
        };
        using Event = std::variant<StoreChange, Line, Ready, Wake, Exited>;

        // The worker's look at the queue, on its own thread.
        EM_JS(void, order_book_look, (), {
            const look = Module["onEngineEvents"];
            if (look) {
                look();
            }
        });

        void lookOnMainThread() { order_book_look(); }

    }  // namespace

    // The engine as the worker holds it: one per module, run again after each clean stop.
    class ModuleEngine {
    public:
        // The page store as the page holds it: [{name, bytes: Uint8Array}].
        explicit ModuleEngine(const val& store)
            : engine(instrumentListPath, storeOf(store), callbacks()), terminal(engine.shadows(), engine.store()), sandbox(engine.shadows()) {}

        ModuleEngine(const ModuleEngine&) = delete;
        ModuleEngine& operator=(const ModuleEngine&) = delete;

        // The module ends with its worker, so this runs only if the page deletes the object: a
        // run still going is forced to stop, which returns at once, and its thread joined, since
        // it holds this.
        ~ModuleEngine() {
            if (isRunning && stopping) {
                stopping->request();
                stopping->request();
            }
            if (runner.joinable()) {
                runner.join();
            }
        }

        // Starts a run on a pthread of its own, returning at once: {ok} or {ok: false, error}.
        val start() {
            val reply = val::object();
            reply.set("ok", false);
            if (isRunning) {
                reply.set("error", std::string("the engine is already running"));
                return reply;
            }
            try {
                if (runner.joinable()) {
                    // A run whose end has been taken: its thread queued that end as it returned,
                    // so this waits only for it to finish returning to the pool.
                    runner.join();
                }
                readySeen = false;
                announced = false;
                seeding.reset();
                seeded.reset();
                stopping = std::make_unique<network::StopSource>();
                runner = std::thread([this, stop = stopping.get()] { runOnce(*stop); });
            } catch (const std::exception& error) {
                reply.set("error", "the engine can't start: " + std::string(error.what()));
                return reply;
            }
            isRunning = true;
            reply.set("ok", true);
            return reply;
        }

        // A first stop starts the clean stop, a second forces it.
        void stop() {
            if (isRunning && stopping) {
                stopping->request();
            }
        }

        bool running() const { return isRunning; }

        // The sandbox's typed commands: {ok: true, correlation, orderId?} once sent, its answer
        // to come as a `sandboxAnswer` event, or {ok: false, error}; with `events`, as run's.
        val submit(const std::string& symbol, const val& order) { return sent(sandbox.submit(symbol, orderOf(order))); }
        val cancel(const std::string& symbol, const val& orderId) { return sent(sandbox.cancel(symbol, textOf(orderId))); }
        val replace(const std::string& symbol, const val& orderId, const val& price, const val& quantity) {
            return sent(sandbox.replace(symbol, textOf(orderId), textOf(price), textOf(quantity)));
        }

        // The page has consumed this many bytes of the sandbox's answers or the terminal's
        // reading: what that lets through.
        val consumed(const std::string& connection, double bytes) {
            if (connection == "terminal") {
                terminal.consumed(bytesOf(bytes));
            } else {
                sandbox.consumed(bytesOf(bytes));
            }
            return takeEvents();
        }

        // The shadow's book: levels with their orders and owners, pending stops, and the last
        // trade price; null for a symbol off the list.
        val book(const std::string& text) const {
            const std::optional<replay::Symbol> symbol = replay::Symbol::parse(text);
            if (!symbol || !engine.shadows().shardOf(*symbol)) {
                return val::null();
            }
            const BookView view = engine.shadows().book(*symbol);
            const auto side = [](const std::vector<LevelView>& levels) {
                val out = val::array();
                for (const LevelView& level : levels) {
                    val posted = val::object();
                    posted.set("price", jsNumber(level.price));
                    posted.set("quantity", jsNumber(level.quantity));
                    val orders = val::array();
                    for (const RestingOrderView& order : level.orders) {
                        val resting = val::object();
                        resting.set("orderId", jsNumber(order.orderId));
                        resting.set("quantity", jsNumber(order.remainingQuantity));
                        resting.set("initialQuantity", jsNumber(order.initialQuantity));
                        if (order.owner != 0) {
                            resting.set("owner", jsNumber(order.owner));
                        }
                        orders.call<void>("push", resting);
                    }
                    posted.set("orders", orders);
                    out.call<void>("push", posted);
                }
                return out;
            };
            val posted = val::object();
            posted.set("symbol", text);
            posted.set("bids", side(view.bids));
            posted.set("asks", side(view.asks));
            val stops = val::array();
            for (const PendingStopView& stop : view.stops) {
                val pending = val::object();
                pending.set("orderId", jsNumber(stop.orderId));
                pending.set("side", std::string(sideName(stop.side)));
                pending.set("type", std::string(orderTypeName(stop.orderType)));
                pending.set("stopPrice", jsNumber(stop.stopPrice));
                if (stop.orderType == OrderType::STOP_LIMIT) {
                    pending.set("price", jsNumber(stop.limitPrice));
                }
                pending.set("quantity", jsNumber(stop.quantity));
                pending.set("timeInForce", std::string(timeInForceName(stop.timeInForce)));
                if (stop.owner != 0) {
                    pending.set("owner", jsNumber(stop.owner));
                    pending.set("selfTradePrevention", std::string(modeName(stop.selfTradePrevention)));
                }
                pending.set("postOnly", stop.postOnly);
                stops.call<void>("push", pending);
            }
            posted.set("stops", stops);
            posted.set("lastTradePrice", view.lastTradePrice ? val(jsNumber(*view.lastTradePrice)) : val::null());
            return posted;
        }

        // Each shard's instruments and the records its log holds.
        val status() const {
            val shards = val::array();
            const runtime::InstrumentDirectory& directory = engine.shadows().instruments();
            for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
                val posted = val::object();
                val instruments = val::array();
                for (const runtime::Instrument& instrument : directory.onShard(shard)) {
                    instruments.call<void>("push", instrument.symbol.text());
                }
                posted.set("shard", static_cast<double>(shard));
                posted.set("instruments", instruments);
                posted.set("records", jsNumber(engine.shadows().records(shard)));
                shards.call<void>("push", posted);
            }
            return shards;
        }

        // One typed line: {done: true, stdout, stderr, exit}, or {done: false} while its answer
        // is due, which then comes as an `answer` event. Either way `events` holds what was
        // queued before it, which the worker posts first: an answer that came at once may have
        // waited for a log write still in the queue.
        val run(const std::string& line) {
            std::optional<cli::CommandResult> result;
            try {
                result = terminal.run(line);
            } catch (const std::exception& error) {
                result = cli::CommandResult{"", "order_book_cli: " + std::string(error.what()) + "\n", cli::engineFailure};
            }
            val reply = result ? resultOf(*result) : val::object();
            reply.set("done", result.has_value());
            reply.set("events", takeEvents());
            return reply;
        }

        // Everything queued since the last look, in order, as the worker posts it, then what
        // the wakes, ready, and the run's end gave: the seed's progress, the terminal's answer,
        // `seeded`, `ready`, and `exited`. The engine writes a record to the store before it
        // sends the answer that waited for it, so once pumping has read an answer, its write is
        // in the queue: the queue is taken again after pumping, and what pumping gave goes last.
        // Anything thrown here, which only memory running out can, is posted as `aborted`.
        val takeEvents() {
            scheduled = false;
            val out = val::array();
            try {
                val pumped = val::array();
                takeQueued(out, pumped);
                pump(pumped);
                takeQueued(out, pumped);
                const std::size_t count = pumped["length"].as<std::size_t>();
                for (std::size_t i = 0; i < count; ++i) {
                    out.call<void>("push", pumped[i]);
                }
            } catch (const std::exception& error) {
                val posted = val::object();
                posted.set("type", std::string("aborted"));
                posted.set("message", std::string(error.what()));
                out.call<void>("push", posted);
            }
            return out;
        }

    private:
        static std::vector<PageLogStorage::StoredFile> storeOf(const val& store) {
            std::vector<PageLogStorage::StoredFile> files;
            const std::size_t count = store["length"].as<std::size_t>();
            for (std::size_t i = 0; i < count; ++i) {
                const val file = store[i];
                files.push_back(PageLogStorage::StoredFile{file["name"].as<std::string>(), copyIn(file["bytes"])});
            }
            return files;
        }

        BrowserCallbacks callbacks() {
            BrowserCallbacks told;
            told.wake = [this] { queue(Wake{}); };
            told.stored = [this](const PageLogStorage::Change& change) {
                queue(StoreChange{change.kind, change.name, change.to, change.offset,
                                  std::vector<std::byte>(change.bytes.begin(), change.bytes.end()), change.size});
            };
            told.ready = [this] { queue(Ready{}); };
            return told;
        }

        // The queue as it stands: changes and lines into out in order, ready and the run's end
        // acted on where they fall, which may give more.
        void takeQueued(val& out, val& pumped) {
            std::vector<Event> taken;
            {
                const std::lock_guard<std::mutex> held(guard);
                taken.swap(events);
            }
            for (Event& event : taken) {
                if (auto* change = std::get_if<StoreChange>(&event)) {
                    out.call<void>("push", changeEvent(*change));
                } else if (auto* line = std::get_if<Line>(&event)) {
                    val posted = val::object();
                    posted.set("type", std::string("line"));
                    posted.set("stream", line->stream);
                    posted.set("text", line->text);
                    out.call<void>("push", posted);
                } else if (std::holds_alternative<Ready>(event)) {
                    readySeen = true;
                } else if (auto* exited = std::get_if<Exited>(&event)) {
                    pump(pumped);  // what came before the end
                    finish(pumped, exited->exit);
                }
                // A wake asks only for the pump, which every look does.
            }
        }

        // From any thread: the worker looks once, however many events come before it does.
        void queue(Event event) {
            {
                const std::lock_guard<std::mutex> held(guard);
                events.push_back(std::move(event));
            }
            if (!scheduled.exchange(true)) {
                if (emscripten_is_main_runtime_thread()) {
                    emscripten_async_call([](void*) { lookOnMainThread(); }, nullptr, 0);
                } else {
                    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, &lookOnMainThread);
                }
            }
        }

        // The engine's lines as `line` events, each whole line as it ends.
        class LineStream final : public std::streambuf {
        public:
            LineStream(ModuleEngine& engine, std::string name) : owner(engine), stream(std::move(name)) {}

            void flushPartial() {
                const std::lock_guard<std::mutex> held(guard);
                if (!pending.empty()) {
                    owner.queue(Line{stream, std::exchange(pending, {})});
                }
            }

        protected:
            int_type overflow(int_type character) override {
                if (traits_type::eq_int_type(character, traits_type::eof())) {
                    return traits_type::not_eof(character);
                }
                const std::lock_guard<std::mutex> held(guard);
                take(traits_type::to_char_type(character));
                return character;
            }

            std::streamsize xsputn(const char* text, std::streamsize count) override {
                const std::lock_guard<std::mutex> held(guard);
                for (std::streamsize i = 0; i < count; ++i) {
                    take(text[i]);
                }
                return count;
            }

        private:
            void take(char character) {
                if (character == '\n') {
                    owner.queue(Line{stream, std::exchange(pending, {})});
                } else {
                    pending.push_back(character);
                }
            }

            ModuleEngine& owner;
            std::string stream;
            std::mutex guard;
            std::string pending;
        };

        // The run's own pthread: runEngine's, the last of the module's pool.
        void runOnce(network::StopSource& stop) {
            LineStream reportLines(*this, "report");
            LineStream diagnosticLines(*this, "diagnostics");
            std::ostream report(&reportLines);
            std::ostream diagnostics(&diagnosticLines);
            network::EngineExit exit = network::EngineExit::START_REFUSED;
            try {
                exit = engine.run(stop, report, diagnostics);
            } catch (const std::exception& error) {
                diagnostics << "order_book_engine: " << error.what() << '\n';
            }
            report.flush();
            diagnostics.flush();
            reportLines.flushPartial();
            diagnosticLines.flushPartial();
            queue(Exited{exit});
        }

        // On each look: the seed, sent over a connection of its own on a start with an empty
        // store, then `seeded` and `ready`, with the terminal connected; and the terminal's
        // answer once it comes.
        void pump(val& out) {
            if (!isRunning) {
                return;
            }
            if (readySeen && !announced) {
                if (engine.startedEmpty() && !seeding && !seeded) {
                    const std::string address = runtime::formatListenAddress(browserListen);
                    const std::vector<std::string_view> args{seedPath, "--connect", address};
                    std::variant<PageSend, cli::CommandResult> started = startSend(engine.sockets(), args);
                    if (auto* refused = std::get_if<cli::CommandResult>(&started)) {
                        seeded = *refused;
                    } else {
                        seeding.emplace(std::move(std::get<PageSend>(started)));
                    }
                }
                if (seeding) {
                    if (std::optional<cli::CommandResult> result = seeding->pump()) {
                        seeded = std::move(*result);
                        seeding.reset();
                    }
                }
                if (!engine.startedEmpty() || seeded) {
                    if (seeded) {
                        val posted = resultOf(*seeded);
                        posted.set("type", std::string("seeded"));
                        out.call<void>("push", posted);
                    }
                    announced = true;
                    terminal.connect(engine.sockets());
                    sandbox.connect(engine.sockets());
                    out.call<void>("push", readyEvent());
                }
            }
            answer(out, terminal.pump());
            for (const SandboxAnswer& answered : sandbox.pump()) {
                out.call<void>("push", sandboxAnswerOf(answered));
            }
            announce(out);
        }

        // What the terminal read since it last said, for the page to acknowledge.
        void announce(val& out) {
            if (const std::size_t bytes = terminal.progress(); bytes != 0) {
                val posted = val::object();
                posted.set("type", std::string("progress"));
                posted.set("connection", std::string("terminal"));
                posted.set("bytes", static_cast<double>(bytes));
                out.call<void>("push", posted);
            }
        }

        val sent(const std::variant<SandboxSent, std::string>& result) {
            val reply = val::object();
            if (const auto* problem = std::get_if<std::string>(&result)) {
                reply.set("ok", false);
                reply.set("error", *problem);
            } else {
                const SandboxSent& command = std::get<SandboxSent>(result);
                reply.set("ok", true);
                reply.set("correlation", static_cast<double>(command.correlation));
                if (command.orderId) {
                    reply.set("orderId", jsNumber(*command.orderId));
                }
            }
            reply.set("events", takeEvents());
            return reply;
        }

        void answer(val& out, const std::optional<cli::CommandResult>& result) {
            if (result) {
                val posted = resultOf(*result);
                posted.set("type", std::string("answer"));
                out.call<void>("push", posted);
            }
        }

        // The run has returned: a command still waiting is reported lost, then `exited`.
        void finish(val& out, network::EngineExit exit) {
            seeding.reset();
            seeded.reset();
            announced = true;
            terminal.disconnect();
            answer(out, terminal.pump());
            for (const SandboxAnswer& lost : sandbox.disconnect()) {
                out.call<void>("push", sandboxAnswerOf(lost));
            }
            isRunning = false;
            val posted = val::object();
            posted.set("type", std::string("exited"));
            posted.set("code", static_cast<int>(exit));
            posted.set("name", std::string(exitName(exit)));
            out.call<void>("push", posted);
        }

        val readyEvent() const {
            val posted = val::object();
            posted.set("type", std::string("ready"));
            val instruments = val::array();
            val placement = val::array();
            const runtime::InstrumentDirectory& directory = engine.shadows().instruments();
            for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
                val onShard = val::array();
                for (const runtime::Instrument& instrument : directory.onShard(shard)) {
                    instruments.call<void>("push", instrument.symbol.text());
                    onShard.call<void>("push", instrument.symbol.text());
                }
                placement.call<void>("push", onShard);
            }
            posted.set("instruments", instruments);
            posted.set("placement", placement);
            return posted;
        }

        // One page-store change as the page applies it: its type, its name, and what the type needs.
        static val changeEvent(const StoreChange& change) {
            val posted = val::object();
            posted.set("name", change.name);
            const auto typed = [&posted](const char* type) { posted.set("type", std::string(type)); };
            switch (change.kind) {
                case PageLogStorage::Change::Kind::CREATE: typed("create"); break;
                case PageLogStorage::Change::Kind::REMOVE: typed("remove"); break;
                case PageLogStorage::Change::Kind::WRITE:
                    typed("write");
                    posted.set("offset", static_cast<double>(change.offset));
                    posted.set("bytes", copyOut(change.bytes));
                    break;
                case PageLogStorage::Change::Kind::EXTEND:
                    typed("extend");
                    posted.set("size", static_cast<double>(change.size));
                    break;
                case PageLogStorage::Change::Kind::CUT:
                    typed("cut");
                    posted.set("size", static_cast<double>(change.size));
                    break;
                case PageLogStorage::Change::Kind::RENAME:
                    typed("rename");
                    posted.set("to", change.to);
                    break;
                case PageLogStorage::Change::Kind::REPLACE:
                    typed("replace");
                    posted.set("to", change.to);
                    break;
            }
            return posted;
        }

        std::mutex guard;
        std::vector<Event> events;  // under the guard
        std::atomic<bool> scheduled{false};

        BrowserEngine engine;
        Terminal terminal;
        Sandbox sandbox;
        std::unique_ptr<network::StopSource> stopping;
        std::thread runner;
        std::atomic<bool> isRunning{false};

        // The worker's thread alone touches these.
        bool readySeen = false;
        bool announced = false;
        std::optional<PageSend> seeding;
        std::optional<cli::CommandResult> seeded;
    };

    // CMake sizes the module's strict pool from settings.hpp's counts; this holds it to the
    // formula there.
    static_assert(browserEngineThreads == ORDER_BOOK_WASM_POOL, "the module's pool is the engine's threads");

    // Makes the samples' root the working directory, as the terminal's file commands expect: 0,
    // or chdir's -1.
    int enterSamples() { return ::chdir(samplesRoot); }

    // The release the module was built as.
    std::string release() { return ORDER_BOOK_WASM_RELEASE; }

    EMSCRIPTEN_BINDINGS(order_book_wasm_module) {
        emscripten::class_<ModuleEngine>("Engine")
            .constructor<const val&>()
            .function("start", &ModuleEngine::start)
            .function("stop", &ModuleEngine::stop)
            .function("running", &ModuleEngine::running)
            .function("run", &ModuleEngine::run)
            .function("takeEvents", &ModuleEngine::takeEvents)
            .function("submit", &ModuleEngine::submit)
            .function("cancel", &ModuleEngine::cancel)
            .function("replace", &ModuleEngine::replace)
            .function("consumed", &ModuleEngine::consumed)
            .function("book", &ModuleEngine::book)
            .function("status", &ModuleEngine::status);
        emscripten::function("enterSamples", &enterSamples);
        emscripten::function("release", &release);
    }

}  // namespace order_book::wasm
