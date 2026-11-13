#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/cli/commands.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/wasm/browser_engine.hpp"
#include "order_book/wasm/page_connection.hpp"
#include "order_book/wasm/sandbox.hpp"
#include "order_book/wasm/terminal.hpp"
#include "wasm_page.hpp"

// The sandbox's standing connection, natively and under Emscripten, on a BrowserEngine seeded
// from the shipped seed: typed commands checked and encoded, their answers decoded with the text
// send prints, refusals before anything is sent, a credit window that stops reading until the
// page consumes, and commands lost with the connection. See docs/internals/wasm.md.
namespace {

using namespace order_book;
using namespace order_book::wasm;
using namespace order_book::wasm_testing;

void expect(bool condition, const std::string& message) { check(condition, message); }

std::filesystem::path source;

// A started engine, seeded as the engine worker seeds it before ready.
struct Seeded {
    Seeded() : engine((source / "src/wasm/instruments.csv").string(), {}, page.callbacks()), running(engine) {
        page.waitUntilReady();
        const std::string seed = (source / "src/wasm/seed.csv").string();
        const std::vector<std::string_view> args{seed, "--connect", "127.0.0.1:9000"};
        std::variant<PageSend, cli::CommandResult> started = startSend(engine.sockets(), args);
        expect(std::holds_alternative<PageSend>(started), "the seed is sent");
        const cli::CommandResult seeded = pumpUntilDone(page, [&] { return std::get<PageSend>(started).pump(); });
        expect(seeded.exit == 0, "the seed is answered whole:\n" + seeded.err);
    }

