#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "benchmark_runner.hpp"
#include "clock.hpp"
#include "engine_process.hpp"
#include "histogram.hpp"
#include "order_book/book_event.hpp"
#include "timetable.hpp"
#include "workload.hpp"

// The round-trip interval: from a command's send, over loopback, through the shipped
// order_book_engine's gateway and matching thread, to the frame flagged last in its response. The
// engine is a child process on port 0, so this measures what ships and shares no heap with the
// client. It has no log until the log writer lands. Each target is one fresh engine with one book, preloaded over the
// wire, untimed, and one connection.
namespace order_book::latency {

struct RoundTripSetup {
    std::filesystem::path engine;
    // Passed to the engine as they are: --pin-matching, --pin-gateway, and --thread-priority.
    std::vector<std::string> engineArguments;
    // Runs first on the open-loop driver's receiving thread, for pinning it.
    std::function<void()> setUpReceiver;
};

// What one open-loop run measured.
struct RoundTripRun {
    Histogram response;  // from the scheduled send to the frame flagged last
    Histogram first;     // from the scheduled send to the response's first frame
    Histogram lateness;  // from the scheduled send to the real send
    Ticks elapsed = 0;   // from the start to the last frame
    bool sustained = true;
};

class RoundTripTarget {
public:
    // Starts the engine, connects, and preloads the book, untimed. The workload is copied. Throws std::runtime_error if the engine doesn't start or refuses the preload.
    RoundTripTarget(const benchmark::Workload& workload, const RoundTripSetup& setup);
    ~RoundTripTarget();

    // Closed-loop: sends command index and returns once the frame flagged last is back, on this
    // thread. What comes back is counted after the closing reading.
    template <typename DriverClock>
    Stamps operator()(std::size_t index, DriverClock& clock) {
        const std::span<const std::byte> frame = frameOf(index);
        events_.clear();
        const Ticks started = clock.now();
        benchmark::sendAll(link_.socket, frame);
        bool last = false;
        while (!last) {
            const std::size_t bytes = receiveSome();
            if (bytes != 0) {
                last = takeFrames(index);
            }
        }
        const Ticks done = clock.now();
        countAnswer(index);
        return {started, done};
    }

    // Open-loop: this thread sends command i at start + schedule[i], whatever has come back, and
    // another thread stamps each frame as it arrives. A command scheduled within discard ticks of
    // the start is sent and checked but goes in no histogram. The schedule must cover every timed
    // command.
    template <typename DriverClock>
    RoundTripRun runOpenLoop(const std::vector<Ticks>& schedule, DriverClock& clock, Ticks discard);

    // Half-closes the connection, reads it to its end, and stops the engine. Throws
    // std::runtime_error, describing both, if what the frames said differs from the workload's
    // prediction, or if the engine didn't stop cleanly or its book grew.
    void checkOutcome();

    std::uint64_t capacityGrowths() const { return 0; }  // checkOutcome refuses any growth

private:
    std::span<const std::byte> frameOf(std::size_t index) const {
        return std::span<const std::byte>(frames_).subspan(index * benchmark::commandFrameBytes, benchmark::commandFrameBytes);
    }
    // Reads what has arrived into the reader, and returns how many bytes that was.
    std::size_t receiveSome();
    // Takes every whole frame the reader holds, keeping each event, and returns whether one was
    // flagged last. Throws if a frame is not an event of command expected.
    bool takeFrames(std::size_t expected);
    // Counts command index, answered, and the events takeFrames kept.
    void countAnswer(std::size_t index);

    std::vector<benchmark::Workload> workloads_;
    std::unique_ptr<benchmark::EngineProcess> engine_;
    benchmark::LoopbackConnection link_;
    std::vector<std::byte> frames_;
    std::vector<replay::ReplayRecord> records_;
    std::vector<BookEvent> events_;
    std::array<std::byte, 64 * 1024> buffer_{};
    std::function<void()> setUpReceiver_;
    benchmark::WorkloadOutcome counted_;
    std::size_t answered_ = 0;  // commands answered and counted
};

template <typename DriverClock>
RoundTripRun RoundTripTarget::runOpenLoop(const std::vector<Ticks>& schedule, DriverClock& clock, Ticks discard) {
    if (schedule.size() != records_.size()) {
        throw std::invalid_argument("round trip: a schedule must time every command");
    }
    RoundTripRun run;
    std::atomic<bool> receiverReady{false};
    std::atomic<bool> started{false};
    std::atomic<bool> failed{false};
    std::exception_ptr failure;
    Ticks start = 0;  // written before started is set, and read after it is seen
    std::unique_ptr<ArrivalTally> tally;

    std::thread receiver([&] {
        try {
            if (setUpReceiver_) {
                setUpReceiver_();
            }
            receiverReady.store(true, std::memory_order_seq_cst);
            while (!started.load(std::memory_order_seq_cst)) {
                if (failed.load(std::memory_order_seq_cst)) {
                    return;
                }
            }
            tally = std::make_unique<ArrivalTally>(schedule, start, discard);
            network::FrameReader& reader = link_.reader;
            while (!tally->complete()) {
                if (failed.load(std::memory_order_seq_cst)) {
                    return;
                }
                const std::size_t bytes = receiveSome();
                if (bytes == 0) {
                    continue;
                }
                const Ticks at = clock.now();  // every frame in this read arrived by now
                for (network::FrameResult result = reader.next(); std::holds_alternative<network::Frame>(result); result = reader.next()) {
                    const network::Frame& frame = std::get<network::Frame>(result);
                    const std::optional<network::EventFrame> event = network::decodeEvent(frame);
                    if (!event || frame.correlation >= schedule.size()) {
                        throw std::runtime_error("round trip: an answer was not an event of a command sent");
                    }
                    tally->arrived(static_cast<std::size_t>(frame.correlation), at, frame.last);
                    benchmark::countResponse(event->event, counted_);
                }
            }
        } catch (...) {
            failure = std::current_exception();
            failed.store(true, std::memory_order_seq_cst);
        }
    });

    while (!receiverReady.load(std::memory_order_seq_cst) && !failed.load(std::memory_order_seq_cst)) {
    }
    start = clock.now();
    started.store(true, std::memory_order_seq_cst);
    try {
        for (std::size_t index = 0; index < schedule.size() && !failed.load(std::memory_order_seq_cst); ++index) {
            const Ticks scheduled = start + schedule[index];
            const Ticks sentAt = clock.spinUntil(scheduled);
            // Not sendAll: a full socket must not hold this thread once the receiver has failed.
            for (std::span<const std::byte> frame = frameOf(index); !frame.empty();) {
                if (failed.load(std::memory_order_seq_cst)) {
                    break;
                }
                const std::optional<std::size_t> taken = link_.socket.send(frame);
                if (!taken) {
                    throw std::runtime_error("round trip: the engine closed the connection while it was being sent to");
                }
                frame = frame.subspan(*taken);
            }
            if (schedule[index] >= discard) {
                run.lateness.record(sentAt - scheduled);
            }
        }
    } catch (...) {
        failed.store(true, std::memory_order_seq_cst);
        receiver.join();
        throw;
    }
    receiver.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    for (const replay::ReplayRecord& record : records_) {
        benchmark::countCommand(record.command, {}, counted_);
        ++answered_;
    }
    run.response = tally->response();
    run.first = tally->first();
    run.elapsed = tally->lastArrival() - start;
    run.sustained = tally->sustained();
    return run;
}

}  // namespace order_book::latency
