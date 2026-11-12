#include <Resource/ResourcesManager.hpp>
#include <Core/Allocation.hpp>
#include <Core/Log.hpp>

#include <EnderVFiles2/Reader.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

EE_NAMESPACE_BEGIN

namespace {

constexpr Size DefaultResourceCacheSize = 256ull * 1024ull * 1024ull;
constexpr Size ResourceWorkerCount = 2;

/// @brief Small dedicated worker pool for resource reads.
///
/// The engine's job system is not globally accessible from this module, and the
/// reader only needs a submit() entry point, so a small self-contained pool keeps
/// ResourcesManager independent of engine startup order.
class ResourceThreadPool final {
public:
	ResourceThreadPool() {
		m_workers.reserve(ResourceWorkerCount);
		for (Size index = 0; index < ResourceWorkerCount; ++index) {
			m_workers.emplace_back([this]() { workerLoop(); });
		}
	}

	~ResourceThreadPool() {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stopping = true;
		}
		m_condition.notify_all();
		for (std::thread& worker : m_workers) {
			if (worker.joinable()) worker.join();
		}
	}

	EE_NO_COPY(ResourceThreadPool)

	void submit(std::function<void()> task) {
		if (!task) return;

		bool executeInline = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stopping) {
				executeInline = true;
			} else {
				m_tasks.emplace_back(std::move(task));
			}
		}

		if (executeInline) {
			task();
			return;
		}
		m_condition.notify_one();
	}

private:
	void workerLoop() {
		for (;;) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_condition.wait(lock, [this]() { return m_stopping || !m_tasks.empty(); });
				if (m_tasks.empty()) {
					if (m_stopping) return;
					continue;
				}
				task = std::move(m_tasks.front());
				m_tasks.pop_front();
			}
			task();
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::deque<std::function<void()>> m_tasks;
	Vector<std::thread> m_workers;
	bool m_stopping = false;
};

String virtualPathString(const ResPath& path) {
	String value = path.path.generic_string();
	while (!value.empty() && value.front() == '/') {
		value.erase(value.begin());
	}
	while (value.size() > 1 && value.back() == '/') {
		value.pop_back();
	}
	return value;
}

ResData copyBuffer(const EnderVFiles2::SharedBuffer& buffer) {
	ResData data;
	if (buffer != nullptr && buffer->size() != 0) {
		const Byte* first = buffer->data();
		data.assign(first, first + buffer->size());
	}
	return data;
}

ResourceError mapReadError(EnderVFiles2::ErrorCode code) noexcept {
	switch (code) {
	case EnderVFiles2::ErrorCode::NotMounted:
		return ResourceError::NotInitialized;
	case EnderVFiles2::ErrorCode::NotFound:
	case EnderVFiles2::ErrorCode::IsDirectory:
	case EnderVFiles2::ErrorCode::NotAFile:
	case EnderVFiles2::ErrorCode::UnsafePath:
	case EnderVFiles2::ErrorCode::PathTooLong:
		return ResourceError::FileNotFound;
	case EnderVFiles2::ErrorCode::VolumeMissing:
	case EnderVFiles2::ErrorCode::VolumeMismatch:
		return ResourceError::VolumeNotFound;
	case EnderVFiles2::ErrorCode::AllocationFailure:
		return ResourceError::OutOfMemory;
	case EnderVFiles2::ErrorCode::AuthenticationFailed:
	case EnderVFiles2::ErrorCode::PasswordRequired:
		return ResourceError::InvalidPassword;
	default:
		return ResourceError::CorruptedData;
	}
}

ResourceError mapEntryError(EnderVFiles2::ErrorCode code) noexcept {
	switch (code) {
	case EnderVFiles2::ErrorCode::NotMounted:
		return ResourceError::NotInitialized;
	case EnderVFiles2::ErrorCode::NotFound:
	case EnderVFiles2::ErrorCode::UnsafePath:
	case EnderVFiles2::ErrorCode::PathTooLong:
	case EnderVFiles2::ErrorCode::IsDirectory:
	case EnderVFiles2::ErrorCode::NotAFile:
		return ResourceError::FileNotFound;
	case EnderVFiles2::ErrorCode::AllocationFailure:
		return ResourceError::OutOfMemory;
	case EnderVFiles2::ErrorCode::VolumeMissing:
	case EnderVFiles2::ErrorCode::VolumeMismatch:
		return ResourceError::VolumeNotFound;
	default:
		return ResourceError::CorruptedData;
	}
}

