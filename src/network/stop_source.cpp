#include "order_book/network/stop_source.hpp"

#include <array>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace order_book::network {

    namespace {
        // Reads stdin to its end through the system, never through stdio: a thread blocked in
        // fread holds stdin's lock, and glibc's exit takes every stream's lock, so the process
        // could never exit while stdin stayed open.
        void readStdinToItsEnd() {
            std::array<char, 4096> sink;
#ifdef _WIN32
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD read = 0;
            while (input != nullptr && input != INVALID_HANDLE_VALUE &&
                   ReadFile(input, sink.data(), static_cast<DWORD>(sink.size()), &read, nullptr) && read != 0) {
            }
#else
            for (;;) {
                const ssize_t got = ::read(STDIN_FILENO, sink.data(), sink.size());
                if (got == 0 || (got < 0 && errno != EINTR)) {
                    return;
                }
            }
#endif
        }
    }  // namespace

    void StopSource::request() {
        count.fetch_add(1, std::memory_order_seq_cst);
        const std::lock_guard<std::mutex> hold(guard);
        if (action) {
            action();
        }
    }

    void StopSource::onRequest(std::function<void()> newAction) {
        const std::lock_guard<std::mutex> hold(guard);
        action = std::move(newAction);
    }

    struct StdinStopSource::Shared {
        std::mutex guard;
        StopSource* source = nullptr;  // until the StdinStopSource is gone
    };

    StdinStopSource::StdinStopSource() : shared(std::make_shared<Shared>()) {
        shared->source = this;
        std::thread([shared = shared] {
            readStdinToItsEnd();
            const std::lock_guard<std::mutex> hold(shared->guard);
            if (shared->source != nullptr) {
                shared->source->request();
            }
        }).detach();
    }

    StdinStopSource::~StdinStopSource() {
        const std::lock_guard<std::mutex> hold(shared->guard);
        shared->source = nullptr;
    }

}  // namespace order_book::network
