#include "order_book/wasm/sandbox.hpp"

#include <algorithm>
#include <utility>

#include "client_rows.hpp"
#include "order_book/network/wire_codec.hpp"

namespace order_book::wasm {

    using namespace rows;

    namespace {

        constexpr const char* notRunning = "the engine isn't running";

    }  // namespace

    void Sandbox::connect(std::shared_ptr<network::PageSocketSet> sockets) {
        standing.reset();
        waiting.clear();
        commands = 0;
        read = 0;
        given = 0;
        acknowledged = 0;
        if (std::optional<PageConnection> opened = PageConnection::open(std::move(sockets))) {
            standing.emplace(std::move(*opened));
        }
    }

    std::vector<SandboxAnswer> Sandbox::disconnect() {
        std::vector<SandboxAnswer> lost = loseAll("the connection was lost before it was answered: what ran is unknown");
        standing.reset();
        return lost;
    }

    std::variant<SandboxSent, std::string> Sandbox::submit(std::string_view symbol, const OrderInput& order) {
        std::vector<std::string> row(COLUMNS);
        row[COMMAND] = "ADD";
        row[ORDER_ID] = "1";  // a stand-in for the check: the ID is assigned once the row passes
        row[SIDE] = order.side;
        row[PRICE] = order.price;
        row[QUANTITY] = order.quantity;
        row[ORDER_TYPE] = order.type;
        row[TIME_IN_FORCE] = order.timeInForce;
        row[STOP_PRICE] = order.stopPrice;
        row[POST_ONLY] = order.postOnly ? "Y" : "";
        row[OWNER] = order.owner;
        row[STP] = order.selfTradePrevention;
        row[SYMBOL] = std::string(symbol);
        return send(std::move(row));
    }

    std::variant<SandboxSent, std::string> Sandbox::cancel(std::string_view symbol, std::string_view orderId) {
        std::vector<std::string> row(COLUMNS);
        row[COMMAND] = "CANCEL";
        row[ORDER_ID] = std::string(orderId);
        row[SYMBOL] = std::string(symbol);
        return send(std::move(row));
    }

    std::variant<SandboxSent, std::string> Sandbox::replace(std::string_view symbol, std::string_view orderId, std::string_view price,
                                                            std::string_view quantity) {
        std::vector<std::string> row(COLUMNS);
        row[COMMAND] = "REPLACE";
        row[ORDER_ID] = std::string(orderId);
        row[PRICE] = std::string(price);
        row[QUANTITY] = std::string(quantity);
        row[SYMBOL] = std::string(symbol);
        return send(std::move(row));
    }

    std::variant<SandboxSent, std::string> Sandbox::send(std::vector<std::string> row) {
        fillDefaults(row);
        std::variant<replay::ReplayRecord, std::string> checked = check(std::move(row), commands + 1, shadows);
        if (auto* problem = std::get_if<std::string>(&checked)) {
            return std::move(*problem);
        }
        replay::ReplayRecord& record = std::get<replay::ReplayRecord>(checked);
        if (!standing || standing->done()) {
            return std::string(notRunning);
        }
        SandboxSent sent;
        if (auto* add = std::get_if<replay::AddCommand>(&record.command)) {
            const std::optional<OrderId> id = shadows.assignOrderId(record.symbol);
            if (!id) {
                return "no order ID is left for " + record.symbol.text();
            }
            add->orderId = *id;
            sent.orderId = *id;
        }
        ++commands;
        sent.correlation = static_cast<network::Correlation>(commands);
        standing->stream().add(network::ClientCommand{sent.correlation, record.symbol, record.command});
        waiting.emplace(sent.correlation, sent.orderId);
        return sent;
    }

    std::vector<SandboxAnswer> Sandbox::pump() {
        if (!standing) {
            return {};
        }
        std::vector<SandboxAnswer> answers;
        network::ClientStream& stream = standing->stream();
        // Reads while the window has room. A read that took all the room may have stopped inside a
        // frame, so it reads again, until a read comes up short or the answers it completed fill
        // the window; what then waits in the set comes on a later pump, after the page consumes.
        for (;;) {
            const std::size_t limit = room();
            const std::size_t got = standing->pump(limit);
            read += got;
            for (auto next = waiting.begin(); next != waiting.end();) {
                if (!stream.answered(next->first)) {
                    ++next;
                    continue;
                }
                SandboxAnswer answer;
                answer.correlation = next->first;
                answer.orderId = next->second;
                answer.events = stream.answersOf(next->first);
                answer.text = stream.answerLines(next->first);
                for (const network::ClientStream::Answer& frame : answer.events) {
                    answer.frameBytes += frame.frameBytes;
                }
                given += answer.frameBytes;
                stream.forget(next->first);
                next = waiting.erase(next);
                answers.push_back(std::move(answer));
            }
            if (got < limit || limit == 0 || room() == 0) {
                break;
            }
        }
        if (standing->done()) {
            std::vector<SandboxAnswer> lost = loseAll("the connection ended before it was answered: what ran is unknown");
            answers.insert(answers.end(), std::make_move_iterator(lost.begin()), std::make_move_iterator(lost.end()));
            standing.reset();
        }
        return answers;
    }

    void Sandbox::consumed(std::size_t bytes) noexcept { acknowledged += std::min(bytes, given - acknowledged); }

    std::size_t Sandbox::room() const noexcept {
        // The engine's Welcome, the first frame on a connection, answers no command.
        const std::size_t answering = read - std::min(read, network::helloFrameBytes);
        const std::size_t unconsumed = answering - acknowledged;
        if (unconsumed < windowBytes) {
            return windowBytes - unconsumed;
        }
        return given == acknowledged ? windowBytes : 0;
    }

    std::vector<SandboxAnswer> Sandbox::loseAll(const std::string& why) {
        std::vector<SandboxAnswer> lost;
        for (const auto& [correlation, orderId] : waiting) {
            SandboxAnswer answer;
            answer.correlation = correlation;
            answer.orderId = orderId;
            answer.lost = why;
            lost.push_back(std::move(answer));
        }
        waiting.clear();
        return lost;
    }

}  // namespace order_book::wasm
