#include "connection_streams.hpp"

#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <variant>

#include "order_book/event_format.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book::engine_testing {

    using namespace order_book::network;
    using namespace order_book::runtime;
    using order_book::gateway_testing::Bytes;
    using order_book::gateway_testing::helloFrame;
    using order_book::gateway_testing::rawCommandFrame;

    namespace {
        std::string describe(const std::vector<Line>& lines, std::size_t index) {
            if (index >= lines.size()) {
                return "nothing";
            }
            const Line& line = lines[index];
            return "correlation " + std::to_string(line.correlation) + " " + formatEvent(line.event) + (line.last ? " (last)" : "");
        }
    }  // namespace

    ConnectionStreams connectionStreams(const StreamShape& shape, const std::vector<EngineCommand>& lines) {
        ConnectionStreams streams;
        std::vector<Symbol> symbols;
        for (std::size_t i = 0; i < shape.instruments; ++i) {
            symbols.push_back(*Symbol::parse("SYM" + std::to_string(i)));
            const std::optional<std::uint32_t> shard =
                shape.pinned ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(i % shape.shards)) : std::nullopt;
            streams.instruments.push_back(Instrument{symbols.back(), shard, std::nullopt, std::nullopt, std::nullopt});
        }
        const InstrumentDirectory directory(streams.instruments, shape.shards);
        std::vector<std::unique_ptr<Shard>> witnesses;
        for (std::uint32_t shard = 0; shard < shape.shards; ++shard) {
            witnesses.push_back(std::make_unique<Shard>(directory, shard));
        }

        streams.bytes.assign(shape.connections, helloFrame());
        streams.frames.assign(shape.connections, 1);  // each gets a Welcome
        for (std::size_t index = 0; index < lines.size(); ++index) {
            const EngineCommand& line = lines[index];
            if (!line.converted) {
                continue;
            }
            if (line.instrument >= shape.instruments) {
                throw std::invalid_argument("a line names instrument " + std::to_string(line.instrument) + " of " +
                                            std::to_string(shape.instruments));
            }
            const Correlation correlation = index + 1;
            const std::array<char, Symbol::size> stored =
                line.fault == SymbolFault::NONE ? symbols[line.instrument].bytes() : Symbol::parse("ZZZZ")->bytes();
            const replay::RawCommand raw = rawCommandOf(*line.converted, stored);
            const std::size_t connection = line.instrument % shape.connections;
            const Bytes frame = rawCommandFrame(correlation, raw);
            if (!order_book::gateway_testing::roundTrips(frame, raw)) {
                throw std::runtime_error("a stream's frame isn't its command");
            }
            streams.bytes[connection].insert(streams.bytes[connection].end(), frame.begin(), frame.end());

            const EntryDecision decision = checkEntry(CommandFrame{correlation, raw}, directory, false);
            if (std::holds_alternative<EntryRefused>(decision)) {
                streams.refusedAtEntry.insert(correlation);
                ++streams.frames[connection];
            } else if (const EntryMalformed* malformed = std::get_if<EntryMalformed>(&decision)) {
                streams.refusedAsMalformed.insert({correlation, malformed->event});
                ++streams.frames[connection];
            } else {
                const EntryAccepted& accepted = std::get<EntryAccepted>(decision);
                streams.commands.push_back(StreamCommand{correlation, connection, streams.bytes[connection].size(), accepted});
                Shard& witness = *witnesses[accepted.location.shard];
                witness.process(accepted.command, accepted.location.slot);
                const std::span<const BookEvent> events = witness.result().events();
                for (std::size_t i = 0; i < events.size(); ++i) {
                    streams.expected[symbols[line.instrument].text()].push_back(Line{correlation, events[i], i + 1 == events.size()});
                    ++streams.frames[connection];
                }
            }
        }
        if (shape.plantExtraAnswer && shape.instruments != 0) {
            std::vector<Line>& first = streams.expected[symbols[0].text()];
            if (!first.empty()) {
                first.insert(first.begin(), first.front());  // never counted in frames, so nobody waits for it
            }
        }
        return streams;
    }

    std::vector<std::string> differencesFromSingleThreaded(const ConnectionStreams& streams,
                                                           const std::vector<std::vector<Received>>& received) {
        std::map<std::string, std::vector<Line>> answered;
        std::map<Correlation, BookEvent> malformed;
        std::set<Correlation> refused;
        std::vector<std::string> differences;
        for (const std::vector<Received>& frames : received) {
            for (const Received& frame : frames) {
                if (frame.type == FrameType::REFUSED) {
                    refused.insert(frame.correlation);
                } else if (frame.event && streams.refusedAsMalformed.contains(frame.correlation)) {
                    if (!frame.last) {
                        differences.push_back("malformed command " + std::to_string(frame.correlation) + "'s refusal isn't one whole answer");
                    }
                    malformed.insert({frame.correlation, frame.event->event});
                } else if (frame.event) {
                    answered[frame.event->symbol.text()].push_back(Line{frame.correlation, frame.event->event, frame.last});
                }
            }
        }
        std::set<std::string> symbols;
        for (const auto& [symbol, lines] : streams.expected) {
            symbols.insert(symbol);
        }
        for (const auto& [symbol, lines] : answered) {
            symbols.insert(symbol);
        }
        static const std::vector<Line> none;
        for (const std::string& symbol : symbols) {
            const auto wanted = streams.expected.find(symbol);
            const auto got = answered.find(symbol);
            const std::vector<Line>& expected = wanted == streams.expected.end() ? none : wanted->second;
            const std::vector<Line>& actual = got == answered.end() ? none : got->second;
            std::size_t first = 0;
            while (first < expected.size() && first < actual.size() && expected[first] == actual[first]) {
                ++first;
            }
            if (first != expected.size() || first != actual.size()) {
                differences.push_back(symbol + ": answer " + std::to_string(first + 1) + " of " + std::to_string(expected.size()) +
                                      " should be " + describe(expected, first) + ", but was " + describe(actual, first));
            }
        }
        if (malformed != streams.refusedAsMalformed) {
            differences.push_back("the malformed commands' refusals differ");
        }
        if (refused != streams.refusedAtEntry) {
            differences.push_back("the Refused frames differ");
        }
        return differences;
    }

}  // namespace order_book::engine_testing
