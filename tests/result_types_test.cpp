#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "order_book/cancellation_result.hpp"
#include "order_book/result_recorder.hpp"
#include "order_book/submission_result.hpp"
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

using order_book::BookEvent;
using order_book::FillStatus;
using order_book::OrderAccepted;
using order_book::OrderExpired;
using order_book::ResultRecorder;
using order_book::SelfTradePrevented;
using order_book::Side;
using order_book::StopTriggered;
using order_book::SubmissionResult;
using order_book::Trade;
using order_book::TradeExecuted;
using Events = std::vector<BookEvent>;

// The recorder checks each append as it is made, so an engine bug throws at the append that
// breaks a rule rather than producing a result that contradicts itself.
void testResultRecorder() {
    // A submission that trades twice, has a prevention between, and expires the rest.
    {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 101, 10});
        recorder.trade(Trade(1, 2, 7, 100, 3, Side::BUY));
        recorder.prevented(SelfTradePrevented{7, 3, 101, 4}, Side::BUY);
        recorder.trade(Trade(2, 4, 7, 101, 5, Side::BUY));
        recorder.expire(2);
        const SubmissionResult result = recorder.finish();
        expect(result.getEvents() == Events{OrderAccepted{7, Side::BUY, 101, 10}, TradeExecuted{7, 2, 100, 3},
                                            SelfTradePrevented{7, 3, 101, 4}, TradeExecuted{7, 4, 101, 5},
                                            OrderExpired{7, 2}},
               "the recorder should hold every event in the order it was appended");
        expect(result.getGeneratedTrades().size() == 2 && result.getGeneratedTrades()[1].getTradeId() == 2,
               "the recorder should hold the trades, and only the trades");
        expect(result.getSubmissionId() == 7, "the result should be the submitted order's");
        expect(result.getRemainingQuantity() == 2, "the expired remainder still counts as remaining");
        expect(result.getFillStatus() == FillStatus::PARTIAL, "a partly traded submission should be PARTIAL");
    }
    // A submission accepted then wholly expired: no fill.
    {
        ResultRecorder recorder(OrderAccepted{8, Side::SELL, 100, 6});
        recorder.expire(6);
        const SubmissionResult result = recorder.finish();
        expect(result.getEvents() == Events{OrderAccepted{8, Side::SELL, 100, 6}, OrderExpired{8, 6}},
               "a wholly expired submission should hold its acceptance and its expiry");
        expect(result.getRemainingQuantity() == 6 && result.getFillStatus() == FillStatus::NONE,
               "a wholly expired submission should have filled nothing");
    }
    // A market order (price 0) has no limit, so any price is within it.
    {
        ResultRecorder recorder(OrderAccepted{9, Side::BUY, 0, 4});
        recorder.trade(Trade(1, 2, 9, 4000000000u, 4, Side::BUY));
        const SubmissionResult result = recorder.finish();
        expect(result.getFillStatus() == FillStatus::FILLED && result.getRemainingQuantity() == 0,
               "a market order filled whole should be FILLED");
    }
    // The acceptance, then four trades and an expiry, fit inline; so do four trades.
    {
        ResultRecorder recorder(OrderAccepted{10, Side::SELL, 100, 50});
        for (unsigned int i = 1; i <= 4; ++i) {
            recorder.trade(Trade(i, 20 + i, 10, 100, 10, Side::SELL));
        }
        recorder.expire(10);
        const SubmissionResult result = recorder.finish();
        expect(result.getEvents().size() == 6 && !result.getEvents().usesHeap(),
               "an acceptance, four trades, and an expiry should be held inline");
        expect(!result.getGeneratedTrades().usesHeap(), "four trades should be held inline");
    }

    expectThrows([] { ResultRecorder(OrderAccepted{0, Side::BUY, 100, 5}); },
                 "a recorder should refuse to open for the reserved ID 0");
    expectThrows([] { ResultRecorder(OrderAccepted{5, Side::BUY, 100, 0}); },
                 "a recorder should refuse to open for an order with no quantity");

    // Each trade must be the current order's, on its side, within its limit, and no more
    // than it has open.
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).trade(Trade(1, 2, 8, 100, 3, Side::BUY)); },
                 "a trade for another incoming order should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).trade(Trade(1, 2, 7, 100, 3, Side::SELL)); },
                 "a trade whose resting order is on the incoming order's own side should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).trade(Trade(1, 2, 7, 101, 3, Side::BUY)); },
                 "a buy's trade above its limit should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::SELL, 100, 10}).trade(Trade(1, 2, 7, 99, 3, Side::SELL)); },
                 "a sell's trade below its limit should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        recorder.trade(Trade(1, 2, 7, 100, 6, Side::BUY));
        recorder.trade(Trade(2, 3, 7, 100, 5, Side::BUY));
    }, "trades beyond the order's quantity should be refused");

    // So must each prevention.
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).prevented(SelfTradePrevented{8, 2, 100, 3}, Side::BUY); },
                 "a prevention for another incoming order should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).prevented(SelfTradePrevented{7, 2, 100, 3}, Side::SELL); },
                 "a prevention whose resting order is on the incoming order's own side should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).prevented(SelfTradePrevented{7, 2, 101, 3}, Side::BUY); },
                 "a buy's prevention above its limit should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::SELL, 100, 10}).prevented(SelfTradePrevented{7, 2, 99, 0}, Side::SELL); },
                 "a sell's prevention below its limit should be refused");

    // An expiry is the order's whole open quantity, at most once, and nothing of that order follows it.
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).expire(9); },
                 "an expiry of less than the open quantity should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{7, Side::BUY, 100, 10}).expire(11); },
                 "an expiry of more than the open quantity should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        recorder.trade(Trade(1, 2, 7, 100, 10, Side::BUY));
        recorder.expire(0);
    }, "an expiry of nothing should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        recorder.trade(Trade(1, 2, 7, 100, 4, Side::BUY));
        recorder.expire(6);
        recorder.expire(6);
    }, "a second expiry of one order should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        recorder.expire(10);
        recorder.trade(Trade(1, 2, 7, 100, 4, Side::BUY));
    }, "a trade after the order's expiry should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        recorder.expire(10);
        recorder.prevented(SelfTradePrevented{7, 2, 100, 3}, Side::BUY);
    }, "a prevention after the order's expiry should be refused");

    // A cascade: each triggered stop opens its own part, and the submitted order's remaining
    // quantity and fill status are fixed when its own part ends.
    {
        ResultRecorder recorder(OrderAccepted{1, Side::SELL, 100, 10});
        recorder.trade(Trade(1, 50, 1, 100, 4, Side::SELL));
        recorder.triggered(OrderAccepted{20, Side::SELL, 0, 5}, 101, 100);
        recorder.trade(Trade(2, 51, 20, 99, 2, Side::SELL));
        recorder.trade(Trade(3, 1, 20, 3, 3, Side::SELL));  // a market stop may trade at any price
        recorder.triggered(OrderAccepted{21, Side::BUY, 105, 8}, 104, 99);
        recorder.trade(Trade(4, 52, 21, 105, 1, Side::BUY));
        recorder.expire(7);
        const SubmissionResult result = recorder.finish();
        expect(result.getEvents() == Events{OrderAccepted{1, Side::SELL, 100, 10}, TradeExecuted{1, 50, 100, 4},
                                            StopTriggered{20, 101, 100}, TradeExecuted{20, 51, 99, 2},
                                            TradeExecuted{20, 1, 3, 3}, StopTriggered{21, 104, 99},
                                            TradeExecuted{21, 52, 105, 1}, OrderExpired{21, 7}},
               "a cascade should hold each stop's trigger, then its own events, in execution order");
        expect(result.getGeneratedTrades().size() == 4, "the result's trades should include the cascade's");
        expect(result.getSubmissionId() == 1 && result.getRemainingQuantity() == 6
                   && result.getFillStatus() == FillStatus::PARTIAL,
               "remaining quantity and fill status should describe the submitted order at the end of its own match");
    }
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{1, Side::SELL, 100, 10});
        recorder.triggered(OrderAccepted{20, Side::SELL, 0, 5}, 101, 100);
        recorder.trade(Trade(1, 50, 1, 100, 4, Side::SELL));
    }, "once a stop has triggered, a trade for the submitted order should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{1, Side::SELL, 100, 10});
        recorder.triggered(OrderAccepted{20, Side::SELL, 98, 5}, 101, 100);
        recorder.trade(Trade(1, 50, 20, 97, 4, Side::SELL));
    }, "a triggered stop-limit's trade beyond its limit should be refused");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{1, Side::SELL, 100, 10});
        recorder.triggered(OrderAccepted{20, Side::SELL, 0, 5}, 101, 100);
        recorder.trade(Trade(1, 50, 20, 97, 4, Side::SELL));
        recorder.expire(2);
    }, "a triggered stop's trades plus expiry should not exceed its quantity");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{1, Side::SELL, 100, 10});
        recorder.triggered(OrderAccepted{20, Side::SELL, 0, 5}, 101, 100);
        recorder.expire(5);
        recorder.trade(Trade(1, 50, 20, 97, 4, Side::SELL));
    }, "a trade after a triggered stop's expiry should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{1, Side::SELL, 100, 10}).triggered(OrderAccepted{0, Side::SELL, 0, 5}, 101, 100); },
                 "a triggered stop with the reserved ID 0 should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{1, Side::SELL, 100, 10}).triggered(OrderAccepted{20, Side::SELL, 0, 0}, 101, 100); },
                 "a triggered stop with no quantity should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{1, Side::SELL, 100, 10}).triggered(OrderAccepted{20, Side::SELL, 0, 5}, 0, 100); },
                 "a triggered stop with no stop price should be refused");
    expectThrows([] { ResultRecorder(OrderAccepted{1, Side::SELL, 100, 10}).triggered(OrderAccepted{20, Side::SELL, 0, 5}, 101, 0); },
                 "a triggered stop with no trade price should be refused");

    // A finished recorder takes no more.
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        static_cast<void>(recorder.finish());
        recorder.expire(10);
    }, "a finished recorder should refuse further appends");
    expectThrows([] {
        ResultRecorder recorder(OrderAccepted{7, Side::BUY, 100, 10});
        static_cast<void>(recorder.finish());
        static_cast<void>(recorder.finish());
    }, "a finished recorder should not finish again");

    // accepted() is shorthand for a recorder fed only trades.
    expect(SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 99, 5, Side::BUY)}, 3).getEvents()
               == Events{OrderAccepted{12, Side::BUY, 100, 8}, TradeExecuted{12, 2, 99, 5}},
           "accepted() should hold the acceptance then one TradeExecuted per trade");
}

}  // namespace

