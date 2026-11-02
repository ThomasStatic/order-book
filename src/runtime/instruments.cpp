#include "order_book/runtime/instruments.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

#include "order_book/replay_parser.hpp"

namespace order_book::runtime {

    namespace {

        constexpr std::string_view expectedHeader = "symbol,shard,orders,stops,levels";
        constexpr std::size_t columnCount = 5;
        constexpr std::array<std::string_view, columnCount> columnNames{"symbol", "shard", "orders", "stops", "levels"};

        InstrumentListError fileError(std::size_t lineNumber, InstrumentListErrorCode code, std::string message) {
            return InstrumentListError{lineNumber, code, "", std::move(message)};
        }

        InstrumentListError fieldError(std::size_t lineNumber, InstrumentListErrorCode code, std::size_t column,
                                       std::string message) {
            return InstrumentListError{lineNumber, code, std::string(columnNames[column]), std::move(message)};
        }

        struct NumberField {
            std::optional<std::uint64_t> value;  // absent for an empty field
            std::optional<InstrumentListError> error;
        };

        NumberField parseNumber(std::string_view text, std::size_t lineNumber, std::size_t column) {
            if (text.empty()) {
                return {};
            }
            for (const char c : text) {
                if (c < '0' || c > '9') {
                    return {std::nullopt, fieldError(lineNumber, InstrumentListErrorCode::INVALID_NUMBER, column,
                                                     "'" + std::string(text) + "' is not an unsigned whole number")};
                }
            }
            std::uint64_t value = 0;
            for (const char c : text) {
                const auto digit = static_cast<std::uint64_t>(c - '0');
                if (value > (maxInstrumentListNumber - digit) / 10) {
                    return {std::nullopt, fieldError(lineNumber, InstrumentListErrorCode::NUMBER_OUT_OF_RANGE, column,
                                                     "'" + std::string(text) + "' exceeds the maximum " +
                                                         std::to_string(maxInstrumentListNumber))};
                }
                value = value * 10 + digit;
            }
            return {value, std::nullopt};
        }

        // One capacity column: 0 is refused, as in every replay column.
        std::optional<InstrumentListError> parseCapacity(std::string_view text, std::size_t lineNumber,
                                                         std::size_t column, std::optional<std::size_t>& out) {
            NumberField field = parseNumber(text, lineNumber, column);
            if (field.error) {
                return field.error;
            }
            if (field.value) {
                if (*field.value == 0) {
                    return fieldError(lineNumber, InstrumentListErrorCode::INVALID_VALUE, column,
                                      std::string(columnNames[column]) + " must be positive, not '" + std::string(text) + "'");
                }
                out = static_cast<std::size_t>(*field.value);
            }
            return std::nullopt;
        }

        // A row in the order the columns come, or its first problem.
        std::variant<Instrument, InstrumentListError> parseRow(std::string_view text, std::size_t lineNumber,
                                                               std::uint32_t shardCount) {
            if (text.empty()) {
                return fileError(lineNumber, InstrumentListErrorCode::EMPTY_RECORD, "a blank line");
            }
            if (text.find('"') != std::string_view::npos) {
                return fileError(lineNumber, InstrumentListErrorCode::QUOTE_CHARACTER, "fields are never quoted");
            }
            const std::vector<std::string> fields = replay::splitRecord(text, lineNumber).fields;
            if (fields.size() != columnCount) {
                return fileError(lineNumber, InstrumentListErrorCode::WRONG_COLUMN_COUNT,
                                 "expected 5 fields, found " + std::to_string(fields.size()));
            }

            const std::optional<Symbol> symbol = Symbol::parse(fields[0]);
            if (!symbol) {
                return fieldError(lineNumber, InstrumentListErrorCode::INVALID_SYMBOL, 0,
                                  "'" + fields[0] + "' is not a symbol");
            }
            Instrument instrument{*symbol, std::nullopt, std::nullopt, std::nullopt, std::nullopt};

            const NumberField shard = parseNumber(fields[1], lineNumber, 1);
            if (shard.error) {
                return *shard.error;
            }
            if (shard.value) {
                if (*shard.value >= shardCount) {
                    return fieldError(lineNumber, InstrumentListErrorCode::SHARD_OUT_OF_RANGE, 1,
                                      "shard " + fields[1] + " is not below the shard count " +
                                          std::to_string(shardCount));
                }
                instrument.shard = static_cast<std::uint32_t>(*shard.value);
            }

            if (auto error = parseCapacity(fields[2], lineNumber, 2, instrument.orders)) {
                return *std::move(error);
            }
            if (auto error = parseCapacity(fields[3], lineNumber, 3, instrument.stops)) {
                return *std::move(error);
            }
            if (auto error = parseCapacity(fields[4], lineNumber, 4, instrument.levels)) {
                return *std::move(error);
            }
            return instrument;
        }

    }  // namespace

    BookCapacity capacityOf(const Instrument& instrument, const BookCapacity& defaults) {
        return BookCapacity{instrument.orders.value_or(defaults.orders), instrument.stops.value_or(defaults.stops),
                            instrument.levels.value_or(defaults.levels)};
    }

