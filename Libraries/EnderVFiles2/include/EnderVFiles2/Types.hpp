#pragma once

#include <cstdint>
#include <span>

namespace EnderVFiles2
{
	using Byte = std::uint8_t;
	using ByteSpan = std::span<const Byte>;
	using MutableByteSpan = std::span<Byte>;
}
