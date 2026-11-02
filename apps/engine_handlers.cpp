#include "engine_handlers.hpp"

#include <array>
#include <csignal>
#include <cstdlib>
#include <thread>

#include "order_book/runtime/abort_report.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace order_book::engine_app {

    namespace {
        // On the aborting thread's own stack, so two threads aborting at once can't mix lines.
        // Then the default action, so a SIGABRT sent from outside ends the process as abort does.
        extern "C" void onAbort(int) {
            std::array<char, 160> line{};
            const std::size_t size = runtime::describeAbort(line);
#ifdef _WIN32
            static_cast<void>(_write(2, line.data(), static_cast<unsigned int>(size)));
#else
            // Nothing to do if it fails; glibc's warn_unused_result ignores a cast to void.
            const ssize_t written = ::write(STDERR_FILENO, line.data(), size);
            static_cast<void>(written);
#endif
            std::signal(SIGABRT, SIG_DFL);
            std::raise(SIGABRT);
        }

#ifdef _WIN32
        network::StopSource* stopSource = nullptr;

        BOOL WINAPI onConsoleEvent(DWORD event) {
            stopSource->request();
            if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
                // Returning would end the process now. Waiting lets the stop finish, unless the
                // system's timeout ends the process first.
                Sleep(INFINITE);
            }
            return TRUE;
        }
#else
        sigset_t stopSignals() {
            sigset_t signals;
            sigemptyset(&signals);
            sigaddset(&signals, SIGINT);
            sigaddset(&signals, SIGTERM);
            sigaddset(&signals, SIGHUP);
            return signals;
        }
#endif
    }  // namespace

    void prepareStopSignals() {
#ifndef _WIN32
        const sigset_t signals = stopSignals();
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    void forwardStopSignals(network::StopSource& stop) {
#ifdef _WIN32
        stopSource = &stop;
        // A process started in a group of its own begins with Ctrl-C ignored; take it back.
        SetConsoleCtrlHandler(nullptr, FALSE);
        SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
        // Never joined: it waits for the next signal until the process ends.
        std::thread([&stop] {
            const sigset_t signals = stopSignals();
            for (;;) {
                int received = 0;
                if (sigwait(&signals, &received) == 0) {
                    stop.request();
                }
            }
        }).detach();
#endif
    }

    void installAbortHandler() {
#ifdef _WIN32
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        std::signal(SIGABRT, onAbort);
    }

}  // namespace order_book::engine_app
