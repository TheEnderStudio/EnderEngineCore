#pragma once

#include <Engine/Core/Subsystem.hpp>
#include <Engine/Input/ActionMap.hpp>
#include <Engine/Input/Errors.hpp>
#include <Engine/Input/InputState.hpp>
#include <Engine/Input/MouseMotionProvider.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Native window handle wrapper (replicated here to avoid a hard
 *        dependency on the Rendering module in the public Input API).
 */
struct InputWindowHandle {
	void* handle = nullptr;
#ifdef EE_LINUX
	void* display = nullptr;
#endif
};

/**
 * @brief Input subsystem configuration descriptor.
 */
struct InputDesc {
	InputWindowHandle windowHandle;
	IMouseMotionProvider* mouseProvider = nullptr;
	bool enableKeyboard = true;
	bool enableMouse = true;
	bool enableGamepads = true;
	bool bufferedEvents = true;
};

/**
 * @brief Input subsystem.
 *
 * Manages OIS input devices (keyboard, mouse, gamepads) and exposes both
 * immediate-mode state polling and buffered event emission through the
 * Subsystem event bus. OIS types are hidden in the implementation.
 */
class EE_API InputSubsystem : public Subsystem {
public:
	explicit InputSubsystem(const InputDesc& desc);
	~InputSubsystem() override;

	EE_NO_COPY(InputSubsystem)
	EE_NO_MOVE(InputSubsystem)

	/**
	 * @brief Get the current aggregated input state.
	 * @return Const reference to the input state snapshot.
	 */
	EE_NODISCARD const InputState& inputState() const { return m_inputState; }

	/**
	 * @brief Get the action map used to translate keys/buttons into game actions.
	 * @return Reference to the action map.
	 */
	EE_NODISCARD ActionMap& actionMap() { return m_actionMap; }

	/**
	 * @brief Get the action map (const overload).
	 * @return Const reference to the action map.
	 */
	EE_NODISCARD const ActionMap& actionMap() const { return m_actionMap; }

	/**
	 * @brief Check whether the keyboard device is available.
	 * @return true if a keyboard is active.
	 */
	EE_NODISCARD bool hasKeyboard() const;

	/**
	 * @brief Check whether the mouse device is available.
	 * @return true if a mouse is active.
	 */
	EE_NODISCARD bool hasMouse() const;

	/**
	 * @brief Check whether a gamepad device is available.
	 * @param deviceId Gamepad device index.
	 * @return true if the gamepad is active.
	 */
	EE_NODISCARD bool hasGamepad(UInt32 deviceId) const;

	/**
	 * @brief Set the mouse cursor visibility.
	 * @param visible true to show the cursor, false to hide it.
	 */
	void setCursorVisible(bool visible);

	/**
	 * @brief Get the mouse cursor visibility.
	 * @return True if the cursor is visible, or false.
	 */
	bool isCursorVisible() const;

	/**
	 * @brief Warp the mouse cursor to the specified screen position.
	 * @param x Screen X coordinate.
	 * @param y Screen Y coordinate.
	 */
	void setMousePosition(Int32 x, Int32 y);

	/**
	 * @brief Lock the mouse cursor to the center of the window.
	 *
	 * When locked, the cursor is hidden and automatically recentered after each
	 * update, which is suitable for first-person camera controls.
	 *
	 * @param locked true to lock the cursor, false to release it.
	 */
	void setCursorLocked(bool locked);

	/// @return true if the cursor is currently locked to the window center.
	EE_NODISCARD bool isCursorLocked() const;

	/**
	 * @brief Check whether a gamepad supports force feedback.
	 * @param deviceId Gamepad device index.
	 * @return true if the device has a force feedback interface.
	 */
	EE_NODISCARD bool supportsForceFeedback(UInt32 deviceId) const;

	/**
	 * @brief Set the master gain of a force feedback device.
	 * @param deviceId Gamepad device index.
	 * @param gain Master gain in [0, 1].
	 */
	void setForceFeedbackGain(UInt32 deviceId, F32 gain);

	/**
	 * @brief Enable or disable auto-centering on a force feedback device.
	 * @param deviceId Gamepad device index.
	 * @param enabled true to enable auto-centering, false to disable.
	 */
	void setForceFeedbackAutoCenter(UInt32 deviceId, bool enabled);

	/**
	 * @brief Play a constant-force rumble effect on a gamepad.
	 *
	 * The effect plays for the requested duration and then stops automatically.
	 *
	 * @param deviceId Gamepad device index.
	 * @param magnitude Effect strength in [0, 1].
	 * @param durationSeconds Duration of the effect in seconds.
	 * @return true if the effect was uploaded successfully.
	 */
	bool playForceFeedbackRumble(UInt32 deviceId, F32 magnitude, F32 durationSeconds);

	/**
	 * @brief Stop and remove all active force feedback effects from a device.
	 * @param deviceId Gamepad device index.
	 */
	void stopAllForceFeedback(UInt32 deviceId);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;
	void onUpdate(F64 deltaTime) override;
	bool onRecover() override;

private:
	struct Impl;

	InputDesc m_desc;
	InputState m_inputState;
	ActionMap m_actionMap;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_INPUT_END
