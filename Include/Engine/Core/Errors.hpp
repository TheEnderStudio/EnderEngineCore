#pragma once

#include "Types.hpp"

EE_NAMESPACE_BEGIN

/**
 * @brief Core module error codes.
 */
enum class CoreError {
	None,
	AlreadyInitialized,
	NotInitialized,
	InvalidArgument,
	ObjectNotFound,
	ObjectAlreadyRegistered,
	OperationFailed,
	SubsystemCrashed,
	SubsystemRecoveryFailed,
	ThreadingError,
};

/**
 * @brief Convert a CoreError to a human-readable string.
 * @param error The error code.
 * @return A null-terminated descriptive string.
 */
inline const char* ToString(CoreError error) {
	switch (error) {
	case CoreError::None: return "None";
	case CoreError::AlreadyInitialized: return "AlreadyInitialized";
	case CoreError::NotInitialized: return "NotInitialized";
	case CoreError::InvalidArgument: return "InvalidArgument";
	case CoreError::ObjectNotFound: return "ObjectNotFound";
	case CoreError::ObjectAlreadyRegistered: return "ObjectAlreadyRegistered";
	case CoreError::OperationFailed: return "OperationFailed";
	case CoreError::SubsystemCrashed: return "SubsystemCrashed";
	case CoreError::SubsystemRecoveryFailed: return "SubsystemRecoveryFailed";
	case CoreError::ThreadingError: return "ThreadingError";
	}
	return "Unknown";
}

EE_NAMESPACE_END
