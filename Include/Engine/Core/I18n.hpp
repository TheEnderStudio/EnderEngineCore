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
		static LanguageRegistry instance{};
		return instance;
	}

	String toString(LanguageId id) const;
	LanguageId fromString(const StringView tag) const;
	Optional<LanguageInfo> get(LanguageId id) const;
	Size size() const { return m_idToInfo.size(); }

	Optional<LanguageId> add(const StringView tag, const StringView englishName, const StringView nativeName);
	bool remove(LanguageId id);
	bool remove(const StringView tag);

	Vector<LanguageInfo> snapshot() const; // COPY

	Language getSystemLanguage() const;
	// When m_currentLanguage == 0, it will try to get system language as
	// the result to return; If get is failed, it will set the language
	// to English and return.
	LanguageId currentLanguage();
	// Return false when the language does not exist or is invalid
	bool setLanguage(LanguageId id);

private:
	LanguageRegistry();

	mutable Mutex m_mutex;
	HashMap<LanguageId, LanguageInfo> m_idToInfo;
	HashMap<String, LanguageId> m_tagToId;
	LanguageId m_nextDynamicId;
	LanguageId m_currentLanguage;
};

inline String toString(Language lang) {
	return LanguageRegistry::getInstance().toString(static_cast<LanguageId>(lang));
}

inline LanguageId fromString(const StringView tag) {
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

class EE_API LocalizedString {
public:
	explicit LocalizedString(LanguageRegistry* reg = &LanguageRegistry::getInstance()) :
		m_reg(reg) {}

	LocalizedString(const LocalizedString& a) : m_reg(a.m_reg), m_l10n(a.m_l10n) {}
	LocalizedString& operator=(const LocalizedString& a) {
		LocalizedString r;
		m_reg = r.m_reg = a.m_reg; m_l10n = r.m_l10n = a.m_l10n;
		return r;
	}

	void set(LanguageId id, StringView ptr) {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n[id] = ptr;
	}

	void set(const HashMap<LanguageId, StringView>& ptrs) {
		std::unique_lock<SharedMutex> lock(m_mutex);
		for (const auto& [id, ptr] : ptrs) {
			m_l10n[id] = ptr;
		}
	}

	StringView get(LanguageId id) const {
		std::shared_lock<SharedMutex> lock(m_mutex);
		auto it = m_l10n.find(id);
		return it == m_l10n.end() ? "???" : it->second;
	}

	// Get by current language hold by `m_reg`; Default to English
	StringView get() const {
		return get(m_reg ? m_reg->currentLanguage() : static_cast<LanguageId>(Language::English));
	}

	LanguageRegistry* registry() { return m_reg; }
	const LanguageRegistry* registry() const { return m_reg; }
	void clear() {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n.clear();
	}
	void reserve(Size size) {
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n.reserve(size);
	}

	operator StringView() {
		return get();
	}

	operator String() {
		return String(get());
	}

private:
	mutable SharedMutex m_mutex;
	LanguageRegistry* m_reg;
	HashMap<LanguageId, StringView> m_l10n;
};

EE_NAMESPACE_END