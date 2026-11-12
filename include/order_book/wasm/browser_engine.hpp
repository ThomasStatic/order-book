#pragma once

#include <atomic>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "order_book/network/engine.hpp"
#include "order_book/network/page_socket_set.hpp"
#include "order_book/network/stop_source.hpp"
#include "order_book/runtime/engine_settings.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/wasm/shadow_books.hpp"

// The engine as the browser runs it (spec #127, "order_book_wasm"; docs/internals/wasm.md): the
// shipped settings, the page store, the in-page connections, and the shadow books, started
// through runEngine's injected parts. Not public API.
namespace order_book::wasm {

    // What the engine worker is told, each called on whichever engine thread got there.
    struct BrowserCallbacks {
        // The connections have something for the client: PageSocketSet's callback, called with
        // no lock held.
        std::function<void()> wake;
        // One page-store change, called under the store's lock once the shadows have followed it,
        // so an answer the page sees never comes before the log write it waited for.
        std::function<void(const runtime::PageLogStorage::Change&)> stored;
        // The engine is ready for connections, in place of `listening ADDR:PORT`.
        std::function<void()> ready;
    };

    class BrowserEngine {
    public:
        // The instrument list at this path, under browserSettings, and the page store as the page
        // holds it. Throws std::runtime_error if the list can't be read or parsed, since it ships
        // with the module.
        BrowserEngine(const std::string& instrumentList, std::vector<runtime::PageLogStorage::StoredFile> store,
                      BrowserCallbacks callbacks);

        BrowserEngine(const BrowserEngine&) = delete;
        BrowserEngine& operator=(const BrowserEngine&) = delete;

        // One run of the engine on the store, on the calling thread, until it stops: the shadows
        // rebuilt from the store, a new set of connections, then runEngine with these parts. A run
        // after a clean stop recovers, as any start does. runEngine never throws; before it, only
        // memory running out can.
        network::EngineExit run(network::StopSource& stop, std::ostream& report, std::ostream& diagnostics);

        // Whether the current run started on an empty store, the first load's or a reset's: then
        // the seed goes in over one connection before the page is told the engine is ready.
        bool startedEmpty() const noexcept { return emptyAtStart.load(); }

        // The settings each run starts with.
        const runtime::EngineSettings& settings() const noexcept { return engineSettings; }
        // The current run's connections, made before it starts, so they are there by its ready;
        // null before the first run.
        std::shared_ptr<network::PageSocketSet> sockets() const;
        ShadowBooks& shadows() noexcept { return books; }
        const ShadowBooks& shadows() const noexcept { return books; }
        runtime::PageLogStorage& store() noexcept { return *pageStore; }

    private:
        runtime::EngineSettings engineSettings;
        BrowserCallbacks told;
        ShadowBooks books;
        std::shared_ptr<runtime::PageLogStorage> pageStore;
        std::atomic<bool> emptyAtStart{false};
        mutable std::mutex guard;
        std::shared_ptr<network::PageSocketSet> connections;  // under the guard
    };

}  // namespace order_book::wasm
