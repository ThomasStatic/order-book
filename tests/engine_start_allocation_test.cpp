#include <atomic>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <new>
#include <ostream>
#include <algorithm>
#include <iterator>
#include <map>
#include <streambuf>
#include <string>
#include <variant>
#include <vector>

#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/symbol.hpp"

#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

// Each allocation runEngine makes before it is ready fails in turn, on whichever thread makes it,
// and each must refuse the start cleanly: no crash, no hang, and a message. Under MSVC's checked
// iterators, which allocate inside noexcept members, failing one terminates, so the test skips
// there. See docs/internals/testing.md.
namespace {
std::atomic<long> allocationsBeforeFailure{-1};  // negative = disarmed
std::atomic<bool> failed{false};

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool iteratorDebugging = true;
#else
constexpr bool iteratorDebugging = false;
#endif
constexpr int skipped = 77;
}  // namespace

void* operator new(std::size_t size) {
    long left = allocationsBeforeFailure.load();
    while (left >= 0) {
        if (allocationsBeforeFailure.compare_exchange_weak(left, left - 1)) {
            if (left == 0) {
                failed.store(true);
                throw std::bad_alloc();
            }
            break;
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

using namespace order_book;
using namespace order_book::network;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// Text a stream collects in storage reserved before the run, so writing it never allocates and
// only the engine's own allocations are counted.
class Collected : public std::streambuf {
public:
    Collected() { text.reserve(64 * 1024); }
    std::string text;

protected:
    int_type overflow(int_type character) override {
        if (character != traits_type::eof()) {
            text.push_back(static_cast<char>(character));
        }
        return character;
    }
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        text.append(data, static_cast<std::size_t>(count));
        return count;
    }
};

// The report stream: once the engine says it is listening, the start is over, so counting stops
// and the run is asked to stop.
class ReadyTrigger : public std::streambuf {
public:
    explicit ReadyTrigger(StopSource& stopSource) : stop(stopSource) { text.reserve(64 * 1024); }
    std::string text;

protected:
    int_type overflow(int_type character) override {
        if (character != traits_type::eof()) {
            const char held = static_cast<char>(character);
            xsputn(&held, 1);
        }
        return character;
    }
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        const long left = allocationsBeforeFailure.exchange(-1);  // off before anything here allocates
        text.append(data, static_cast<std::size_t>(count));
        if (text.find('\n') != std::string::npos && !requested) {
            requested = true;
            stop.request();
        } else if (!requested) {
            allocationsBeforeFailure.store(left);
        }
        return count;
    }

private:
    StopSource& stop;
    bool requested = false;
};

// Each shard holds an instrument, so each of the earlier run's files holds records, and no
// refused start may remove one: recovery removes only trailing files with none.
constexpr const char* listText = "symbol,shard,orders,stops,levels\nAAPL,0,,,\nMSFT,1,10,10,10\n";

// Every file in a directory but the lock, which a start that took it leaves for good, and its
// bytes.
std::map<std::string, std::string> logsIn(const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name != "engine.lock") {
            std::ifstream in(entry.path(), std::ios::binary);
            files[name] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    return files;
}

// An earlier run's log, stopped cleanly: MSFT holds 20 orders on 20 levels, past the 10 it
// declares, so recovery warms its book further, and AAPL holds 3.
void writeEarlierLog(const std::filesystem::path& logs) {
    const runtime::InstrumentDirectory directory(std::get<std::vector<runtime::Instrument>>(runtime::parseInstrumentList(listText, 2)), 2);
    runtime::FileLogStorage storage(logs);
    runtime::Housekeeping housekeeping(storage, directory, runtime::Durability::PROCESS, runtime::defaultLogFileBytes);
    for (std::uint32_t shard = 0; shard < 2; ++shard) {
        runtime::Ring<runtime::LogEntry> ring(64);
        runtime::DurablePosition durable;
        runtime::LogWriter writer(housekeeping, shard, ring, durable);
        for (std::uint32_t slot = 0; slot < directory.onShard(shard).size(); ++slot) {
            const bool msft = directory.onShard(shard)[slot].symbol == *replay::Symbol::parse("MSFT");
            for (OrderId id = 1; id <= (msft ? 20U : 3U); ++id) {
                replay::AddCommand add{};
                add.orderId = id;
                add.side = Side::BUY;
                add.price = static_cast<Price>(100 + id);
                add.quantity = 1;
                ring.tryPush(runtime::LogEntry{add, slot, runtime::ControlKind::COMMAND});
            }
        }
        ring.tryPush(runtime::LogEntry::stop());
        while (!writer.stopped()) {
            writer.pass();
        }
    }
    housekeeping.removeUnused();
}

// Once with no log, once with a new one, where a refused start must also leave no log behind, or
// the next start would be refused on it, and once recovering an earlier run's, which a refused
// start must leave as it was.
long failEachStartAllocation(const std::filesystem::path& list, const std::optional<std::filesystem::path>& logs, bool earlier = false) {
    runtime::EngineSettings settings;
    settings.instruments = list.string();
    settings.shards = 2;
    settings.listen = runtime::ListenAddress{{127, 0, 0, 1}, 0};
    settings.capacityOrders = 64;  // small books, so a start makes few allocations
    settings.capacityStops = 8;
    settings.capacityLevels = 8;
    if (logs) {
        std::filesystem::remove_all(*logs);
        std::filesystem::create_directories(*logs);
        settings.logDir = logs->string();
        settings.logRing = 64;
        if (earlier) {
            writeEarlierLog(*logs);
        }
    }
    const std::map<std::string, std::string> before = logs ? logsIn(*logs) : std::map<std::string, std::string>{};
    if (earlier) {
        expect(before.size() == 2, "the earlier run left a log for each shard");
    }

    long refusals = 0;
    for (long k = 0;; ++k) {
        expect(k < 100000, "a start that never got past failing allocations");
        StopSource stop;
        ReadyTrigger trigger(stop);
        std::ostream report(&trigger);
        Collected collected;
        std::ostream diagnostics(&collected);
        failed.store(false);
        allocationsBeforeFailure.store(k);
        const EngineExit exit = runEngine(settings, stop, report, diagnostics);
        allocationsBeforeFailure.store(-1);
        if (!failed.load()) {
            expect(exit == EngineExit::CLEAN_STOP, "with nothing failing the run starts and stops cleanly:\n" + collected.text);
            break;
        }
        const std::string said = collected.text;
        expect(exit == EngineExit::START_REFUSED, "allocation " + std::to_string(k) + " failing refuses the start, not exit " +
                                                      std::to_string(static_cast<int>(exit)) + ":\n" + said);
        expect(said.find("order_book_engine: start refused: ") != std::string::npos, "and says so:\n" + said);
        expect(trigger.text.empty(), "and never listened");
        if (logs) {
            const std::map<std::string, std::string> after = logsIn(*logs);
            if (after != before) {
                std::string names;
                for (const auto& [name, bytes] : after) {
                    names += "\n" + name + ", " + std::to_string(bytes.size()) + " bytes";
                }
                expect(false, "allocation " + std::to_string(k) + " failing left the log other than it found it, holding:" + names +
                                  "\nwith:\n" + said);
            }
        }
        ++refusals;
    }
    expect(refusals > 10, "the start allocates enough for this to mean something");
    if (logs) {
        std::filesystem::remove_all(*logs);
    }
    return refusals;
}

int failEachStartAllocation() {
    const std::filesystem::path list = std::filesystem::temp_directory_path() / "order_book_engine_start_allocation_test.csv";
    std::ofstream(list, std::ios::binary) << listText;
    const std::filesystem::path logs = std::filesystem::temp_directory_path() / "order_book_engine_start_allocation_logs";
    const long unlogged = failEachStartAllocation(list, std::nullopt);
    const long logged = failEachStartAllocation(list, logs);
    const long recovered = failEachStartAllocation(list, logs, true);
    expect(logged > unlogged, "a log adds allocations to the start");
    expect(recovered > logged, "and recovering one adds more");
    std::filesystem::remove(list);
    std::cout << "engine start allocation tests passed after " << unlogged << " refused starts, " << logged << " with a new log, and "
              << recovered << " recovering one" << std::endl;
    return EXIT_SUCCESS;
}

}  // namespace

#if defined(__linux__)
// A failed allocation where nothing can catch it, inside a noexcept function or past the top of a
// thread, terminates: this prints where, so a CI log names the frame. Reached only once the one
// allocation meant to fail has failed, so nothing here fails.
[[noreturn]] void printWhereThenAbort() {
    static const char said[] = "FAILED: terminate, from:\n";
    const ssize_t written = ::write(STDERR_FILENO, said, sizeof said - 1);
    static_cast<void>(written);
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
    std::abort();
}
#endif

int main() {
#if defined(__linux__)
    std::set_terminate(printWhereThenAbort);
#endif
    // Discarded rather than skipped over, so MSVC sees no unreachable code.
    if constexpr (iteratorDebugging) {
        std::cout << "SKIPPED: MSVC iterator debugging is on" << std::endl;
        return skipped;
    } else {
        return failEachStartAllocation();
    }
}
