#include <cxxopts.hpp>
#include <simdjson.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Version Definations
// ---------------------------------------------------------------------------
#define L10NGEN_VERSION_MAJOR 0
#define L10NGEN_VERSION_MINOR 1
#define L10NGEN_VERSION_PATCH 0

namespace fs = std::filesystem;

static const std::map<std::string, std::string>& langCodeToEnum() {
    static const std::map<std::string, std::string> table = [] {
        std::map<std::string, std::string> m;
#define LANGUAGE(EnumName, Code, EngName, NatName, Win32Macro) m.emplace(Code, #EnumName);
#include <Engine/Core/Languages.def>
#undef LANGUAGE
        return m;
        }();
    return table;
}

static std::string snakeToPascal(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    bool upperNext = true;
    for (unsigned char c : input) {
        if (c == '_') { upperNext = true; continue; }
        if (upperNext) { result += static_cast<char>(std::toupper(c)); upperNext = false; }
        else { result += static_cast<char>(std::tolower(c)); }
    }
    return result;
}

static std::string escapeString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        default:   out += c;      break;
        }
    }
    out += "\"";
    return out;
}

struct Node {
    std::map<std::string, Node> children; // namespace name -> child
    // varName -> langCode -> value
    std::map<std::string, std::map<std::string, std::string>> translations;
};

// ---------------------------------------------------------------------------
// JSON parsing (recursive)
// ---------------------------------------------------------------------------
static void processElement(simdjson::dom::element el, Node& node,
    const std::string& langCode) {
    simdjson::dom::object obj;
    if (el.get_object().get(obj) != simdjson::SUCCESS) return;

    for (auto field : obj) {
        std::string key = std::string(field.key);
        simdjson::dom::element value = field.value;
        std::string pascalName = snakeToPascal(key);

        if (value.is_string()) {
            std::string_view sv = value.get_string().value();
            node.translations[pascalName][langCode] = std::string(sv);
        }
        else if (value.is_object()) {
            processElement(value, node.children[pascalName], langCode);
        }
    }
}

// ---------------------------------------------------------------------------
// Directory walking
// ---------------------------------------------------------------------------
static void walkInput(const fs::path& inputDir, Node& root) {
    if (!fs::exists(inputDir))
        throw std::runtime_error("Input directory does not exist: " + inputDir.string());

    fs::path absInput = fs::absolute(inputDir);
    const auto& langMap = langCodeToEnum();

    for (const auto& entry : fs::recursive_directory_iterator(absInput)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string langCode = entry.path().stem().string();
        if (langMap.find(langCode) == langMap.end()) {
            std::cerr << "Warning: unknown language code '" << langCode
                << "' for " << entry.path() << " (skipped)\n";
            continue;
        }

        // Relative folder path (excluding file name) -> namespace path
        fs::path rel = entry.path().lexically_relative(absInput).parent_path();

        Node* node = &root;
        for (const auto& part : rel) {
            if (part.empty() || part == ".") continue;
            node = &node->children[snakeToPascal(part.string())];
        }

        std::string pathStr = entry.path().string();
        simdjson::dom::parser parser;
        auto result = parser.load(pathStr);
        if (result.error()) {
            std::cerr << "Failed to parse " << entry.path() << ": "
                << simdjson::error_message(result.error()) << "\n";
            continue;
        }

        processElement(result.value(), *node, langCode);
    }
}

// ---------------------------------------------------------------------------
// English fallback / ordering
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, std::string>> resolveLanguages(const std::map<std::string, std::string>& langMap) {
    std::map<std::string, std::string> merged = langMap;

    bool hasEn = merged.count("en") > 0;
    bool hasEnUS = merged.count("en-US") > 0;
    if (hasEnUS && !hasEn)  merged["en"] = merged["en-US"];
    if (hasEn && !hasEnUS) merged["en-US"] = merged["en"];

    std::vector<std::pair<std::string, std::string>> ordered;
    if (merged.count("en-US")) { ordered.emplace_back("en-US", merged["en-US"]); merged.erase("en-US"); }
    if (merged.count("en")) { ordered.emplace_back("en", merged["en"]);    merged.erase("en"); }
    for (const auto& [k, v] : merged) ordered.emplace_back(k, v);
    return ordered;
}

