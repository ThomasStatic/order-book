#include "order_book/runtime/thread_placement.hpp"

#include <cerrno>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace order_book::runtime {

#if defined(_WIN32) || defined(__linux__)
    namespace {
        std::string pastTheLast(std::uint32_t core, unsigned long long last) {
            return "core " + std::to_string(core) + " is past the last this build can pin, " + std::to_string(last);
        }
    }  // namespace
#endif

#ifdef _WIN32
    std::optional<std::string> pinThisThread(std::uint32_t core) {
        constexpr std::uint32_t bits = sizeof(DWORD_PTR) * 8;
        if (core >= bits) {
            return pastTheLast(core, bits - 1);  // processor groups past the first aren't reached
        }
        if (SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core) == 0) {
            return std::system_category().message(static_cast<int>(GetLastError()));
        }
        return std::nullopt;
    }

    std::optional<std::string> setThisThreadPriority(ThreadPriority priority) {
        if (priority == ThreadPriority::HIGH && !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
            return std::system_category().message(static_cast<int>(GetLastError()));
        }
        return std::nullopt;
    }
#elif defined(__linux__)
    std::optional<std::string> pinThisThread(std::uint32_t core) {
        if (core >= static_cast<std::uint32_t>(CPU_SETSIZE)) {
            return pastTheLast(core, static_cast<unsigned long long>(CPU_SETSIZE) - 1);
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
            return std::system_category().message(error);
        }
        return std::nullopt;
    }

    // Linux gives each thread its own nice value, named by its thread id.
    std::optional<std::string> setThisThreadPriority(ThreadPriority priority) {
        constexpr int highNice = -10;
        if (priority == ThreadPriority::HIGH && setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), highNice) != 0) {
            return std::system_category().message(errno);
        }
        return std::nullopt;
    }
#else
    std::optional<std::string> pinThisThread(std::uint32_t) {
        return std::string("pinning isn't supported on this platform");
    }

    std::optional<std::string> setThisThreadPriority(ThreadPriority priority) {
        if (priority == ThreadPriority::HIGH) {
            return std::string("high priority isn't supported on this platform");
        }
        return std::nullopt;
    }
#endif

}  // namespace order_book::runtime
