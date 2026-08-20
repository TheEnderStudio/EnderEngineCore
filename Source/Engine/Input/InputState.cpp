#include <Engine/Input/InputState.hpp>

EE_NAMESPACE_INPUT_BEGIN

InputState::InputState() {
	clear();
}

void InputState::clear() {
	m_keys.fill(false);
	m_previousKeys.fill(false);
	m_mouseButtons.fill(false);
	m_previousMouseButtons.fill(false);
	m_mouseX = 0;
	m_mouseY = 0;
	m_mouseDeltaX = 0;
	m_mouseDeltaY = 0;
	m_mouseWheelDelta = 0;

	for (auto& gamepad : m_gamepads) {
		gamepad.buttons.fill(false);
		gamepad.axes.fill(0.0f);
		gamepad.connected = false;
	}
}

void InputState::beginNewFrame() {
	m_previousKeys = m_keys;
	m_previousMouseButtons = m_mouseButtons;
	m_mouseDeltaX = 0;
	m_mouseDeltaY = 0;
	m_mouseWheelDelta = 0;
}

bool InputState::isKeyDown(KeyCode key) const {
	const UInt32 index = static_cast<UInt32>(key);
	return index < KeyCodeCount && m_keys[index];
}

bool InputState::wasKeyPressedThisFrame(KeyCode key) const {
	const UInt32 index = static_cast<UInt32>(key);
	if (index >= KeyCodeCount) {
		return false;
	}
	return m_keys[index] && !m_previousKeys[index];
}

bool InputState::wasKeyReleasedThisFrame(KeyCode key) const {
	const UInt32 index = static_cast<UInt32>(key);
	if (index >= KeyCodeCount) {
		return false;
	}
	return !m_keys[index] && m_previousKeys[index];
}

bool InputState::isMouseButtonDown(MouseButton button) const {
	const UInt32 index = static_cast<UInt32>(button);
	return index < MouseButtonCount && m_mouseButtons[index];
}

bool InputState::wasMouseButtonPressedThisFrame(MouseButton button) const {
	const UInt32 index = static_cast<UInt32>(button);
	if (index >= MouseButtonCount) {
		return false;
	}
	return m_mouseButtons[index] && !m_previousMouseButtons[index];
}

bool InputState::wasMouseButtonReleasedThisFrame(MouseButton button) const {
	const UInt32 index = static_cast<UInt32>(button);
	if (index >= MouseButtonCount) {
		return false;
	}
	return !m_mouseButtons[index] && m_previousMouseButtons[index];
}

bool InputState::isGamepadButtonDown(UInt32 deviceId, GamepadButton button) const {
	if (deviceId >= s_maxGamepads) {
		return false;
	}
	const UInt32 index = static_cast<UInt32>(button);
	return m_gamepads[deviceId].connected && index < GamepadButtonCount && m_gamepads[deviceId].buttons[index];
}

F32 InputState::gamepadAxis(UInt32 deviceId, GamepadAxis axis) const {
	if (deviceId >= s_maxGamepads) {
		return 0.0f;
	}
	const UInt32 index = static_cast<UInt32>(axis);
	if (!m_gamepads[deviceId].connected || index >= GamepadAxisCount) {
		return 0.0f;
	}
	return m_gamepads[deviceId].axes[index];
}

bool InputState::isGamepadConnected(UInt32 deviceId) const {
	return deviceId < s_maxGamepads && m_gamepads[deviceId].connected;
}

void InputState::nextFrame() {
	beginNewFrame();
}

void InputState::setKey(KeyCode key, bool down) {
	const UInt32 index = static_cast<UInt32>(key);
	if (index < KeyCodeCount) {
		m_keys[index] = down;
	}
}

void InputState::setMouseButton(MouseButton button, bool down) {
	const UInt32 index = static_cast<UInt32>(button);
	if (index < MouseButtonCount) {
		m_mouseButtons[index] = down;
	}
}

void InputState::setMousePosition(Int32 x, Int32 y) {
	m_mouseX = x;
	m_mouseY = y;
}

void InputState::addMouseDelta(Int32 dx, Int32 dy) {
	m_mouseDeltaX += dx;
	m_mouseDeltaY += dy;
}

void InputState::setGamepadConnected(UInt32 deviceId, bool connected) {
	if (deviceId < s_maxGamepads) {
		m_gamepads[deviceId].connected = connected;
	}
}

void InputState::setGamepadButton(UInt32 deviceId, GamepadButton button, bool down) {
	if (deviceId >= s_maxGamepads) {
		return;
	}
	const UInt32 index = static_cast<UInt32>(button);
	if (index < GamepadButtonCount) {
		m_gamepads[deviceId].buttons[index] = down;
	}
}

void InputState::setGamepadAxis(UInt32 deviceId, GamepadAxis axis, F32 value) {
	if (deviceId >= s_maxGamepads) {
		return;
	}
	const UInt32 index = static_cast<UInt32>(axis);
	if (index < GamepadAxisCount) {
		m_gamepads[deviceId].axes[index] = value;
	}
}

EE_NAMESPACE_INPUT_END
