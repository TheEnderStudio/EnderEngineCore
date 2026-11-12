#pragma once

#include <Core/Types.hpp>
#include "Types.hpp"
#include "Errors.hpp"

EE_NAMESPACE_BEGIN

/// @brief Singleton manager for encrypted resource archives with LRU caching and async I/O.
class EE_API ResourcesManager {
public:
	/// @brief Construct the resources manager.
	ResourcesManager();
	/// @brief Destroy the resources manager.
	~ResourcesManager();

	EE_NO_COPY(ResourcesManager)
	EE_NO_MOVE(ResourcesManager)

	/// @brief Thread-safe singleton accessor.
	static ResourcesManager& getInstance();

	/// @brief Mount an encrypted archive (.evfidx index file).
	Result<void, ResourceError> mount(const Path& indexPath, const String& password);

	/// @brief Unmount the current archive.
	void unmount();

	/// @brief Check whether an archive is mounted.
	bool isMounted() const;

	/// @brief Synchronously read a file by virtual path.
	/// @note Kept for compatibility: an empty file is reported as CorruptedData.
	EE_DEPRECATED("Use readFileEx() to distinguish an empty file from a read failure.")
	Result<ResData, ResourceError> readFile(const ResPath& virtualPath);

	/// @brief Synchronously read a file by virtual path.
	///
	/// Unlike readFile(), an existing empty file is returned as an empty success.
	EE_NODISCARD Result<ResData, ResourceError> readFileEx(const ResPath& virtualPath) const;

	/// @brief Asynchronously read a file. Callback is invoked on a worker thread.
	void readFileAsync(const ResPath& virtualPath, ResourceAsyncCallback callback);

	/// @brief Check whether a file (not a directory) exists in the archive.
	bool fileExists(const ResPath& virtualPath);

	/// @brief Check whether a virtual path is a directory.
	EE_NODISCARD bool isDirectory(const ResPath& virtualPath) const;

	/// @brief Get metadata for one archive entry.
	EE_NODISCARD Result<ResourceFileInfo, ResourceError> statFile(const ResPath& virtualPath) const;

	/// @brief List all files matching a prefix.
	Vector<ResPath> listFiles(const ResPath& prefix = ResPath(""));

	/// @brief List the immediate children of a directory.
	EE_NODISCARD Result<Vector<ResPath>, ResourceError> listDirectory(const ResPath& directory = ResPath("")) const;

	/// @brief Get summary information for the mounted archive.
	EE_NODISCARD Result<ResourceArchiveInfo, ResourceError> archiveInfo() const;

	/// @brief Set the LRU cache size in bytes. Applies immediately when mounted.
	void setCacheSize(Size maxSize);

	/// @brief Clear the cache.
	void clearCache();

	/// @brief Get cache hit/miss counts.
	Size cacheHitCount() const;
	Size cacheMissCount() const;

	/// @brief Get detailed cache counters.
	EE_NODISCARD ResourceCacheStatistics cacheStatistics() const;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_END
