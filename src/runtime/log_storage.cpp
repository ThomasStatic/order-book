#include "order_book/runtime/log_storage.hpp"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace order_book::runtime {

    std::string logFileName(std::uint32_t shard, std::uint64_t firstPosition) {
        char name[48];
        std::snprintf(name, sizeof name, "log-%04u-%020llu.oblog", static_cast<unsigned int>(shard),
                      static_cast<unsigned long long>(firstPosition));
        return name;
    }

    namespace {

#ifdef _WIN32
        using NativeError = DWORD;
        NativeError lastError() noexcept { return GetLastError(); }
#else
        using NativeError = int;
        NativeError lastError() noexcept { return errno; }
#endif

        LogStorageError failure(const std::string& what, const std::string& path, NativeError error) {
            return LogStorageError(LogStorageError::Kind::FAILED,
                                   "cannot " + what + " " + path + ": " + std::system_category().message(static_cast<int>(error)));
        }

        // The name a log file is known by must survive a machine crash, or its records go with
        // it. On Windows MOVEFILE_WRITE_THROUGH returns only once the move is on the drive, and on
        // Linux it takes a flush of the directory.
        void moveInDirectory(const std::filesystem::path& directory, const std::string& from, const std::string& to) {
            const std::filesystem::path source = directory / from;
            const std::filesystem::path target = directory / to;
#ifdef _WIN32
            if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const DWORD error = lastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                    throw LogStorageError(LogStorageError::Kind::EXISTS, target.string() + " already exists");
                }
                throw failure("rename " + source.string() + " to", target.string(), error);
            }
#else
#ifdef RENAME_NOREPLACE
            int renamed = ::renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE);
            if (renamed != 0 && (errno == EINVAL || errno == ENOSYS)) {
                // A file system that can't refuse to replace: the start's check that no log is
                // there, and every name being new to this run, keep the target free.
                renamed = ::rename(source.c_str(), target.c_str());
            }
#else
            const int renamed = ::rename(source.c_str(), target.c_str());
#endif
            if (renamed != 0) {
                const int error = lastError();
                if (error == EEXIST) {
                    throw LogStorageError(LogStorageError::Kind::EXISTS, target.string() + " already exists");
                }
                throw failure("rename " + source.string() + " to", target.string(), error);
            }
            const int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            const int flushed = parent < 0 ? -1 : ::fsync(parent);
            const int error = lastError();
            if (parent >= 0) {
                ::close(parent);
            }
            if (flushed != 0) {
                throw failure("flush the directory of", target.string(), error);
            }
#endif
        }

#ifdef _WIN32
        class NativeFile final : public LogFile {
        public:
            NativeFile(std::filesystem::path in, std::string named)
                : directory(std::move(in)), name(std::move(named)), path((directory / name).string()) {}
            // Only once the file is made, so nothing left to allocate can strand it.
            void adopt(HANDLE opened) noexcept { handle = opened; }
            ~NativeFile() override {
                if (handle != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle);
                }
            }

            void write(std::uint64_t offset, std::span<const std::byte> bytes) override {
                while (!bytes.empty()) {
                    OVERLAPPED at{};
                    at.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
                    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
                    const DWORD wanted = bytes.size() > 0x40000000U ? 0x40000000U : static_cast<DWORD>(bytes.size());
                    DWORD written = 0;
                    if (!WriteFile(handle, bytes.data(), wanted, &written, &at)) {
                        throw failure("write", path, lastError());
                    }
                    offset += written;
                    bytes = bytes.subspan(written);
                }
            }

            void flush() override {
                if (!FlushFileBuffers(handle)) {
                    throw failure("flush", path, lastError());
                }
            }

            // Moves the end without writing what lies before it: NTFS reads past a file's
            // valid data as zeros, and zero-fills only when a write lands there.
            void extend(std::uint64_t size) override { setEnd(size, "extend"); }
            void cut(std::uint64_t size) override { setEnd(size, "cut"); }

            void close() override {
                const HANDLE closing = std::exchange(handle, INVALID_HANDLE_VALUE);
                if (!CloseHandle(closing)) {
                    throw failure("close", path, lastError());
                }
            }

            // Opened sharing delete, so it moves while this handle holds it.
            void rename(const std::string& to) override {
                std::string moved = (directory / to).string();
                moveInDirectory(directory, name, to);
                name = to;
                path = std::move(moved);
            }

        private:
            void setEnd(std::uint64_t size, const char* what) {
                FILE_END_OF_FILE_INFO end{};
                end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
                if (!SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof end)) {
                    throw failure(what, path, lastError());
                }
            }

            HANDLE handle = INVALID_HANDLE_VALUE;
            std::filesystem::path directory;
            std::string name;
            std::string path;  // for messages
        };
