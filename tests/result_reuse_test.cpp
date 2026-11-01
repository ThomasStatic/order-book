#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"
#include "order_book/replace_request.hpp"

// The out overloads against the by-value ones: a seeded stream of every kind of command, on two
// books, must give the same results, with one result reused for the whole stream.
namespace {

using namespace order_book;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// Its own generator, so the stream is the same on every platform.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    unsigned int below(unsigned int bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned int>((state >> 33) % bound);
    }

private:
    std::uint64_t state;
};

OrderRequest drawRequest(Lcg& random, OrderId id) {
    OrderRequest request{};
    request.id = id;
    request.side = random.below(2) == 0 ? Side::BUY : Side::SELL;
    request.price = 95 + random.below(11);
    request.quantity = 1 + random.below(40);
    request.owner = random.below(4);
    const unsigned int kind = random.below(10);
    if (kind == 0) {
        request.type = OrderType::MARKET;
        request.price = 0;
        request.timeInForce = TimeInForce::IOC;
    } else if (kind == 1) {
        request.timeInForce = TimeInForce::IOC;
    } else if (kind == 2) {
        request.timeInForce = TimeInForce::FOK;
    } else if (kind == 3) {
        request.postOnly = true;
    } else if (kind == 4) {
        request.type = OrderType::STOP;
        request.price = 0;
        request.timeInForce = TimeInForce::IOC;
        request.stopPrice = 90 + random.below(21);
    } else if (kind == 5) {
        request.type = OrderType::STOP_LIMIT;
        request.stopPrice = 90 + random.below(21);
    }
    return request;
}

bool sameTrades(const SubmissionTrades& left, const SubmissionTrades& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].getTradeId() != right[i].getTradeId() || left[i].getIncomingId() != right[i].getIncomingId() ||
            left[i].getRestingId() != right[i].getRestingId() ||
            left[i].getExecutionPrice() != right[i].getExecutionPrice() ||
            left[i].getExecutionQuantity() != right[i].getExecutionQuantity() ||
            left[i].getIncomingSide() != right[i].getIncomingSide()) {
            return false;
        }
    }
    return true;
}

void expectSameSubmission(const SubmissionResult& byValue, const SubmissionResult& reused, const std::string& context) {
    expect(reused.getSubmissionId() == byValue.getSubmissionId(), context + ": submission ID");
    expect(reused.getStatus() == byValue.getStatus(), context + ": status");
    expect(reused.getRejectionReason() == byValue.getRejectionReason(), context + ": rejection reason");
    expect(reused.getRemainingQuantity() == byValue.getRemainingQuantity(), context + ": remaining quantity");
    expect(reused.getFillStatus() == byValue.getFillStatus(), context + ": fill status");
    expect(sameTrades(reused.getGeneratedTrades(), byValue.getGeneratedTrades()), context + ": trades");
    expect(reused.getEvents() == byValue.getEvents(), context + ": events");
}

void expectSameReplace(const ReplaceResult& byValue, const ReplaceResult& reused, const std::string& context) {
    expect(reused.isSuccess() == byValue.isSuccess(), context + ": success");
    expect(reused.getRejectionReason() == byValue.getRejectionReason(), context + ": rejection reason");
    expect(reused.getRemainingQuantity() == byValue.getRemainingQuantity(), context + ": remaining quantity");
    expect(reused.getFillStatus() == byValue.getFillStatus(), context + ": fill status");
    expect(sameTrades(reused.getGeneratedTrades(), byValue.getGeneratedTrades()), context + ": trades");
    expect(reused.getEvents() == byValue.getEvents(), context + ": events");
}

