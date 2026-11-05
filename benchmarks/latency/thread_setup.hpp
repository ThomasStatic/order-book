#pragma once

#include <cstdint>
#include <optional>

// Pins and raises the calling thread. Each throws std::runtime_error if the system refuses, so a
// run's record never claims a setting that did not apply.
namespace order_book::latency {

void pinCurrentThread(unsigned int core);

// THREAD_PRIORITY_HIGHEST on Windows and nice -10 on Linux, never a real-time policy.
void raiseCurrentThreadPriority();

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

private:
    ThreadCpuTime() = default;
    void* handle = nullptr;  // Windows: the thread's HANDLE
    int clock = 0;           // Linux: its CPU clock's clockid_t
};

}  // namespace order_book::latency
