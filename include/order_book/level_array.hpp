#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "node_pool.hpp"
#include "types.hpp"

namespace order_book {

    // One side's levels: a sorted array of {price, pointer to level}, best price at the back,
    // with each level object in the book's node pool. It reads like the std::map it replaced:
    // iteration runs best price first, and an element has .first and .second.
    //
    // Matching works at the best price, which is the array's last element: reaching it costs
    // one load, and removing it costs nothing else. Finding a price gallops from that end and
    // then bisects, so a price near the touch stays in the last cache line. Because the array
    // holds pointers, a level never moves, so a Level* stays valid until that level is erased.
    //
    // The cost is a new level far from the touch, which shifts every level better than it, and
    // any client can place one on purpose. So level insertion is not O(log L): see ADR 0015.
    //
    // A level takes its block from the pool, or from the heap with no pool. Iterators are
    // invalidated by any insertion or erasure. A moved-from array is empty and has no pool, so
    // it holds nothing in the pool its successor owns, and moving cannot throw.
    template <typename Level, typename Better>
    class LevelArray {
    private:
        // Test-only access to internals, to put levels out of order; defined in tests.
        friend struct OrderBookTestAccess;

        struct Entry {
            Price price;
            Level* level;
        };

    public:
        // What dereferencing an iterator gives, in place of a map's pair.
        template <bool IsConst>
        struct Element {
            Price first;
            std::conditional_t<IsConst, const Level&, Level&> second;
        };

        template <bool IsConst>
        class Iterator {
        public:
            struct Arrow {
                Element<IsConst> element;
                Element<IsConst>* operator->() noexcept { return &element; }
            };

            using iterator_category = std::forward_iterator_tag;
            using value_type = Element<IsConst>;
            using difference_type = std::ptrdiff_t;
            using pointer = Arrow;
            using reference = Element<IsConst>;

            Iterator() = default;

            reference operator*() const noexcept {
                const Entry& entry = *(position - 1);
                return {entry.price, *entry.level};
            }
            pointer operator->() const noexcept { return Arrow{**this}; }

            Iterator& operator++() noexcept {
                --position;
                return *this;
            }
            Iterator operator++(int) noexcept {
                Iterator before = *this;
                --position;
                return before;
            }

            bool operator==(const Iterator& other) const noexcept { return position == other.position; }

            operator Iterator<true>() const noexcept
                requires(!IsConst)
            {
                return Iterator<true>(position);
            }

        private:
            friend class LevelArray;
            template <bool>
            friend class Iterator;

            explicit Iterator(const Entry* afterEntry) noexcept : position(afterEntry) {}

            // Just past the entry it names: iteration runs from the array's back to its front,
            // and one past the back is a pointer that may be formed where one before the front
            // is not.
            const Entry* position = nullptr;
        };

        using iterator = Iterator<false>;
        using const_iterator = Iterator<true>;

        explicit LevelArray(NodePool* nodePool = nullptr) noexcept(noexcept(std::vector<Entry>())) : pool(nodePool) {}
        LevelArray(const LevelArray&) = delete;
        LevelArray& operator=(const LevelArray&) = delete;

        LevelArray(LevelArray&& other) noexcept
            : entries(std::move(other.entries)),
              pool(std::exchange(other.pool, nullptr)),
              growthCount(std::exchange(other.growthCount, 0)) {
            other.entries.clear();
        }

        // Levels this array still holds are released into its own pool first.
        LevelArray& operator=(LevelArray&& other) noexcept {
            if(this != &other) {
                clear();
                entries = std::move(other.entries);
                other.entries.clear();
                pool = std::exchange(other.pool, nullptr);
                growthCount = std::exchange(other.growthCount, 0);
            }
            return *this;
        }

        ~LevelArray() { clear(); }

        iterator begin() noexcept { return iterator(entries.data() + entries.size()); }
        iterator end() noexcept { return iterator(entries.data()); }
        const_iterator begin() const noexcept { return const_iterator(entries.data() + entries.size()); }
        const_iterator end() const noexcept { return const_iterator(entries.data()); }

