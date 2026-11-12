#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "order_book/network/page_socket_set.hpp"

// PageSocketSet's two sides, called as the gateway and the engine worker call them. See
// docs/internals/network.md.
namespace {

using namespace order_book::network;
using ClientEnd = PageSocketSet::ClientEnd;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::byte> bytesOf(std::size_t count, std::size_t first = 0) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>((first + i) % 251);
    }
    return bytes;
}

std::vector<std::byte> readAll(PageSocketSet& set, SocketId socket, std::size_t most) {
    std::vector<std::byte> into(most);
    const ReadResult result = set.read(socket, into);
    into.resize(result.bytes);
    return into;
}

// A set whose callback only counts, and the accepted connection of one client.
struct Pair {
    explicit Pair(std::size_t buffer = 16) : set([this] { ++told; }, buffer) {
        client = *set.clientConnect();
        gateway = *set.accept();
    }
    std::atomic<int> told{0};
    PageSocketSet set;
    SocketId client = 0;
    SocketId gateway = 0;
};

void testOrderedBytesEachWayAndTrimming() {
    Pair pair;
    expect(pair.client == pair.gateway, "the gateway accepts the connection the client made");
    expect(pair.set.clientSend(pair.client, bytesOf(5)) == 5, "the client sends five bytes");
    expect(pair.set.clientSend(pair.client, bytesOf(3, 5)) == 3, "then three more");
    expect(readAll(pair.set, pair.gateway, 2) == bytesOf(2), "the gateway reads the first two");
    expect(readAll(pair.set, pair.gateway, 100) == bytesOf(6, 2), "then the rest, in order, nothing read twice");
    const ReadResult nothing = pair.set.read(pair.gateway, std::span<std::byte>());
    expect(nothing.bytes == 0 && nothing.status == ReadStatus::OPEN, "and then nothing, still open");

    expect(pair.set.write(pair.gateway, bytesOf(4, 10)) == std::optional<std::size_t>(4), "the gateway writes four");
    expect(pair.set.clientReceive(pair.client, 1) == bytesOf(1, 10), "the client takes one");
    expect(pair.set.clientReceive(pair.client) == bytesOf(3, 11), "then the rest, in order");
    expect(pair.set.clientReceive(pair.client).empty(), "then nothing");
    expect(pair.set.clientEnd(pair.client) == ClientEnd::OPEN, "and the connection is open");
}

void testBuffersAreBoundedBothWays() {
    Pair pair(16);
    expect(pair.set.clientSend(pair.client, bytesOf(20)) == 16, "a client send takes only what fits");
    expect(pair.set.clientSend(pair.client, bytesOf(1)) == 0, "and nothing more while the buffer is full");
    expect(readAll(pair.set, pair.gateway, 10).size() == 10, "the gateway reads ten");
    expect(pair.set.clientSend(pair.client, bytesOf(20, 16)) == 10, "which makes room for ten");
    expect(readAll(pair.set, pair.gateway, 100) == [] {
        std::vector<std::byte> all = bytesOf(6, 10);
        const std::vector<std::byte> more = bytesOf(10, 16);
        all.insert(all.end(), more.begin(), more.end());
        return all;
    }(), "and what it took follows what was waiting");

    expect(pair.set.write(pair.gateway, bytesOf(30)) == std::optional<std::size_t>(16), "a gateway write takes only what fits");
    expect(pair.set.write(pair.gateway, bytesOf(1)) == std::optional<std::size_t>(0), "and nothing more while it is full, as a socket's");
    expect(pair.set.clientReceive(pair.client, 6).size() == 6, "the client takes six");
    expect(pair.set.write(pair.gateway, bytesOf(30, 16)) == std::optional<std::size_t>(6), "which makes room for six");

    std::array<SocketInterest, 1> writable{SocketInterest{pair.gateway, false, true}};
    std::array<SocketInterest, 1> readable{SocketInterest{pair.gateway, true, false}};
    expect(!pair.set.clientReceive(pair.client).empty(), "the client takes everything");
    pair.set.wait(readable, WaitMode::LOOK);  // returns at once
    pair.set.wait(writable, WaitMode::SLEEP);  // room to write, so it doesn't sleep
}

