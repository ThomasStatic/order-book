#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "order_book/runtime/log_storage.hpp"

namespace order_book::runtime {

    // The browser's LogStorage (docs/internals/runtime.md, ADR 0017): the log's and the
    // checkpoints' files, in one store, held in memory and read there, as InMemoryLogStorage's
    // are, with every change also told to an injected callback before the call that made it
    // returns. In the browser that callback hands the change to the page, whose copy, the page
    // store, outlives the engine worker: a new worker is given it back as a list of files, and
    // the engine recovers from that as from a directory after a crash.
    //
    // Every change is reported: create, write (offset and bytes), extend (the new size), cut
    // (the size kept), rename, replace, and remove. A flush reports nothing, since the browser
    // runs at process durability, where a write already holds once it returns. The callback
    // runs under the store's lock, so changes from different threads reach it in the order they
    // were made; it must not call back into the store. A rename never replaces a file, and the
    // lock is a flag, as InMemoryLogStorage's is. Durability is ignored.
    class PageLogStorage final : public LogStorage {
    public:
        struct Change {
            enum class Kind { CREATE, WRITE, EXTEND, CUT, RENAME, REPLACE, REMOVE };
            Kind kind = Kind::CREATE;
            std::string name;                  // the file changed
            std::string to;                    // RENAME and REPLACE: its new name
            std::uint64_t offset = 0;          // WRITE: where the bytes go
            std::span<const std::byte> bytes;  // WRITE: what was written, valid during the call
            std::uint64_t size = 0;            // EXTEND: the new size; CUT: the size kept
        };

        // A file as the page store holds it: every byte up to its size, zeros past the last
        // write included.
        struct StoredFile {
            std::string name;
            std::vector<std::byte> bytes;

            bool operator==(const StoredFile&) const = default;
        };

        // An empty store, or one holding these files, as a new engine worker is given them.
        explicit PageLogStorage(std::function<void(const Change&)> changes, std::vector<StoredFile> from = {});

        std::unique_ptr<StorageLock> lock() override;
        std::vector<std::string> names() const override;
        std::unique_ptr<LogFile> open(const std::string& name) override;
        std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override;
        void remove(const std::string& name) override;
        std::string describe(const std::string& name) const override;

        // Every file, sorted by name, as the page store must hold them after every change so far.
        std::vector<StoredFile> files() const;

        // Makes change in a page store's files, as the page does with each one it is told of. A
        // change to a file the store doesn't hold is ignored.
        static void apply(std::map<std::string, std::vector<std::byte>>& store, const Change& change);
        // Such a copy as the list of files a store is built from.
        static std::vector<StoredFile> storedFiles(const std::map<std::string, std::vector<std::byte>>& store);

    private:
        struct File {
            std::vector<std::byte> bytes;  // up to the end of the last write; zeros follow, to size
            std::uint64_t size = 0;
            std::string name;  // what the store holds it under, whichever handle renamed it
        };
        class OpenFile;
        class HeldLock;

        const std::function<void(const Change&)> onChange;
        mutable std::mutex guard;
        // Shared with each open file. A removed file's handle still writes to it, where no one
        // reads, and is told of nothing.
        std::map<std::string, std::shared_ptr<File>> stored;
        bool locked = false;
    };

}  // namespace order_book::runtime
