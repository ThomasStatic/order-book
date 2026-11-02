// order_book_engine: the matching engine as a process. Everything it does is runEngine's; this
// file only reads the command line, picks the stop source, and turns how the run ended into an
// exit status. See docs/internals/network.md.
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"

namespace {

constexpr int usageError = 2;

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const std::variant<order_book::runtime::EngineSettings, order_book::runtime::UsageError> parsed =
        order_book::runtime::parseEngineArguments(args);
    if (const auto* error = std::get_if<order_book::runtime::UsageError>(&parsed)) {
        std::cerr << "order_book_engine: " << error->message << '\n' << order_book::runtime::engineUsage() << std::flush;
        return usageError;
    }
    const order_book::runtime::EngineSettings& settings = std::get<order_book::runtime::EngineSettings>(parsed);

    // Signals come with their own stop source; until then, only closing stdin stops cleanly.
    std::unique_ptr<order_book::network::StopSource> stop;
    if (settings.stopOnStdinClose) {
        stop = std::make_unique<order_book::network::StdinStopSource>();
    } else {
        stop = std::make_unique<order_book::network::StopSource>();
    }
    const order_book::network::EngineExit exit = order_book::network::runEngine(settings, *stop, std::cout, std::cerr);
    if (exit == order_book::network::EngineExit::FORCED_STOP) {
        // Threads may still be running, so leave without destroying anything they use.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(static_cast<int>(exit));
    }
    return static_cast<int>(exit);
}