void testHalfClose() {
    Pair pair;
    pair.set.clientSend(pair.client, bytesOf(3));
    pair.set.clientHalfClose(pair.client);
    expect(pair.set.clientSend(pair.client, bytesOf(1)) == 0, "a client that half-closed sends nothing more");
    const std::vector<std::byte> into = readAll(pair.set, pair.gateway, 2);
    expect(into.size() == 2, "the gateway still reads what came before");
    std::vector<std::byte> rest(10);
    ReadResult result = pair.set.read(pair.gateway, rest);
    expect(result.bytes == 1 && result.status == ReadStatus::OPEN, "all of it");
    result = pair.set.read(pair.gateway, rest);
    expect(result.bytes == 0 && result.status == ReadStatus::ENDED, "then reads the end");
    expect(pair.set.write(pair.gateway, bytesOf(4)) == std::optional<std::size_t>(4), "and still writes what it owes");
    expect(pair.set.clientReceive(pair.client) == bytesOf(4), "which the client still hears");
}

void testEndedAfterEveryByte() {
    Pair pair;
    pair.set.write(pair.gateway, bytesOf(5));
    pair.set.shutdownSend(pair.gateway);
    pair.set.close(pair.gateway);
    expect(pair.set.clientEnd(pair.client) == ClientEnd::OPEN, "a graceful close leaves the bytes waiting, so it hasn't ended yet");
    expect(pair.set.clientReceive(pair.client, 3) == bytesOf(3), "the client takes them");
    expect(pair.set.clientEnd(pair.client) == ClientEnd::OPEN, "some");
    expect(pair.set.clientReceive(pair.client) == bytesOf(2, 3), "then the rest");
    expect(pair.set.clientEnd(pair.client) == ClientEnd::ENDED, "and only then has it ended");
}

void testResetDropsWhatWasWaiting() {
    Pair pair;
    pair.set.write(pair.gateway, bytesOf(5));
    pair.set.close(pair.gateway);  // no shutdownSend: at once, as after a failed write
    expect(pair.set.clientEnd(pair.client) == ClientEnd::RESET, "a close without a shutdown is a reset");
    expect(pair.set.clientReceive(pair.client).empty(), "and the bytes waiting are lost");

    Pair other;
    other.set.clientSend(other.client, bytesOf(4));
    other.set.clientReset(other.client);
    std::vector<std::byte> into(10);
    const ReadResult result = other.set.read(other.gateway, into);
    expect(result.bytes == 0 && result.status == ReadStatus::FAILED, "a client's reset fails the gateway's read, its bytes dropped");
    expect(!other.set.write(other.gateway, bytesOf(1)).has_value(), "and its write finds the connection gone");
    std::array<SocketInterest, 1> readable{SocketInterest{other.gateway, true, false}};
    other.set.wait(readable, WaitMode::SLEEP);  // ready, since it is gone
}

void testSlotsAreReusedUnderAGeneration() {
    Pair pair;
    const SocketId first = pair.client;
    pair.set.shutdownSend(pair.gateway);
    pair.set.close(pair.gateway);
    pair.set.clientRelease(first);
    const SocketId second = *pair.set.clientConnect();
    expect(pair.set.slots() == 1, "a finished connection's slot is reused");
    expect(second != first, "under a new generation, so the ids differ");
    expect(*pair.set.accept() == second, "and the gateway accepts the new one");
    bool refused = false;
    try {
        static_cast<void>(pair.set.clientSend(first, bytesOf(1)));
    } catch (const std::logic_error&) {
        refused = true;
    }
    expect(refused, "the old id names nothing now");

    const SocketId third = *pair.set.clientConnect();
    expect(pair.set.slots() == 2, "two at once take two slots");
    pair.set.clientRelease(third);
    expect(pair.set.slots() == 2 && *pair.set.accept() == third, "a slot the gateway hasn't closed stays held, and its connection waits");
    pair.set.close(third);
    const SocketId fourth = *pair.set.clientConnect();
    expect(pair.set.slots() == 2 && fourth != third, "until both sides are done, when it is reused too");
}

void testAClosedListenerResetsWhatWaited() {
    std::atomic<int> told{0};
    PageSocketSet set([&told] { ++told; });
    const SocketId waiting = *set.clientConnect();
    set.closeListener();
    expect(set.clientEnd(waiting) == ClientEnd::RESET, "a connection never accepted is reset");
    expect(!set.accept().has_value(), "and accept finds nothing");
    expect(!set.clientConnect().has_value(), "and no new connection opens");
}

