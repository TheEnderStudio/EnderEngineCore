#include <Engine/Core/I18n.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace EnderEngine;

namespace {

LanguageId Native() {
	// Restore whatever the machine resolved at startup, so the other tests (and the
	// process) are not left on a language this test picked.
	return static_cast<LanguageId>(LanguageRegistry::getInstance().getSystemLanguage());
}

/// @brief A registry entry that is removed again when the test ends.
class DynamicLanguage {
public:
	DynamicLanguage(const char* tag, const char* en, const char* native) {
		m_id = LanguageRegistry::getInstance().add(tag, en, native);
	}
	~DynamicLanguage() {
		if (m_id.has_value()) LanguageRegistry::getInstance().remove(m_id.value());
	}
	DynamicLanguage(const DynamicLanguage&) = delete;
	DynamicLanguage& operator=(const DynamicLanguage&) = delete;

	EE_NODISCARD bool isValid() const { return m_id.has_value(); }
	EE_NODISCARD LanguageId id() const { return m_id.value(); }

private:
	Optional<LanguageId> m_id;
};

} // namespace

// ---------------------------------------------------------------- registry

TEST(LanguageRegistryTest, BuiltinsRoundTrip) {
	auto& reg = LanguageRegistry::getInstance();

	EXPECT_EQ(reg.toString(static_cast<LanguageId>(Language::English)), "en");
	EXPECT_EQ(reg.toString(static_cast<LanguageId>(Language::ChineseSimplified)), "zh-Hans");

	EXPECT_EQ(reg.fromString("en"), static_cast<LanguageId>(Language::English));
	EXPECT_EQ(reg.fromString("EN"), static_cast<LanguageId>(Language::English));
	// Tags are normalised (case folded, '_' -> '-') and fall back by stripping subtags.
	EXPECT_EQ(reg.fromString("zh_hans"), static_cast<LanguageId>(Language::ChineseSimplified));
	EXPECT_EQ(reg.fromString("zh-Hant-HK"), static_cast<LanguageId>(Language::ChineseHongKong));
	EXPECT_EQ(reg.fromString("en-AU"), static_cast<LanguageId>(Language::English));
	EXPECT_EQ(reg.fromString("und"), static_cast<LanguageId>(Language::Unknown));

	EXPECT_EQ(reg.fromString("not-a-language"), InvalidLanguageId);
	EXPECT_EQ(reg.fromString(""), InvalidLanguageId);
	EXPECT_EQ(reg.toString(InvalidLanguageId), "und");

	auto info = reg.get(static_cast<LanguageId>(Language::Japanese));
	ASSERT_TRUE(info.has_value());
	EXPECT_TRUE(info->builtin);
	EXPECT_EQ(info->englishName, "Japanese");
	EXPECT_FALSE(reg.get(InvalidLanguageId).has_value());
}

TEST(LanguageRegistryTest, DynamicLanguages) {
	auto& reg = LanguageRegistry::getInstance();
	const Size before = reg.size();

	DynamicLanguage dyn("tst-xq", "Test Language", "Test Native");
	ASSERT_TRUE(dyn.isValid());
	EXPECT_GE(dyn.id(), BuiltinLanguageCount);
	EXPECT_EQ(reg.size(), before + 1);

	// The same tag cannot be added twice.
	const auto dup = reg.add("tst-xq", "Dup", "Dup");
	EXPECT_FALSE(dup.has_value());

	// Normalised lookup finds it.
	EXPECT_EQ(reg.fromString("TST_XQ"), dyn.id());

	// Selecting it works, and unknown ids are rejected.
	const LanguageId prev = reg.currentLanguage();
	EXPECT_TRUE(reg.setLanguage(dyn.id()));
	EXPECT_EQ(reg.currentLanguage(), dyn.id());
	EXPECT_EQ(reg.currentLanguageFast(), dyn.id());
	EXPECT_FALSE(reg.setLanguage(InvalidLanguageId));
	EXPECT_TRUE(reg.setLanguage(prev));
	EXPECT_EQ(reg.currentLanguageFast(), prev);

	// Builtin entries cannot be removed, dynamic ones can.
	EXPECT_FALSE(reg.remove(static_cast<LanguageId>(Language::English)));
	EXPECT_TRUE(reg.remove(dyn.id()));
	EXPECT_EQ(reg.size(), before);
	EXPECT_FALSE(reg.setLanguage(dyn.id()));
}

TEST(LanguageRegistryTest, UnknownIsNotSelectable) {
	auto& reg = LanguageRegistry::getInstance();
	// Unknown (0) doubles as the "not resolved" sentinel of currentLanguage(), so
	// selecting it would make the registry re-resolve the system language forever.
	EXPECT_FALSE(reg.setLanguage(static_cast<LanguageId>(Language::Unknown)));
}

TEST(LanguageRegistryTest, FastPathMatchesLockedPath) {
	auto& reg = LanguageRegistry::getInstance();
	const LanguageId prev = reg.currentLanguage();
	EXPECT_EQ(reg.currentLanguageFast(), prev);
	(void)reg.setLanguage(static_cast<LanguageId>(Language::English));
	EXPECT_EQ(reg.currentLanguageFast(), reg.currentLanguage());
	(void)reg.setLanguage(prev);
}

