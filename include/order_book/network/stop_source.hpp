#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

// What asks a running engine to stop: a first request starts the clean stop, a second forces
// it. Adapters turn something outside into requests: closing stdin here, signals in apps/, and a
// test's direct call through the base. See docs/internals/network.md.
namespace order_book::network {

    class StopSource {
    public:
        StopSource() = default;
        StopSource(const StopSource&) = delete;
        StopSource& operator=(const StopSource&) = delete;
        virtual ~StopSource() = default;

        // From any thread, any number of times.
        void request();
        std::size_t requests() const noexcept { return count.load(std::memory_order_seq_cst); }

        // Runs on every request from now on, on the requesting thread; empty to stop. Replacing
        // it waits until no request is running it.
        void onRequest(std::function<void()> action);

    private:
        std::atomic<std::size_t> count{0};
        std::mutex guard;
        std::function<void()> action;
    };

    // Closing stdin counts as a stop: a thread reads stdin, throwing away what it reads, and
    // requests a stop when it ends. That thread can't be cancelled, so it outlives this object
    // when stdin is still open, and holds only what the two share.
    class StdinStopSource final : public StopSource {
    public:
        StdinStopSource();
        ~StdinStopSource() override;

    private:
        struct Shared;
        std::shared_ptr<Shared> shared;
    };

}  // namespace order_book::network
