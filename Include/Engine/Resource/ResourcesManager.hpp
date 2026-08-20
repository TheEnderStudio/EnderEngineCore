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

	/// @brief Mount an encrypted archive (.index file).
	Result<void, ResourceError> mount(const Path& indexPath, const String& password);

	/// @brief Unmount the current archive.
	void unmount();

	/// @brief Check whether an archive is mounted.
	bool isMounted() const;

	/// @brief Synchronously read a file by virtual path.
	Result<ResData, ResourceError> readFile(const ResPath& virtualPath);

	/// @brief Asynchronously read a file. Callback is invoked on a worker thread.
	void readFileAsync(const ResPath& virtualPath, ResourceAsyncCallback callback);

	/// @brief Check whether a virtual path exists in the archive.
	bool fileExists(const ResPath& virtualPath);

	/// @brief List all files matching a prefix.
	Vector<ResPath> listFiles(const ResPath& prefix = ResPath(""));

	/// @brief Set the LRU cache size in bytes.
	void setCacheSize(Size maxSize);

	/// @brief Clear the cache.
	void clearCache();

	/// @brief Get cache hit/miss counts.
	Size cacheHitCount() const;
	Size cacheMissCount() const;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_END
