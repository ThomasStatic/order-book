#include "command.hpp"

#include <charconv>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace order_book::randomized {

namespace {

constexpr unsigned int streamFormatVersion = 1;

constexpr CommandIntent allIntents[] = {
    CommandIntent::NEW_ID,          CommandIntent::REUSED_ID,       CommandIntent::DUPLICATE_ACTIVE_ID,
    CommandIntent::ZERO_ORDER_ID,   CommandIntent::ZERO_PRICE,      CommandIntent::ZERO_QUANTITY,
    CommandIntent::INVALID_SIDE,    CommandIntent::UNSUPPORTED_TYPE, CommandIntent::MARKED_FILLED,
    CommandIntent::INVALID_TIME_IN_FORCE, CommandIntent::MARKET_WITH_PRICE, CommandIntent::MARKET_GTC,
    CommandIntent::TYPE_CANNOT_REST, CommandIntent::POST_ONLY_NOT_GTC_LIMIT, CommandIntent::INVALID_STP_MODE,
    CommandIntent::STOP_GTC,        CommandIntent::POST_ONLY_STOP,  CommandIntent::MISSING_STOP_PRICE,
    CommandIntent::STOP_PRICE_ON_LIMIT, CommandIntent::CANCEL_PENDING_STOP,
    CommandIntent::CANCEL_ACTIVE,   CommandIntent::CANCEL_REMOVED,  CommandIntent::CANCEL_UNKNOWN,
    CommandIntent::CANCEL_ZERO_ID,  CommandIntent::UNSPECIFIED};

std::uint64_t parseNumber(const std::string& text, std::uint64_t max, const std::string& what) {
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || ptr != end || value > max) {
        throw std::invalid_argument("bad " + what + " '" + text + "'");
    }
    return value;
}

unsigned int parseField(const std::string& text, const std::string& what) {
    return static_cast<unsigned int>(parseNumber(text, std::numeric_limits<unsigned int>::max(), what));
}

// A side or type outside its enum is written as its underlying number.
std::string sideText(Side side) {
    if (side == Side::BUY) {
        return "BUY";
    }
    if (side == Side::SELL) {
        return "SELL";
    }
    return std::to_string(static_cast<int>(side));
}

Side parseSide(const std::string& text) {
    if (text == "BUY") {
        return Side::BUY;
    }
    if (text == "SELL") {
        return Side::SELL;
    }
    return static_cast<Side>(parseNumber(text, std::numeric_limits<int>::max(), "side"));
}

std::string typeText(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::STOP: return "STOP";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
    }
    return std::to_string(static_cast<int>(type));
}

OrderType parseType(const std::string& text) {
    if (text == "LIMIT") {
        return OrderType::LIMIT;
    }
    if (text == "MARKET") {
        return OrderType::MARKET;
    }
    if (text == "STOP") {
        return OrderType::STOP;
    }
    if (text == "STOP_LIMIT") {
        return OrderType::STOP_LIMIT;
    }
    return static_cast<OrderType>(parseNumber(text, std::numeric_limits<int>::max(), "type"));
}

std::string timeInForceText(TimeInForce timeInForce) {
    switch (timeInForce) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return std::to_string(static_cast<int>(timeInForce));
}

TimeInForce parseTimeInForce(const std::string& text) {
    if (text == "GTC") {
        return TimeInForce::GTC;
    }
    if (text == "IOC") {
        return TimeInForce::IOC;
    }
    if (text == "FOK") {
        return TimeInForce::FOK;
    }
    return static_cast<TimeInForce>(parseNumber(text, std::numeric_limits<int>::max(), "time in force"));
}

std::string modeText(SelfTradePrevention mode) {
    switch (mode) {
        case SelfTradePrevention::CANCEL_RESTING: return "CANCEL_RESTING";
        case SelfTradePrevention::CANCEL_INCOMING: return "CANCEL_INCOMING";
        case SelfTradePrevention::CANCEL_BOTH: return "CANCEL_BOTH";
    }
    return std::to_string(static_cast<int>(mode));
}

