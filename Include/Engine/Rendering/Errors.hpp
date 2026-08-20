#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief Error codes for the Rendering module.
 */
enum class RenderError {
	None,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	DeviceCreationFailed,
	SwapChainCreationFailed,
	WindowNotBound,
	ShaderCompilationFailed,
	PipelineStateCreationFailed,
	BufferCreationFailed,
	TextureCreationFailed,
	SamplerCreationFailed,
	MeshCreationFailed,
	MaterialCreationFailed,
	ModelLoadFailed,
	CameraNotFound,
	LightNotFound,
	ResourceNotFound,
	InvalidHandle,
	PoolExhausted,
	OperationFailed,
	BackendError,
};

/**
 * @brief Convert a RenderError to a human-readable string.
 * @param error The error code.
 * @return C-string representation.
 */
inline const char* ToString(RenderError error) {
	switch (error) {
	case RenderError::None: return "None";
	case RenderError::NotInitialized: return "NotInitialized";
	case RenderError::AlreadyInitialized: return "AlreadyInitialized";
	case RenderError::InvalidArgument: return "InvalidArgument";
	case RenderError::DeviceCreationFailed: return "DeviceCreationFailed";
	case RenderError::SwapChainCreationFailed: return "SwapChainCreationFailed";
	case RenderError::WindowNotBound: return "WindowNotBound";
	case RenderError::ShaderCompilationFailed: return "ShaderCompilationFailed";
	case RenderError::PipelineStateCreationFailed: return "PipelineStateCreationFailed";
	case RenderError::BufferCreationFailed: return "BufferCreationFailed";
	case RenderError::TextureCreationFailed: return "TextureCreationFailed";
	case RenderError::SamplerCreationFailed: return "SamplerCreationFailed";
	case RenderError::MeshCreationFailed: return "MeshCreationFailed";
	case RenderError::MaterialCreationFailed: return "MaterialCreationFailed";
	case RenderError::ModelLoadFailed: return "ModelLoadFailed";
	case RenderError::CameraNotFound: return "CameraNotFound";
	case RenderError::LightNotFound: return "LightNotFound";
	case RenderError::ResourceNotFound: return "ResourceNotFound";
	case RenderError::InvalidHandle: return "InvalidHandle";
	case RenderError::PoolExhausted: return "PoolExhausted";
	case RenderError::OperationFailed: return "OperationFailed";
	case RenderError::BackendError: return "BackendError";
	}
	return "Unknown";
}

EE_NAMESPACE_RENDERING_END
