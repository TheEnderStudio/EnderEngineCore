#pragma once

#include "Types.hpp"
#include "Guid.hpp"

EE_NAMESPACE_L10N_BEGIN
class LocalizationManager;
EE_NAMESPACE_L10N_END

EE_NAMESPACE_BEGIN

enum class Language : Size {
#define LANGUAGE(ID, TAG, EN, NATIVE) ID,
#include "Languages.def"
#undef LANGUAGE
	_Count
};

using LanguageId = Size;

inline constexpr LanguageId InvalidLanguageId = 0xffffffffffffffffUi64;
inline constexpr LanguageId BuiltinLanguageCount = static_cast<LanguageId>(Language::_Count);

struct LanguageInfo {
	LanguageId id = InvalidLanguageId;
	String tag;
	String englishName;
	String nativeName;
	bool builtin = false;
};

class EE_API LanguageRegistry {
public:
	static LanguageRegistry& getInstance() {
		static LanguageRegistry instance;
		return instance;
	}

	String toString(LanguageId id) const;
	LanguageId fromString(const StringView tag) const;
	Optional<LanguageInfo> get(LanguageId id) const;
	Size size() const { std::lock_guard<Mutex> lock(m_mutex); return m_idToInfo.size(); }

	Optional<LanguageId> add(const StringView tag, const StringView englishName, const StringView nativeName);
	bool remove(LanguageId id);
	bool remove(const StringView tag);

	Vector<LanguageInfo> snapshot() const; // COPY

private:
	LanguageRegistry();

	mutable Mutex m_mutex;
	HashMap<LanguageId, LanguageInfo> m_idToInfo;
	HashMap<String, LanguageId> m_tagToId;
	LanguageId m_nextDynamicId;
};

inline String toString(Language lang) {
	return LanguageRegistry::getInstance().toString(static_cast<LanguageId>(lang));
}

inline LanguageId fromString(std::string_view tag) {
	return LanguageRegistry::getInstance().fromString(tag);
}

inline Optional<LanguageId> addLanguage(StringView tag, StringView englishName, StringView nativeName) {
	return LanguageRegistry::getInstance().add(tag, englishName, nativeName);
}

inline bool removeLanguage(LanguageId id) {
	return LanguageRegistry::getInstance().remove(id);
}

inline bool removeLanguage(StringView tag) {
	return LanguageRegistry::getInstance().remove(tag);
}

class LocalizedString {
public:
	void flush(); // Implemented in `LocalizationManager.cpp`
	const StringView get_f() const { // FAST PATH
		return m_current;
	}
	const StringView get() {
		flush(); // The mutex is locked in this call
		return m_current;
	}

private:
	mutable Mutex m_mutex;

	String m_current;
	LanguageId m_cachedLang;
	Guid m_guid;
	Sptr<Array<String, BuiltinLanguageCount>> m_l10n;
	Sptr<HashMap<LanguageId, String>> m_nonBuiltinL10n;

	friend class Localization::LocalizationManager;
};

EE_NAMESPACE_END