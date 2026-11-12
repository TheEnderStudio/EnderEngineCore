#pragma once

#include "Error.hpp"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace EnderVFiles2::Detail
{
	bool isValidPackagePath(std::string_view path, bool allowRoot) noexcept;
	bool isValidPackageName(std::string_view name) noexcept;
	bool isValidFileName(std::string_view name) noexcept;
	bool isValidUtf8(std::string_view text) noexcept;

	Result<std::string> normalizeQueryPath(std::string_view path);
	Result<void> ensureDirectory(const std::filesystem::path& path);
	bool isPathInsideOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& parent);

	std::string utf8FromPath(const std::filesystem::path& path);
	std::filesystem::path pathFromUtf8(std::string_view text);
	Result<std::string> randomHex(std::size_t byteCount);
	std::string toHex(ByteSpan bytes);
	std::string errorMessage(const std::error_code& errorCode, std::string_view prefix);
}
