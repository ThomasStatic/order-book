#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine_harness.hpp"
#include "gateway_frames.hpp"
#include "gateway_rig.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/runtime/instruments.hpp"

// A stream of lines as clients send it, one connection owning each instrument it divides into,
// and what a single-threaded run answers, which the stepper and the real-thread runs both hold
// the gateway to. See docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    struct Line {
        network::Correlation correlation;
        BookEvent event;
        bool last;

        bool operator==(const Line&) const = default;
    };

    // A command the entry check passed, and where its frame ends in its connection's stream.
    struct StreamCommand {
        network::Correlation correlation;
        std::size_t connection;
        std::size_t frameEnd;
        network::EntryAccepted accepted;
        randomized::Command converted;  // what the model takes for it
    };

    // A log whose replay through a book differs from the model's: a logic bug, not a timing one.
    class LogReplayDiverged : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct StreamShape {
        std::size_t instruments = 6;
        std::uint32_t shards = 2;
        std::size_t connections = 3;  // connection i owns every instrument k with k mod connections = i
        bool pinned = false;          // instrument k on shard k mod shards, rather than by its hash
        // For testing the real-thread triage: instrument 0's first answer is expected twice, which
        // no run gives, so every run with one of its commands fails the same way.
        bool plantExtraAnswer = false;
        // Above 1, each instrument has this many connections of its own, and connections is
        // ignored: instrument k's are k × this + j, and a command goes to the j of its order ID
        // mod this, so every command about one order is on one connection and in order there. How
        // the connections' commands interleave is then the matching thread's take order, so
        // expected and frames mean nothing until expectedFromLog fills them in.
        std::size_t connectionsPerInstrument = 1;
    };

    struct ConnectionStreams {
        std::vector<runtime::Instrument> instruments;
        std::vector<gateway_testing::Bytes> bytes;          // per connection, Hello first
        std::vector<std::size_t> frames;                    // the frames expected back, per connection
        std::vector<std::size_t> answers;                   // the commands sent, each answered once, per connection
        std::map<std::string, std::vector<Line>> expected;  // from the books, per instrument
        std::map<network::Correlation, BookEvent> refusedAsMalformed;
        std::set<network::Correlation> refusedAtEntry;      // with a Refused frame
        std::vector<StreamCommand> commands;
    };

    // Line i goes under correlation i + 1. Every line's instrument must be one of shape's.
    ConnectionStreams connectionStreams(const StreamShape& shape, const std::vector<EngineCommand>& lines);

    // streams with expected and frames from each instrument's log in place of a single-threaded
    // run: each record goes to the one connection whose next command for that instrument it is,
    // and is replayed single-threaded through the model, its ReferenceBook, and a fresh book,
    // whose events must agree; expected lists the model's answers, connection by connection, as
    // differencesFromSingleThreaded gathers what was received. Throws std::runtime_error if a
    // record is no connection's next command, or a command that passed entry isn't logged, and
    // LogReplayDiverged if the book and the model disagree.
    ConnectionStreams expectedFromLog(const ConnectionStreams& streams, const std::map<std::string, std::vector<replay::Command>>& logged);

    // How what each connection received differs from the single-threaded run: per instrument, its
    // first differing answer, then entry's own answers. Empty if nothing does.
    std::vector<std::string> differencesFromSingleThreaded(const ConnectionStreams& streams,
                                                           const std::vector<std::vector<Received>>& received);

}  // namespace order_book::engine_testing
