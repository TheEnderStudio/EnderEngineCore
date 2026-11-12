#include "Reader.hpp"

#include "Detail/Crypto.hpp"
#include "Detail/Format.hpp"
#include "Detail/Index.hpp"
#include "Detail/NativeFile.hpp"
#include "Detail/ReaderState.hpp"
#include "Detail/Utility.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace EnderVFiles2
{
	namespace Detail
	{
		inline constexpr std::size_t DecryptionChunkSize = 1024ull * 1024ull;

		struct SecureKeyGuard
		{
			~SecureKeyGuard()
			{
				secureZero(std::span<Byte>(key.data(), key.size()));
			}

			AesGcmKey key{};
		};
	}

	namespace
	{
		Result<void> validateReaderConfig(const ReaderConfig& config)
		{
			if (config.bufferAlignment == 0
				|| !std::has_single_bit(config.bufferAlignment)
				|| config.bufferAlignment > 2ull * 1024ull * 1024ull)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::bufferAlignment must be a power of two up to 2 MiB");
			}
			if (config.password.size() > 4096)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::password is too long");
			}
			if (config.maxKdfIterations == 0)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxKdfIterations must be positive");
			}
			if (config.maxIndexFileBytes < Detail::IndexHeaderSize)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxIndexFileBytes is too small");
			}
			if (config.maxIndexPayloadBytes == 0)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxIndexPayloadBytes must be positive");
			}
			if (config.maxFileBytes == 0)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxFileBytes must be positive");
			}
			if (config.maxEntries == 0)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxEntries must be positive");
			}
			if (config.maxVolumeCount == 0)
			{
				return makeError(ErrorCode::InvalidArgument, "ReaderConfig::maxVolumeCount must be positive");
			}
			return {};
		}
	}

	Reader::Reader(ReaderConfig config)
		: m_config(std::move(config))
		, m_memoryResource(m_config.memoryResource != nullptr ? m_config.memoryResource : std::pmr::new_delete_resource())
		, m_threadPool(m_config.threadPool)
	{
	}

	Reader::Reader(Reader&& other) noexcept
		: m_config(std::move(other.m_config))
		, m_memoryResource(other.m_memoryResource)
		, m_threadPool(other.m_threadPool)
		, m_state(std::move(other.m_state))
	{
		other.m_memoryResource = nullptr;
		other.m_threadPool.reset();
	}

	Reader& Reader::operator=(Reader&& other) noexcept
	{
		if (this != &other)
		{
			Detail::secureClear(m_config.password);
			m_config = std::move(other.m_config);
			m_memoryResource = other.m_memoryResource;
			m_threadPool = other.m_threadPool;
			m_state = std::move(other.m_state);
			other.m_memoryResource = nullptr;
			other.m_threadPool.reset();
		}
		return *this;
	}

	Reader::~Reader()
	{
		Detail::secureClear(m_config.password);
	}

	Result<void> Reader::mount(const std::filesystem::path& indexPath)
	{
		if (m_state != nullptr)
		{
			return makeError(ErrorCode::InvalidState, "Reader is already mounted");
		}

		auto configResult = validateReaderConfig(m_config);
		if (!configResult)
		{
			return std::move(configResult).error();
		}
		if (indexPath.empty())
		{
			return makeError(ErrorCode::InvalidArgument, "Index path is empty");
		}

		std::error_code error;
		auto indexFileResult = Detail::NativeFile::openRead(indexPath);
		if (!indexFileResult)
		{
			return std::move(indexFileResult).error();
		}
		Detail::NativeFile indexFile = std::move(indexFileResult).value();

		auto indexSizeResult = indexFile.size();
		if (!indexSizeResult)
		{
			return std::move(indexSizeResult).error();
		}
		const std::uint64_t indexSize = indexSizeResult.value();
		if (indexSize < Detail::IndexHeaderSize || indexSize > m_config.maxIndexFileBytes)
		{
			return makeError(ErrorCode::IndexTooLarge, "Index file size is outside the configured bounds");
		}
		if (indexSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return makeError(ErrorCode::IndexTooLarge, "Index file cannot fit into memory");
		}

		Buffer indexBuffer(m_memoryResource, static_cast<std::size_t>(indexSize), m_config.bufferAlignment);
		auto indexReadResult = indexFile.readAt(0, indexBuffer.bytes());
		if (!indexReadResult)
		{
			return std::move(indexReadResult).error();
		}

		auto headerResult = Detail::parseIndexHeader(indexBuffer.bytes().first(Detail::IndexHeaderSize));
		if (!headerResult)
		{
			return std::move(headerResult).error();
		}
		const Detail::IndexHeader& header = headerResult.value();

		if (header.versionMajor != Detail::FormatVersionMajor || header.versionMinor > Detail::FormatVersionMinor)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Unsupported index format version");
		}
		if ((header.flags & ~(Detail::IndexFlagEncrypted | Detail::IndexFlagCompressed)) != 0)
		{
			return makeError(ErrorCode::UnsupportedVersion, "Index uses unknown flags");
		}

		const bool encryptedIndex = (header.flags & Detail::IndexFlagEncrypted) != 0;
		const bool compressedIndex = (header.flags & Detail::IndexFlagCompressed) != 0;
		if (header.payloadStoredSize == 0 || header.payloadUncompressedSize == 0)
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload is empty");
		}
		if (header.payloadStoredSize > indexSize - Detail::IndexHeaderSize)
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload is truncated");
		}
		if (header.payloadStoredSize != indexSize - Detail::IndexHeaderSize)
		{
			return makeError(ErrorCode::CorruptIndex, "Index file contains trailing bytes");
		}
		if (header.payloadStoredSize > m_config.maxIndexPayloadBytes
			|| header.payloadUncompressedSize > m_config.maxIndexPayloadBytes)
		{
			return makeError(ErrorCode::IndexTooLarge, "Index payload exceeds the configured limit");
		}
		if (header.payloadStoredSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
			|| header.payloadUncompressedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		{
			return makeError(ErrorCode::IndexTooLarge, "Index payload cannot fit into memory");
		}
		if (compressedIndex)
		{
			if (header.payloadStoredSize > header.payloadUncompressedSize)
			{
				return makeError(ErrorCode::CorruptIndex, "Compressed index payload has an invalid size");
			}
		}
		else if (header.payloadStoredSize != header.payloadUncompressedSize)
		{
			return makeError(ErrorCode::CorruptIndex, "Uncompressed index payload has inconsistent sizes");
		}

		Detail::SecureKeyGuard masterKeyGuard;
		if (encryptedIndex)
		{
			if (header.cipherSuite != Detail::CipherSuite::Aes256Gcm)
			{
				return makeError(ErrorCode::UnsupportedVersion, "Index uses an unsupported cipher suite");
			}
			if (header.kdfIterations == 0 || header.kdfIterations > m_config.maxKdfIterations)
			{
				return makeError(ErrorCode::InvalidFormat, "Index KDF iteration count is outside the allowed range");
			}
			if (m_config.password.empty())
			{
				return makeError(ErrorCode::PasswordRequired, "The package index requires a password");
			}

			auto deriveResult = Detail::pbkdf2Sha256(
				std::string_view(m_config.password.data(), m_config.password.size()),
				ByteSpan(header.salt.data(), header.salt.size()),
				header.kdfIterations,
				MutableByteSpan(masterKeyGuard.key.data(), masterKeyGuard.key.size()));
			if (!deriveResult)
			{
				return std::move(deriveResult).error();
			}
		}
		else
		{
			if (header.cipherSuite != Detail::CipherSuite::None || header.kdfIterations != 0)
			{
				return makeError(ErrorCode::CorruptIndex, "Unencrypted index contains cipher settings");
			}
		}

		Buffer storedPayload(m_memoryResource, static_cast<std::size_t>(header.payloadStoredSize), m_config.bufferAlignment);
		std::memcpy(storedPayload.data(), indexBuffer.data() + Detail::IndexHeaderSize, storedPayload.size());

		if (encryptedIndex)
		{
			Detail::SecureKeyGuard indexKeyGuard;
			auto keyResult = Detail::hkdfSha256(
				ByteSpan(masterKeyGuard.key.data(), masterKeyGuard.key.size()),
				ByteSpan(header.nonce.data(), header.nonce.size()),
				ByteSpan(reinterpret_cast<const Byte*>(Detail::IndexKeyInfo.data()), Detail::IndexKeyInfo.size()),
				MutableByteSpan(indexKeyGuard.key.data(), indexKeyGuard.key.size()));
			if (!keyResult)
			{
				return std::move(keyResult).error();
			}

			Detail::AesGcmDecryptor decryptor;
			auto initializeResult = decryptor.initialize(
				ByteSpan(indexKeyGuard.key.data(), indexKeyGuard.key.size()),
				ByteSpan(header.nonce.data(), header.nonce.size()),
				ByteSpan(indexBuffer.data(), Detail::IndexHeaderAuthenticatedSize));
			if (!initializeResult)
			{
				return std::move(initializeResult).error();
			}
			auto updateResult = decryptor.update(storedPayload.bytes(), storedPayload.bytes());
			if (!updateResult)
			{
				Detail::secureZero(storedPayload.bytes());
				return std::move(updateResult).error();
			}
			auto finalizeResult = decryptor.finalize(ByteSpan(header.authTag.data(), header.authTag.size()));
			if (!finalizeResult)
			{
				Detail::secureZero(storedPayload.bytes());
				return std::move(finalizeResult).error();
			}
		}

		Buffer plainPayload(m_memoryResource, static_cast<std::size_t>(header.payloadUncompressedSize), m_config.bufferAlignment);
		ByteSpan plainSpan;
		if (compressedIndex)
		{
			auto decompressResult = Detail::decompressBuffer(storedPayload.bytes(), plainPayload.bytes());
			if (!decompressResult)
			{
				return std::move(decompressResult).error();
			}
			plainSpan = plainPayload.bytes();
		}
		else
		{
			if (storedPayload.size() != plainPayload.size())
			{
				return makeError(ErrorCode::CorruptIndex, "Uncompressed index payload size is invalid");
			}
			std::memcpy(plainPayload.data(), storedPayload.data(), storedPayload.size());
			plainSpan = plainPayload.bytes();
		}

		Detail::Sha256Digest computedDigest{};
		auto digestResult = Detail::sha256(plainSpan, computedDigest);
		if (!digestResult)
		{
			return std::move(digestResult).error();
		}
		if (!Detail::constantTimeEqual(
			ByteSpan(computedDigest.data(), computedDigest.size()),
			ByteSpan(header.payloadDigest.data(), header.payloadDigest.size())))
		{
			return makeError(ErrorCode::CorruptIndex, "Index payload digest mismatch");
		}

		auto parsedIndexResult = Detail::parseIndexPayload(plainSpan, m_memoryResource, m_config.maxEntries);
		if (!parsedIndexResult)
		{
			return std::move(parsedIndexResult).error();
		}
		Detail::Index index = std::move(parsedIndexResult).value();

		if (index.contentEncrypted && !encryptedIndex)
		{
			return makeError(ErrorCode::InvalidFormat, "Encrypted content requires an encrypted index");
		}
		if (index.contentEncrypted && m_config.password.empty())
		{
			return makeError(ErrorCode::PasswordRequired, "The package content requires a password");
		}
		if (index.volumeNames.size() > m_config.maxVolumeCount)
		{
			return makeError(ErrorCode::TooManyVolumes, "Package exceeds the configured volume count limit");
		}

		const std::filesystem::path indexDirectory = indexPath.parent_path().empty()
			? std::filesystem::path(".")
			: indexPath.parent_path();

		std::pmr::vector<Detail::Volume> volumes(m_memoryResource);
		volumes.reserve(index.volumeNames.size());
		for (std::uint32_t volumeIndex = 0; volumeIndex < index.volumeNames.size(); ++volumeIndex)
		{
			const std::pmr::string& volumeName = index.volumeNames[volumeIndex];
			const std::string_view volumeNameView(volumeName.data(), volumeName.size());
			if (!Detail::isValidFileName(volumeNameView))
			{
				return makeError(ErrorCode::InvalidFormat, "Index contains an invalid volume file name");
			}
			const std::filesystem::path volumePath = indexDirectory / Detail::pathFromUtf8(std::string_view(volumeNameView));
			auto volumeFileResult = Detail::NativeFile::openRead(volumePath);
			if (!volumeFileResult)
			{
				return std::move(volumeFileResult).error();
			}
			Detail::NativeFile volumeFile = std::move(volumeFileResult).value();
			auto volumeSizeResult = volumeFile.size();
			if (!volumeSizeResult)
			{
				return std::move(volumeSizeResult).error();
			}
			const std::uint64_t volumeSize = volumeSizeResult.value();
			if (volumeSize < Detail::VolumeHeaderSize)
			{
				return makeError(ErrorCode::InvalidFormat, "Volume file is smaller than its header");
			}

			std::array<Byte, Detail::VolumeHeaderSize> volumeHeaderBytes{};
			auto volumeReadResult = volumeFile.readAt(0, MutableByteSpan(volumeHeaderBytes.data(), volumeHeaderBytes.size()));
			if (!volumeReadResult)
			{
				return std::move(volumeReadResult).error();
			}
			auto volumeHeaderResult = Detail::parseVolumeHeader(ByteSpan(volumeHeaderBytes.data(), volumeHeaderBytes.size()));
			if (!volumeHeaderResult)
			{
				return std::move(volumeHeaderResult).error();
			}
			const Detail::VolumeHeader& volumeHeader = volumeHeaderResult.value();
			if (volumeHeader.versionMajor != Detail::FormatVersionMajor || volumeHeader.versionMinor > Detail::FormatVersionMinor)
			{
				return makeError(ErrorCode::UnsupportedVersion, "Unsupported volume format version");
			}
			if ((volumeHeader.flags & ~Detail::VolumeFlagEncryptedContent) != 0)
			{
				return makeError(ErrorCode::UnsupportedVersion, "Volume uses unknown flags");
			}
			if (volumeHeader.volumeIndex != volumeIndex)
			{
				return makeError(ErrorCode::VolumeMismatch, "Volume header index does not match the index entry");
			}
			if (volumeHeader.packageUuid != header.packageUuid)
			{
				return makeError(ErrorCode::VolumeMismatch, "Volume belongs to a different package");
			}
			const bool volumeEncrypted = (volumeHeader.flags & Detail::VolumeFlagEncryptedContent) != 0;
			if (volumeEncrypted != index.contentEncrypted)
			{
				return makeError(ErrorCode::InvalidFormat, "Volume encryption flag does not match the index");
			}

			Detail::Volume volume;
			volume.file = std::move(volumeFile);
			volume.size = volumeSize;
			volumes.push_back(std::move(volume));
		}

		if (m_config.validateVolumesOnMount)
		{
			for (const Detail::IndexEntry& entry : index.entries)
			{
				if (entry.kind != Detail::EntryKind::File)
				{
					continue;
				}
				if (entry.volumeIndex >= volumes.size())
				{
					return makeError(ErrorCode::CorruptIndex, "Index file entry references a missing volume");
				}
				const Detail::Volume& volume = volumes[entry.volumeIndex];
				if (entry.volumeOffset > volume.size || entry.storedSize > volume.size - entry.volumeOffset)
				{
					return makeError(ErrorCode::CorruptIndex, "Index file range exceeds the volume size");
				}
			}
		}

		auto state = std::allocate_shared<Detail::ReaderState>(
			std::pmr::polymorphic_allocator<Detail::ReaderState>(m_memoryResource),
			m_memoryResource,
			m_config.cacheCapacityBytes);
		state->bufferAlignment = m_config.bufferAlignment;
		state->verifyChecksums = m_config.verifyChecksums;
		state->maxFileBytes = m_config.maxFileBytes;
		state->encryptedContent = index.contentEncrypted;
		state->encryptedIndex = encryptedIndex;
		state->compressedIndex = compressedIndex;
		state->packageUuid = header.packageUuid;
		state->masterKey = masterKeyGuard.key;
		state->index = std::move(index);
		state->volumes = std::move(volumes);

		m_state = std::move(state);
		Detail::secureClear(m_config.password);
		return {};
	}

	void Reader::unmount() noexcept
	{
		m_state.reset();
	}

	bool Reader::isMounted() const noexcept
	{
		return m_state != nullptr;
	}

	Result<SharedBuffer> Reader::read(std::string_view path) const
	{
		if (m_state == nullptr)
		{
			return makeError(ErrorCode::NotMounted, "Reader is not mounted");
		}
		return Detail::readFromState(*m_state, path);
	}

	AsyncTask<SharedBuffer> Reader::readAsync(std::string_view path) const
	{
		std::shared_ptr<Detail::AsyncState<SharedBuffer>> asyncState = Detail::makeAsyncState<SharedBuffer>(m_memoryResource);
		if (m_state == nullptr)
		{
			asyncState->complete(Result<SharedBuffer>(makeError(ErrorCode::NotMounted, "Reader is not mounted")));
			return AsyncTask<SharedBuffer>(std::move(asyncState));
		}
		if (!m_threadPool)
		{
			asyncState->complete(Result<SharedBuffer>(makeError(ErrorCode::ThreadPoolUnavailable, "ReaderConfig::threadPool is empty")));
			return AsyncTask<SharedBuffer>(std::move(asyncState));
		}

		std::shared_ptr<Detail::ReaderState> readerState = m_state;
		ThreadPoolRef threadPool = m_threadPool;
		std::string pathCopy(path);
		threadPool.submit(
			[asyncState, readerState, pathCopy = std::move(pathCopy)]() mutable
			{
				asyncState->complete(Detail::readFromState(*readerState, pathCopy));
			});
		return AsyncTask<SharedBuffer>(std::move(asyncState));
	}

	bool Reader::exists(std::string_view path) const
	{
		if (m_state == nullptr)
		{
			return false;
		}
		auto normalizedResult = Detail::normalizeQueryPath(path);
		if (!normalizedResult)
		{
			return false;
		}
		return m_state->index.find(std::string_view(normalizedResult.value())) != nullptr;
	}

	bool Reader::isDirectory(std::string_view path) const
	{
		if (m_state == nullptr)
		{
			return false;
		}
		auto normalizedResult = Detail::normalizeQueryPath(path);
		if (!normalizedResult)
		{
			return false;
		}
		const Detail::IndexEntry* entry = m_state->index.find(std::string_view(normalizedResult.value()));
		return entry != nullptr && entry->kind == Detail::EntryKind::Directory;
	}

	Result<EntryInfo> Reader::stat(std::string_view path) const
	{
		if (m_state == nullptr)
		{
			return makeError(ErrorCode::NotMounted, "Reader is not mounted");
		}
		auto normalizedResult = Detail::normalizeQueryPath(path);
		if (!normalizedResult)
		{
			return std::move(normalizedResult).error();
		}
		const Detail::IndexEntry* entry = m_state->index.find(std::string_view(normalizedResult.value()));
		if (entry == nullptr)
		{
			return makeError(ErrorCode::NotFound, "Entry was not found");
		}

		EntryInfo info;
		info.path = normalizedResult.value();
		info.size = entry->uncompressedSize;
		info.storedSize = entry->storedSize;
		info.crc32 = entry->crc32;
		info.volumeIndex = entry->volumeIndex;
		info.isDirectory = entry->kind == Detail::EntryKind::Directory;
		info.compressed = entry->compression == Detail::CompressionMethod::Deflate;
		info.encrypted = entry->encryption == Detail::EncryptionMethod::Aes256Gcm;
		return info;
	}

	Result<void> Reader::listDirectory(std::string_view path, const DirectoryVisitor& visitor) const
	{
		if (m_state == nullptr)
		{
			return makeError(ErrorCode::NotMounted, "Reader is not mounted");
		}
		if (!visitor)
		{
			return makeError(ErrorCode::InvalidArgument, "DirectoryVisitor is empty");
		}

		auto normalizedResult = Detail::normalizeQueryPath(path);
		if (!normalizedResult)
		{
			return std::move(normalizedResult).error();
		}
		const Detail::IndexEntry* directory = m_state->index.find(std::string_view(normalizedResult.value()));
		if (directory == nullptr)
		{
			return makeError(ErrorCode::NotFound, "Directory was not found");
		}
		if (directory->kind != Detail::EntryKind::Directory)
		{
			return makeError(ErrorCode::NotAFile, "Path is not a directory");
		}

		const std::size_t first = directory->firstChild;
		const std::size_t count = directory->childCount;
		if (first > m_state->index.childIndices.size() || count > m_state->index.childIndices.size() - first)
		{
			return makeError(ErrorCode::CorruptIndex, "Directory child range is invalid");
		}
		for (std::size_t index = 0; index < count; ++index)
		{
			const std::uint32_t childIndex = m_state->index.childIndices[first + index];
			if (childIndex >= m_state->index.entries.size())
			{
				return makeError(ErrorCode::CorruptIndex, "Directory child index is out of range");
			}
			const Detail::IndexEntry& child = m_state->index.entries[childIndex];
			visitor(m_state->index.nameOf(child), child.kind == Detail::EntryKind::Directory, child.uncompressedSize);
		}
		return {};
	}

	Result<ReaderInfo> Reader::info() const
	{
		if (m_state == nullptr)
		{
			return makeError(ErrorCode::NotMounted, "Reader is not mounted");
		}

		const Detail::ReaderState& state = *m_state;
		ReaderInfo info;
		info.packageName.assign(state.index.packageName.data(), state.index.packageName.size());
		info.formatVersionMajor = Detail::FormatVersionMajor;
		info.formatVersionMinor = Detail::FormatVersionMinor;
		info.entryCount = static_cast<std::uint32_t>(state.index.entries.size());
		info.fileCount = state.index.fileCount();
		info.directoryCount = state.index.directoryCount();
		info.volumeCount = static_cast<std::uint32_t>(state.volumes.size());
		info.encryptedContent = state.encryptedContent;
		info.encryptedIndex = state.encryptedIndex;
		info.compressedIndex = state.compressedIndex;
		for (const Detail::IndexEntry& entry : state.index.entries)
		{
			if (entry.kind == Detail::EntryKind::File)
			{
				info.totalUncompressedBytes += entry.uncompressedSize;
				info.totalStoredBytes += entry.storedSize;
			}
		}
		return info;
	}

	void Reader::setCacheCapacity(std::size_t cacheCapacityBytes) noexcept
	{
		m_config.cacheCapacityBytes = cacheCapacityBytes;
		if (m_state != nullptr)
		{
			m_state->cache.setCapacity(cacheCapacityBytes);
		}
	}

	void Reader::clearCache() noexcept
	{
		if (m_state != nullptr)
		{
			m_state->cache.clear();
		}
	}

	CacheStatistics Reader::cacheStatistics() const noexcept
	{
		CacheStatistics statistics;
		if (m_state != nullptr)
		{
			statistics.hits = m_state->cache.hits();
			statistics.misses = m_state->cache.misses();
			statistics.bytes = m_state->cache.bytes();
			statistics.entries = m_state->cache.entries();
		}
		return statistics;
	}

	namespace Detail
	{
		Result<SharedBuffer> readFromState(ReaderState& state, std::string_view path)
		{
			auto normalizedResult = normalizeQueryPath(path);
			if (!normalizedResult)
			{
				return std::move(normalizedResult).error();
			}
			const std::string normalized = std::move(normalizedResult).value();

			const IndexEntry* entry = state.index.find(std::string_view(normalized));
			if (entry == nullptr)
			{
				return makeError(ErrorCode::NotFound, "Entry was not found");
			}
			if (entry->kind == EntryKind::Directory)
			{
				return makeError(ErrorCode::IsDirectory, "Cannot read a directory");
			}
			if (entry->uncompressedSize > state.maxFileBytes
				|| entry->storedSize > state.maxFileBytes
				|| entry->uncompressedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
				|| entry->storedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
			{
				return makeError(ErrorCode::FileTooLarge, "File exceeds ReaderConfig::maxFileBytes");
			}

			SharedBuffer cached = state.cache.find(std::string_view(normalized));
			if (cached != nullptr)
			{
				return cached;
			}

			const std::string_view indexPath = state.index.pathOf(*entry);
			const EncryptionMethod encryption = entry->encryption;
			const CompressionMethod compression = entry->compression;
			const bool encrypted = encryption == EncryptionMethod::Aes256Gcm;

			SecureKeyGuard fileKeyGuard;
			std::pmr::vector<Byte> associatedData(state.memoryResource);
			if (encrypted)
			{
				associatedData = buildFileAssociatedData(
					state.memoryResource,
					ByteSpan(state.packageUuid.data(), state.packageUuid.size()),
					indexPath,
					entry->uncompressedSize,
					entry->storedSize,
					compression,
					encryption);
				auto keyResult = hkdfSha256(
					ByteSpan(state.masterKey.data(), state.masterKey.size()),
					ByteSpan(entry->nonce.data(), entry->nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()),
					MutableByteSpan(fileKeyGuard.key.data(), fileKeyGuard.key.size()));
				if (!keyResult)
				{
					return std::move(keyResult).error();
				}
			}

			const std::size_t uncompressedSize = static_cast<std::size_t>(entry->uncompressedSize);
			std::shared_ptr<Buffer> writable = std::allocate_shared<Buffer>(
				std::pmr::polymorphic_allocator<Buffer>(state.memoryResource),
				state.memoryResource,
				uncompressedSize,
				state.bufferAlignment);

			auto readRange = [&state, entry](MutableByteSpan destination) -> Result<void>
			{
				if (entry->volumeIndex >= state.volumes.size())
				{
					return makeError(ErrorCode::CorruptIndex, "Index file entry references a missing volume");
				}
				const Volume& volume = state.volumes[entry->volumeIndex];
				if (entry->volumeOffset > volume.size || entry->storedSize > volume.size - entry->volumeOffset)
				{
					return makeError(ErrorCode::CorruptIndex, "Index file range exceeds the volume size");
				}
				if (destination.size() != entry->storedSize)
				{
					return makeError(ErrorCode::InternalFailure, "Read buffer size does not match the index");
				}
				return volume.file.readAt(entry->volumeOffset, destination);
			};

			if (compression == CompressionMethod::Store)
			{
				auto readResult = readRange(writable->bytes());
				if (!readResult)
				{
					return std::move(readResult).error();
				}

				if (encrypted)
				{
					AesGcmDecryptor decryptor;
					auto initializeResult = decryptor.initialize(
						ByteSpan(fileKeyGuard.key.data(), fileKeyGuard.key.size()),
						ByteSpan(entry->nonce.data(), entry->nonce.size()),
						ByteSpan(associatedData.data(), associatedData.size()));
					if (!initializeResult)
					{
						secureZero(writable->bytes());
						return std::move(initializeResult).error();
					}
					for (std::size_t offset = 0; offset < writable->size(); offset += DecryptionChunkSize)
					{
						const std::size_t chunkSize = std::min(DecryptionChunkSize, writable->size() - offset);
						MutableByteSpan chunk = writable->bytes().subspan(offset, chunkSize);
						auto updateResult = decryptor.update(chunk, chunk);
						if (!updateResult)
						{
							secureZero(writable->bytes());
							return std::move(updateResult).error();
						}
					}
					auto finalizeResult = decryptor.finalize(ByteSpan(entry->tag.data(), entry->tag.size()));
					if (!finalizeResult)
					{
						secureZero(writable->bytes());
						return std::move(finalizeResult).error();
					}
				}

				if (state.verifyChecksums && computeCrc32(writable->bytes()) != entry->crc32)
				{
					secureZero(writable->bytes());
					return makeError(ErrorCode::CorruptIndex, "File checksum mismatch");
				}

				SharedBuffer result = writable;
				state.cache.insert(std::string_view(normalized), result);
				return result;
			}

			Buffer stored(state.memoryResource, static_cast<std::size_t>(entry->storedSize), state.bufferAlignment);
			auto readResult = readRange(stored.bytes());
			if (!readResult)
			{
				return std::move(readResult).error();
			}

			if (encrypted)
			{
				AesGcmDecryptor decryptor;
				auto initializeResult = decryptor.initialize(
					ByteSpan(fileKeyGuard.key.data(), fileKeyGuard.key.size()),
					ByteSpan(entry->nonce.data(), entry->nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()));
				if (!initializeResult)
				{
					secureZero(stored.bytes());
					return std::move(initializeResult).error();
				}
				for (std::size_t offset = 0; offset < stored.size(); offset += DecryptionChunkSize)
				{
					const std::size_t chunkSize = std::min(DecryptionChunkSize, stored.size() - offset);
					MutableByteSpan chunk = stored.bytes().subspan(offset, chunkSize);
					auto updateResult = decryptor.update(chunk, chunk);
					if (!updateResult)
					{
						secureZero(stored.bytes());
						return std::move(updateResult).error();
					}
				}
				auto finalizeResult = decryptor.finalize(ByteSpan(entry->tag.data(), entry->tag.size()));
				if (!finalizeResult)
				{
					secureZero(stored.bytes());
					return std::move(finalizeResult).error();
				}
			}

			auto decompressResult = decompressBuffer(stored.bytes(), writable->bytes());
			if (!decompressResult)
			{
				secureZero(stored.bytes());
				secureZero(writable->bytes());
				return std::move(decompressResult).error();
			}
			secureZero(stored.bytes());

			if (state.verifyChecksums && computeCrc32(writable->bytes()) != entry->crc32)
			{
				secureZero(writable->bytes());
				return makeError(ErrorCode::CorruptIndex, "File checksum mismatch");
			}

			SharedBuffer result = writable;
			state.cache.insert(std::string_view(normalized), result);
			return result;
		}
	}
}
