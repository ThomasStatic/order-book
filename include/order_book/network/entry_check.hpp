#pragma once

#include <variant>

#include "order_book/book_event.hpp"
#include "order_book/command.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/runtime/instruments.hpp"

namespace order_book::network {

    // A command to queue: well formed, on a listed instrument, with the engine taking commands.
    struct EntryAccepted {
        Symbol symbol;
        runtime::BookLocation location;
        replay::Command command;
    };

    // Answered with a Refused frame, and safe to retry.
    struct EntryRefused {
        RefusedCode code;

        bool operator==(const EntryRefused&) const = default;
    };

    // Answered with this event, flagged last: what the book would have said.
    struct EntryMalformed {
        BookEvent event;

        bool operator==(const EntryMalformed&) const = default;
    };

    using EntryDecision = std::variant<EntryAccepted, EntryRefused, EntryMalformed>;

    // What the gateway does with a command whose frame's shape has passed: pure, stopping at the
    // first failure. An unknown or malformed symbol is refused. A malformed command gets the
    // book's own event and reason, where an unknown byte is an out-of-range enumerator, so
    // nothing here can disagree with the book. Last, a stopping engine refuses, so that code only
    // ever means this command would have been queued. What passes encodes to a record the log
    // reader accepts. With no owner the self-trade mode is cleared to absent, and an owner with
    // an absent mode takes CANCEL_RESTING.
    EntryDecision checkEntry(const CommandFrame& frame, const runtime::InstrumentDirectory& directory, bool stopping);

}  // namespace order_book::network
