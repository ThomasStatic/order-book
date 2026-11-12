#include "order_book/network/page_socket_set.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace order_book::network {

    namespace {
        constexpr std::size_t indexBits = 16;
        constexpr std::size_t mostSlots = std::size_t{1} << indexBits;
    }  // namespace

    PageSocketSet::PageSocketSet(std::function<void()> tellTheClient, std::size_t buffer)
        : clientHasSomething(std::move(tellTheClient)), bufferBytes(buffer) {
        if (!clientHasSomething) {
            throw std::invalid_argument("a PageSocketSet needs its client's callback");
        }
        if (bufferBytes == 0) {
            throw std::invalid_argument("a PageSocketSet's buffers can't be empty");
        }
    }

    SocketId PageSocketSet::idOf(std::size_t index, std::uint16_t generation) noexcept {
        return static_cast<SocketId>((static_cast<SocketId>(generation) << indexBits) | static_cast<SocketId>(index));
    }

    PageSocketSet::Slot& PageSocketSet::slotOf(SocketId socket) {
        return const_cast<Slot&>(std::as_const(*this).slotOf(socket));
    }

    const PageSocketSet::Slot& PageSocketSet::slotOf(SocketId socket) const {
        const std::size_t index = socket & (mostSlots - 1);
        const auto generation = static_cast<std::uint16_t>(socket >> indexBits);
        if (index >= slotList.size() || !slotList[index].inUse || slotList[index].generation != generation) {
            throw std::logic_error("connection " + std::to_string(socket) + " is not one this PageSocketSet holds");
        }
        return slotList[index];
    }

    // Once both sides are done, the slot is free for the next connection, under a new generation.
    void PageSocketSet::freeIfDone(Slot& slot) {
        if (slot.gatewayClosed && slot.clientReleased) {
            const auto generation = static_cast<std::uint16_t>(slot.generation + 1);
            slot = Slot{};
            slot.generation = generation;
        }
    }

    std::optional<SocketId> PageSocketSet::clientConnect() {
        const std::lock_guard<std::mutex> held(lock);
        if (listenerClosed) {
            return std::nullopt;
        }
        auto free = std::find_if(slotList.begin(), slotList.end(), [](const Slot& slot) { return !slot.inUse; });
        if (free == slotList.end()) {
            if (slotList.size() == mostSlots) {
                throw std::length_error("a PageSocketSet holds at most 65536 connections at once");
            }
            slotList.emplace_back();
            free = std::prev(slotList.end());
        }
        free->inUse = true;
        const SocketId socket = idOf(static_cast<std::size_t>(free - slotList.begin()), free->generation);
        waitingToBeAccepted.push_back(socket);
        changed.notify_all();
        return socket;
    }

    std::size_t PageSocketSet::clientSend(SocketId socket, std::span<const std::byte> bytes) {
        const std::lock_guard<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        if (slot.clientHalfClosed || slot.clientReset || slot.gatewayClosed) {
            return 0;
        }
        const std::size_t taken = std::min(bytes.size(), bufferBytes - slot.toEngine.size());
        slot.toEngine.insert(slot.toEngine.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(taken));
        if (taken != 0) {
            changed.notify_all();
        }
        return taken;
    }

    std::vector<std::byte> PageSocketSet::clientReceive(SocketId socket, std::size_t most) {
        const std::lock_guard<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        const std::size_t taken = std::min(most, slot.toClient.size());
        std::vector<std::byte> bytes(slot.toClient.begin(), slot.toClient.begin() + static_cast<std::ptrdiff_t>(taken));
        slot.toClient.erase(slot.toClient.begin(), slot.toClient.begin() + static_cast<std::ptrdiff_t>(taken));
        if (taken != 0) {
            changed.notify_all();  // room for the gateway's next write
        }
        return bytes;
    }

    PageSocketSet::ClientEnd PageSocketSet::clientEnd(SocketId socket) const {
        const std::lock_guard<std::mutex> held(lock);
        const Slot& slot = slotOf(socket);
        if (slot.resetByGateway || slot.clientReset) {
            return ClientEnd::RESET;
        }
        return slot.sendShutDown && slot.toClient.empty() ? ClientEnd::ENDED : ClientEnd::OPEN;
    }

    void PageSocketSet::clientHalfClose(SocketId socket) {
        const std::lock_guard<std::mutex> held(lock);
        slotOf(socket).clientHalfClosed = true;
        changed.notify_all();
    }

    void PageSocketSet::clientReset(SocketId socket) {
        const std::lock_guard<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        slot.clientReset = true;
        slot.toEngine.clear();
        slot.toClient.clear();
        changed.notify_all();
    }

    void PageSocketSet::clientRelease(SocketId socket) {
        const std::lock_guard<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        slot.clientReleased = true;
        freeIfDone(slot);
    }

    // Ready to read once there are bytes or the client ended its side; to write once there is
    // room or the client reset. A connection the client reset is ready either way, so the gateway
    // finds it gone.
    bool PageSocketSet::ready(std::span<const SocketInterest> interest) const {
        for (const SocketInterest& wanted : interest) {
            const Slot& slot = slotOf(wanted.socket);
            if (slot.clientReset) {
                return true;
            }
            if (wanted.read && (!slot.toEngine.empty() || slot.clientHalfClosed)) {
                return true;
            }
            if (wanted.write && slot.toClient.size() < bufferBytes) {
                return true;
            }
        }
        return false;
    }

    void PageSocketSet::wait(std::span<const SocketInterest> interest, WaitMode mode) {
        std::unique_lock<std::mutex> held(lock);
        if (mode == WaitMode::SLEEP) {
            changed.wait(held, [&] { return woken || !waitingToBeAccepted.empty() || ready(interest); });
        }
        woken = false;
    }

    std::optional<SocketId> PageSocketSet::accept() {
        const std::lock_guard<std::mutex> held(lock);
        if (waitingToBeAccepted.empty()) {
            return std::nullopt;
        }
        const SocketId socket = waitingToBeAccepted.front();
        waitingToBeAccepted.pop_front();
        return socket;
    }

    ReadResult PageSocketSet::read(SocketId socket, std::span<std::byte> into) {
        std::unique_lock<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        if (slot.clientReset) {
            return ReadResult{0, ReadStatus::FAILED};
        }
        const std::size_t taken = std::min(into.size(), slot.toEngine.size());
        std::copy_n(slot.toEngine.begin(), taken, into.begin());
        slot.toEngine.erase(slot.toEngine.begin(), slot.toEngine.begin() + static_cast<std::ptrdiff_t>(taken));
        const ReadStatus status = taken == 0 && slot.clientHalfClosed ? ReadStatus::ENDED : ReadStatus::OPEN;
        held.unlock();
        if (taken != 0) {
            clientHasSomething();  // room freed
        }
        return ReadResult{taken, status};
    }

    std::optional<std::size_t> PageSocketSet::write(SocketId socket, std::span<const std::byte> bytes) {
        std::unique_lock<std::mutex> held(lock);
        Slot& slot = slotOf(socket);
        if (slot.clientReset) {
            return std::nullopt;
        }
        const std::size_t taken = std::min(bytes.size(), bufferBytes - slot.toClient.size());
        slot.toClient.insert(slot.toClient.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(taken));
        held.unlock();
        if (taken != 0) {
            clientHasSomething();
        }
        return taken;
    }

    void PageSocketSet::shutdownSend(SocketId socket) {
        {
            const std::lock_guard<std::mutex> held(lock);
            slotOf(socket).sendShutDown = true;
        }
        clientHasSomething();
    }

    // After shutdownSend, a close lets the client take what is still waiting, then end; without
    // it, the connection resets and the waiting bytes are lost, as a socket closed with data
    // unsent is.
    void PageSocketSet::close(SocketId socket) {
        {
            const std::lock_guard<std::mutex> held(lock);
            Slot& slot = slotOf(socket);
            slot.gatewayClosed = true;
            slot.toEngine.clear();
            if (!slot.sendShutDown) {
                slot.resetByGateway = true;
                slot.toClient.clear();
            }
            freeIfDone(slot);
        }
        clientHasSomething();
    }

    // As closing a listener resets the connections waiting on it.
    void PageSocketSet::closeListener() {
        {
            const std::lock_guard<std::mutex> held(lock);
            listenerClosed = true;
            for (const SocketId socket : waitingToBeAccepted) {
                Slot& slot = slotOf(socket);
                slot.gatewayClosed = true;
                slot.resetByGateway = true;
                freeIfDone(slot);
            }
            waitingToBeAccepted.clear();
        }
        clientHasSomething();
    }

    void PageSocketSet::wake() noexcept {
        const std::lock_guard<std::mutex> held(lock);
        woken = true;
        changed.notify_all();
    }

    HandedSocket PageSocketSet::release(SocketId) {
        throw std::logic_error("a PageSocketSet serves one gateway thread, so no connection is handed on");
    }

    SocketId PageSocketSet::adopt(HandedSocket) {
        throw std::logic_error("a PageSocketSet serves one gateway thread, so no connection is handed on");
    }

    std::size_t PageSocketSet::slots() const {
        const std::lock_guard<std::mutex> held(lock);
        return slotList.size();
    }

}  // namespace order_book::network
