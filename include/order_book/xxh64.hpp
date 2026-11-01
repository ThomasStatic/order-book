#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace order_book::replay {

    // XXH64, written by hand from the public specification, the one checksum in every file
    // format here. Header-only so a command log's record pays no call: its input is short and
    // its size is known. It reads little-endian on every platform, so a file's checksums are
    // the same everywhere.
    namespace xxh64detail {
        inline constexpr std::uint64_t prime1 = 0x9E3779B185EBCA87ULL;
        inline constexpr std::uint64_t prime2 = 0xC2B2AE3D27D4EB4FULL;
        inline constexpr std::uint64_t prime3 = 0x165667B19E3779F9ULL;
        inline constexpr std::uint64_t prime4 = 0x85EBCA77C2B2AE63ULL;
        inline constexpr std::uint64_t prime5 = 0x27D4EB2F165667C5ULL;

        inline std::uint64_t load64(const std::byte* bytes) noexcept {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            if constexpr (std::endian::native == std::endian::big) {
                value = ((value & 0x00000000000000FFULL) << 56) | ((value & 0x000000000000FF00ULL) << 40) |
                        ((value & 0x0000000000FF0000ULL) << 24) | ((value & 0x00000000FF000000ULL) << 8) |
                        ((value & 0x000000FF00000000ULL) >> 8) | ((value & 0x0000FF0000000000ULL) >> 24) |
                        ((value & 0x00FF000000000000ULL) >> 40) | ((value & 0xFF00000000000000ULL) >> 56);
            }
            return value;
        }

        inline std::uint32_t load32(const std::byte* bytes) noexcept {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            if constexpr (std::endian::native == std::endian::big) {
                value = ((value & 0x000000FFU) << 24) | ((value & 0x0000FF00U) << 8) | ((value & 0x00FF0000U) >> 8) |
                        ((value & 0xFF000000U) >> 24);
            }
            return value;
        }

        inline std::uint64_t round(std::uint64_t accumulator, std::uint64_t input) noexcept {
            return std::rotl(accumulator + input * prime2, 31) * prime1;
        }

        inline std::uint64_t mergeRound(std::uint64_t hash, std::uint64_t accumulator) noexcept {
            return (hash ^ round(0, accumulator)) * prime1 + prime4;
        }
    }  // namespace xxh64detail

    inline std::uint64_t xxh64(std::span<const std::byte> input, std::uint64_t seed = 0) noexcept {
        using namespace xxh64detail;
        const std::byte* at = input.data();
        const std::byte* const end = at + input.size();
        std::uint64_t hash = 0;

        if (input.size() >= 32) {
            std::uint64_t v1 = seed + prime1 + prime2;
            std::uint64_t v2 = seed + prime2;
            std::uint64_t v3 = seed;
            std::uint64_t v4 = seed - prime1;
            do {
                v1 = round(v1, load64(at));
                v2 = round(v2, load64(at + 8));
                v3 = round(v3, load64(at + 16));
                v4 = round(v4, load64(at + 24));
                at += 32;
            } while (end - at >= 32);
            hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
            hash = mergeRound(hash, v1);
            hash = mergeRound(hash, v2);
            hash = mergeRound(hash, v3);
            hash = mergeRound(hash, v4);
        } else {
            hash = seed + prime5;
        }

        hash += static_cast<std::uint64_t>(input.size());
        while (end - at >= 8) {
            hash = std::rotl(hash ^ round(0, load64(at)), 27) * prime1 + prime4;
            at += 8;
        }
        if (end - at >= 4) {
            hash = std::rotl(hash ^ (static_cast<std::uint64_t>(load32(at)) * prime1), 23) * prime2 + prime3;
            at += 4;
        }
        while (at != end) {
            hash = std::rotl(hash ^ (static_cast<std::uint64_t>(*at) * prime5), 11) * prime1;
            ++at;
        }

        hash ^= hash >> 33;
        hash *= prime2;
        hash ^= hash >> 29;
        hash *= prime3;
        hash ^= hash >> 32;
        return hash;
    }

}  // namespace order_book::replay
