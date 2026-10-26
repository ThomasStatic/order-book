#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace order_book {

    // A vector of trivially copyable elements that keeps up to InlineCapacity of them inside
    // the object and moves to the heap only beyond that. The command results hold their
    // events and trades in it, so a command with one event and a few trades (nearly every
    // command) allocates nothing to report what it did.
    //
    // Not a drop-in std::vector: only what the results need, which is appending, reserving,
    // reading, copying, moving, and comparing, including with a std::vector of the same
    // elements; no insert, erase, clear, resize, or allocator. It is a contiguous range, so a
    // std::span<const T> binds to it directly, and a caller that wants a std::vector of its own
    // asks for one with toVector(), so no copy happens without being visible. reserve and
    // push_back give the strong guarantee: if growing throws std::bad_alloc, nothing changes.
    // Moving a vector whose elements are inline copies them, so unlike std::vector a move
    // does not keep pointers into the source valid.
    template <typename T, std::size_t InlineCapacity>
    class SmallVector {
        static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                      "SmallVector copies and discards elements without running their constructors or destructors");
        static_assert(InlineCapacity > 0, "SmallVector needs room for at least one element inline");

    public:
        using value_type = T;
        using size_type = std::size_t;
        using iterator = T*;
        using const_iterator = const T*;

        // User-provided, so a const SmallVector can be default-constructed: the inline storage
        // is left uninitialized until elements are added, as std::vector leaves its capacity.
        SmallVector() noexcept {}

        SmallVector(std::initializer_list<T> values) {
            append(values.begin(), values.size());
        }

        SmallVector(const SmallVector& other) {
            append(other.data(), other.count);
        }

        SmallVector(SmallVector&& other) noexcept {
            takeFrom(other);
        }

        SmallVector& operator=(const SmallVector& other) {
            if(this != &other) {
                SmallVector copy(other);
                *this = std::move(copy);
            }
            return *this;
        }

        SmallVector& operator=(SmallVector&& other) noexcept {
            if(this != &other) {
                releaseHeap();
                takeFrom(other);
            }
            return *this;
        }

        ~SmallVector() {
            releaseHeap();
        }

        size_type size() const noexcept { return count; }
        size_type capacity() const noexcept { return limit; }
        bool empty() const noexcept { return count == 0; }
        // Whether the elements have moved to the heap; for tests.
        bool usesHeap() const noexcept { return heap != nullptr; }

        T* data() noexcept { return heap != nullptr ? heap : inlineElements(); }
        const T* data() const noexcept { return heap != nullptr ? heap : inlineElements(); }

        iterator begin() noexcept { return data(); }
        iterator end() noexcept { return data() + count; }
        const_iterator begin() const noexcept { return data(); }
        const_iterator end() const noexcept { return data() + count; }

        T& operator[](size_type index) noexcept {
            assert(index < count);
            return data()[index];
        }
        const T& operator[](size_type index) const noexcept {
            assert(index < count);
            return data()[index];
        }
        const T& front() const noexcept {
            assert(count > 0);
            return data()[0];
        }
        const T& back() const noexcept {
            assert(count > 0);
            return data()[count - 1];
        }


        // A copy of the elements, for code that keeps them in a std::vector of its own.
        std::vector<T> toVector() const { return std::vector<T>(begin(), end()); }

        // Makes room for at least `wanted` elements, so that many push_backs cannot throw.
        void reserve(size_type wanted) {
            if(wanted <= limit) {
                return;
            }
            std::allocator<T> allocator;
            T* const grown = allocator.allocate(wanted);
            std::uninitialized_copy_n(data(), count, grown);
            releaseHeap();
            heap = grown;
            limit = wanted;
        }

        void push_back(const T& value) {
            if(count == limit) {
                // value may be one of this vector's own elements, which growing would free.
                const T copy = value;
                reserve(limit * 2);
                ::new(static_cast<void*>(data() + count)) T(copy);
            }
            else {
                ::new(static_cast<void*>(data() + count)) T(value);
            }
            ++count;
        }

        friend bool operator==(const SmallVector& left, const SmallVector& right)
            requires std::equality_comparable<T>
        {
            return std::equal(left.begin(), left.end(), right.begin(), right.end());
        }

        friend bool operator==(const SmallVector& left, const std::vector<T>& right)
            requires std::equality_comparable<T>
        {
            return std::equal(left.begin(), left.end(), right.begin(), right.end());
        }

    private:
        // Laundered, since the elements were created in the storage by placement new or copying.
        T* inlineElements() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        const T* inlineElements() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }

        void append(const T* values, size_type added) {
            reserve(added);
            std::uninitialized_copy_n(values, added, data());
            count = added;
        }

        void releaseHeap() noexcept {
            if(heap != nullptr) {
                std::allocator<T>().deallocate(heap, limit);
                heap = nullptr;
                limit = InlineCapacity;
            }
        }

        // Takes other's elements, leaving it empty with its inline capacity. This vector must
        // hold no heap block.
        void takeFrom(SmallVector& other) noexcept {
            if(other.heap != nullptr) {
                heap = other.heap;
                limit = other.limit;
                other.heap = nullptr;
                other.limit = InlineCapacity;
            }
            else {
                std::uninitialized_copy_n(other.inlineElements(), other.count, inlineElements());
            }
            count = other.count;
            other.count = 0;
        }

        T* heap = nullptr;
        size_type count = 0;
        size_type limit = InlineCapacity;
        alignas(T) std::byte storage[sizeof(T) * InlineCapacity];
    };

}  // namespace order_book