int main() {
    using order_book::RejectionReason;
    using order_book::SubmissionResult;
    using order_book::SubmissionStatus;
    using order_book::Trade;
    using order_book::FillStatus;
    using order_book::Side;

    // Results are built only through accepted() and rejected(), so none can be half-formed.
    static_assert(!std::is_constructible_v<SubmissionResult, unsigned int>,
                  "SubmissionResult should not be constructible from a bare submission ID");

    const SubmissionResult resting = SubmissionResult::accepted({11, Side::BUY, 100, 5}, {}, 5);
    expect(resting.getSubmissionId() == 11, "an accepted result should preserve the submission ID");
    expect(resting.getGeneratedTrades().empty(), "an accepted result with no trades should report none");
    expect(resting.getRemainingQuantity() == 5, "an accepted result should preserve the remaining quantity");
    expect(resting.getFillStatus() == FillStatus::NONE, "an accepted result with no trades should report no fill");
    expect(resting.getStatus() == SubmissionStatus::ACCEPTED, "an accepted result should be accepted");
    expect(resting.getRejectionReason() == RejectionReason::NONE, "an accepted result should have no rejection reason");
    expect(SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 100, 5, Side::BUY)}, 3).getFillStatus() == FillStatus::PARTIAL,
           "an accepted result with trades and quantity left should be partially filled");
    expect(SubmissionResult::accepted({13, Side::BUY, 100, 5}, {Trade(1, 2, 13, 100, 5, Side::BUY)}, 0).getFillStatus() == FillStatus::FILLED,
           "an accepted result with trades and nothing left should be filled");
    expectThrows([] { SubmissionResult::accepted({0, Side::BUY, 100, 5}, {}, 5); }, "an accepted result should reject a non-positive submission ID");

    // The acceptance, trades, and remainder must describe one consistent submission.
    expectThrows([] { SubmissionResult::accepted({5, Side::BUY, 100, 0}, {}, 0); },
                 "an accepted result should reject an acceptance with no quantity");
    expectThrows([] { SubmissionResult::accepted({5, Side::BUY, 0, 5}, {}, 5); },
                 "an accepted result should reject an acceptance with a zero price");
    expectThrows([] { SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 99, 100, 5, Side::BUY)}, 3); },
                 "an accepted result should reject a trade for a different incoming order");
    expectThrows([] { SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 100, 5, Side::SELL)}, 3); },
                 "an accepted result should reject a trade on the other side");
    expectThrows([] { SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 100, 5, Side::BUY)}, 4); },
                 "an accepted result should reject trades and a remainder exceeding the accepted quantity");
    expectThrows([] { SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 100, 5, Side::BUY)}, 2); },
                 "an accepted result should reject trades and a remainder short of the accepted quantity");
    expectThrows([] { SubmissionResult::accepted({11, Side::BUY, 100, 5}, {}, 4); },
                 "an accepted result with no trades should leave the whole accepted quantity");
    expectThrows([] { SubmissionResult::accepted({12, Side::BUY, 100, 8}, {Trade(1, 2, 12, 101, 5, Side::BUY)}, 3); },
                 "an accepted buy should reject a trade above its limit price");
    expectThrows([] { SubmissionResult::accepted({12, Side::SELL, 100, 8}, {Trade(1, 2, 12, 99, 5, Side::SELL)}, 3); },
                 "an accepted sell should reject a trade below its limit price");
    expect(SubmissionResult::accepted({12, Side::BUY, 100, 8},
                                       {Trade(1, 2, 12, 99, 5, Side::BUY), Trade(2, 3, 12, 100, 3, Side::BUY)}, 0)
                   .getFillStatus() == FillStatus::FILLED,
           "an accepted buy may trade at or better than its limit across several trades");
    expect(SubmissionResult::accepted({12, Side::SELL, 100, 8}, {Trade(1, 2, 12, 101, 5, Side::SELL)}, 3)
                   .getFillStatus() == FillStatus::PARTIAL,
           "an accepted sell may trade above its limit");

    const SubmissionResult refused = SubmissionResult::rejected(14, 5, RejectionReason::DUPLICATE_ORDER_ID);
    expect(refused.getSubmissionId() == 14, "a rejected result should preserve the submission ID");
    expect(refused.getStatus() == SubmissionStatus::REJECTED, "a rejected result should be rejected");
    expect(refused.getRejectionReason() == RejectionReason::DUPLICATE_ORDER_ID, "a rejected result should keep its reason");
    expect(refused.getGeneratedTrades().empty() && refused.getFillStatus() == FillStatus::NONE,
           "a rejected result should report no execution");
    expect(refused.getRemainingQuantity() == 5, "a rejected result should report the submitted quantity as unexecuted");
    expect(SubmissionResult::rejected(0, 5, RejectionReason::INVALID_ORDER_ID).getSubmissionId() == 0,
           "a rejected result may echo the reserved ID 0");
    expectThrows([] { SubmissionResult::rejected(15, 5, RejectionReason::NONE); },
                 "a rejected result should require a rejection reason");

    using order_book::BookEvent;
    using order_book::CancellationResult;

    // Cancellation results too are built only through their factories.
    static_assert(!std::is_default_constructible_v<CancellationResult>,
                  "CancellationResult should not be default constructible");
    static_assert(!std::is_aggregate_v<CancellationResult>,
                  "CancellationResult should not be an aggregate whose fields could disagree");

    const CancellationResult cancelled = CancellationResult::cancelled(7, 30);
    expect(cancelled.isSuccess(), "a cancelled result should report success");
    expect(!cancelled.getRejectionReason().has_value(), "a cancelled result should carry no rejection reason");
    expect(cancelled.getEvents() == std::vector<BookEvent>{order_book::OrderCancelled{7, 30}},
           "a cancelled result should hold exactly one OrderCancelled with the ID and cancelled quantity");
    expectThrows([] { CancellationResult::cancelled(0, 30); }, "a cancelled result should reject the reserved ID 0");
    expectThrows([] { CancellationResult::cancelled(7, 0); }, "a cancelled result should reject a zero cancelled quantity");

    const CancellationResult refusedCancel = CancellationResult::rejected(8, RejectionReason::ORDER_NOT_FOUND);
    expect(!refusedCancel.isSuccess(), "a rejected cancellation should report failure");
    expect(refusedCancel.getRejectionReason() == RejectionReason::ORDER_NOT_FOUND,
           "a rejected cancellation should keep its reason");
    expect(refusedCancel.getEvents()
               == std::vector<BookEvent>{order_book::CancelRejected{8, RejectionReason::ORDER_NOT_FOUND}},
           "a rejected cancellation should hold exactly one CancelRejected with the ID and reason");
    expect(CancellationResult::rejected(0, RejectionReason::ORDER_NOT_FOUND).getEvents()
               == std::vector<BookEvent>{order_book::CancelRejected{0, RejectionReason::ORDER_NOT_FOUND}},
           "a rejected cancellation may echo the reserved ID 0");
    expectThrows([] { CancellationResult::rejected(8, RejectionReason::NONE); },
                 "a rejected cancellation should require a rejection reason");

    testResultRecorder();

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
