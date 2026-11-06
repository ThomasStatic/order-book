#include "thread_cpu.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include "order_book/runtime/thread_placement.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace order_book::benchmark {

ThreadCpuTime ThreadCpuTime::ofCurrentThread() {
    ThreadCpuTime meter;
#ifdef _WIN32
    meter.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    if (meter.handle == nullptr) {
        throw std::runtime_error("could not open this thread to read its CPU time: error " + std::to_string(GetLastError()));
    }
#else
    clockid_t id{};
    if (const int error = pthread_getcpuclockid(pthread_self(), &id); error != 0) {
        throw std::runtime_error("could not find this thread's CPU clock: error " + std::to_string(error));
    }
    meter.clock = static_cast<int>(id);
#endif
    return meter;
}

ThreadCpuTime::ThreadCpuTime(ThreadCpuTime&& other) noexcept : handle(other.handle), clock(other.clock) {
    other.handle = nullptr;
}

ThreadCpuTime::~ThreadCpuTime() {
#ifdef _WIN32
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
    }
#endif
}

std::uint64_t ThreadCpuTime::reading() const {
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(static_cast<HANDLE>(handle), &cycles)) {
        throw std::runtime_error("could not read a thread's cycle time: error " + std::to_string(GetLastError()));
    }
    return cycles;
#else
    timespec time{};
    if (clock_gettime(static_cast<clockid_t>(clock), &time) != 0) {
        throw std::runtime_error("could not read a thread's CPU clock");
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000u + static_cast<std::uint64_t>(time.tv_nsec);
#endif
}

std::optional<double> ThreadCpuTime::shareOf(std::uint64_t used, double spanNanoseconds, bool clockIsTsc, double tscTicksPerNanosecond) {
    if (spanNanoseconds <= 0.0) {
        return std::nullopt;
    }
#ifdef _WIN32
    if (!clockIsTsc) {
        return std::nullopt;
    }
    return static_cast<double>(used) / tscTicksPerNanosecond / spanNanoseconds;
#else
    static_cast<void>(clockIsTsc);
    static_cast<void>(tscTicksPerNanosecond);
    return static_cast<double>(used) / spanNanoseconds;
#endif
}


std::optional<double> ThreadCpuTime::shareOf(std::uint64_t used, double spanNanoseconds) {
#ifdef _WIN32
    // Cycles a busy thread counts per nanosecond, measured once.
    static const double cyclesPerNanosecond = [] {
        const ThreadCpuTime self = ofCurrentThread();
        const auto begin = std::chrono::steady_clock::now();
        const std::uint64_t first = self.reading();
        auto now = begin;
        while (now - begin < std::chrono::milliseconds(20)) {
            now = std::chrono::steady_clock::now();
        }
        const std::uint64_t last = self.reading();
        const double nanoseconds = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
        return static_cast<double>(last - first) / nanoseconds;
    }();
    return shareOf(used, spanNanoseconds, true, cyclesPerNanosecond);
#else
    return shareOf(used, spanNanoseconds, false, 1.0);
#endif
}

void placeCurrentThread(std::optional<unsigned int> core, bool highPriority) {
    if (core) {
        if (const std::optional<std::string> refused = runtime::pinThisThread(*core)) {
            throw std::runtime_error("could not pin to core " + std::to_string(*core) + ": " + *refused);
        }
    }
    if (highPriority) {
        if (const std::optional<std::string> refused = runtime::setThisThreadPriority(runtime::ThreadPriority::HIGH)) {
            throw std::runtime_error("could not raise a thread's priority: " + *refused);
        }
    }
}

}  // namespace order_book::benchmark
