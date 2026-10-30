#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "order_book/order_type.hpp"
#include "order_book/self_trade_prevention.hpp"
#include "order_book/side.hpp"
#include "order_book/time_in_force.hpp"
#include "order_book/types.hpp"

namespace order_book::randomized {

enum class CommandType { SUBMIT, CANCEL, REPLACE };

// Which submitOrder overload a submission goes through. Nearly every command uses the request;
// the Order overload carries the faults only an Order can have (MARKED_FILLED) and a share of
// valid v1-shaped limits, so the two overloads keep being shown to agree.
enum class SubmitOverload { REQUEST, ORDER };

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
    INVALID_TIME_IN_FORCE,
    MARKET_WITH_PRICE,  // PRICE_NOT_ALLOWED
    MARKET_GTC,         // TIME_IN_FORCE_NOT_ALLOWED
    TYPE_CANNOT_REST,   // through submitOrder(Order): a valid type other than LIMIT
    POST_ONLY_NOT_GTC_LIMIT,  // POST_ONLY_NOT_ALLOWED: on a market order, or an IOC or FOK limit
    INVALID_STP_MODE,         // INVALID_SELF_TRADE_PREVENTION, with or without an owner
    STOP_GTC,                 // TIME_IN_FORCE_NOT_ALLOWED on a STOP
    POST_ONLY_STOP,           // POST_ONLY_NOT_ALLOWED on a GTC STOP_LIMIT
    MISSING_STOP_PRICE,       // INVALID_STOP_PRICE: a stop with none
    STOP_PRICE_ON_LIMIT,      // STOP_PRICE_NOT_ALLOWED: a limit or market order with one
    // Valid cancellations, then intentionally invalid ones.
    CANCEL_ACTIVE,
    CANCEL_PENDING_STOP,
    CANCEL_REMOVED,  // filled or cancelled already
    CANCEL_UNKNOWN,  // never issued
    CANCEL_ZERO_ID,
    // Valid replaces of a resting order, then intentionally invalid ones.
    REPLACE_DECREASE,       // same price, a lower total above what has executed: keeps priority
    REPLACE_INSTRUCTIONS,   // same price and total, new owner, mode, or post-only: keeps priority
    REPLACE_PRICE,          // a new price, near the touch or marketable: loses priority, may cross
    REPLACE_INCREASE,       // same price, a higher total: loses priority
    REPLACE_POST_ONLY_TAKE, // post-only at a price that would take: expires whole
    REPLACE_ZERO_ID,
    REPLACE_ZERO_PRICE,
    REPLACE_ZERO_QUANTITY,
    REPLACE_INVALID_MODE,
    REPLACE_UNKNOWN,
    REPLACE_REMOVED,
    REPLACE_PENDING_STOP,   // ORDER_NOT_REPLACEABLE
    REPLACE_AT_EXECUTED,    // REPLACE_QUANTITY_TOO_LOW, a total of exactly what has executed
    REPLACE_BELOW_EXECUTED, // and below it
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
    TimeInForce timeInForce = TimeInForce::GTC;  // only through the REQUEST overload
    Price stopPrice = 0;                         // likewise
    bool postOnly = false;                       // likewise
    OwnerId owner = 0;                           // likewise
    SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;  // likewise
    bool markedFilled = false;  // only through the ORDER overload: a request has no fill status
    SubmitOverload overload = SubmitOverload::REQUEST;
    CommandIntent intent = CommandIntent::UNSPECIFIED;

    bool operator==(const Command&) const = default;

    static Command submit(OrderId orderId, Side side, Price price, Quantity quantity,
                          CommandIntent intent = CommandIntent::UNSPECIFIED);
    static Command cancel(OrderId orderId, CommandIntent intent = CommandIntent::UNSPECIFIED);
    // A replace's fields are its ID, price, quantity (the new total), post-only, owner, and mode.
    static Command replace(OrderId orderId, Price price, Quantity total,
                           CommandIntent intent = CommandIntent::UNSPECIFIED);
};

// One line, e.g. "SUBMIT id=17 side=BUY price=10002 qty=5 intent=NEW_ID",
// "CANCEL id=291 intent=CANCEL_ACTIVE", or "REPLACE id=8 price=10001 qty=12 intent=REPLACE_PRICE". A side or type outside its enum prints as a number,
// and type, time in force, stop price, post-only, owner, mode, status, and overload appear only
// when they differ from LIMIT, GTC, 0, not post-only, owner 0, CANCEL_RESTING, an unfilled
// order, and the request overload.
std::string formatCommand(const Command& command);

// The inverse of formatCommand; throws std::invalid_argument saying what is wrong, including
// for status=FILLED without overload=ORDER, or a request-only field with it.
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