        std::size_t size() const noexcept { return entries.size(); }
        bool empty() const noexcept { return entries.empty(); }
        Better key_comp() const noexcept { return Better{}; }

        // Room for this many levels in the array; the level objects themselves are the pool's.
        void reserve(std::size_t levels) { entries.reserve(levels); }
        // How many times a new level has grown the array; reserve is not counted.
        std::uint64_t growths() const noexcept { return growthCount; }

        iterator find(Price price) noexcept {
            const std::size_t index = locate(price);
            return holds(index, price) ? atIndex(index) : end();
        }
        const_iterator find(Price price) const noexcept {
            const std::size_t index = locate(price);
            return holds(index, price) ? const_iterator(atIndex(index)) : end();
        }

        bool contains(Price price) const noexcept { return holds(locate(price), price); }

        // Throws std::out_of_range if no level has this price.
        Level& at(Price price) {
            const std::size_t index = locate(price);
            if(!holds(index, price)) {
                throw std::out_of_range("no price level at this price");
            }
            return *entries[index].level;
        }
        const Level& at(Price price) const { return const_cast<LevelArray*>(this)->at(price); }

        // The level at this price, built from `arguments` if there is none, and whether it was
        // built. All that can fail comes first, so a throw changes nothing.
        template <typename... Arguments>
        std::pair<iterator, bool> try_emplace(Price price, Arguments&&... arguments) {
            const std::size_t index = locate(price);
            if(holds(index, price)) {
                return {atIndex(index), false};
            }
            if(entries.size() == entries.capacity()) {
                entries.reserve(entries.empty() ? minimumCapacity : entries.size() * 2);
                ++growthCount;
            }
            PoolAllocator<Level> allocator(pool);
            Level* const level = allocator.allocate(1);
            try {
                std::construct_at(level, std::forward<Arguments>(arguments)...);
            }
            catch(...) {
                allocator.deallocate(level, 1);
                throw;
            }
            // There is room, and an Entry copies without throwing.
            entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), Entry{price, level});
            return {atIndex(index), true};
        }

        // Releases the level; any iterator past it is invalidated with it.
        void erase(const_iterator position) noexcept {
            eraseIndex(static_cast<std::size_t>(position.position - entries.data()) - 1);
        }

        std::size_t erase(Price price) noexcept {
            const std::size_t index = locate(price);
            if(!holds(index, price)) {
                return 0;
            }
            eraseIndex(index);
            return 1;
        }

        void clear() noexcept {
            for(const Entry& entry : entries) {
                release(entry.level);
            }
            entries.clear();
        }

    private:
        static constexpr std::size_t minimumCapacity = 16;

        iterator atIndex(std::size_t index) noexcept { return iterator(entries.data() + index + 1); }
        const_iterator atIndex(std::size_t index) const noexcept { return const_iterator(entries.data() + index + 1); }

        bool holds(std::size_t index, Price price) const noexcept {
            return index < entries.size() && entries[index].price == price;
        }

        // The index of the first entry at this price or better, which is where a new level at
        // this price belongs. Gallops back from the best price, then bisects what is left.
        std::size_t locate(Price price) const noexcept {
            const Better better{};
            const Entry* const array = entries.data();
            std::size_t low = 0;
            std::size_t high = entries.size();
            for(std::size_t step = 1; step < high; step *= 2) {
                const std::size_t probe = high - step;
                if(better(price, array[probe].price)) {
                    low = probe + 1;
                    break;
                }
                high = probe;
            }
            while(low < high) {
                const std::size_t middle = low + (high - low) / 2;
                if(better(price, array[middle].price)) {
                    low = middle + 1;
                }
                else {
                    high = middle;
                }
            }
            return low;
        }

        void eraseIndex(std::size_t index) noexcept {
            release(entries[index].level);
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
        }

        void release(Level* level) noexcept {
            std::destroy_at(level);
            PoolAllocator<Level>(pool).deallocate(level, 1);
        }

        std::vector<Entry> entries;
        NodePool* pool;
        std::uint64_t growthCount = 0;
    };

}  // namespace order_book
