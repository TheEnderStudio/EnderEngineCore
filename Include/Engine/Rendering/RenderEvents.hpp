#pragma once

#include <Engine/Core/Types.hpp>
#include <Engine/Core/Event.hpp>

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Base class for all render-related events.
class RenderEvent : public Event {
public:
	RenderEvent() = default;
	EE_DEFAULT_COPY(RenderEvent)
	EE_DEFAULT_MOVE(RenderEvent)
	virtual ~RenderEvent() = default;
};

/// @brief Event emitted at the start of each rendered frame.
class FrameBeginEvent : public RenderEvent {
public:
	UInt64 frameNumber = 0;
	F32 deltaTime = 0.0f;
};

/// @brief Event emitted at the end of each rendered frame.
class FrameEndEvent : public RenderEvent {
public:
	UInt64 frameNumber = 0;
	F32 gpuTimeMs = 0.0f;
};

/// @brief Event emitted when the swap chain is resized.
class SwapChainResizeEvent : public RenderEvent {
public:
	UInt32 w = 0;
	UInt32 h = 0;
};

/// @brief Event emitted when a model has finished loading.
class ModelLoadedEvent : public RenderEvent {
public:
	String filePath;
	Guid owner;
};

/// @brief Event emitted when a model fails to load.
class ModelLoadFailedEvent : public RenderEvent {
public:
	String filePath;
	String errorMessage;
};

EE_NAMESPACE_RENDERING_END
