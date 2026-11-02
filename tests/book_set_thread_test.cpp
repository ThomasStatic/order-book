#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

#include "order_book/invariants.hpp"
#include "order_book/runtime/book_set.hpp"

// The owning-thread assert: a call from a second thread should abort, printing
//   order book runtime: BookSet::<call> called off its owning thread: shard=0 slot=0
// which CTest matches. Skipped where automatic checks are off, since the assert is too.
namespace {

using namespace order_book;
using namespace order_book::runtime;

[[maybe_unused]] int run(const std::string& call) {
    const Symbol symbol = *Symbol::parse("AAPL");
    const InstrumentDirectory directory({Instrument{symbol, 0, std::nullopt, std::nullopt, std::nullopt}}, 1);
    BookSet set(directory, 0);
    set.execute(0, replay::CancelCommand{1});  // the owner may, of course

    std::thread intruder([&set, &call] {
        if (call == "execute") {
            set.execute(0, replay::CancelCommand{1});
        } else if (call == "book") {
            static_cast<void>(set.book(0));
        } else {
            std::cerr << "FAILED: unknown call " << call << std::endl;
            std::exit(EXIT_FAILURE);
        }
    });
    intruder.join();
    std::cerr << "FAILED: " << call << " returned on a thread that does not own the set" << std::endl;
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    if constexpr (!invariantChecksAfterCommands) {
        std::cout << "SKIPPED: automatic invariant checks are off in this build" << std::endl;
        return EXIT_SUCCESS;
    } else {
        if (argc != 2) {
            std::cerr << "FAILED: usage: book_set_thread_tests <call>" << std::endl;
            return EXIT_FAILURE;
        }
#ifdef _MSC_VER
        // The abort is expected: skip the debug CRT's message box and crash report.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        // CTest fails any test killed by a signal, whatever it printed, so turn the expected
        // abort into a normal exit and let the output alone decide.
        std::signal(SIGABRT, [](int) { std::_Exit(EXIT_SUCCESS); });
        return run(argv[1]);
    }
}
