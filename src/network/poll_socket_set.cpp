#include "order_book/network/poll_socket_set.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace order_book::network {

    PollSocketSet::PollSocketSet(std::uint16_t port, Clock lingerClock) : PollSocketSet({127, 0, 0, 1}, port, std::move(lingerClock)) {
        startListening();
    }

    PollSocketSet::PollSocketSet(const std::array<std::uint8_t, 4>& host, std::uint16_t port, Clock lingerClock)
        : listener(TcpSocket::bindIpv4(host, port)), clock(std::move(lingerClock)) {
        listeningPort = listener.localPort();

        // The wake pair: a connection to a listener of its own, made before anything can race it.
        const TcpSocket wakeListener = TcpSocket::listenLoopback(0);
        wakeSender = TcpSocket::connectLoopback(wakeListener.localPort());
        std::optional<TcpSocket> accepted;
        for (int attempt = 0; attempt < 1000 && !accepted; ++attempt) {
            accepted = wakeListener.accept();
            if (!accepted) {
                Poller once;
                once.add(wakeListener.handle(), true, false);
                once.wait(10);
            }
        }
        if (!accepted) {
            throw std::runtime_error("the gateway's wake socket could not connect to itself");
        }
        wakeReceiver = std::move(*accepted);
    }

    void PollSocketSet::startListening() {
        listener.listen();
        listening = true;
    }

    void PollSocketSet::wake() noexcept {
        if (!wakePending.exchange(true, std::memory_order_seq_cst)) {
            const std::array<std::byte, 1> ring{std::byte{1}};
            static_cast<void>(wakeSender.send(ring));
        }
    }

    bool PollSocketSet::noDelay(SocketId socket) const {
        return slots.at(socket)->socket.noDelay();
    }

    void PollSocketSet::wait(std::span<const SocketInterest> interest, WaitMode mode) {
        poller.clear();
        poller.add(wakeReceiver.handle(), true, false);
        const std::size_t first = listening ? 2 : 1;
        if (listening) {
            poller.add(listener.handle(), true, false);
        }
        for (const SocketInterest& wanted : interest) {
            poller.add(slots.at(wanted.socket)->socket.handle(), wanted.read, wanted.write);
        }
        for (const SocketId socket : lingering) {
            poller.add(slots[socket]->socket.handle(), true, false);
        }
        int timeout = mode == WaitMode::SLEEP ? -1 : 0;
        if (mode == WaitMode::SLEEP && !lingering.empty()) {  // so a silent lingerer still gets closed
            const auto soonest = std::min_element(lingering.begin(), lingering.end(), [this](SocketId left, SocketId right) {
                return slots[left]->lingerEnds < slots[right]->lingerEnds;
            });
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(slots[*soonest]->lingerEnds - clock());
            timeout = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count() + 1, 0, 60'000));
        }
        poller.wait(timeout);

        if (poller.readable(0)) {
            // Drained before the flag clears. A wake whose exchange comes after this one's sends a
            // byte of its own. One whose exchange came before it saw true and sent none, but its
            // thread changed what it woke this for first, and in the single order of these
            // exchanges and that thread's change, this thread's next look comes after both.
            std::array<std::byte, 64> sink;
            while (wakeReceiver.receive(sink).bytes != 0) {
            }
            wakePending.exchange(false, std::memory_order_seq_cst);
        }
        listenerReadable = listening && poller.readable(1);
        for (std::size_t i = 0; i < interest.size(); ++i) {
            if (poller.readable(first + i)) {
                slots[interest[i].socket]->readable = true;
            }
        }
        for (std::size_t i = 0; i < lingering.size(); ++i) {
            if (poller.readable(first + interest.size() + i)) {
                slots[lingering[i]]->readable = true;
            }
        }
        linger();
    }

    void PollSocketSet::linger() {
        const std::chrono::steady_clock::time_point now = clock();
        std::size_t kept = 0;
        for (const SocketId socket : lingering) {
            Slot& slot = *slots[socket];
            bool done = now >= slot.lingerEnds;
            std::array<std::byte, 16 * 1024> sink;
            while (!done && slot.readable) {
                const ReadResult read = slot.socket.receive(sink);
                slot.discarded += read.bytes;
                if (read.status != ReadStatus::OPEN || slot.discarded > lingerBytes) {
                    done = true;
                } else if (read.bytes == 0) {
                    slot.readable = false;
                }
            }
            if (done) {
                release(socket);
            } else {
                lingering[kept++] = socket;
            }
        }
        lingering.resize(kept);
    }

    void PollSocketSet::release(SocketId socket) {
        slots[socket].reset();
        freeSlots.push_back(socket);
    }

    std::optional<SocketId> PollSocketSet::accept() {
        if (!listening || !listenerReadable) {
            return std::nullopt;
        }
        std::optional<TcpSocket> accepted = listener.accept();
        if (!accepted) {
            listenerReadable = false;
            return std::nullopt;
        }
        Slot slot;
        slot.socket = std::move(*accepted);
        slot.readable = true;  // it may have sent already, and a read will say
        SocketId id;
        if (freeSlots.empty()) {
            id = static_cast<SocketId>(slots.size());
            slots.emplace_back(std::move(slot));
        } else {
            id = freeSlots.back();
            freeSlots.pop_back();
            slots[id].emplace(std::move(slot));
        }
        return id;
    }

    ReadResult PollSocketSet::read(SocketId socket, std::span<std::byte> into) {
        Slot& slot = *slots.at(socket);
        if (!slot.readable) {
            return ReadResult{0, ReadStatus::OPEN};
        }
        const ReadResult result = slot.socket.receive(into);
        if (result.status == ReadStatus::OPEN && result.bytes < into.size()) {
            slot.readable = false;  // most likely drained, and the next wait will say if not
        }
        return result;
    }

    std::optional<std::size_t> PollSocketSet::write(SocketId socket, std::span<const std::byte> bytes) {
        return slots.at(socket)->socket.send(bytes);
    }

    void PollSocketSet::shutdownSend(SocketId socket) {
        Slot& slot = *slots.at(socket);
        slot.socket.shutdownSend();
        slot.shutDown = true;
    }

    void PollSocketSet::close(SocketId socket) {
        Slot& slot = *slots.at(socket);
        if (!slot.shutDown) {
            release(socket);
            return;
        }
        slot.readable = true;  // whatever is already waiting goes at the next wait
        slot.lingerEnds = clock() + lingerTime;
        lingering.push_back(socket);
    }

    void PollSocketSet::closeListener() {
        listener.close();
        listening = false;
        listenerReadable = false;
    }

}  // namespace order_book::network