ResourceError mapArchiveError(EnderVFiles2::ErrorCode code) noexcept {
	switch (code) {
	case EnderVFiles2::ErrorCode::NotMounted:
		return ResourceError::NotInitialized;
	case EnderVFiles2::ErrorCode::AllocationFailure:
		return ResourceError::OutOfMemory;
	default:
		return ResourceError::CorruptedData;
	}
}

Result<ResData, ResourceError> readFromReader(
	const EnderVFiles2::Reader& reader,
	const ResPath& virtualPath) {
	const String pathString = virtualPathString(virtualPath);
	auto result = reader.read(std::string_view(pathString));
	if (!result) {
		return mapReadError(result.error().code);
	}
	return copyBuffer(result.value());
}

void collectFiles(
	const EnderVFiles2::Reader& reader,
	const String& directory,
	const String& prefix,
	Vector<ResPath>& files) {
	const auto result = reader.listDirectory(std::string_view(directory),
		[&](std::string_view name, bool isDirectoryEntry, UInt64) {
			String child = directory.empty() ? String(name) : directory + "/" + String(name);
			if (isDirectoryEntry) {
				collectFiles(reader, child, prefix, files);
			} else if (prefix.empty() || child.starts_with(prefix)) {
				files.emplace_back(Path(child));
			}
		});
	(void)result;
}

std::shared_ptr<EnderVFiles2::Reader> snapshotReader(
	std::mutex& mutex,
	const std::shared_ptr<EnderVFiles2::Reader>& reader) {
	std::lock_guard<std::mutex> lock(mutex);
	return reader;
}

} // namespace

struct ResourcesManager::Impl {
	mutable std::mutex mutex;
	std::shared_ptr<EnderVFiles2::Reader> reader;
	std::atomic<bool> mounted{ false };
	Size cacheSize = DefaultResourceCacheSize;
	ResourceThreadPool threadPool;
};

ResourcesManager::ResourcesManager() : m_impl(std::make_unique<Impl>()) {}

ResourcesManager::~ResourcesManager() = default;

ResourcesManager& ResourcesManager::getInstance() {
	static ResourcesManager instance;
	return instance;
}

Result<void, ResourceError> ResourcesManager::mount(const Path& indexPath, const String& password) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lock(p.mutex);

	p.reader.reset();
	p.mounted.store(false, std::memory_order_release);

	std::error_code errorCode;
	if (!std::filesystem::exists(indexPath, errorCode) || errorCode) {
		EError("Resource: index not found: {}", indexPath.string());
		return ResourceError::FileNotFound;
	}

	EnderVFiles2::ReaderConfig config;
	config.password = password;
	config.memoryResource = PoolMemoryResource::get();
	config.threadPool = p.threadPool;
	config.cacheCapacityBytes = p.cacheSize;

	auto reader = std::make_shared<EnderVFiles2::Reader>(std::move(config));
	auto mountResult = reader->mount(indexPath);
	if (!mountResult) {
		EError("Resource: mount failed for {}: {}", indexPath.string(), mountResult.error().message);
		// Preserve the historical mount contract: all failures other than a
		// missing index are reported as MountFailed.
		return ResourceError::MountFailed;
	}

	p.reader = std::move(reader);
	p.mounted.store(true, std::memory_order_release);
	EInfo("Resource: mounted {}", indexPath.string());
	return {};
}

void ResourcesManager::unmount() {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lock(p.mutex);
	p.reader.reset();
	p.mounted.store(false, std::memory_order_release);
}

bool ResourcesManager::isMounted() const {
	return m_impl->mounted.load(std::memory_order_acquire);
}

Result<ResData, ResourceError> ResourcesManager::readFile(const ResPath& virtualPath) {
	auto result = readFileEx(virtualPath);
	if (!result) {
		const ResourceError error = result.error();
		if (error == ResourceError::FileNotFound || error == ResourceError::NotInitialized) {
			return error;
		}
		return ResourceError::CorruptedData;
	}

	ResData data = std::move(result.value());
	if (data.empty()) {
		// Preserve the historical contract: readFile() could not distinguish an
		// empty file from a failed read, and reported CorruptedData.
		return ResourceError::CorruptedData;
	}
	return data;
}

Result<ResData, ResourceError> ResourcesManager::readFileEx(const ResPath& virtualPath) const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) {
		return ResourceError::NotInitialized;
	}
	return readFromReader(*reader, virtualPath);
}

