#pragma once

#include "Types.hpp"

EE_NAMESPACE_BEGIN

#define LANGUAGE(ENUM, CODE, NAME) ENUM,
enum class Language {
#include "Languages.def"
	_Count
};
#undef LANGUAGE

inline const StringView toString(Language lang) {
	static const HashMap<Language, const String> toStringTable = {
#define LANGUAGE(ENUM, CODE, NAME) { Language::ENUM, CODE },
#include "Languages.def"
#undef LANGUAGE
	};
	if (auto it = toStringTable.find(lang); it != toStringTable.end()) {
		return it->second;
	}
	static const String unknown = "??";
	return unknown;
}

inline const StringView toName(Language lang) {
	static const HashMap<Language, const String> toNameTable = {
#define LANGUAGE(ENUM, CODE, NAME) { Language::ENUM, NAME },
#include "Languages.def"
#undef LANGUAGE
	};
	if (auto it = toNameTable.find(lang); it != toNameTable.end()) {
		return it->second;
	}
	static const String unknown = "Unknown";
	return unknown;
}

inline Language fromLanguageCode(const String& str) {
	static const HashMap<String, const Language> codeLangTable = {
#define LANGUAGE(ENUM, CODE, NAME) { CODE, Language::ENUM },
#include "Languages.def"
#undef LANGUAGE
	};
	if (auto it = codeLangTable.find(str); it != codeLangTable.end()) {
		return it->second;
	}
	return Language::_Count;
}

EE_NAMESPACE_END