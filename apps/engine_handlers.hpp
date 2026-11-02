#pragma once

#include "order_book/network/stop_source.hpp"

// What order_book_engine installs before it runs: the stop signals and the abort handler. The
// only code that touches either, outside the tests that link it. See docs/internals/network.md.
namespace order_book::engine_app {

    // First thing in main, before any thread starts. On Linux it blocks SIGINT, SIGTERM, and
    // SIGHUP, which every later thread inherits, and ignores SIGPIPE. Elsewhere it does nothing.
    void prepareStopSignals();

    // Turns every stop signal into a request on stop, which must outlive the process. On Linux
    // one thread collects them with sigwait. On Windows a console control handler takes Ctrl-C,
    // Ctrl-Break, close, logoff, and shutdown; for the last three it holds the process open until
    // the stop finishes, or the system ends it when its own timeout passes.
    void forwardStopSignals(network::StopSource& stop);

    // SIGABRT prints runtime::describeAbort's line from a fixed buffer, straight to stderr, and
    // the process then dies as usual: by SIGABRT on Linux and with status 3 on Windows, where
    // the crash dialog is turned off.
    void installAbortHandler();

}  // namespace order_book::engine_app
