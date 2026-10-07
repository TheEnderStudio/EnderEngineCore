#pragma once
#include <Engine/Core/I18n.hpp>

EE_NAMESPACE_L10N_BEGIN

class Namespace {
public:
	String toString() const {
		String result;
		for (const auto& part : m_parts) {
			result += part;
			result += "::";
		}
		result.erase(result.size() - 2); // Remove "::" in the end
		return result;
	}

	const Guid guid() const { return m_guid; }

private:
	Vector<String> m_parts;
	Guid m_guid;

	friend class LocalizationManager;
};

class LocalizationManager {
public:
	static Language getSystemLanguage();

	static LocalizationManager& getInstance() {
		static LocalizationManager instance;
		return instance;
	}

	LanguageId currentLanguage();
	void setLanguage(LanguageId id);
	Optional<Namespace> getNamespace(Guid guid) const;
	Guid getGuid(const Namespace& ns) const;
	Namespace create(const Vector<StringView>& ns);
	bool exists(const Namespace& ns) const;
	bool existsString(const Namespace& ns) const {
		std::lock_guard<Mutex> lock(m_mutex);
		return exists(ns) && (m_strings.find(ns.guid()) != m_strings.end());
	}
	void registry(const Namespace& ns);
	void unregistry(const Namespace& ns);

	Sptr<LocalizedString> createString(const Namespace& ns, const Array<String, BuiltinLanguageCount>& l10n) {
		if (!exists(ns)) {
			return nullptr;
		}
		Sptr<LocalizedString> string = std::make_shared<LocalizedString>();
		string->m_guid = ns.guid();
		string->m_l10n = std::make_shared<Array<String, BuiltinLanguageCount>>(l10n);
		if (LanguageId currentLang = currentLanguage(); currentLang < BuiltinLanguageCount) {
			string->m_current = (*string->m_l10n)[currentLang];
		}
		std::lock_guard<Mutex> lock(m_mutex);
		m_strings.insert({ ns.guid(), string });
		return string;
	}

	Sptr<LocalizedString> get(const Namespace& ns) {
		if (!existsString(ns)) return nullptr;
		std::lock_guard<Mutex> lock(m_mutex);
		return m_strings.find(ns.guid())->second;
	}

	void removeString(const Namespace& ns) {
		if (!existsString(ns)) return;
		std::lock_guard<Mutex> lock(m_mutex);
		m_strings.erase(ns.guid());
	}

	void addLanguagePack(LanguageId id, const HashMap<Guid, String>& langPack);
	bool removeLanguagePack(LanguageId id);
	bool existsLanguage(LanguageId id) const {
		std::lock_guard<Mutex> lock(m_mutex);
		return (id < BuiltinLanguageCount) ||
			(std::find(m_nonBuiltinLanguages.begin(), m_nonBuiltinLanguages.end(), id) != m_nonBuiltinLanguages.end());
	}

private:
	LocalizationManager() = default; // Use `getInstance()` instead
	~LocalizationManager() = default;

	mutable Mutex m_mutex;
	HashMap<String, Guid> m_namespaceToGuid;
	HashMap<Guid, Namespace> m_guidToNamespace;
	HashMap<Guid, Sptr<LocalizedString>> m_strings;
	Vector<LanguageId> m_nonBuiltinLanguages;
	LanguageId m_currentLanguage = (LanguageId)Language::Unknown;
};

EE_NAMESPACE_L10N_END