#include <Engine/Core/I18n.hpp>
#include <Engine/Utilities/StringPool.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#ifdef _DEBUG
# include <crtdbg.h>
#endif

using namespace EnderEngine;

namespace {

/// @brief Turns on the CRT's "check every block" mode for the scope.
///
/// A write past a heap block is otherwise only noticed later, by an unrelated allocation
/// that trips over the damaged metadata (which is exactly how the Demo failed), so the
/// tests that hammer the interning path can check the whole heap as they go. It validates
/// every block on *every* allocation, so it is opt-in: `set EE_TEST_HEAP_CHECK=1`.
class HeapCheckScope {
public:
	HeapCheckScope() {
#ifdef _DEBUG
		if (std::getenv("EE_TEST_HEAP_CHECK") == nullptr) return;
		m_previous = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
		_CrtSetDbgFlag(m_previous | _CRTDBG_CHECK_ALWAYS_DF);
#endif
	}
	~HeapCheckScope() {
#ifdef _DEBUG
		if (m_previous == 0) return;
		_CrtSetDbgFlag(m_previous);
#endif
	}
	HeapCheckScope(const HeapCheckScope&) = delete;
	HeapCheckScope& operator=(const HeapCheckScope&) = delete;

private:
#ifdef _DEBUG
	int m_previous = 0;
#endif
};

} // namespace

TEST(StringPoolTest, InternReturnsStableContents) {
	auto& pool = Utilities::sharedStringPool();

	const StringView a = pool.intern("Exit");
	const StringView b = pool.intern("Exit");
	// Same contents are interned to the same characters.
	EXPECT_EQ(a.data(), b.data());
	EXPECT_EQ(a, "Exit");

	// The pooled copy survives the source going away.
	StringView c;
	{
		const std::string temporary = "Press ESC to Exit";
		c = pool.intern(temporary);
	}
	EXPECT_EQ(c, "Press ESC to Exit");

	// Binary-ish contents, including embedded NULs and a byte needing the full range.
	const char raw[] = { 'a', '\0', 'b', static_cast<char>(0x7f) };
	const StringView d = pool.intern(StringView(raw, sizeof(raw)));
	ASSERT_EQ(d.size(), sizeof(raw));
	EXPECT_EQ(std::memcmp(d.data(), raw, sizeof(raw)), 0);
}

TEST(StringPoolTest, EmptyStringIsItsOwnInternedValue) {
	auto& pool = Utilities::sharedStringPool();
	const StringView empty = pool.intern(StringView{});
	EXPECT_TRUE(empty.empty());
	EXPECT_EQ(pool.intern(StringView{}).data(), empty.data());
}

TEST(StringPoolTest, ManyDistinctStringsThenLargeAllocations) {
	// Mirrors what the Demo does right before initialising the job system: intern the
	// generated translation table, then make ordinary large std::vector allocations. A
	// stray write in the interning path used to surface as an access violation there.
	HeapCheckScope check;
	auto& pool = Utilities::sharedStringPool();

	std::vector<std::string> source;
	source.reserve(4000);
	for (int i = 0; i < 4000; ++i) {
		source.push_back("string-" + std::to_string(i) + "-" + std::string(static_cast<size_t>(i % 97), 'x'));
	}
	for (const std::string& s : source) {
		const StringView v = pool.intern(s);
		ASSERT_EQ(v, s);
	}

	// The same strings again: all must hit the interned copies.
	for (const std::string& s : source) {
		EXPECT_EQ(pool.intern(s), s);
	}

	// Large, ordinary allocations right afterwards (the failing site in the Demo was a
	// 16384-entry reserve).
	{
		std::vector<UInt32> freeWorkItems;
		freeWorkItems.reserve(16384);
		for (UInt32 i = 0; i < 16384; ++i) freeWorkItems.push_back(16383 - i);
		EXPECT_EQ(freeWorkItems.front(), 16383u);
		EXPECT_EQ(freeWorkItems.back(), 0u);
	}
	{
		std::vector<std::string> strings(2000, "payload");
		EXPECT_EQ(strings.back(), "payload");
	}
	{
		std::pmr::vector<int> pmr(PoolMemoryResource::get());
		for (int i = 0; i < 10000; ++i) pmr.push_back(i);
		EXPECT_EQ(pmr[9999], 9999);
	}
}

TEST(StringPoolTest, InterningSurvivesClearAndReuse) {
	auto& pool = Utilities::sharedStringPool();
	const StringView before = pool.intern("transient-value");
	EXPECT_EQ(before, "transient-value");
	pool.clear();
	// Everything is pooled again after a clear; the contents must still be correct.
	const StringView after = pool.intern("transient-value");
	EXPECT_EQ(after, "transient-value");
	pool.clear();
	EXPECT_EQ(pool.size(), 0u);
	EXPECT_EQ(pool.intern("after-clear"), "after-clear");
}

TEST(StringPoolTest, ConcurrentInterning) {
	HeapCheckScope check;
	auto& pool = Utilities::sharedStringPool();

	constexpr int kThreads = 4;
	constexpr int kPerThread = 2000;
	std::atomic<int> mismatches{ 0 };
	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([t, &pool, &mismatches] {
			for (int i = 0; i < kPerThread; ++i) {
				const std::string s = "t" + std::to_string(t) + "-item-" + std::to_string(i);
				const StringView v = pool.intern(s);
				if (v != s) ++mismatches;
				// Intern something another thread also interns.
				if (pool.intern("shared-token") != "shared-token") ++mismatches;
			}
		});
	}
	for (auto& th : threads) th.join();
	EXPECT_EQ(mismatches.load(), 0);
}
