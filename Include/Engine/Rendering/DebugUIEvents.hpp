#pragma once

#include <Core/Types.hpp>
#include <Core/Event.hpp>

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief Emitted each frame between ImGui::NewFrame() and ImGui::EndFrame().
 *
 * Subscribe to this event to draw your own ImGui widgets.
 * The handler runs on the main thread during the rendering loop.
 */
class DebugUIRenderEvent : public Event {
public:
	DebugUIRenderEvent() = default;
	EE_DEFAULT_COPY(DebugUIRenderEvent)
	EE_DEFAULT_MOVE(DebugUIRenderEvent)
	UInt32 width = 0;
	UInt32 height = 0;
};

EE_NAMESPACE_RENDERING_END
