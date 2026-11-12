#include "Detail/Format.hpp"

#include "Detail/ByteOrder.hpp"
#include "Detail/Compression.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string>

namespace EnderVFiles2::Detail
{
	Result<std::array<Byte, IndexHeaderSize>> serializeIndexHeader(const IndexHeader& header)
	{
		std::array<Byte, IndexHeaderSize> bytes{};
		std::copy(header.magic.begin(), header.magic.end(), bytes.begin());
		writeLittleEndian(std::span<Byte>(bytes), 8, header.versionMajor);
		writeLittleEndian(std::span<Byte>(bytes), 10, header.versionMinor);
		writeLittleEndian(std::span<Byte>(bytes), 12, header.headerSize);
		writeLittleEndian(std::span<Byte>(bytes), 16, header.flags);
		writeLittleEndian(std::span<Byte>(bytes), 20, header.kdfIterations);
		writeLittleEndian(std::span<Byte>(bytes), 24, static_cast<std::uint32_t>(header.cipherSuite));
		std::copy(header.salt.begin(), header.salt.end(), bytes.begin() + 28);
		std::copy(header.nonce.begin(), header.nonce.end(), bytes.begin() + 60);
		std::copy(header.packageUuid.begin(), header.packageUuid.end(), bytes.begin() + 72);
		writeLittleEndian(std::span<Byte>(bytes), 88, header.payloadUncompressedSize);
		writeLittleEndian(std::span<Byte>(bytes), 96, header.payloadStoredSize);
		std::copy(header.payloadDigest.begin(), header.payloadDigest.end(), bytes.begin() + 104);
		std::copy(header.authTag.begin(), header.authTag.end(), bytes.begin() + 136);
		writeLittleEndian(std::span<Byte>(bytes), 152, std::uint32_t{ 0 });
		writeLittleEndian(std::span<Byte>(bytes), 156, std::uint32_t{ 0 });

		const std::uint32_t crc = updateCrc32(0, ByteSpan(bytes.data(), IndexHeaderCrcCoverage));
		writeLittleEndian(std::span<Byte>(bytes), 152, crc);
		return bytes;
	}

	Result<IndexHeader> parseIndexHeader(ByteSpan bytes)
	{
		if (bytes.size() < IndexHeaderSize)
		{
			return makeError(ErrorCode::InvalidFormat, "Index file is smaller than its header");
		}

		IndexHeader header;
		std::copy_n(bytes.data(), header.magic.size(), header.magic.begin());
		if (header.magic != IndexFileMagic)
		{
			return makeError(ErrorCode::InvalidFormat, "Index file magic does not match EnderVFiles2");
		}

		header.versionMajor = readLittleEndian<std::uint16_t>(bytes, 8);
		header.versionMinor = readLittleEndian<std::uint16_t>(bytes, 10);
		header.headerSize = readLittleEndian<std::uint32_t>(bytes, 12);
		header.flags = readLittleEndian<std::uint32_t>(bytes, 16);
		header.kdfIterations = readLittleEndian<std::uint32_t>(bytes, 20);
		header.cipherSuite = static_cast<CipherSuite>(readLittleEndian<std::uint32_t>(bytes, 24));
		std::copy_n(bytes.data() + 28, header.salt.size(), header.salt.begin());
		std::copy_n(bytes.data() + 60, header.nonce.size(), header.nonce.begin());
		std::copy_n(bytes.data() + 72, header.packageUuid.size(), header.packageUuid.begin());
		header.payloadUncompressedSize = readLittleEndian<std::uint64_t>(bytes, 88);
		header.payloadStoredSize = readLittleEndian<std::uint64_t>(bytes, 96);
		std::copy_n(bytes.data() + 104, header.payloadDigest.size(), header.payloadDigest.begin());
		std::copy_n(bytes.data() + 136, header.authTag.size(), header.authTag.begin());
		header.headerCrc32 = readLittleEndian<std::uint32_t>(bytes, 152);
		header.reserved = readLittleEndian<std::uint32_t>(bytes, 156);

		if (header.headerSize != IndexHeaderSize)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Unsupported index header size");
		}
		if (header.headerSize > bytes.size())
		{
			return makeError(ErrorCode::InvalidFormat, "Index header size exceeds file size");
		}

