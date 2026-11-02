#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <ostream>
#include <streambuf>
#include <string>

#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"

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

int failEachStartAllocation() {
    const std::filesystem::path list = std::filesystem::temp_directory_path() / "order_book_engine_start_allocation_test.csv";
    std::ofstream(list, std::ios::binary) << "symbol,shard,orders,stops,levels\nAAPL,,,,\nMSFT,1,10,10,10\n";
    runtime::EngineSettings settings;
    settings.instruments = list.string();
    settings.shards = 2;
    settings.listen = runtime::ListenAddress{{127, 0, 0, 1}, 0};
    settings.capacityOrders = 64;  // small books, so a start makes few allocations
    settings.capacityStops = 8;
    settings.capacityLevels = 8;

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
        ++refusals;
    }
    expect(refusals > 10, "the start allocates enough for this to mean something");
    std::filesystem::remove(list);
    std::cout << "engine start allocation tests passed after " << refusals << " refused starts" << std::endl;
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    // Discarded rather than skipped over, so MSVC sees no unreachable code.
    if constexpr (iteratorDebugging) {
        std::cout << "SKIPPED: MSVC iterator debugging is on" << std::endl;
        return skipped;
    } else {
        return failEachStartAllocation();
    }
}
