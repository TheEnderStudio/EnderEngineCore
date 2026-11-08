#include <Engine/Core/Allocation.hpp>

#include <Memorizer/SmallPool.hpp>
#include <Memorizer/MediumPool.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace EnderEngine;

namespace {

/// Every size class boundary plus the two pool transitions and the large allocator.
constexpr Size kProbeSizes[] = {
	0, 1, 15, 16, 17, 31, 32, 33, 48, 63, 64, 96, 127, 128, 192, 256,
	384, 512, 768, 1024, 1536, 2048, 3072, 4095, 4096,       // small pool
	4097, 5120, 6144, 8192, 10240, 12288, 14336, 16384,      // medium pool classes
	16385, 20000, 262128, 262129, 300000, 1u << 20,          // medium large path / large allocator
};

void fillPattern(void* p, Size size, unsigned char seed) {
	auto* b = static_cast<unsigned char*>(p);
	for (Size i = 0; i < size; ++i) b[i] = static_cast<unsigned char>(seed + i);
}

bool checkPattern(const void* p, Size size, unsigned char seed) {
	const auto* b = static_cast<const unsigned char*>(p);
	for (Size i = 0; i < size; ++i) {
		if (b[i] != static_cast<unsigned char>(seed + i)) return false;
	}
	return true;
}

struct alignas(32) OverAligned {
	unsigned char bytes[96];
};

} // namespace

TEST(AllocationTest, EveryTierAllocatesAndFrees) {
	for (Size size : kProbeSizes) {
		void* p = engineAllocate(size);
		ASSERT_NE(p, nullptr) << "size " << size;
		// The pools only guarantee 16 bytes, which is also the malloc contract.
		EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u) << "size " << size;

		const Size write = size ? size : 1;
		fillPattern(p, write, static_cast<unsigned char>(size & 0xff));
		EXPECT_TRUE(checkPattern(p, write, static_cast<unsigned char>(size & 0xff))) << "size " << size;
		engineFree(p);
	}
}

TEST(AllocationTest, ZeroSizeReturnsDistinctPointers) {
	void* a = engineAllocate(0);
	void* b = engineAllocate(0);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_NE(a, b);
	engineFree(a);
	engineFree(b);
}

TEST(AllocationTest, FreeNullIsANoop) {
	engineFree(nullptr);
	engineFreeAligned(nullptr, 32);
	SUCCEED();
}

TEST(AllocationTest, LargeAllocationsFreeThroughTheRightPath) {
	// These used to deadlock (the large allocator re-locked its own mutex from
	// free(void*)) and, for sizes between 256 KB and the fault-commit granularity,
	// hand out reserved-but-uncommitted memory.
	for (Size size : { Size(262129), Size(300000), Size(1u << 20), Size(3u << 20) }) {
		void* p = engineAllocate(size);
		ASSERT_NE(p, nullptr) << "size " << size;
		fillPattern(p, size, 0x5A);
		EXPECT_TRUE(checkPattern(p, size, 0x5A)) << "size " << size;
		engineFree(p);
	}
}

TEST(AllocationTest, ManySmallAllocationsSpanMultipleSpans) {
	constexpr Size kCount = 20000;
	constexpr Size kSize = 64; // spans hold ~4000 of these, so this forces several grows
	std::vector<void*> ptrs(kCount);
	for (Size i = 0; i < kCount; ++i) {
		ptrs[i] = engineAllocate(kSize);
		ASSERT_NE(ptrs[i], nullptr);
		fillPattern(ptrs[i], kSize, static_cast<unsigned char>(i));
	}
	for (Size i = 0; i < kCount; ++i) {
		EXPECT_TRUE(checkPattern(ptrs[i], kSize, static_cast<unsigned char>(i))) << "index " << i;
	}
	// Free in a shuffled order so the thread cache and the central free lists both see
	// interleaved pushes.
	std::vector<Size> order(kCount);
	std::iota(order.begin(), order.end(), Size(0));
	std::mt19937 rng(1234);
	std::shuffle(order.begin(), order.end(), rng);
	for (Size idx : order) engineFree(ptrs[idx]);
}

TEST(AllocationTest, MixedSizesRoundTrip) {
	std::mt19937 rng(0xBEEF);
	std::uniform_int_distribution<Size> dist(1, 4000);
	std::vector<std::pair<void*, Size>> blocks;
	blocks.reserve(5000);
	for (int i = 0; i < 5000; ++i) {
		const Size size = dist(rng);
		void* p = engineAllocate(size);
		ASSERT_NE(p, nullptr);
		fillPattern(p, size, static_cast<unsigned char>(i));
		blocks.emplace_back(p, size);
	}
	for (auto& [p, size] : blocks) engineFree(p);
}

TEST(AllocationTest, AlignedAllocateHonoursAlignment) {
	for (Size alignment : { Size(16), Size(32), Size(64), Size(128) }) {
		void* p = engineAllocateAligned(257, alignment);
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignment, 0u) << "alignment " << alignment;
		fillPattern(p, 257, 0x11);
		EXPECT_TRUE(checkPattern(p, 257, 0x11));
		engineFreeAligned(p, alignment);
	}
}

