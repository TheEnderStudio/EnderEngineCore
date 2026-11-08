// ======================================================================================
// Memorizer benchmark
//
// Compares the pools against the CRT heap and, more importantly, measures the shapes an
// engine actually allocates in: a live set that grows and is freed in a different order,
// mixed sizes, and several threads allocating at the same time.
//
// Two notes on reading the numbers:
//  - Build it in Release (`xmake f -m release`) for the real cost. Debug builds compile
//    with set_optimize("none"), and EE_MEMORIZER_DEBUG adds two atomic RMWs per
//    allocation, which dominates everything else.
//  - A timing that comes out of a broken allocator is worthless, so main() runs a
//    pattern-verifying self-check first and aborts on corruption.
// ======================================================================================
#include <Memorizer/SmallPool.hpp>
#include <Memorizer/MediumPool.hpp>
#include <Memorizer/LargeAllocator.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
# include <windows.h>
# include <psapi.h>
# pragma comment(lib, "psapi.lib")
#endif

using namespace EnderEngine;
using namespace EnderEngine::Memorizer;

using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------- timing helpers

template <typename T>
inline void doNotOptimize(T const& value) {
#if defined(_MSC_VER)
	// A volatile local alone does not stop MSVC from moving or merging the allocation,
	// so fence on both sides of it.
	std::atomic_signal_fence(std::memory_order_seq_cst);
	volatile auto* p = const_cast<volatile T*>(&value);
	(void)p;
	std::atomic_signal_fence(std::memory_order_seq_cst);
#else
	asm volatile("" : : "r,m"(value) : "memory");
#endif
}

struct BenchResult {
	double elapsedMs = 0.0;
	UInt64 ops = 0;
	UInt64 bytes = 0;

	double nsPerOp() const { return elapsedMs * 1e6 / static_cast<double>(ops); }
	double mopsPerSec() const { return static_cast<double>(ops) / elapsedMs / 1000.0; }
	double mbPerSec() const {
		return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
	}
};

/// Runs @p fn `repeats` times (plus one warmup) and keeps the best time.
template <typename Fn>
BenchResult timeIt(UInt64 ops, UInt64 bytes, Fn&& fn, int repeats = 3) {
	fn(); // warmup: grows the spans, fills the thread caches, warms the CPU
	double bestMs = 1e300;
	for (int i = 0; i < repeats; ++i) {
		const auto t0 = Clock::now();
		fn();
		const auto t1 = Clock::now();
		const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		bestMs = std::min(bestMs, ms);
	}
	return { bestMs, ops, bytes };
}

/// @brief A fixed set of threads that all start a round together.
///
/// The old benchmark created and joined its threads inside the timed region, which on a
/// 16M-op round is a measurable part of the result. Here the threads live across rounds
/// and only the barrier and the work are timed.
class ThreadPool {
public:
	explicit ThreadPool(int threads) : m_pending(threads) {
		m_threads.reserve(static_cast<size_t>(threads));
		for (int t = 0; t < threads; ++t) {
			m_threads.emplace_back([this, t] { worker(t); });
		}
	}

	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
			++m_generation;
			m_cv.notify_all();
		}
		for (auto& th : m_threads) th.join();
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	/// Runs @p body(tid) on every thread at once; returns the wall time of the round.
	double run(const std::function<void(int)>& body) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_body = &body;
			m_done = 0;
			++m_generation;
			m_cv.notify_all();
		}
		const auto t0 = Clock::now();
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_doneCv.wait(lock, [this] { return m_done == m_pending; });
		}
		const auto t1 = Clock::now();
		return std::chrono::duration<double, std::milli>(t1 - t0).count();
	}

	[[nodiscard]] int size() const { return m_pending; }

private:
	void worker(int tid) {
		int seen = 0; // the generation counter starts at 0 and run() increments it
		for (;;) {
			const std::function<void(int)>* body = nullptr;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [&] { return m_stop || m_generation != seen; });
				if (m_stop) return;
				seen = m_generation;
				body = m_body;
			}
			if (body) (*body)(tid);
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				++m_done;
			}
			m_doneCv.notify_one();
		}
	}

	std::vector<std::thread> m_threads;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::condition_variable m_doneCv;
	const std::function<void(int)>* m_body = nullptr;
	int m_generation = 0;
	int m_pending = 0;
	int m_done = 0;
	bool m_stop = false;
};

// ---------------------------------------------------------------- small utilities

/// @brief Stamps a block so a later check can prove it survived.
///
/// First and last byte only - this runs inside timed loops - with the one-byte case
/// written once (writing first *and* last there would overwrite the first byte).
inline void touch(void* p, Size size, unsigned char seed) {
	auto* b = static_cast<unsigned char*>(p);
	if (size <= 1) { b[0] = seed; return; }
	b[0] = seed;
	b[size - 1] = static_cast<unsigned char>(seed ^ 0xA5);
}