SelfTradePrevention parseMode(const std::string& text) {
    for (SelfTradePrevention mode : {SelfTradePrevention::CANCEL_RESTING, SelfTradePrevention::CANCEL_INCOMING,
                                     SelfTradePrevention::CANCEL_BOTH}) {
        if (text == modeText(mode)) {
            return mode;
        }
    }
    return static_cast<SelfTradePrevention>(parseNumber(text, std::numeric_limits<int>::max(), "self-trade prevention"));
}

CommandIntent parseIntent(const std::string& text) {
    for (CommandIntent intent : allIntents) {
        if (text == intentName(intent)) {
            return intent;
        }
    }
    throw std::invalid_argument("unknown intent '" + text + "'");
}

}  // namespace

const char* intentName(CommandIntent intent) {
    switch (intent) {
        case CommandIntent::NEW_ID: return "NEW_ID";
        case CommandIntent::REUSED_ID: return "REUSED_ID";
        case CommandIntent::DUPLICATE_ACTIVE_ID: return "DUPLICATE_ACTIVE_ID";
        case CommandIntent::ZERO_ORDER_ID: return "ZERO_ORDER_ID";
        case CommandIntent::ZERO_PRICE: return "ZERO_PRICE";
        case CommandIntent::ZERO_QUANTITY: return "ZERO_QUANTITY";
        case CommandIntent::INVALID_SIDE: return "INVALID_SIDE";
        case CommandIntent::UNSUPPORTED_TYPE: return "UNSUPPORTED_TYPE";
        case CommandIntent::MARKED_FILLED: return "MARKED_FILLED";
        case CommandIntent::INVALID_TIME_IN_FORCE: return "INVALID_TIME_IN_FORCE";
        case CommandIntent::MARKET_WITH_PRICE: return "MARKET_WITH_PRICE";
        case CommandIntent::MARKET_GTC: return "MARKET_GTC";
        case CommandIntent::TYPE_CANNOT_REST: return "TYPE_CANNOT_REST";
        case CommandIntent::POST_ONLY_NOT_GTC_LIMIT: return "POST_ONLY_NOT_GTC_LIMIT";
        case CommandIntent::INVALID_STP_MODE: return "INVALID_STP_MODE";
        case CommandIntent::STOP_GTC: return "STOP_GTC";
        case CommandIntent::POST_ONLY_STOP: return "POST_ONLY_STOP";
        case CommandIntent::MISSING_STOP_PRICE: return "MISSING_STOP_PRICE";
        case CommandIntent::STOP_PRICE_ON_LIMIT: return "STOP_PRICE_ON_LIMIT";
        case CommandIntent::CANCEL_PENDING_STOP: return "CANCEL_PENDING_STOP";
        case CommandIntent::CANCEL_ACTIVE: return "CANCEL_ACTIVE";
        case CommandIntent::CANCEL_REMOVED: return "CANCEL_REMOVED";
        case CommandIntent::CANCEL_UNKNOWN: return "CANCEL_UNKNOWN";
        case CommandIntent::CANCEL_ZERO_ID: return "CANCEL_ZERO_ID";
        case CommandIntent::UNSPECIFIED: return "UNSPECIFIED";
    }
    return "UNKNOWN";
}

bool isIntentionallyInvalid(CommandIntent intent) {
    switch (intent) {
        case CommandIntent::NEW_ID:
        case CommandIntent::REUSED_ID:
        case CommandIntent::CANCEL_ACTIVE:
        case CommandIntent::CANCEL_PENDING_STOP:
        case CommandIntent::UNSPECIFIED:
            return false;
        default:
            return true;
    }
}

Command Command::submit(OrderId orderId, Side side, Price price, Quantity quantity, CommandIntent intent) {
    Command command;
    command.type = CommandType::SUBMIT;
    command.orderId = orderId;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    command.intent = intent;
    return command;
}

