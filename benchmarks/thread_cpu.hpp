#pragma once

#include <cstdint>
#include <optional>

// A thread's CPU time, for the share of a core a pinned thread used: the latency harness's
// matching-thread share, and a threaded benchmark run's lowest. Not public API.
namespace order_book::benchmark {

// The CPU a thread has used, read from any thread. Made on the thread it measures, and must not
// outlive it. On Windows a reading is the thread's cycle count, which counts at the TSC's rate;
// on Linux it is the nanoseconds of the thread's CPU clock.
class ThreadCpuTime {
public:
    static ThreadCpuTime ofCurrentThread();
    ThreadCpuTime(ThreadCpuTime&& other) noexcept;
    ThreadCpuTime& operator=(ThreadCpuTime&&) = delete;
    ThreadCpuTime(const ThreadCpuTime&) = delete;
    ~ThreadCpuTime();

    std::uint64_t reading() const;
    // The share of one core that used, a reading's difference, is of a span this many
    // nanoseconds long. Nothing on Windows unless the latency clock is the TSC, whose rate
    // tscTicksPerNanosecond is.
    static std::optional<double> shareOf(std::uint64_t used, double spanNanoseconds, bool clockIsTsc,
                                         double tscTicksPerNanosecond);
    // The same with no clock to hand: on Windows the cycle rate is measured once, on a thread
    // spinning for 20 ms against steady_clock, and then kept.
    static std::optional<double> shareOf(std::uint64_t used, double spanNanoseconds);

private:
    ThreadCpuTime() = default;
    void* handle = nullptr;  // Windows: the thread's HANDLE
    int clock = 0;           // Linux: its CPU clock's clockid_t
};

// Pins the calling thread to core, if given, and raises it to high priority, if asked, as the
// engine's own placement does. Throws std::runtime_error with the system's reason if refused, so
// a result never claims a setting that did not apply.
void placeCurrentThread(std::optional<unsigned int> core, bool highPriority);

}  // namespace order_book::benchmark
