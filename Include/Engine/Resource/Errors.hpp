#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_BEGIN

/// @brief Error codes for resource management operations.
enum class ResourceError : UInt8 {
	None = 0,
	FileNotFound,
	InvalidPassword,
	CorruptedData,
	VolumeNotFound,
	OutOfMemory,
	MountFailed,
	NotInitialized,
};

/// @brief Convert a ResourceError to a human-readable string.
inline const char* ToString(ResourceError e) {
	switch (e) {
	case ResourceError::None:            return "None";
	case ResourceError::FileNotFound:    return "FileNotFound";
	case ResourceError::InvalidPassword: return "InvalidPassword";
	case ResourceError::CorruptedData:   return "CorruptedData";
	case ResourceError::VolumeNotFound:  return "VolumeNotFound";
	case ResourceError::OutOfMemory:     return "OutOfMemory";
	case ResourceError::MountFailed:     return "MountFailed";
	case ResourceError::NotInitialized:  return "NotInitialized";
	}
	return "Unknown";
}

EE_NAMESPACE_END
