#include "Packer.hpp"

#include "Detail/Compression.hpp"
#include "Detail/Crypto.hpp"
#include "Detail/Format.hpp"
#include "Detail/Index.hpp"
#include "Detail/NativeFile.hpp"
#include "Detail/Utility.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace EnderVFiles2
{
	namespace
	{
		using Detail::compressBuffer;
		using Detail::computeCrc32;
		using Detail::ensureDirectory;
		using Detail::errorMessage;
		using Detail::hkdfSha256;
		using Detail::isPathInsideOrEqual;
		using Detail::isValidPackageName;
		using Detail::isValidPackagePath;
		using Detail::pathFromUtf8;
		using Detail::pbkdf2Sha256;
		using Detail::randomBytes;
		using Detail::randomHex;
		using Detail::secureZero;
		using Detail::sha256;
		using Detail::updateCrc32;
		using Detail::utf8FromPath;

		struct ScanEntry
		{
			explicit ScanEntry(std::pmr::memory_resource* memoryResource)
				: relativePath(memoryResource)
			{
			}

			std::pmr::string relativePath;
			Detail::EntryKind kind = Detail::EntryKind::File;
			std::uint64_t size = 0;
		};

		struct VolumeArtifact
		{
			explicit VolumeArtifact(std::pmr::memory_resource* memoryResource)
				: finalName(memoryResource)
			{
			}

			std::pmr::string finalName;
			std::filesystem::path tempPath;
			std::filesystem::path finalPath;
			std::uint64_t size = 0;
			std::uint32_t index = 0;
		};

		struct CommitItem
		{
			std::filesystem::path finalPath;
			std::filesystem::path tempPath;
			std::filesystem::path backupPath;
			bool hadBackup = false;
			bool committed = false;
		};

		class PackSession
		{
		public:
			explicit PackSession(const PackerConfig& config);
			~PackSession();

			PackSession(const PackSession&) = delete;
			PackSession& operator=(const PackSession&) = delete;

			Result<PackResult> run();

		private:
			Result<void> validateConfig();
			Result<void> scanInput();
			Result<void> prepareCrypto();
			Result<void> processEntries();
			Result<void> processDirectory(const ScanEntry& source);
			Result<void> processFile(const ScanEntry& source);
			Result<void> processInMemoryFile(const ScanEntry& source, Detail::NativeFile& file, std::uint64_t fileSize);
			Result<void> processStreamingFile(const ScanEntry& source, Detail::NativeFile& file, std::uint64_t fileSize);
			Result<void> appendFileEntry(
				const ScanEntry& source,
				std::uint64_t uncompressedSize,
				std::uint64_t storedSize,
				std::uint64_t volumeOffset,
				std::uint32_t volumeIndex,
				Detail::CompressionMethod compression,
				std::uint32_t crc32,
				const Detail::AesGcmNonce& nonce,
				const Detail::AesGcmTag& tag);
			Result<void> ensureVolumeSpace(std::uint64_t storedSize, std::uint64_t& volumeOffset, std::uint32_t& volumeIndex);
			Result<void> startVolume();
			Result<void> finishCurrentVolume();
			Result<void> writeData(ByteSpan data);
			Result<void> writeIndex();
			Result<void> commit();
			std::filesystem::path sourcePath(const ScanEntry& source) const;
			void cleanupTemporaryFiles() noexcept;

			std::pmr::memory_resource* m_memoryResource = nullptr;
			const PackerConfig& m_config;
			std::filesystem::path m_inputDirectory;
			std::filesystem::path m_outputDirectory;
			std::pmr::vector<ScanEntry> m_scan;
			std::pmr::vector<VolumeArtifact> m_volumeArtifacts;
			Detail::Index m_index;
			PackResult m_result;
			std::optional<Detail::FileWriter> m_currentWriter;
			std::filesystem::path m_indexTempPath;
			std::filesystem::path m_indexFinalPath;
			std::array<Byte, Detail::KdfSaltSize> m_salt{};
			Detail::AesGcmNonce m_indexNonce{};
			Detail::AesGcmKey m_masterKey{};
			std::array<Byte, 16> m_packageUuid{};
			std::uint64_t m_currentVolumeSize = 0;
			std::uint32_t m_currentVolumeIndex = Detail::InvalidIndex;
			std::uint64_t m_totalUncompressedBytes = 0;
			std::uint64_t m_totalStoredBytes = 0;
			std::uint32_t m_fileCount = 0;
			std::uint32_t m_directoryCount = 0;
			std::uint32_t m_skippedEntries = 0;
			std::size_t m_bufferAlignment = 64;
			std::size_t m_ioBufferSize = 1024ull * 1024ull;
			bool m_encrypted = false;
			bool m_committed = false;
		};

		PackSession::PackSession(const PackerConfig& config)
			: m_memoryResource(config.memoryResource != nullptr ? config.memoryResource : std::pmr::new_delete_resource())
			, m_config(config)
			, m_scan(m_memoryResource)
			, m_volumeArtifacts(m_memoryResource)
			, m_index(m_memoryResource)
		{
		}

		PackSession::~PackSession()
		{
			secureZero(std::span<Byte>(m_masterKey.data(), m_masterKey.size()));
			m_currentWriter.reset();
			if (!m_committed)
			{
				cleanupTemporaryFiles();
			}
		}

		void PackSession::cleanupTemporaryFiles() noexcept
		{
			std::error_code error;
			for (const VolumeArtifact& artifact : m_volumeArtifacts)
			{
				if (!artifact.tempPath.empty())
				{
					std::filesystem::remove(artifact.tempPath, error);
					error.clear();
				}
			}
			if (!m_indexTempPath.empty())
			{
				std::filesystem::remove(m_indexTempPath, error);
			}
		}

		std::filesystem::path PackSession::sourcePath(const ScanEntry& source) const
		{
			return m_inputDirectory / pathFromUtf8(std::string_view(source.relativePath.data(), source.relativePath.size()));
		}

		Result<void> PackSession::validateConfig()
		{
			if (!isValidPackageName(std::string_view(m_config.packageName.data(), m_config.packageName.size())))
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::packageName is not a valid package name");
			}
			if (m_config.password.size() > 4096)
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::password is too long");
			}
			if (m_config.ioBufferSize < 4096 || m_config.ioBufferSize > 64ull * 1024ull * 1024ull)
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::ioBufferSize must be between 4 KiB and 64 MiB");
			}
			if (m_config.bufferAlignment == 0 || !std::has_single_bit(m_config.bufferAlignment) || m_config.bufferAlignment > 2ull * 1024ull * 1024ull)
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::bufferAlignment must be a power of two up to 2 MiB");
			}
			if (m_config.compressionThresholdBytes > 1024ull * 1024ull * 1024ull)
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::compressionThresholdBytes must not exceed 1 GiB");
			}
			switch (m_config.compressionLevel)
			{
			case CompressionLevel::Store:
			case CompressionLevel::Fastest:
			case CompressionLevel::Default:
			case CompressionLevel::Best:
				break;
			default:
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::compressionLevel is invalid");
			}

			m_encrypted = !m_config.password.empty();
			if (m_encrypted && (m_config.kdfIterations < 1000 || m_config.kdfIterations > 2000000))
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::kdfIterations must be between 1000 and 2000000");
			}

			std::error_code error;
			if (m_config.inputDirectory.empty())
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::inputDirectory is empty");
			}
			const std::filesystem::path input = std::filesystem::weakly_canonical(m_config.inputDirectory, error);
			if (error || input.empty())
			{
				error.clear();
				m_inputDirectory = std::filesystem::absolute(m_config.inputDirectory, error);
				if (error)
				{
					return makeError(ErrorCode::IoFailure, "Failed to resolve input directory");
				}
			}
			else
			{
				m_inputDirectory = input;
			}
			m_inputDirectory = m_inputDirectory.lexically_normal();

			if (!std::filesystem::exists(m_inputDirectory, error) || error || !std::filesystem::is_directory(m_inputDirectory, error) || error)
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::inputDirectory must be an existing directory");
			}

			if (m_config.outputDirectory.empty())
			{
				return makeError(ErrorCode::InvalidArgument, "PackerConfig::outputDirectory is empty");
			}

			error.clear();
			const std::filesystem::path output = std::filesystem::weakly_canonical(m_config.outputDirectory, error);
			if (error || output.empty())
			{
				error.clear();
				m_outputDirectory = std::filesystem::absolute(m_config.outputDirectory, error);
				if (error)
				{
					return makeError(ErrorCode::IoFailure, "Failed to resolve output directory");
				}
			}
			else
			{
				m_outputDirectory = output;
			}
			m_outputDirectory = m_outputDirectory.lexically_normal();

			if (isPathInsideOrEqual(m_outputDirectory, m_inputDirectory))
			{
				return makeError(ErrorCode::InvalidArgument, "Output directory must not be inside the input directory");
			}

			auto outputResult = ensureDirectory(m_outputDirectory);
			if (!outputResult)
			{
				return std::move(outputResult).error();
			}

			m_bufferAlignment = m_config.bufferAlignment;
			m_ioBufferSize = m_config.ioBufferSize;
			m_directoryCount = 1;
			return {};
		}

		Result<void> PackSession::scanInput()
		{
			std::error_code error;
			std::filesystem::recursive_directory_iterator iterator(m_inputDirectory, std::filesystem::directory_options::none, error);
			if (error)
			{
				return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to open input directory"));
			}

			const std::filesystem::recursive_directory_iterator end;
			while (iterator != end)
			{
				if (error)
				{
					return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to scan input directory"));
				}

				const std::filesystem::directory_entry& directoryEntry = *iterator;
				const std::filesystem::file_status status = directoryEntry.symlink_status(error);
				if (error)
				{
					return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to query a directory entry"));
				}
				if (std::filesystem::is_symlink(status))
				{
					++m_skippedEntries;
					iterator.increment(error);
					continue;
				}

				const bool isDirectory = std::filesystem::is_directory(status);
				const bool isRegularFile = std::filesystem::is_regular_file(status);
				if (!isDirectory && !isRegularFile)
				{
					++m_skippedEntries;
					iterator.increment(error);
					continue;
				}

				const std::filesystem::path relative = directoryEntry.path().lexically_relative(m_inputDirectory);
				const std::string relativeUtf8 = utf8FromPath(relative);
				if (!isValidPackagePath(std::string_view(relativeUtf8.data(), relativeUtf8.size()), false))
				{
					return makeError(ErrorCode::UnsafePath, "Input directory contains an unsupported path");
				}
				if (m_scan.size() + 1 > Detail::MaxEntries)
				{
					return makeError(ErrorCode::TooManyEntries, "Input directory contains too many entries");
				}

				std::uint64_t size = 0;
				if (isRegularFile)
				{
					size = directoryEntry.file_size(error);
					if (error)
					{
						return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to query a file size"));
					}
				}

				m_scan.emplace_back(m_memoryResource);
				ScanEntry& entry = m_scan.back();
				if (!relativeUtf8.empty())
				{
					entry.relativePath.assign(relativeUtf8.data(), relativeUtf8.size());
				}
				entry.kind = isDirectory ? Detail::EntryKind::Directory : Detail::EntryKind::File;
				entry.size = size;

				iterator.increment(error);
			}
			if (error)
			{
				return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to scan input directory"));
			}

			std::sort(m_scan.begin(), m_scan.end(), [](const ScanEntry& left, const ScanEntry& right)
			{
				return left.relativePath < right.relativePath;
			});

			for (std::size_t index = 1; index < m_scan.size(); ++index)
			{
				if (m_scan[index - 1].relativePath == m_scan[index].relativePath)
				{
					return makeError(ErrorCode::DuplicateEntry, "Input directory contains duplicate paths");
				}
			}
			return {};
		}

		Result<void> PackSession::prepareCrypto()
		{
			auto saltResult = randomBytes(MutableByteSpan(m_salt.data(), m_salt.size()));
			if (!saltResult)
			{
				return std::move(saltResult).error();
			}
			auto nonceResult = randomBytes(MutableByteSpan(m_indexNonce.data(), m_indexNonce.size()));
			if (!nonceResult)
			{
				return std::move(nonceResult).error();
			}
			auto uuidResult = randomBytes(MutableByteSpan(m_packageUuid.data(), m_packageUuid.size()));
			if (!uuidResult)
			{
				return std::move(uuidResult).error();
			}

			if (m_encrypted)
			{
				auto deriveResult = pbkdf2Sha256(
					std::string_view(m_config.password.data(), m_config.password.size()),
					ByteSpan(m_salt.data(), m_salt.size()),
					m_config.kdfIterations,
					MutableByteSpan(m_masterKey.data(), m_masterKey.size()));
				if (!deriveResult)
				{
					return std::move(deriveResult).error();
				}
			}

			m_index.packageName.assign(m_config.packageName.data(), m_config.packageName.size());
			m_index.contentEncrypted = m_encrypted;
			return {};
		}

		Result<void> PackSession::processEntries()
		{
			Detail::IndexEntry root;
			root.kind = Detail::EntryKind::Directory;
			auto rootResult = m_index.appendEntry({}, root);
			if (!rootResult)
			{
				return std::move(rootResult).error();
			}

			for (const ScanEntry& source : m_scan)
			{
				if (source.kind == Detail::EntryKind::Directory)
				{
					auto directoryResult = processDirectory(source);
					if (!directoryResult)
					{
						return std::move(directoryResult).error();
					}
				}
				else
				{
					auto fileResult = processFile(source);
					if (!fileResult)
					{
						return std::move(fileResult).error();
					}
				}
			}
			return {};
		}

		Result<void> PackSession::processDirectory(const ScanEntry& source)
		{
			Detail::IndexEntry entry;
			entry.kind = Detail::EntryKind::Directory;
			auto appendResult = m_index.appendEntry(std::string_view(source.relativePath.data(), source.relativePath.size()), entry);
			if (!appendResult)
			{
				return std::move(appendResult).error();
			}
			++m_directoryCount;
			return {};
		}

		Result<void> PackSession::processFile(const ScanEntry& source)
		{
			auto fileResult = Detail::NativeFile::openRead(sourcePath(source));
			if (!fileResult)
			{
				return std::move(fileResult).error();
			}
			Detail::NativeFile file = std::move(fileResult).value();

			auto sizeResult = file.size();
			if (!sizeResult)
			{
				return std::move(sizeResult).error();
			}
			const std::uint64_t fileSize = sizeResult.value();
			if (fileSize != source.size)
			{
				return makeError(ErrorCode::IoFailure, "Input file changed size while scanning");
			}
			if (fileSize > Detail::MaxStoredFileSize)
			{
				return makeError(ErrorCode::FileTooLarge, "Input file is too large");
			}

			const bool canCompress = fileSize > 0
				&& fileSize <= m_config.compressionThresholdBytes
				&& m_config.compressionLevel != CompressionLevel::Store
				&& fileSize <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());

			if (canCompress)
			{
				return processInMemoryFile(source, file, fileSize);
			}
			return processStreamingFile(source, file, fileSize);
		}

		Result<void> PackSession::processInMemoryFile(const ScanEntry& source, Detail::NativeFile& file, std::uint64_t fileSize)
		{
			Buffer raw(m_memoryResource, static_cast<std::size_t>(fileSize), m_bufferAlignment);
			auto readResult = file.readAt(0, raw.bytes());
			if (!readResult)
			{
				return std::move(readResult).error();
			}
			auto finalSizeResult = file.size();
			if (!finalSizeResult)
			{
				return std::move(finalSizeResult).error();
			}
			if (finalSizeResult.value() != fileSize)
			{
				return makeError(ErrorCode::IoFailure, "Input file changed size while reading");
			}

			const std::uint32_t crc = computeCrc32(raw.bytes());
			Detail::CompressionMethod method = Detail::CompressionMethod::Store;
			Buffer compressed(m_memoryResource, 0, m_bufferAlignment);
			bool useCompressed = false;

			auto compressedResult = compressBuffer(
				raw.bytes(),
				m_memoryResource,
				static_cast<int>(m_config.compressionLevel),
				m_bufferAlignment);
			if (compressedResult)
			{
				compressed = std::move(compressedResult).value();
				if (compressed.size() < raw.size())
				{
					method = Detail::CompressionMethod::Deflate;
					useCompressed = true;
				}
			}

			Buffer& payload = useCompressed ? compressed : raw;
			const std::uint64_t storedSize = payload.size();
			std::uint64_t volumeOffset = 0;
			std::uint32_t volumeIndex = Detail::InvalidIndex;
			auto spaceResult = ensureVolumeSpace(storedSize, volumeOffset, volumeIndex);
			if (!spaceResult)
			{
				return std::move(spaceResult).error();
			}

			Detail::AesGcmNonce nonce{};
			Detail::AesGcmTag tag{};
			const Detail::EncryptionMethod encryption = m_encrypted ? Detail::EncryptionMethod::Aes256Gcm : Detail::EncryptionMethod::None;
			const std::string_view relativePath(source.relativePath.data(), source.relativePath.size());

			if (m_encrypted)
			{
				auto nonceResult = randomBytes(MutableByteSpan(nonce.data(), nonce.size()));
				if (!nonceResult)
				{
					return std::move(nonceResult).error();
				}

				auto associatedData = Detail::buildFileAssociatedData(
					m_memoryResource,
					ByteSpan(m_packageUuid.data(), m_packageUuid.size()),
					relativePath,
					fileSize,
					storedSize,
					method,
					encryption);
				Detail::AesGcmKey fileKey{};
				auto keyResult = hkdfSha256(
					ByteSpan(m_masterKey.data(), m_masterKey.size()),
					ByteSpan(nonce.data(), nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()),
					MutableByteSpan(fileKey.data(), fileKey.size()));
				if (!keyResult)
				{
					return std::move(keyResult).error();
				}

				Detail::AesGcmEncryptor encryptor;
				auto initializeResult = encryptor.initialize(
					ByteSpan(fileKey.data(), fileKey.size()),
					ByteSpan(nonce.data(), nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()));
				if (!initializeResult)
				{
					secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
					return std::move(initializeResult).error();
				}

				auto updateResult = encryptor.update(payload.bytes(), payload.bytes());
				if (!updateResult)
				{
					secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
					return std::move(updateResult).error();
				}
				auto finalizeResult = encryptor.finalize(tag);
				secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
				if (!finalizeResult)
				{
					return std::move(finalizeResult).error();
				}
			}

			auto writeResult = writeData(payload.bytes());
			if (!writeResult)
			{
				return std::move(writeResult).error();
			}

			auto appendResult = appendFileEntry(source, fileSize, storedSize, volumeOffset, volumeIndex, method, crc, nonce, tag);
			if (!appendResult)
			{
				return std::move(appendResult).error();
			}
			m_totalUncompressedBytes += fileSize;
			m_totalStoredBytes += storedSize;
			return {};
		}

		Result<void> PackSession::processStreamingFile(const ScanEntry& source, Detail::NativeFile& file, std::uint64_t fileSize)
		{
			const std::uint64_t storedSize = fileSize;
			std::uint64_t volumeOffset = 0;
			std::uint32_t volumeIndex = Detail::InvalidIndex;
			auto spaceResult = ensureVolumeSpace(storedSize, volumeOffset, volumeIndex);
			if (!spaceResult)
			{
				return std::move(spaceResult).error();
			}

			const Detail::CompressionMethod method = Detail::CompressionMethod::Store;
			const Detail::EncryptionMethod encryption = m_encrypted ? Detail::EncryptionMethod::Aes256Gcm : Detail::EncryptionMethod::None;
			const std::string_view relativePath(source.relativePath.data(), source.relativePath.size());
			Detail::AesGcmNonce nonce{};
			Detail::AesGcmTag tag{};
			Detail::AesGcmKey fileKey{};
			Detail::AesGcmEncryptor encryptor;
			std::optional<Buffer> chunk;

			if (m_encrypted)
			{
				auto nonceResult = randomBytes(MutableByteSpan(nonce.data(), nonce.size()));
				if (!nonceResult)
				{
					return std::move(nonceResult).error();
				}

				auto associatedData = Detail::buildFileAssociatedData(
					m_memoryResource,
					ByteSpan(m_packageUuid.data(), m_packageUuid.size()),
					relativePath,
					fileSize,
					storedSize,
					method,
					encryption);
				auto keyResult = hkdfSha256(
					ByteSpan(m_masterKey.data(), m_masterKey.size()),
					ByteSpan(nonce.data(), nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()),
					MutableByteSpan(fileKey.data(), fileKey.size()));
				if (!keyResult)
				{
					return std::move(keyResult).error();
				}

				auto initializeResult = encryptor.initialize(
					ByteSpan(fileKey.data(), fileKey.size()),
					ByteSpan(nonce.data(), nonce.size()),
					ByteSpan(associatedData.data(), associatedData.size()));
				if (!initializeResult)
				{
					secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
					return std::move(initializeResult).error();
				}
			}

			const std::size_t chunkSize = fileSize > 0
				? static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, m_ioBufferSize))
				: 0;
			if (chunkSize > 0)
			{
				chunk.emplace(m_memoryResource, chunkSize, m_bufferAlignment);
			}

			std::uint32_t crc = 0;
			std::uint64_t offset = 0;
			while (offset < fileSize)
			{
				const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(fileSize - offset, chunkSize));
				MutableByteSpan chunkBytes(chunk->data(), requested);
				auto readResult = file.readAt(offset, chunkBytes);
				if (!readResult)
				{
					secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
					return std::move(readResult).error();
				}

				crc = updateCrc32(crc, ByteSpan(chunkBytes.data(), chunkBytes.size()));
				if (m_encrypted)
				{
					auto updateResult = encryptor.update(ByteSpan(chunkBytes.data(), chunkBytes.size()), chunkBytes);
					if (!updateResult)
					{
						secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
						return std::move(updateResult).error();
					}
				}

				auto writeResult = writeData(ByteSpan(chunkBytes.data(), chunkBytes.size()));
				if (!writeResult)
				{
					secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
					return std::move(writeResult).error();
				}
				offset += requested;
			}

			if (m_encrypted)
			{
				auto finalizeResult = encryptor.finalize(tag);
				secureZero(std::span<Byte>(fileKey.data(), fileKey.size()));
				if (!finalizeResult)
				{
					return std::move(finalizeResult).error();
				}
			}

			auto finalSizeResult = file.size();
			if (!finalSizeResult)
			{
				return std::move(finalSizeResult).error();
			}
			if (finalSizeResult.value() != fileSize)
			{
				return makeError(ErrorCode::IoFailure, "Input file changed size while reading");
			}

			auto appendResult = appendFileEntry(source, fileSize, storedSize, volumeOffset, volumeIndex, method, crc, nonce, tag);
			if (!appendResult)
			{
				return std::move(appendResult).error();
			}
			m_totalUncompressedBytes += fileSize;
			m_totalStoredBytes += storedSize;
			return {};
		}

		Result<void> PackSession::appendFileEntry(
			const ScanEntry& source,
			std::uint64_t uncompressedSize,
			std::uint64_t storedSize,
			std::uint64_t volumeOffset,
			std::uint32_t volumeIndex,
			Detail::CompressionMethod compression,
			std::uint32_t crc32,
			const Detail::AesGcmNonce& nonce,
			const Detail::AesGcmTag& tag)
		{
			Detail::IndexEntry entry;
			entry.kind = Detail::EntryKind::File;
			entry.compression = compression;
			entry.encryption = m_encrypted ? Detail::EncryptionMethod::Aes256Gcm : Detail::EncryptionMethod::None;
			entry.uncompressedSize = uncompressedSize;
			entry.storedSize = storedSize;
			entry.volumeOffset = volumeOffset;
			entry.volumeIndex = volumeIndex;
			entry.crc32 = crc32;
			entry.nonce = nonce;
			entry.tag = tag;

			auto appendResult = m_index.appendEntry(std::string_view(source.relativePath.data(), source.relativePath.size()), entry);
			if (!appendResult)
			{
				return std::move(appendResult).error();
			}
			++m_fileCount;
			return {};
		}

		Result<void> PackSession::ensureVolumeSpace(std::uint64_t storedSize, std::uint64_t& volumeOffset, std::uint32_t& volumeIndex)
		{
			if (!m_currentWriter.has_value())
			{
				auto startResult = startVolume();
				if (!startResult)
				{
					return std::move(startResult).error();
				}
			}

			if (m_config.volumeSize != 0 && m_currentVolumeSize > Detail::VolumeHeaderSize)
			{
				const bool needsNewVolume = m_currentVolumeSize >= m_config.volumeSize
					|| storedSize > m_config.volumeSize - m_currentVolumeSize;
				if (needsNewVolume)
				{
					auto finishResult = finishCurrentVolume();
					if (!finishResult)
					{
						return std::move(finishResult).error();
					}
					auto startResult = startVolume();
					if (!startResult)
					{
						return std::move(startResult).error();
					}
				}
			}

			if (m_currentVolumeIndex == Detail::InvalidIndex)
			{
				return makeError(ErrorCode::InternalFailure, "Volume was not initialized");
			}
			if (m_currentVolumeSize > std::numeric_limits<std::uint64_t>::max() - storedSize)
			{
				return makeError(ErrorCode::FileTooLarge, "Volume size would overflow");
			}

			volumeOffset = m_currentVolumeSize;
			volumeIndex = m_currentVolumeIndex;
			return {};
		}

		Result<void> PackSession::startVolume()
		{
			if (m_volumeArtifacts.size() >= Detail::MaxVolumes)
			{
				return makeError(ErrorCode::TooManyVolumes, "Package exceeded the maximum number of volumes");
			}

			const std::uint32_t volumeIndex = static_cast<std::uint32_t>(m_volumeArtifacts.size());
			const std::string finalName = Detail::makeVolumeFileName(std::string_view(m_config.packageName.data(), m_config.packageName.size()), volumeIndex);
			auto randomResult = randomHex(8);
			if (!randomResult)
			{
				return std::move(randomResult).error();
			}
			const std::string tempName = finalName + "." + randomResult.value() + ".tmp";
			const std::filesystem::path tempPath = m_outputDirectory / pathFromUtf8(std::string_view(tempName));
			const std::filesystem::path finalPath = m_outputDirectory / pathFromUtf8(std::string_view(finalName));

			auto fileResult = Detail::NativeFile::openWrite(tempPath);
			if (!fileResult)
			{
				return std::move(fileResult).error();
			}

			m_volumeArtifacts.emplace_back(m_memoryResource);
			VolumeArtifact& artifact = m_volumeArtifacts.back();
			artifact.finalName.assign(finalName.data(), finalName.size());
			artifact.tempPath = tempPath;
			artifact.finalPath = finalPath;
			artifact.index = volumeIndex;

			Detail::FileWriter writer(std::move(fileResult).value(), m_memoryResource, m_ioBufferSize);

			Detail::VolumeHeader header;
			header.volumeIndex = volumeIndex;
			header.packageUuid = m_packageUuid;
			header.flags = m_encrypted ? Detail::VolumeFlagEncryptedContent : 0;
			auto headerResult = Detail::serializeVolumeHeader(header);
			if (!headerResult)
			{
				return std::move(headerResult).error();
			}
			auto writeResult = writer.write(ByteSpan(headerResult.value().data(), headerResult.value().size()));
			if (!writeResult)
			{
				return std::move(writeResult).error();
			}

			m_index.volumeNames.emplace_back(
				finalName.data(),
				finalName.size());
			m_currentWriter.emplace(std::move(writer));
			m_currentVolumeSize = Detail::VolumeHeaderSize;
			m_currentVolumeIndex = volumeIndex;
			return {};
		}

		Result<void> PackSession::finishCurrentVolume()
		{
			if (!m_currentWriter.has_value())
			{
				return {};
			}

			auto flushResult = m_currentWriter->flush(true);
			if (!flushResult)
			{
				return std::move(flushResult).error();
			}
			m_volumeArtifacts[m_currentVolumeIndex].size = m_currentWriter->position();
			m_currentWriter.reset();
			m_currentVolumeSize = 0;
			m_currentVolumeIndex = Detail::InvalidIndex;
			return {};
		}

		Result<void> PackSession::writeData(ByteSpan data)
		{
			if (!m_currentWriter.has_value())
			{
				return makeError(ErrorCode::InternalFailure, "No active volume writer");
			}
			if (data.empty())
			{
				return {};
			}
			if (m_currentVolumeSize > std::numeric_limits<std::uint64_t>::max() - data.size())
			{
				return makeError(ErrorCode::FileTooLarge, "Volume size would overflow");
			}

			auto writeResult = m_currentWriter->write(data);
			if (!writeResult)
			{
				return std::move(writeResult).error();
			}
			m_currentVolumeSize += data.size();
			return {};
		}

		Result<void> PackSession::writeIndex()
		{
			auto payloadResult = Detail::serializeIndexPayload(m_index);
			if (!payloadResult)
			{
				return std::move(payloadResult).error();
			}
			std::pmr::vector<Byte> payload = std::move(payloadResult).value();
			if (payload.size() > Detail::MaxIndexPayloadSize)
			{
				return makeError(ErrorCode::IndexTooLarge, "Index payload exceeds the supported size");
			}

			Detail::Sha256Digest digest{};
			auto digestResult = sha256(ByteSpan(payload.data(), payload.size()), digest);
			if (!digestResult)
			{
				return std::move(digestResult).error();
			}

			bool compressed = false;
			std::optional<Buffer> compressedPayload;
			if (m_config.compressionLevel != CompressionLevel::Store && !payload.empty())
			{
				auto compressResult = compressBuffer(
					ByteSpan(payload.data(), payload.size()),
					m_memoryResource,
					static_cast<int>(m_config.compressionLevel),
					m_bufferAlignment);
				if (compressResult && compressResult.value().size() < payload.size())
				{
					compressedPayload.emplace(std::move(compressResult).value());
					compressed = true;
				}
			}

			ByteSpan storedPayload = compressed
				? ByteSpan(compressedPayload->data(), compressedPayload->size())
				: ByteSpan(payload.data(), payload.size());

			Detail::IndexHeader header;
			header.flags = (m_encrypted ? Detail::IndexFlagEncrypted : 0u)
				| (compressed ? Detail::IndexFlagCompressed : 0u);
			header.kdfIterations = m_encrypted ? m_config.kdfIterations : 0;
			header.cipherSuite = m_encrypted ? Detail::CipherSuite::Aes256Gcm : Detail::CipherSuite::None;
			header.salt = m_salt;
			header.nonce = m_indexNonce;
			header.packageUuid = m_packageUuid;
			header.payloadUncompressedSize = payload.size();
			header.payloadStoredSize = storedPayload.size();
			header.payloadDigest = digest;

			if (m_encrypted)
			{
				Detail::AesGcmKey indexKey{};
				auto keyResult = hkdfSha256(
					ByteSpan(m_masterKey.data(), m_masterKey.size()),
					ByteSpan(m_indexNonce.data(), m_indexNonce.size()),
					ByteSpan(reinterpret_cast<const Byte*>(Detail::IndexKeyInfo.data()), Detail::IndexKeyInfo.size()),
					MutableByteSpan(indexKey.data(), indexKey.size()));
				if (!keyResult)
				{
					return std::move(keyResult).error();
				}

				auto preliminaryHeader = Detail::serializeIndexHeader(header);
				if (!preliminaryHeader)
				{
					secureZero(std::span<Byte>(indexKey.data(), indexKey.size()));
					return std::move(preliminaryHeader).error();
				}

				Detail::AesGcmEncryptor encryptor;
				auto initializeResult = encryptor.initialize(
					ByteSpan(indexKey.data(), indexKey.size()),
					ByteSpan(m_indexNonce.data(), m_indexNonce.size()),
					ByteSpan(preliminaryHeader.value().data(), Detail::IndexHeaderAuthenticatedSize));
				if (!initializeResult)
				{
					secureZero(std::span<Byte>(indexKey.data(), indexKey.size()));
					return std::move(initializeResult).error();
				}

				MutableByteSpan mutableStored = compressed
					? compressedPayload->bytes()
					: MutableByteSpan(payload.data(), payload.size());
				auto updateResult = encryptor.update(storedPayload, mutableStored);
				if (!updateResult)
				{
					secureZero(std::span<Byte>(indexKey.data(), indexKey.size()));
					return std::move(updateResult).error();
				}
				Detail::AesGcmTag tag{};
				auto finalizeResult = encryptor.finalize(tag);
				secureZero(std::span<Byte>(indexKey.data(), indexKey.size()));
				if (!finalizeResult)
				{
					return std::move(finalizeResult).error();
				}
				header.authTag = tag;
				storedPayload = ByteSpan(mutableStored.data(), mutableStored.size());
			}

			auto finalHeader = Detail::serializeIndexHeader(header);
			if (!finalHeader)
			{
				return std::move(finalHeader).error();
			}

			const std::string finalName = Detail::makeIndexFileName(std::string_view(m_config.packageName.data(), m_config.packageName.size()));
			auto randomResult = randomHex(8);
			if (!randomResult)
			{
				return std::move(randomResult).error();
			}
			const std::string tempName = finalName + "." + randomResult.value() + ".tmp";
			const std::filesystem::path finalPath = m_outputDirectory / pathFromUtf8(std::string_view(finalName));
			const std::filesystem::path tempPath = m_outputDirectory / pathFromUtf8(std::string_view(tempName));

			auto fileResult = Detail::NativeFile::openWrite(tempPath);
			if (!fileResult)
			{
				return std::move(fileResult).error();
			}
			m_indexFinalPath = finalPath;
			m_indexTempPath = tempPath;
			Detail::FileWriter writer(std::move(fileResult).value(), m_memoryResource, m_ioBufferSize);
			auto headerWriteResult = writer.write(ByteSpan(finalHeader.value().data(), finalHeader.value().size()));
			if (!headerWriteResult)
			{
				return std::move(headerWriteResult).error();
			}
			auto payloadWriteResult = writer.write(storedPayload);
			if (!payloadWriteResult)
			{
				return std::move(payloadWriteResult).error();
			}
			auto flushResult = writer.flush(true);
			if (!flushResult)
			{
				return std::move(flushResult).error();
			}

			m_result.indexPath = m_indexFinalPath;
			return {};
		}

		Result<void> PackSession::commit()
		{
			std::vector<CommitItem> items;
			items.reserve(m_volumeArtifacts.size() + 1);
			for (const VolumeArtifact& artifact : m_volumeArtifacts)
			{
				CommitItem item;
				item.finalPath = artifact.finalPath;
				item.tempPath = artifact.tempPath;
				items.push_back(std::move(item));
			}
			CommitItem indexItem;
			indexItem.finalPath = m_indexFinalPath;
			indexItem.tempPath = m_indexTempPath;
			items.push_back(std::move(indexItem));

			std::error_code error;
			for (const CommitItem& item : items)
			{
				const bool exists = std::filesystem::exists(item.finalPath, error);
				if (error)
				{
					return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to inspect an output file"));
				}
				if (exists && !m_config.overwriteExisting)
				{
					return makeError(ErrorCode::IoFailure, "Output package file already exists; enable overwriteExisting to replace it");
				}
			}

			for (CommitItem& item : items)
			{
				const bool exists = std::filesystem::exists(item.finalPath, error);
				if (error)
				{
					for (CommitItem& rollback : items)
					{
						if (rollback.hadBackup)
						{
							std::filesystem::rename(rollback.backupPath, rollback.finalPath, error);
							error.clear();
						}
					}
					return makeError(ErrorCode::IoFailure, errorMessage(error, "Failed to inspect an output file"));
				}
				if (!exists)
				{
					continue;
				}

				auto randomResult = randomHex(8);
				if (!randomResult)
				{
					for (CommitItem& rollback : items)
					{
						if (rollback.hadBackup)
						{
							std::filesystem::rename(rollback.backupPath, rollback.finalPath, error);
							error.clear();
						}
					}
					return std::move(randomResult).error();
				}
				item.backupPath = item.finalPath;
				item.backupPath += pathFromUtf8(std::string_view("." + randomResult.value() + ".bak"));
				std::filesystem::rename(item.finalPath, item.backupPath, error);
				if (error)
				{
					for (CommitItem& rollback : items)
					{
						if (rollback.hadBackup)
						{
							std::filesystem::rename(rollback.backupPath, rollback.finalPath, error);
							error.clear();
						}
					}
					return makeError(ErrorCode::IoFailure, "Failed to back up an existing package file");
				}
				item.hadBackup = true;
			}

			for (CommitItem& item : items)
			{
				std::filesystem::rename(item.tempPath, item.finalPath, error);
				if (error)
				{
					for (CommitItem& rollback : items)
					{
						if (rollback.committed)
						{
							std::filesystem::remove(rollback.finalPath, error);
							error.clear();
						}
					}
					for (CommitItem& rollback : items)
					{
						if (rollback.hadBackup)
						{
							std::filesystem::rename(rollback.backupPath, rollback.finalPath, error);
							error.clear();
						}
					}
					return makeError(ErrorCode::IoFailure, "Failed to commit the package");
				}
				item.committed = true;
			}

			for (CommitItem& item : items)
			{
				if (item.hadBackup)
				{
					std::filesystem::remove(item.backupPath, error);
					error.clear();
				}
			}

			m_committed = true;
			m_result.totalUncompressedBytes = m_totalUncompressedBytes;
			m_result.totalStoredBytes = m_totalStoredBytes;
			m_result.fileCount = m_fileCount;
			m_result.directoryCount = m_directoryCount;
			m_result.volumeCount = static_cast<std::uint32_t>(m_volumeArtifacts.size());
			m_result.skippedEntries = m_skippedEntries;
			return {};
		}

		Result<PackResult> PackSession::run()
		{
			auto validateResult = validateConfig();
			if (!validateResult)
			{
				return std::move(validateResult).error();
			}
			auto scanResult = scanInput();
			if (!scanResult)
			{
				return std::move(scanResult).error();
			}
			auto cryptoResult = prepareCrypto();
			if (!cryptoResult)
			{
				return std::move(cryptoResult).error();
			}
			auto processResult = processEntries();
			if (!processResult)
			{
				return std::move(processResult).error();
			}
			auto finishResult = finishCurrentVolume();
			if (!finishResult)
			{
				return std::move(finishResult).error();
			}
			auto finalizeResult = m_index.finalize();
			if (!finalizeResult)
			{
				return std::move(finalizeResult).error();
			}
			auto indexResult = writeIndex();
			if (!indexResult)
			{
				return std::move(indexResult).error();
			}
			auto commitResult = commit();
			if (!commitResult)
			{
				return std::move(commitResult).error();
			}
			return std::move(m_result);
		}
	}

	Packer::Packer(PackerConfig config)
		: m_config(std::move(config))
	{
	}

	Packer::Packer(Packer&& other) noexcept
		: m_config(std::move(other.m_config))
	{
	}

	Packer& Packer::operator=(Packer&& other) noexcept
	{
		if (this != &other)
		{
			Detail::secureClear(m_config.password);
			m_config = std::move(other.m_config);
		}
		return *this;
	}

	Packer::~Packer()
	{
		Detail::secureClear(m_config.password);
	}

	Result<PackResult> Packer::pack()
	{
		PackSession session(m_config);
		return session.run();
	}
}
