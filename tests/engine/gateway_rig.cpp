#include "gateway_rig.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace order_book::engine_testing {

    using namespace order_book::network;
    using namespace order_book::runtime;

    ShardActor::ShardActor(const InstrumentDirectory& directory, std::uint32_t number, std::size_t ringSlots)
        : shard(directory, number), commands(ringSlots), responses(ringSlots) {}

    Rig::Rig(std::vector<Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots, GatewaySettings settings)
        : directory(std::move(instruments), shardCount) {
        settings.clock = [this] { return now; };
        settings.spinWindow = std::chrono::nanoseconds::zero();
        std::vector<ShardLink> links;
        for (std::uint32_t number = 0; number < shardCount; ++number) {
            shards.push_back(std::make_unique<ShardActor>(directory, number, ringSlots));
            ShardActor& actor = *shards.back();
            links.push_back(ShardLink{&actor.commands, &actor.responses, &actor.doorbell});
            actor.loop.emplace(actor.shard, actor.doorbell, std::vector<RingPair>{RingPair{&actor.commands, &actor.responses}},
                               std::chrono::nanoseconds::zero(), [this] {
                                   ++waitsInPlace;
                                   gateway->pass();
                               });
        }
        gateway.emplace(directory, std::move(links), sockets, std::move(settings));
    }

    std::size_t Rig::match(std::uint32_t shard) {
        ShardActor& actor = *shards[shard];
        const std::size_t taken = actor.loop->pass();
        if (actor.loop->stopped() && !actor.reportedStop) {
            actor.reportedStop = true;
            actor.loop->run();
        }
        return taken;
    }

    void Rig::settle() {
        for (int round = 0; round < 512; ++round) {
            gateway->pass();
            for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
                match(shard);
            }
        }
    }

    Client::Client(InMemorySocketSet& socketSet) : sockets(socketSet), socket(socketSet.connect()), reader(FrameSource::ENGINE) {}

    const std::vector<Received>& Client::frames() {
        const std::vector<std::byte> bytes = sockets.clientReceive(socket);
        reader.feed(bytes);
        for (;;) {
            const FrameResult result = reader.next();
            const Frame* frame = std::get_if<Frame>(&result);
            if (frame == nullptr) {
                if (!std::holds_alternative<NeedMore>(result)) {
                    throw std::runtime_error("the engine sent a frame a client can't read");
                }
                return received;
            }
            Received item{frame->type, frame->last, frame->correlation, 0, std::nullopt, std::nullopt, std::nullopt};
            if (frame->type == FrameType::WELCOME) {
                item.version = decodeVersion(*frame);
            } else if (frame->type == FrameType::REFUSED) {
                item.refused = decodeRefused(*frame);
            } else if (frame->type == FrameType::PROTOCOL_ERROR) {
                item.error = decodeProtocolError(*frame);
            } else {
                item.event = decodeEvent(*frame);
                if (!item.event) {
                    throw std::runtime_error("the engine sent an event frame that can't be decoded");
                }
            }
            received.push_back(item);
        }
    }

    std::size_t Client::answered() {
        std::size_t count = 0;
        for (const Received& frame : frames()) {
            if (frame.last && frame.type != FrameType::WELCOME && frame.type != FrameType::PROTOCOL_ERROR) {
                ++count;
            }
        }
        return count;
    }

    bool refusedForStopping(const Received& frame) {
        return frame.refused && frame.refused->code == static_cast<std::uint16_t>(RefusedCode::ENGINE_STOPPING);
    }

}  // namespace order_book::engine_testing