void ResourcesManager::readFileAsync(const ResPath& virtualPath, ResourceAsyncCallback callback) {
	auto& p = *m_impl;
	auto reader = snapshotReader(p.mutex, p.reader);
	if (!reader) {
		if (callback) callback(virtualPath, {}, ResourceError::NotInitialized);
		return;
	}
	if (!callback) return;

	ResPath pathCopy = virtualPath;
	p.threadPool.submit(
		[reader = std::move(reader), pathCopy = std::move(pathCopy), callback = std::move(callback)]() mutable {
			auto result = readFromReader(*reader, pathCopy);
			ResData data;
			ResourceError error = ResourceError::None;
			if (result) {
				data = std::move(result.value());
			} else {
				error = result.error();
			}
			callback(pathCopy, std::move(data), error);
		});
}

bool ResourcesManager::fileExists(const ResPath& virtualPath) {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) return false;
	auto result = reader->stat(virtualPathString(virtualPath));
	return result && !result.value().isDirectory;
}

bool ResourcesManager::isDirectory(const ResPath& virtualPath) const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) return false;
	const String pathString = virtualPathString(virtualPath);
	return reader->isDirectory(std::string_view(pathString));
}

Result<ResourceFileInfo, ResourceError> ResourcesManager::statFile(const ResPath& virtualPath) const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) {
		return ResourceError::NotInitialized;
	}

	const String pathString = virtualPathString(virtualPath);
	auto result = reader->stat(std::string_view(pathString));
	if (!result) {
		return mapEntryError(result.error().code);
	}

	const EnderVFiles2::EntryInfo& entry = result.value();
	ResourceFileInfo info;
	info.path.path = virtualPath.path;
	info.size = entry.size;
	info.storedSize = entry.storedSize;
	info.crc32 = entry.crc32;
	info.volumeIndex = entry.volumeIndex;
	info.isDirectory = entry.isDirectory;
	info.compressed = entry.compressed;
	info.encrypted = entry.encrypted;
	return info;
}

Vector<ResPath> ResourcesManager::listFiles(const ResPath& prefix) {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) return {};

	const String prefixString = virtualPathString(prefix);
	Vector<ResPath> files;
	if (prefixString.empty()) {
		collectFiles(*reader, String(), String(), files);
	} else if (reader->isDirectory(std::string_view(prefixString))) {
		collectFiles(*reader, prefixString, String(), files);
	} else {
		collectFiles(*reader, String(), prefixString, files);
	}
	return files;
}

Result<Vector<ResPath>, ResourceError> ResourcesManager::listDirectory(const ResPath& directory) const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) {
		return ResourceError::NotInitialized;
	}

	Vector<ResPath> entries;
	const String directoryString = virtualPathString(directory);
	auto result = reader->listDirectory(std::string_view(directoryString),
		[&](std::string_view name, bool, UInt64) {
			const String child = directoryString.empty() ? String(name) : directoryString + "/" + String(name);
			entries.emplace_back(Path(child));
		});
	if (!result) {
		return mapEntryError(result.error().code);
	}
	return entries;
}

Result<ResourceArchiveInfo, ResourceError> ResourcesManager::archiveInfo() const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) {
		return ResourceError::NotInitialized;
	}

	auto result = reader->info();
	if (!result) {
		return mapArchiveError(result.error().code);
	}

	const EnderVFiles2::ReaderInfo& source = result.value();
	ResourceArchiveInfo info;
	info.packageName = source.packageName;
	info.fileCount = source.fileCount;
	info.directoryCount = source.directoryCount;
	info.volumeCount = source.volumeCount;
	info.totalUncompressedBytes = source.totalUncompressedBytes;
	info.totalStoredBytes = source.totalStoredBytes;
	info.encryptedContent = source.encryptedContent;
	info.encryptedIndex = source.encryptedIndex;
	info.compressedIndex = source.compressedIndex;
	return info;
}

void ResourcesManager::setCacheSize(Size maxSize) {
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lock(p.mutex);
	p.cacheSize = maxSize;
	if (p.reader) {
		p.reader->setCacheCapacity(maxSize);
	}
}

void ResourcesManager::clearCache() {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (reader) {
		reader->clearCache();
	}
}

Size ResourcesManager::cacheHitCount() const {
	return static_cast<Size>(cacheStatistics().hits);
}

Size ResourcesManager::cacheMissCount() const {
	return static_cast<Size>(cacheStatistics().misses);
}

ResourceCacheStatistics ResourcesManager::cacheStatistics() const {
	auto reader = snapshotReader(m_impl->mutex, m_impl->reader);
	if (!reader) return {};

	const EnderVFiles2::CacheStatistics source = reader->cacheStatistics();
	ResourceCacheStatistics statistics;
	statistics.hits = source.hits;
	statistics.misses = source.misses;
	statistics.bytes = source.bytes;
	statistics.entries = source.entries;
	return statistics;
}

EE_NAMESPACE_END
