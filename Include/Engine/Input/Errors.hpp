#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Types.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Input subsystem error codes.
 */
enum class InputError {
	None,
	AlreadyInitialized,
	NotInitialized,
	InvalidArgument,
	DeviceCreationFailed,
	BackendError,
	ForceFeedbackNotSupported,
};

/**
 * @brief Convert an InputError to a human-readable string.
 * @param error The error code to convert.
 * @return A null-terminated descriptive string.
 */
inline const char* ToString(InputError error) {
	switch (error) {
	case InputError::None: return "None";
	case InputError::AlreadyInitialized: return "AlreadyInitialized";
	case InputError::NotInitialized: return "NotInitialized";
	case InputError::InvalidArgument: return "InvalidArgument";
	case InputError::DeviceCreationFailed: return "DeviceCreationFailed";
	case InputError::BackendError: return "BackendError";
	case InputError::ForceFeedbackNotSupported: return "ForceFeedbackNotSupported";
	}
	return "Unknown";
}

EE_NAMESPACE_INPUT_END
