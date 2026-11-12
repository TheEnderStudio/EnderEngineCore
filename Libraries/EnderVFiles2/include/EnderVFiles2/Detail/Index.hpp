#pragma once

#include "ByteOrder.hpp"
#include "Compression.hpp"
#include "Crypto.hpp"
#include "Error.hpp"
#include "Format.hpp"
#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace EnderVFiles2::Detail
{
	enum class EntryKind : std::uint8_t
	{
		Directory = 0,
		File = 1,
	};

	enum class EncryptionMethod : std::uint8_t
	{
		None = 0,
		Aes256Gcm = 1,
	};

	inline constexpr std::uint32_t InvalidIndex = 0xFFFFFFFFu;
	inline constexpr std::uint16_t IndexPayloadVersionMajor = 1;
	inline constexpr std::uint16_t IndexPayloadVersionMinor = 0;

	struct IndexEntry
	{
		std::uint32_t pathOffset = 0;
		std::uint32_t pathLength = 0;
		std::uint32_t parentIndex = InvalidIndex;
		std::uint32_t firstChild = 0;
		std::uint32_t childCount = 0;
		std::uint64_t uncompressedSize = 0;
		std::uint64_t storedSize = 0;
		std::uint64_t volumeOffset = 0;
		std::uint32_t volumeIndex = InvalidIndex;
		std::uint32_t crc32 = 0;
		EntryKind kind = EntryKind::File;
		CompressionMethod compression = CompressionMethod::Store;
		EncryptionMethod encryption = EncryptionMethod::None;
		AesGcmNonce nonce{};
		AesGcmTag tag{};
	};

	struct Index
	{
		explicit Index(std::pmr::memory_resource* memoryResource);

		Index(Index&&) noexcept = default;
		Index& operator=(Index&&) noexcept = default;
		Index(const Index&) = delete;
		Index& operator=(const Index&) = delete;

		Result<void> appendEntry(std::string_view path, IndexEntry entry);
		Result<void> finalize();

		std::string_view pathOf(const IndexEntry& entry) const noexcept;
		const IndexEntry* find(std::string_view path) const noexcept;
		std::string_view nameOf(const IndexEntry& entry) const noexcept;
		std::uint32_t fileCount() const noexcept;
		std::uint32_t directoryCount() const noexcept;

		std::pmr::string packageName;
		std::pmr::string pathBlob;
		std::pmr::vector<IndexEntry> entries;
		std::pmr::vector<std::pmr::string> volumeNames;
		std::pmr::vector<std::uint32_t> childIndices;
		bool contentEncrypted = false;
		bool finalized = false;
	};

	Result<std::pmr::vector<Byte>> serializeIndexPayload(const Index& index);
	Result<Index> parseIndexPayload(ByteSpan payload, std::pmr::memory_resource* memoryResource, std::uint32_t maxEntries);

	std::pmr::vector<Byte> buildFileAssociatedData(
		std::pmr::memory_resource* memoryResource,
		ByteSpan packageUuid,
		std::string_view path,
		std::uint64_t uncompressedSize,
		std::uint64_t storedSize,
		CompressionMethod compression,
		EncryptionMethod encryption);
}