#else
        class NativeFile final : public LogFile {
        public:
            NativeFile(std::filesystem::path in, std::string named)
                : directory(std::move(in)), name(std::move(named)), path((directory / name).string()) {}
            // Only once the file is made, so nothing left to allocate can strand it.
            void adopt(int opened) noexcept { descriptor = opened; }
            ~NativeFile() override {
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
            }

            void write(std::uint64_t offset, std::span<const std::byte> bytes) override {
                while (!bytes.empty()) {
                    const ssize_t written = ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        throw failure("write", path, lastError());
                    }
                    offset += static_cast<std::uint64_t>(written);
                    bytes = bytes.subspan(static_cast<std::size_t>(written));
                }
            }

            void flush() override {
                if (::fsync(descriptor) != 0) {
                    throw failure("flush", path, lastError());
                }
            }

            // Allocates the space without writing it, so a full drive is known now, while the
            // current file still has room, and Linux reads the unwritten extent as zeros. A file
            // system with no fallocate gets a sparse end instead, which a full drive fails later.
            void extend(std::uint64_t size) override {
                for (;;) {
                    if (::fallocate(descriptor, 0, 0, static_cast<off_t>(size)) == 0) {
                        return;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EOPNOTSUPP || errno == ENOSYS) {
                        setEnd(size, "extend");
                        return;
                    }
                    throw failure("extend", path, lastError());
                }
            }
            void cut(std::uint64_t size) override { setEnd(size, "cut"); }

            void close() override {
                const int closing = std::exchange(descriptor, -1);
                if (::close(closing) != 0) {
                    throw failure("close", path, lastError());
                }
            }

            void rename(const std::string& to) override {
                std::string moved = (directory / to).string();
                moveInDirectory(directory, name, to);
                name = to;
                path = std::move(moved);
            }

        private:
            void setEnd(std::uint64_t size, const char* what) {
                if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
                    throw failure(what, path, lastError());
                }
            }

            int descriptor = -1;
            std::filesystem::path directory;
            std::string name;
            std::string path;  // for messages
        };
#endif

    }  // namespace

    FileLogStorage::FileLogStorage(std::filesystem::path logDirectory) : directory(std::move(logDirectory)) {}

    std::string FileLogStorage::describe(const std::string& name) const {
        return (directory / name).string();
    }

    // Everything that allocates comes before the file is made, so a create that throws leaves no
    // file behind to refuse the next start.
    std::unique_ptr<LogFile> FileLogStorage::create(const std::string& name, Durability durability) {
        const std::filesystem::path path = directory / name;
        auto file = std::make_unique<NativeFile>(directory, name);
#ifdef _WIN32
        const DWORD flags = FILE_ATTRIBUTE_NORMAL | (durability == Durability::MACHINE ? FILE_FLAG_WRITE_THROUGH : 0);
        // Sharing delete lets the file be renamed while this handle holds it.
        const HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = lastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                throw LogStorageError(LogStorageError::Kind::EXISTS, path.string() + " already exists");
            }
            throw failure("create", path.string(), error);
        }
        file->adopt(handle);
        return file;
#else
        const int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | (durability == Durability::MACHINE ? O_DSYNC : 0);
        const int descriptor = ::open(path.c_str(), flags, 0644);
        if (descriptor < 0) {
            const int error = lastError();
            if (error == EEXIST) {
                throw LogStorageError(LogStorageError::Kind::EXISTS, path.string() + " already exists");
            }
            throw failure("create", path.string(), error);
        }
        file->adopt(descriptor);
        return file;
