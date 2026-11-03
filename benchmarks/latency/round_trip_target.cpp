#include "round_trip_target.hpp"

#include <utility>

namespace order_book::latency {

RoundTripTarget::RoundTripTarget(const benchmark::Workload& workload, const RoundTripSetup& setup)
    : workloads_{workload}, setUpReceiver_(setup.setUpReceiver) {
    engine_ = std::make_unique<benchmark::EngineProcess>(setup.engine, workloads_, benchmark::EngineOptions{1, setup.engineArguments});
    benchmark::LoopbackClients clients(workloads_, {});
    clients.connect(engine_->port());
    clients.preload(benchmark::defaultGatewayWindow);
    link_ = clients.take(0);
    records_ = benchmark::workloadRecords(workloads_.front(), benchmark::instrumentSymbol(0));
    frames_ = benchmark::commandFrames(records_, benchmark::instrumentSymbol(0));
    events_.reserve(64);
}

RoundTripTarget::~RoundTripTarget() = default;

std::size_t RoundTripTarget::receiveSome() {
    const network::ReadResult read = link_.socket.receive(buffer_);
    if (read.status != network::ReadStatus::OPEN) {
        throw std::runtime_error("round trip: the engine closed the connection with commands unanswered");
    }
    if (read.bytes != 0) {
        link_.reader.feed(std::span<const std::byte>(buffer_).first(read.bytes));
    }
    return read.bytes;
}

bool RoundTripTarget::takeFrames(std::size_t expected) {
    bool last = false;
    for (network::FrameResult result = link_.reader.next(); std::holds_alternative<network::Frame>(result); result = link_.reader.next()) {
        const network::Frame& frame = std::get<network::Frame>(result);
        const std::optional<network::EventFrame> event = network::decodeEvent(frame);
        if (!event || frame.correlation != expected) {
            throw std::runtime_error("round trip: an answer was not an event of the command sent");
        }
        events_.push_back(event->event);
        last = last || frame.last;
    }
    return last;
}

void RoundTripTarget::countAnswer(std::size_t index) {
    benchmark::countCommand(records_[index].command, events_, counted_);
    ++answered_;
}

void RoundTripTarget::checkOutcome() {
    link_.socket.shutdownSend();
    std::array<std::byte, 1024> into{};
    while (link_.socket.receive(into).status == network::ReadStatus::OPEN) {
    }
    engine_->stop();
    if (answered_ != records_.size()) {
        throw std::runtime_error("round trip: " + std::to_string(answered_) + " of " + std::to_string(records_.size()) +
                                 " commands were sent and answered");
    }
    benchmark::requireOutcomes({benchmark::withPredictedShape(counted_, workloads_.front())}, workloads_,
                               benchmark::BenchmarkKind::ENGINE);
}

}  // namespace order_book::latency
