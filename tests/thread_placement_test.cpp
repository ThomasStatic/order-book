#ifdef __EMSCRIPTEN__
#include <cstdio>
int main() { std::puts("PROBE: skipped under Emscripten"); return 0; }
#else
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "order_book/runtime/thread_placement.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// pinThisThread and setThisThreadPriority, each on a thread of its own, read back through the
// system rather than through anything the functions report. See docs/internals/runtime.md.
namespace {

using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void onAThreadOfItsOwn(const std::function<void()>& body) {
    std::thread(body).join();
}

#ifdef _WIN32
bool mayRunOn(std::uint32_t core) {
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    return core < 64 && (process & (DWORD_PTR{1} << core)) != 0;
}

// The calling thread's mask, read by widening it to the process's and setting it back.
DWORD_PTR ownMask() {
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    const DWORD_PTR mask = SetThreadAffinityMask(GetCurrentThread(), process);
    SetThreadAffinityMask(GetCurrentThread(), mask);
    return mask;
}

bool pinnedTo(std::uint32_t core) {
    return core < 64 && ownMask() == (DWORD_PTR{1} << core);
}

bool raised() {
    return GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_HIGHEST;
}

bool mayRaise() {
    return true;  // THREAD_PRIORITY_HIGHEST needs no privilege
}
#else
bool mayRunOn(std::uint32_t core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(set), &set);
    return core < static_cast<std::uint32_t>(CPU_SETSIZE) && CPU_ISSET(core, &set);
}

bool pinnedTo(std::uint32_t core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
    return CPU_COUNT(&set) == 1 && CPU_ISSET(core, &set);
}

bool raised() {
    return getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid))) == -10;
}

// Root, or a nice limit that reaches -10, which is 20 - (-10) in RLIMIT_NICE's terms. A process
// given CAP_SYS_NICE alone would be wrongly expected to be refused, and fail this test.
bool mayRaise() {
    rlimit limit{};
    getrlimit(RLIMIT_NICE, &limit);
    return geteuid() == 0 || limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= 30;
}
#endif

std::uint32_t firstCoreWhere(bool allowed) {
    std::uint32_t core = 0;
    while (core < 4096 && mayRunOn(core) != allowed) {
        ++core;
    }
    return core;
}

void testPinning() {
    const std::uint32_t allowed = firstCoreWhere(true);
    onAThreadOfItsOwn([allowed] {
        const std::optional<std::string> refused = pinThisThread(allowed);
        expect(!refused, "a core this process may run on pins, not: " + refused.value_or(""));
        expect(pinnedTo(allowed), "and the thread runs on that core alone");
    });

    const std::uint32_t forbidden = firstCoreWhere(false);
    onAThreadOfItsOwn([forbidden] {
        const std::optional<std::string> refused = pinThisThread(forbidden);
        expect(refused.has_value() && !refused->empty(), "core " + std::to_string(forbidden) + ", where this process may not run, is refused");
        expect(!pinnedTo(forbidden), "and nothing changed");
    });

    onAThreadOfItsOwn([] {
        const std::optional<std::string> refused = pinThisThread(4'000'000'000u);
        expect(refused.has_value() && refused->find("past") != std::string::npos, "a core past what can be named says so: " + refused.value_or(""));
    });
}

void testPriority() {
    onAThreadOfItsOwn([] {
        expect(!setThisThreadPriority(ThreadPriority::NORMAL), "normal leaves the thread as it was, which always works");
        expect(!raised(), "and doesn't raise it");
    });
    onAThreadOfItsOwn([] {
        const std::optional<std::string> refused = setThisThreadPriority(ThreadPriority::HIGH);
        if (mayRaise()) {
            expect(!refused, "this process may raise a thread's priority, and did, not: " + refused.value_or(""));
            expect(raised(), "to THREAD_PRIORITY_HIGHEST or nice -10");
        } else {
            expect(refused.has_value() && !refused->empty(), "this process may not raise a thread's priority, and is told why");
            expect(!raised(), "and nothing changed");
        }
    });
}

}  // namespace

int main() {
    testPinning();
    testPriority();
    std::cout << "thread placement tests passed" << std::endl;
    return EXIT_SUCCESS;
}

#endif