TEST(AllocationTest, PoolAllocatorDrivesStdContainers) {
	std::vector<int, PoolAllocator<int>> v;
	for (int i = 0; i < 10000; ++i) v.push_back(i); // exercises several growths, each a pool block
	ASSERT_EQ(v.size(), 10000u);
	for (int i = 0; i < 10000; ++i) EXPECT_EQ(v[static_cast<size_t>(i)], i);

	std::vector<std::pair<int, std::vector<int, PoolAllocator<int>>>, PoolAllocator<std::pair<int, std::vector<int, PoolAllocator<int>>>>> nested;
	for (int i = 0; i < 100; ++i) {
		nested.emplace_back();
		nested.back().first = i;
		nested.back().second.assign(static_cast<size_t>(i + 1), i);
	}
	EXPECT_EQ(nested.size(), 100u);
	EXPECT_EQ(nested[42].first, 42);
	EXPECT_EQ(nested[42].second.size(), 43u);
}

TEST(AllocationTest, PoolAllocatorHandlesOverAlignedTypes) {
	// alignof == 32 is more than the pools guarantee, so the allocator has to fall back
	// to the platform's aligned allocator - and free it the same way.
	std::vector<OverAligned, PoolAllocator<OverAligned>> v(8);
	for (int i = 0; i < 8; ++i) {
		EXPECT_EQ(reinterpret_cast<uintptr_t>(&v[static_cast<size_t>(i)]) % 32, 0u);
		v[static_cast<size_t>(i)].bytes[0] = static_cast<unsigned char>(i);
	}
	EXPECT_EQ(v[7].bytes[0], 7);
}

TEST(AllocationTest, PoolMemoryResourceForPmrContainers) {
	std::pmr::vector<int> v(PoolMemoryResource::get());
	for (int i = 0; i < 4000; ++i) v.push_back(i);
	ASSERT_EQ(v.size(), 4000u);
	EXPECT_EQ(v[1234], 1234);

	std::pmr::string s(PoolMemoryResource::get());
	for (int i = 0; i < 2000; ++i) s.push_back('x');
	EXPECT_EQ(s.size(), 2000u);
	EXPECT_EQ(PoolMemoryResource::get()->is_equal(*PoolMemoryResource::get()), true);
}

TEST(AllocationTest, ConcurrentAllocation) {
	constexpr int kThreads = 4;
	constexpr int kIterations = 20000;
	std::atomic<int> failures{ 0 };
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([t, &failures] {
			std::mt19937 rng(static_cast<unsigned>(t) + 7u);
			std::uniform_int_distribution<Size> dist(1, 3000);
			std::vector<void*> live;
			live.reserve(32);
			for (int i = 0; i < kIterations; ++i) {
				const Size size = dist(rng);
				void* p = engineAllocate(size);
				if (!p) { ++failures; continue; }
				fillPattern(p, size, static_cast<unsigned char>(i));
				if (!checkPattern(p, size, static_cast<unsigned char>(i))) ++failures;
				live.push_back(p);
				if (live.size() >= 8) {
					for (void* q : live) engineFree(q);
					live.clear();
				}
			}
			for (void* q : live) engineFree(q);
		});
	}
	for (auto& th : threads) th.join();
	EXPECT_EQ(failures.load(), 0);
}

TEST(AllocationTest, PoolSingletonsLiveInStaticStorage) {
	// Regression: the pool singletons were created with `new`. With the engine's global
	// operator new enabled (xmake f --pool_allocator=y) that allocation comes straight
	// back into the pool, which needs the very singleton that is still being initialized,
	// and the function-local-static guard is re-entered by the same thread - the process
	// hangs on the first allocation. They must be plain static storage now: stable
	// addresses, no allocation.
	auto& span1 = Memorizer::PageDetail::SpanSource::getInstance();
	auto& span2 = Memorizer::PageDetail::SpanSource::getInstance();
	EXPECT_EQ(&span1, &span2);

	auto& small1 = Memorizer::SDetail::CentralPool::getInstance();
	auto& small2 = Memorizer::SDetail::CentralPool::getInstance();
	EXPECT_EQ(&small1, &small2);

	auto& medium1 = Memorizer::MDetail::CentralPool::getInstance();
	auto& medium2 = Memorizer::MDetail::CentralPool::getInstance();
	EXPECT_EQ(&medium1, &medium2);

	// The span source hands out distinct spans, super-page sized and aligned - that
	// alignment is what the whole header lookup depends on.
	void* a = span1.allocateSpan();
	void* b = span1.allocateSpan();
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	EXPECT_NE(a, b);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(a) % Memorizer::PageDetail::SuperPageSize, 0u);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(b) % Memorizer::PageDetail::SuperPageSize, 0u);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(b) - reinterpret_cast<uintptr_t>(a),
		static_cast<uintptr_t>(Memorizer::PageDetail::SuperPageSize));
}

#ifdef EE_MEMORIZER_DEBUG
TEST(AllocationTest, StatsReturnToBaseline) {	// The pools count live bytes in debug builds; after freeing everything this test
	// allocated, the counters must come back to where they started.
	auto& small = Memorizer::SDetail::getStats();
	auto& medium = Memorizer::MDetail::getStats();
	const UInt64 smallBefore = small.currentBytes.load();
	const UInt64 mediumBefore = medium.currentBytes.load();
	{
		std::vector<void*> blocks;
		for (Size size : kProbeSizes) {
			if (size == 0) continue;
			blocks.push_back(engineAllocate(size));
		}
		for (void* p : blocks) engineFree(p);
	}
	EXPECT_EQ(small.currentBytes.load(), smallBefore);
	EXPECT_EQ(medium.currentBytes.load(), mediumBefore);
}
#endif
