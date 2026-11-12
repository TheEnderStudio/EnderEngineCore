#pragma once

#include <Core/Types.hpp>
#include "Errors.hpp"

#include <functional>

EE_NAMESPACE_BEGIN

/// @brief Raw resource data buffer.
using ResData = Vector<Byte>;

/// @brief Async read callback: (virtualPath, data, error). Data is empty on failure.
using ResourceAsyncCallback = std::function<void(const ResPath&, ResData, ResourceError)>;

/// @brief Metadata of one entry in the mounted archive.
struct ResourceFileInfo {
	ResPath path;                 ///< Virtual path used for the query.
	UInt64 size = 0;              ///< Uncompressed size in bytes.
	UInt64 storedSize = 0;        ///< Stored size in bytes.
	UInt32 crc32 = 0;             ///< CRC32 of the uncompressed content.
	UInt32 volumeIndex = 0;       ///< Index of the volume containing the entry.
	bool isDirectory = false;     ///< Whether the entry is a directory.
	bool compressed = false;      ///< Whether the entry is compressed.
	bool encrypted = false;       ///< Whether the entry is encrypted.
};

/// @brief Summary information of the mounted archive.
struct ResourceArchiveInfo {
	String packageName;           ///< Package name recorded in the index.
	UInt32 fileCount = 0;         ///< Number of files.
	UInt32 directoryCount = 0;    ///< Number of directories, including the root.
	UInt32 volumeCount = 0;       ///< Number of volume files.
	UInt64 totalUncompressedBytes = 0; ///< Sum of uncompressed file sizes.
	UInt64 totalStoredBytes = 0;  ///< Sum of stored file sizes.
	bool encryptedContent = false; ///< Whether file content is encrypted.
	bool encryptedIndex = false;  ///< Whether the index is encrypted.
	bool compressedIndex = false; ///< Whether the index payload is compressed.
};

/// @brief Cache counters for the mounted archive.
struct ResourceCacheStatistics {
	UInt64 hits = 0;              ///< Number of cache hits.
	UInt64 misses = 0;            ///< Number of cache misses.
	UInt64 bytes = 0;             ///< Bytes currently held by the cache.
	UInt64 entries = 0;           ///< Entries currently held by the cache.
};

EE_NAMESPACE_END
