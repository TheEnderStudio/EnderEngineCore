#pragma once
/**
 * @file Json.hpp
 * @brief Minimal JSON value used by the AO baker to patch a glTF's JSON.
 *
 * A glTF carries hundreds of float accessor values, so the parser keeps every scalar's
 * *source text* verbatim: re-printing a parsed double would change the last digit of
 * positions and UVs. Arrays and objects are kept as an ordered tree, which is all the
 * baker needs (append images/samplers/textures, set materials[i].occlusionTexture).
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aob {

class Json {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	/// Source text of a scalar: the number literal, the quoted string token, or "true"/"false".
	std::string raw;
	std::vector<Json> items;                              ///< Array elements.
	std::vector<std::pair<std::string, Json>> members;    ///< Object members (decoded names).

	// ---------------------------------------------------------------- factories

	static Json makeNull() { return Json{}; }

	static Json makeBool(bool v) {
		Json j; j.type = Type::Bool; j.raw = v ? "true" : "false"; return j;
	}
	static Json makeNumber(double v) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.9g", v);
		Json j; j.type = Type::Number; j.raw = buf; return j;
	}
	static Json makeInteger(std::int64_t v) {
		Json j; j.type = Type::Number; j.raw = std::to_string(v); return j;
	}
	static Json makeString(std::string_view s) {
		Json j; j.type = Type::String; j.raw = quote(s); return j;
	}
	static Json makeArray() { Json j; j.type = Type::Array; return j; }
	static Json makeObject() { Json j; j.type = Type::Object; return j; }

	// ---------------------------------------------------------------- queries

	[[nodiscard]] bool isArray() const { return type == Type::Array; }
	[[nodiscard]] bool isObject() const { return type == Type::Object; }
	[[nodiscard]] bool isNumber() const { return type == Type::Number; }
	[[nodiscard]] bool isString() const { return type == Type::String; }
	[[nodiscard]] std::size_t size() const { return isArray() ? items.size() : members.size(); }

	[[nodiscard]] const Json& at(std::size_t i) const { return items[i]; }
	[[nodiscard]] Json& at(std::size_t i) { return items[i]; }

	[[nodiscard]] const Json* find(std::string_view name) const {
		if (!isObject()) return nullptr;
		for (const auto& m : members) if (m.first == name) return &m.second;
		return nullptr;
	}
	[[nodiscard]] Json* find(std::string_view name) {
		if (!isObject()) return nullptr;
		for (auto& m : members) if (m.first == name) return &m.second;
		return nullptr;
	}

	/// @brief Number value, or @p fallback when this is not a number.
	[[nodiscard]] double number(double fallback = 0.0) const {
		if (!isNumber()) return fallback;
		return std::strtod(raw.c_str(), nullptr);
	}
	/// @brief Integer value, or @p fallback.
	[[nodiscard]] std::int64_t integer(std::int64_t fallback = 0) const {
		if (isNumber()) return static_cast<std::int64_t>(std::strtod(raw.c_str(), nullptr));
		return fallback;
	}
	/// @brief Decoded string value (escapes resolved), or @p fallback.
	[[nodiscard]] std::string string(std::string_view fallback = {}) const;

	// ---------------------------------------------------------------- mutation

	Json& push(Json value) { type = Type::Array; items.push_back(std::move(value)); return *this; }
	Json& set(std::string name, Json value) {
		type = Type::Object;
		for (auto& m : members) {
			if (m.first == name) { m.second = std::move(value); return m.second; }
		}
		members.emplace_back(std::move(name), std::move(value));
		return members.back().second;
	}
	/// @brief Object member, created as an object when missing.
	Json& object(std::string name) {
		Json* existing = find(name);
		if (existing && existing->isObject()) return *existing;
		return set(std::move(name), makeObject());
	}
	/// @brief Array member, created when missing (a non-array is replaced).
	Json& array(std::string name) {
		Json* existing = find(name);
		if (existing && existing->isArray()) return *existing;
		return set(std::move(name), makeArray());
	}

	/// @brief Escape @p s into a quoted JSON string token.
	static std::string quote(std::string_view s);

private:
};

/// @brief Parse @p text into @p out. On failure returns false and fills @p error.
bool parseJson(std::string_view text, Json& out, std::string& error);

/// @brief Serialize to text (pretty printed with two spaces).
std::string writeJson(const Json& value, int indent = 0);

} // namespace aob
