#include "order_book/network/client_stream.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "order_book/event_format.hpp"
#include "order_book/network/tcp.hpp"

namespace order_book::network {

    namespace {
        // A batch of outgoing frames: big enough that a write is rarely short of bytes.
        constexpr std::size_t fillBytes = 64 * 1024;
        constexpr std::size_t readBytes = 64 * 1024;
    }  // namespace

    std::string ClientStream::answerLine(const Answer& answer) {
        return "line " + std::to_string(answer.correlation) + ": " + answer.text + '\n';
    }

    ClientStream::ClientStream(std::vector<ClientCommand> sent)
        : commands(std::move(sent)), byCorrelation(commands.size()), isAnswered(commands.size(), false), unanswered(commands.size()) {
        std::iota(byCorrelation.begin(), byCorrelation.end(), std::size_t{0});
        std::sort(byCorrelation.begin(), byCorrelation.end(),
                  [this](std::size_t a, std::size_t b) { return commands[a].correlation < commands[b].correlation; });
    }

    void ClientStream::add(ClientCommand command) {
        if (command.correlation == 0 || (!commands.empty() && command.correlation <= commands[byCorrelation.back()].correlation)) {
            throw std::invalid_argument("a command added to a stream needs a correlation above every earlier one's");
        }
        // Each vector grows before anything indexes the new command, so a throw leaves it unseen.
        byCorrelation.reserve(byCorrelation.size() + 1);
        isAnswered.reserve(isAnswered.size() + 1);
        commands.push_back(std::move(command));
        byCorrelation.push_back(commands.size() - 1);
        isAnswered.push_back(false);
        ++unanswered;
    }

    std::span<const std::byte> ClientStream::outgoing() {
        if (outStart == out.size()) {
            fill();
        }
        return std::span<const std::byte>(out).subspan(outStart);
    }

    void ClientStream::wrote(std::size_t bytes) {
        outStart += std::min(bytes, out.size() - outStart);
    }

    bool ClientStream::allWritten() const noexcept {
        return helloQueued && nextCommand == commands.size() && outStart == out.size();
    }

    // Frames are made a batch at a time, so a large file isn't held twice.
    void ClientStream::fill() {
        out.resize(fillBytes);
        std::size_t size = 0;
        if (!helloQueued) {
            size += encodeHello(0, wireVersion, std::span<std::byte>(out).subspan(size));
            helloQueued = true;
        }
        while (nextCommand < commands.size() && out.size() - size >= commandFrameBytes) {
            const ClientCommand& command = commands[nextCommand++];
            size += encodeCommand(command.correlation, command.symbol, command.command, std::span<std::byte>(out).subspan(size));
        }
        out.resize(size);
        outStart = 0;
    }

    void ClientStream::received(std::span<const std::byte> bytes) {
        if (!reading()) {
            return;
        }
        reader.feed(bytes);
        while (!broken) {
            const FrameResult result = reader.next();
            if (std::holds_alternative<NeedMore>(result)) {
                return;
            }
            if (const FrameError* error = std::get_if<FrameError>(&result)) {
                broke("the engine sent a frame this client can't read: " + protocolErrorName(static_cast<std::uint16_t>(error->code)));
                return;
            }
            take(std::get<Frame>(result));
        }
    }

    void ClientStream::take(const Frame& frame) {
        if (protocolError) {
            broke("the engine sent a frame after its ProtocolError");
            return;
        }
        if (frame.type == FrameType::PROTOCOL_ERROR) {
            protocolError = true;
            const ProtocolErrorFrame error = decodeProtocolError(frame);
            problems.push_back("the engine sent ProtocolError " + protocolErrorName(error.code) +
                               (frame.correlation == 0 ? std::string() : " at line " + std::to_string(frame.correlation)));
            return;
        }
        // FrameReader has already refused a version this release can't speak.
        if (frame.type == FrameType::WELCOME) {
            if (welcomed) {
                broke("the engine sent a second Welcome");
                return;
            }
            welcomed = true;
            return;
        }
        if (!welcomed) {
            broke("the engine answered before its Welcome");
            return;
        }

        const std::optional<std::size_t> found = commandOf(frame.correlation);
        if (!found || isAnswered[*found]) {
            broke("the engine answered line " + std::to_string(frame.correlation) + ", which awaits no answer");
            return;
        }
        const std::size_t command = *found;

        // Every answer echoes its command's symbol, so a section holds only its own book's.
        const Symbol& symbol = commands[command].symbol;
        std::string text;
        std::variant<BookEvent, RefusedFrame> decoded;
        bool sameSymbol = true;
        if (frame.type == FrameType::REFUSED) {
            const RefusedFrame refusal = decodeRefused(frame);
            sameSymbol = refusal.symbol == symbol.bytes();
            text = "Refused symbol=" + symbol.text() + " id=" + std::to_string(refusal.orderId) + " code=" + refusedCodeName(refusal.code);
            decoded = refusal;
            refused = true;
        } else if (const std::optional<EventFrame> event = decodeEvent(frame)) {
            sameSymbol = event->symbol == symbol;
            text = formatEvent(event->event);
            decoded = event->event;
        } else {
            broke("the engine sent a frame this client can't read at line " + std::to_string(frame.correlation));
            return;
        }
        if (!sameSymbol) {
            broke("the engine answered line " + std::to_string(frame.correlation) + " under another symbol than " + symbol.text());
            return;
        }
        answers.push_back(Answer{frame.correlation, command, std::move(text), std::move(decoded), frameHeaderBytes + frame.body.size()});
        if (frame.last) {
            isAnswered[command] = true;
            --unanswered;
        }
    }

