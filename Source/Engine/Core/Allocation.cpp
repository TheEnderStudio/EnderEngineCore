#include <Core/Allocation.hpp>
#include <Core/Crash.h>
#include <Core/Types.hpp>
#include <Core/Log.hpp>

#include <Memorizer/SmallPool.hpp>
#include <Memorizer/MediumPool.hpp>
#include <Memorizer/LargeAllocator.hpp>

#include <cstdlib>
#include <new>

using namespace EnderEngine::Memorizer;

EE_NAMESPACE_BEGIN

inline constexpr Size SmallPoolMaxSize = SDetail::MaxPooledSize;
// Above the medium pool's classes it still serves the block itself (one span = one
// block) up to this size; past it the large allocator takes over.
inline constexpr Size MediumPoolMaxSize = MDetail::MaxLargeSize;

inline constexpr UInt64 PoolMagicSmallSpan = SDetail::SpanMagic;
inline constexpr UInt64 PoolMagicMediumSpan = MDetail::SpanMagic;
inline constexpr UInt64 PoolMagicLarge = SDetail::LargeMagic;
inline constexpr UInt64 LargeAllocMagic = 0x4A8FBA528FAD7AFFull;

static_assert(PoolMagicSmallSpan != PoolMagicMediumSpan, "span magics must be distinct");
static_assert(PoolMagicSmallSpan != PoolMagicLarge, "span magic must differ from the large magic");
static_assert(PoolMagicMediumSpan != PoolMagicLarge, "span magic must differ from the large magic");
// The two pools share the large-block magic on purpose: both hand out a super page from
// _aligned_malloc, so freeing one with alignedFree is correct either way. (This used to
// be asserted as *equal* while claiming the opposite in the message.)
static_assert(MDetail::LargeMagic == SDetail::LargeMagic, "large blocks of both pools are freed the same way");

inline constexpr Size SuperPageSize = SDetail::SuperPageSize;
inline constexpr Size SuperPageMask = SuperPageSize - 1;

struct alignas(16) PoolSpanHeaderView {
	UInt64 magic;
    UInt32 sizeClassIndex;
    UInt32 reserved;
};
static_assert(sizeof(PoolSpanHeaderView) == sizeof(SDetail::SpanHeader));
static_assert(sizeof(PoolSpanHeaderView) == sizeof(MDetail::SpanHeader));
static_assert(SDetail::SpanSize == SuperPageSize, "a small-pool span must be one super page");
static_assert(MDetail::SpanSize == SuperPageSize, "a medium-pool span must be one super page");

/// @brief The shared allocator for blocks above the pools' largest size.
///
/// Static storage and never destroyed (see PageDetail::neverDestroyed): constructing it
/// with `new` would call the engine's own operator new with EE_POOL_GLOBAL_NEW enabled,
/// which comes right back here, and re-entering the function-local-static guard deadlocks.
static LargeAllocator& getSharedLargeAllocator() {
    return PageDetail::neverDestroyed<LargeAllocator>();
}

namespace {

/// @brief Depth of nested engineAllocate/engineFree calls on this thread.
///
/// The pools' own bookkeeping allocates: the large allocator keeps a record vector, the
/// fault-commit registry keeps a map, and with EE_POOL_GLOBAL_NEW both allocate through
/// the engine's operator new - i.e. straight back into this file, while the allocator's
/// mutex is held. Serving such a nested request from the pools is a self-deadlock
/// (measured: engineFree -> LargeAllocator::free -> releaseLocked -> registry erase ->
/// free the node -> engineFree -> LargeAllocator::isAllocByThis -> lock the same mutex).
///
/// Nested requests therefore go to a bootstrap allocator that touches nothing but the
/// platform: the OS allocation is its own lock, so no pool lock is ever taken twice.
thread_local UInt32 t_poolDepth = 0;

struct PoolScope {
	PoolScope() { ++t_poolDepth; }
	~PoolScope() { --t_poolDepth; }
	PoolScope(const PoolScope&) = delete;
	PoolScope& operator=(const PoolScope&) = delete;
};

// ---------------------------------------------------------------- bootstrap path

/// Header the bootstrap allocations carry so free() can recognise them.
///
/// It sits immediately before the returned pointer; for a pool pointer the same address is
/// either the span header (a different magic) or the previous object, whose contents are
/// user data - so a false positive would need 64 bits of coincidence.
struct alignas(16) BootstrapHeader {
	UInt64 magic;
	UInt64 size;
};

inline constexpr UInt64 BootstrapMagic = 0xEE00'B057'7A9C'0001ull;

void* bootstrapAllocate(Size size) {
#if defined(_MSC_VER)
	void* base = _aligned_malloc(size + sizeof(BootstrapHeader), 16);
#else
	const Size total = size + sizeof(BootstrapHeader);
	void* base = std::aligned_alloc(16, (total + 15) & ~Size(15));
#endif
	if (!base) return nullptr;
	auto* header = static_cast<BootstrapHeader*>(base);
	header->magic = BootstrapMagic;
	header->size = size;
	return static_cast<UInt8*>(base) + sizeof(BootstrapHeader);
}

/// @return true when @p ptr was a bootstrap allocation (and has been released).
bool bootstrapFree(void* ptr) {
	auto* header = reinterpret_cast<BootstrapHeader*>(static_cast<UInt8*>(ptr) - sizeof(BootstrapHeader));
	if (header->magic != BootstrapMagic) return false;
	header->magic = 0;
#if defined(_MSC_VER)
	_aligned_free(header);
#else
	std::free(header);
#endif
	return true;
}

} // namespace

