#include "order_book/network/stop_source.hpp"

#include <array>
#include <cstdio>
#include <thread>
#include <utility>

namespace order_book::network {

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
            std::array<char, 4096> sink;
            while (std::fread(sink.data(), 1, sink.size(), stdin) != 0) {
            }
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
