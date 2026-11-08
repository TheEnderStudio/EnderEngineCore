#pragma once

#include "Common.hpp"

EE_NAMESPACE_MEMORIZER_BEGIN

namespace SDetail {

    inline constexpr Size Alignment = 16;
    inline constexpr Size MaxPooledSize = 4096;

    // A span is exactly one super page, and every span is super-page aligned. That is
    // what makes the "span base = pointer & ~SuperPageMask" header lookup valid - the
    // previous 64 KB spanned / 64 KB aligned layout did not satisfy a 256 KB mask, so
    // a span that was not 64 KB aligned *and* 256 KB aligned handed the deallocator a
    // pointer to unrelated memory (and engineFree read a header from it).
    inline constexpr Size SuperPageSize = PageDetail::SuperPageSize;
    inline constexpr Size SpanSize = SuperPageSize;
    inline constexpr Size SuperPageMask = SuperPageSize - 1;
    inline constexpr Size MaxLargeSize = SuperPageSize - 16;

    inline constexpr Size SizeClasses[] = {
        16, 32, 48, 64, 96, 128, 192, 256,
        384, 512, 768, 1024, 1536, 2048, 3072, 4096
    };
    inline constexpr Size NumSizeClasses = sizeof(SizeClasses) / sizeof(SizeClasses[0]);

    inline constexpr UInt8  LargeAllocationMarker = 0xFF;
    inline constexpr UInt64 SpanMagic = 0xDEADBEEFCAFEBABEull;
    inline constexpr UInt64 LargeMagic = 0xBEEFFACEDEADFEEDull;

    struct SizeClassTable {
        UInt8 data[MaxPooledSize + 1];
        constexpr SizeClassTable() : data{} {
            UInt8 classIdx = 0;
            for (Size s = 1; s <= MaxPooledSize; ++s) {
                while (classIdx + 1 < NumSizeClasses && SizeClasses[classIdx] < s) {
                    ++classIdx;
                }
                data[s] = classIdx;
            }
        }
    };
    inline constexpr SizeClassTable SizeClassTable{};

    EE_NODISCARD EE_FORCE_INLINE UInt8 sizeToClass(Size size) {
        return SizeClassTable.data[size];
    }

    struct alignas(16) SpanHeader {
        UInt64 magic;           // For verification
        UInt32 sizeClassIndex;  // class index or LargeAllocationMarker
        UInt32 reserved;        // store size (lower 32 bits) when large object
    };
    static_assert(sizeof(SpanHeader) == 16);
    static_assert(sizeof(SpanHeader) == Alignment);
    inline constexpr Size SpanHeaderSize = sizeof(SpanHeader);

    struct FreeNode {
        FreeNode* next;
    };
    static_assert(sizeof(FreeNode) <= SizeClasses[0], "FreeNode must fit in smallest class");

    EE_NODISCARD EE_FORCE_INLINE UInt32 batchSizeForClass(UInt8 idx) {
        Size b = SpanSize / SizeClasses[idx] / 16;
        return static_cast<UInt32>(b < 2 ? 2 : b);
    }

    EE_NODISCARD EE_FORCE_INLINE void* alignedAllocate(Size alignment, Size size) {
#if defined(_MSC_VER)
        return _aligned_malloc(size, alignment);
#else
        return std::aligned_alloc(alignment, size);
#endif
    }