    InstrumentListResult parseInstrumentList(std::string_view text, std::uint32_t shardCount) {
        if (shardCount == 0) {
            throw std::invalid_argument("an instrument list needs at least one shard");
        }
        if (text.empty()) {
            return fileError(1, InstrumentListErrorCode::MISSING_HEADER, "the list is empty, with no header");
        }

        std::vector<Instrument> instruments;
        std::set<Symbol> seen;
        std::size_t lineNumber = 0;
        while (!text.empty()) {
            ++lineNumber;
            const std::size_t lineFeed = text.find('\n');
            std::string_view line = text.substr(0, lineFeed);
            text.remove_prefix(lineFeed == std::string_view::npos ? text.size() : lineFeed + 1);
            // Only a CR directly before the LF ends a record; any other belongs to its field.
            if (lineFeed != std::string_view::npos && !line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }

            if (lineNumber == 1) {
                if (line != expectedHeader) {
                    return fileError(1, InstrumentListErrorCode::INVALID_HEADER,
                                     "the first line must be exactly " + std::string(expectedHeader));
                }
                continue;
            }

            std::variant<Instrument, InstrumentListError> row = parseRow(line, lineNumber, shardCount);
            if (InstrumentListError* error = std::get_if<InstrumentListError>(&row)) {
                return std::move(*error);
            }
            const Instrument& instrument = std::get<Instrument>(row);
            if (!seen.insert(instrument.symbol).second) {
                return fieldError(lineNumber, InstrumentListErrorCode::DUPLICATE_SYMBOL, 0,
                                  "'" + instrument.symbol.text() + "' is listed twice");
            }
            instruments.push_back(instrument);
        }

        if (instruments.empty()) {
            return fileError(lineNumber, InstrumentListErrorCode::NO_INSTRUMENTS, "the list names no instruments");
        }
        return instruments;
    }

    std::string formatInstrumentListError(std::string_view path, const InstrumentListError& error) {
        std::string text = std::string(path) + ":" + std::to_string(error.lineNumber) + ": ";
        if (!error.column.empty()) {
            text += error.column + ": ";
        }
        return text + error.message;
    }

    std::uint64_t symbolHash(const Symbol& symbol) noexcept {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char byte : symbol.bytes()) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::uint32_t hashedShard(const Symbol& symbol, std::uint32_t shardCount) noexcept {
        return static_cast<std::uint32_t>(symbolHash(symbol) % shardCount);
    }

    InstrumentDirectory::InstrumentDirectory(const std::vector<Instrument>& instruments, std::uint32_t shardCount)
        : shards(shardCount), bySlot(instruments) {
        if (shardCount == 0) {
            throw std::invalid_argument("an instrument directory needs at least one shard");
        }
        if (instruments.empty()) {
            throw std::invalid_argument("an instrument directory needs at least one instrument");
        }

        const auto shardOf = [shardCount](const Instrument& instrument) {
            return instrument.shard.value_or(hashedShard(instrument.symbol, shardCount));
        };
        for (const Instrument& instrument : bySlot) {
            if (instrument.symbol.empty()) {
                throw std::invalid_argument("an instrument directory cannot hold an empty symbol");
            }
            if (shardOf(instrument) >= shardCount) {
                throw std::invalid_argument("instrument " + instrument.symbol.text() + " is pinned past the last shard");
            }
        }

        std::sort(bySlot.begin(), bySlot.end(), [&shardOf](const Instrument& left, const Instrument& right) {
            const std::uint32_t leftShard = shardOf(left);
            const std::uint32_t rightShard = shardOf(right);
            return leftShard != rightShard ? leftShard < rightShard : left.symbol < right.symbol;
        });

        shardStarts.assign(static_cast<std::size_t>(shardCount) + 1, 0);
        for (const Instrument& instrument : bySlot) {
            ++shardStarts[static_cast<std::size_t>(shardOf(instrument)) + 1];
        }
        std::partial_sum(shardStarts.begin(), shardStarts.end(), shardStarts.begin());

        bySymbol.reserve(bySlot.size());
        for (std::size_t index = 0; index < bySlot.size(); ++index) {
            const std::uint32_t shard = shardOf(bySlot[index]);
            const auto slot = static_cast<std::uint32_t>(index - shardStarts[shard]);
            bySymbol.push_back(Entry{bySlot[index].symbol, BookLocation{shard, slot}});
        }

        std::sort(bySymbol.begin(), bySymbol.end(),
                  [](const Entry& left, const Entry& right) { return left.symbol < right.symbol; });
        const auto repeated = std::adjacent_find(bySymbol.begin(), bySymbol.end(), [](const Entry& left, const Entry& right) {
            return left.symbol == right.symbol;
        });
        if (repeated != bySymbol.end()) {
            throw std::invalid_argument("instrument " + repeated->symbol.text() + " is listed twice");
        }
    }

    std::optional<BookLocation> InstrumentDirectory::find(const Symbol& symbol) const noexcept {
        const auto found = std::lower_bound(bySymbol.begin(), bySymbol.end(), symbol,
                                            [](const Entry& entry, const Symbol& wanted) { return entry.symbol < wanted; });
        if (found == bySymbol.end() || found->symbol != symbol) {
            return std::nullopt;
        }
        return found->location;
    }

    std::span<const Instrument> InstrumentDirectory::onShard(std::uint32_t shard) const {
        if (shard >= shards) {
            throw std::out_of_range("no such shard: " + std::to_string(shard));
        }
        return std::span<const Instrument>(bySlot).subspan(shardStarts[shard], shardStarts[shard + 1] - shardStarts[shard]);
    }

}  // namespace order_book::runtime
