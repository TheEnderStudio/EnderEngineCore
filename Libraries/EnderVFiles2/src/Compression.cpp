#include "Detail/Compression.hpp"

#if __has_include(<zlib/zlib.h>)
#include <zlib/zlib.h>
#elif __has_include(<zlib.h>)
#include <zlib.h>
#else
#error "EnderVFiles2 requires zlib. Configure the zlib include directory."
#endif

#include <algorithm>
#include <climits>
#include <cstddef>
#include <limits>
#include <span>

namespace EnderVFiles2::Detail
{
	Result<Buffer> compressBuffer(ByteSpan input, std::pmr::memory_resource* memoryResource, int level, std::size_t alignment)
	{
		if (level < 0 || level > 9)
		{
			return makeError(ErrorCode::InvalidArgument, "zlib compression level must be between 0 and 9");
		}
		if (input.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
		{
			return makeError(ErrorCode::FileTooLarge, "Input is too large for zlib compression");
		}

		const uLong sourceLength = static_cast<uLong>(input.size());
		const uLong bound = compressBound(sourceLength);
		if (bound == 0 && !input.empty())
		{
			return makeError(ErrorCode::CompressionFailed, "zlib compressBound returned zero");
		}

		Buffer output(memoryResource, static_cast<std::size_t>(bound), alignment);
		uLongf destinationLength = static_cast<uLongf>(output.capacity());

		const Bytef* source = input.empty() ? reinterpret_cast<const Bytef*>("") : input.data();
		const int result = compress2(output.data(), &destinationLength, source, sourceLength, level);
		if (result != Z_OK)
		{
			return makeError(ErrorCode::CompressionFailed, "zlib compress2 failed");
		}
		output.resize(static_cast<std::size_t>(destinationLength));
		return std::move(output);
	}

	Result<void> decompressBuffer(ByteSpan compressed, MutableByteSpan output)
	{
		if (compressed.empty())
		{
			return makeError(ErrorCode::DecompressionFailed, "Compressed input is empty");
		}
		if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
		{
			return makeError(ErrorCode::DecompressionFailed, "Compressed input is too large for zlib");
		}
		if (output.size() > static_cast<std::size_t>(std::numeric_limits<uLongf>::max()))
		{
			return makeError(ErrorCode::FileTooLarge, "Decompressed output is too large for zlib");
		}
		if (output.empty())
		{
			return makeError(ErrorCode::DecompressionFailed, "A compressed file cannot expand to zero bytes");
		}

		uLongf destinationLength = static_cast<uLongf>(output.size());
		uLong sourceLength = static_cast<uLong>(compressed.size());
		const int result = uncompress2(output.data(), &destinationLength, compressed.data(), &sourceLength);
		if (result != Z_OK)
		{
			return makeError(ErrorCode::DecompressionFailed, "zlib uncompress2 failed");
		}
		if (destinationLength != output.size())
		{
			return makeError(ErrorCode::DecompressionFailed, "Decompressed size does not match the index");
		}
		if (sourceLength != compressed.size())
		{
			return makeError(ErrorCode::CorruptIndex, "Trailing bytes after the compressed stream");
		}
		return {};
	}

	std::uint32_t updateCrc32(std::uint32_t crc, ByteSpan data) noexcept
	{
		uLong value = static_cast<uLong>(crc);
		std::size_t offset = 0;
		while (offset < data.size())
		{
			const std::size_t chunk = std::min<std::size_t>(data.size() - offset, 1024ull * 1024ull);
			value = crc32(value, data.data() + offset, static_cast<uInt>(chunk));
			offset += chunk;
		}
		return static_cast<std::uint32_t>(value);
	}

	std::uint32_t computeCrc32(ByteSpan data) noexcept
	{
		return updateCrc32(0, data);
	}
}
