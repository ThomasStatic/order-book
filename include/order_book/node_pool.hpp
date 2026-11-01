#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define ORDER_BOOK_POISON_POOL 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ORDER_BOOK_POISON_POOL 1
#endif
#endif
#if defined(ORDER_BOOK_POISON_POOL)
#include <sanitizer/asan_interface.h>
#endif

namespace order_book {

    template <typename T>
    class PoolAllocator;

    // A pool of fixed-size blocks for one book's resting-order nodes. Each block a node frees
    // goes on a free list and is handed to the next node of its size, so resting and removing
    // orders reuse memory without going to the heap; the heap is reached only to add a chunk
    // of blocks when a free list is empty.
    //
    // Blocks are grouped into a few size classes, one per distinct (size, alignment) a
    // container asks for: the two list nodes, the two level objects, and, in builds with checked
    // iterators, a container's small bookkeeping object, with one to spare, so no single first
    // request can crowd another out. A request of a further size, or for more than one object,
    // goes to the heap directly.
    //
    // Blocks are returned to the pool, not to the heap, so a book keeps the node memory of its
    // largest size until it is destroyed. In ASan builds each freed block is poisoned whole, so
    // a stale pointer into one, a PriceLevel* above all, is still caught although the pool
    // never gives the memory back. Not thread-safe, like the book that owns it.
    class NodePool {
    public:
        static constexpr std::size_t maxClasses = 6;

        // What check found: blocks handed out and not yet returned, per size class. A free list
        // longer than the blocks ever carved can only be a corrupted one (a block freed twice
        // links it into a cycle), and that is what sound says.
        struct Usage {
            std::array<std::size_t, maxClasses> liveBlocks{};
            bool sound = true;

            std::size_t totalLiveBlocks() const noexcept {
                std::size_t total = 0;
                for(const std::size_t live : liveBlocks) {
                    total += live;
                }
                return total;
            }
        };

        NodePool() = default;
        NodePool(const NodePool&) = delete;
        NodePool& operator=(const NodePool&) = delete;

        ~NodePool() {
            for(const SizeClass& sizeClass : classes) {
                for(std::byte* chunk : sizeClass.chunks) {
                    markUsable(chunk, sizeClass.size * blocksPerChunk);
                    release(chunk, sizeClass.align);
                }
            }
        }

        // Throws std::bad_alloc if a new chunk cannot be allocated, leaving every free list
        // and chunk as it was.
        void* allocate(std::size_t bytes, std::size_t alignment) {
            SizeClass* const sizeClass = classFor(bytes, alignment, true);
            if(sizeClass == nullptr) {
                return obtain(bytes, alignment);
            }
            if(sizeClass->freeBlocks != nullptr) {
                FreeBlock* const block = sizeClass->freeBlocks;
                markUsable(block, sizeClass->size);
                sizeClass->freeBlocks = block->next;
                return block;
            }
            if(sizeClass->carvedBlocks == blocksPerChunk) {
                // Reserve first, so recording the new chunk cannot throw once it is allocated.
                if(sizeClass->chunks.size() == sizeClass->chunks.capacity()) {
                    sizeClass->chunks.reserve(sizeClass->chunks.empty() ? 8 : sizeClass->chunks.size() * 2);
                }
                sizeClass->chunks.push_back(
                    static_cast<std::byte*>(obtain(sizeClass->size * blocksPerChunk, sizeClass->align)));
                sizeClass->carvedBlocks = 0;
                ++chunkCount;
            }
            return sizeClass->chunks.back() + sizeClass->size * sizeClass->carvedBlocks++;
        }

        void deallocate(void* block, std::size_t bytes, std::size_t alignment) noexcept {
            SizeClass* const sizeClass = classFor(bytes, alignment, false);
            if(sizeClass == nullptr) {
                release(block, alignment);
                return;
            }
            sizeClass->freeBlocks = ::new(block) FreeBlock{sizeClass->freeBlocks};
            markUnusable(block, sizeClass->size);
        }

        // How many chunks it has taken from the heap, ever.
        std::uint64_t chunksTaken() const noexcept { return chunkCount; }

        // Walks every free list and counts carved blocks minus free ones, per class. Never
        // allocates or throws, and leaves each block as it was.
        Usage check() const noexcept {
            Usage usage;
            for(std::size_t index = 0; index < maxClasses; ++index) {
                const SizeClass& sizeClass = classes[index];
                if(sizeClass.chunks.empty()) {
                    continue;
                }
                const std::size_t carved = (sizeClass.chunks.size() - 1) * blocksPerChunk + sizeClass.carvedBlocks;
                std::size_t free = 0;
                for(FreeBlock* block = sizeClass.freeBlocks; block != nullptr; ++free) {
                    if(free == carved) {
                        usage.sound = false;
                        break;
                    }
                    markUsable(block, sizeClass.size);
                    FreeBlock* const next = block->next;
                    markUnusable(block, sizeClass.size);
                    block = next;
                }
                usage.liveBlocks[index] = carved - free;
            }
            return usage;
        }

