#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "order_book/order_type.hpp"
#include "order_book/side.hpp"
#include "order_book/types.hpp"

namespace order_book::randomized {

enum class CommandType { SUBMIT, CANCEL };

// What the generator meant a command to exercise. Informational only: running a command
// never reads it, and after shrinking it can be stale (a CANCEL_ACTIVE whose submission was
// removed now names an unknown ID).
enum class CommandIntent {
    // Valid submissions.
    NEW_ID,
    REUSED_ID,  // an ID that rested once and has since been filled or cancelled
    // Intentionally invalid submissions, each breaking one thing.
    DUPLICATE_ACTIVE_ID,
    ZERO_ORDER_ID,
    ZERO_PRICE,
    ZERO_QUANTITY,
    INVALID_SIDE,
    UNSUPPORTED_TYPE,
    MARKED_FILLED,
    // A valid cancellation, then intentionally invalid ones.
    CANCEL_ACTIVE,
    CANCEL_REMOVED,  // filled or cancelled already
    CANCEL_UNKNOWN,  // never issued
    CANCEL_ZERO_ID,
    UNSPECIFIED      // written by hand
};

const char* intentName(CommandIntent intent);
bool isIntentionallyInvalid(CommandIntent intent);

// One public command. Submission fields are held raw, so a deliberately malformed order (a
// zero field, a side or type outside its enum, or one already marked FILLED) survives a
// round trip through a saved stream exactly.
struct Command {
    CommandType type = CommandType::SUBMIT;
    OrderId orderId = 0;
    Side side = Side::BUY;
    Price price = 0;
    Quantity quantity = 0;
    OrderType orderType = OrderType::LIMIT;
    bool markedFilled = false;
    CommandIntent intent = CommandIntent::UNSPECIFIED;

    bool operator==(const Command&) const = default;

    static Command submit(OrderId orderId, Side side, Price price, Quantity quantity,
                          CommandIntent intent = CommandIntent::UNSPECIFIED);
    static Command cancel(OrderId orderId, CommandIntent intent = CommandIntent::UNSPECIFIED);
};

// One line, e.g. "SUBMIT id=17 side=BUY price=10002 qty=5 intent=NEW_ID" or
// "CANCEL id=291 intent=CANCEL_ACTIVE". A side or type outside its enum prints as a number,
// and type and status appear only when they differ from LIMIT and an unfilled order.
std::string formatCommand(const Command& command);

// The inverse of formatCommand; throws std::invalid_argument saying what is wrong.
Command parseCommand(const std::string& line);

// Where a saved stream came from. Replay never needs any of it: the commands alone fix
// the run. It is there so a reader can regenerate the stream or see what produced it.
struct StreamHeader {
    unsigned int generatorVersion = 0;  // 0 for a stream not produced by the generator
    std::string profile;
    std::optional<std::uint64_t> seed;
    std::string config;  // the profile's parameters, on one line
    std::string note;

    bool operator==(const StreamHeader&) const = default;
};

struct CommandStream {
    StreamHeader header;
    std::vector<Command> commands;

    bool operator==(const CommandStream&) const = default;
};

// A line-based text format: comments start with '#', header lines are "key value", and
// every other line is one formatCommand line. readStream throws std::invalid_argument
// naming the offending line.
void writeStream(std::ostream& out, const CommandStream& stream);
CommandStream readStream(std::istream& in);

// Throw std::runtime_error if the file cannot be written or opened.
void saveStream(const std::string& path, const CommandStream& stream);
CommandStream loadStream(const std::string& path);

// Collects each command as it is about to run, so that after a failure it holds the
// complete executed prefix, failing command included.
class CommandRecorder {
public:
    explicit CommandRecorder(StreamHeader header = {}) : streamHeader(std::move(header)) {}

    void record(const Command& command) { recorded.push_back(command); }
    const std::vector<Command>& commands() const { return recorded; }
    CommandStream stream(const std::string& note) const;
    void save(const std::string& path, const std::string& note) const { saveStream(path, stream(note)); }

private:
    StreamHeader streamHeader;
    std::vector<Command> recorded;
};

}  // namespace order_book::randomized
