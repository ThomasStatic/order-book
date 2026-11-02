#include "handoff_target.hpp"

#include <utility>

namespace order_book::latency {

HandOffTarget::HandOffTarget(const benchmark::Workload& workload, std::function<void()> setUpMatchingThread)
    : workloads_{workload}, run_(workloads_, std::move(setUpMatchingThread)) {}

void HandOffTarget::checkOutcome() {
    run_.finish();
    run_.check();
}

}  // namespace order_book::latency
