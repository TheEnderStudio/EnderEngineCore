#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_PHYSICS_BEGIN

/// @brief Error codes for physics operations.
enum class PhysicsError {
	None,
	InitFailed,
	CreateBodyFailed,
	InvalidHandle,
	StepFailed,
};

/// @brief Converts a PhysicsError to its string representation.
inline const char* ToString(PhysicsError error) {
	switch (error) {
	case PhysicsError::None: return "None";
	case PhysicsError::InitFailed: return "InitFailed";
	case PhysicsError::CreateBodyFailed: return "CreateBodyFailed";
	case PhysicsError::InvalidHandle: return "InvalidHandle";
	case PhysicsError::StepFailed: return "StepFailed";
	}
	return "Unknown";
}

EE_NAMESPACE_PHYSICS_END
