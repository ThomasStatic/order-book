#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/page_log_storage.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/symbol.hpp"
#include "order_book/wasm/shadow_books.hpp"

// For the tests that run the whole engine on a page store: after a clean stop, which published
// each book's image at its log's end once it matched the live book, each shadow must equal it.
namespace order_book::wasm_testing {

    // The first way the shadows differ from the shards' books at the clean stop that left these
    // files, or nothing.
    inline std::optional<std::string> shadowsDifferFromTheStop(const wasm::ShadowBooks& shadows, const std::vector<replay::Symbol>& symbols,
                                                               const std::vector<runtime::PageLogStorage::StoredFile>& files) {
        if (const std::optional<std::string> problem = shadows.problem()) {
            return "the shadows stopped following: " + *problem;
        }
        std::map<std::string, std::span<const std::byte>> stored;
        for (const runtime::PageLogStorage::StoredFile& file : files) {
            if (file.name.ends_with(".live")) {
                return "a live image differed from its log at the stop: " + file.name;
            }
            stored[file.name] = file.bytes;
        }
        for (const replay::Symbol& symbol : symbols) {
            const std::string name = runtime::checkpointFileName(symbol, shadows.records(*shadows.shardOf(symbol)));
            const auto found = stored.find(name);
            if (found == stored.end()) {
                return name + " wasn't published at its shard's log end";
            }
            const auto decoded = runtime::decodeCheckpoint(found->second);
            if (!std::holds_alternative<runtime::DecodedCheckpoint>(decoded)) {
                return name + " doesn't decode";
            }
            const std::span<const std::byte> image = std::get<runtime::DecodedCheckpoint>(decoded).image;
            if (shadows.image(symbol) != std::vector<std::byte>(image.begin(), image.end())) {
                return symbol.text() + "'s shadow differs from its shard's book";
            }
        }
        return std::nullopt;
    }

}  // namespace order_book::wasm_testing
