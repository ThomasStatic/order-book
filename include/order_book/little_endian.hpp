#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Little-endian fields in a byte span, for every format that has some. The caller checks sizes.
namespace order_book::little_endian {

    inline void store8(std::span<std::byte> out, std::size_t offset, std::uint8_t value) noexcept {
        out[offset] = static_cast<std::byte>(value);
    }

    inline void store16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
        store8(out, offset, static_cast<std::uint8_t>(value & 0xFFU));
        store8(out, offset + 1, static_cast<std::uint8_t>(value >> 8));
    }

    // A memcpy where the machine is little-endian: MSVC does not merge a loop of byte stores,
    // and a record's encode and read run per command.
    template <typename Word>
    void storeWord(std::span<std::byte> out, std::size_t offset, Word value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(out.data() + offset, &value, sizeof(value));
        } else {
            for (std::size_t i = 0; i < sizeof(value); ++i) {
                store8(out, offset + i, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU));
            }
        }
    }

    inline void store32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
        storeWord(out, offset, value);
    }

    inline void store64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
        storeWord(out, offset, value);
    }

    inline std::uint8_t load8(std::span<const std::byte> in, std::size_t offset) noexcept {
        return static_cast<std::uint8_t>(in[offset]);
    }

    inline std::uint16_t load16(std::span<const std::byte> in, std::size_t offset) noexcept {
        return static_cast<std::uint16_t>(load8(in, offset) | (load8(in, offset + 1) << 8));
    }

    template <typename Word>
    Word loadWord(std::span<const std::byte> in, std::size_t offset) noexcept {
        Word value = 0;
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(&value, in.data() + offset, sizeof(value));
        } else {
            for (std::size_t i = 0; i < sizeof(value); ++i) {
                value = static_cast<Word>(value | (static_cast<Word>(load8(in, offset + i)) << (i * 8)));
            }
        }
        return value;
    }

    inline std::uint32_t load32(std::span<const std::byte> in, std::size_t offset) noexcept {
        return loadWord<std::uint32_t>(in, offset);
    }

    inline std::uint64_t load64(std::span<const std::byte> in, std::size_t offset) noexcept {
        return loadWord<std::uint64_t>(in, offset);
    }

}  // namespace order_book::little_endian
