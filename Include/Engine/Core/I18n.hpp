#pragma once

#include "Types.hpp"
#include "Guid.hpp"

#include <atomic>
#include <shared_mutex>

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
	/**
	 * @brief currentLanguage() without the registry lock.
	 *
	 * LocalizedString::get() runs on every string fetch (UI text, log messages), and the
	 * locked version serialises all of them on one mutex. The current language only
	 * changes through setLanguage(), which publishes it here, so the read is a single
	 * atomic load. Returns 0 while it has not been resolved yet - callers fall back to
	 * currentLanguage() in that case.
	 */
	LanguageId currentLanguageFast() const noexcept {
		return m_currentLanguageAtomic.load(std::memory_order_acquire);
	}
	// Return false when the language does not exist or is invalid
	bool setLanguage(LanguageId id);

private:
	LanguageRegistry();

	mutable Mutex m_mutex;
	HashMap<LanguageId, LanguageInfo> m_idToInfo;
	HashMap<String, LanguageId> m_tagToId;
	LanguageId m_nextDynamicId;
	LanguageId m_currentLanguage;
	/// Published copy of m_currentLanguage for lock-free reads (0 = not resolved yet).
	std::atomic<LanguageId> m_currentLanguageAtomic{ 0 };
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
	explicit LocalizedString(LanguageRegistry* reg = &LanguageRegistry::getInstance()) : m_reg(reg) {}

	// Copying has to take the source's lock: the map can be written by set()/clear()
	// while another thread copies the string (the old copy constructor read it unlocked,
	// and the copy assignment returned a reference to a local object and never assigned
	// anything at all).
	LocalizedString(const LocalizedString& a) : m_reg(a.m_reg) {
		std::shared_lock<SharedMutex> lock(a.m_mutex);
		m_l10n = a.m_l10n;
	}
	LocalizedString& operator=(const LocalizedString& a) {
		if (this == &a) return *this;
		HashMap<LanguageId, StringView> copy;
		LanguageRegistry* reg;
		{
			std::shared_lock<SharedMutex> lock(a.m_mutex);
			copy = a.m_l10n;
			reg = a.m_reg;
		}
		{
			std::unique_lock<SharedMutex> lock(m_mutex);
			m_l10n = std::move(copy);
			m_reg = reg;
		}
		return *this;
	}
	LocalizedString(LocalizedString&& a) noexcept : m_reg(a.m_reg) {
		std::unique_lock<SharedMutex> lock(a.m_mutex);
		m_l10n = std::move(a.m_l10n);
	}
	LocalizedString& operator=(LocalizedString&& a) noexcept {
		if (this == &a) return *this;
		HashMap<LanguageId, StringView> moved;
		{
			std::unique_lock<SharedMutex> lock(a.m_mutex);
			moved = std::move(a.m_l10n);
		}
		std::unique_lock<SharedMutex> lock(m_mutex);
		m_l10n = std::move(moved);
		m_reg = a.m_reg;
		return *this;
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
		// One lookup in the common case; only a miss pays for the English fallback.
		if (auto it = m_l10n.find(id); it != m_l10n.end()) return it->second;
		if (auto it = m_l10n.find(static_cast<LanguageId>(Language::English)); it != m_l10n.end()) return it->second;
		return missing();
	}

	// Get by current language hold by `m_reg`; Default to English
	StringView get() const {
		if (!m_reg) return get(static_cast<LanguageId>(Language::English));
		// Lock-free read of the current language (see currentLanguageFast), and only
		// fall back to the locked path until it has been resolved once.
		LanguageId id = m_reg->currentLanguageFast();
		if (id == 0) [[unlikely]] id = m_reg->currentLanguage();
		return get(id);
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
	/// @brief Number of languages this string has a translation for.
	EE_NODISCARD Size size() const {
		std::shared_lock<SharedMutex> lock(m_mutex);
		return m_l10n.size();
	}
	EE_NODISCARD bool empty() const { return size() == 0; }

	// The conversions were non-const, which made them unusable on a const reference (and
	// on the temporaries a getter returns) even though get() itself is const.
	operator StringView() const {
		return get();
	}

	operator String() const /* COPY */ {
		return String(get());
	}

private:
	/// @brief Placeholder returned for a string with no translation at all.
	///
	/// A function-local static instead of a literal so the view is never null (a view
	/// constructed from a literal is fine too, but the literal is per call site and this
	/// keeps one address).
	static StringView missing() {
		static constexpr char kMissing[] = "???";
		return StringView(kMissing, 3);
	}

	mutable SharedMutex m_mutex;
	LanguageRegistry* m_reg;
	HashMap<LanguageId, StringView> m_l10n;
};

EE_NAMESPACE_END