#pragma once

#include <ostream>

#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"

namespace order_book::network {

    // How a run ended, grouped by what the operator does next, with the process exit status each
    // gives. A usage error, 2, never reaches runEngine, and 3 is left to an abort on Windows.
    enum class EngineExit : int {
        CLEAN_STOP = 0,            // every book published
        START_REFUSED = 1,         // fix the cause and start again
        PLACEMENT_CHANGED = 4,     // any placement change while a log exists, until D6
        RECOVERY_STOPPED = 5,      // the log holds what recovery can't explain
        STOPPED_BEFORE_READY = 6,  // nothing harmed: start again
        NOT_PUBLISHED = 7,         // from D6
        FORCED_STOP = 8,           // a second stop: the next start recovers
    };

    // The one way to run an engine: N shards, each on a matching thread of its own, behind one
    // gateway thread listening on settings.listen. Writes "listening ADDR:PORT", flushed, to report
    // once every shard is ready, and after a clean stop the stop report. diagnostics gets the run's
    // flags first, then one line per event, each prefixed "order_book_engine: ". A first request
    // from stop starts the clean stop, and a second forces one, leaving the threads to the process's
    // exit. A request before ready stops at once. Returns how the run ended; never throws.
    EngineExit runEngine(const runtime::EngineSettings& settings, StopSource& stop, std::ostream& report, std::ostream& diagnostics);

}  // namespace order_book::network
