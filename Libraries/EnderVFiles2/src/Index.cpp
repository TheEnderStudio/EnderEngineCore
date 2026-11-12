#include "Detail/Index.hpp"

#include "Detail/ByteOrder.hpp"
#include "Detail/Compression.hpp"
#include "Detail/Utility.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace EnderVFiles2::Detail
{
	namespace
	{
		bool isZero(ByteSpan bytes) noexcept
		{
			for (const Byte value : bytes)
			{
				if (value != 0)
				{
					return false;
				}
			}
			return true;
		}

		bool validateDirectoryEntry(const IndexEntry& entry)
		{
			return entry.uncompressedSize == 0
				&& entry.storedSize == 0
				&& entry.volumeOffset == 0
				&& entry.volumeIndex == InvalidIndex
				&& entry.crc32 == 0
				&& entry.compression == CompressionMethod::Store
				&& entry.encryption == EncryptionMethod::None
				&& isZero(ByteSpan(entry.nonce.data(), entry.nonce.size()))
				&& isZero(ByteSpan(entry.tag.data(), entry.tag.size()));
		}
	}

	Index::Index(std::pmr::memory_resource* memoryResource)
		: packageName(memoryResource)
		, pathBlob(memoryResource)
		, entries(memoryResource)
		, volumeNames(memoryResource)
		, childIndices(memoryResource)
	{
	}

	Result<void> Index::appendEntry(std::string_view path, IndexEntry entry)
	{
		if (finalized)
		{
			return makeError(ErrorCode::InvalidState, "Cannot append to a finalized index");
		}
		if (path.size() > MaxPathBytes)
		{
			return makeError(ErrorCode::PathTooLong, "Index path is too long");
		}
		if (pathBlob.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - path.size()
			|| pathBlob.size() + path.size() > MaxIndexPayloadSize)
		{
			return makeError(ErrorCode::IndexTooLarge, "Index path table exceeds the supported size");
		}

		entry.pathOffset = static_cast<std::uint32_t>(pathBlob.size());
		entry.pathLength = static_cast<std::uint32_t>(path.size());
		if (!path.empty())
		{
			pathBlob.append(path.data(), path.size());
		}
		entries.push_back(entry);
		return {};
	}

	std::string_view Index::pathOf(const IndexEntry& entry) const noexcept
	{
		if (entry.pathLength == 0)
		{
			return {};
		}
		return std::string_view(pathBlob.data() + entry.pathOffset, entry.pathLength);
	}

	std::string_view Index::nameOf(const IndexEntry& entry) const noexcept
	{
		const std::string_view path = pathOf(entry);
		const std::size_t separator = path.find_last_of('/');
		if (separator == std::string_view::npos)
		{
			return path;
		}
		return path.substr(separator + 1);
	}

	const IndexEntry* Index::find(std::string_view path) const noexcept
	{
		std::size_t low = 0;
		std::size_t high = entries.size();
		while (low < high)
		{
			const std::size_t middle = low + (high - low) / 2;
			const std::string_view candidate = pathOf(entries[middle]);
			const int comparison = candidate.compare(path);
			if (comparison < 0)
			{
				low = middle + 1;
			}
			else
			{
				high = middle;
			}
		}

		if (low < entries.size() && pathOf(entries[low]) == path)
		{
			return &entries[low];
		}
		return nullptr;
	}

	std::uint32_t Index::fileCount() const noexcept
	{
		std::uint32_t count = 0;
		for (const IndexEntry& entry : entries)
		{
			if (entry.kind == EntryKind::File)
			{
				++count;
			}
		}
		return count;
	}

	std::uint32_t Index::directoryCount() const noexcept
	{
		std::uint32_t count = 0;
		for (const IndexEntry& entry : entries)
		{
			if (entry.kind == EntryKind::Directory)
			{
				++count;
			}
		}
		return count;
	}

	Result<void> Index::finalize()
	{
		if (finalized)
		{
			return {};
		}
		if (entries.empty())
		{
			return makeError(ErrorCode::CorruptIndex, "Index contains no entries");
		}
		if (entries.size() > MaxEntries)
		{
			return makeError(ErrorCode::TooManyEntries, "Index contains too many entries");
		}
		if (packageName.empty() || !isValidPackageName(std::string_view(packageName.data(), packageName.size())))
		{
			return makeError(ErrorCode::InvalidFormat, "Index package name is invalid");
		}
		if (volumeNames.size() > MaxVolumes)
		{
			return makeError(ErrorCode::TooManyVolumes, "Index contains too many volumes");
		}

		std::pmr::memory_resource* memoryResource = entries.get_allocator().resource();
		std::pmr::vector<std::string_view> sortedVolumeNames(memoryResource);
		sortedVolumeNames.reserve(volumeNames.size());
		for (const std::pmr::string& volumeName : volumeNames)
		{
			if (!isValidFileName(std::string_view(volumeName.data(), volumeName.size())))
			{
				return makeError(ErrorCode::InvalidFormat, "Index contains an invalid volume file name");
			}
			sortedVolumeNames.push_back(std::string_view(volumeName.data(), volumeName.size()));
		}
		std::sort(sortedVolumeNames.begin(), sortedVolumeNames.end());
		for (std::size_t index = 1; index < sortedVolumeNames.size(); ++index)
		{
			if (sortedVolumeNames[index - 1] == sortedVolumeNames[index])
			{
				return makeError(ErrorCode::InvalidFormat, "Index contains duplicate volume file names");
			}
		}

		const IndexEntry& root = entries.front();
		if (root.pathLength != 0 || root.kind != EntryKind::Directory || !validateDirectoryEntry(root))
		{
			return makeError(ErrorCode::CorruptIndex, "Index root entry is invalid");
		}

		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			IndexEntry& entry = entries[index];
			const std::string_view path = pathOf(entry);
			if (index == 0)
			{
				entry.parentIndex = InvalidIndex;
				continue;
			}

			if (!isValidPackagePath(path, false))
			{
				return makeError(ErrorCode::UnsafePath, "Index contains an invalid file path");
			}
			if (path.compare(pathOf(entries[index - 1])) <= 0)
			{
				return makeError(ErrorCode::DuplicateEntry, "Index paths are not strictly sorted");
			}

			const std::size_t separator = path.find_last_of('/');
			const std::string_view parentPath = separator == std::string_view::npos ? std::string_view() : path.substr(0, separator);
			const IndexEntry* parent = find(parentPath);
			if (parent == nullptr || parent->kind != EntryKind::Directory)
			{
				return makeError(ErrorCode::CorruptIndex, "Index entry has no valid parent directory");
			}
			entry.parentIndex = static_cast<std::uint32_t>(parent - entries.data());

			if (entry.kind == EntryKind::Directory)
			{
				if (!validateDirectoryEntry(entry))
				{
					return makeError(ErrorCode::CorruptIndex, "Index directory entry contains file-only metadata");
				}
				continue;
			}

			if (entry.uncompressedSize > MaxStoredFileSize || entry.storedSize > MaxStoredFileSize)
			{
				return makeError(ErrorCode::FileTooLarge, "Index file entry exceeds the supported file size");
			}
			if (entry.volumeOffset > std::numeric_limits<std::uint64_t>::max() - entry.storedSize)
			{
				return makeError(ErrorCode::CorruptIndex, "Index file range overflows");
			}
			if (entry.volumeOffset < VolumeHeaderSize)
			{
				return makeError(ErrorCode::CorruptIndex, "Index file range overlaps the volume header");
			}
			if (volumeNames.empty() || entry.volumeIndex >= volumeNames.size())
			{
				return makeError(ErrorCode::CorruptIndex, "Index file entry references an invalid volume");
			}
			if (entry.compression == CompressionMethod::Store)
			{
				if (entry.storedSize != entry.uncompressedSize)
				{
					return makeError(ErrorCode::CorruptIndex, "Stored file sizes do not match");
				}
			}
			else if (entry.uncompressedSize == 0 || entry.storedSize == 0 || entry.storedSize > entry.uncompressedSize)
			{
				return makeError(ErrorCode::CorruptIndex, "Deflate file sizes are inconsistent");
			}

			if (contentEncrypted)
			{
				if (entry.encryption != EncryptionMethod::Aes256Gcm)
				{
					return makeError(ErrorCode::CorruptIndex, "Encrypted package contains an unencrypted file");
				}
			}
			else
			{
				if (entry.encryption != EncryptionMethod::None
					|| !isZero(ByteSpan(entry.nonce.data(), entry.nonce.size()))
					|| !isZero(ByteSpan(entry.tag.data(), entry.tag.size())))
				{
					return makeError(ErrorCode::CorruptIndex, "Unencrypted package contains encryption metadata");
				}
			}
		}

		std::pmr::vector<std::uint32_t> childCounts(entries.size(), 0, memoryResource);
		for (std::size_t index = 1; index < entries.size(); ++index)
		{
			const std::uint32_t parent = entries[index].parentIndex;
			if (parent >= entries.size())
			{
				return makeError(ErrorCode::CorruptIndex, "Index parent reference is out of range");
			}
			++childCounts[parent];
			entries[index].firstChild = 0;
			entries[index].childCount = 0;
		}
		entries.front().firstChild = 0;
		entries.front().childCount = 0;

		std::uint64_t totalChildren = 0;
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			if (entries[index].kind == EntryKind::Directory)
			{
				entries[index].firstChild = static_cast<std::uint32_t>(totalChildren);
				entries[index].childCount = childCounts[index];
				totalChildren += childCounts[index];
			}
		}
		if (totalChildren > std::numeric_limits<std::uint32_t>::max())
		{
			return makeError(ErrorCode::TooManyEntries, "Index child table is too large");
		}

		childIndices.assign(static_cast<std::size_t>(totalChildren), 0);
		std::pmr::vector<std::uint32_t> nextChild(entries.size(), 0, memoryResource);
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			nextChild[index] = entries[index].firstChild;
		}
		for (std::size_t index = 1; index < entries.size(); ++index)
		{
			const std::uint32_t parent = entries[index].parentIndex;
			childIndices[nextChild[parent]++] = static_cast<std::uint32_t>(index);
		}

		for (const IndexEntry& entry : entries)
		{
			if (entry.kind == EntryKind::Directory && entry.firstChild + entry.childCount > childIndices.size())
			{
				return makeError(ErrorCode::InternalFailure, "Index child table construction failed");
			}
		}

		finalized = true;
		return {};
	}

	Result<std::pmr::vector<Byte>> serializeIndexPayload(const Index& index)
	{
		if (!index.finalized)
		{
			return makeError(ErrorCode::InvalidState, "Cannot serialize an unfinalized index");
		}

		std::uint64_t estimatedSize = 24ull + 4ull + index.packageName.size() + index.pathBlob.size()
			+ static_cast<std::uint64_t>(index.entries.size()) * 68ull;
		for (const std::pmr::string& volumeName : index.volumeNames)
		{
			estimatedSize += 4ull + volumeName.size();
		}
		if (estimatedSize > MaxIndexPayloadSize)
		{
			return makeError(ErrorCode::IndexTooLarge, "Index payload exceeds the supported size");
		}

		std::pmr::memory_resource* memoryResource = index.entries.get_allocator().resource();
		std::pmr::vector<Byte> payload(memoryResource);
		payload.reserve(static_cast<std::size_t>(estimatedSize));

		BinaryWriter writer(payload);
		writer.writeBytes(ByteSpan(IndexPayloadMagic.data(), IndexPayloadMagic.size()));
		writer.writeU16(IndexPayloadVersionMajor);
		writer.writeU16(IndexPayloadVersionMinor);
		writer.writeU32(index.contentEncrypted ? IndexPayloadFlagEncryptedContent : 0);
		writer.writeU32(static_cast<std::uint32_t>(index.entries.size()));
		writer.writeU32(static_cast<std::uint32_t>(index.volumeNames.size()));

		auto packageNameResult = writer.writeString(std::string_view(index.packageName.data(), index.packageName.size()));
		if (!packageNameResult)
		{
			return std::move(packageNameResult).error();
		}
		for (const std::pmr::string& volumeName : index.volumeNames)
		{
			auto nameResult = writer.writeString(std::string_view(volumeName.data(), volumeName.size()));
			if (!nameResult)
			{
				return std::move(nameResult).error();
			}
		}

		for (const IndexEntry& entry : index.entries)
		{
			writer.writeU8(static_cast<std::uint8_t>(entry.kind));
			writer.writeU8(static_cast<std::uint8_t>(entry.compression));
			writer.writeU8(static_cast<std::uint8_t>(entry.encryption));
			writer.writeU8(0);
			auto pathResult = writer.writeString(index.pathOf(entry));
			if (!pathResult)
			{
				return std::move(pathResult).error();
			}
			writer.writeU64(entry.uncompressedSize);
			writer.writeU64(entry.storedSize);
			writer.writeU64(entry.volumeOffset);
			writer.writeU32(entry.volumeIndex);
			writer.writeU32(entry.crc32);
			writer.writeBytes(ByteSpan(entry.nonce.data(), entry.nonce.size()));
			writer.writeBytes(ByteSpan(entry.tag.data(), entry.tag.size()));
		}

		return std::move(payload);
	}

	Result<Index> parseIndexPayload(ByteSpan payload, std::pmr::memory_resource* memoryResource, std::uint32_t maxEntries)
	{
		BinaryReader reader(payload);
		auto magicResult = reader.readBytes(IndexPayloadMagic.size());
		if (!magicResult || !std::equal(IndexPayloadMagic.begin(), IndexPayloadMagic.end(), magicResult.value().begin()))
		{
			return makeError(ErrorCode::InvalidFormat, "Index payload magic does not match EnderVFiles2");
		}

		auto majorResult = reader.readU16();
		auto minorResult = reader.readU16();
		auto flagsResult = reader.readU32();
		auto entryCountResult = reader.readU32();
		auto volumeCountResult = reader.readU32();
		if (!majorResult || !minorResult || !flagsResult || !entryCountResult || !volumeCountResult)
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload header is truncated");
		}

		if (majorResult.value() != IndexPayloadVersionMajor || minorResult.value() > IndexPayloadVersionMinor)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Unsupported index payload version");
		}
		const std::uint32_t flags = flagsResult.value();
		if ((flags & ~IndexPayloadFlagEncryptedContent) != 0)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Index payload uses unknown flags");
		}

		const std::uint32_t entryCount = entryCountResult.value();
		const std::uint32_t volumeCount = volumeCountResult.value();
		if (entryCount == 0 || entryCount > maxEntries || entryCount > MaxEntries)
		{
			return makeError(ErrorCode::TooManyEntries, "Index payload entry count is invalid");
		}
		if (volumeCount > MaxVolumes)
		{
			return makeError(ErrorCode::TooManyVolumes, "Index payload volume count is invalid");
		}
		if (static_cast<std::uint64_t>(entryCount) * 68ull > reader.remaining())
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload entry table is truncated");
		}
		if (static_cast<std::uint64_t>(volumeCount) * 4ull > reader.remaining())
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload volume table is truncated");
		}

		Index index(memoryResource);
		index.contentEncrypted = (flags & IndexPayloadFlagEncryptedContent) != 0;

		auto packageNameResult = reader.readStringView(MaxPackageNameBytes);
		if (!packageNameResult)
		{
			return std::move(packageNameResult).error();
		}
		if (!packageNameResult.value().empty())
		{
			index.packageName.assign(packageNameResult.value().data(), packageNameResult.value().size());
		}

		for (std::uint32_t volumeIndex = 0; volumeIndex < volumeCount; ++volumeIndex)
		{
			auto volumeNameResult = reader.readStringView(MaxVolumeNameBytes);
			if (!volumeNameResult)
			{
				return std::move(volumeNameResult).error();
			}
			if (!isValidFileName(volumeNameResult.value()))
			{
				return makeError(ErrorCode::InvalidFormat, "Index payload contains an invalid volume name");
			}
			index.volumeNames.emplace_back(
				volumeNameResult.value().data(),
				volumeNameResult.value().size());
		}

		index.entries.reserve(entryCount);
		for (std::uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex)
		{
			auto kindResult = reader.readU8();
			auto compressionResult = reader.readU8();
			auto encryptionResult = reader.readU8();
			auto reservedResult = reader.readU8();
			if (!kindResult || !compressionResult || !encryptionResult || !reservedResult)
			{
				return makeError(ErrorCode::CorruptIndex, "Index payload entry is truncated");
			}
			if (kindResult.value() > static_cast<std::uint8_t>(EntryKind::File)
				|| compressionResult.value() > static_cast<std::uint8_t>(CompressionMethod::Deflate)
				|| encryptionResult.value() > static_cast<std::uint8_t>(EncryptionMethod::Aes256Gcm)
				|| reservedResult.value() != 0)
			{
				return makeError(ErrorCode::InvalidFormat, "Index payload entry uses unknown flags");
			}

			auto pathResult = reader.readStringView(MaxPathBytes);
			if (!pathResult)
			{
				return std::move(pathResult).error();
			}
			auto uncompressedResult = reader.readU64();
			auto storedResult = reader.readU64();
			auto offsetResult = reader.readU64();
			auto volumeIndexResult = reader.readU32();
			auto crcResult = reader.readU32();
			auto nonceResult = reader.readBytes(AesGcmNonceSize);
			auto tagResult = reader.readBytes(AesGcmTagSize);
			if (!uncompressedResult || !storedResult || !offsetResult || !volumeIndexResult
				|| !crcResult || !nonceResult || !tagResult)
			{
				return makeError(ErrorCode::CorruptIndex, "Index payload entry is truncated");
			}

			IndexEntry entry;
			entry.kind = static_cast<EntryKind>(kindResult.value());
			entry.compression = static_cast<CompressionMethod>(compressionResult.value());
			entry.encryption = static_cast<EncryptionMethod>(encryptionResult.value());
			entry.uncompressedSize = uncompressedResult.value();
			entry.storedSize = storedResult.value();
			entry.volumeOffset = offsetResult.value();
			entry.volumeIndex = volumeIndexResult.value();
			entry.crc32 = crcResult.value();
			std::copy(nonceResult.value().begin(), nonceResult.value().end(), entry.nonce.begin());
			std::copy(tagResult.value().begin(), tagResult.value().end(), entry.tag.begin());

			auto appendResult = index.appendEntry(pathResult.value(), entry);
			if (!appendResult)
			{
				return std::move(appendResult).error();
			}
		}

		if (reader.remaining() != 0)
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload contains trailing bytes");
		}

		auto finalizeResult = index.finalize();
		if (!finalizeResult)
		{
			return std::move(finalizeResult).error();
		}
		return std::move(index);
	}

	std::pmr::vector<Byte> buildFileAssociatedData(
		std::pmr::memory_resource* memoryResource,
		ByteSpan packageUuid,
		std::string_view path,
		std::uint64_t uncompressedSize,
		std::uint64_t storedSize,
		CompressionMethod compression,
		EncryptionMethod encryption)
	{
		std::pmr::vector<Byte> associatedData(memoryResource);
		associatedData.reserve(FileAssociatedDataMagic.size() + packageUuid.size() + 8 + 8 + 1 + 1 + 4 + path.size());

		BinaryWriter writer(associatedData);
		writer.writeBytes(ByteSpan(FileAssociatedDataMagic.data(), FileAssociatedDataMagic.size()));
		writer.writeBytes(packageUuid);
		writer.writeU64(uncompressedSize);
		writer.writeU64(storedSize);
		writer.writeU8(static_cast<std::uint8_t>(compression));
		writer.writeU8(static_cast<std::uint8_t>(encryption));
		writer.writeU32(static_cast<std::uint32_t>(path.size()));
		if (!path.empty())
		{
			writer.writeBytes(ByteSpan(reinterpret_cast<const Byte*>(path.data()), path.size()));
		}
		return std::move(associatedData);
	}
}
