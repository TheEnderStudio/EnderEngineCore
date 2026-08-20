#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Types.hpp>
#include <Engine/Input/InputTypes.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Per-gamepad state snapshot.
 */
struct GamepadState {
	Array<bool, GamepadButtonCount> buttons{};
	Array<F32, GamepadAxisCount> axes{};
	bool connected = false;
};

/**
 * @brief Snapshot of all current input device states.
 *
 * InputState is updated by the InputSubsystem each frame and can be polled
 * directly for immediate-mode input queries. It is also the source of truth
 * used when emitting buffered events.
 */
class EE_API InputState {
public:
	InputState();

	/**
	 * @brief Reset all state to default (no keys/buttons pressed, zero axes).
	 */
	void clear();

	/**
	 * @brief Check whether a key is currently held down.
	 * @param key The key to test.
	 * @return true if the key is down.
	 */
	EE_NODISCARD bool isKeyDown(KeyCode key) const;

	/**
	 * @brief Check whether a key was pressed this frame (transition from up to down).
	 * @param key The key to test.
	 * @return true if the key transitioned to down this frame.
	 */
	EE_NODISCARD bool wasKeyPressedThisFrame(KeyCode key) const;

	/**
	 * @brief Check whether a key was released this frame (transition from down to up).
	 * @param key The key to test.
	 * @return true if the key transitioned to up this frame.
	 */
	EE_NODISCARD bool wasKeyReleasedThisFrame(KeyCode key) const;

	/**
	 * @brief Check whether a mouse button is currently held down.
	 * @param button The button to test.
	 * @return true if the button is down.
	 */
	EE_NODISCARD bool isMouseButtonDown(MouseButton button) const;

	/**
	 * @brief Check whether a mouse button was pressed this frame.
	 * @param button The button to test.
	 * @return true if the button transitioned to down this frame.
	 */
	EE_NODISCARD bool wasMouseButtonPressedThisFrame(MouseButton button) const;

	/**
	 * @brief Check whether a mouse button was released this frame.
	 * @param button The button to test.
	 * @return true if the button transitioned to up this frame.
	 */
	EE_NODISCARD bool wasMouseButtonReleasedThisFrame(MouseButton button) const;

	/// @return Current absolute mouse X coordinate in pixels.
	EE_NODISCARD Int32 mouseX() const { return m_mouseX; }

	/// @return Current absolute mouse Y coordinate in pixels.
	EE_NODISCARD Int32 mouseY() const { return m_mouseY; }

	/// @return Mouse X movement since the last update.
	EE_NODISCARD Int32 mouseDeltaX() const { return m_mouseDeltaX; }

	/// @return Mouse Y movement since the last update.
	EE_NODISCARD Int32 mouseDeltaY() const { return m_mouseDeltaY; }

	/// @return Mouse wheel delta from the last update.
	EE_NODISCARD Int32 mouseWheelDelta() const { return m_mouseWheelDelta; }

	/**
	 * @brief Query a gamepad button state.
	 * @param deviceId Gamepad device index.
	 * @param button Button to test.
	 * @return true if the button is down on the requested gamepad.
	 */
	EE_NODISCARD bool isGamepadButtonDown(UInt32 deviceId, GamepadButton button) const;

	/**
	 * @brief Query a normalized gamepad axis value.
	 * @param deviceId Gamepad device index.
	 * @param axis Axis to read.
	 * @return Normalized axis value, or 0 if the device/axis is unavailable.
	 */
	EE_NODISCARD F32 gamepadAxis(UInt32 deviceId, GamepadAxis axis) const;

	/// @return true if the requested gamepad is connected.
	EE_NODISCARD bool isGamepadConnected(UInt32 deviceId) const;

	/// @return Maximum number of gamepads tracked by the state.
	EE_NODISCARD static constexpr UInt32 maxGamepads() { return s_maxGamepads; }

	/**
	 * @brief Advance to a new frame.
	 *
	 * Copies current key/button states to previous states and zeros deltas.
	 */
	void nextFrame();

	/**
	 * @brief Set the pressed state of a key.
	 * @param key The key to modify.
	 * @param down true if the key is pressed.
	 */
	void setKey(KeyCode key, bool down);

	/**
	 * @brief Set the pressed state of a mouse button.
	 * @param button The button to modify.
	 * @param down true if the button is pressed.
	 */
	void setMouseButton(MouseButton button, bool down);

	/**
	 * @brief Set the absolute mouse position.
	 * @param x Absolute X coordinate.
	 * @param y Absolute Y coordinate.
	 */
	void setMousePosition(Int32 x, Int32 y);

	/**
	 * @brief Add relative mouse movement.
	 * @param dx Relative X movement.
	 * @param dy Relative Y movement.
	 */
	void addMouseDelta(Int32 dx, Int32 dy);

	/**
	 * @brief Set a gamepad connection state.
	 * @param deviceId Gamepad device index.
	 * @param connected true if connected.
	 */
	void setGamepadConnected(UInt32 deviceId, bool connected);

	/**
	 * @brief Set a gamepad button state.
	 * @param deviceId Gamepad device index.
	 * @param button Button to modify.
	 * @param down true if pressed.
	 */
	void setGamepadButton(UInt32 deviceId, GamepadButton button, bool down);

	/**
	 * @brief Set a gamepad axis value.
	 * @param deviceId Gamepad device index.
	 * @param axis Axis to modify.
	 * @param value Normalized axis value.
	 */
	void setGamepadAxis(UInt32 deviceId, GamepadAxis axis, F32 value);

private:
	friend class InputSubsystem;

	void beginNewFrame();

	static constexpr UInt32 s_maxGamepads = 4;

	Array<bool, KeyCodeCount> m_keys{};
	Array<bool, KeyCodeCount> m_previousKeys{};

	Array<bool, MouseButtonCount> m_mouseButtons{};
	Array<bool, MouseButtonCount> m_previousMouseButtons{};

	Int32 m_mouseX = 0;
	Int32 m_mouseY = 0;
	Int32 m_mouseDeltaX = 0;
	Int32 m_mouseDeltaY = 0;
	Int32 m_mouseWheelDelta = 0;

	Array<GamepadState, s_maxGamepads> m_gamepads{};
};

EE_NAMESPACE_INPUT_END
