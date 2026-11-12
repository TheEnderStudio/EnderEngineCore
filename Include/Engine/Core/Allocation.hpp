#pragma once

#include "Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

EE_NAMESPACE_BEGIN

using Size = size_t;

/**
 * @brief Allocate @p size bytes from the engine's own allocator.
 *
 * Three tiers back it (see Libraries/Memorizer):
 *  - up to 4 KB   a small-object pool: per-size-class spans, refilled in batches through
 *                 a thread-local cache, so the common allocation is a pointer bump on a
 *                 thread-private free list;
 *  - up to 256 KB a medium-object pool with the same design and coarser classes;
 *  - above that   the large allocator: VirtualAlloc, optionally large/huge pages, with
 *                 reserve/commit-on-demand.
 *
 * The result is at least 16-byte aligned and behaves like malloc's. Free it with
 * engineFree(), which recovers the owning pool from the block itself; that is also why
 * free() needs no size.
 */
EE_NODISCARD EE_API void* engineAllocate(Size size);
EE_API void engineFree(void* ptr);

/**
 * @brief engineAllocate with an explicit alignment.
 *
 * The pools only guarantee 16 bytes, so anything stricter goes to the platform's aligned
 * allocator. Free the result with engineFreeAligned() and the *same* alignment.
 */
EE_NODISCARD EE_API void* engineAllocateAligned(Size size, Size alignment);
EE_API void engineFreeAligned(void* ptr, Size alignment);

/**
 * @brief Whether this module routes plain new/delete through the engine pools.
 *
 * True only when the module was built with `--pool_allocator` (EE_POOL_GLOBAL_NEW). The
 * switch changes the allocator ABI: a module built one way must not hand ownership of its
 * memory to a module built the other way, so on startup the engine logs which side it is
 * on. Mixing the two (for example a stale extension DLL left over from the other
 * configuration) is what makes unrelated allocations crash later on.
 */
EE_NODISCARD EE_API bool engineUsesPoolGlobalAllocator() noexcept;

namespace Detail {
	/// @brief What an out-of-memory condition does: throws when exceptions are enabled,
	/// aborts otherwise (the release configuration builds with /EHs-c- / -fno-exceptions).
	[[noreturn]] EE_API void poolOutOfMemory();
} // namespace Detail

/**
 * @brief STL allocator that serves from the engine pools.
 *
 * This is the least invasive way to move a container (or a whole subsystem's) off the
 * global heap: it is stateless, so the container's type only gains a template argument,
 * there is no per-container resource pointer, and two allocators of different value
 * types always compare equal (deallocate needs no size, which the pools do not need
 * either).
 *
 * @code
 * using PoolVector = std::vector<T, EnderEngine::PoolAllocator<T>>;
 * @endcode
 *
 * Types aligned beyond 16 bytes cannot come from the pools and transparently fall back
 * to the platform's aligned allocator.
 */
template <typename T>
class PoolAllocator {
public:
	using value_type = T;
	using is_always_equal = std::true_type;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_copy_assignment = std::true_type;
	using propagate_on_container_swap = std::true_type;

	PoolAllocator() noexcept = default;
	template <typename U>
	PoolAllocator(const PoolAllocator<U>&) noexcept {}

	EE_NODISCARD T* allocate(std::size_t count) {
		if (count > static_cast<std::size_t>(-1) / sizeof(T)) Detail::poolOutOfMemory();
		const Size bytes = count * sizeof(T);
		void* p = engineAllocateAligned(bytes, AlignOf());
		if (!p) [[unlikely]] Detail::poolOutOfMemory();
		return static_cast<T*>(p);
	}

	void deallocate(T* ptr, std::size_t) noexcept {
		engineFreeAligned(ptr, AlignOf());
	}

	template <typename U>
	bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
	template <typename U>
	bool operator!=(const PoolAllocator<U>&) const noexcept { return false; }

private:
	static constexpr Size AlignOf() noexcept {
		return alignof(T) < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignof(T);
	}
};

/**
 * @brief std::pmr memory resource backed by the engine pools.
 *
 * For pmr containers, and for hooking an allocation that already takes a resource (the
 * StringPool hands its shard pool to one of these, so interning stops going through the
 * CRT one chunk at a time).
 */
class EE_API PoolMemoryResource final : public std::pmr::memory_resource {
public:
	static PoolMemoryResource* get() noexcept;

	PoolMemoryResource(const PoolMemoryResource&) = delete;
	PoolMemoryResource& operator=(const PoolMemoryResource&) = delete;

private:
	PoolMemoryResource() = default;

	void* do_allocate(std::size_t bytes, std::size_t alignment) override;
	void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override;
	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
		return this == &other;
	}
};

EE_API void printAllocationStatistics();

EE_NAMESPACE_END
