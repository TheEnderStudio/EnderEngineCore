#include "Detail/Utility.hpp"

#include "Detail/Crypto.hpp"
#include "Detail/Format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <system_error>

namespace EnderVFiles2::Detail
{
	namespace
	{
		bool hasControlCharacter(std::string_view text) noexcept
		{
			for (const char character : text)
			{
				const unsigned char value = static_cast<unsigned char>(character);
				if (value < 0x20 || value == 0x7F)
				{
					return true;
				}
			}
			return false;
		}

		bool isValidUtf8Impl(std::string_view text) noexcept
		{
			std::size_t index = 0;
			while (index < text.size())
			{
				const unsigned char first = static_cast<unsigned char>(text[index]);
				if (first < 0x80)
				{
					++index;
					continue;
				}

				std::uint32_t codePoint = 0;
				std::size_t continuation = 0;
				if ((first & 0xE0) == 0xC0)
				{
					codePoint = first & 0x1F;
					continuation = 1;
					if (codePoint < 0x02)
					{
						return false;
					}
				}
				else if ((first & 0xF0) == 0xE0)
				{
					codePoint = first & 0x0F;
					continuation = 2;
				}
				else if ((first & 0xF8) == 0xF0)
				{
					codePoint = first & 0x07;
					continuation = 3;
					if (codePoint > 0x04)
					{
						return false;
					}
				}
				else
				{
					return false;
				}

				if (index + continuation >= text.size())
				{
					return false;
				}
				for (std::size_t offset = 1; offset <= continuation; ++offset)
				{
					const unsigned char next = static_cast<unsigned char>(text[index + offset]);
					if ((next & 0xC0) != 0x80)
					{
						return false;
					}
					codePoint = (codePoint << 6) | (next & 0x3F);
				}

				if (continuation == 2 && codePoint < 0x0800)
				{
					return false;
				}
				if (continuation == 3 && codePoint < 0x10000)
				{
					return false;
				}
				if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
				{
					return false;
				}
				if (codePoint > 0x10FFFF)
				{
					return false;
				}
				index += continuation + 1;
			}
			return true;
		}

