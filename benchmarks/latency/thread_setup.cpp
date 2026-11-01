#include "thread_setup.hpp"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace order_book::latency {

void pinCurrentThread(unsigned int core) {
#if defined(_WIN32)
    if (core >= 64 || SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core) == 0) {
        throw std::runtime_error("could not pin to core " + std::to_string(core));
    }
#elif defined(__linux__)
    cpu_set_t cores;
    CPU_ZERO(&cores);
    CPU_SET(core, &cores);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cores), &cores) != 0) {
        throw std::runtime_error("could not pin to core " + std::to_string(core));
    }
#else
    throw std::runtime_error("pinning is not supported here, core " + std::to_string(core));
#endif
}

void raiseCurrentThreadPriority() {
#if defined(_WIN32)
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) == 0) {
        throw std::runtime_error("could not raise this thread's priority");
    }
#elif defined(__linux__)
    // On Linux, nice is per thread, and 0 names the calling one.
    if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
        throw std::runtime_error("could not set nice -10 (it needs CAP_SYS_NICE or an rlimit)");
    }
#else
    throw std::runtime_error("raising priority is not supported here");
#endif
}

}  // namespace order_book::latency
