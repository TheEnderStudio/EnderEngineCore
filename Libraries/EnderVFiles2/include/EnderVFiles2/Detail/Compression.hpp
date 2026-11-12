#pragma once

#include "Buffer.hpp"
#include "Error.hpp"
#include "Types.hpp"

#include <cstdint>
#include <memory_resource>
#include <span>

namespace EnderVFiles2::Detail
{
	enum class CompressionMethod : std::uint8_t
	{
		Store = 0,
		Deflate = 1,
	};

	Result<Buffer> compressBuffer(ByteSpan input, std::pmr::memory_resource* memoryResource, int level, std::size_t alignment);
	Result<void> decompressBuffer(ByteSpan compressed, MutableByteSpan output);
	std::uint32_t computeCrc32(ByteSpan data) noexcept;
	std::uint32_t updateCrc32(std::uint32_t crc, ByteSpan data) noexcept;
}