Command Command::cancel(OrderId orderId, CommandIntent intent) {
    Command command;
    command.type = CommandType::CANCEL;
    command.orderId = orderId;
    command.intent = intent;
    return command;
}

std::string formatCommand(const Command& command) {
    std::string line;
    if (command.type == CommandType::CANCEL) {
        line = "CANCEL id=" + std::to_string(command.orderId);
    } else {
        line = "SUBMIT id=" + std::to_string(command.orderId) + " side=" + sideText(command.side) +
               " price=" + std::to_string(command.price) + " qty=" + std::to_string(command.quantity);
        if (command.orderType != OrderType::LIMIT) {
            line += " type=" + typeText(command.orderType);
        }
        if (command.timeInForce != TimeInForce::GTC) {
            line += " tif=" + timeInForceText(command.timeInForce);
        }
        if (command.stopPrice != 0) {
            line += " stop_price=" + std::to_string(command.stopPrice);
        }
        if (command.postOnly) {
            line += " post_only=Y";
        }
        if (command.owner != 0) {
            line += " owner=" + std::to_string(command.owner);
        }
        if (command.selfTradePrevention != SelfTradePrevention::CANCEL_RESTING) {
            line += " stp=" + modeText(command.selfTradePrevention);
        }
        if (command.markedFilled) {
            line += " status=FILLED";
        }
        if (command.overload == SubmitOverload::ORDER) {
            line += " overload=ORDER";
        }
    }
    if (command.intent != CommandIntent::UNSPECIFIED) {
        line += std::string(" intent=") + intentName(command.intent);
    }
    return line;
}

Command parseCommand(const std::string& line) {
    std::istringstream tokens(line);
    std::string operation;
    tokens >> operation;
    Command command;
    if (operation == "SUBMIT") {
        command.type = CommandType::SUBMIT;
    } else if (operation == "CANCEL") {
        command.type = CommandType::CANCEL;
    } else {
        throw std::invalid_argument("unknown operation '" + operation + "'");
    }

    bool sawId = false;
    bool sawSide = false;
    bool sawPrice = false;
    bool sawQuantity = false;
    std::string token;
    while (tokens >> token) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            throw std::invalid_argument("expected key=value, got '" + token + "'");
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        const bool submitOnly = key == "side" || key == "price" || key == "qty" || key == "type" || key == "status" ||
                                key == "overload" || key == "tif" || key == "post_only" || key == "owner" ||
                                key == "stp" || key == "stop_price";
        if (submitOnly && command.type == CommandType::CANCEL) {
            throw std::invalid_argument("a cancellation has no " + key);
        }
        if (key == "id") {
            command.orderId = parseField(value, "id");
            sawId = true;
        } else if (key == "side") {
            command.side = parseSide(value);
            sawSide = true;
        } else if (key == "price") {
            command.price = parseField(value, "price");
            sawPrice = true;
        } else if (key == "qty") {
            command.quantity = parseField(value, "qty");
            sawQuantity = true;
        } else if (key == "type") {
            command.orderType = parseType(value);
        } else if (key == "stop_price") {
            command.stopPrice = parseField(value, "stop price");
        } else if (key == "owner") {
            command.owner = parseField(value, "owner");
        } else if (key == "stp") {
            command.selfTradePrevention = parseMode(value);
        } else if (key == "post_only") {
            if (value != "Y") {
                throw std::invalid_argument("post_only can only be Y, got '" + value + "'");
            }
            command.postOnly = true;
        } else if (key == "tif") {
            command.timeInForce = parseTimeInForce(value);
        } else if (key == "status") {
            if (value != "FILLED") {
                throw std::invalid_argument("status can only be FILLED, got '" + value + "'");
            }
            command.markedFilled = true;
        } else if (key == "overload") {
            if (value != "ORDER") {
                throw std::invalid_argument("overload can only be ORDER, got '" + value + "'");
            }
            command.overload = SubmitOverload::ORDER;
        } else if (key == "intent") {
            command.intent = parseIntent(value);
        } else {
            throw std::invalid_argument("unknown field '" + key + "'");
        }
    }
    if (!sawId || (command.type == CommandType::SUBMIT && !(sawSide && sawPrice && sawQuantity))) {
        throw std::invalid_argument("missing a required field");
    }
    if (command.markedFilled && command.overload != SubmitOverload::ORDER) {
        throw std::invalid_argument("status=FILLED needs overload=ORDER: a request has no fill status");
    }
    if (command.overload == SubmitOverload::ORDER &&
        (command.timeInForce != TimeInForce::GTC || command.stopPrice != 0 || command.postOnly || command.owner != 0 ||
         command.selfTradePrevention != SelfTradePrevention::CANCEL_RESTING)) {
        throw std::invalid_argument("an Order has no time in force or post-only flag: they need the request overload");
    }
    return command;
}

