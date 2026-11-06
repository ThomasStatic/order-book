#pragma once

#include "thread_cpu.hpp"

// Pins and raises the calling thread. Each throws std::runtime_error if the system refuses, so a
// run's record never claims a setting that did not apply.
namespace order_book::latency {

void pinCurrentThread(unsigned int core);

// THREAD_PRIORITY_HIGHEST on Windows and nice -10 on Linux, never a real-time policy.
void raiseCurrentThreadPriority();

using ThreadCpuTime = benchmark::ThreadCpuTime;

}  // namespace order_book::latency
