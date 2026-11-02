// order_book_engine: the matching engine as a process. Everything it does is runEngine's; this
// file only reads the command line, installs the stop signals and the abort handler, picks the
// stop source, and turns how the run ended into an exit status. See docs/internals/network.md.
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <variant>
#include <vector>

#include "engine_handlers.hpp"
#include "order_book/network/engine.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"

namespace {

constexpr int usageError = 2;

// Never destroyed, since the thread or handler that requests a stop may outlive main.
order_book::network::StopSource* stop = nullptr;

}  // namespace

int main(int argc, char** argv) {
    // Before any thread starts, so every thread inherits the blocked stop signals.
    order_book::engine_app::prepareStopSignals();
    order_book::engine_app::installAbortHandler();

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    // A help option anywhere wins over everything else on the line, as in order_book_cli.
    for (const std::string_view arg : args) {
        if (arg == "-h" || arg == "--help") {
            std::cout << order_book::runtime::engineUsage() << std::flush;
            return EXIT_SUCCESS;
        }
    }
    const std::variant<order_book::runtime::EngineSettings, order_book::runtime::UsageError> parsed =
        order_book::runtime::parseEngineArguments(args);
    if (const auto* error = std::get_if<order_book::runtime::UsageError>(&parsed)) {
        std::cerr << "order_book_engine: " << error->message << '\n' << order_book::runtime::engineUsage() << std::flush;
        return usageError;
    }
    const order_book::runtime::EngineSettings& settings = std::get<order_book::runtime::EngineSettings>(parsed);

    // Signals stop the engine either way; closing stdin does too when asked.
    if (settings.stopOnStdinClose) {
        stop = new order_book::network::StdinStopSource();
    } else {
        stop = new order_book::network::StopSource();
    }
    order_book::engine_app::forwardStopSignals(*stop);

    const order_book::network::EngineExit exit = order_book::network::runEngine(settings, *stop, std::cout, std::cerr);
    if (exit == order_book::network::EngineExit::FORCED_STOP) {
        // Threads may still be running, so leave without destroying anything they use.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(static_cast<int>(exit));
    }
    return static_cast<int>(exit);
}
