#include "order_book/network/gateway_thread.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace order_book::network {

    namespace {
        constexpr std::size_t readBufferBytes = 64 * 1024;
        constexpr std::size_t largestFrameBytes = 64;

        // Encodes one frame, through encode(out) returning its size, onto the end of unsent.
        template <typename Encode>
        void appendFrame(std::vector<std::byte>& unsent, Encode encode) {
            std::array<std::byte, largestFrameBytes> frame{};
            const std::size_t size = encode(std::span<std::byte>(frame));
            unsent.insert(unsent.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
        }
        std::size_t checkedConnections(std::size_t wanted) {
            if (wanted == 0 || wanted > mostConnectionsPerThread) {
                throw std::invalid_argument("a gateway thread holds from 1 to " + std::to_string(mostConnectionsPerThread) +
                                            " connections, not " + std::to_string(wanted));
            }
            return wanted;
        }
    }  // namespace

    GatewayThread::GatewayThread(const runtime::InstrumentDirectory& instruments, std::vector<ShardLink> links, SocketSet& socketSet,
                                 GatewaySettings gatewaySettings)
        : directory(instruments),
          shards(std::move(links)),
          sockets(socketSet),
          settings(std::move(gatewaySettings)),
          connections(checkedConnections(settings.maxConnections)),
          shardStates(shards.size()),
          readBuffer(readBufferBytes) {
        parkedOldestFirst.reserve(connections.size());
        interest.reserve(connections.size());
    }

    bool GatewayThread::readable(const Connection& connection) const noexcept {
        return connection.open && connection.reading && !connection.parked && connection.unsent.size() <= settings.highWaterMark;
    }

    bool GatewayThread::framesInAReader() const noexcept {
        return std::any_of(connections.begin(), connections.end(),
                           [this](const Connection& connection) { return readable(connection) && connection.framesWaiting; });
    }

    bool GatewayThread::mayWait() const noexcept {
        return !stopBegun && inFlight == 0 && parkedOldestFirst.empty() && !framesInAReader();
    }

    WaitMode GatewayThread::waitMode() {
        if (!mayWait()) {
            idle = false;
            return WaitMode::LOOK;
        }
        if (settings.spinWindow == std::chrono::nanoseconds::zero()) {
            return WaitMode::SLEEP;
        }
        if (settings.spinWindow == runtime::unlimitedSpin) {
            return WaitMode::LOOK;
        }
        const std::chrono::steady_clock::time_point now = settings.clock();
        if (!idle) {
            idle = true;
            idleSince = now;
        }
        return now - idleSince >= settings.spinWindow ? WaitMode::SLEEP : WaitMode::LOOK;
    }

    void GatewayThread::listInterest() {
        interest.clear();
        for (const Connection& connection : connections) {
            if (!connection.open) {
                continue;
            }
            const bool read = readable(connection);
            const bool write = !connection.unsent.empty();
            if (read || write) {
                interest.push_back(SocketInterest{connection.socket, read, write});
            }
        }
    }

    bool GatewayThread::pass() {
        moved = false;
        // A pass skips the wait while a reader holds frames, so a lone streaming client doesn't
        // pay one per framesPerPass of them, but not for ever, or nothing else would be looked at.
        if (framesInAReader() && waitsSkipped < mostWaitsSkipped) {
            ++waitsSkipped;
        } else {
            waitsSkipped = 0;
            const WaitMode mode = waitMode();
            listInterest();
            sockets.wait(interest, mode);
            if (mode == WaitMode::SLEEP) {
                idle = false;
            }
        }
        acceptConnections();
        drainResponses();
        if (!flushing && shardsStopped == shards.size()) {
            beginFlush();
        }
        retryParked();
        for (std::uint32_t index = 0; index < connections.size(); ++index) {
            if (readable(connections[index])) {
                readConnection(index);
            }
        }
        sendStopMarkers();
        ringDoorbells();
        sendResponses();
        if (flushing && settings.clock() >= graceEnds) {
            for (std::uint32_t index = 0; index < connections.size(); ++index) {
                if (connections[index].open) {
                    closeConnection(index);
                }
            }
        }
        return moved;
    }

    void runGateway(GatewayThread& gateway, const std::atomic<bool>& stopRequested) {
        while (!gateway.finished()) {
            if (stopRequested.load(std::memory_order_seq_cst)) {
                gateway.beginStop();
            }
            if (!gateway.pass()) {
                std::this_thread::yield();
            }
        }
    }

    void GatewayThread::beginStop() {
        if (stopBegun) {
            return;
        }
        stopBegun = true;
        sockets.closeListener();
        refuseParked();
        for (ShardState& state : shardStates) {
            state.markerOwed = true;
        }
    }

    bool GatewayThread::finished() const noexcept {
        return flushing && std::none_of(connections.begin(), connections.end(), [](const Connection& connection) { return connection.open; });
    }

    void GatewayThread::acceptConnections() {
        for (;;) {
            const std::optional<SocketId> socket = sockets.accept();
            if (!socket) {
                return;
            }
            const auto vacant = std::find_if(connections.begin(), connections.end(), [](const Connection& held) { return !held.open; });
            if (vacant == connections.end()) {
                refuseConnection(*socket);
                moved = true;
                continue;
            }
            vacant->reader = FrameReader(FrameSource::CLIENT);
            vacant->unsent.clear();
            vacant->error.reset();
            vacant->inFlight = 0;
            vacant->socket = *socket;
            vacant->open = true;
            vacant->reading = true;
            vacant->framesWaiting = false;
            vacant->finishing = false;
            moved = true;
        }
    }

    // Only a client that was never taken on hears this, so nothing is owed but this one frame.
    void GatewayThread::refuseConnection(SocketId socket) {
        std::vector<std::byte> frame;
        appendFrame(frame, [](std::span<std::byte> out) { return encodeProtocolError(0, ProtocolError::TOO_MANY_CONNECTIONS, out); });
        static_cast<void>(sockets.write(socket, frame));
        sockets.shutdownSend(socket);
        sockets.close(socket);
    }

    void GatewayThread::closeConnection(std::uint32_t index) {
        Connection& connection = connections[index];
        if (connection.parked) {
            --shardStates[connection.parked->shard].parkedOn;
            connection.parked.reset();
            parkedOldestFirst.erase(std::find(parkedOldestFirst.begin(), parkedOldestFirst.end(), index));
        }
        sockets.close(connection.socket);
        connection.open = false;
        ++connection.generation;
        moved = true;
    }

    void GatewayThread::drainResponses() {
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            runtime::Ring<runtime::ResponseRecord>& responses = *shards[shard].responses;
            responses.drain(responses.capacity(), [this, shard](const runtime::ResponseRecord& record) {
                moved = true;
                if (record.kind == runtime::ResponseKind::SHARD_STOPPED) {
                    ++shardsStopped;
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
                if (last) {
                    --connection.inFlight;
                }
                const Symbol symbol = directory.onShard(shard)[record.slot].symbol;
                appendFrame(connection.unsent,
                            [&](std::span<std::byte> out) { return encodeEvent(record.correlation, symbol, record.event, last, out); });
            });
        }
    }

    void GatewayThread::retryParked() {
        for (ShardState& state : shardStates) {
            state.retryFailed = false;
        }
        std::size_t stillParked = 0;
        for (const std::uint32_t index : parkedOldestFirst) {
            Connection& connection = connections[index];
            const std::uint32_t shard = connection.parked->shard;
            if (shardStates[shard].retryFailed || !tryQueue(index, shard, connection.parked->element)) {
                shardStates[shard].retryFailed = true;  // so nobody younger on this shard passes it
                parkedOldestFirst[stillParked++] = index;
            } else {
                --shardStates[shard].parkedOn;
                connection.parked.reset();
            }
        }
        parkedOldestFirst.resize(stillParked);
    }

    void GatewayThread::refuseParked() {
        for (const std::uint32_t index : parkedOldestFirst) {
            Connection& connection = connections[index];
            const Parked& parked = *connection.parked;
            const Symbol symbol = directory.onShard(parked.shard)[parked.element.slot].symbol;
            const OrderId orderId = std::visit([](const auto& command) { return command.orderId; }, parked.element.command);
            appendFrame(connection.unsent, [&](std::span<std::byte> out) {
                return encodeRefused(parked.element.correlation, symbol.bytes(), orderId, RefusedCode::ENGINE_STOPPING, out);
            });
            connection.parked.reset();
        }
        parkedOldestFirst.clear();
        for (ShardState& state : shardStates) {
            state.parkedOn = 0;
        }
    }

    void GatewayThread::sendStopMarkers() {
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            ShardState& state = shardStates[shard];
            if (state.markerOwed && shards[shard].commands->tryPush(runtime::CommandElement::stopMarker())) {
                state.markerOwed = false;
                state.pushedTo = true;
                moved = true;
            }
        }
    }

    void GatewayThread::beginFlush() {
        flushing = true;
        graceEnds = settings.clock() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(settings.stopGrace);
        for (Connection& connection : connections) {
            connection.reading = false;
            connection.finishing = true;
        }
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
                moved = true;
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
                if (arrived.status == ReadStatus::FAILED) {
                    closeConnection(index);
                    return;
                }
                if (arrived.status == ReadStatus::ENDED) {
                    connection.reading = false;
                    connection.finishing = true;
                    return;
                }
                if (arrived.bytes == 0) {
                    return;
                }
                connection.reader.feed(std::span<const std::byte>(readBuffer).first(arrived.bytes));
            } else {
                connection.reading = false;
                connection.finishing = true;
                connection.error = std::get<FrameError>(result);
                return;
            }
        }
        connection.framesWaiting = true;
    }

    void GatewayThread::takeFrame(std::uint32_t index, const Frame& frame) {
        if (frame.type == FrameType::HELLO) {
            appendFrame(connections[index].unsent,
                        [&](std::span<std::byte> out) { return encodeWelcome(frame.correlation, decodeVersion(frame), out); });
        } else {
            takeCommand(index, frame);
        }
    }

    void GatewayThread::takeCommand(std::uint32_t index, const Frame& frame) {
        const CommandFrame command = decodeCommandFrame(frame);
        const EntryDecision decision = checkEntry(command, directory, stopBegun);
        std::vector<std::byte>& unsent = connections[index].unsent;
        if (const EntryAccepted* accepted = std::get_if<EntryAccepted>(&decision)) {
            queue(index, *accepted, frame.correlation);
        } else if (const EntryRefused* refused = std::get_if<EntryRefused>(&decision)) {
            appendFrame(unsent, [&](std::span<std::byte> out) {
                return encodeRefused(frame.correlation, command.command.symbol, command.command.orderId, refused->code, out);
            });
        } else {
            const EntryMalformed& malformed = std::get<EntryMalformed>(decision);
            appendFrame(unsent, [&](std::span<std::byte> out) { return encodeEvent(frame.correlation, malformed.symbol, malformed.event, true, out); });
        }
    }

    void GatewayThread::queue(std::uint32_t index, const EntryAccepted& accepted, Correlation correlation) {
        const std::uint32_t shard = accepted.location.shard;
        const runtime::CommandElement element{correlation, accepted.command, accepted.location.slot, handleOf(index),
                                              runtime::ControlKind::COMMAND};
        if (shardStates[shard].parkedOn == 0 && tryQueue(index, shard, element)) {
            return;
        }
        // TCP carries the backpressure: nothing is refused, and this connection isn't read until
        // the command goes, since a later command of its stream may not pass this one.
        connections[index].parked = Parked{shard, element};
        ++shardStates[shard].parkedOn;
        parkedOldestFirst.push_back(index);
    }

    bool GatewayThread::tryQueue(std::uint32_t index, std::uint32_t shard, const runtime::CommandElement& element) {
        if (!shards[shard].commands->tryPush(element)) {
            return false;
        }
        ++inFlight;
        ++connections[index].inFlight;
        moved = true;
        shardStates[shard].pushedTo = true;
        return true;
    }

    void GatewayThread::ringDoorbells() {
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            if (shardStates[shard].pushedTo) {
                shardStates[shard].pushedTo = false;
                shards[shard].doorbell->ring();
            }
        }
    }

    void GatewayThread::sendResponses() {
        for (std::uint32_t index = 0; index < connections.size(); ++index) {
            if (connections[index].open) {
                sendTo(index);
            }
        }
    }

    void GatewayThread::sendTo(std::uint32_t index) {
        Connection& connection = connections[index];
        const bool answeredEverything = connection.finishing && connection.inFlight == 0;
        if (answeredEverything && connection.error) {
            const FrameError error = *connection.error;
            appendFrame(connection.unsent, [&error](std::span<std::byte> out) { return encodeProtocolError(error.correlation, error.code, out); });
            connection.error.reset();
        }
        if (!connection.unsent.empty()) {
            const std::optional<std::size_t> written = sockets.write(connection.socket, connection.unsent);
            if (!written) {
                closeConnection(index);
                return;
            }
            connection.unsent.erase(connection.unsent.begin(), connection.unsent.begin() + static_cast<std::ptrdiff_t>(*written));
            moved = moved || *written != 0;
        }
        if (connection.unsent.size() > settings.hardLimit) {
            closeConnection(index);
        } else if (answeredEverything && connection.unsent.empty()) {
            sockets.shutdownSend(connection.socket);
            closeConnection(index);
        }
    }

}  // namespace order_book::network
