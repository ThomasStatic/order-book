#include "order_book/runtime/matching_loop.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace order_book::runtime {
    namespace {
        void push(Ring<ResponseRecord>& responses, const ResponseRecord& record) {
            // Mid-command, so this waits in place. It can't deadlock: a gateway thread facing a
            // full command ring parks that connection and keeps draining.
            while (!responses.tryPush(record)) {
                std::this_thread::yield();
            }
        }

        // Writes with fixed buffers only, since the failure may be that nothing can be allocated.
        [[noreturn]] void abortOnEscaped(const Shard& shard, const CommandElement& element, const char* what) {
            const std::array<char, Symbol::size> symbol = shard.books().symbol(element.slot).bytes();
            const char* kind = std::holds_alternative<replay::AddCommand>(element.command)      ? "ADD"
                               : std::holds_alternative<replay::CancelCommand>(element.command) ? "CANCEL"
                                                                                                : "REPLACE";
            const OrderId orderId = std::visit([](const auto& held) { return held.orderId; }, element.command);
            std::fprintf(stderr, "order book runtime: shard %u symbol %.8s position %llu command %s id=%u: %s\n",
                         static_cast<unsigned int>(shard.books().shard()), symbol.data(),
                         static_cast<unsigned long long>(shard.nextPosition()), kind, static_cast<unsigned int>(orderId), what);
            std::fflush(stderr);
            std::abort();
        }
    }  // namespace

    MatchingLoop::MatchingLoop(Shard& matchingShard, Doorbell& bell, std::vector<RingPair> pairs, std::chrono::nanoseconds spin)
        : shard(matchingShard), doorbell(bell), rings(std::move(pairs)), spinWindow(spin) {}

    void MatchingLoop::take(const CommandElement& element, Ring<ResponseRecord>& responses) {
        if (element.control == ControlKind::STOP) {
            ++stopMarkers;
            return;
        }
        try {
            shard.process(element.command, element.slot);
        } catch (const std::exception& escaped) {
            abortOnEscaped(shard, element, escaped.what());
        } catch (...) {
            abortOnEscaped(shard, element, "an unknown exception");
        }

        const std::span<const BookEvent> events = shard.result().events();
        for (std::size_t i = 0; i < events.size(); ++i) {
            push(responses, ResponseRecord{element.correlation, element.connection, element.slot, events[i],
                                           i + 1 == events.size() ? ResponseKind::LAST_EVENT : ResponseKind::EVENT});
        }
    }

    std::size_t MatchingLoop::pass() {
        std::size_t taken = 0;
        for (const RingPair& pair : rings) {
            taken += pair.commands->drain(takeBatch, [this, &pair](const CommandElement& element) { take(element, *pair.responses); });
        }
        return taken;
    }

    bool MatchingLoop::commandsWaiting() const noexcept {
        for (const RingPair& pair : rings) {
            if (pair.commands->hasItems()) {
                return true;
            }
        }
        return false;
    }

    void MatchingLoop::sleepUntilRung() {
        doorbell.announceSleep();
        if (!commandsWaiting()) {
            doorbell.wait();
        }
        doorbell.cancelSleep();
    }

    void MatchingLoop::run() {
        using Clock = std::chrono::steady_clock;
        bool idle = false;
        Clock::time_point idleSince;
        while (!stopped()) {
            if (pass() != 0) {
                idle = false;
                continue;
            }
            if (spinWindow != std::chrono::nanoseconds::zero()) {
                const Clock::time_point now = Clock::now();
                if (!idle) {
                    idle = true;
                    idleSince = now;
                }
                if (spinWindow == unlimitedSpin || now - idleSince < spinWindow) {
                    continue;
                }
            }
            sleepUntilRung();
            idle = false;
        }
        for (const RingPair& pair : rings) {
            push(*pair.responses, ResponseRecord{0, 0, 0, BookEvent{}, ResponseKind::SHARD_STOPPED});
        }
    }

}  // namespace order_book::runtime
