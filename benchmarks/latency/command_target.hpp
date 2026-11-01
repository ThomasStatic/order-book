#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "benchmark_runner.hpp"
#include "clock.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replace_result.hpp"
#include "order_book/submission_result.hpp"
#include "workload.hpp"

// The command interval: one book call, from just before it to just after it returns, on a
// freshly preloaded book. Counting the result and destroying it happen after the closing
// reading, so a run still proves it did what the workload predicted without timing that proof.
namespace order_book::latency {

class CommandTarget {
public:
    // Builds the book and applies the preload, untimed. The workload must outlive the target.
    explicit CommandTarget(const benchmark::Workload& workload);

    template <typename DriverClock>
    Ticks operator()(std::size_t index, DriverClock& clock) {
        namespace bench = benchmark;
        if (workload_.config.family == bench::WorkloadFamily::V1) {
            const bench::BenchmarkCommand& command = workload_.timed[index];
            if (command.type == bench::CommandType::SUBMIT) {
                const Order order(command.orderId, command.price, command.quantity, 1, command.side);
                const SubmissionResult result = book_.submitOrder(order);
                const Ticks done = clock.now();
                count(result);
                return done;
            }
            const CancellationResult result = book_.cancelOrder(command.orderId);
            const Ticks done = clock.now();
            count(result);
            return done;
        }
        const bench::BehaviorCommand& command = workload_.behaviorTimed[index];
        if (command.type == bench::CommandType::SUBMIT) {
            const SubmissionResult result = book_.submitOrder(command.request);
            const Ticks done = clock.now();
            count(result);
            return done;
        }
        if (command.type == bench::CommandType::REPLACE) {
            const ReplaceResult result = book_.replaceOrder(command.replacement);
            const Ticks done = clock.now();
            count(result);
            return done;
        }
        const CancellationResult result = book_.cancelOrder(command.request.id);
        const Ticks done = clock.now();
        count(result);
        return done;
    }

    // Throws std::runtime_error, describing both, if the book's outcome differs from the
    // workload's prediction, so a run never stands for work the book did not do.
    void checkOutcome() const;

    std::uint64_t capacityGrowths() const { return book_.capacityGrowths(); }

private:
    void count(const SubmissionResult& result);
    void count(const CancellationResult& result);
    void count(const ReplaceResult& result);

    const benchmark::Workload& workload_;
    OrderBook book_;
    benchmark::WorkloadOutcome outcome_;
};

}  // namespace order_book::latency