    private:
        template <typename T>
        friend class PoolAllocator;

        static void markUsable([[maybe_unused]] void* block, [[maybe_unused]] std::size_t bytes) noexcept {
#if defined(ORDER_BOOK_POISON_POOL)
            ASAN_UNPOISON_MEMORY_REGION(block, bytes);
#endif
        }

        static void markUnusable([[maybe_unused]] void* block, [[maybe_unused]] std::size_t bytes) noexcept {
#if defined(ORDER_BOOK_POISON_POOL)
            ASAN_POISON_MEMORY_REGION(block, bytes);
#endif
        }

        struct FreeBlock {
            FreeBlock* next;
        };

        struct SizeClass {
            std::size_t size = 0;  // zero while unused
            std::size_t align = 0;
            FreeBlock* freeBlocks = nullptr;
            std::vector<std::byte*> chunks;
            std::size_t carvedBlocks = blocksPerChunk;  // blocks handed out from the newest chunk
        };

        static constexpr std::size_t blocksPerChunk = 256;

        static std::size_t blockSizeFor(std::size_t bytes, std::size_t alignment) noexcept {
            const std::size_t size = bytes < sizeof(FreeBlock) ? sizeof(FreeBlock) : bytes;
            return (size + alignment - 1) / alignment * alignment;
        }

        static std::size_t blockAlignFor(std::size_t alignment) noexcept {
            return alignment < alignof(FreeBlock) ? alignof(FreeBlock) : alignment;
        }

        // The class that serves this request, claiming an unused one if `claim` and there is
        // room; null if the request goes to the heap. A deallocation never claims, so it finds
        // exactly the class its allocation used, or none, as that allocation did.
        SizeClass* classFor(std::size_t bytes, std::size_t alignment, bool claim) noexcept {
            const std::size_t align = blockAlignFor(alignment);
            const std::size_t size = blockSizeFor(bytes, align);
            for(SizeClass& sizeClass : classes) {
                if(sizeClass.size == size && sizeClass.align == align) {
                    return &sizeClass;
                }
                if(sizeClass.size == 0) {
                    if(!claim) {
                        return nullptr;
                    }
                    sizeClass.size = size;
                    sizeClass.align = align;
                    return &sizeClass;
                }
            }
            return nullptr;
        }

        // The plain global operator new whenever the alignment allows it, so replacements of
        // it (allocation-failure tests, allocation counters) see the pool's chunks, and
        // PoolAllocator's heap allocations, too. A direct call, unlike std::allocator's
        // __builtin_operator_new, also keeps an optimizer from assuming the replacement
        // touches nothing else, and so from moving a counter's reads across it.
        static void* obtain(std::size_t bytes, std::size_t alignment) {
            if(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                return ::operator new(bytes);
            }
            return ::operator new(bytes, std::align_val_t{alignment});
        }

        static void release(void* block, std::size_t alignment) noexcept {
            if(alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                ::operator delete(block);
                return;
            }
            ::operator delete(block, std::align_val_t{alignment});
        }

        SizeClass classes[maxClasses];
        std::uint64_t chunkCount = 0;
    };

    // An allocator that takes single objects from a NodePool, and anything else, or
    // everything when it has no pool, from the heap. Two allocators are equal when they use
    // the same pool, so a container's nodes are always freed into the pool they came from.
    template <typename T>
    class PoolAllocator {
    public:
        using value_type = T;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap = std::true_type;

        PoolAllocator() noexcept = default;
        explicit PoolAllocator(NodePool* nodePool) noexcept : pool(nodePool) {}
        template <typename U>
        PoolAllocator(const PoolAllocator<U>& other) noexcept : pool(other.pool) {}

        T* allocate(std::size_t count) {
            if(pool != nullptr && count == 1) {
                return static_cast<T*>(pool->allocate(sizeof(T), alignof(T)));
            }
            if(count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
                throw std::bad_array_new_length();
            }
            return static_cast<T*>(NodePool::obtain(count * sizeof(T), alignof(T)));
        }

        void deallocate(T* pointer, std::size_t count) noexcept {
            if(pool != nullptr && count == 1) {
                pool->deallocate(pointer, sizeof(T), alignof(T));
                return;
            }
            NodePool::release(pointer, alignof(T));
        }

        template <typename U>
        bool operator==(const PoolAllocator<U>& other) const noexcept {
            return pool == other.pool;
        }

    private:
        template <typename U>
        friend class PoolAllocator;

        NodePool* pool = nullptr;
    };

}  // namespace order_book
