#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "order_location.hpp"
#include "types.hpp"

namespace order_book {

    // The book's index from a resting order's ID to its location: an open-addressed hash
    // table. Entries live in one array, so finding an order usually touches one cache line,
    // where a node-based map touches a bucket array and then a separately allocated node; and
    // adding or removing an entry allocates nothing until the table grows.
    //
    // Linear probing over a power-of-two array, kept at most three quarters full, with a
    // multiplicative (Fibonacci) hash, so IDs that arrive in sequence are spread over the
    // table rather than clustered. Erasing shifts the entries after it back into the gap, so
    // a book that adds and removes orders forever leaves no tombstones and never slows down.
    //
    // The hash is not hardened against chosen IDs: IDs in arithmetic progression whose step is
    // a large Fibonacci number (75025, say) land in a small part of the table, and under
    // linear probing every operation on them then walks one long run. Order IDs come from
    // callers, so a caller that can pick them can slow the book down (as it could with the
    // standard library's hash maps); it cannot make the book wrong.
    //
    // ID 0, which is never a valid order ID, marks an empty slot; emplacing it throws
    // std::invalid_argument. Iterators and references to entries are invalidated by any
    // emplace (which may grow the table) and any erase (which may move entries), as is the
    // position of every entry: iteration order is unspecified. An iterator gives an entry;
    // change only its second and owner. Growing gives the strong guarantee: if allocating the
    // new array throws, the table is unchanged.
    //
    // The same table, over StopLocation, indexes pending stops (PendingStopIndex), so an ID
    // is live while either table holds it.
    //
    // One slot of the index. Named like the std::pair<OrderId, Location> it replaced; the
    // owner sits where that pair had padding, so a slot is no bigger.
    template <typename Location>
    struct IndexEntry {
        OrderId first;  // the order's ID
        OwnerId owner;  // for self-trade prevention; 0 for none (and for a pending stop, whose owner is in its record)
        Location second;
    };

    template <typename Location>
    class BasicOrderIndex {
    public:
        using value_type = IndexEntry<Location>;

        // Visits occupied slots only, in table order.
        template <bool IsConst>
        class Iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = BasicOrderIndex::value_type;
            using difference_type = std::ptrdiff_t;
            using pointer = std::conditional_t<IsConst, const value_type*, value_type*>;
            using reference = std::conditional_t<IsConst, const value_type&, value_type&>;

            Iterator() = default;

            reference operator*() const noexcept { return *slot; }
            pointer operator->() const noexcept { return slot; }

            Iterator& operator++() noexcept {
                ++slot;
                skipEmpty();
                return *this;
            }

            Iterator operator++(int) noexcept {
                Iterator before = *this;
                ++*this;
                return before;
            }

            bool operator==(const Iterator& other) const noexcept { return slot == other.slot; }

            operator Iterator<true>() const noexcept
                requires(!IsConst)
            {
                return Iterator<true>(slot, last);
            }

        private:
            friend class BasicOrderIndex;
            template <bool>
            friend class Iterator;

            Iterator(pointer at, pointer end) noexcept : slot(at), last(end) {}

            void skipEmpty() noexcept {
                while(slot != last && slot->first == emptyId) {
                    ++slot;
                }
            }

            pointer slot = nullptr;
            pointer last = nullptr;
        };

        using iterator = Iterator<false>;
        using const_iterator = Iterator<true>;

        BasicOrderIndex() = default;
        BasicOrderIndex(const BasicOrderIndex&) = default;
        BasicOrderIndex& operator=(const BasicOrderIndex&) = default;

        // A moved-from index is empty and can be used again.
        BasicOrderIndex(BasicOrderIndex&& other) noexcept
            : slots(std::move(other.slots)),
              entries(std::exchange(other.entries, 0)),
              mask(std::exchange(other.mask, 0)),
              shift(std::exchange(other.shift, 64)) {
            other.slots.clear();
        }

        BasicOrderIndex& operator=(BasicOrderIndex&& other) noexcept {
            if(this != &other) {
                slots = std::move(other.slots);
                other.slots.clear();
                entries = std::exchange(other.entries, 0);
                mask = std::exchange(other.mask, 0);
                shift = std::exchange(other.shift, 64);
            }
            return *this;
        }

        iterator begin() noexcept {
            iterator first(slots.data(), slots.data() + slots.size());
            first.skipEmpty();
            return first;
        }
        iterator end() noexcept { return iterator(slots.data() + slots.size(), slots.data() + slots.size()); }
        const_iterator begin() const noexcept { return const_cast<BasicOrderIndex*>(this)->begin(); }
        const_iterator end() const noexcept { return const_cast<BasicOrderIndex*>(this)->end(); }

        std::size_t size() const noexcept { return entries; }
        bool empty() const noexcept { return entries == 0; }
        // Slots in the table; it grows once size() would pass three quarters of this.
        std::size_t capacity() const noexcept { return slots.size(); }