void testStreamsAgree(std::uint64_t seed) {
    Lcg random(seed);
    OrderBook byValue;
    OrderBook reusing;
    SubmissionResult submission = SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE);
    ReplaceResult replacement = ReplaceResult::rejected(1, RejectionReason::ORDER_NOT_FOUND);

    OrderId nextId = 1;
    for (int step = 0; step < 5000; ++step) {
        const std::string context = "seed " + std::to_string(seed) + " step " + std::to_string(step);
        const unsigned int roll = random.below(100);
        if (roll < 65) {
            // Now and then a malformed or duplicate request, so rejections go through out too.
            OrderRequest request = drawRequest(random, nextId++);
            if (roll < 4) {
                request.quantity = 0;
            } else if (roll < 8) {
                request.id = 1 + random.below(nextId);
            }
            reusing.submitOrder(request, submission);
            expectSameSubmission(byValue.submitOrder(request), submission, context);
        } else if (roll < 85) {
            const ReplaceRequest request{1 + random.below(nextId), 95 + random.below(11), 1 + random.below(40),
                                         random.below(5) == 0, random.below(4)};
            reusing.replaceOrder(request, replacement);
            expectSameReplace(byValue.replaceOrder(request), replacement, context);
        } else {
            const OrderId id = 1 + random.below(nextId);
            const CancellationResult first = byValue.cancelOrder(id);
            const CancellationResult second = reusing.cancelOrder(id);
            expect(first.getEvents() == second.getEvents(), context + ": cancel");
        }
    }
    expect(byValue.activeOrderCount() == reusing.activeOrderCount(), "the books end alike");
    expect(reusing.checkInvariants().isValid(), "the reusing book stays sound");
}

// A result that has grown keeps its block: a later command that needs less, and one as large,
// use the same memory.
void testHeapBlocksSurvive() {
    OrderBook book;
    SubmissionResult result = SubmissionResult::rejected(1, 1, RejectionReason::ORDER_NOT_ACTIVE);
    result.reserve(64, 128);
    const void* const trades = result.getGeneratedTrades().data();
    const void* const events = result.getEvents().data();

    for (OrderId id = 1; id <= 20; ++id) {
        OrderRequest resting{};
        resting.id = id;
        resting.side = Side::SELL;
        resting.price = 100;
        resting.quantity = 1;
        book.submitOrder(resting, result);
        expect(result.getGeneratedTrades().data() == trades && result.getEvents().data() == events,
               "a small command keeps the result's blocks");
        expect(result.getEvents().size() == 1, "a reused result holds only this command's events");
    }

    OrderRequest sweep{};
    sweep.id = 100;
    sweep.side = Side::BUY;
    sweep.price = 100;
    sweep.quantity = 20;
    book.submitOrder(sweep, result);
    expect(result.getGeneratedTrades().size() == 20, "a sweep reports every trade");
    expect(result.getGeneratedTrades().data() == trades && result.getEvents().data() == events,
           "a sweep within capacity keeps the result's blocks");

    OrderRequest refused{};
    refused.id = 200;
    book.submitOrder(refused, result);
    expect(result.getStatus() == SubmissionStatus::REJECTED && result.getGeneratedTrades().empty(),
           "a rejection clears what the last command left");
    expect(result.getEvents().size() == 1, "a rejection holds its one event");
    expect(result.getGeneratedTrades().data() == trades && result.getEvents().data() == events,
           "a rejection keeps the result's blocks");
}

void testReplaceKeepsItsBlocks() {
    OrderBook book;
    OrderRequest resting{};
    resting.id = 1;
    resting.side = Side::SELL;
    resting.price = 100;
    resting.quantity = 5;
    book.submitOrder(resting);

    ReplaceResult result = ReplaceResult::rejected(1, RejectionReason::ORDER_NOT_FOUND);
    result.reserve(16, 32);
    const void* const events = result.getEvents().data();
    book.replaceOrder(ReplaceRequest{1, 101, 5}, result);
    expect(result.isSuccess() && result.getEvents().data() == events, "a replace keeps the result's blocks");
    book.replaceOrder(ReplaceRequest{2, 101, 5}, result);
    expect(!result.isSuccess() && result.getEvents().size() == 1 && result.getEvents().data() == events,
           "a refused replace keeps the result's blocks");
}

}  // namespace

int main() {
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 2026ULL}) {
        testStreamsAgree(seed);
    }
    testHeapBlocksSurvive();
    testReplaceKeepsItsBlocks();
    std::cout << "result reuse tests passed\n";
    return EXIT_SUCCESS;
}
