#include "order_book/runtime/abort_report.hpp"

#include <array>
#include <cstdint>
#include <string_view>

#include "order_book/invariants.hpp"

namespace order_book::runtime {

    namespace {
        // Constant-initialized, so reading it in a signal handler runs no initializer.
        thread_local const Shard* abortingShard = nullptr;

        // Appends to a fixed buffer, dropping what doesn't fit.
        class FixedLine {
        public:
            explicit FixedLine(std::span<char> into) noexcept : buffer(into) {}

            void add(std::string_view text) noexcept {
                for (const char character : text) {
                    if (used == buffer.size()) {
                        return;
                    }
                    buffer[used++] = character;
                }
            }

            void add(std::uint64_t number) noexcept {
                std::array<char, 20> digits{};
                std::size_t count = 0;
                do {
                    digits[count++] = static_cast<char>('0' + number % 10);
                    number /= 10;
                } while (number != 0);
                while (count != 0) {
                    add(std::string_view(&digits[--count], 1));
                }
            }

            std::size_t size() const noexcept { return used; }

        private:
            std::span<char> buffer;
            std::size_t used = 0;
        };
    }  // namespace

    AbortScope::AbortScope(const Shard& shard) noexcept : previous(abortingShard) {
        abortingShard = &shard;
    }

    AbortScope::~AbortScope() {
        abortingShard = previous;
    }

    std::size_t describeAbort(std::span<char> buffer) noexcept {
        FixedLine line(buffer);
        line.add("order_book_engine: aborted");
        if constexpr (invariantChecksAfterCommands) {  // other builds record nothing
            const ShardProgress progress = abortingShard != nullptr ? abortingShard->lastProgress() : ShardProgress{};
            if (abortingShard != nullptr && progress.slot < abortingShard->size()) {
                const std::array<char, Symbol::size> symbol = abortingShard->books().symbol(progress.slot).bytes();
                std::string_view text(symbol.data(), symbol.size());
                text = text.substr(0, text.find(' '));  // without its padding
                line.add(": shard ");
                line.add(abortingShard->books().shard());
                line.add(" symbol ");
                line.add(text);
                line.add(" position ");
                line.add(progress.position);
            }
        }
        line.add("\n");
        return line.size();
    }

}  // namespace order_book::runtime