inline bool intact(const void* p, Size size, unsigned char seed) {
	const auto* b = static_cast<const unsigned char*>(p);
	if (size <= 1) return b[0] == seed;
	return b[0] == seed && b[size - 1] == static_cast<unsigned char>(seed ^ 0xA5);
}

[[nodiscard]] Size processPrivateBytes() {
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
		return static_cast<Size>(pmc.PrivateUsage);
	}
#endif
	return 0;
}

// ---------------------------------------------------------------- self check

[[noreturn]] void fatal(const char* what) {
	std::fprintf(stderr, "[self-check] FAILED: %s\n", what);
	std::fflush(stderr);
	std::abort();
}

/// @brief One allocation from the tier its size belongs to (mirrors engineAllocate).
struct TieredBlock {
	void* ptr = nullptr;
	int tier = 0; // 0 = small pool, 1 = medium pool, 2 = large allocator
};

TieredBlock allocateTier(Size size, LargeAllocator& large) {
	TieredBlock b;
	if (size <= SDetail::MaxPooledSize) { b.ptr = SmallPool::allocate(size); b.tier = 0; }
	else if (size <= MDetail::MaxLargeSize) { b.ptr = MediumPool::allocate(size); b.tier = 1; }
	else {
		LargeAllocation a;
		if (!large.allocate(size, a)) return b;
		b.ptr = a.base;
		b.tier = 2;
	}
	return b;
}

void freeTier(const TieredBlock& b, LargeAllocator& large) {
	if (!b.ptr) return;
	if (b.tier == 0) SmallPool::deallocate(b.ptr);
	else if (b.tier == 1) MediumPool::deallocate(b.ptr);
	else large.free(b.ptr); // exact-match lookup, the same path engineFree takes
}

/// @brief Verifies every tier before any number is measured.
void selfCheck() {
	// Sizes that land in every class, at both tier boundaries and in the large path.
	const Size sizes[] = {
		1, 15, 16, 17, 48, 63, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4095, 4096,
		4097, 5120, 6144, 8192, 10240, 12288, 14336, 16384, 16385, 20000, 65536, 131072, 262128, 262129,
		300000, 1u << 20,
	};

	LargeAllocator large;
	for (Size size : sizes) {
		const TieredBlock b = allocateTier(size, large);
		void* p = b.ptr;
		if (!p) { std::fprintf(stderr, "  size %zu\n", size); fatal("allocation returned null"); }
		if (reinterpret_cast<uintptr_t>(p) % 16 != 0) { std::fprintf(stderr, "  size %zu\n", size); fatal("allocation is not 16-byte aligned"); }
		touch(p, size, static_cast<unsigned char>(size & 0xFF));
		if (!intact(p, size, static_cast<unsigned char>(size & 0xFF))) { std::fprintf(stderr, "  size %zu\n", size); fatal("block was corrupted while live"); }
		freeTier(b, large);
	}

	// A live set large enough to span several super pages, verified and freed out of order.
	constexpr Size kCount = 20000;
	std::vector<void*> blocks(kCount);
	constexpr Size kSize = 64;
	for (Size i = 0; i < kCount; ++i) {
		blocks[i] = SmallPool::allocate(kSize);
		if (!blocks[i]) fatal("live-set allocation returned null");
		touch(blocks[i], kSize, static_cast<unsigned char>(i));
	}
	for (Size i = 0; i < kCount; ++i) {
		if (!intact(blocks[i], kSize, static_cast<unsigned char>(i))) fatal("live-set block was corrupted");
	}
	std::vector<Size> order(kCount);
	std::iota(order.begin(), order.end(), Size(0));
	std::mt19937 rng(0xF00D);
	std::shuffle(order.begin(), order.end(), rng);
	for (Size idx : order) SmallPool::deallocate(blocks[idx]);

	// Freeing a block through the wrong pool has to be *detected*, not corrupt the heap.
	// (Only meaningful with EE_MEMORIZER_DEBUG, where the span magic is validated.)
	std::printf("[self-check] all tiers, alignment and a 20k live set verified\n");
}

// ---------------------------------------------------------------- reporting

void printHeader() {
	std::printf("%-30s | %10s | %12s | %9s | %10s | %9s\n",
		"Benchmark", "Pool(ns)", "malloc(ns)", "Speedup", "Pool Mops/s", "Pool MB/s");
	std::printf("%s\n", std::string(112, '-').c_str());
}

void report(const std::string& name, const BenchResult& pool, const BenchResult& sys) {
	const double speedup = sys.nsPerOp() / pool.nsPerOp();
	std::printf("%-30s | %10.2f | %12.2f | %8.2fx | %10.1f | %9.1f\n",
		name.c_str(), pool.nsPerOp(), sys.nsPerOp(), speedup,
		pool.mopsPerSec(), pool.mbPerSec());
}

