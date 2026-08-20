#include <Engine/Input/ActionMap.hpp>

EE_NAMESPACE_INPUT_BEGIN

void ActionMap::bindAxis(const String& name, KeyCode positiveKey, KeyCode negativeKey) {
	auto& binding = m_axisBindings[name];
	binding.positiveKey = positiveKey;
	binding.negativeKey = negativeKey;
	m_axisValues[name] = 0.0f;
}

void ActionMap::bindAxis(const String& name, GamepadAxis axis, F32 scale) {
	auto& binding = m_axisBindings[name];
	binding.gamepadAxis = axis;
	binding.scale = scale;
	binding.useGamepadAxis = true;
	m_axisValues[name] = 0.0f;
}

void ActionMap::bindButton(const String& name, KeyCode key) {
	auto& binding = m_buttonBindings[name];
	binding.key = key;
	m_buttonValues[name] = false;
}

void ActionMap::bindButton(const String& name, GamepadButton button) {
	auto& binding = m_buttonBindings[name];
	binding.gamepadButton = button;
	binding.useGamepadButton = true;
	m_buttonValues[name] = false;
}

void ActionMap::clear() {
	m_axisBindings.clear();
	m_buttonBindings.clear();
	m_axisValues.clear();
	m_buttonValues.clear();
}

void ActionMap::update(const InputState& state) {
	for (auto& [name, value] : m_axisValues) {
		value = 0.0f;
	}
	for (auto& [name, value] : m_buttonValues) {
		value = false;
	}

	for (const auto& [name, binding] : m_axisBindings) {
		F32 value = 0.0f;

		const bool positiveDown = (binding.positiveKey != KeyCode::Unassigned) && state.isKeyDown(binding.positiveKey);
		const bool negativeDown = (binding.negativeKey != KeyCode::Unassigned) && state.isKeyDown(binding.negativeKey);

		if (positiveDown && negativeDown) {
			value = 0.0f;
		}
		else if (positiveDown) {
			value = 1.0f;
		}
		else if (negativeDown) {
			value = -1.0f;
		}

		if (binding.useGamepadAxis) {
			for (UInt32 deviceId = 0; deviceId < InputState::maxGamepads(); ++deviceId) {
				if (state.isGamepadConnected(deviceId)) {
					value += state.gamepadAxis(deviceId, binding.gamepadAxis) * binding.scale;
				}
			}
		}

		m_axisValues[name] = Clamp(value, -1.0f, 1.0f);
	}

	for (const auto& [name, binding] : m_buttonBindings) {
		bool value = false;

		if (binding.key != KeyCode::Unassigned) {
			value = state.isKeyDown(binding.key);
		}

		if (!value && binding.useGamepadButton) {
			for (UInt32 deviceId = 0; deviceId < InputState::maxGamepads(); ++deviceId) {
				if (state.isGamepadConnected(deviceId) && state.isGamepadButtonDown(deviceId, binding.gamepadButton)) {
					value = true;
					break;
				}
			}
		}

		m_buttonValues[name] = value;
	}
}

F32 ActionMap::axisValue(const String& name) const {
	auto it = m_axisValues.find(name);
	if (it == m_axisValues.end()) {
		return 0.0f;
	}
	return it->second;
}

bool ActionMap::isButtonDown(const String& name) const {
	auto it = m_buttonValues.find(name);
	if (it == m_buttonValues.end()) {
		return false;
	}
	return it->second;
}

EE_NAMESPACE_INPUT_END
