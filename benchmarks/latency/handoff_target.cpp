#include "handoff_target.hpp"

namespace order_book::latency {

HandOffTarget::HandOffTarget(const benchmark::Workload& workload) : workloads_{workload}, run_(workloads_) {}

void HandOffTarget::checkOutcome() {
    run_.finish();
    run_.check();
}

}  // namespace order_book::latency