// ---------------------------------------------------------------------------
// Declaration emission
// ---------------------------------------------------------------------------
static void generateNamespace(std::ostream& out, const Node& node, int indent) {
    std::string pad(static_cast<size_t>(indent), ' ');
    for (const auto& [nsName, child] : node.children) {
        out << pad << "namespace " << nsName << " {\n";
        generateNamespace(out, child, indent + 4);
        out << pad << "}\n";
    }
    for (const auto& [varName, _] : node.translations) {
        out << pad << "inline static EnderEngine::LocalizedString " << varName << ";\n";
    }
}

// ---------------------------------------------------------------------------
// setup() body emission
// ---------------------------------------------------------------------------
static void generateSetupBody(std::ostream& out, const Node& node,
    const std::string& prefix) {
    const auto& langMap = langCodeToEnum();

    for (const auto& [nsName, child] : node.children) {
        generateSetupBody(out, child, prefix + nsName + "::");
    }
    for (const auto& [varName, translations] : node.translations) {
        auto resolved = resolveLanguages(translations);
        for (const auto& [langCode, value] : resolved) {
            const std::string& enumName = langMap.at(langCode);
            out << "        " << prefix << varName
                << ".set(static_cast<LanguageId>(Language::" << enumName
                << "), Utilities::sharedStringPool().intern("
                << escapeString(value) << "));\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Top-level output
// ---------------------------------------------------------------------------
static void generateOutput(const Node& root, const fs::path& outputPath,
    int majorVer, int minorVer, int patchVer) {
    std::ofstream out(outputPath);
    if (!out)
        throw std::runtime_error("Cannot open output file: " + outputPath.string());

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmBuf);

    out << "#pragma once\n";
    out << "/**\n";
    out << " * ======================================================================================\n";
    out << " * This file is generated by LocalizationGenerator v"
        << majorVer << "." << minorVer << "." << patchVer
        << ", DO NOT modify manually!\n";
    out << " * Time: " << timeStr << "\n";
    out << " * ======================================================================================\n";
    out << " */\n";
    out << "#include <Engine/Core/I18n.hpp>\n";
    out << "#include <Engine/Utilities/StringPool.hpp>\n";
    out << "namespace Localization::Generated {\n";

    generateNamespace(out, root, 4);

    out << "    inline static void setup() {\n";
    out << "        using namespace EnderEngine;\n";
    generateSetupBody(out, root, "");
    out << "    }\n";
    out << "}\n";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        cxxopts::Options options("L10nGenerator",
            "Convert nested localization JSON files into a generated C++ header.");
        options.add_options()
            ("i,input", "Input directory",
                cxxopts::value<std::string>()->default_value("lang"))
            ("o,output", "Output header file",
                cxxopts::value<std::string>()->default_value("LocalizationGenerated.hpp"))
            ("v,version", "Print version")
            ("h,help", "Print usage and exit");

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cout << options.help() << "\n";
            return 0;
        }
        if (result.count("version")) {
            std::cout << "LocalizationGenerator v"
                << L10NGEN_VERSION_MAJOR << "."
                << L10NGEN_VERSION_MINOR << "."
                << L10NGEN_VERSION_PATCH << "\n";
            return 0;
        }

        fs::path inputDir = result["input"].as<std::string>();
        fs::path outputFile = result["output"].as<std::string>();

        Node root;
        walkInput(inputDir, root);
        generateOutput(root, outputFile, L10NGEN_VERSION_MAJOR, L10NGEN_VERSION_MINOR, L10NGEN_VERSION_PATCH);

        std::cout << "Generated: " << outputFile << "\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}