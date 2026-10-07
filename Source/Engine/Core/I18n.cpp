#include <Core/I18n.hpp>
#include <Core/Log.hpp>

EE_NAMESPACE_BEGIN

namespace {
    static inline String trim(StringView sv) {
        Size start = 0;
        while (start < sv.size() &&
            std::isspace(static_cast<unsigned char>(sv[start]))) {
            ++start;
        }
        Size end = sv.size();
        while (end > start &&
            std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
            --end;
        }
        return String(sv.substr(start, end - start));
    }

    static inline String normalizeTag(StringView tag) {
        String result;
        result.reserve(tag.size());

        for (char c : tag) {
            if (c == '_') c = '-';
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            result.push_back(c);
        }

        Size start = result.find_first_not_of(" \t\n\r");
        if (start == String::npos) return {};
        Size end = result.find_last_not_of(" \t\n\r");
        return result.substr(start, end - start + 1);
    }
}

// --------------------------------------------------------
// LanguageRegistry
// --------------------------------------------------------

LanguageRegistry::LanguageRegistry() : m_nextDynamicId(BuiltinLanguageCount) {
    struct Builtin {
        LanguageId id;
        const char* tag;
        const char* en;
        const char* native;
    };

    static const Builtin builtins[] = {
#define LANGUAGE(ID, TAG, EN, NATIVE, WIN32MACRO) \
    { static_cast<LanguageId>(Language::ID), TAG, EN, NATIVE },
#include <Core/Languages.def>
#undef LANGUAGE
    };

    for (const auto& b : builtins) {
        LanguageInfo info;
        info.id = b.id;
        info.tag = b.tag;
        info.englishName = b.en;
        info.nativeName = b.native;
        info.builtin = true;

        m_idToInfo.emplace(info.id, info);
        m_tagToId.emplace(normalizeTag(info.tag), info.id);
    }
}

String LanguageRegistry::toString(LanguageId id) const {
    std::lock_guard<Mutex> lock(m_mutex);
    auto it = m_idToInfo.find(id);
    return it == m_idToInfo.end() ? "und" : it->second.tag;
}

LanguageId LanguageRegistry::fromString(const StringView tag) const {
    String key = normalizeTag(tag);
    if (key.empty()) return InvalidLanguageId;

    std::lock_guard<Mutex> lock(m_mutex);

    // fallbacks: zh-Hant-HK -> zh-Hant -> zh
    while (!key.empty()) {
        auto it = m_tagToId.find(key);
        if (it != m_tagToId.end()) return it->second;

        auto pos = key.rfind('-');
        if (pos == String::npos) break;
        key.resize(pos);
    }
    return InvalidLanguageId;
}

Optional<LanguageInfo> LanguageRegistry::get(LanguageId id) const {
    std::lock_guard<Mutex> lock(m_mutex);
    auto it = m_idToInfo.find(id);
    if (it == m_idToInfo.end()) return std::nullopt;
    return it->second;
}

Optional<LanguageId> LanguageRegistry::add(const StringView tag, const StringView englishName, const StringView nativeName) {
    String originalTag = trim(tag);
    String key = normalizeTag(originalTag);
    if (key.empty()) return std::nullopt;

    std::lock_guard<Mutex> lock(m_mutex);

    if (m_tagToId.find(key) != m_tagToId.end()) {
        return std::nullopt;
    }

    LanguageId id = m_nextDynamicId++;
    LanguageInfo info;
    info.id = id;
    info.tag = originalTag;
    info.englishName = String(englishName);
    info.nativeName = String(nativeName);
    info.builtin = false;

    m_idToInfo.emplace(id, info);
    m_tagToId.emplace(key, id);
    return id;
}

bool LanguageRegistry::remove(LanguageId id) {
    std::lock_guard<Mutex> lock(m_mutex);

    auto it = m_idToInfo.find(id);
    if (it == m_idToInfo.end()) return false;
    if (it->second.builtin) return false;

    String key = normalizeTag(it->second.tag);
    m_tagToId.erase(key);
    m_idToInfo.erase(it);
    return true;
}

bool LanguageRegistry::remove(const StringView tag) {
    String key = normalizeTag(tag);
    if (key.empty()) return false;

    std::lock_guard<Mutex> lock(m_mutex);

    auto it = m_tagToId.find(key);
    if (it == m_tagToId.end()) return false;

    LanguageId id = it->second;
    auto infoIt = m_idToInfo.find(id);
    if (infoIt == m_idToInfo.end() || infoIt->second.builtin) {
        return false;
    }

    m_tagToId.erase(it);
    m_idToInfo.erase(infoIt);
    return true;
}

Vector<LanguageInfo> LanguageRegistry::snapshot() const {
    std::lock_guard<Mutex> lock(m_mutex);

    Vector<LanguageInfo> result;
    result.reserve(m_idToInfo.size());

    for (const auto& kv : m_idToInfo) {
        result.push_back(kv.second);
    }

    std::sort(result.begin(), result.end(),
        [](const LanguageInfo& a, const LanguageInfo& b) {
            return a.id < b.id;
        });

    return result;
}

EE_NAMESPACE_END
