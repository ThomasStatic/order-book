#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "order_book/symbol.hpp"

namespace {

using order_book::replay::Symbol;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void testExactly8Bytes() {
    static_assert(sizeof(Symbol) == 8, "a symbol is one 64-bit word");
    static_assert(std::is_trivially_copyable_v<Symbol>, "a symbol travels in a ring element");
    const std::array<char, 8> bytes = Symbol::parse("AAPL")->bytes();
    expect(std::memcmp(bytes.data(), "AAPL    ", 8) == 0, "a symbol is right-padded with spaces");
    expect(std::memcmp(Symbol::parse("ABCD1234")->bytes().data(), "ABCD1234", 8) == 0, "eight characters need no padding");
}

void testValidCharacters() {
    for (const char* text : {"A", "Z", "0", "9", ".", "-", "BRK.B", "ES-Z26", "A1B2C3D4"}) {
        const std::optional<Symbol> symbol = Symbol::parse(text);
        expect(symbol.has_value(), std::string("valid: ") + text);
        expect(symbol->text() == text, std::string("text comes back without its padding: ") + text);
    }
}

void testInvalidText() {
    for (const char* text : {"", "ABCDEFGHI", "aapl", "AA PL", " AAPL", "AAPL ", "AA_PL", "AA/PL", "AAPL\n"}) {
        expect(!Symbol::parse(text).has_value(), std::string("invalid: '") + text + "'");
    }
}

void testFromBytes() {
    const auto read = [](const char (&stored)[9]) { return Symbol::fromBytes(std::span<const char, 8>(stored, 8)); };
    expect(read("AAPL    ") == Symbol::parse("AAPL"), "stored bytes read back");
    expect(read("ABCD1234") == Symbol::parse("ABCD1234"), "a full symbol reads back");
    expect(!read("        ").has_value(), "all spaces name nothing");
    expect(!read("AA PL   ").has_value(), "a space inside is refused");
    expect(!read(" AAPL   ").has_value(), "a leading space is refused");
    expect(!read("aapl    ").has_value(), "lower case is refused");
}

void testComparison() {
    expect(Symbol::parse("AAPL") == Symbol::parse("AAPL"), "equal text is equal");
    expect(Symbol::parse("AAPL") != Symbol::parse("AAPM"), "the last byte counts");
    expect(Symbol::parse("A") != Symbol::parse("AA"), "padding counts");
}

// By bytes, not by the word: on a little-endian machine the word's top byte is the last character.
void testOrderIsByBytes() {
    std::vector<Symbol> symbols{*Symbol::parse("ZZZ"), *Symbol::parse("AAPL"), *Symbol::parse("BA"),
                                *Symbol::parse("B"),   *Symbol::parse("AB"),   *Symbol::parse("9"), *Symbol::parse("-")};
    std::sort(symbols.begin(), symbols.end());
    std::vector<std::string> texts;
    for (const Symbol& symbol : symbols) {
        texts.push_back(symbol.text());
    }
    // '-' 0x2D, '9' 0x39, 'A' 0x41; and a space, 0x20, sorts before any character.
    expect((texts == std::vector<std::string>{"-", "9", "AAPL", "AB", "B", "BA", "ZZZ"}), "symbols sort by bytes");
}

void testAbsentSymbol() {
    const Symbol none;
    expect(none.empty(), "a default symbol is none");
    expect(!Symbol::parse("A")->empty(), "a parsed symbol is not");
    expect(none.text().empty(), "none has no text");
}

}  // namespace

int main() {
    testExactly8Bytes();
    testValidCharacters();
    testInvalidText();
    testFromBytes();
    testComparison();
    testOrderIsByBytes();
    testAbsentSymbol();
    std::cout << "symbol tests passed\n";
    return EXIT_SUCCESS;
}
