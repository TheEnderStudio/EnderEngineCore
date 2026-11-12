#pragma once

#include "Crypto.hpp"
#include "Error.hpp"
#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace EnderVFiles2::Detail
{
	inline constexpr std::array<Byte, 8> IndexFileMagic = { 'E', 'V', 'F', '2', 'I', 'D', 'X', 0 };
	inline constexpr std::array<Byte, 8> VolumeFileMagic = { 'E', 'V', 'F', '2', 'V', 'O', 'L', 0 };
	inline constexpr std::array<Byte, 8> IndexPayloadMagic = { 'E', 'V', 'F', '2', 'P', 'A', 'Y', 0 };
	inline constexpr std::array<Byte, 8> FileAssociatedDataMagic = { 'E', 'V', 'F', '2', 'F', 'I', 'L', 0 };

	inline constexpr std::uint16_t FormatVersionMajor = 1;
	inline constexpr std::uint16_t FormatVersionMinor = 0;

	inline constexpr std::size_t IndexHeaderSize = 160;
	inline constexpr std::size_t IndexHeaderAuthenticatedSize = 136;
	inline constexpr std::size_t IndexHeaderCrcCoverage = 152;
	inline constexpr std::size_t VolumeHeaderSize = 64;
	inline constexpr std::size_t VolumeHeaderCrcCoverage = 40;

	inline constexpr std::uint32_t IndexFlagEncrypted = 1u << 0;
	inline constexpr std::uint32_t IndexFlagCompressed = 1u << 1;
	inline constexpr std::uint32_t VolumeFlagEncryptedContent = 1u << 0;
	inline constexpr std::uint32_t IndexPayloadFlagEncryptedContent = 1u << 0;

	inline constexpr std::size_t MaxPathBytes = 4096;
	inline constexpr std::size_t MaxPackageNameBytes = 200;
	inline constexpr std::size_t MaxVolumeNameBytes = 255;
	inline constexpr std::uint32_t MaxVolumes = 1000000;
	inline constexpr std::uint32_t MaxEntries = 4000000;
	inline constexpr std::uint64_t MaxStoredFileSize = 1ull << 40;
	inline constexpr std::uint64_t MaxIndexPayloadSize = 512ull * 1024ull * 1024ull;

	inline constexpr std::string_view IndexFileExtension = ".evfidx";
	inline constexpr std::string_view VolumeFileExtension = ".evfvol";
	inline constexpr std::string_view IndexKeyInfo = "EnderVFiles2/IndexKey/v1";

	struct IndexHeader
	{
		std::array<Byte, 8> magic = IndexFileMagic;
		std::uint16_t versionMajor = FormatVersionMajor;
		std::uint16_t versionMinor = FormatVersionMinor;
		std::uint32_t headerSize = static_cast<std::uint32_t>(IndexHeaderSize);
		std::uint32_t flags = 0;
		std::uint32_t kdfIterations = 0;
		CipherSuite cipherSuite = CipherSuite::None;
		std::array<Byte, KdfSaltSize> salt{};
		std::array<Byte, AesGcmNonceSize> nonce{};
		std::array<Byte, 16> packageUuid{};
		std::uint64_t payloadUncompressedSize = 0;
		std::uint64_t payloadStoredSize = 0;
		Sha256Digest payloadDigest{};
		AesGcmTag authTag{};
		std::uint32_t headerCrc32 = 0;
		std::uint32_t reserved = 0;
	};

	struct VolumeHeader
	{
		std::array<Byte, 8> magic = VolumeFileMagic;
		std::uint16_t versionMajor = FormatVersionMajor;
		std::uint16_t versionMinor = FormatVersionMinor;
		std::uint32_t headerSize = static_cast<std::uint32_t>(VolumeHeaderSize);
		std::uint32_t volumeIndex = 0;
		std::array<Byte, 16> packageUuid{};
		std::uint32_t flags = 0;
		std::uint32_t headerCrc32 = 0;
		std::uint32_t reserved0 = 0;
		std::array<Byte, 16> reserved1{};
	};

	Result<std::array<Byte, IndexHeaderSize>> serializeIndexHeader(const IndexHeader& header);
	Result<IndexHeader> parseIndexHeader(ByteSpan bytes);
	Result<std::array<Byte, VolumeHeaderSize>> serializeVolumeHeader(const VolumeHeader& header);
	Result<VolumeHeader> parseVolumeHeader(ByteSpan bytes);

	std::string formatVolumeIndex(std::uint32_t volumeIndex);
	std::string makeIndexFileName(std::string_view packageName);
	std::string makeVolumeFileName(std::string_view packageName, std::uint32_t volumeIndex);
}
