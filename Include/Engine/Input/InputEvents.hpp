#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Event.hpp>
#include <Engine/Input/InputTypes.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Base class for all input events emitted by the Input subsystem.
 */
class EE_API InputEvent : public Event {
};

/**
 * @brief Event fired when a keyboard key is pressed.
 */
class EE_API KeyPressedEvent : public InputEvent {
public:
	KeyPressedEvent(KeyCode key, UInt32 text, bool repeat)
		: key(key), text(text), repeat(repeat) {
	}

	KeyCode key;      ///< The key that was pressed.
	UInt32 text;      ///< Translated text character (if text translation is enabled).
	bool repeat;      ///< true if this is an auto-repeat event.
};

/**
 * @brief Event fired when a keyboard key is released.
 */
class EE_API KeyReleasedEvent : public InputEvent {
public:
	explicit KeyReleasedEvent(KeyCode key) : key(key) {
	}

	KeyCode key;      ///< The key that was released.
};

/**
 * @brief Event fired when a mouse button is pressed.
 */
class EE_API MouseButtonPressedEvent : public InputEvent {
public:
	MouseButtonPressedEvent(MouseButton button, Int32 x, Int32 y)
		: button(button), x(x), y(y) {
	}

	MouseButton button; ///< The button that was pressed.
	Int32 x;            ///< Absolute X coordinate in pixels.
	Int32 y;            ///< Absolute Y coordinate in pixels.
};

/**
 * @brief Event fired when a mouse button is released.
 */
class EE_API MouseButtonReleasedEvent : public InputEvent {
public:
	MouseButtonReleasedEvent(MouseButton button, Int32 x, Int32 y)
		: button(button), x(x), y(y) {
	}

	MouseButton button; ///< The button that was released.
	Int32 x;            ///< Absolute X coordinate in pixels.
	Int32 y;            ///< Absolute Y coordinate in pixels.
};

/**
 * @brief Event fired when the mouse moves.
 */
class EE_API MouseMovedEvent : public InputEvent {
public:
	MouseMovedEvent(Int32 x, Int32 y, Int32 deltaX, Int32 deltaY)
		: x(x), y(y), deltaX(deltaX), deltaY(deltaY) {
	}

	Int32 x;      ///< Absolute X coordinate in pixels.
	Int32 y;      ///< Absolute Y coordinate in pixels.
	Int32 deltaX; ///< Relative X movement since last event.
	Int32 deltaY; ///< Relative Y movement since last event.
};

/**
 * @brief Event fired when the mouse wheel scrolls.
 */
class EE_API MouseScrolledEvent : public InputEvent {
public:
	explicit MouseScrolledEvent(Int32 delta) : delta(delta) {
	}

	Int32 delta;  ///< Wheel delta (positive = away from user, negative = toward user).
};

/**
 * @brief Event fired when a gamepad button is pressed.
 */
class EE_API GamepadButtonPressedEvent : public InputEvent {
public:
	GamepadButtonPressedEvent(UInt32 deviceId, GamepadButton button)
		: deviceId(deviceId), button(button) {
	}

	UInt32 deviceId;    ///< Gamepad device index.
	GamepadButton button; ///< The button that was pressed.
};

/**
 * @brief Event fired when a gamepad button is released.
 */
class EE_API GamepadButtonReleasedEvent : public InputEvent {
public:
	GamepadButtonReleasedEvent(UInt32 deviceId, GamepadButton button)
		: deviceId(deviceId), button(button) {
	}

	UInt32 deviceId;      ///< Gamepad device index.
	GamepadButton button; ///< The button that was released.
};

/**
 * @brief Event fired when a gamepad axis value changes.
 */
class EE_API GamepadAxisMovedEvent : public InputEvent {
public:
	GamepadAxisMovedEvent(UInt32 deviceId, GamepadAxis axis, F32 value)
		: deviceId(deviceId), axis(axis), value(value) {
	}

	UInt32 deviceId;    ///< Gamepad device index.
	GamepadAxis axis;   ///< The axis that moved.
	F32 value;          ///< Normalized axis value in [-1, 1] (triggers in [0, 1]).
};

EE_NAMESPACE_INPUT_END
