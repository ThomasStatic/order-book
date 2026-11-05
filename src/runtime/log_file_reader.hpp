#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <variant>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/symbol.hpp"

// Reading a log file through LogStorage with CommandLogReader, shared by recovery and roll
// forward. Private to order_book_runtime.
namespace order_book::runtime::log_reading {

        // A LogFile as a stream, for CommandLogReader, reading at most limit bytes. A storage
        // failure escapes the reader as the LogStorageError it was, since its stream rethrows.
        class LogFileBuffer final : public std::streambuf {
        public:
            LogFileBuffer(LogFile& source, std::uint64_t limit) : file(source), end(limit), held(64 * 1024) {}

        protected:
            int_type underflow() override {
                if (gptr() < egptr()) {
                    return traits_type::to_int_type(*gptr());
                }
                const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(held.size(), end - std::min(end, offset)));
                const std::size_t got = wanted == 0 ? 0 : file.read(offset, std::as_writable_bytes(std::span(held.data(), wanted)));
                if (got == 0) {
                    return traits_type::eof();
                }
                offset += got;
                setg(held.data(), held.data(), held.data() + got);
                return traits_type::to_int_type(*gptr());
            }

        private:
            LogFile& file;
            std::uint64_t end;
            std::uint64_t offset = 0;
            std::vector<char> held;
        };

        // One log file opened and read through CommandLogReader, whole or only its header.
        class LogFileReader {
        public:
            LogFileReader(LogStorage& storage, const std::string& name, std::uint64_t limit = UINT64_MAX)
                : file(storage.open(name)), buffer(*file, limit), input(&buffer), reader(input) {
                input.exceptions(std::ios::badbit);
            }

            replay::ReadResult next() { return reader.next(); }
            const replay::LogHeader* header() const { return reader.header(); }

        private:
            std::unique_ptr<LogFile> file;
            LogFileBuffer buffer;
            std::istream input;
            replay::CommandLogReader reader;
        };

        inline std::uint32_t load32(std::span<const std::byte> bytes, std::size_t at) {
            std::uint32_t value = 0;
            for (std::size_t i = 4; i-- > 0;) {
                value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at + i]);
            }
            return value;
        }

        // What a header takes in its file, symbols and checksum included: where records start.
        inline std::uint64_t wholeHeaderBytes(const replay::LogHeader& header) {
            return std::uint64_t{header.headerBytes} + header.symbols.size() * Symbol::size + sizeof(std::uint64_t);
        }

        struct FileHeader {
            std::variant<replay::LogHeader, replay::ParseError> parsed;
            std::uint64_t bytes = 0;  // the whole header's
        };

        // Only the header, so a file's zero tail isn't read to find a torn tail nobody asked about.
        inline FileHeader readHeader(LogStorage& storage, const std::string& name) {
            std::uint64_t limit = replay::logHeaderBytes;
            {
                const std::unique_ptr<LogFile> file = storage.open(name);
                std::array<std::byte, replay::logHeaderBytes> fixed{};
                if (file->read(0, fixed) == fixed.size()) {
                    limit = std::min<std::uint64_t>(file->size(), std::uint64_t{load32(fixed, 16)} +
                                                                      std::uint64_t{load32(fixed, 48)} * Symbol::size +
                                                                      sizeof(std::uint64_t));
                }
            }
            LogFileReader reader(storage, name, limit);
            const replay::ReadResult result = reader.next();
            if (reader.header() == nullptr) {
                return FileHeader{std::get<replay::ParseError>(result), 0};
            }
            return FileHeader{*reader.header(), wholeHeaderBytes(*reader.header())};
        }

}  // namespace order_book::runtime::log_reading
