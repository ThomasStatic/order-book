#include "shrink.hpp"

#include <algorithm>

#include "order_book/order_book.hpp"

namespace order_book::randomized {

ShrinkResult shrinkFailingStream(const std::vector<Command>& commands, const Failure& failure,
                                 const CommandReplayer& replayer, std::size_t maxAttempts) {
    ShrinkResult result{
        std::vector<Command>(commands.begin(),
                             commands.begin() + static_cast<std::ptrdiff_t>(
                                                    std::min(failure.commandIndex + 1, commands.size()))),
        failure, 0, false};

    // The candidate's failure, if it fails the same way as the original.
    auto failsTheSameWay = [&](const std::vector<Command>& candidate) -> std::optional<Failure> {
        ++result.attempts;
        OrderBook book;
        RunResult run = replayer.replay(book, candidate);
        if (run.failure && run.failure->sameKindAs(failure)) {
            return run.failure;
        }
        return std::nullopt;
    };

    std::size_t chunks = 2;
    while (result.commands.size() >= 2) {
        const std::size_t size = result.commands.size();
        const std::size_t chunkSize = (size + chunks - 1) / chunks;
        bool reduced = false;
        for (std::size_t start = 0; start < size; start += chunkSize) {
            if (result.attempts >= maxAttempts) {
                return result;
            }
            std::vector<Command> candidate(result.commands.begin(),
                                           result.commands.begin() + static_cast<std::ptrdiff_t>(start));
            candidate.insert(candidate.end(),
                             result.commands.begin() + static_cast<std::ptrdiff_t>(std::min(start + chunkSize, size)),
                             result.commands.end());
            if (std::optional<Failure> candidateFailure = failsTheSameWay(candidate)) {
                candidate.resize(candidateFailure->commandIndex + 1);
                result.commands = std::move(candidate);
                result.failure = std::move(*candidateFailure);
                chunks = std::max<std::size_t>(chunks - 1, 2);
                reduced = true;
                break;
            }
        }
        if (!reduced) {
            if (chunks >= size) {
                break;
            }
            chunks = std::min(chunks * 2, size);
        }
    }
    result.complete = true;
    return result;
}

}  // namespace order_book::randomized
