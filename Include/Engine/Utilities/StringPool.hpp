#pragma once
#include <Engine/Core/Types.hpp>
#include <Engine/Core/Allocation.hpp>

#include <memory_resource>
#include <span>

#ifndef EE_STRPOOL_TLS_CACHE_SIZE
#  define EE_STRPOOL_TLS_CACHE_SIZE 128
#endif

EE_NAMESPACE_UTILITIES_BEGIN

template <Size CacheSize = EE_STRPOOL_TLS_CACHE_SIZE>
	requires ((CacheSize & (CacheSize - 1)) == 0) // must be a power of two
class StringPool {
public:
    explicit StringPool(Size shard_count = 64)
        : m_shardCount(round_up_pow2(shard_count))
        , m_shardMask(m_shardCount - 1)
        , m_id(s_nextPoolId.fetch_add(1, std::memory_order_relaxed) + 1)
        , m_shards(new Shard[m_shardCount])
    {}

    EE_NO_COPY(StringPool);

    // Convenient Entrance: no preset hash
    StringView intern(StringView sv) {
        const Size h = std::hash<StringView>{}(sv);
        return intern_with_hash(sv, h);
    }

    // High Performance Entrance: hash has been computed
    StringView intern_with_hash(StringView sv, Size h) {
        // When length == 32, use slow path
        if (sv.size() <= 0xffff'ffffu) [[likely]] {
            Entry& e = s_tlsEntries[h & CacheMask];
            const UInt32 g = m_gen.load(std::memory_order_acquire);

            if (e.ownerId == m_id
                && e.gen == g
                && e.hash32 == static_cast<UInt32>(h)
                && e.len == sv.size()
                && (sv.empty() ||
                    std::memcmp(e.data, sv.data(), sv.size()) == 0)) {
                return StringView(e.data, e.len);
            }

            StringView r = intern_slow(sv, h);
            // The cached pointer stays valid until clear() bumps m_gen. Callers must not
            // call clear() while other threads are inside intern(): the generation check
            // above and the memcmp below are not atomic with respect to the free.
            e = Entry{ r.data(),
                       m_id,
                       static_cast<UInt32>(r.size()),
                       static_cast<UInt32>(h),
                       g,
                       0 };
            return r;
        }
        return intern_slow(sv, h);
    }

    void reserve(Size per_shard) {
        for (Shard& s : shards()) {
            std::lock_guard<Mutex> lock(s.mutex);
            s.set.reserve(per_shard);
        }
    }

    void clear() {
        // Invalid TLS cache first，then release the memory pools
        m_gen.fetch_add(1, std::memory_order_release);
        for (Shard& s : shards()) {
            std::lock_guard<Mutex> lk(s.mutex);
            // Safe in this order: the container's own storage does not come from `mem`
            // (see Shard::set), so dropping the elements and then the character chunks
            // leaves a valid, empty shard behind.
            s.set.clear();
            s.mem.release();
        }
    }

    [[nodiscard]] Size size() const {
        Size total = 0;
        for (const Shard& s : shards()) {
            std::lock_guard<Mutex> lk(s.mutex);
            total += s.set.size();
        }
        return total;
    }

private:
    static constexpr Size CacheMask = CacheSize - 1;

    struct Entry {
        const char* data;
        UInt64 ownerId;
        UInt32 len;
        UInt32 hash32;
        UInt32 gen;
        UInt32 _pad; // Padding to 32 bytes
    };
    static_assert(sizeof(Entry) == 32, "'Entry' must be 32 bytes");

    // Every thread 128 * 32B = 4KB TLS；Initialize by .tbss, no runtime guard
    inline static thread_local Array<Entry, CacheSize> s_tlsEntries;
    // Global unique pool ID: Resolve the identity ambiguity caused by "address-based deconstruction and reconstruction"
    inline static std::atomic<UInt64> s_nextPoolId{ 0 };

    struct TransparentHash {
        using is_transparent = void;
        Size operator()(StringView sv) const noexcept {
            return std::hash<StringView>{}(sv);
        }
    };

    using BucketSet = std::pmr::unordered_set<StringView, TransparentHash, std::equal_to<>>;

    struct Shard {
        mutable Mutex mutex;
        // Interning keeps every string forever, so the chunk allocator is only asked for
        // more memory, never given any back. Its upstream is the engine pool: the
        // characters come from the same allocator as the rest of the engine instead of
        // the CRT, which is one allocation source less to profile and keeps the interning
        // path off the global heap entirely.
        std::pmr::unsynchronized_pool_resource mem{ PoolMemoryResource::get() };
        // The set itself deliberately does *not* use `mem`: clear() releases that pool, and
        // an unordered_set keeps its bucket array (and its size bookkeeping) across
        // clear(). Sharing the pool meant the shard was left with a bucket array inside a
        // released chunk, and the next find()/insert() walked into freed memory - an
        // access violation on the first intern() after a clear(). With the default
        // resource (the global allocator, i.e. the pools when EE_POOL_GLOBAL_NEW is on)
        // the container's own storage always outlives the character pool.
        BucketSet set;
    };

    static constexpr Size round_up_pow2(Size n) {
        Size p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    std::span<Shard>       shards() { return { m_shards.get(), m_shardCount }; }
    std::span<const Shard> shards() const { return { m_shards.get(), m_shardCount }; }

    StringView intern_slow(StringView sv, Size h) {
        Shard& shard = m_shards[h & m_shardMask];
        std::lock_guard<Mutex> lock(shard.mutex);
        auto& s = shard.set;

        if (auto it = s.find(sv); it != s.end()) [[likely]] {
            return *it;
        }
        char* p = static_cast<char*>(shard.mem.allocate(sv.size() + 1, 1));
        if (!sv.empty()) std::memcpy(p, sv.data(), sv.size());
        p[sv.size()] = '\0';
        StringView r(p, sv.size());
        // insert() keeps the view we hand out: the pool owns the characters, and the
        // caller's input buffer is not retained.
        s.insert(r);
        return r;
    }

    Size          m_shardCount;
    Size          m_shardMask;
    UInt64        m_id;
    Uptr<Shard[]> m_shards;

    // Read when hot path，write when clear; Avoid false sharing
    alignas(64) std::atomic<UInt32> m_gen{ 0 };
};

/// @brief The process-wide pool every localized string is interned into.
///
/// Never destroyed, and a single instance for the whole program:
///  - `inline static` gave this internal linkage, so every translation unit including
///    this header got its *own* pool (the "shared" pool was not shared at all);
///  - as an ordinary static it was destroyed during exit, at which point the shards free
///    their memory through the engine pool - which lives in another module with its own
///    static destruction order.
inline StringPool<>& sharedStringPool() {
    // Static storage and placement new: no allocation (with the global pool allocator an
    // allocating singleton would call straight back into the interning path), and no
    // destructor at exit.
    alignas(StringPool<>) static unsigned char storage[sizeof(StringPool<>)];
    static StringPool<>* const shared = ::new (static_cast<void*>(storage)) StringPool<>{};
    return *shared;
}

EE_NAMESPACE_UTILITIES_END