    EE_FORCE_INLINE void alignedFree(void* ptr) {
#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

#ifdef EE_MEMORIZER_DEBUG
    inline std::atomic<UInt64>& debugSpanGrowsRef(); // forward
#endif

    class alignas(64) CentralFreeList {
    public:
        void grow(UInt8 sizeClass) {
            // Spans are super-page aligned by construction (they are carved out of
            // super-page aligned chunks), which is what the `addr & ~SuperPageMask` 
            // header lookup requires.
            void* spanMem = PageDetail::SpanSource::getInstance().allocateSpan();
            if (!spanMem) [[unlikely]] return;

            auto* span = static_cast<SpanHeader*>(spanMem);
            span->magic = SpanMagic;
            span->sizeClassIndex = sizeClass;
            span->reserved = 0;

            const Size objSize = SizeClasses[sizeClass];
            const Size numObjects = (SpanSize - SpanHeaderSize) / objSize;
            auto* start = reinterpret_cast<UInt8*>(spanMem) + SpanHeaderSize;

            FreeNode* first = nullptr;
            FreeNode* last = nullptr;
            for (Size i = 0; i < numObjects; ++i) {
                auto* node = reinterpret_cast<FreeNode*>(start + i * objSize);
                node->next = nullptr;
                if (last) [[likely]] last->next = node;
                else                 first = node;
                last = node;
            }

            {
                std::lock_guard<Mutex> lock(m_mutex);
                last->next = m_head;
                m_head = first;
                m_count += numObjects;
            }

#ifdef EE_MEMORIZER_DEBUG
            debugSpanGrowsRef().fetch_add(1, std::memory_order_relaxed);
#endif
        }

        EE_NODISCARD FreeNode* popBatch(UInt8 sizeClass, Size n, Size& outCount) {
            std::unique_lock<Mutex> lock(m_mutex);

            if (!m_head) [[unlikely]] {
                lock.unlock();
                grow(sizeClass);
                lock.lock();
            }

            FreeNode* result = nullptr;
            Size    count = 0;
            while (m_head && count < n) {
                FreeNode* node = m_head;
                m_head = node->next;
                --m_count;
                node->next = result;
                result = node;
                ++count;
            }
            outCount = count;
            return result;
        }

        void pushBatch(FreeNode* head, FreeNode* tail, Size count) {
            if (!head) [[unlikely]] return;
            std::lock_guard<Mutex> lock(m_mutex);
            tail->next = m_head;
            m_head = head;
            m_count += count;
        }

    private:
        mutable Mutex m_mutex;
        FreeNode* m_head = nullptr;
        Size      m_count = 0;
    };

    class CentralPool {
    public:
        static CentralPool& getInstance() {
            // Static storage instead of `new`: see PageDetail::SpanSource::getInstance()
            // (the constructor is private, so the pattern is written out here).
            alignas(CentralPool) static unsigned char storage[sizeof(CentralPool)];
            static CentralPool* const instance = ::new (static_cast<void*>(storage)) CentralPool{};
            return *instance;
        }

        EE_NODISCARD CentralFreeList& getList(UInt8 sizeClass) {
            return m_lists[sizeClass];
        }

    private:
        CentralPool() = default;
        CentralFreeList m_lists[NumSizeClasses];
    };

    struct ThreadCacheEntry {
        FreeNode* head = nullptr;
        UInt32    count = 0;
        UInt32    maxCount = 0;
    };

    struct ThreadCache {
        ThreadCacheEntry entries[NumSizeClasses];
        bool             initialized = false;
        ~ThreadCache();
    };

    inline ThreadCache::~ThreadCache() {
        if (!initialized) return;
        for (UInt8 i = 0; i < NumSizeClasses; ++i) {
            auto& e = entries[i];
            if (!e.head) continue;
            FreeNode* tail = e.head;
            while (tail->next) tail = tail->next;
            CentralPool::getInstance().getList(i).pushBatch(e.head, tail, e.count);
            e.head = nullptr;
            e.count = 0;
        }
    }

    // `inline` matters: this is a header, and a non-inline function definition in a
    // header is an ODR violation the moment two translation units include it.
    inline ThreadCache& getThreadCache() {
        thread_local ThreadCache tc;
        if (!tc.initialized) [[unlikely]] {
            for (UInt8 i = 0; i < NumSizeClasses; ++i) {
                tc.entries[i].maxCount = batchSizeForClass(i) * 2;
            }
            tc.initialized = true;
        }
        return tc;
    }

#ifdef EE_MEMORIZER_DEBUG
    struct SmallPoolStats {
        std::atomic<UInt64> totalAllocs{ 0 };
        std::atomic<UInt64> totalFrees{ 0 };
        std::atomic<UInt64> currentBytes{ 0 };
        std::atomic<UInt64> peakBytes{ 0 };
        std::atomic<UInt64> spanGrows{ 0 };
        std::atomic<UInt64> cacheRefills{ 0 };
        std::atomic<UInt64> cacheFlushes{ 0 };
        std::atomic<UInt64> perClassAllocs[NumSizeClasses]{};
        std::atomic<UInt64> perClassFrees[NumSizeClasses]{};
    };

    inline SmallPoolStats& getStats() {
        static SmallPoolStats s_stats;
        return s_stats;
    }

    inline std::atomic<UInt64>& debugSpanGrowsRef() {
        return getStats().spanGrows;
    }

    inline void recordAlloc(UInt8 classIdx, Size bytes) {
        auto& s = getStats();
        s.totalAllocs.fetch_add(1, std::memory_order_relaxed);
        if (classIdx != LargeAllocationMarker) {
            s.perClassAllocs[classIdx].fetch_add(1, std::memory_order_relaxed);
        }
        const UInt64 cur = s.currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        UInt64 peak = s.peakBytes.load(std::memory_order_relaxed);
        while (cur > peak && !s.peakBytes.compare_exchange_weak(peak, cur)) {}
    }

    inline void recordFree(UInt8 classIdx, Size bytes) {
        auto& s = getStats();
        s.totalFrees.fetch_add(1, std::memory_order_relaxed);
        if (classIdx != LargeAllocationMarker) {
            s.perClassFrees[classIdx].fetch_add(1, std::memory_order_relaxed);
        }
        s.currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
#endif

    inline void* allocateInternal(Size size) {
        if (size > MaxPooledSize) [[unlikely]] {
            // One super page holds one such block, so a size that does not fit has to
            // be refused rather than overrunning the page (the old code computed the
            // page count into `total` and then ignored it, always reserving one page).
            if (size > MaxLargeSize) [[unlikely]] return nullptr;
            void* mem = alignedAllocate(SuperPageSize, SuperPageSize);
            if (!mem) [[unlikely]] return nullptr;

            auto* span = static_cast<SpanHeader*>(mem);
            span->magic = LargeMagic;
            span->sizeClassIndex = LargeAllocationMarker;
            span->reserved = static_cast<UInt32>(size);

#ifdef EE_MEMORIZER_DEBUG
            recordAlloc(LargeAllocationMarker, size);
#endif
            return static_cast<UInt8*>(mem) + SpanHeaderSize;
        }

        const UInt8 classIdx = sizeToClass(size);
        auto& cache = getThreadCache();
        auto& entry = cache.entries[classIdx];

        FreeNode* head = entry.head;
        if (!head) [[unlikely]] {
            const Size batch = batchSizeForClass(classIdx);
            Size got = 0;
            FreeNode* nodes = CentralPool::getInstance().getList(classIdx).popBatch(classIdx, batch, got);
            if (!nodes) [[unlikely]] return nullptr;
            entry.head = nodes;
            entry.count = static_cast<UInt32>(got);
#ifdef EE_MEMORIZER_DEBUG
            getStats().cacheRefills.fetch_add(1, std::memory_order_relaxed);
#endif
            head = entry.head;
        }

        entry.head = head->next;
        --entry.count;

#ifdef EE_MEMORIZER_DEBUG
        recordAlloc(classIdx, SizeClasses[classIdx]);
#endif
        return head;
    }

    inline void deallocateInternal(void* ptr) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        auto* span = reinterpret_cast<SpanHeader*>(addr & ~SuperPageMask);

#ifdef EE_MEMORIZER_DEBUG
        if (span->magic != SpanMagic && span->magic != LargeMagic) [[unlikely]] {
            std::fprintf(stderr, "[SmallPool] Corruption detected at %p\n", ptr);
            std::terminate();
        }
#endif

        // Narrow once, with a bound check: a corrupted header would otherwise index
        // every following table/array out of range.
        const UInt8 classIdx = static_cast<UInt8>(span->sizeClassIndex);
        if (classIdx != LargeAllocationMarker && classIdx >= NumSizeClasses) [[unlikely]] {
            std::fprintf(stderr, "[SmallPool] Invalid size class %u at %p\n", span->sizeClassIndex, ptr);
            std::terminate();
        }
        if (classIdx == LargeAllocationMarker) [[unlikely]] {
#ifdef EE_MEMORIZER_DEBUG
            recordFree(LargeAllocationMarker, span->reserved);
#endif
            alignedFree(span);
            return;
        }

        auto& cache = getThreadCache();
        auto& entry = cache.entries[classIdx];

        auto* node = static_cast<FreeNode*>(ptr);
        node->next = entry.head;
        entry.head = node;
        ++entry.count;

        if (entry.count >= entry.maxCount) [[unlikely]] {
            const UInt32 returnCount = entry.count / 2;
            FreeNode* head = entry.head;
            FreeNode* tail = head;
            for (UInt32 i = 0; i < returnCount - 1; ++i) {
                tail = tail->next;
            }
            FreeNode* newHead = tail->next;
            tail->next = nullptr;
            entry.head = newHead;
            entry.count -= returnCount;
            CentralPool::getInstance().getList(classIdx).pushBatch(head, tail, returnCount);
#ifdef EE_MEMORIZER_DEBUG
            getStats().cacheFlushes.fetch_add(1, std::memory_order_relaxed);
#endif
        }

#ifdef EE_MEMORIZER_DEBUG
        recordFree(classIdx, SizeClasses[classIdx]);
#endif
    }

} // namespace SDetail

class SmallPool {
public:
    EE_NODISCARD static EE_FORCE_INLINE void* allocate(Size size) {
        return SDetail::allocateInternal(size);
    }

