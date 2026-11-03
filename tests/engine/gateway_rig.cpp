#include "gateway_rig.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace order_book::engine_testing {

    using namespace order_book::network;
    using namespace order_book::runtime;

    namespace {
        std::vector<std::unique_ptr<RingLink>> ringLinks(std::size_t ringSlots, std::size_t gatewayThreads) {
            std::vector<std::unique_ptr<RingLink>> links;
            for (std::size_t thread = 0; thread < gatewayThreads; ++thread) {
                links.push_back(std::make_unique<RingLink>(ringSlots));
            }
            return links;
        }
    }  // namespace

    ShardActor::ShardActor(const InstrumentDirectory& directory, std::uint32_t number, std::size_t ringSlots, std::size_t gatewayThreads)
        : shard(directory, number),
          links(ringLinks(ringSlots, gatewayThreads)),
          commands(links.front()->commands),
          responses(links.front()->responses),
          logRing(ringSlots) {}

    Rig::Rig(std::vector<Instrument> instruments, std::uint32_t shardCount, std::size_t ringSlots, GatewaySettings settings,
             std::size_t gatewayThreads, std::optional<Durability> log)
        : directory(std::move(instruments), shardCount) {
        settings.clock = [this] { return now; };
        settings.spinWindow = std::chrono::nanoseconds::zero();
        std::vector<SocketSet*> threadSockets{&sockets};
        for (std::size_t thread = 1; thread < gatewayThreads; ++thread) {
            memberSockets.push_back(std::make_unique<InMemorySocketSet>(InMemorySocketSet::MemberOf{}, sockets));
            threadSockets.push_back(memberSockets.back().get());
        }
        handOff = std::make_unique<ConnectionHandOff>(std::move(threadSockets));
        for (std::uint32_t number = 0; number < shardCount; ++number) {
            shards.push_back(std::make_unique<ShardActor>(directory, number, ringSlots, gatewayThreads));
            ShardActor& actor = *shards.back();
            std::vector<RingPair> rings;
            for (const std::unique_ptr<RingLink>& link : actor.links) {
                rings.push_back(RingPair{&link->commands, &link->responses});
            }
            std::optional<LogLink> logLink;
            if (log) {
                actor.writer.emplace(logStorage, directory, number, *log, actor.logRing, actor.durable);
                actor.shard.watchDurablePosition(actor.durable);
                logLink = LogLink{&actor.logRing, &actor.writerBell};
            }
            // Only a gateway or a log writer can make room in a response ring or a log ring.
            actor.loop.emplace(
                actor.shard, actor.doorbell, std::move(rings), std::chrono::nanoseconds::zero(),
                [this] {
                    ++waitsInPlace;
                    for (const std::unique_ptr<GatewayThread>& each : gateways) {
                        each->pass();
                    }
                    for (std::uint32_t each = 0; each < shards.size(); ++each) {
                        writeLog(each);
                    }
                },
                logLink);
        }
        for (std::size_t thread = 0; thread < gatewayThreads; ++thread) {
            std::vector<ShardLink> links;
            for (const std::unique_ptr<ShardActor>& actor : shards) {
                links.push_back(ShardLink{&actor->links[thread]->commands, &actor->links[thread]->responses, &actor->doorbell,
                                          actor->writer ? &actor->durable : nullptr});
            }
            gateways.push_back(std::make_unique<GatewayThread>(directory, std::move(links), socketsOf(thread), settings, handOff.get(), thread));
        }
        gateway = gateways.front().get();
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

    std::size_t Rig::writeLog(std::uint32_t shard) {
        ShardActor& actor = *shards[shard];
        return actor.writer ? actor.writer->pass() : 0;
    }

    void Rig::settle() {
        for (int round = 0; round < 512; ++round) {
            for (const std::unique_ptr<GatewayThread>& each : gateways) {
                each->pass();
            }
            for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
                match(shard);
                writeLog(shard);
            }
        }
    }

    Client::Client(InMemorySocketSet& socketSet) : sockets(socketSet), socket(socketSet.connect()), reader(FrameSource::ENGINE) {}

    const std::vector<Received>& Client::frames() {
        return take(sockets.clientReceive(socket));
    }

    const std::vector<Received>& Client::framesWaiting() {
        return take(sockets.clientReceiveWaiting(socket));
    }

    const std::vector<Received>& Client::take(const std::vector<std::byte>& bytes) {
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
