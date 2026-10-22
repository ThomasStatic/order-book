#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

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
    using order_book::RejectionReason;
    using order_book::SubmissionResults;
    using order_book::SubmissionStatus;
    using order_book::Trade;
    using order_book::FillStatus;
    using order_book::Side;

    // Results are built only through accepted() and rejected(), so none can be half-formed.
    static_assert(!std::is_constructible_v<SubmissionResults, unsigned int>,
                  "SubmissionResults should not be constructible from a bare submission ID");

    const SubmissionResults resting = SubmissionResults::accepted({11, Side::BUY, 100, 5}, {}, 5);
    expect(resting.getSubmissionId() == 11, "an accepted result should preserve the submission ID");
    expect(resting.getGeneratedTrades().empty(), "an accepted result with no trades should report none");
    expect(resting.getRemainingQuantity() == 5, "an accepted result should preserve the remaining quantity");
    expect(resting.getFillStatus() == FillStatus::NONE, "an accepted result with no trades should report no fill");
    expect(resting.getStatus() == SubmissionStatus::ACCEPTED, "an accepted result should be accepted");
    expect(resting.getRejectionReason() == RejectionReason::NONE, "an accepted result should have no rejection reason");
    expect(SubmissionResults::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 100, 5, Side::BUY)}, 3).getFillStatus() == FillStatus::PARTIAL,
           "an accepted result with trades and quantity left should be partially filled");
    expect(SubmissionResults::accepted({13, Side::BUY, 100, 5}, {Trade(1, 2, 13, 100, 5, Side::BUY)}, 0).getFillStatus() == FillStatus::FILLED,
           "an accepted result with trades and nothing left should be filled");
    expectThrows([] { SubmissionResults::accepted({0, Side::BUY, 100, 5}, {}, 5); }, "an accepted result should reject a non-positive submission ID");

    const SubmissionResults refused = SubmissionResults::rejected(14, 5, RejectionReason::DUPLICATE_ORDER_ID);
    expect(refused.getSubmissionId() == 14, "a rejected result should preserve the submission ID");
    expect(refused.getStatus() == SubmissionStatus::REJECTED, "a rejected result should be rejected");
    expect(refused.getRejectionReason() == RejectionReason::DUPLICATE_ORDER_ID, "a rejected result should keep its reason");
    expect(refused.getGeneratedTrades().empty() && refused.getFillStatus() == FillStatus::NONE,
           "a rejected result should report no execution");
    expect(refused.getRemainingQuantity() == 5, "a rejected result should report the submitted quantity as unexecuted");
    expect(SubmissionResults::rejected(0, 5, RejectionReason::INVALID_ORDER_ID).getSubmissionId() == 0,
           "a rejected result may echo the reserved ID 0");
    expectThrows([] { SubmissionResults::rejected(15, 5, RejectionReason::NONE); },
                 "a rejected result should require a rejection reason");

    Trade trade(1, 2, 3, 100, 5, Side::BUY);
    expect(trade.getTradeId() == 1, "trade ID should be preserved");
    expect(trade.getRestingId() == 2, "resting order ID should be preserved");
    expect(trade.getIncomingId() == 3, "incoming order ID should be preserved");
    expect(trade.getExecutionPrice() == 100, "execution price should be preserved");
    expect(trade.getExecutionQuantity() == 5, "execution quantity should be preserved");
    expect(trade.getIncomingSide() == Side::BUY, "incoming side should be preserved");
    expect(trade.getBuyOrderId() == 3 && trade.getSellOrderId() == 2,
           "an incoming buy should be the buy order and the resting order the sell order");
    Trade sellTrade(2, 4, 5, 100, 5, Side::SELL);
    expect(sellTrade.getBuyOrderId() == 4 && sellTrade.getSellOrderId() == 5,
           "an incoming sell should be the sell order and the resting order the buy order");
    expectThrows([] { Trade invalidTradeId(0, 2, 3, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive trade ID");
    expectThrows([] { Trade invalidRestingId(1, 0, 3, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive resting ID");
    expectThrows([] { Trade invalidIncomingId(1, 2, 0, 100, 5, Side::BUY); }, "trade constructor should reject a non-positive incoming ID");
    expectThrows([] { Trade invalidExecutionPrice(1, 2, 3, 0, 5, Side::BUY); }, "trade constructor should reject a non-positive execution price");
    expectThrows([] { Trade invalidExecutionQuantity(1, 2, 3, 100, 0, Side::BUY); }, "trade constructor should reject a non-positive execution quantity");

    std::cout << "result type tests passed" << std::endl;
    return EXIT_SUCCESS;
}