		bool isWindowsReservedName(std::string_view name) noexcept
		{
			std::string base(name);
			const std::size_t dot = base.find('.');
			if (dot != std::string::npos)
			{
				base.resize(dot);
			}
			for (char& character : base)
			{
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}

			if (base == "con" || base == "prn" || base == "aux" || base == "nul")
			{
				return true;
			}
			if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0))
			{
				return base[3] >= '1' && base[3] <= '9';
			}
			return false;
		}

		std::string normalizedPathForComparison(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::weakly_canonical(path, error);
			if (error)
			{
				error.clear();
				absolute = std::filesystem::absolute(path, error);
				if (error)
				{
					return {};
				}
			}

			std::string result = utf8FromPath(absolute.lexically_normal());
#ifdef _WIN32
			for (char& character : result)
			{
				if (character == '\\')
				{
					character = '/';
				}
				else
				{
					character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
				}
			}
#endif
			return result;
		}
	}

	bool isValidPackagePath(std::string_view path, bool allowRoot) noexcept
	{		if (path.empty())
		{
			return allowRoot;
		}
		if (path.size() > MaxPathBytes || path.front() == '/' || path.back() == '/' || hasControlCharacter(path))
		{
			return false;
		}

		std::size_t start = 0;
		while (start <= path.size())
		{
			const std::size_t separator = path.find('/', start);
			const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
			const std::string_view component = path.substr(start, end - start);
			if (component.empty() || component == "." || component == "..")
			{
				return false;
			}
			if (component.find('\\') != std::string_view::npos)
			{
				return false;
			}
			if (separator == std::string_view::npos)
			{
				break;
			}
			start = separator + 1;
		}
		return true;
	}

	bool isValidPackageName(std::string_view name) noexcept
	{
		if (name.empty() || name.size() > MaxPackageNameBytes || hasControlCharacter(name) || !isValidUtf8Impl(name))
		{
			return false;
		}
		if (name == "." || name == "..")
		{
			return false;
		}
		if (name.back() == ' ' || name.back() == '.')
		{
			return false;
		}
		for (const char character : name)
		{
			switch (character)
			{
			case '/':
			case '\\':
			case ':':
			case '*':
			case '?':
			case '"':
			case '<':
			case '>':
			case '|':
				return false;
			default:
				break;
			}
		}
		return !isWindowsReservedName(name);
	}

	bool isValidFileName(std::string_view name) noexcept
	{
		if (name.empty() || name.size() > MaxVolumeNameBytes || hasControlCharacter(name) || !isValidUtf8Impl(name))
		{
			return false;
		}
		if (name == "." || name == "..")
		{
			return false;
		}
		for (const char character : name)
		{
			switch (character)
			{
			case '/':
			case '\\':
			case ':':
			case '*':
			case '?':
			case '"':
			case '<':
			case '>':
			case '|':
				return false;
			default:
				break;
			}
		}
		return !isWindowsReservedName(name);
	}

	bool isValidUtf8(std::string_view text) noexcept
	{
		return isValidUtf8Impl(text);
	}

	Result<std::string> normalizeQueryPath(std::string_view path)
	{
		std::string result;
		result.reserve(path.size());

		std::size_t start = 0;
		while (start <= path.size())
		{
			const std::size_t separator = path.find_first_of("/\\", start);
			const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
			const std::string_view component = path.substr(start, end - start);

			if (!component.empty() && component != ".")
			{
				if (component == "..")
				{
					return makeError(ErrorCode::UnsafePath, "Path traversal is not allowed");
				}
				if (hasControlCharacter(component))
				{
					return makeError(ErrorCode::UnsafePath, "Path contains a control character");
				}
				if (!result.empty())
				{
					result.push_back('/');
				}
				result.append(component);
			}

			if (separator == std::string_view::npos)
			{
				break;
			}
			start = separator + 1;
		}

		if (result.size() > MaxPathBytes)
		{
			return makeError(ErrorCode::PathTooLong, "Path exceeds the maximum length");
		}
		return result;
	}

	Result<void> ensureDirectory(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			return makeError(ErrorCode::InvalidArgument, "Directory path is empty");
		}

		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		if (error)
		{
			return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to inspect output directory"));
		}
		if (exists)
		{
			if (!std::filesystem::is_directory(path, error) || error)
			{
				return makeError(ErrorCode::IoFailure, "Output path exists but is not a directory");
			}
			return {};
		}

		if (!std::filesystem::create_directories(path, error) || error)
		{
			return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to create output directory"));
		}
		return {};
	}

	bool isPathInsideOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& parent)
	{
		const std::string candidateText = normalizedPathForComparison(candidate);
		const std::string parentText = normalizedPathForComparison(parent);
		if (candidateText.empty() || parentText.empty())
		{
			return false;
		}
		if (candidateText == parentText)
		{
			return true;
		}
		if (candidateText.size() <= parentText.size())
		{
			return false;
		}
		if (candidateText.compare(0, parentText.size(), parentText) != 0)
		{
			return false;
		}
		if (parentText.back() == '/')
		{
			return true;
		}
		return candidateText[parentText.size()] == '/';
	}

	std::string utf8FromPath(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	std::filesystem::path pathFromUtf8(std::string_view text)
	{
		std::u8string utf8;
		utf8.reserve(text.size());
		for (const char character : text)
		{
			utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
		}
		return std::filesystem::path(utf8);
	}

	Result<std::string> randomHex(std::size_t byteCount)
	{
		if (byteCount > 64)
		{
			return makeError(ErrorCode::InvalidArgument, "randomHex supports at most 64 bytes");
		}

		static constexpr char HexDigits[] = "0123456789abcdef";
		std::array<Byte, 64> bytes{};
		auto randomResult = randomBytes(MutableByteSpan(bytes.data(), byteCount));
		if (!randomResult)
		{
			return std::move(randomResult).error();
		}

		std::string result;
		result.resize(byteCount * 2);
		for (std::size_t index = 0; index < byteCount; ++index)
		{
			result[index * 2] = HexDigits[bytes[index] >> 4];
			result[index * 2 + 1] = HexDigits[bytes[index] & 0x0F];
		}
		secureZero(MutableByteSpan(bytes.data(), bytes.size()));
		return result;
	}

	std::string toHex(ByteSpan bytes)
	{
		static constexpr char HexDigits[] = "0123456789abcdef";
		std::string result;
		result.resize(bytes.size() * 2);
		for (std::size_t index = 0; index < bytes.size(); ++index)
		{
			result[index * 2] = HexDigits[bytes[index] >> 4];
			result[index * 2 + 1] = HexDigits[bytes[index] & 0x0F];
		}
		return result;
	}

	std::string errorMessage(const std::error_code& errorCode, std::string_view prefix)
	{
		if (errorCode)
		{
			return std::string(prefix) + ": " + errorCode.message();
		}
		return std::string(prefix);
	}
}