// ------------------------------------------------------------ LocalizedString

TEST(LocalizedStringTest, SetAndGet) {
	LocalizedString s;
	s.set(static_cast<LanguageId>(Language::English), "Exit");
	s.set(static_cast<LanguageId>(Language::ChineseSimplified), "退出");

	EXPECT_EQ(s.get(static_cast<LanguageId>(Language::English)), "Exit");
	EXPECT_EQ(s.get(static_cast<LanguageId>(Language::ChineseSimplified)), "退出");
	EXPECT_EQ(s.size(), 2u);
	EXPECT_FALSE(s.empty());
}

TEST(LocalizedStringTest, FallsBackToEnglish) {
	LocalizedString s;
	s.set(static_cast<LanguageId>(Language::English), "Exit");
	// A language the string has no translation for still resolves through English.
	EXPECT_EQ(s.get(static_cast<LanguageId>(Language::Japanese)), "Exit");
}

TEST(LocalizedStringTest, MissingWhenNoEnglishEither) {
	LocalizedString s;
	EXPECT_EQ(s.get(static_cast<LanguageId>(Language::English)), "???");
	EXPECT_TRUE(s.empty());
	s.set(static_cast<LanguageId>(Language::Japanese), "終了");
	EXPECT_EQ(s.get(static_cast<LanguageId>(Language::English)), "???");
}

TEST(LocalizedStringTest, GetUsesCurrentLanguage) {
	auto& reg = LanguageRegistry::getInstance();
	const LanguageId prev = reg.currentLanguage();

	LocalizedString s;
	s.set(static_cast<LanguageId>(Language::English), "Exit");
	s.set(static_cast<LanguageId>(Language::ChineseSimplified), "退出");

	ASSERT_TRUE(reg.setLanguage(static_cast<LanguageId>(Language::ChineseSimplified)));
	EXPECT_EQ(s.get(), "退出");
	// The implicit conversions have to follow the same language.
	StringView asView = s;
	EXPECT_EQ(asView, "退出");
	String asString = s;
	EXPECT_EQ(asString, "退出");

	ASSERT_TRUE(reg.setLanguage(prev));
}

TEST(LocalizedStringTest, CopyAssignmentActuallyCopies) {
	// Regression: the old operator= built a local object, assigned into *that* and
	// returned a reference to it, so the target was never written (and the returned
	// reference dangled).
	LocalizedString src;
	src.set(static_cast<LanguageId>(Language::English), "Exit");
	src.set(static_cast<LanguageId>(Language::ChineseSimplified), "退出");

	LocalizedString dst;
	dst.set(static_cast<LanguageId>(Language::English), "something else");

	dst = src;
	EXPECT_EQ(dst.get(static_cast<LanguageId>(Language::English)), "Exit");
	EXPECT_EQ(dst.get(static_cast<LanguageId>(Language::ChineseSimplified)), "退出");
	EXPECT_EQ(dst.size(), 2u);

	// Self-assignment must not corrupt anything.
	dst = dst;
	EXPECT_EQ(dst.get(static_cast<LanguageId>(Language::English)), "Exit");

	// The source is untouched by the copy.
	EXPECT_EQ(src.get(static_cast<LanguageId>(Language::English)), "Exit");
}

TEST(LocalizedStringTest, CopyConstructionLocks) {
	LocalizedString src;
	src.set(static_cast<LanguageId>(Language::English), "Exit");
	LocalizedString copy(src);
	EXPECT_EQ(copy.get(static_cast<LanguageId>(Language::English)), "Exit");
	EXPECT_EQ(copy.size(), 1u);
}

TEST(LocalizedStringTest, Move) {
	LocalizedString src;
	src.set(static_cast<LanguageId>(Language::English), "Exit");
	LocalizedString moved(std::move(src));
	EXPECT_EQ(moved.get(static_cast<LanguageId>(Language::English)), "Exit");
}

TEST(LocalizedStringTest, ConstConversions) {
	LocalizedString s;
	s.set(static_cast<LanguageId>(Language::English), "Exit");
	const LocalizedString& cs = s;
	StringView v = cs;
	String str = cs;
	EXPECT_EQ(v, "Exit");
	EXPECT_EQ(str, "Exit");
}

TEST(LocalizedStringTest, ConcurrentReadsWhileWriting) {
	LocalizedString s;
	s.set(static_cast<LanguageId>(Language::English), "Exit");
	s.set(static_cast<LanguageId>(Language::ChineseSimplified), "退出");

	std::atomic<bool> stop{ false };
	std::vector<std::thread> readers;
	readers.reserve(4);
	for (int t = 0; t < 4; ++t) {
		readers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				const StringView v = s.get(static_cast<LanguageId>(Language::English));
				EXPECT_EQ(v, "Exit");
			}
		});
	}
	for (int i = 0; i < 200; ++i) {
		s.set(static_cast<LanguageId>(Language::ChineseSimplified), "退出");
	}
	stop.store(true, std::memory_order_relaxed);
	for (auto& th : readers) th.join();
}
