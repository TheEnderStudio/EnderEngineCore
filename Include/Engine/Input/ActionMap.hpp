#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Types.hpp>
#include <Engine/Input/InputTypes.hpp>
#include <Engine/Input/InputState.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief A virtual axis binding.
 *
 * An axis can be driven by a pair of keys (positive / negative) and/or a
 * gamepad analog axis. The resulting value is clamped to [-1, 1].
 */
struct AxisBinding {
	KeyCode positiveKey = KeyCode::Unassigned;
	KeyCode negativeKey = KeyCode::Unassigned;
	GamepadAxis gamepadAxis = GamepadAxis::LeftX;
	F32 scale = 1.0f;
	bool useGamepadAxis = false;
};

/**
 * @brief A virtual button binding.
 *
 * A button can be triggered by a keyboard key and/or a gamepad button.
 */
struct ButtonBinding {
	KeyCode key = KeyCode::Unassigned;
	GamepadButton gamepadButton = GamepadButton::A;
	bool useGamepadButton = false;
};

/**
 * @brief Named action mapping layer.
 *
 * ActionMap translates low-level input state (keys, mouse, gamepad) into
 * game-level named axes and buttons. It is updated once per frame by the
 * InputSubsystem.
 */
class EE_API ActionMap {
public:
	ActionMap() = default;
	~ActionMap() = default;

	EE_NO_COPY(ActionMap)
	EE_DEFAULT_MOVE(ActionMap)

	/**
	 * @brief Bind a keyboard key pair to a named axis.
	 * @param name Action name.
	 * @param positiveKey Key that drives the axis to +1.
	 * @param negativeKey Key that drives the axis to -1.
	 */
	void bindAxis(const String& name, KeyCode positiveKey, KeyCode negativeKey);

	/**
	 * @brief Bind a gamepad analog axis to a named axis.
	 * @param name Action name.
	 * @param axis Gamepad axis to sample.
	 * @param scale Multiplier applied to the raw axis value.
	 */
	void bindAxis(const String& name, GamepadAxis axis, F32 scale = 1.0f);

	/**
	 * @brief Bind a keyboard key to a named button.
	 * @param name Action name.
	 * @param key Key that triggers the button.
	 */
	void bindButton(const String& name, KeyCode key);

	/**
	 * @brief Bind a gamepad button to a named button.
	 * @param name Action name.
	 * @param button Gamepad button that triggers the action.
	 */
	void bindButton(const String& name, GamepadButton button);

	/**
	 * @brief Remove all bindings.
	 */
	void clear();

	/**
	 * @brief Recompute all axis and button values from the current input state.
	 * @param state Snapshot of the current input devices.
	 */
	void update(const InputState& state);

	/**
	 * @brief Get the current value of a named axis.
	 * @param name Action name.
	 * @return Axis value in [-1, 1], or 0 if the action is not bound.
	 */
	EE_NODISCARD F32 axisValue(const String& name) const;

	/**
	 * @brief Check whether a named button is currently held down.
	 * @param name Action name.
	 * @return true if the button is down, false if unbound or up.
	 */
	EE_NODISCARD bool isButtonDown(const String& name) const;

private:
	HashMap<String, AxisBinding> m_axisBindings;
	HashMap<String, ButtonBinding> m_buttonBindings;
	HashMap<String, F32> m_axisValues;
	HashMap<String, bool> m_buttonValues;
};

EE_NAMESPACE_INPUT_END
