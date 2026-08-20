#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_PLATFORM_BEGIN

/// @brief Error codes for platform/windowing operations.
enum class PlatformError {
	None,
	InitFailed,
	WindowCreationFailed,
	AlreadyInitialized,
	NotInitialized,
	InvalidArgument,
};

/// @brief Convert a PlatformError to a human-readable string.
inline const char* ToString(PlatformError e) {
	switch (e) {
	case PlatformError::None: return "None";
	case PlatformError::InitFailed: return "InitFailed";
	case PlatformError::WindowCreationFailed: return "WindowCreationFailed";
	case PlatformError::AlreadyInitialized: return "AlreadyInitialized";
	case PlatformError::NotInitialized: return "NotInitialized";
	case PlatformError::InvalidArgument: return "InvalidArgument";
	}
	return "Unknown";
}

EE_NAMESPACE_PLATFORM_END
