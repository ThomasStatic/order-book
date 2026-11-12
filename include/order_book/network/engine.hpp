#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <ostream>

#include "order_book/network/socket_set.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/log_storage.hpp"

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

    // What the start sequence is given rather than making it, so an engine runs in one process
    // (engine_in_process_tests) and in the browser, through the same sequence as runEngine above,
    // which is a wrapper over the entry below. Each is shared, since a forced stop leaves the
    // engine's threads using them until the process exits.
    struct EngineParts {
        // Gateway thread `thread`'s sockets, called where the start binds them, before recovery,
        // thread 0's first; thread 0's set holds the listener, which listens once every other
        // gateway thread is ready. A std::system_error from it refuses the start as "cannot listen
        // on ADDR:PORT: ...". Required.
        std::function<std::shared_ptr<SocketSet>(std::size_t thread)> sockets;
        // The log and the checkpoints, in one store, in place of settings.logDir's files, which
        // must then be empty, as settings.checkpointDir must. Its lock is taken as a directory's.
        // Null: settings.logDir's files, if any.
        std::shared_ptr<runtime::LogStorage> logStorage;
        // Called once every shard and gateway thread is ready, in place of printing "listening
        // ADDR:PORT", and before any connection is accepted. Required.
        std::function<void()> ready;
    };

    EngineExit runEngine(const runtime::EngineSettings& settings, EngineParts parts, StopSource& stop, std::ostream& report,
                         std::ostream& diagnostics);

}  // namespace order_book::network