// ---------------------------------------------------------------- benchmarks

using AllocFn = void* (*)(Size);
using FreeFn = void (*)(void*);

struct Allocator { const char* name; AllocFn alloc; FreeFn free_; };

const Allocator kSmallPool = { "SmallPool", &SmallPool::allocate, &SmallPool::deallocate };
const Allocator kMediumPool = { "MediumPool", &MediumPool::allocate, &MediumPool::deallocate };
const Allocator kMalloc = { "malloc", &std::malloc, &std::free };

/// allocate/free of one size, one at a time.
BenchResult benchAlternate(const Allocator& a, Size size, Size iters) {
	const UInt64 bytes = static_cast<UInt64>(iters) * size;
	return timeIt(iters, bytes, [&] {
		for (Size i = 0; i < iters; ++i) {
			void* p = a.alloc(size);
			doNotOptimize(p);
			a.free_(p);
		}
	});
}

/// Allocate a whole live set, then free it (the batch pattern of a frame's temporaries).
BenchResult benchLiveSet(const Allocator& a, Size size, Size count, Size rounds) {
	const UInt64 ops = static_cast<UInt64>(count) * rounds * 2; // alloc + free
	const UInt64 bytes = static_cast<UInt64>(count) * rounds * size;
	std::vector<void*> ptrs(count);
	return timeIt(ops, bytes, [&] {
		for (Size r = 0; r < rounds; ++r) {
			for (Size i = 0; i < count; ++i) ptrs[i] = a.alloc(size);
			doNotOptimize(ptrs.data());
			for (Size i = 0; i < count; ++i) a.free_(ptrs[i]);
		}
	});
}

/// Allocate a live set, then free it in a shuffled order - what a scene/frame teardown
/// actually looks like, and the order that exercises the free lists instead of hitting
/// the same freshly-pushed nodes.
BenchResult benchShuffledFree(const Allocator& a, Size size, Size count, Size rounds, unsigned seed) {
	const UInt64 ops = static_cast<UInt64>(count) * rounds * 2;
	const UInt64 bytes = static_cast<UInt64>(count) * rounds * size;
	std::vector<void*> ptrs(count);
	std::vector<Size> order(count);
	std::iota(order.begin(), order.end(), Size(0));
	std::mt19937 rng(seed);
	return timeIt(ops, bytes, [&] {
		for (Size r = 0; r < rounds; ++r) {
			for (Size i = 0; i < count; ++i) ptrs[i] = a.alloc(size);
			std::shuffle(order.begin(), order.end(), rng);
			for (Size i = 0; i < count; ++i) a.free_(ptrs[order[i]]);
		}
	});
}

/// Random size classes, live set then free - the mixed workload of a real frame.
BenchResult benchMixed(const Allocator& a, Size count, Size rounds, unsigned seed) {
	std::mt19937 rng(seed);
	std::uniform_int_distribution<Size> classDist(0, SDetail::NumSizeClasses - 1);
	std::vector<Size> sizes(count);
	UInt64 bytes = 0;
	for (Size i = 0; i < count; ++i) {
		sizes[i] = SDetail::SizeClasses[classDist(rng)];
		bytes += sizes[i];
	}
	std::vector<void*> ptrs(count);
	const UInt64 ops = static_cast<UInt64>(count) * rounds * 2;
	bytes *= rounds;
	return timeIt(ops, bytes, [&] {
		for (Size r = 0; r < rounds; ++r) {
			for (Size i = 0; i < count; ++i) ptrs[i] = a.alloc(sizes[i]);
			doNotOptimize(ptrs.data());
			for (Size i = 0; i < count; ++i) a.free_(ptrs[i]);
		}
	});
}

/// Same body on N threads at once, threads reused across rounds.
BenchResult benchThreads(const Allocator& a, Size size, Size perThread, Size rounds) {
	const int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
	ThreadPool pool(threads);
	const UInt64 ops = static_cast<UInt64>(perThread) * static_cast<UInt64>(threads) * rounds * 2;
	const UInt64 bytes = ops / 2 * size;

	const std::function<void(int)> body = [&, size](int) {
		for (Size i = 0; i < perThread; ++i) {
			void* p = a.alloc(size);
			doNotOptimize(p);
			a.free_(p);
		}
	};

	pool.run(body); // warmup: fills every thread's cache and grows the spans
	double bestMs = 1e300;
	for (Size r = 0; r < rounds; ++r) bestMs = std::min(bestMs, pool.run(body));
	return { bestMs, ops, bytes };
}

