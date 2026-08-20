#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Types.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Abstract source of raw mouse motion.
 *
 * The Input subsystem can optionally consume relative mouse movement from an
 * implementation of this interface instead of relying solely on OIS. This is
 * useful when the underlying windowing system (e.g. GLFW) is already receiving
 * raw mouse events and OIS cannot see them.
 *
 * Implementations are expected to accumulate per-frame deltas and reset them
 * when polled.
 */
class EE_API IMouseMotionProvider {
public:
	virtual ~IMouseMotionProvider() = default;

	/**
	 * @brief Poll the accumulated relative mouse movement.
	 * @param dx Receives the X movement since the last poll.
	 * @param dy Receives the Y movement since the last poll.
	 * @return true if the provider is active and returned valid deltas.
	 */
	virtual bool pollMouseDelta(Int32& dx, Int32& dy) = 0;

	/**
	 * @brief Enable or disable raw mouse motion capture.
	 *
	 * When enabled, the provider should switch the underlying cursor into a
	 * hidden, unconstrained raw-input mode suitable for first-person camera
	 * controls. When disabled, normal cursor behavior should be restored.
	 *
	 * @param enabled true to enable raw motion capture, false to restore normal.
	 */
	virtual void setRawMouseMotionEnabled(bool enabled) {
		EE_UNUSED(enabled);
	}
};

EE_NAMESPACE_INPUT_END
