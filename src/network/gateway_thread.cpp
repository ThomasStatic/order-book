#include "order_book/network/gateway_thread.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace order_book::network {

    namespace {
        constexpr std::size_t readBufferBytes = 64 * 1024;
        constexpr std::size_t largestFrameBytes = 64;

        void appendFrame(std::vector<std::byte>& unsent, std::span<const std::byte> frame) {
            unsent.insert(unsent.end(), frame.begin(), frame.end());
        }
    }  // namespace

    GatewayThread::GatewayThread(const runtime::InstrumentDirectory& instruments, std::vector<ShardLink> links, SocketSet& socketSet)
        : directory(instruments),
          shards(std::move(links)),
          sockets(socketSet),
          connections(connectionsPerThread),
          parkedOnShard(shards.size(), 0),
          shardBlocked(shards.size(), 0),
          shardRung(shards.size(), 0),
          readBuffer(readBufferBytes) {
        parkedOldestFirst.reserve(connectionsPerThread);
    }

    bool GatewayThread::mayWait() const noexcept {
        if (inFlight != 0 || !parkedOldestFirst.empty()) {
            return false;
        }
        return std::none_of(connections.begin(), connections.end(), [](const Connection& connection) {
            return connection.open && (connection.framesWaiting || !connection.unsent.empty());
        });
    }

    void GatewayThread::pass() {
        if (mayWait()) {
            sockets.wait();
        }
        acceptConnections();
        drainResponses();
        retryParked();
        for (std::uint32_t index = 0; index < connections.size(); ++index) {
            if (connections[index].open && connections[index].reading && !connections[index].parked) {
                readConnection(index);
            }
        }
        ringDoorbells();
        sendResponses();
    }

    void GatewayThread::acceptConnections() {
        for (std::uint32_t index = 0; index < connections.size(); ++index) {
            Connection& connection = connections[index];
            if (connection.open) {
                continue;
            }
            const std::optional<SocketId> socket = sockets.accept();
            if (!socket) {
                return;
            }
            connection.reader = FrameReader(FrameSource::CLIENT);
            connection.unsent.clear();
            connection.socket = *socket;
            connection.open = true;
            connection.reading = true;
            connection.framesWaiting = false;
        }
    }

    void GatewayThread::closeConnection(std::uint32_t index) {
        Connection& connection = connections[index];
        if (connection.parked) {
            --parkedOnShard[connection.parked->shard];
            connection.parked.reset();
            parkedOldestFirst.erase(std::find(parkedOldestFirst.begin(), parkedOldestFirst.end(), index));
        }
        sockets.close(connection.socket);
        connection.open = false;
        ++connection.generation;
    }

    void GatewayThread::drainResponses() {
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            runtime::Ring<runtime::ResponseRecord>& responses = *shards[shard].responses;
            responses.drain(responses.capacity(), [this, shard](const runtime::ResponseRecord& record) {
                if (record.kind == runtime::ResponseKind::SHARD_STOPPED) {
                    return;
                }
                const bool last = record.kind == runtime::ResponseKind::LAST_EVENT;
                if (last) {
                    --inFlight;
                }
                const std::uint32_t index = connectionIndex(record.connection);
                Connection& connection = connections[index];
                if (!connection.open || handleOf(index) != record.connection) {
                    return;
                }
                const Symbol symbol = directory.onShard(shard)[record.slot].symbol;
                std::array<std::byte, largestFrameBytes> frame{};
                const std::size_t size = encodeEvent(record.correlation, symbol, record.event, last, frame);
                appendFrame(connection.unsent, std::span<const std::byte>(frame).first(size));
            });
        }
    }

    void GatewayThread::retryParked() {
        std::fill(shardBlocked.begin(), shardBlocked.end(), char{0});
        std::size_t stillParked = 0;
        for (const std::uint32_t index : parkedOldestFirst) {
            Connection& connection = connections[index];
            const std::uint32_t shard = connection.parked->shard;
            if (shardBlocked[shard] != 0 || !tryQueue(shard, connection.parked->element)) {
                shardBlocked[shard] = 1;  // so nobody younger on this shard passes it
                parkedOldestFirst[stillParked++] = index;
            } else {
                --parkedOnShard[shard];
                connection.parked.reset();
            }
        }
        parkedOldestFirst.resize(stillParked);
    }

    void GatewayThread::readConnection(std::uint32_t index) {
        Connection& connection = connections[index];
        connection.framesWaiting = false;
        bool socketRead = false;
        std::size_t frames = 0;
        while (frames < framesPerPass) {
            const FrameResult result = connection.reader.next();
            if (const Frame* frame = std::get_if<Frame>(&result)) {
                ++frames;
                takeFrame(index, *frame);
                if (connection.parked) {
                    return;
                }
            } else if (std::holds_alternative<NeedMore>(result)) {
                if (socketRead) {
                    return;
                }
                socketRead = true;
                const ReadResult arrived = sockets.read(connection.socket, readBuffer);
                if (arrived.status != ReadStatus::OPEN) {
                    closeConnection(index);
                    return;
                }
                if (arrived.bytes == 0) {
                    return;
                }
                connection.reader.feed(std::span<const std::byte>(readBuffer).first(arrived.bytes));
            } else {
                connection.reading = false;  // what follows a protocol error comes with the failure paths
                return;
            }
        }
        connection.framesWaiting = true;
    }

    void GatewayThread::takeFrame(std::uint32_t index, const Frame& frame) {
        if (frame.type == FrameType::HELLO) {
            std::array<std::byte, largestFrameBytes> welcome{};
            const std::size_t size = encodeWelcome(frame.correlation, decodeVersion(frame), welcome);
            appendFrame(connections[index].unsent, std::span<const std::byte>(welcome).first(size));
        } else {
            takeCommand(index, frame);
        }
    }

    void GatewayThread::takeCommand(std::uint32_t index, const Frame& frame) {
        const CommandFrame command = decodeCommandFrame(frame);
        const EntryDecision decision = checkEntry(command, directory, false);
        std::array<std::byte, largestFrameBytes> answer{};
        std::size_t size = 0;
        if (const EntryAccepted* accepted = std::get_if<EntryAccepted>(&decision)) {
            queue(index, *accepted, frame.correlation);
        } else if (const EntryRefused* refused = std::get_if<EntryRefused>(&decision)) {
            size = encodeRefused(frame.correlation, command.command.symbol, command.command.orderId, refused->code, answer);
        } else {
            const EntryMalformed& malformed = std::get<EntryMalformed>(decision);
            size = encodeEvent(frame.correlation, malformed.symbol, malformed.event, true, answer);
        }
        appendFrame(connections[index].unsent, std::span<const std::byte>(answer).first(size));
    }

    void GatewayThread::queue(std::uint32_t index, const EntryAccepted& accepted, Correlation correlation) {
        const std::uint32_t shard = accepted.location.shard;
        const runtime::CommandElement element{correlation, accepted.command, accepted.location.slot, handleOf(index),
                                              runtime::ControlKind::COMMAND};
        if (parkedOnShard[shard] == 0 && tryQueue(shard, element)) {
            return;
        }
        // TCP carries the backpressure: nothing is refused, and this connection isn't read until
        // the command goes, since a later command of its stream may not pass this one.
        connections[index].parked = Parked{shard, element};
        ++parkedOnShard[shard];
        parkedOldestFirst.push_back(index);
    }

    bool GatewayThread::tryQueue(std::uint32_t shard, const runtime::CommandElement& element) {
        if (!shards[shard].commands->tryPush(element)) {
            return false;
        }
        ++inFlight;
        shardRung[shard] = 1;
        return true;
    }

    void GatewayThread::ringDoorbells() {
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            if (shardRung[shard] != 0) {
                shardRung[shard] = 0;
                shards[shard].doorbell->ring();
            }
        }
    }

    void GatewayThread::sendResponses() {
        for (Connection& connection : connections) {
            if (connection.open && !connection.unsent.empty()) {
                const std::size_t written = sockets.write(connection.socket, connection.unsent);
                connection.unsent.erase(connection.unsent.begin(), connection.unsent.begin() + static_cast<std::ptrdiff_t>(written));
            }
        }
    }

}  // namespace order_book::network
