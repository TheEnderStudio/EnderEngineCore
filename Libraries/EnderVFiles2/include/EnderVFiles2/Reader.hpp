#pragma once

#include "AsyncTask.hpp"
#include "Buffer.hpp"
#include "Error.hpp"
#include "ResourceLoader.hpp"
#include "ThreadPool.hpp"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

namespace EnderVFiles2
{
	struct ReaderConfig
	{
		std::string password;
		std::pmr::memory_resource* memoryResource = nullptr;
		ThreadPoolRef threadPool;
		std::size_t cacheCapacityBytes = 64ull * 1024ull * 1024ull;
		std::size_t bufferAlignment = 64;
		std::uint32_t maxKdfIterations = 2000000;
		std::uint64_t maxIndexFileBytes = 1024ull * 1024ull * 1024ull;
		std::uint64_t maxIndexPayloadBytes = 512ull * 1024ull * 1024ull;
		std::uint64_t maxFileBytes = 8ull * 1024ull * 1024ull * 1024ull;
		std::uint32_t maxEntries = 4000000;
		std::uint32_t maxVolumeCount = 8192;
		bool verifyChecksums = true;
		bool validateVolumesOnMount = true;
	};

	struct ReaderInfo
	{
		std::string packageName;
		std::uint32_t formatVersionMajor = 0;
		std::uint32_t formatVersionMinor = 0;
		std::uint32_t entryCount = 0;
		std::uint32_t fileCount = 0;
		std::uint32_t directoryCount = 0;
		std::uint32_t volumeCount = 0;
		std::uint64_t totalUncompressedBytes = 0;
		std::uint64_t totalStoredBytes = 0;
		bool encryptedContent = false;
		bool encryptedIndex = false;
		bool compressedIndex = false;
	};

	struct EntryInfo
	{
		std::string path;
		std::uint64_t size = 0;
		std::uint64_t storedSize = 0;
		std::uint32_t crc32 = 0;
		std::uint32_t volumeIndex = 0;
		bool isDirectory = false;
		bool compressed = false;
		bool encrypted = false;
	};

	struct CacheStatistics
	{
		std::uint64_t hits = 0;
		std::uint64_t misses = 0;
		std::uint64_t bytes = 0;
		std::uint64_t entries = 0;
	};

	using DirectoryVisitor = std::function<void(std::string_view name, bool isDirectory, std::uint64_t size)>;

	namespace Detail
	{
		struct ReaderState;

		Result<SharedBuffer> readFromState(ReaderState& state, std::string_view path);
	}

	class Reader
	{
	public:
		explicit Reader(ReaderConfig config);
		Reader(const Reader&) = delete;
		Reader& operator=(const Reader&) = delete;
		Reader(Reader&&) noexcept;
		Reader& operator=(Reader&&) noexcept;
		~Reader();

		Result<void> mount(const std::filesystem::path& indexPath);
		void unmount() noexcept;
		bool isMounted() const noexcept;

		Result<SharedBuffer> read(std::string_view path) const;
		AsyncTask<SharedBuffer> readAsync(std::string_view path) const;

		template <LoadableResource Resource>
		Result<Resource> load(std::string_view path) const
		{
			auto bufferResult = read(path);
			if (!bufferResult)
			{
				return Result<Resource>(std::move(bufferResult).error());
			}

			SharedBuffer buffer = std::move(bufferResult).value();
			const ResourceLoadContext context{ path, buffer->bytes() };
			return Result<Resource>(ResourceLoader<Resource>::load(context));
		}

		template <LoadableResource Resource>
		AsyncTask<Resource> loadAsync(std::string_view path) const
		{
			std::shared_ptr<Detail::AsyncState<Resource>> asyncState = Detail::makeAsyncState<Resource>(m_memoryResource);

			if (m_state == nullptr)
			{
				asyncState->complete(Result<Resource>(makeError(ErrorCode::NotMounted, "Reader is not mounted")));
				return AsyncTask<Resource>(std::move(asyncState));
			}

			if (!m_threadPool)
			{
				asyncState->complete(Result<Resource>(makeError(ErrorCode::ThreadPoolUnavailable, "ReaderConfig::threadPool is empty")));
				return AsyncTask<Resource>(std::move(asyncState));
			}

			std::shared_ptr<Detail::ReaderState> readerState = m_state;
			ThreadPoolRef threadPool = m_threadPool;
			std::string pathCopy(path);

			threadPool.submit(
				[asyncState, readerState, pathCopy = std::move(pathCopy)]() mutable
				{
					auto bufferResult = Detail::readFromState(*readerState, pathCopy);
					if (!bufferResult)
					{
						asyncState->complete(Result<Resource>(std::move(bufferResult).error()));
						return;
					}

					SharedBuffer buffer = std::move(bufferResult).value();
					const ResourceLoadContext context{ std::string_view(pathCopy.data(), pathCopy.size()), buffer->bytes() };
					asyncState->complete(Result<Resource>(ResourceLoader<Resource>::load(context)));
				});

			return AsyncTask<Resource>(std::move(asyncState));
		}

		bool exists(std::string_view path) const;
		bool isDirectory(std::string_view path) const;
		Result<EntryInfo> stat(std::string_view path) const;
		Result<void> listDirectory(std::string_view path, const DirectoryVisitor& visitor) const;

		Result<ReaderInfo> info() const;
		/// @brief Replace the cache capacity; evicts entries if needed.
		void setCacheCapacity(std::size_t cacheCapacityBytes) noexcept;
		void clearCache() noexcept;
		CacheStatistics cacheStatistics() const noexcept;

	private:
		ReaderConfig m_config;
		std::pmr::memory_resource* m_memoryResource = nullptr;
		ThreadPoolRef m_threadPool;
		std::shared_ptr<Detail::ReaderState> m_state;
	};
}
