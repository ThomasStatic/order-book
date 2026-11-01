#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace order_book::replay {

    // An instrument's only identity in any format (docs/adr/0007): exactly 8 bytes, 1 to 8
    // characters of A-Z, 0-9, '.', or '-', right-padded with spaces. Held as one 64-bit word, so
    // comparing two is one comparison. Ordering is by bytes, whatever the machine's byte order.
    //
    // A default-constructed Symbol is the absence of one, as a v1 or v2 record has: no text
    // parses to it, and no format ever writes it.
    class Symbol {
    public:
        static constexpr std::size_t size = 8;

        constexpr Symbol() noexcept = default;

        // The symbol this text names, or nothing: empty, too long, or a character outside the
        // set. Padding is added here, so a space in the text is refused.
        static std::optional<Symbol> parse(std::string_view text) noexcept {
            if(text.empty() || text.size() > size) {
                return std::nullopt;
            }
            std::array<char, size> bytes;
            bytes.fill(' ');
            for(std::size_t i = 0; i < text.size(); ++i) {
                if(!isSymbolCharacter(text[i])) {
                    return std::nullopt;
                }
                bytes[i] = text[i];
            }
            return fromValidBytes(bytes);
        }

        // The symbol these stored bytes hold, or nothing: 1 to 8 valid characters, then only
        // spaces.
        static std::optional<Symbol> fromBytes(std::span<const char, size> stored) noexcept {
            std::size_t length = size;
            while(length > 0 && stored[length - 1] == ' ') {
                --length;
            }
            const std::optional<Symbol> symbol = parse(std::string_view(stored.data(), length));
            return symbol;
        }

        // The 8 bytes as any format stores them.
        std::array<char, size> bytes() const noexcept {
            std::array<char, size> stored;
            std::memcpy(stored.data(), &word, size);
            return stored;
        }

        // Without its padding.
        std::string text() const {
            const std::array<char, size> stored = bytes();
            std::size_t length = size;
            while(length > 0 && (stored[length - 1] == ' ' || stored[length - 1] == '\0')) {
                --length;
            }
            return std::string(stored.data(), length);
        }

        bool empty() const noexcept { return word == 0; }

        bool operator==(const Symbol&) const noexcept = default;
        std::strong_ordering operator<=>(const Symbol& other) const noexcept {
            return std::memcmp(&word, &other.word, size) <=> 0;
        }

    private:
        static constexpr bool isSymbolCharacter(char character) noexcept {
            return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                   character == '.' || character == '-';
        }

        static Symbol fromValidBytes(const std::array<char, size>& stored) noexcept {
            Symbol symbol;
            std::memcpy(&symbol.word, stored.data(), size);
            return symbol;
        }

        std::uint64_t word = 0;
    };

}  // namespace order_book::replay
