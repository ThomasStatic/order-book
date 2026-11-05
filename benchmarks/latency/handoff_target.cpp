#include "handoff_target.hpp"

#include <utility>

namespace order_book::latency {

HandOffTarget::HandOffTarget(const benchmark::Workload& workload, std::function<void()> setUpMatchingThread,
                             std::chrono::nanoseconds spinWindow)
    : workloads_{workload},
      run_(
          workloads_,
          [this, setUp = std::move(setUpMatchingThread)] {
              if (setUp) {
                  setUp();
              }
              meter_.emplace(ThreadCpuTime::ofCurrentThread());
          },
          benchmark::RunDurability::NONE, benchmark::defaultLogParent(), spinWindow) {}

void HandOffTarget::checkOutcome() {
    run_.finish();
    run_.check();
}

}  // namespace order_book::latency
