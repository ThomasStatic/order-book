#include "order_book/runtime/page_log_storage.hpp"

#include <algorithm>
#include <utility>

namespace order_book::runtime {

    namespace {
        void place(std::vector<std::byte>& into, std::uint64_t offset, std::span<const std::byte> bytes) {
            if (into.size() < offset + bytes.size()) {
                into.resize(static_cast<std::size_t>(offset + bytes.size()));
            }
            std::copy(bytes.begin(), bytes.end(), into.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }  // namespace

    // One handle on a file. Each change is made, then told, under the store's lock, so the
    // callback sees changes in the order they were made, and only those to a file the store
    // still holds under the handle's name: a removed file's writes reach no one.
    class PageLogStorage::OpenFile final : public LogFile {
    public:
        OpenFile(PageLogStorage& owner, std::shared_ptr<File> opened, std::string named)
            : storage(owner), file(std::move(opened)), name(std::move(named)) {}

        std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
            const std::lock_guard<std::mutex> held(storage.guard);
            if (offset >= file->size) {
                return 0;
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), file->size - offset));
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t at = offset + i;
                out[i] = at < file->bytes.size() ? file->bytes[static_cast<std::size_t>(at)] : std::byte{0};
            }
            return count;
        }

        std::uint64_t size() override {
            const std::lock_guard<std::mutex> held(storage.guard);
            return file->size;
        }

        void write(std::uint64_t offset, std::span<const std::byte> bytes) override {
            const std::lock_guard<std::mutex> held(storage.guard);
            place(file->bytes, offset, bytes);
            file->size = std::max<std::uint64_t>(file->size, offset + bytes.size());
            Change change;
            change.kind = Change::Kind::WRITE;
            change.offset = offset;
            change.bytes = bytes;
            tell(std::move(change));
        }

        void flush() override {}  // process durability: nothing to tell

        void extend(std::uint64_t size) override {
            const std::lock_guard<std::mutex> held(storage.guard);
            if (size <= file->size) {
                return;  // never shortens, so nothing changed
            }
            file->size = size;
            Change change;
            change.kind = Change::Kind::EXTEND;
            change.size = size;
            tell(std::move(change));
        }

        void cut(std::uint64_t size) override {
            const std::lock_guard<std::mutex> held(storage.guard);
            if (file->bytes.size() > size) {
                file->bytes.resize(static_cast<std::size_t>(size));
            }
            file->size = size;
            Change change;
            change.kind = Change::Kind::CUT;
            change.size = size;
            tell(std::move(change));
        }

        void close() override {}

        void rename(const std::string& to) override { moveTo(to, false); }
        void replace(const std::string& to) override { moveTo(to, true); }

    private:
        bool current() const {
            const auto found = storage.stored.find(name);
            return found != storage.stored.end() && found->second == file;
        }

        // Under the store's lock.
        void tell(Change change) {
            if (!current()) {
                return;
            }
            change.name = name;
            storage.onChange(change);
        }

        void moveTo(const std::string& to, bool replacing) {
            const std::lock_guard<std::mutex> held(storage.guard);
            if (!current()) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot rename " + name + ": it was removed");
            }
            if (storage.stored.contains(to)) {
                if (!replacing) {
                    throw LogStorageError(LogStorageError::Kind::EXISTS, to + " already exists");
                }
                storage.stored.erase(to);
            }
            storage.stored.erase(name);
            storage.stored.insert_or_assign(to, file);
            Change change;
            change.kind = replacing ? Change::Kind::REPLACE : Change::Kind::RENAME;
            change.name = name;
            change.to = to;
            name = to;
            storage.onChange(change);
        }

        PageLogStorage& storage;
        std::shared_ptr<File> file;
        std::string name;
    };

    class PageLogStorage::HeldLock final : public StorageLock {
    public:
        explicit HeldLock(PageLogStorage& owner) noexcept : storage(owner) {}
        ~HeldLock() override {
            if (owns) {
                const std::lock_guard<std::mutex> held(storage.guard);
                storage.locked = false;
            }
        }
        bool owns = false;  // once it took the lock

    private:
        PageLogStorage& storage;
    };

    PageLogStorage::PageLogStorage(std::function<void(const Change&)> changes, std::vector<StoredFile> from) : onChange(std::move(changes)) {
        if (!onChange) {
            throw std::invalid_argument("a PageLogStorage needs somewhere to tell its changes");
        }
        for (StoredFile& given : from) {
            auto file = std::make_shared<File>();
            file->size = given.bytes.size();
            file->bytes = std::move(given.bytes);
            stored.insert_or_assign(std::move(given.name), std::move(file));
        }
    }

    std::unique_ptr<StorageLock> PageLogStorage::lock() {
        auto taken = std::make_unique<HeldLock>(*this);
        const std::lock_guard<std::mutex> lockHeld(guard);
        if (locked) {
            throw LogStorageError(LogStorageError::Kind::LOCKED, "the page store is locked: the engine is using it");
        }
        locked = true;
        taken->owns = true;
        return taken;
    }

    std::vector<std::string> PageLogStorage::names() const {
        const std::lock_guard<std::mutex> lockHeld(guard);
        std::vector<std::string> all;
        all.reserve(stored.size());
        for (const auto& entry : stored) {
            all.push_back(entry.first);  // a map's order is the sorted order
        }
        return all;
    }

    std::unique_ptr<LogFile> PageLogStorage::open(const std::string& name) {
        const std::lock_guard<std::mutex> lockHeld(guard);
        const auto found = stored.find(name);
        if (found == stored.end()) {
            throw LogStorageError(LogStorageError::Kind::FAILED, "cannot open " + name + ": no such file");
        }
        return std::make_unique<OpenFile>(*this, found->second, name);
    }

    std::unique_ptr<LogFile> PageLogStorage::create(const std::string& name, Durability) {
        auto file = std::make_shared<File>();
        auto opened = std::make_unique<OpenFile>(*this, file, name);
        const std::lock_guard<std::mutex> lockHeld(guard);
        if (stored.contains(name)) {
            throw LogStorageError(LogStorageError::Kind::EXISTS, name + " already exists");
        }
        stored.emplace(name, std::move(file));
        Change change;
        change.kind = Change::Kind::CREATE;
        change.name = name;
        onChange(change);
        return opened;
    }

    void PageLogStorage::remove(const std::string& name) {
        const std::lock_guard<std::mutex> lockHeld(guard);
        if (stored.erase(name) == 0) {
            throw LogStorageError(LogStorageError::Kind::FAILED, "cannot remove " + name + ": no such file");
        }
        Change change;
        change.kind = Change::Kind::REMOVE;
        change.name = name;
        onChange(change);
    }

    std::string PageLogStorage::describe(const std::string& name) const {
        return name;
    }

    std::vector<PageLogStorage::StoredFile> PageLogStorage::files() const {
        const std::lock_guard<std::mutex> lockHeld(guard);
        std::vector<StoredFile> all;
        all.reserve(stored.size());
        for (const auto& [name, file] : stored) {
            std::vector<std::byte> bytes = file->bytes;
            bytes.resize(static_cast<std::size_t>(file->size));
            all.push_back(StoredFile{name, std::move(bytes)});
        }
        return all;
    }

    void PageLogStorage::apply(std::map<std::string, std::vector<std::byte>>& store, const Change& change) {
        if (change.kind == Change::Kind::CREATE) {
            store[change.name].clear();
            return;
        }
        const auto found = store.find(change.name);
        if (found == store.end()) {
            return;
        }
        std::vector<std::byte>& bytes = found->second;
        switch (change.kind) {
            case Change::Kind::WRITE:
                place(bytes, change.offset, change.bytes);
                break;
            case Change::Kind::EXTEND:
                bytes.resize(std::max(bytes.size(), static_cast<std::size_t>(change.size)));
                break;
            case Change::Kind::CUT:
                bytes.resize(static_cast<std::size_t>(change.size));
                break;
            case Change::Kind::RENAME:
            case Change::Kind::REPLACE: {
                std::vector<std::byte> moved = std::move(bytes);
                store.erase(found);
                store.insert_or_assign(change.to, std::move(moved));
                break;
            }
            case Change::Kind::REMOVE:
                store.erase(found);
                break;
            case Change::Kind::CREATE:
                break;
        }
    }

}  // namespace order_book::runtime
