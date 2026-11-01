#include "command_target.hpp"

#include <stdexcept>

namespace order_book::latency {

CommandTarget::CommandTarget(const benchmark::Workload& workload) : workload_(workload) {
    benchmark::preloadBook(book_, workload_);
}

void CommandTarget::count(const SubmissionResult& result) {
    ++outcome_.submits;
    if (result.getStatus() == SubmissionStatus::REJECTED) {
        ++outcome_.rejections;
    }
    benchmark::countEvents(result.getEvents(), outcome_);
}

void CommandTarget::count(const CancellationResult& result) {
    ++outcome_.cancels;
    if (!result.isSuccess()) {
        ++outcome_.rejections;
    }
}

void CommandTarget::count(const ReplaceResult& result) {
    ++outcome_.replaces;
    if (!result.isSuccess()) {
        ++outcome_.rejections;
    }
    benchmark::countEvents(result.getEvents(), outcome_);
}

void CommandTarget::checkOutcome() const {
    benchmark::WorkloadOutcome outcome = outcome_;
    benchmark::recordFinalBook(book_, outcome);
    if (outcome != workload_.expected) {
        throw std::runtime_error("workload " + workload_.config.name + ": the book did " +
                                 benchmark::describeOutcome(outcome) + " but the workload predicted " +
                                 benchmark::describeOutcome(workload_.expected));
    }
}

}  // namespace order_book::latency