// The callback comes on every change the client must hear of, with no lock held: this one calls
// back into the set, which would deadlock if it came under the set's lock.
void testTheClientIsToldWithNoLockHeld() {
    std::atomic<int> told{0};
    PageSocketSet* self = nullptr;
    PageSocketSet set([&] {
        ++told;
        static_cast<void>(self->slots());
    });
    self = &set;
    const SocketId client = *set.clientConnect();
    const SocketId gateway = *set.accept();
    expect(told == 0, "nothing the client does tells it anything");
    set.clientSend(client, bytesOf(4));
    expect(told == 0, "its own send included");
    readAll(set, gateway, 2);
    expect(told == 1, "a read that frees room tells it");
    readAll(set, gateway, 0);
    expect(told == 1, "a read that takes nothing doesn't");
    set.write(gateway, bytesOf(3));
    expect(told == 2, "a write tells it");
    set.shutdownSend(gateway);
    expect(told == 3, "so does a shutdown");
    set.close(gateway);
    expect(told == 4, "and a close");
}

// A gateway thread that echoes whatever comes until the client half-closes, sleeping when there
// is nothing to do, and a client that sends more than the buffers hold, waiting on a condition
// variable the callback signals, as native tests stand in for the engine worker.
void testAThreadedEcho() {
    std::mutex guard;
    std::condition_variable signalled;
    bool something = false;
    PageSocketSet set(
        [&] {
            const std::lock_guard<std::mutex> held(guard);
            something = true;
            signalled.notify_all();
        },
        64);
    const SocketId client = *set.clientConnect();
    std::thread gateway([&set] {
        std::optional<SocketId> socket;
        while (!socket) {
            set.wait({}, WaitMode::SLEEP);
            socket = set.accept();
        }
        std::vector<std::byte> unsent;
        bool ended = false;
        while (!ended || !unsent.empty()) {
            const std::array<SocketInterest, 1> interest{SocketInterest{*socket, !ended, !unsent.empty()}};
            set.wait(interest, WaitMode::SLEEP);
            std::array<std::byte, 48> buffer{};
            const ReadResult read = ended ? ReadResult{0, ReadStatus::ENDED} : set.read(*socket, buffer);
            unsent.insert(unsent.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read.bytes));
            ended = ended || read.status == ReadStatus::ENDED;
            if (!unsent.empty()) {
                const std::size_t written = set.write(*socket, unsent).value_or(0);
                unsent.erase(unsent.begin(), unsent.begin() + static_cast<std::ptrdiff_t>(written));
            }
        }
        set.shutdownSend(*socket);
        set.close(*socket);
    });
    set.wake();  // a wake the gateway thread's first wait may meet, which changes nothing

    const std::vector<std::byte> sent = bytesOf(10'000);
    std::size_t written = 0;
    std::vector<std::byte> echoed;
    bool halfClosed = false;
    while (set.clientEnd(client) == ClientEnd::OPEN) {
        written += set.clientSend(client, std::span<const std::byte>(sent).subspan(written));
        if (written == sent.size() && !halfClosed) {
            set.clientHalfClose(client);
            halfClosed = true;
        }
        const std::vector<std::byte> taken = set.clientReceive(client);
        echoed.insert(echoed.end(), taken.begin(), taken.end());
        if (taken.empty()) {
            std::unique_lock<std::mutex> held(guard);
            const bool woke = signalled.wait_for(held, std::chrono::seconds(60), [&] { return something; });
            expect(woke, "the gateway's changes reach the client");
            something = false;
        }
    }
    gateway.join();
    expect(set.clientEnd(client) == ClientEnd::ENDED, "the echo ends after every byte");
    expect(echoed == sent, "every byte came back, in order, through buffers of 64");
}

}  // namespace

int main() {
    testOrderedBytesEachWayAndTrimming();
    testBuffersAreBoundedBothWays();
    testHalfClose();
    testEndedAfterEveryByte();
    testResetDropsWhatWasWaiting();
    testSlotsAreReusedUnderAGeneration();
    testAClosedListenerResetsWhatWaited();
    testTheClientIsToldWithNoLockHeld();
    testAThreadedEcho();
    std::cout << "page socket set tests passed" << std::endl;
    return EXIT_SUCCESS;
}
