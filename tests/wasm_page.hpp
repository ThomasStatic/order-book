#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/wasm/browser_engine.hpp"

// For the tests that play the engine worker natively: what it sees of a BrowserEngine (wakes,
// ready, every store change) and a run on a thread of its own, as the worker starts the entry.
namespace order_book::wasm_testing {

    using Change = runtime::PageLogStorage::Change;
    using wasm::BrowserCallbacks;
    using wasm::BrowserEngine;

    inline void check(bool condition, const std::string& message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << std::endl;
            std::exit(EXIT_FAILURE);
        }
    }

    // What the engine worker would see: changes, wakes, and ready, under the engine's threads.
    class Page {
    public:
        BrowserCallbacks callbacks() {
            BrowserCallbacks told;
            told.wake = [this] {
                const std::lock_guard held(guard);
                ++wakes;
                woken.notify_all();
            };
            told.stored = [this](const Change& change) {
                runtime::PageLogStorage::apply(copy, change);
                ++changes;
            };
            told.ready = [this] {
                const std::lock_guard held(guard);
                readied = true;
                woken.notify_all();
            };
            return told;
        }

        void waitUntilReady() {
            std::unique_lock held(guard);
            check(woken.wait_for(held, std::chrono::seconds(60), [this] { return readied; }), "the engine became ready");
            readied = false;
        }

        // Sleeps until the next wake after `seen`, and gives the count.
        std::size_t waitPast(std::size_t seen) {
            std::unique_lock held(guard);
            check(woken.wait_for(held, std::chrono::seconds(60), [&] { return wakes > seen; }), "the connections woke the page");
            return wakes;
        }

        std::size_t wakesSoFar() {
            const std::lock_guard held(guard);
            return wakes;
        }

        // Under the store's lock, which every change is told under.
        std::map<std::string, std::vector<std::byte>> copy;
        std::size_t changes = 0;

    private:
        std::mutex guard;
        std::condition_variable woken;
        std::size_t wakes = 0;
        bool readied = false;
    };

    // One BrowserEngine run on a thread of its own, as the engine worker starts the entry.
    class Running {
    public:
        explicit Running(BrowserEngine& browser) : engine(browser) {
            thread = std::thread([this] { exit = engine.run(stop, report, diagnostics); });
        }
        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
        ~Running() {
            if (thread.joinable()) {
                stop.request();
                thread.join();
            }
        }

        network::EngineExit cleanStop() {
            stop.request();
            thread.join();
            return *exit;
        }

        BrowserEngine& engine;
        network::StopSource stop;
        std::ostringstream report;
        std::ostringstream diagnostics;
        std::optional<network::EngineExit> exit;

    private:
        std::thread thread;
    };

    // Pumps until it gives a result, sleeping on the page's wakes between, as the worker does.
    template <typename Pump>
    auto pumpUntilDone(Page& page, Pump pump) {
        for (;;) {
            const std::size_t seen = page.wakesSoFar();
            if (auto result = pump()) {
                return *result;
            }
            static_cast<void>(page.waitPast(seen));
        }
    }

}  // namespace order_book::wasm_testing