void* engineAllocate(Size size) {    // A zero-byte request still has to return a distinct pointer (the standard
    // allocator contract), so round it up to the smallest class.
    if (size == 0) size = 1;

    // Nested call from inside the allocator: never take a pool lock (see t_poolDepth).
    if (t_poolDepth != 0) [[unlikely]] return bootstrapAllocate(size);

    PoolScope scope;
    if (size <= SmallPoolMaxSize) return SmallPool::allocate(size);
    if (size <= MediumPoolMaxSize) return MediumPool::allocate(size);
    LargeAllocation alloc;
    if (getSharedLargeAllocator().allocate(size, alloc)) {
        return alloc.base; // the allocator keeps the record; base is the block itself
    }
    ECrash("Unable to allocate memory (Size: %ull Bytes)", size);
    return nullptr; // ECrash terminates; this keeps the compiler from warning C4715
}

namespace {

// The pools guarantee 16 bytes; anything stricter goes to the platform allocator.
inline constexpr Size PoolAlignment = 16;

void* alignedAllocRaw(Size size, Size alignment) {
    if (alignment <= PoolAlignment) return engineAllocate(size);
#if defined(_MSC_VER)
    return _aligned_malloc(size ? size : 1, alignment);
#else
    // aligned_alloc wants a size that is a multiple of the alignment.
    const Size rounded = (size + alignment - 1) & ~(alignment - 1);
    return std::aligned_alloc(alignment, rounded ? rounded : alignment);
#endif
}

void alignedFreeRaw(void* ptr, Size alignment) {
    if (alignment <= PoolAlignment) { engineFree(ptr); return; }
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace

void* engineAllocateAligned(Size size, Size alignment) { return alignedAllocRaw(size, alignment); }
void engineFreeAligned(void* ptr, Size alignment) {
    if (!ptr) return;
    alignedFreeRaw(ptr, alignment);
}

bool engineUsesPoolGlobalAllocator() noexcept {
#ifdef EE_POOL_GLOBAL_NEW
    return true;
#else
    return false;
#endif
}

void Detail::poolOutOfMemory() {

#if defined(_CPPUNWIND) || defined(__cpp_exceptions)
    throw std::bad_alloc{};
#else
    std::abort();
#endif
}

PoolMemoryResource* PoolMemoryResource::get() noexcept {
    // Static storage, never allocated and never destroyed: constructing the resource with
    // `new` would re-enter the pool through the engine's global operator new, and it has
    // to outlive every container that still holds memory (see PageDetail::neverDestroyed).
    alignas(PoolMemoryResource) static unsigned char storage[sizeof(PoolMemoryResource)];
    static PoolMemoryResource* const instance = ::new (static_cast<void*>(storage)) PoolMemoryResource{};
    return instance;
}

void* PoolMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    void* p = alignedAllocRaw(bytes, alignment);
    if (!p) [[unlikely]] Detail::poolOutOfMemory();
    return p;
}

void PoolMemoryResource::do_deallocate(void* ptr, std::size_t, std::size_t alignment) {
    if (!ptr) return;
    alignedFreeRaw(ptr, alignment);
}

void engineFree(void* ptr) {	if (!ptr) [[unlikely]] return;

    // Nested free, from inside the allocator's own bookkeeping (see t_poolDepth). Only a
    // bootstrap block can legitimately be freed here; taking a pool lock now is what hung
    // the first version of this dispatcher, because the caller holds it already.
    if (t_poolDepth != 0) [[unlikely]] {
        if (bootstrapFree(ptr)) return;
        ECrash("engineFree() re-entered with a pool pointer (%p): a pool's bookkeeping must "
            "not free pool memory (see bootstrapAllocate in Allocation.cpp).", ptr);
        return;
    }

    PoolScope scope;

    // Blocks from the large allocator come straight from VirtualAlloc and carry no
    // header, so the masked-header trick below cannot find them (and on a 256 KB-masked
    // address it would read unmapped memory). The lookup is an exact pointer match, so a
    // pool pointer can never accidentally match a large block.
    if (getSharedLargeAllocator().isAllocByThis(ptr)) {
        if (getSharedLargeAllocator().free(ptr)) return;
    }

    // Pooled and large-page blocks live in super pages whose base holds a header.
	const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	auto* header = reinterpret_cast<PoolSpanHeaderView*>(addr & ~SuperPageMask);

    switch (header->magic) {
    case PoolMagicSmallSpan:
        SmallPool::deallocate(ptr);
        break;
    case PoolMagicMediumSpan:
        MediumPool::deallocate(ptr);
        break;
    case PoolMagicLarge:
        SDetail::alignedFree(header);
        break;
    default:
        //ECrash("Try to free an unknown pointer (%p)", ptr);
        break;
    }
}

void printAllocationStatistics() {
#ifdef EE_MEMORIZER_DEBUG
    String smallStats = SmallPool::dumpStats();
    String mediumStats = MediumPool::dumpStats();
    LargeAllocStats largeAllocStats = getSharedLargeAllocator().getStats();
    String largeStats = fmt::format(R"(
=== LargeAllocator Statistics ===
Reserved Bytes  : {}
Committed Bytes : {}
Block Count     : {}
Huge Blocks     : {}
Large Blocks    : {}
Normal Blocks   : {})",
largeAllocStats.reservedBytes,
largeAllocStats.committedBytes,
largeAllocStats.blockCount,
largeAllocStats.hugeBlocks,
largeAllocStats.largeBlocks,
largeAllocStats.normalBlocks);

    EInfo("Memory Pools Statistics:\n{}\n{}\n{}", smallStats, mediumStats, largeStats);
#else
    EWarn("Statistics of Memory Pools is unavailable");
#endif
}

EE_NAMESPACE_END
