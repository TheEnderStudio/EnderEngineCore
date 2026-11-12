#pragma once

#include "Types.hpp"
#include "Error.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace EnderVFiles2::Detail
{
	template <typename Integer>
	void writeLittleEndian(std::span<Byte> destination, std::size_t offset, Integer value) noexcept
	{
		static_assert(std::is_unsigned_v<Integer>, "Little-endian helpers require unsigned integers");
		for (std::size_t index = 0; index < sizeof(Integer); ++index)
		{
			destination[offset + index] = static_cast<Byte>((value >> (index * 8)) & static_cast<Integer>(0xFF));
		}
	}

	template <typename Integer>
	Integer readLittleEndian(ByteSpan source, std::size_t offset) noexcept
	{
		static_assert(std::is_unsigned_v<Integer>, "Little-endian helpers require unsigned integers");
		Integer value = 0;
		for (std::size_t index = 0; index < sizeof(Integer); ++index)
		{
			value |= static_cast<Integer>(source[offset + index]) << (index * 8);
		}
		return value;
	}

	class BinaryWriter
	{
	public:
		explicit BinaryWriter(std::pmr::vector<Byte>& destination) noexcept
			: m_destination(destination)
		{
		}

		std::size_t position() const noexcept
		{
			return m_destination.size();
		}

		void writeU8(std::uint8_t value)
		{
			m_destination.push_back(static_cast<Byte>(value));
		}

		void writeU16(std::uint16_t value)
		{
			const std::size_t offset = m_destination.size();
			m_destination.resize(offset + sizeof(value));
			writeLittleEndian(std::span<Byte>(m_destination.data() + offset, sizeof(value)), 0, value);
		}

		void writeU32(std::uint32_t value)
		{
			const std::size_t offset = m_destination.size();
			m_destination.resize(offset + sizeof(value));
			writeLittleEndian(std::span<Byte>(m_destination.data() + offset, sizeof(value)), 0, value);
		}

		void writeU64(std::uint64_t value)
		{
			const std::size_t offset = m_destination.size();
			m_destination.resize(offset + sizeof(value));
			writeLittleEndian(std::span<Byte>(m_destination.data() + offset, sizeof(value)), 0, value);
		}

		Result<void> writeString(std::string_view text)
		{
			if (text.size() > 0xFFFFFFFFull)
			{
				return makeError(ErrorCode::InvalidArgument, "String is too long to serialize");
			}
			writeU32(static_cast<std::uint32_t>(text.size()));
			if (!text.empty())
			{
				m_destination.insert(m_destination.end(),
					reinterpret_cast<const Byte*>(text.data()),
					reinterpret_cast<const Byte*>(text.data()) + text.size());
			}
			return {};
		}

		void writeBytes(ByteSpan bytes)
		{
			if (!bytes.empty())
			{
				m_destination.insert(m_destination.end(), bytes.begin(), bytes.end());
			}
		}

	private:
		std::pmr::vector<Byte>& m_destination;
	};

	class BinaryReader
	{
	public:
		explicit BinaryReader(ByteSpan source) noexcept
			: m_source(source)
		{
		}

		std::size_t offset() const noexcept
		{
			return m_offset;
		}

		std::size_t remaining() const noexcept
		{
			return m_source.size() - m_offset;
		}

		Result<std::uint8_t> readU8()
		{
			if (remaining() < sizeof(std::uint8_t))
			{
				return makeError(ErrorCode::CorruptIndex, "Unexpected end of index payload");
			}
			return m_source[m_offset++];
		}

		Result<std::uint16_t> readU16()
		{
			if (remaining() < sizeof(std::uint16_t))
			{
				return makeError(ErrorCode::CorruptIndex, "Unexpected end of index payload");
			}
			const std::uint16_t value = readLittleEndian<std::uint16_t>(m_source, m_offset);
			m_offset += sizeof(std::uint16_t);
			return value;
		}

		Result<std::uint32_t> readU32()
		{
			if (remaining() < sizeof(std::uint32_t))
			{
				return makeError(ErrorCode::CorruptIndex, "Unexpected end of index payload");
			}
			const std::uint32_t value = readLittleEndian<std::uint32_t>(m_source, m_offset);
			m_offset += sizeof(std::uint32_t);
			return value;
		}

		Result<std::uint64_t> readU64()
		{
			if (remaining() < sizeof(std::uint64_t))
			{
				return makeError(ErrorCode::CorruptIndex, "Unexpected end of index payload");
			}
			const std::uint64_t value = readLittleEndian<std::uint64_t>(m_source, m_offset);
			m_offset += sizeof(std::uint64_t);
			return value;
		}

		Result<ByteSpan> readBytes(std::size_t count)
		{
			if (count > remaining())
			{
				return makeError(ErrorCode::CorruptIndex, "Unexpected end of index payload");
			}
			ByteSpan bytes = m_source.subspan(m_offset, count);
			m_offset += count;
			return bytes;
		}

		Result<std::string_view> readStringView(std::size_t maxLength)
		{
			auto lengthResult = readU32();
			if (!lengthResult)
			{
				return std::move(lengthResult).error();
			}
			const std::uint32_t length = lengthResult.value();
			if (length > maxLength)
			{
				return makeError(ErrorCode::CorruptIndex, "Serialized string exceeds the allowed length");
			}
			auto bytesResult = readBytes(length);
			if (!bytesResult)
			{
				return std::move(bytesResult).error();
			}
			const ByteSpan bytes = bytesResult.value();
			if (bytes.empty())
			{
				return std::string_view();
			}
			return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}

	private:
		ByteSpan m_source;
		std::size_t m_offset = 0;
	};
}
