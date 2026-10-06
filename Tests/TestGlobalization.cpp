#include <Engine/Core/I18n.hpp>

#include <gtest/gtest.h>
#include <random>

using namespace EnderEngine;

TEST(I18nTest, GetLanguageCode) {
	static const HashMap<Language, const String> toStringTable = {
#define LANGUAGE(ENUM, CODE, NAME) { Language::ENUM, CODE },
#include <Engine/Core/Languages.def>
#undef LANGUAGE
	};
	
	Vector<Language> langs;
	langs.reserve(toStringTable.size());

	for (const auto& lang : toStringTable) {
		langs.push_back(lang.first);
	}

	std::random_device rd;
	std::mt19937 gen(rd());
	std::shuffle(langs.begin(), langs.end(), gen);

	for (const auto& lang : langs) {
		EXPECT_EQ(toString(lang), toStringTable.at(lang));
	}
}

TEST(I18nTest, GetLanguageName) {
	static const HashMap<Language, const String> toNameTable = {
#define LANGUAGE(ENUM, CODE, NAME) { Language::ENUM, NAME },
#include <Engine/Core/Languages.def>
#undef LANGUAGE
	};

	Vector<Language> names;
	names.reserve(toNameTable.size());

	for (const auto& name : toNameTable) {
		names.push_back(name.first);
	}

	std::random_device rd;
	std::mt19937 gen(rd());
	std::shuffle(names.begin(), names.end(), gen);

	for (const auto& name : names) {
		EXPECT_EQ(toName(name), toNameTable.at(name));
	}
}

TEST(I18nTest, GetLanguageEnum) {
	static const HashMap<String, const Language> codeLangTable = {
#define LANGUAGE(ENUM, CODE, NAME) { CODE, Language::ENUM },
#include <Engine/Core/Languages.def>
#undef LANGUAGE
	};

	Vector<String> codes;
	codes.reserve(codeLangTable.size());

	for (const auto& code : codeLangTable) {
		codes.push_back(code.first);
	}

	std::random_device rd;
	std::mt19937 gen(rd());
	std::shuffle(codes.begin(), codes.end(), gen);

	for (const auto& code : codes) {
		EXPECT_EQ(fromLanguageCode(code), codeLangTable.at(code));
	}
}

TEST(I18nTest, UnknownLanguageHandle) {
	EXPECT_EQ(toString((Language)99999999), "??");
	EXPECT_EQ(toName((Language)99999999), "Unknown");
	EXPECT_EQ(fromLanguageCode("____"), Language::_Count);
}