/// 1 MB alloc/free of the large allocator, reserving and releasing per iteration.
BenchResult benchLargeAlternate(Size size, Size iters) {
	LargeAllocator large;
	return timeIt(iters, static_cast<UInt64>(iters) * size, [&] {
		for (Size i = 0; i < iters; ++i) {
			LargeAllocation a;
			if (!large.allocate(size, a)) fatal("large allocation failed");
			doNotOptimize(a.base);
			large.free(a);
		}
	});
}

/// Per-size-class throughput of the small pool, one row per class.
void sweepSizeClasses() {
	std::printf("\n%-8s | %12s | %12s | %10s\n", "Class", "Pool(ns)", "malloc(ns)", "Speedup");
	std::printf("%s\n", std::string(56, '-').c_str());
	for (UInt32 i = 0; i < SDetail::NumSizeClasses; ++i) {
		const Size size = SDetail::SizeClasses[i];
		const Size iters = 400'000;
		const auto pool = benchAlternate(kSmallPool, size, iters);
		const auto sys = benchAlternate(kMalloc, size, iters);
		std::printf("%-8zu | %12.2f | %12.2f | %9.2fx\n",
			size, pool.nsPerOp(), sys.nsPerOp(), sys.nsPerOp() / pool.nsPerOp());
	}
}

} // namespace

int main() {
	const int hw = static_cast<int>(std::thread::hardware_concurrency());
	std::printf("=== Memorizer benchmark ===\n");
	std::printf("Hardware threads : %d\n", hw);
#ifdef EE_MEMORIZER_DEBUG
	std::printf("Build mode       : Debug/statistics ON - timings include two atomic RMWs per call.\n");
	std::printf("                   Rebuild with `xmake f -m release` for the real cost.\n");
#else
	std::printf("Build mode       : no statistics\n");
#endif
	std::printf("\n");

	selfCheck();
	std::printf("\n");

	printHeader();

	// 1) One size, allocate/free alternately: the pointer-chase cost of the free list.
	report("alternate-64", benchAlternate(kSmallPool, 64, 10'000'000), benchAlternate(kMalloc, 64, 10'000'000));
	report("alternate-1024", benchAlternate(kSmallPool, 1024, 5'000'000), benchAlternate(kMalloc, 1024, 5'000'000));
	report("alternate-4096", benchAlternate(kSmallPool, 4096, 2'000'000), benchAlternate(kMalloc, 4096, 2'000'000));

	// 2) A live set of 4096 x 64 B, allocated then freed: what a frame's temporaries do.
	report("live-set-64 x4096", benchLiveSet(kSmallPool, 64, 4096, 1000), benchLiveSet(kMalloc, 64, 4096, 1000));

	// 3) Same live set, freed in a shuffled order.
	report("shuffled-free-64 x4096", benchShuffledFree(kSmallPool, 64, 4096, 1000, 7),
		benchShuffledFree(kMalloc, 64, 4096, 1000, 7));

	// 4) Mixed size classes - the realistic "many different allocations per frame" case.
	report("mixed-classes x8192", benchMixed(kSmallPool, 8192, 200, 11), benchMixed(kMalloc, 8192, 200, 11));

	// 5) Medium pool: 8 KB objects come from their own span set.
	report("alternate-8192 (medium)", benchAlternate(kMediumPool, 8192, 1'000'000), benchAlternate(kMalloc, 8192, 1'000'000));
	report("live-set-8192 x512", benchLiveSet(kMediumPool, 8192, 512, 500), benchLiveSet(kMalloc, 8192, 512, 500));

	// 6) The large allocator: 1 MB blocks, reserve now / commit on first touch by
	//    design, so this is the cost of the reservation and the record keeping.
	report("alternate-1MB (large)", benchLargeAlternate(1u << 20, 20'000), benchAlternate(kMalloc, 1u << 20, 20'000));

	// 7) Concurrency: the same work on every hardware thread, threads reused.
	report("threads-64 (all cores)", benchThreads(kSmallPool, 64, 2'000'000, 3), benchThreads(kMalloc, 64, 2'000'000, 3));

	// 8) Per-class table, useful for spotting a class that behaves oddly.
	sweepSizeClasses();

	// 9) Footprint of a large live set (what the pool actually has to hold).
	std::printf("\n=== Footprint of a 200k x 64 B live set ===\n");
	const Size before = processPrivateBytes();
	{
		std::vector<void*> blocks(200'000);
		for (auto& p : blocks) p = SmallPool::allocate(64);
		const Size peak = processPrivateBytes();
		std::printf("private bytes: %+.1f MB while live (12.8 MB of payload)\n",
			(static_cast<double>(peak) - static_cast<double>(before)) / (1024.0 * 1024.0));
		for (void* p : blocks) SmallPool::deallocate(p);
	}

#ifdef EE_MEMORIZER_DEBUG
	std::printf("\n");
	SmallPool::dumpStats();
	MediumPool::dumpStats();
#endif

	return 0;
}