#endif
    }

    void FileLogStorage::remove(const std::string& name) {
        std::error_code error;
        if (!std::filesystem::remove(directory / name, error) && error) {
            throw LogStorageError(LogStorageError::Kind::FAILED, "cannot remove " + describe(name) + ": " + error.message());
        }
    }

    class InMemoryLogStorage::OpenFile final : public LogFile {
    public:
        OpenFile(InMemoryLogStorage& owner, std::shared_ptr<File> opened, std::string named)
            : storage(owner), file(std::move(opened)), name(std::move(named)) {}

        void write(std::uint64_t offset, std::span<const std::byte> bytes) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (storage.writesBeforeFailure) {
                if (*storage.writesBeforeFailure == 0) {
                    throw LogStorageError(LogStorageError::Kind::FAILED, "cannot write " + name + ": a failure the test asked for");
                }
                --*storage.writesBeforeFailure;
            }
            if (file->bytes.size() < offset + bytes.size()) {
                file->bytes.resize(static_cast<std::size_t>(offset + bytes.size()));
            }
            std::copy(bytes.begin(), bytes.end(), file->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            file->size = std::max<std::uint64_t>(file->size, offset + bytes.size());
            ++file->writes;
        }

        void flush() override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (storage.flushesFail) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot flush " + name + ": a failure the test asked for");
            }
            ++file->flushes;
        }

        void extend(std::uint64_t size) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            file->size = std::max(file->size, size);
            ++file->extends;
        }

        void cut(std::uint64_t size) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (file->bytes.size() > size) {
                file->bytes.resize(static_cast<std::size_t>(size));
            }
            file->size = size;
            ++file->cuts;
        }

        void close() override {
            const std::lock_guard<std::mutex> held(storage.lock);
            file->closed = true;
        }

        void rename(const std::string& to) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (storage.files.contains(to)) {
                throw LogStorageError(LogStorageError::Kind::EXISTS, to + " already exists");
            }
            std::string moved = to;
            storage.files.erase(name);  // a removed file renames into view again, as on Linux
            storage.files.emplace(to, file);
            ++file->renames;
            name = std::move(moved);
        }

    private:
        InMemoryLogStorage& storage;
        std::shared_ptr<File> file;
        std::string name;
    };

    std::unique_ptr<LogFile> InMemoryLogStorage::create(const std::string& name, Durability durability) {
        const std::lock_guard<std::mutex> held(lock);
        if (files.contains(name)) {
            throw LogStorageError(LogStorageError::Kind::EXISTS, name + " already exists");
        }
        if (createsBeforeFailure) {
            if (*createsBeforeFailure == 0) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot create " + name + ": a failure the test asked for");
            }
            --*createsBeforeFailure;
        }
        auto file = std::make_shared<File>();
        file->durability = durability;
        files.emplace(name, file);
        return std::make_unique<OpenFile>(*this, std::move(file), name);
    }

    void InMemoryLogStorage::remove(const std::string& name) {
        const std::lock_guard<std::mutex> held(lock);
        files.erase(name);
    }

    std::string InMemoryLogStorage::describe(const std::string& name) const {
        return name;
    }

    std::optional<InMemoryLogStorage::File> InMemoryLogStorage::file(const std::string& name) const {
        const std::lock_guard<std::mutex> held(lock);
        const auto found = files.find(name);
        if (found == files.end()) {
            return std::nullopt;
        }
        return *found->second;
    }

    std::vector<std::byte> InMemoryLogStorage::read(const std::string& name, std::size_t offset) const {
        const std::lock_guard<std::mutex> held(lock);
        const auto found = files.find(name);
        if (found == files.end() || found->second->bytes.size() <= offset) {
            return {};
        }
        const std::vector<std::byte>& bytes = found->second->bytes;
        return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    }

    std::vector<std::string> InMemoryLogStorage::names() const {
        const std::lock_guard<std::mutex> held(lock);
        std::vector<std::string> all;
        for (const auto& [name, file] : files) {
            all.push_back(name);
        }
        return all;
    }

    void InMemoryLogStorage::failCreateAfter(std::size_t creates) {
        const std::lock_guard<std::mutex> held(lock);
        createsBeforeFailure = creates;
    }

    void InMemoryLogStorage::failWriteAfter(std::size_t writes) {
        const std::lock_guard<std::mutex> held(lock);
        writesBeforeFailure = writes;
    }

    void InMemoryLogStorage::failFlushes() {
        const std::lock_guard<std::mutex> held(lock);
        flushesFail = true;
    }

}  // namespace order_book::runtime
