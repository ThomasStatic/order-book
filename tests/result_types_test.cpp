#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "order_book/cancellation_results.hpp"
#include "order_book/submission_results.hpp"
#include "order_book/trade.hpp"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Callable>
void expectThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }

    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
    using order_book::CancellationResults;
    using order_book::CancellationStatus;
    using order_book::RejectionReason;
    using order_book::SubmissionResults;
    using order_book::SubmissionStatus;
    using order_book::Trade;
    using order_book::FillStatus;
    using order_book::Side;

    CancellationResults cancellation(7);
    expect(cancellation.getTargetedOrder() == 7, "cancellation should preserve the targeted order ID");
    expect(cancellation.getRemainingQuantity() == 0, "new cancellation result should start with zero remaining quantity");
    expect(cancellation.getRejectionReason() == RejectionReason::NONE, "new cancellation result should have no rejection reason");
    expect(cancellation.getStatus() == CancellationStatus::CANCELLED, "new cancellation result should be marked cancelled");
    expectThrows([] { CancellationResults invalidCancellation(0); }, "cancellation constructor should reject a non-positive target order ID");

    SubmissionResults submission(11);
    expect(submission.getSubmissionId() == 11, "submission should preserve the submission ID");
    expect(submission.getGeneratedTrades().empty(), "new submission result should start with no generated trades");
    expect(submission.getRemainingQuantity() == 0, "new submission result should start with zero remaining quantity");
    expect(submission.getFillStatus() == FillStatus::NONE, "new submission result should start with no fill status");
    expect(submission.getStatus() == SubmissionStatus::ACCEPTED, "new submission result should be accepted by default");
    expect(submission.getRejectionReason() == RejectionReason::NONE, "new submission result should have no rejection reason");
    expectThrows([] { SubmissionResults invalidSubmission(0); }, "submission constructor should reject a non-positive submission ID");

    Trade trade(1, 2, 3, 100, 5, Side::BUY);
    expect(trade.getTradeId() == 1, "trade ID should be preserved");
    expect(trade.getRestingId() == 2, "resting order ID should be preserved");
    expect(trade.getIncomingId() == 3, "incoming order ID should be preserved");
    expect(trade.getExecutionPrice() == 100, "execution price should be preserved");
    expect(trade.getExecutionQuantity() == 5, "execution quantity should be preserved");
    expect(trade.getIncomingSide() == Side::BUY, "incoming side should be preserved");
    expectThrows([] { Trade invalidTradeId(0, 2, 3, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive trade ID");
    expectThrows([] { Trade invalidRestingId(1, 0, 3, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive resting ID");
    expectThrows([] { Trade invalidIncomingId(1, 2, 0, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive incoming ID");
    expectThrows([] { Trade invalidExecutionPrice(1, 2, 3, 0, 5, Side::BUY); }, "trade constructor should reject a non-positive execution price");
    expectThrows([] { Trade invalidExecutionQuantity(1, 2, 3, 100, 0, Side::BUY); }, "trade constructor should reject a non-positive execution quantity");

    std::cout << "result type tests passed" << std::endl;
    return EXIT_SUCCESS;
}