void writeStream(std::ostream& out, const CommandStream& stream) {
    const StreamHeader& header = stream.header;
    out << "# order_book randomized command stream: " << stream.commands.size() << " commands\n";
    out << "# replay it exactly with: randomized_tests --replay <this file>\n";
    out << "format " << streamFormatVersion << '\n';
    if (header.generatorVersion != 0) {
        out << "generator " << header.generatorVersion << '\n';
    }
    if (!header.profile.empty()) {
        out << "profile " << header.profile << '\n';
    }
    if (header.seed) {
        out << "seed " << *header.seed << '\n';
    }
    if (!header.config.empty()) {
        out << "config " << header.config << '\n';
    }
    if (!header.note.empty()) {
        out << "note " << header.note << '\n';
    }
    for (std::size_t i = 0; i < stream.commands.size(); ++i) {
        out << formatCommand(stream.commands[i]) << '\n';
    }
}

CommandStream readStream(std::istream& in) {
    CommandStream stream;
    bool sawFormat = false;
    std::string line;
    for (std::size_t lineNumber = 1; std::getline(in, line); ++lineNumber) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        try {
            if (line.rfind("SUBMIT", 0) == 0 || line.rfind("CANCEL", 0) == 0) {
                stream.commands.push_back(parseCommand(line));
                continue;
            }
            const std::size_t space = line.find(' ');
            const std::string key = line.substr(0, space);
            const std::string value = space == std::string::npos ? "" : line.substr(space + 1);
            if (key == "format") {
                if (parseNumber(value, std::numeric_limits<unsigned int>::max(), "format") != streamFormatVersion) {
                    throw std::invalid_argument("unsupported stream format " + value);
                }
                sawFormat = true;
            } else if (key == "generator") {
                stream.header.generatorVersion = parseField(value, "generator version");
            } else if (key == "profile") {
                stream.header.profile = value;
            } else if (key == "seed") {
                stream.header.seed = parseNumber(value, std::numeric_limits<std::uint64_t>::max(), "seed");
            } else if (key == "config") {
                stream.header.config = value;
            } else if (key == "note") {
                stream.header.note = value;
            } else {
                throw std::invalid_argument("unknown header '" + key + "'");
            }
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("line " + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    if (!sawFormat) {
        throw std::invalid_argument("not a command stream: no format line");
    }
    return stream;
}

void saveStream(const std::string& path, const CommandStream& stream) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
    writeStream(out, stream);
    out.flush();
    if (!out) {
        throw std::runtime_error("failed writing " + path);
    }
}

CommandStream loadStream(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    try {
        return readStream(in);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(path + ": " + error.what());
    }
}

CommandStream CommandRecorder::stream(const std::string& note) const {
    CommandStream saved{streamHeader, recorded};
    saved.header.note = note;
    return saved;
}

}  // namespace order_book::randomized
