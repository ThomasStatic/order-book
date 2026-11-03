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

#ifdef _WIN32
        class NativeFile final : public LogFile {
        public:
            explicit NativeFile(std::string where) : path(std::move(where)) {}
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

            void cut(std::uint64_t size) override {
                FILE_END_OF_FILE_INFO end{};
                end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
                if (!SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof end)) {
                    throw failure("cut", path, lastError());
                }
            }

            void close() override {
                const HANDLE closing = std::exchange(handle, INVALID_HANDLE_VALUE);
                if (!CloseHandle(closing)) {
                    throw failure("close", path, lastError());
                }
            }

        private:
            HANDLE handle = INVALID_HANDLE_VALUE;
            std::string path;
        };
#else
        class NativeFile final : public LogFile {
        public:
            explicit NativeFile(std::string where) : path(std::move(where)) {}
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

            void cut(std::uint64_t size) override {
                if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
                    throw failure("cut", path, lastError());
                }
            }

            void close() override {
                const int closing = std::exchange(descriptor, -1);
                if (::close(closing) != 0) {
                    throw failure("close", path, lastError());
                }
            }

        private:
            int descriptor = -1;
            std::string path;
        };
#endif

    }  // namespace

    FileLogStorage::FileLogStorage(std::filesystem::path logDirectory) : directory(std::move(logDirectory)) {}

    std::string FileLogStorage::describe(const std::string& name) const {
        return (directory / name).string();
    }

    // Everything that allocates comes before the file is made, and a failure after it removes
    // it, so a create that throws leaves no file behind to refuse the next start.
    std::unique_ptr<LogFile> FileLogStorage::create(const std::string& name, Durability durability) {
        const std::filesystem::path path = directory / name;
        auto file = std::make_unique<NativeFile>(path.string());
#ifdef _WIN32
        const DWORD flags = FILE_ATTRIBUTE_NORMAL | (durability == Durability::MACHINE ? FILE_FLAG_WRITE_THROUGH : 0);
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, flags, nullptr);
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
        // The new name itself must survive a machine crash, or the file's records go with it.
        // NTFS journals that, and on Linux it takes a flush of the directory.
        const int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int flushed = parent < 0 ? -1 : ::fsync(parent);
        const int error = lastError();
        if (parent >= 0) {
            ::close(parent);
        }
        if (flushed != 0) {
            file.reset();
            ::unlink(path.c_str());
            throw failure("flush the directory of", path.string(), error);
        }
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
        OpenFile(InMemoryLogStorage& owner, std::string named) : storage(owner), name(std::move(named)) {}

        void write(std::uint64_t offset, std::span<const std::byte> bytes) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (storage.writesBeforeFailure) {
                if (*storage.writesBeforeFailure == 0) {
                    throw LogStorageError(LogStorageError::Kind::FAILED, "cannot write " + name + ": a failure the test asked for");
                }
                --*storage.writesBeforeFailure;
            }
            File& file = storage.files.at(name);
            if (file.bytes.size() < offset + bytes.size()) {
                file.bytes.resize(static_cast<std::size_t>(offset + bytes.size()));
            }
            std::copy(bytes.begin(), bytes.end(), file.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            ++file.writes;
        }

        void flush() override {
            const std::lock_guard<std::mutex> held(storage.lock);
            if (storage.flushesFail) {
                throw LogStorageError(LogStorageError::Kind::FAILED, "cannot flush " + name + ": a failure the test asked for");
            }
            ++storage.files.at(name).flushes;
        }

        void cut(std::uint64_t size) override {
            const std::lock_guard<std::mutex> held(storage.lock);
            File& file = storage.files.at(name);
            file.bytes.resize(static_cast<std::size_t>(size));
            ++file.cuts;
        }

        void close() override {
            const std::lock_guard<std::mutex> held(storage.lock);
            storage.files.at(name).closed = true;
        }

    private:
        InMemoryLogStorage& storage;
        std::string name;
    };

    std::unique_ptr<LogFile> InMemoryLogStorage::create(const std::string& name, Durability durability) {
        const std::lock_guard<std::mutex> held(lock);
        File file;
        file.durability = durability;
        if (!files.emplace(name, std::move(file)).second) {
            throw LogStorageError(LogStorageError::Kind::EXISTS, name + " already exists");
        }
        return std::make_unique<OpenFile>(*this, name);
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
        return found->second;
    }

    std::vector<std::byte> InMemoryLogStorage::read(const std::string& name, std::size_t offset) const {
        const std::lock_guard<std::mutex> held(lock);
        const auto found = files.find(name);
        if (found == files.end() || found->second.bytes.size() <= offset) {
            return {};
        }
        return std::vector<std::byte>(found->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset), found->second.bytes.end());
    }

    std::vector<std::string> InMemoryLogStorage::names() const {
        const std::lock_guard<std::mutex> held(lock);
        std::vector<std::string> all;
        for (const auto& [name, file] : files) {
            all.push_back(name);
        }
        return all;
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