    void ClientStream::broke(std::string why) {
        broken = true;
        problems.push_back(std::move(why));
    }

    void ClientStream::ended() {
        if (streamEnded) {
            return;
        }
        streamEnded = true;
        if (!welcomed && !broken && !protocolError) {
            problems.push_back("the engine closed the connection before its Welcome");
        }
        if (unanswered > 0) {
            problems.push_back(std::to_string(unanswered) + " of " + std::to_string(commands.size()) +
                               " commands unanswered: what ran is unknown");
        }
    }

    bool ClientStream::reading() const noexcept {
        return !streamEnded && !broken;
    }

    // A ProtocolError after every answer changes nothing that ran, so it counts only as a loss
    // of the commands still unanswered.
    ConnectionEnd ClientStream::end() const noexcept {
        if (broken || !welcomed || unanswered > 0) {
            return ConnectionEnd::LOST;
        }
        return refused ? ConnectionEnd::REFUSED : ConnectionEnd::ANSWERED;
    }

    std::string ClientStream::sections() const {
        std::vector<const Answer*> sorted;
        sorted.reserve(answers.size());
        for (const Answer& answer : answers) {
            sorted.push_back(&answer);
        }
        // Stable, so a line's frames stay in the order they came.
        std::stable_sort(sorted.begin(), sorted.end(), [this](const Answer* a, const Answer* b) {
            const Symbol& left = commands[a->command].symbol;
            const Symbol& right = commands[b->command].symbol;
            return left != right ? left < right : a->correlation < b->correlation;
        });
        std::string text;
        const Symbol* section = nullptr;
        for (const Answer* answer : sorted) {
            const Symbol& symbol = commands[answer->command].symbol;
            if (section == nullptr || *section != symbol) {
                text += (section == nullptr ? "symbol " : "\nsymbol ") + symbol.text() + '\n';
                section = &symbol;
            }
            text += answerLine(*answer);
        }
        return text;
    }

    std::optional<std::size_t> ClientStream::commandOf(Correlation correlation) const {
        const auto found = std::lower_bound(byCorrelation.begin(), byCorrelation.end(), correlation,
                                            [this](std::size_t index, Correlation wanted) { return commands[index].correlation < wanted; });
        if (found == byCorrelation.end() || commands[*found].correlation != correlation) {
            return std::nullopt;
        }
        return *found;
    }

    bool ClientStream::answered(Correlation correlation) const {
        const std::optional<std::size_t> found = commandOf(correlation);
        return found && isAnswered[*found];
    }

    std::vector<ClientStream::Answer> ClientStream::answersOf(Correlation correlation) const {
        std::vector<Answer> found;
        for (const Answer& answer : answers) {
            if (answer.correlation == correlation) {
                found.push_back(answer);
            }
        }
        return found;
    }

    void ClientStream::forget(Correlation correlation) {
        std::erase_if(answers, [correlation](const Answer& answer) { return answer.correlation == correlation; });
    }

    std::string ClientStream::answerLines(Correlation correlation) const {
        std::string text;
        for (const Answer& answer : answers) {
            if (answer.correlation == correlation) {
                text += answerLine(answer);
            }
        }
        return text;
    }

    std::string protocolErrorName(std::uint16_t code) {
        switch (static_cast<ProtocolError>(code)) {
            case ProtocolError::BAD_LENGTH: return "BAD_LENGTH";
            case ProtocolError::BAD_MAGIC: return "BAD_MAGIC";
            case ProtocolError::UNKNOWN_FRAME_TYPE: return "UNKNOWN_FRAME_TYPE";
            case ProtocolError::HELLO_REQUIRED: return "HELLO_REQUIRED";
            case ProtocolError::DUPLICATE_HELLO: return "DUPLICATE_HELLO";
            case ProtocolError::UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
            case ProtocolError::UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
            case ProtocolError::UNEXPECTED_FIELD: return "UNEXPECTED_FIELD";
            case ProtocolError::TOO_MANY_CONNECTIONS: return "TOO_MANY_CONNECTIONS";
        }
        return std::to_string(code);
    }

    std::string refusedCodeName(std::uint16_t code) {
        switch (static_cast<RefusedCode>(code)) {
            case RefusedCode::UNKNOWN_INSTRUMENT: return "UNKNOWN_INSTRUMENT";
            case RefusedCode::ENGINE_STOPPING: return "ENGINE_STOPPING";
        }
        return std::to_string(code);
    }

    void exchange(ClientStream& stream, TcpSocket& socket) {
        socket.setNonBlocking();
        Poller poller;
        std::vector<std::byte> buffer(readBytes);
        bool writing = true;
        while (stream.reading()) {
            if (writing && stream.allWritten()) {
                socket.shutdownSend();  // the engine answers in full, then closes
                writing = false;
            }
            const bool wantsToWrite = writing && !stream.outgoing().empty();
            poller.clear();
            poller.add(socket.handle(), true, wantsToWrite);
            poller.wait(-1);
            if (wantsToWrite && poller.writable(0)) {
                if (const std::optional<std::size_t> sent = socket.send(stream.outgoing())) {
                    stream.wrote(*sent);
                } else {
                    writing = false;  // the connection is gone: the read that follows says how
                }
            }
            if (poller.readable(0)) {
                const ReadResult read = socket.receive(buffer);
                stream.received(std::span<const std::byte>(buffer).first(read.bytes));
                if (read.status != ReadStatus::OPEN) {
                    break;
                }
            }
        }
        stream.ended();
    }

}  // namespace order_book::network
