#include "order_book/wasm/browser_engine.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <variant>

#include "order_book/runtime/instruments.hpp"
#include "order_book/wasm/settings.hpp"

namespace order_book::wasm {

    namespace {

        runtime::InstrumentDirectory directoryFrom(const std::string& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                throw std::runtime_error("cannot read the instrument list " + path);
            }
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            runtime::InstrumentListResult parsed = runtime::parseInstrumentList(text, browserShards);
            if (const auto* error = std::get_if<runtime::InstrumentListError>(&parsed)) {
                throw std::runtime_error(runtime::formatInstrumentListError(path, *error));
            }
            return runtime::InstrumentDirectory(std::get<std::vector<runtime::Instrument>>(parsed), browserShards);
        }

        BookCapacity capacityIn(const runtime::EngineSettings& settings) {
            return BookCapacity{settings.capacityOrders, settings.capacityStops, settings.capacityLevels};
        }

    }  // namespace

    BrowserEngine::BrowserEngine(const std::string& instrumentList, std::vector<runtime::PageLogStorage::StoredFile> store,
                                 BrowserCallbacks callbacks)
        : engineSettings(browserSettings(instrumentList)),
          told(std::move(callbacks)),
          books(directoryFrom(instrumentList), capacityIn(engineSettings)),
          pageStore(std::make_shared<runtime::PageLogStorage>(
              [this](const runtime::PageLogStorage::Change& change) {
                  books.follow(change);
                  if (told.stored) {
                      told.stored(change);
                  }
              },
              std::move(store))) {}

    network::EngineExit BrowserEngine::run(network::StopSource& stop, std::ostream& report, std::ostream& diagnostics) {
        books.rebuild(*pageStore);
        auto made = std::make_shared<network::PageSocketSet>(
            [this] {
                if (told.wake) {
                    told.wake();
                }
            },
            pageSocketBuffer);
        {
            const std::lock_guard held(guard);
            connections = made;
        }
        network::EngineParts parts;
        // One gateway thread (M = 1), so the set is thread 0's alone.
        parts.sockets = [made](std::size_t) -> std::shared_ptr<network::SocketSet> { return made; };
        parts.logStorage = pageStore;
        parts.ready = [this] {
            if (told.ready) {
                told.ready();
            }
        };
        return network::runEngine(engineSettings, std::move(parts), stop, report, diagnostics);
    }

    std::shared_ptr<network::PageSocketSet> BrowserEngine::sockets() const {
        const std::lock_guard held(guard);
        return connections;
    }

}  // namespace order_book::wasm
