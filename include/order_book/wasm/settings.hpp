#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "order_book/runtime/engine_settings.hpp"

// The browser engine's settings: the one place the shipped values live (spec #127, "order_book_wasm";
// docs/internals/wasm.md). Not public API.
namespace order_book::wasm {

    inline constexpr std::uint32_t browserShards = 2;          // N
    inline constexpr std::uint32_t browserGatewayThreads = 1;  // M
    inline constexpr std::uint32_t browserRollWorkers = 1;     // R

    // Every thread the engine starts, its runEngine thread included: a matching thread and a log
    // writer per shard, each gateway thread, each roll worker, housekeeping, and runEngine's own.
    // The module's strict pthread pool is exactly this.
    inline constexpr std::uint32_t browserEngineThreads = 2 * browserShards + browserGatewayThreads + browserRollWorkers + 2;

    // Each way, per connection: what PageSocketSet holds.
    inline constexpr std::size_t pageSocketBuffer = 64 * 1024;
    // W: the frame bytes in flight per connection and direction between the page and the engine
    // worker before an acknowledgement.
    inline constexpr std::size_t creditWindow = 64 * 1024;

    // The engine's nominal address. Nothing binds it: it is what send's --connect names in the
    // terminal, native's default, and send to any other address can't connect.
    inline constexpr runtime::ListenAddress browserListen{{127, 0, 0, 1}, 9000};

    // The engine's settings in the browser, with the instrument list at this path. No log or
    // checkpoint directory: the page store is injected. Rings that fill are backpressure, and
    // books still grow past their capacity, which only warms them.
    inline runtime::EngineSettings browserSettings(const std::string& instrumentList) {
        runtime::EngineSettings settings;
        settings.instruments = instrumentList;
        settings.shards = browserShards;
        settings.listen = browserListen;
        settings.commandRing = 1024;
        settings.responseRing = 4096;
        settings.logRing = 4096;
        settings.capacityOrders = 4096;
        settings.capacityStops = 1024;
        settings.capacityLevels = 256;
        settings.maxConnections = 3;
        settings.gatewayThreads = browserGatewayThreads;
        settings.highWaterMark = 64 * 1024;
        settings.hardLimit = 1024 * 1024;
        settings.durability = runtime::Durability::PROCESS;
        settings.logFileSize = 64 * 1024;
        settings.keepLog = false;
        settings.rollWorkers = browserRollWorkers;
        return settings;
    }

}  // namespace order_book::wasm
