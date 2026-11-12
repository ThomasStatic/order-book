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

        std::string_view changeName(PageLogStorage::Change::Kind kind) {
            switch (kind) {
                case PageLogStorage::Change::Kind::CREATE: return "create";
                case PageLogStorage::Change::Kind::WRITE: return "write";
                case PageLogStorage::Change::Kind::EXTEND: return "extend";
                case PageLogStorage::Change::Kind::CUT: return "cut";
                case PageLogStorage::Change::Kind::RENAME: return "rename";
                case PageLogStorage::Change::Kind::REPLACE: return "replace";
                case PageLogStorage::Change::Kind::REMOVE: return "remove";
            }
            return "unknown";
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
            : engine(instrumentListPath, storeOf(store), callbacks()), terminal(engine.shadows(), engine.store()) {}

        ModuleEngine(const ModuleEngine&) = delete;
        ModuleEngine& operator=(const ModuleEngine&) = delete;

        // The module ends with its worker; a run still going keeps its thread.
        ~ModuleEngine() {
            if (runner.joinable()) {
                runner.detach();
            }
        }

        // Starts a run on a pthread of its own, returning at once: {ok} or {ok: false, error}.
        val start() {
            val reply = val::object();
            if (isRunning) {
                reply.set("ok", false);
                reply.set("error", std::string("the engine is already running"));
                return reply;
            }
            if (runner.joinable()) {
                runner.join();  // a run whose end has been taken: its thread has returned
            }
            isRunning = true;
            readySeen = false;
            announced = false;
            seeding.reset();
            stopping = std::make_unique<network::StopSource>();
            runner = std::thread([this, stop = stopping.get()] { runOnce(*stop); });
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
        val takeEvents() {
            scheduled = false;
            val out = val::array();
            val gave = val::array();
            takeQueued(out, gave);
            pump(gave);
            takeQueued(out, gave);
            const std::size_t count = gave["length"].as<std::size_t>();
            for (std::size_t i = 0; i < count; ++i) {
                out.call<void>("push", gave[i]);
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
        void takeQueued(val& out, val& gave) {
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
                    pump(gave);  // what came before the end
                    finish(gave, exited->exit);
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
            LineStream(ModuleEngine& owner, std::string name) : engine(owner), stream(std::move(name)) {}

            void flushPartial() {
                const std::lock_guard<std::mutex> held(guard);
                if (!pending.empty()) {
                    engine.queue(Line{stream, std::exchange(pending, {})});
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
                    engine.queue(Line{stream, std::exchange(pending, {})});
                } else {
                    pending.push_back(character);
                }
            }

            ModuleEngine& engine;
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
                    out.call<void>("push", readyEvent());
                }
            }
            answer(out, terminal.pump());
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

        static val changeEvent(const StoreChange& change) {
            val posted = val::object();
            posted.set("type", std::string(changeName(change.kind)));
            posted.set("name", change.name);
            switch (change.kind) {
                case PageLogStorage::Change::Kind::WRITE:
                    posted.set("offset", static_cast<double>(change.offset));
                    posted.set("bytes", copyOut(change.bytes));
                    break;
                case PageLogStorage::Change::Kind::EXTEND:
                case PageLogStorage::Change::Kind::CUT:
                    posted.set("size", static_cast<double>(change.size));
                    break;
                case PageLogStorage::Change::Kind::RENAME:
                case PageLogStorage::Change::Kind::REPLACE:
                    posted.set("to", change.to);
                    break;
                case PageLogStorage::Change::Kind::CREATE:
                case PageLogStorage::Change::Kind::REMOVE:
                    break;
            }
            return posted;
        }

        std::mutex guard;
        std::vector<Event> events;  // under the guard
        std::atomic<bool> scheduled{false};

        BrowserEngine engine;
        Terminal terminal;
        std::unique_ptr<network::StopSource> stopping;
        std::thread runner;
        std::atomic<bool> isRunning{false};

        // The worker's thread alone touches these.
        bool readySeen = false;
        bool announced = false;
        std::optional<PageSend> seeding;
        std::optional<cli::CommandResult> seeded;
    };

    // What the module's files are named from: the samples' root, as the terminal expects.
    // The protocol version the worker and the client exchange.
    int enterSamples() { return ::chdir(samplesRoot); }

    std::string release() { return ORDER_BOOK_WASM_RELEASE; }

    EMSCRIPTEN_BINDINGS(order_book_wasm_module) {
        emscripten::class_<ModuleEngine>("Engine")
            .constructor<const val&>()
            .function("start", &ModuleEngine::start)
            .function("stop", &ModuleEngine::stop)
            .function("running", &ModuleEngine::running)
            .function("run", &ModuleEngine::run)
            .function("takeEvents", &ModuleEngine::takeEvents);
        emscripten::function("enterSamples", &enterSamples);
        emscripten::function("release", &release);
    }

}  // namespace order_book::wasm
