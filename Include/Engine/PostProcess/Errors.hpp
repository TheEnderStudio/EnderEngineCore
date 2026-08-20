#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_POSTPROCESS_BEGIN

/// @brief Error codes for post-process subsystem operations.
enum class PostProcessError {
	None,
	NotInitialized,
	AlreadyInitialized,
	DeviceCreationFailed,
	ShaderCompilationFailed,
	PipelineStateCreationFailed,
	ResourceCreationFailed,
	InvalidArgument,
	OperationFailed,
};

/// @brief Convert a PostProcessError to a human-readable string.
inline const char* ToString(PostProcessError e) {
	switch (e) {
	case PostProcessError::None: return "None";
	case PostProcessError::NotInitialized: return "NotInitialized";
	case PostProcessError::AlreadyInitialized: return "AlreadyInitialized";
	case PostProcessError::DeviceCreationFailed: return "DeviceCreationFailed";
	case PostProcessError::ShaderCompilationFailed: return "ShaderCompilationFailed";
	case PostProcessError::PipelineStateCreationFailed: return "PipelineStateCreationFailed";
	case PostProcessError::ResourceCreationFailed: return "ResourceCreationFailed";
	case PostProcessError::InvalidArgument: return "InvalidArgument";
	case PostProcessError::OperationFailed: return "OperationFailed";
	}
	return "Unknown";
}

EE_NAMESPACE_POSTPROCESS_END