		const std::uint32_t expectedCrc = updateCrc32(0, ByteSpan(bytes.data(), IndexHeaderCrcCoverage));
		if (expectedCrc != header.headerCrc32)
		{
			return makeError(ErrorCode::CorruptIndex, "Index header checksum mismatch");
		}
		return header;
	}

	Result<std::array<Byte, VolumeHeaderSize>> serializeVolumeHeader(const VolumeHeader& header)
	{
		std::array<Byte, VolumeHeaderSize> bytes{};
		std::copy(header.magic.begin(), header.magic.end(), bytes.begin());
		writeLittleEndian(std::span<Byte>(bytes), 8, header.versionMajor);
		writeLittleEndian(std::span<Byte>(bytes), 10, header.versionMinor);
		writeLittleEndian(std::span<Byte>(bytes), 12, header.headerSize);
		writeLittleEndian(std::span<Byte>(bytes), 16, header.volumeIndex);
		std::copy(header.packageUuid.begin(), header.packageUuid.end(), bytes.begin() + 20);
		writeLittleEndian(std::span<Byte>(bytes), 36, header.flags);
		writeLittleEndian(std::span<Byte>(bytes), 40, std::uint32_t{ 0 });
		writeLittleEndian(std::span<Byte>(bytes), 44, std::uint32_t{ 0 });
		std::fill(bytes.begin() + 48, bytes.end(), Byte{ 0 });

		const std::uint32_t crc = updateCrc32(0, ByteSpan(bytes.data(), VolumeHeaderCrcCoverage));
		writeLittleEndian(std::span<Byte>(bytes), 40, crc);
		return bytes;
	}

	Result<VolumeHeader> parseVolumeHeader(ByteSpan bytes)
	{
		if (bytes.size() < VolumeHeaderSize)
		{
			return makeError(ErrorCode::InvalidFormat, "Volume file is smaller than its header");
		}

		VolumeHeader header;
		std::copy_n(bytes.data(), header.magic.size(), header.magic.begin());
		if (header.magic != VolumeFileMagic)
		{
			return makeError(ErrorCode::InvalidFormat, "Volume file magic does not match EnderVFiles2");
		}

		header.versionMajor = readLittleEndian<std::uint16_t>(bytes, 8);
		header.versionMinor = readLittleEndian<std::uint16_t>(bytes, 10);
		header.headerSize = readLittleEndian<std::uint32_t>(bytes, 12);
		header.volumeIndex = readLittleEndian<std::uint32_t>(bytes, 16);
		std::copy_n(bytes.data() + 20, header.packageUuid.size(), header.packageUuid.begin());
		header.flags = readLittleEndian<std::uint32_t>(bytes, 36);
		header.headerCrc32 = readLittleEndian<std::uint32_t>(bytes, 40);
		header.reserved0 = readLittleEndian<std::uint32_t>(bytes, 44);
		std::copy_n(bytes.data() + 48, header.reserved1.size(), header.reserved1.begin());

		if (header.headerSize != VolumeHeaderSize)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Unsupported volume header size");
		}
		if (header.headerSize > bytes.size())
		{
			return makeError(ErrorCode::InvalidFormat, "Volume header size exceeds file size");
		}

		const std::uint32_t expectedCrc = updateCrc32(0, ByteSpan(bytes.data(), VolumeHeaderCrcCoverage));
		if (expectedCrc != header.headerCrc32)
		{
			return makeError(ErrorCode::CorruptIndex, "Volume header checksum mismatch");
		}
		return header;
	}

	std::string formatVolumeIndex(std::uint32_t volumeIndex)
	{
		char buffer[16] = {};
		const auto result = std::to_chars(buffer, buffer + sizeof(buffer), volumeIndex);
		std::string digits(buffer, result.ptr);
		if (digits.size() < 4)
		{
			digits.insert(digits.begin(), 4 - digits.size(), '0');
		}
		return digits;
	}

	std::string makeIndexFileName(std::string_view packageName)
	{
		std::string result(packageName);
		result.append(IndexFileExtension);
		return result;
	}

	std::string makeVolumeFileName(std::string_view packageName, std::uint32_t volumeIndex)
	{
		std::string result(packageName);
		result.append(VolumeFileExtension);
		result.append(formatVolumeIndex(volumeIndex));
		return result;
	}
}
