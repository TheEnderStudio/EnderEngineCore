#pragma once

#include <Engine/Core/Types.hpp>
#include <spdlog/fmt/fmt.h>

#include <cstdlib>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#define EE_NAMESPACE_MEMORIZER_BEGIN namespace EnderEngine::Memorizer {
#define EE_NAMESPACE_MEMORIZER_END }

// #define EE_MEMORIZER_NO_STATS

// The statistics are useful but expensive (one atomic RMW per allocation), so they
// follow the engine's debug switch. The guard name used to be misspelled
// (`EE_MOMERIZER_DEBUG`), which made it impossible to opt in or out from the build.
#if !defined(EE_MEMORIZER_DEBUG) && !defined(EE_MEMORIZER_NO_STATS)
# ifdef EE_DEBUG
#  define EE_MEMORIZER_DEBUG
# endif
#endif

EE_NAMESPACE_MEMORIZER_BEGIN

// A type the pool can placement-new: any non-array object type.
template <typename T>
concept Newable = std::is_object_v<T> && !std::is_array_v<T>;

// A type the pool can destroy. (The old `requires { ~T(); }` was ill-formed: an explicit
// destructor call needs an object, so the concept rejected every type it was meant for.)
template <typename T>
concept Deletable = std::is_destructible_v<T>;

namespace PageDetail {

	/// One span is one super page, and both pools rely on it: the deallocator recovers
	/// the span header with `pointer & ~(SuperPageSize - 1)`, so a span has to be exactly
	/// this big and aligned to it.
	inline constexpr Size SuperPageSize = 256 * 1024;

	/**
	 * @brief A process-lifetime singleton that is never destroyed, and never allocated.
	 *
	 * The pools' bookkeeping needs singletons that are safe to reach from *inside* the
	 * allocator. Two things make that harder than `static T* p = new T;`:
	 *
	 *  - `new` goes through the global `operator new`, which with EE_POOL_GLOBAL_NEW is
	 *    the pool itself, so the allocation made while constructing the singleton comes
	 *    straight back into the pool - and if that nested call needs the very singleton
	 *    that is still being initialized, the function-local-static initialisation guard
	 *    is re-entered by the same thread and the process hangs. That is exactly what
	 *    `new SpanSource{}` inside SpanSource::getInstance() did on the first small
	 *    allocation.
	 *  - destroying them at exit would make allocation during static destruction a
	 *    use-after-destroy.
	 *
	 * Placement-new into static storage solves both: no allocation happens, and nothing
	 * is ever destroyed. T must therefore have a constructor that does not allocate
	 * (true for these pools' singletons - they only touch OS primitives).
	 */
	template <typename T>
	T& neverDestroyed() {
		alignas(T) static unsigned char storage[sizeof(T)];
		static T* const instance = ::new (static_cast<void*>(storage)) T{};
		return *instance;
	}

	/**
	 * @brief Hands out super-page aligned spans, carved from larger chunks.
	 *
	 * Asking the OS for one aligned 256 KB block per span made alignment cost up to a
	 * whole extra 256 KB per span (an aligned allocator has to over-allocate to align):
	 * measured as ~1.8x memory for a plain 64-byte workload. Carving spans out of 4 MB
	 * chunks that are themselves aligned costs ~6% instead, and turns one system call per
	 * span into one per sixteen.
	 *
	 * Spans are never returned - a pool keeps them for the lifetime of the process, which
	 * is what makes the header lookup possible - so this hands out memory and never frees
	 * it, on purpose.
	 */
	class SpanSource {
	public:
		static SpanSource& getInstance() {
			// Static storage, never allocated and never destroyed. Constructing this with
			// `new` would go through the engine's global operator new (EE_POOL_GLOBAL_NEW),
			// back into the pool that needs this very object - and re-entering a function-
			// local static's initialisation guard deadlocks on the first allocation. Same
			// pattern as PageDetail::neverDestroyed, written out because the constructor is
			// private. Requires a constructor that does not allocate.
			alignas(SpanSource) static unsigned char storage[sizeof(SpanSource)];
			static SpanSource* const instance = ::new (static_cast<void*>(storage)) SpanSource{};
			return *instance;
		}

		EE_NODISCARD void* allocateSpan() {
			std::lock_guard<Mutex> lock(m_mutex);
			if (m_remaining < SuperPageSize) {
				if (!refill()) return nullptr;
			}
			void* span = m_cur;
			m_cur += SuperPageSize;
			m_remaining -= SuperPageSize;
			++m_spanCount;
			return span;
		}

		EE_NODISCARD Size spanCount() const {
			std::lock_guard<Mutex> lock(m_mutex);
			return m_spanCount;
		}
		EE_NODISCARD Size chunkCount() const {
			std::lock_guard<Mutex> lock(m_mutex);
			return m_chunkCount;
		}

	private:
		static constexpr Size ChunkSize = 4 * 1024 * 1024;

		SpanSource() = default;

		bool refill() {
#if defined(_MSC_VER)
			void* chunk = _aligned_malloc(ChunkSize, SuperPageSize);
#else
			void* chunk = std::aligned_alloc(SuperPageSize, ChunkSize);
#endif
			if (!chunk) return false;
			m_cur = static_cast<UInt8*>(chunk);
			m_remaining = ChunkSize;
			++m_chunkCount;
			return true;
		}

		mutable Mutex m_mutex;
		UInt8* m_cur = nullptr;
		Size   m_remaining = 0;
		Size   m_spanCount = 0;
		Size   m_chunkCount = 0;
	};

} // namespace PageDetail

EE_NAMESPACE_MEMORIZER_END