    static EE_FORCE_INLINE void deallocate(void* ptr) {
        if (ptr) [[likely]] SDetail::deallocateInternal(ptr);
    }

#ifdef EE_MEMORIZER_DEBUG
    static void dumpStats();
    static void resetStats();
#endif
};

template <typename T, typename... Args>
    requires Newable<T>
EE_NODISCARD T* smallNew(Args&&... args) {
    static_assert(alignof(T) <= SDetail::Alignment,
        "Alignment > 16 not supported by basic pool");
    static_assert(sizeof(T) <= SDetail::MaxPooledSize,
        "Small pool only serves objects up to MaxPooledSize; use mediumNew or engineAllocate");
    void* mem = SmallPool::allocate(sizeof(T));
    if (!mem) [[unlikely]] {
        // Pool `new` reports failure the way the rest of the engine does (a null
        // result the caller checks) instead of throwing.
        return nullptr;
    }
    return ::new (mem) T(std::forward<Args>(args)...);
}

template <typename T>
    requires Deletable<T>
void smallDelete(T* ptr) {
    if (ptr) [[likely]] {
        ptr->~T();
        SmallPool::deallocate(ptr);
    }
}

#ifdef EE_MEMORIZER_DEBUG
inline void SmallPool::dumpStats() {
    auto& s = SDetail::getStats();
    String table;
    for (UInt32 i = 0; i < SDetail::NumSizeClasses; ++i) {
        const UInt64 a = s.perClassAllocs[i].load();
        const UInt64 f = s.perClassFrees[i].load();
        table = fmt::format("{}{:<12}|{:<12}|{:<12}|{:<12}\n", table, SDetail::SizeClasses[i], a, f, a - f);
    }
    fmt::println(R"(
=== SmallPool Statistics ===
Total allocations : {}
Total frees       : {}
Current bytes     : {}
Peak bytes        : {}
Span grows        : {}
Cache refills     : {}
Cache flushes     : {}

{:^12}|{:^12}|{:^12}|{:^12}
{:-<12}+{:-<12}+{:-<12}+{:-<12}
{})", s.totalAllocs.load(),
        s.totalFrees.load(),
        s.currentBytes.load(),
        s.peakBytes.load(),
        s.spanGrows.load(),
        s.cacheRefills.load(),
        s.cacheFlushes.load(),
        "Size", "Allocs", "Frees", "Live",
        "", "", "", "", table);
}

inline void SmallPool::resetStats() {
    auto& s = SDetail::getStats();
    s.totalAllocs.store(0);
    s.totalFrees.store(0);
    s.currentBytes.store(0);
    s.peakBytes.store(0);
    s.spanGrows.store(0);
    s.cacheRefills.store(0);
    s.cacheFlushes.store(0);
    for (auto& c : s.perClassAllocs) c.store(0);
    for (auto& c : s.perClassFrees)  c.store(0);
}
#endif

EE_NAMESPACE_MEMORIZER_END