    Page page;
    BrowserEngine engine;
    Running running;
};

SandboxSent sent(const std::variant<SandboxSent, std::string>& result, const std::string& what) {
    if (const auto* problem = std::get_if<std::string>(&result)) {
        expect(false, what + " is sent, not refused: " + *problem);
    }
    return std::get<SandboxSent>(result);
}

// Pumps on each wake until this many answers have come.
std::vector<SandboxAnswer> answers(Sandbox& sandbox, Page& page, std::size_t count) {
    std::vector<SandboxAnswer> came;
    while (came.size() < count) {
        const std::size_t seen = page.wakesSoFar();
        for (SandboxAnswer& answer : sandbox.pump()) {
            came.push_back(std::move(answer));
        }
        if (came.size() < count) {
            static_cast<void>(page.waitPast(seen));
        }
    }
    return came;
}

template <typename Event>
const Event& eventOf(const SandboxAnswer& answer, std::size_t index) {
    expect(index < answer.events.size(), "the answer has an event " + std::to_string(index) + ":\n" + answer.text);
    const auto* event = std::get_if<BookEvent>(&answer.events[index].decoded);
    expect(event != nullptr && std::holds_alternative<Event>(*event), "event " + std::to_string(index) + " is the one expected:\n" + answer.text);
    return std::get<Event>(*event);
}

OrderInput limit(std::string side, std::uint64_t quantity, std::uint64_t price) {
    OrderInput order;
    order.side = std::move(side);
    order.quantity = std::to_string(quantity);
    order.price = std::to_string(price);
    return order;
}

void testTypedCommandsAndTheirAnswers() {
    Seeded run;
    Sandbox sandbox(run.engine.shadows());
    sandbox.connect(run.engine.sockets());

    // The seed rests AAPL's bids at 97 to 99 and asks at 101 to 103, IDs 1 to 6.
    const SandboxSent bid = sent(sandbox.submit("AAPL", limit("BUY", 5, 100)), "a bid");
    expect(bid.correlation == 1 && bid.orderId == OrderId{7}, "the first command is correlation 1, and its ID is past the seed's");
    std::vector<SandboxAnswer> came = answers(sandbox, run.page, 1);
    expect(came[0].correlation == 1 && came[0].orderId == OrderId{7} && came[0].lost.empty(), "the bid is answered");
    const OrderAccepted& accepted = eventOf<OrderAccepted>(came[0], 0);
    expect(accepted.orderId == 7 && accepted.side == Side::BUY && accepted.price == 100 && accepted.quantity == 5, "decoded as sent");
    expect(came[0].text == "line 1: OrderAccepted id=7 side=BUY price=100 qty=5\n", "its text is what send prints:\n" + came[0].text);
    expect(came[0].frameBytes == came[0].events[0].frameBytes && came[0].frameBytes > network::frameHeaderBytes, "it counts its frame's bytes");

    // A market sell through the bid and into the seed's.
    OrderInput sell;
    sell.side = "SELL";
    sell.quantity = "7";
    const SandboxSent market = sent(sandbox.submit("AAPL", sell), "a market sell");
    came = answers(sandbox, run.page, 1);
    expect(came[0].orderId == market.orderId && came[0].events.size() == 3, "accepted, then two trades:\n" + came[0].text);
    expect(eventOf<OrderAccepted>(came[0], 0).price == 0, "a market order carries no price");
    const TradeExecuted& first = eventOf<TradeExecuted>(came[0], 1);
    expect(first.restingOrderId == 7 && first.executionPrice == 100 && first.executionQuantity == 5, "it takes the bid first");
    expect(eventOf<TradeExecuted>(came[0], 2).executionQuantity == 2, "then the seed's best bid");

    const SandboxSent cancelled = sent(sandbox.cancel("AAPL", "3"), "a cancel");
    expect(!cancelled.orderId, "a cancel assigns no ID");
    came = answers(sandbox, run.page, 1);
    expect(eventOf<OrderCancelled>(came[0], 0).orderId == 3, "the seed's bid at 97 is cancelled:\n" + came[0].text);

    sent(sandbox.replace("AAPL", "4", "104", "3"), "a replace");
    came = answers(sandbox, run.page, 1);
    expect(eventOf<OrderReplaced>(came[0], 0).price == 104, "the ask moves to 104:\n" + came[0].text);

    sent(sandbox.cancel("AAPL", "999"), "a cancel of nothing");
    came = answers(sandbox, run.page, 1);
    expect(eventOf<CancelRejected>(came[0], 0).reason == RejectionReason::ORDER_NOT_FOUND, "the book rejects it:\n" + came[0].text);

    // Refused before anything is sent, taking no correlation.
    const std::variant<SandboxSent, std::string> offList = sandbox.submit("XYZ", limit("BUY", 1, 1));
    expect(std::get_if<std::string>(&offList) != nullptr && std::get<std::string>(offList) == "XYZ is not on the instrument list",
           "a symbol off the list is refused");
    const std::variant<SandboxSent, std::string> none = sandbox.submit("AAPL", limit("BUY", 0, 1));
    expect(std::get_if<std::string>(&none) != nullptr && std::get<std::string>(none).starts_with("quantity: "),
           "the parser's column and message: " + (std::get_if<std::string>(&none) ? std::get<std::string>(none) : std::string()));
    OrderInput fraction = limit("BUY", 1, 1);
    fraction.price = "100.7";  // as the page's 100.7 arrives
    const std::variant<SandboxSent, std::string> decimal = sandbox.submit("AAPL", fraction);
    expect(std::get_if<std::string>(&decimal) != nullptr && std::get<std::string>(decimal).starts_with("price: "),
           "a fraction is refused as a row's would be, not truncated");
    const SandboxSent next = sent(sandbox.submit("MSFT", limit("SELL", 1, 250)), "the next");
    expect(next.correlation == 6, "refusals took no correlation");
    static_cast<void>(answers(sandbox, run.page, 1));

    expect(run.running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
    expect(sandbox.disconnect().empty(), "nothing was waiting");
    const std::variant<SandboxSent, std::string> after = sandbox.submit("AAPL", limit("BUY", 1, 1));
    expect(std::get_if<std::string>(&after) != nullptr && std::get<std::string>(after) == "the engine isn't running",
           "with the engine stopped, nothing is sent");
}

// The window: a page that consumes nothing stops the reading once W bytes are out, and the
// rest comes as it consumes.
void testTheWindowStopsTheReading() {
    Seeded run;
    // A window of one OrderAccepted, which every resting bid gets, and no more.
    Sandbox probe(run.engine.shadows());
    probe.connect(run.engine.sockets());
    sent(probe.submit("NVDA", limit("BUY", 1, 40)), "a probe");
    const std::size_t oneAnswer = answers(probe, run.page, 1)[0].frameBytes;
    probe.disconnect();

    Sandbox sandbox(run.engine.shadows(), oneAnswer);
    sandbox.connect(run.engine.sockets());
    expect(sandbox.room() == oneAnswer, "an empty window has all its room");
    for (std::uint64_t price = 41; price <= 44; ++price) {
        sent(sandbox.submit("NVDA", limit("BUY", 1, price)), "a bid");
    }
    std::vector<SandboxAnswer> came = answers(sandbox, run.page, 1);
    expect(came.size() == 1 && came[0].correlation == 1, "one answer fits");
    expect(sandbox.room() == 0, "and then the window is full, so the next pump reads nothing");
    for (int pumps = 0; pumps < 100; ++pumps) {
        expect(sandbox.pump().empty(), "nothing more comes until the page consumes");
    }
    std::vector<network::Correlation> order{came[0].correlation};
    for (int consumed = 0; consumed < 3; ++consumed) {
        sandbox.consumed(came[0].frameBytes);
        came = answers(sandbox, run.page, 1);
        expect(came.size() == 1, "each consumption lets one more through");
        order.push_back(came[0].correlation);
    }
    expect(order == std::vector<network::Correlation>{1, 2, 3, 4}, "in order");
    expect(run.running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
}

// Commands waiting when the run ends are lost, each said so.
void testCommandsLostWithTheConnection() {
    Seeded run;
    Sandbox sandbox(run.engine.shadows(), 0);  // a window that never opens, so nothing is read
    sandbox.connect(run.engine.sockets());
    sent(sandbox.submit("TSLA", limit("SELL", 1, 160)), "a sell");
    sent(sandbox.cancel("TSLA", "1"), "a cancel");
    static_cast<void>(sandbox.pump());  // sends them
    expect(run.running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
    const std::vector<SandboxAnswer> lost = sandbox.disconnect();
    expect(lost.size() == 2 && lost[0].correlation == 1 && lost[1].correlation == 2, "both waiting commands are lost");
    expect(lost[0].orderId.has_value() && !lost[1].orderId && lost[0].events.empty() && !lost[0].lost.empty(), "each says so");
}

// At most 3 connections are ever open, the page's two standing ones and a send's: a fourth is
// refused with TOO_MANY_CONNECTIONS, as the gateway refuses one past --max-connections.
void testAtMostThreeConnections() {
    Seeded run;
    Sandbox sandbox(run.engine.shadows());
    sandbox.connect(run.engine.sockets());
    std::optional<PageConnection> second = PageConnection::open(run.engine.sockets());
    std::optional<PageConnection> third = PageConnection::open(run.engine.sockets());
    std::optional<PageConnection> fourth = PageConnection::open(run.engine.sockets());
    expect(second && third && fourth, "the set takes each, as a listener's backlog would");
    while (!fourth->done()) {
        const std::size_t seen = run.page.wakesSoFar();
        fourth->pump();
        if (!fourth->done()) {
            static_cast<void>(run.page.waitPast(seen));
        }
    }
    const std::vector<std::string>& said = fourth->stream().diagnostics();
    expect(!said.empty() && said.front().find("TOO_MANY_CONNECTIONS") != std::string::npos,
           "the fourth is refused: " + (said.empty() ? std::string("nothing said") : said.front()));
    second->pump();
    third->pump();
    expect(!second->done() && !third->done(), "the second and third stay open");
    sent(sandbox.submit("MSFT", limit("BUY", 1, 150)), "the sandbox's");
    static_cast<void>(answers(sandbox, run.page, 1));
    expect(run.running.cleanStop() == network::EngineExit::CLEAN_STOP, "a clean stop");
}

}  // namespace

int main(int argc, char** argv) {
    expect(argc == 2, "usage: wasm_sandbox_tests <source directory>");
    source = argv[1];
    testTypedCommandsAndTheirAnswers();
    testTheWindowStopsTheReading();
    testCommandsLostWithTheConnection();
    testAtMostThreeConnections();
    std::cout << "wasm sandbox tests passed" << std::endl;
    return EXIT_SUCCESS;
}