        iterator find(OrderId id) noexcept {
            if(id == emptyId || slots.empty()) {
                return end();
            }
            for(std::size_t slot = home(id);; slot = (slot + 1) & mask) {
                if(slots[slot].first == id) {
                    return iterator(slots.data() + slot, slots.data() + slots.size());
                }
                if(slots[slot].first == emptyId) {
                    return end();
                }
            }
        }

        const_iterator find(OrderId id) const noexcept {
            return const_cast<BasicOrderIndex*>(this)->find(id);
        }

        std::size_t count(OrderId id) const noexcept { return find(id) == end() ? 0 : 1; }

        // Throws std::out_of_range if no entry has this ID.
        Location& at(OrderId id) {
            const iterator found = find(id);
            if(found == end()) {
                throw std::out_of_range("order index has no entry with this id");
            }
            return found->second;
        }

        const Location& at(OrderId id) const {
            return const_cast<BasicOrderIndex*>(this)->at(id);
        }

        // Adds an entry unless one with this ID exists; either way returns the entry with the
        // ID and whether it was added. May grow the table first, invalidating iterators.
        std::pair<iterator, bool> emplace(OrderId id, const Location& location, OwnerId owner = 0) {
            if(id == emptyId) {
                throw std::invalid_argument("order index cannot hold the reserved id 0");
            }
            if(const iterator existing = find(id); existing != end()) {
                return {existing, false};
            }
            if((entries + 1) * 4 > slots.size() * 3) {
                growTo(slots.empty() ? minimumCapacity : slots.size() * 2);
            }
            std::size_t slot = home(id);
            while(slots[slot].first != emptyId) {
                slot = (slot + 1) & mask;
            }
            slots[slot] = value_type{id, owner, location};
            ++entries;
            return {iterator(slots.data() + slot, slots.data() + slots.size()), true};
        }

        // Grows the table, if needed, so it holds `wantedEntries` without growing again.
        // Throws std::length_error for a count no table could hold, std::bad_alloc if the
        // allocation fails; either way the index is unchanged.
        void reserve(std::size_t wantedEntries) {
            if(wantedEntries > std::numeric_limits<std::size_t>::max() / 8 / sizeof(value_type)) {
                throw std::length_error("order index cannot reserve that many entries");
            }
            std::size_t wanted = minimumCapacity;
            while(wanted * 3 < wantedEntries * 4) {
                wanted *= 2;
            }
            if(wanted > slots.size()) {
                growTo(wanted);
            }
        }

        std::size_t erase(OrderId id) noexcept {
            const iterator found = find(id);
            if(found == end()) {
                return 0;
            }
            erase(found);
            return 1;
        }

        // Removes the entry at position, which must be valid, shifting the entries after it in
        // its probe run back so every remaining entry stays reachable from its home slot.
        void erase(iterator position) noexcept {
            std::size_t gap = static_cast<std::size_t>(position.slot - slots.data());
            for(std::size_t next = (gap + 1) & mask; slots[next].first != emptyId; next = (next + 1) & mask) {
                // The entry at next may fill the gap unless its home lies cyclically in
                // (gap, next]: then the gap is before its home, and moving it would hide it.
                const std::size_t entryHome = home(slots[next].first);
                const bool homeAfterGap = gap <= next ? (gap < entryHome && entryHome <= next)
                                                      : (gap < entryHome || entryHome <= next);
                if(!homeAfterGap) {
                    slots[gap] = slots[next];
                    gap = next;
                }
            }
            slots[gap].first = emptyId;
            --entries;
        }

        void clear() noexcept {
            for(value_type& slot : slots) {
                slot.first = emptyId;
            }
            entries = 0;
        }

    private:
        static constexpr OrderId emptyId = 0;
        static constexpr std::size_t minimumCapacity = 16;

        std::size_t home(OrderId id) const noexcept {
            return static_cast<std::size_t>((static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL) >> shift);
        }

        // capacity is a power of two, at least minimumCapacity and more than size() / 0.75.
        void growTo(std::size_t capacity) {
            std::vector<value_type> grown(capacity, value_type{emptyId, 0, Location(0, Side::BUY, {}, nullptr)});
            unsigned int grownShift = 64;
            for(std::size_t remaining = capacity; remaining > 1; remaining >>= 1) {
                --grownShift;
            }
            // Nothing below can throw: the new array exists, and copying entries cannot fail.
            std::swap(grown, slots);
            shift = grownShift;
            mask = capacity - 1;
            for(const value_type& entry : grown) {
                if(entry.first != emptyId) {
                    std::size_t slot = home(entry.first);
                    while(slots[slot].first != emptyId) {
                        slot = (slot + 1) & mask;
                    }
                    slots[slot] = entry;
                }
            }
        }

        std::vector<value_type> slots;
        std::size_t entries = 0;
        std::size_t mask = 0;
        unsigned int shift = 64;
    };

    using OrderIndexEntry = IndexEntry<OrderLocation>;
    using OrderIndex = BasicOrderIndex<OrderLocation>;
    using PendingStopIndex = BasicOrderIndex<StopLocation>;
    static_assert(sizeof(OrderIndexEntry) == sizeof(std::pair<OrderId, OrderLocation>),
                  "the owner should fill the slot's padding, not grow it");

}  // namespace order_book
