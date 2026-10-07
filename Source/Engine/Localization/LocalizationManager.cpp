#include <Localization/LocalizationManager.hpp>
#include <Core/Log.hpp>

#ifdef EE_WINDOWS
# ifndef NOMINMAX
#  define NOMINMAX
# endif // NOMINMAX
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif // WIN32_LEAN_AND_MEAN
# include <Windows.h>
#endif // EE_WINDOWS

EE_NAMESPACE_L10N_BEGIN

namespace {
#ifdef EE_WINDOWS
	static inline Language win32ToLanguage(LANGID id) {
		static HashMap<LANGID, Language> map = {
#define LANGUAGE(ID, TAG, EN, NATIVE, WIN32MACRO) \
    { LANG_##WIN32MACRO, Language::ID },
#include <Core/Languages.def>
#undef LANGUAGE
		};

		// The map has multiple pairs that key is LANG_ENGLISH (N/A), that is undefined
		// Handle it manually
		if (PRIMARYLANGID(id) == LANG_ENGLISH) return Language::English;

		if (auto it = map.find(id); it != map.end()) {
			return it->second;
		}

		// Default to English
		return Language::English;
	}
#endif // EE_WINDOWS
}

Language LocalizationManager::getSystemLanguage() {
#ifdef EE_WINDOWS
	LANGID langId = GetUserDefaultUILanguage(); // No need to use GetUserPreferredUILanguages
	return win32ToLanguage(langId);
#endif // EE_WINDOWS

	// TODO: Linux implementation
	EWarn("Not implemented for Linux! Default to English");
	return Language::English;
}

Optional<Namespace> LocalizationManager::getNamespace(Guid guid) const {
	std::lock_guard<Mutex> lock(m_mutex);
	if (auto it = m_guidToNamespace.find(guid); it != m_guidToNamespace.end()) {
		return it->second;
	}
	return std::nullopt;
}

Guid LocalizationManager::getGuid(const Namespace& ns) const {
	std::lock_guard<Mutex> lock(m_mutex);
	if (auto it = m_namespaceToGuid.find(ns.toString()); it != m_namespaceToGuid.end()) {
		return it->second;
	}
	return Guid{};
}

Namespace LocalizationManager::create(const Vector<StringView>& ns) {
	std::lock_guard<Mutex> lock(m_mutex);
	Namespace result;
	for (const auto& part : ns) {
		result.m_parts.emplace_back(part);
	}
	if (Guid guid = getGuid(result); guid != Guid{}) {
		result.m_guid = guid;
		return result;
	}
	result.m_guid = Guid::generate();
	m_guidToNamespace.insert({ result.m_guid, result });
	m_namespaceToGuid.insert({ result.toString(), result.guid() });
	return result;
}

bool LocalizationManager::exists(const Namespace& ns) const {
	std::lock_guard<Mutex> lock(m_mutex);
	return (m_namespaceToGuid.find(ns.toString()) != m_namespaceToGuid.end()) &&
		(m_guidToNamespace.find(ns.guid()) != m_guidToNamespace.end());
}

void LocalizationManager::registry(const Namespace& ns) {
	if (exists(ns)) return;
	std::lock_guard<Mutex> lock(m_mutex);
	m_namespaceToGuid.insert_or_assign(ns.toString(), ns.guid());
	m_guidToNamespace.insert_or_assign(ns.guid(), ns);
}

LanguageId LocalizationManager::currentLanguage() {
	std::lock_guard<Mutex> lock(m_mutex);
	if (m_currentLanguage == (LanguageId)Language::Unknown) {
		m_currentLanguage = (LanguageId)getSystemLanguage();
	}
	return m_currentLanguage;
}

void LocalizationManager::setLanguage(LanguageId id) {
	std::lock_guard<Mutex> lock(m_mutex);
	m_currentLanguage = id;
}

void LocalizationManager::unregistry(const Namespace& ns) {
	if (!exists(ns)) return;
	std::lock_guard<Mutex> lock(m_mutex);
	m_namespaceToGuid.erase(ns.toString());
	m_guidToNamespace.erase(ns.guid());
}

void LocalizationManager::addLanguagePack(LanguageId id, const HashMap<Guid, String>& langPack) {
	if (id < BuiltinLanguageCount) {
		EError("Target Language ID {} is built in", id);
		return;
	}
	if (existsLanguage(id)) {
		EError("Language Pack of Language ID {} has existed!", id);
		return;
	}
	std::lock_guard<Mutex> lock(m_mutex);
	for (const auto& pair : m_strings) {
		if (!pair.second) continue;
		if (auto it = langPack.find(pair.first); it != langPack.end()) {
			pair.second->m_nonBuiltinL10n->insert({ id, it->second });
			continue;
		}
		String eng = (*pair.second->m_l10n)[(LanguageId)Language::English];
		pair.second->m_nonBuiltinL10n->insert({ id, eng });
		EWarn("The content of Guid {{{}}} doesn't exist in the Language Pack of Language ID {}, use English version \"{}\" to replace it", pair.first.toString(), id, eng);
	}
	m_nonBuiltinLanguages.push_back(id);
}

bool LocalizationManager::removeLanguagePack(LanguageId id) {
	if (id < BuiltinLanguageCount) {
		EError("Target Language ID {} is built in", id);
		return;
	}
	if (!existsLanguage(id)) {
		EError("Language Pack of Language ID {} doesn't exist!", id);
		return false;
	}
	std::lock_guard<Mutex> lock(m_mutex);
	for (const auto& pair : m_strings) {
		if (!pair.second) continue;
		pair.second->m_nonBuiltinL10n->erase(id); // No need to check
	}
	m_nonBuiltinLanguages.erase(std::find(m_nonBuiltinLanguages.begin(), m_nonBuiltinLanguages.end(), id));
}

EE_NAMESPACE_L10N_END

EE_NAMESPACE_BEGIN

using namespace Localization;

void LocalizedString::flush() {
	std::lock_guard<Mutex> lock(m_mutex);

	LanguageId currentLang = LocalizationManager::getInstance().currentLanguage();
	if (currentLang == m_cachedLang) return;
	if (currentLang < BuiltinLanguageCount) {
		m_current = (*m_l10n)[currentLang];
		return;
	}
	if (!LocalizationManager::getInstance().existsLanguage(currentLang)) {
		EError("Current Language (ID: {}) doesn't exist! Use English instead", currentLang);
		m_current = (*m_l10n)[(LanguageId)Language::English];
		return;
	}
	m_current = (*m_nonBuiltinL10n)[currentLang];
}

EE_NAMESPACE_END
