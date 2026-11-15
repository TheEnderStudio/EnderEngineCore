#include "Json.hpp"

#include <cstdlib>
#include <cstring>

namespace aob {

namespace {

struct Parser {
	std::string_view text;
	std::size_t pos = 0;
	std::string error;

	[[nodiscard]] bool atEnd() const { return pos >= text.size(); }
	[[nodiscard]] char peek() const { return atEnd() ? '\0' : text[pos]; }

	void skipSpace() {
		while (!atEnd()) {
			const char c = text[pos];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos; continue; }
			break;
		}
	}

	bool fail(const std::string& what) {
		if (error.empty()) {
			error = what + " at byte " + std::to_string(pos);
		}
		return false;
	}

	/// @brief Scan a quoted string and keep its source token (quotes included).
	bool scanStringToken(std::string& out) {
		if (peek() != '"') return fail("expected a string");
		const std::size_t begin = pos++;
		while (!atEnd()) {
			const char c = text[pos++];
			if (c == '\\') {
				if (atEnd()) return fail("unterminated escape");
				++pos; // skip the escaped character (a \" does not end the string)
				continue;
			}
			if (c == '"') {
				out.assign(text.data() + begin, pos - begin);
				return true;
			}
		}
		return fail("unterminated string");
	}

	bool parseValue(Json& out) {
		skipSpace();
		const char c = peek();
		if (c == '{') return parseObject(out);
		if (c == '[') return parseArray(out);
		if (c == '"') {
			out.type = Json::Type::String;
			return scanStringToken(out.raw);
		}
		if (c == 't') { if (text.compare(pos, 4, "true") != 0) return fail("bad literal"); out.type = Json::Type::Bool; out.raw = "true"; pos += 4; return true; }
		if (c == 'f') { if (text.compare(pos, 5, "false") != 0) return fail("bad literal"); out.type = Json::Type::Bool; out.raw = "false"; pos += 5; return true; }
		if (c == 'n') { if (text.compare(pos, 4, "null") != 0) return fail("bad literal"); out.type = Json::Type::Null; out.raw = "null"; pos += 4; return true; }
		if (c == '-' || (c >= '0' && c <= '9')) {
			const std::size_t begin = pos;
			if (peek() == '-') ++pos;
			while (peek() >= '0' && peek() <= '9') ++pos;
			if (peek() == '.') { ++pos; while (peek() >= '0' && peek() <= '9') ++pos; }
			if (peek() == 'e' || peek() == 'E') {
				++pos;
				if (peek() == '+' || peek() == '-') ++pos;
				while (peek() >= '0' && peek() <= '9') ++pos;
			}
			if (pos == begin) return fail("bad number");
			out.type = Json::Type::Number;
			out.raw.assign(text.data() + begin, pos - begin);
			return true;
		}
		return fail("unexpected character");
	}

	bool parseArray(Json& out) {
		out.type = Json::Type::Array;
		++pos; // '['
		skipSpace();
		if (peek() == ']') { ++pos; return true; }
		for (;;) {
			Json element;
			if (!parseValue(element)) return false;
			out.items.push_back(std::move(element));
			skipSpace();
			if (peek() == ',') { ++pos; continue; }
			if (peek() == ']') { ++pos; return true; }
			return fail("expected ',' or ']'");
		}
	}

	bool parseObject(Json& out) {
		out.type = Json::Type::Object;
		++pos; // '{'
		skipSpace();
		if (peek() == '}') { ++pos; return true; }
		for (;;) {
			skipSpace();
			std::string token;
			if (!scanStringToken(token)) return false;
			Json name;
			name.type = Json::Type::String;
			name.raw = token;
			skipSpace();
			if (peek() != ':') return fail("expected ':'");
			++pos;
			Json value;
			if (!parseValue(value)) return false;
			out.members.emplace_back(name.string(), std::move(value));
			skipSpace();
			if (peek() == ',') { ++pos; continue; }
			if (peek() == '}') { ++pos; return true; }
			return fail("expected ',' or '}'");
		}
	}
};

void writeInto(std::string& out, const Json& value, int indent) {
	const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
	const std::string padInner(static_cast<std::size_t>(indent + 1) * 2, ' ');
	switch (value.type) {
	case Json::Type::Null: out += "null"; return;
	case Json::Type::Bool:
	case Json::Type::Number:
	case Json::Type::String: out += value.raw; return;
	case Json::Type::Array:
		if (value.items.empty()) { out += "[]"; return; }
		out += "[\n";
		for (std::size_t i = 0; i < value.items.size(); ++i) {
			out += padInner;
			writeInto(out, value.items[i], indent + 1);
			if (i + 1 < value.items.size()) out += ',';
			out += '\n';
		}
		out += pad;
		out += ']';
		return;
	case Json::Type::Object:
		if (value.members.empty()) { out += "{}"; return; }
		out += "{\n";
		for (std::size_t i = 0; i < value.members.size(); ++i) {
			out += padInner;
			out += Json::quote(value.members[i].first);
			out += ": ";
			writeInto(out, value.members[i].second, indent + 1);
			if (i + 1 < value.members.size()) out += ',';
			out += '\n';
		}
		out += pad;
		out += '}';
		return;
	}
}

} // namespace

std::string Json::string(std::string_view fallback) const {
	if (type != Type::String || raw.size() < 2) return std::string(fallback);
	std::string out;
	out.reserve(raw.size() - 2);
	for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
		const char c = raw[i];
		if (c != '\\') { out += c; continue; }
		++i;
		if (i + 1 > raw.size()) break;
		switch (raw[i]) {
		case '"':  out += '"';  break;
		case '\\': out += '\\'; break;
		case '/':  out += '/';  break;
		case 'b':  out += '\b'; break;
		case 'f':  out += '\f'; break;
		case 'n':  out += '\n'; break;
		case 'r':  out += '\r'; break;
		case 't':  out += '\t'; break;
		case 'u': {
			// Decode the BMP code point into UTF-8; surrogate pairs are rare in file
			// names and are left as the replacement character rather than guessed at.
			if (i + 4 < raw.size()) {
				char hex[5] = { raw[i + 1], raw[i + 2], raw[i + 3], raw[i + 4], 0 };
				const unsigned long cp = std::strtoul(hex, nullptr, 16);
				i += 4;
				if (cp < 0x80) {
					out += static_cast<char>(cp);
				}
				else if (cp < 0x800) {
					out += static_cast<char>(0xC0 | (cp >> 6));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
				else {
					out += static_cast<char>(0xE0 | (cp >> 12));
					out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
			}
			break;
		}
		default: out += raw[i]; break;
		}
	}
	return out;
}

std::string Json::quote(std::string_view s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += '"';
	for (const char c : s) {
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b";  break;
		case '\f': out += "\\f";  break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
				out += buf;
			}
			else out += c;
			break;
		}
	}
	out += '"';
	return out;
}

bool parseJson(std::string_view text, Json& out, std::string& error) {
	// A glTF JSON chunk may be padded with trailing spaces (GLB pads with 0x20); a leading
	// UTF-8 BOM shows up in hand-edited files.
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
	    static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	Parser parser{ text };
	parser.skipSpace();
	if (!parser.parseValue(out)) { error = parser.error; return false; }
	parser.skipSpace();
	if (!parser.atEnd()) { error = "trailing characters at byte " + std::to_string(parser.pos); return false; }
	return true;
}

std::string writeJson(const Json& value, int indent) {
	std::string out;
	writeInto(out, value, indent);
	return out;
}

} // namespace aob
