#include <Engine/Core/Core.hpp>
#include <Engine/Core/I18n.hpp>
#include <Engine/Jobs/JobSubsystem.hpp>
#include <Engine/Utilities/StringPool.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace EnderEngine;

namespace {

/// @brief The order the demo uses, up to the point where it failed: Engine::initialize()
/// (logging + crash handler + extensions), the generated localization table, then a job
/// system with the demo's defaults (which is where the reporting access violation hit).
TEST(EngineInitTest, DemoStartupSequence) {
	ASSERT_TRUE(static_cast<bool>(Engine::initialize()));

	// What Localization::Generated::setup() does, including the interning.
	LocalizedString exitButton;
	exitButton.set(static_cast<LanguageId>(Language::EnglishUS), Utilities::sharedStringPool().intern("Exit"));
	exitButton.set(static_cast<LanguageId>(Language::English), Utilities::sharedStringPool().intern("Exit"));
	exitButton.set(static_cast<LanguageId>(Language::ChineseSimplified), Utilities::sharedStringPool().intern("退出"));
	EXPECT_EQ(exitButton.get(static_cast<LanguageId>(Language::English)), "Exit");

	Jobs::JobSubsystem jobs;
	ASSERT_TRUE(static_cast<bool>(jobs.initialize()));
	EXPECT_EQ(jobs.queryStats().maxWorkItemCount, 16384u);
	jobs.shutdown();
}

} // namespace
