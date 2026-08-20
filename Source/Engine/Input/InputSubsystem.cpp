#include <Engine/Input/InputSubsystem.hpp>

#include <Engine/Core/Log.hpp>
#include <Engine/Input/InputEvents.hpp>

#include <ois/OIS.h>

#include <sstream>
#include <cstring>
#include <algorithm>

#ifdef EE_WINDOWS
# define WIN32_LEAN_AND_MEAN
# include <Windows.h>
#endif

EE_NAMESPACE_INPUT_BEGIN

namespace {

KeyCode convertKeyCode(OIS::KeyCode key) {
	return static_cast<KeyCode>(static_cast<UInt32>(key));
}

MouseButton convertMouseButton(OIS::MouseButtonID button) {
	return static_cast<MouseButton>(static_cast<UInt32>(button));
}

GamepadButton convertGamepadButton(int button) {
	if (button < 0 || button >= static_cast<int>(GamepadButtonCount)) {
		return GamepadButton::A;
	}
	return static_cast<GamepadButton>(static_cast<UInt32>(button));
}

GamepadAxis convertGamepadAxis(int axis) {
	if (axis < 0 || axis >= static_cast<int>(GamepadAxisCount)) {
		return GamepadAxis::LeftX;
	}
	return static_cast<GamepadAxis>(static_cast<UInt32>(axis));
}

F32 normalizeAxis(int value, int minAxis, int maxAxis) {
	if (value == 0) {
		return 0.0f;
	}
	const F32 range = static_cast<F32>(maxAxis - minAxis);
	if (range <= 0.0f) {
		return 0.0f;
	}
	const F32 normalized = (static_cast<F32>(value) * 2.0f / range) - 1.0f;
	return Clamp(normalized, -1.0f, 1.0f);
}

F32 normalizeTrigger(int value, int maxAxis) {
	if (value <= 0 || maxAxis <= 0) {
		return 0.0f;
	}
	const F32 normalized = static_cast<F32>(value) / static_cast<F32>(maxAxis);
	return Clamp(normalized, 0.0f, 1.0f);
}

} // anonymous namespace

struct InputSubsystem::Impl : public OIS::KeyListener,
                              public OIS::MouseListener,
                              public OIS::JoyStickListener {
	OIS::InputManager* inputManager = nullptr;
	OIS::Keyboard* keyboard = nullptr;
	OIS::Mouse* mouse = nullptr;
	Vector<OIS::JoyStick*> gamepads;
	HashMap<UInt32, OIS::ForceFeedback*> forceFeedback;

	struct ActiveEffect {
		UInt32 deviceId = 0;
		OIS::ForceFeedback* ff = nullptr;
		OIS::Effect* effect = nullptr;
	};
	Vector<ActiveEffect> activeEffects;

	InputSubsystem* owner = nullptr;
	Int32 lastMouseX = 0;
	Int32 lastMouseY = 0;
	bool cursorVisible = true;
	bool cursorLocked = false;
	bool ignoreNextMouseDelta = false;

	bool keyPressed(const OIS::KeyEvent& arg) override {
		const KeyCode key = convertKeyCode(arg.key);
		m_frameKeys[static_cast<UInt32>(key)] = true;
		owner->emit(KeyPressedEvent(key, arg.text, false));
		return true;
	}

	bool keyReleased(const OIS::KeyEvent& arg) override {
		const KeyCode key = convertKeyCode(arg.key);
		m_frameKeys[static_cast<UInt32>(key)] = false;
		owner->emit(KeyReleasedEvent(key));
		return true;
	}

	bool mouseMoved(const OIS::MouseEvent& arg) override {
		const OIS::MouseState& s = arg.state;
		m_frameMouseX = s.X.abs;
		m_frameMouseY = s.Y.abs;
		// Do not accumulate m_frameMouseDeltaX/Y here; onUpdate reads the
		// authoritative MouseState.X/Y.rel directly to avoid double-counting.
		owner->emit(MouseMovedEvent(s.X.abs, s.Y.abs, s.X.rel, s.Y.rel));
		return true;
	}

	bool mousePressed(const OIS::MouseEvent& arg, OIS::MouseButtonID id) override {
		const OIS::MouseState& s = arg.state;
		m_frameMouseButtons[static_cast<UInt32>(id)] = true;
		owner->emit(MouseButtonPressedEvent(convertMouseButton(id), s.X.abs, s.Y.abs));
		return true;
	}

	bool mouseReleased(const OIS::MouseEvent& arg, OIS::MouseButtonID id) override {
		const OIS::MouseState& s = arg.state;
		m_frameMouseButtons[static_cast<UInt32>(id)] = false;
		owner->emit(MouseButtonReleasedEvent(convertMouseButton(id), s.X.abs, s.Y.abs));
		return true;
	}

	bool buttonPressed(const OIS::JoyStickEvent& arg, int button) override {
		const UInt32 deviceId = static_cast<UInt32>(arg.device->getID());
		if (deviceId < InputState::maxGamepads()) {
			m_frameGamepadButtons[deviceId][button] = true;
		}
		owner->emit(GamepadButtonPressedEvent(deviceId, convertGamepadButton(button)));
		return true;
	}

	bool buttonReleased(const OIS::JoyStickEvent& arg, int button) override {
		const UInt32 deviceId = static_cast<UInt32>(arg.device->getID());
		if (deviceId < InputState::maxGamepads()) {
			m_frameGamepadButtons[deviceId][button] = false;
		}
		owner->emit(GamepadButtonReleasedEvent(deviceId, convertGamepadButton(button)));
		return true;
	}

	bool axisMoved(const OIS::JoyStickEvent& arg, int axis) override {
		const UInt32 deviceId = static_cast<UInt32>(arg.device->getID());
		if (deviceId >= InputState::maxGamepads()) {
			return true;
		}

		const OIS::Axis& oisAxis = arg.state.mAxes[axis];
		F32 value = 0.0f;

		if (axis < static_cast<int>(GamepadAxis::LeftTrigger)) {
			value = normalizeAxis(oisAxis.abs, OIS::JoyStick::MIN_AXIS, OIS::JoyStick::MAX_AXIS);
		}
		else {
			value = normalizeTrigger(oisAxis.abs, OIS::JoyStick::MAX_AXIS);
		}

		m_frameGamepadAxes[deviceId][axis] = value;
		owner->emit(GamepadAxisMovedEvent(deviceId, convertGamepadAxis(axis), value));
		return true;
	}

	bool sliderMoved(const OIS::JoyStickEvent& arg, int index) override {
		EE_UNUSED(arg);
		EE_UNUSED(index);
		return true;
	}

	bool povMoved(const OIS::JoyStickEvent& arg, int index) override {
		EE_UNUSED(arg);
		EE_UNUSED(index);
		return true;
	}

	bool vector3Moved(const OIS::JoyStickEvent& arg, int index) override {
		EE_UNUSED(arg);
		EE_UNUSED(index);
		return true;
	}

	void resetFrameState() {
		std::memset(m_frameKeys.data(), 0, m_frameKeys.size());
		std::memset(m_frameMouseButtons.data(), 0, m_frameMouseButtons.size());
		m_frameMouseDeltaX = 0;
		m_frameMouseDeltaY = 0;
		m_frameMouseWheelDelta = 0;
		for (auto& buttons : m_frameGamepadButtons) {
			buttons.fill(false);
		}
		for (auto& axes : m_frameGamepadAxes) {
			axes.fill(0.0f);
		}
	}

	Array<bool, KeyCodeCount> m_frameKeys{};
	Array<bool, MouseButtonCount> m_frameMouseButtons{};
	Int32 m_frameMouseX = 0;
	Int32 m_frameMouseY = 0;
	Int32 m_frameMouseDeltaX = 0;
	Int32 m_frameMouseDeltaY = 0;
	Int32 m_frameMouseWheelDelta = 0;
	Array<Array<bool, GamepadButtonCount>, InputState::maxGamepads()> m_frameGamepadButtons{};
	Array<Array<F32, GamepadAxisCount>, InputState::maxGamepads()> m_frameGamepadAxes{};
};

InputSubsystem::InputSubsystem(const InputDesc& desc)
	: Subsystem("Input"), m_desc(desc), m_impl(std::make_unique<Impl>()) {
	m_impl->owner = this;
	m_impl->cursorVisible = false;
	setUpdateMode(SubsystemUpdateMode::FixedMainThread);
}

InputSubsystem::~InputSubsystem() {
	if (state() != SubsystemState::Shutdown) {
		shutdown();
	}
}

Result<void, CoreError> InputSubsystem::onInitialize() {
	if (m_impl->inputManager) {
		return CoreError::AlreadyInitialized;
	}

	if (!m_desc.windowHandle.handle) {
		EError("Input subsystem initialized without a valid window handle.");
		return CoreError::InvalidArgument;
	}

	try {
		OIS::ParamList paramList;
		std::ostringstream windowHandleStr;
		windowHandleStr << reinterpret_cast<std::size_t>(m_desc.windowHandle.handle);
		paramList.insert(std::make_pair(std::string("WINDOW"), windowHandleStr.str()));

#ifdef EE_WINDOWS
		// Use Raw Input (hardware) mouse mode so relative movement does not depend
		// on the system cursor position or GLFW's message dispatching.
		paramList.insert(std::make_pair(std::string("w32_mouse"), std::string("Hardware")));
#endif

#ifdef EE_LINUX
		if (m_desc.windowHandle.display) {
			std::ostringstream displayStr;
			displayStr << reinterpret_cast<std::size_t>(m_desc.windowHandle.display);
			paramList.insert(std::make_pair(std::string("X11Display"), displayStr.str()));
		}
#endif

		m_impl->inputManager = OIS::InputManager::createInputSystem(paramList);
		if (!m_impl->inputManager) {
			return CoreError::OperationFailed;
		}

		EInfo("OIS input system created ({}).", m_impl->inputManager->inputSystemName());

		if (m_desc.enableKeyboard) {
			m_impl->keyboard = static_cast<OIS::Keyboard*>(
				m_impl->inputManager->createInputObject(OIS::OISKeyboard, m_desc.bufferedEvents));
			if (m_impl->keyboard) {
				m_impl->keyboard->setEventCallback(m_impl.get());
				EDebug("Keyboard device created.");
			}
		}

		if (m_desc.enableMouse) {
			m_impl->mouse = static_cast<OIS::Mouse*>(
				m_impl->inputManager->createInputObject(OIS::OISMouse, m_desc.bufferedEvents));
			if (m_impl->mouse) {
				m_impl->mouse->setEventCallback(m_impl.get());

#ifdef EE_WINDOWS
				HWND hwnd = static_cast<HWND>(m_desc.windowHandle.handle);
				RECT clientRect;
				if (GetClientRect(hwnd, &clientRect)) {
					m_impl->mouse->getMouseState().width  = clientRect.right - clientRect.left;
					m_impl->mouse->getMouseState().height = clientRect.bottom - clientRect.top;
				}
#endif
				EDebug("Mouse device created.");
			}
		}

		if (m_desc.enableGamepads) {
			const int joyCount = m_impl->inputManager->getNumberOfDevices(OIS::OISJoyStick);
			for (int i = 0; i < joyCount && i < static_cast<int>(InputState::maxGamepads()); ++i) {
				auto* joystick = static_cast<OIS::JoyStick*>(
					m_impl->inputManager->createInputObject(OIS::OISJoyStick, m_desc.bufferedEvents, ""));
				if (joystick) {
					joystick->setEventCallback(m_impl.get());
					m_impl->gamepads.push_back(joystick);
					const UInt32 deviceId = static_cast<UInt32>(joystick->getID());
					if (deviceId < InputState::maxGamepads()) {
						m_inputState.m_gamepads[deviceId].connected = true;

						OIS::Interface* ffInterface = joystick->queryInterface(OIS::Interface::ForceFeedback);
						if (ffInterface) {
							auto* ff = static_cast<OIS::ForceFeedback*>(ffInterface);
							m_impl->forceFeedback[deviceId] = ff;
							EDebug("Gamepad {} supports force feedback.", deviceId);
						}
					}
					EDebug("Gamepad {} created: '{}'.", deviceId, joystick->vendor());
				}
			}
		}
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception during input initialization: {}", e.eText);
		return CoreError::OperationFailed;
	}
	catch (const std::exception& e) {
		EError("Standard exception during input initialization: {}", e.what());
		return CoreError::OperationFailed;
	}

	m_inputState.clear();
	m_impl->resetFrameState();
	return {};
}

void InputSubsystem::onShutdown() {
	try {
		for (const auto& [deviceId, ff] : m_impl->forceFeedback) {
			stopAllForceFeedback(deviceId);
		}
		m_impl->forceFeedback.clear();

		for (auto* joystick : m_impl->gamepads) {
			const UInt32 deviceId = static_cast<UInt32>(joystick->getID());
			if (deviceId < InputState::maxGamepads()) {
				m_inputState.m_gamepads[deviceId].connected = false;
			}
			if (m_impl->inputManager) {
				m_impl->inputManager->destroyInputObject(joystick);
			}
		}
		m_impl->gamepads.clear();

		if (m_impl->mouse && !m_impl->cursorVisible) {
			if (m_impl->inputManager) {
				m_impl->inputManager->destroyInputObject(m_impl->mouse);
			}
			m_impl->mouse = nullptr;
		}

		if (m_impl->keyboard) {
			if (m_impl->inputManager) {
				m_impl->inputManager->destroyInputObject(m_impl->keyboard);
			}
			m_impl->keyboard = nullptr;
		}

		if (m_impl->inputManager) {
			OIS::InputManager::destroyInputSystem(m_impl->inputManager);
			m_impl->inputManager = nullptr;
		}
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception during input shutdown: {}", e.eText);
	}

	m_inputState.clear();
}

void InputSubsystem::onUpdate(F64 deltaTime) {
	EE_UNUSED(deltaTime);

	if (!m_impl->inputManager) {
		return;
	}

	m_inputState.beginNewFrame();
	m_impl->resetFrameState();

	try {
		if (m_impl->keyboard) {
			m_impl->keyboard->capture();
			char keyBuffer[256] = {};
			m_impl->keyboard->copyKeyStates(keyBuffer);
			for (UInt32 i = 0; i < KeyCodeCount; ++i) {
				m_inputState.m_keys[i] = keyBuffer[i] != 0;
			}
		}

		if (m_impl->mouse && !m_impl->cursorVisible) {
			m_impl->mouse->capture();
			const OIS::MouseState& mouseState = m_impl->mouse->getMouseState();

			m_inputState.m_mouseX = mouseState.X.abs;
			m_inputState.m_mouseY = mouseState.Y.abs;

			// In all modes, rely on OIS's relative movement. In hardware mouse mode
			// this is the raw device delta; in software mode it is the cursor delta.
			m_inputState.m_mouseDeltaX = mouseState.X.rel;
			m_inputState.m_mouseDeltaY = mouseState.Y.rel;

			if (m_impl->cursorLocked && !m_desc.mouseProvider && m_desc.windowHandle.handle) {
#ifdef EE_WINDOWS
				// Keep the cursor centered so it cannot leave the window or hit
				// screen edges while locked.
				HWND hwnd = static_cast<HWND>(m_desc.windowHandle.handle);
				RECT clientRect;
				if (GetClientRect(hwnd, &clientRect)) {
					const Int32 centerX = (clientRect.right - clientRect.left) / 2;
					const Int32 centerY = (clientRect.bottom - clientRect.top) / 2;
					POINT screenCenter{ centerX, centerY };
					if (ClientToScreen(hwnd, &screenCenter)) {
						SetCursorPos(screenCenter.x, screenCenter.y);
					}
				}
#endif
			}

			if (m_impl->ignoreNextMouseDelta) {
				m_inputState.m_mouseDeltaX = 0;
				m_inputState.m_mouseDeltaY = 0;
				m_impl->ignoreNextMouseDelta = false;
			}

			m_inputState.m_mouseWheelDelta = mouseState.Z.rel;

			for (UInt32 i = 0; i < MouseButtonCount; ++i) {
				m_inputState.m_mouseButtons[i] = mouseState.buttonDown(static_cast<OIS::MouseButtonID>(i));
			}

			m_impl->lastMouseX = mouseState.X.abs;
			m_impl->lastMouseY = mouseState.Y.abs;
		}

		// If a raw mouse motion provider is available (e.g. GLFW), merge its deltas
		// into the input state. This is a fallback/replacement for OIS mouse relative
		// motion when the OIS backend cannot see window messages.
		if (m_desc.mouseProvider) {
			Int32 providerDx = 0;
			Int32 providerDy = 0;
			if (m_desc.mouseProvider->pollMouseDelta(providerDx, providerDy)) {
				m_inputState.m_mouseDeltaX += providerDx;
				m_inputState.m_mouseDeltaY += providerDy;
			}
		}

		for (auto* joystick : m_impl->gamepads) {
			joystick->capture();
			const UInt32 deviceId = static_cast<UInt32>(joystick->getID());
			if (deviceId >= InputState::maxGamepads()) {
				continue;
			}

			auto& gamepad = m_inputState.m_gamepads[deviceId];
			gamepad.connected = true;

			const OIS::JoyStickState& joyState = joystick->getJoyStickState();
			for (size_t i = 0; i < joyState.mButtons.size() && i < GamepadButtonCount; ++i) {
				gamepad.buttons[i] = joyState.mButtons[i];
			}

			for (size_t i = 0; i < joyState.mAxes.size() && i < GamepadAxisCount; ++i) {
				if (i < static_cast<size_t>(GamepadAxis::LeftTrigger)) {
					gamepad.axes[i] = normalizeAxis(joyState.mAxes[i].abs, OIS::JoyStick::MIN_AXIS, OIS::JoyStick::MAX_AXIS);
				}
				else {
					gamepad.axes[i] = normalizeTrigger(joyState.mAxes[i].abs, OIS::JoyStick::MAX_AXIS);
				}
			}
		}

		// Merge buffered frame contributions captured by OIS listeners into the state.
		for (UInt32 i = 0; i < KeyCodeCount; ++i) {
			if (m_impl->m_frameKeys[i]) {
				m_inputState.m_keys[i] = true;
			}
		}
		for (UInt32 i = 0; i < MouseButtonCount; ++i) {
			m_inputState.m_mouseButtons[i] = m_impl->m_frameMouseButtons[i];
		}
		m_inputState.m_mouseX = m_impl->m_frameMouseX;
		m_inputState.m_mouseY = m_impl->m_frameMouseY;
		m_inputState.m_mouseDeltaX += m_impl->m_frameMouseDeltaX;
		m_inputState.m_mouseDeltaY += m_impl->m_frameMouseDeltaY;
		m_inputState.m_mouseWheelDelta += m_impl->m_frameMouseWheelDelta;

		for (UInt32 deviceId = 0; deviceId < InputState::maxGamepads(); ++deviceId) {
			for (UInt32 i = 0; i < GamepadButtonCount; ++i) {
				m_inputState.m_gamepads[deviceId].buttons[i] = m_impl->m_frameGamepadButtons[deviceId][i];
			}
			for (UInt32 i = 0; i < GamepadAxisCount; ++i) {
				m_inputState.m_gamepads[deviceId].axes[i] += m_impl->m_frameGamepadAxes[deviceId][i];
			}
		}
		m_actionMap.update(m_inputState);
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception during input update: {}", e.eText);
	}
}

bool InputSubsystem::onRecover() {
	EWarn("Input subsystem attempting crash recovery.");
	onShutdown();
	return onInitialize().isOk();
}

bool InputSubsystem::hasKeyboard() const {
	return m_impl->keyboard != nullptr;
}

bool InputSubsystem::hasMouse() const {
	return m_impl->mouse != nullptr;
}

bool InputSubsystem::hasGamepad(UInt32 deviceId) const {
	return deviceId < InputState::maxGamepads() && m_inputState.isGamepadConnected(deviceId);
}

void InputSubsystem::setCursorVisible(bool visible) {
	if (m_impl->cursorVisible == visible) {
		return;
	}
	m_impl->cursorVisible = visible;
	if (visible) {
		m_impl->inputManager->destroyInputObject(m_impl->mouse);
		EDebug("Mouse device destroyed.");
	}
	else {
		m_impl->mouse = static_cast<OIS::Mouse*>(
			m_impl->inputManager->createInputObject(OIS::OISMouse, m_desc.bufferedEvents));
		if (m_impl->mouse) {
			m_impl->mouse->setEventCallback(m_impl.get());

#ifdef EE_WINDOWS
			HWND hwnd = static_cast<HWND>(m_desc.windowHandle.handle);
			RECT clientRect;
			if (GetClientRect(hwnd, &clientRect)) {
				m_impl->mouse->getMouseState().width = clientRect.right - clientRect.left;
				m_impl->mouse->getMouseState().height = clientRect.bottom - clientRect.top;
			}
#endif
			EDebug("Mouse device created.");
		}
	}
}

bool InputSubsystem::isCursorVisible() const {
	return m_impl->cursorVisible;
}

void InputSubsystem::setMousePosition(Int32 x, Int32 y) {
#ifdef EE_WINDOWS
	SetCursorPos(x, y);
#else
	EE_UNUSED(x);
	EE_UNUSED(y);
#endif
}

void InputSubsystem::setCursorLocked(bool locked) {
	if (m_impl->cursorLocked == locked) {
		return;
	}
	m_impl->cursorLocked = locked;

	if (m_desc.mouseProvider) {
		// Delegate cursor capture and raw motion entirely to the provider.
		m_desc.mouseProvider->setRawMouseMotionEnabled(locked);
		m_impl->cursorVisible = !locked;
		return;
	}

	setCursorVisible(!locked);

#ifdef EE_WINDOWS
	if (locked && m_desc.windowHandle.handle && m_impl->mouse) {
		HWND hwnd = static_cast<HWND>(m_desc.windowHandle.handle);
		RECT clientRect;
		if (GetClientRect(hwnd, &clientRect)) {
			const Int32 centerX = (clientRect.right - clientRect.left) / 2;
			const Int32 centerY = (clientRect.bottom - clientRect.top) / 2;
			POINT screenCenter{ centerX, centerY };
			if (ClientToScreen(hwnd, &screenCenter)) {
				SetCursorPos(screenCenter.x, screenCenter.y);
			}
			// Ignore the large delta caused by the cursor warp on the next update.
			m_impl->ignoreNextMouseDelta = true;
		}
	}
#endif
}

bool InputSubsystem::isCursorLocked() const {
	return m_impl->cursorLocked;
}

bool InputSubsystem::supportsForceFeedback(UInt32 deviceId) const {
	if (deviceId >= InputState::maxGamepads()) {
		return false;
	}
	if (!m_inputState.isGamepadConnected(deviceId)) {
		return false;
	}
	return m_impl->forceFeedback.find(deviceId) != m_impl->forceFeedback.end();
}

void InputSubsystem::setForceFeedbackGain(UInt32 deviceId, F32 gain) {
	if (deviceId >= InputState::maxGamepads()) {
		return;
	}
	auto it = m_impl->forceFeedback.find(deviceId);
	if (it == m_impl->forceFeedback.end() || !it->second) {
		return;
	}
	try {
		it->second->setMasterGain(Clamp(gain, 0.0f, 1.0f));
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception while setting force feedback gain: {}", e.eText);
	}
}

void InputSubsystem::setForceFeedbackAutoCenter(UInt32 deviceId, bool enabled) {
	if (deviceId >= InputState::maxGamepads()) {
		return;
	}
	auto it = m_impl->forceFeedback.find(deviceId);
	if (it == m_impl->forceFeedback.end() || !it->second) {
		return;
	}
	try {
		it->second->setAutoCenterMode(enabled);
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception while setting force feedback auto-center: {}", e.eText);
	}
}

bool InputSubsystem::playForceFeedbackRumble(UInt32 deviceId, F32 magnitude, F32 durationSeconds) {
	if (deviceId >= InputState::maxGamepads()) {
		return false;
	}
	auto it = m_impl->forceFeedback.find(deviceId);
	if (it == m_impl->forceFeedback.end() || !it->second) {
		return false;
	}

	OIS::ForceFeedback* ff = it->second;
	try {
		auto* effect = new OIS::Effect(OIS::Effect::ConstantForce, OIS::Effect::Constant);
		effect->direction = OIS::Effect::East;
		effect->trigger_button = -1;
		effect->replay_length = static_cast<unsigned int>(Clamp(durationSeconds, 0.0f, 60.0f) * 1'000'000.0f);
		effect->replay_delay = 0;
		effect->setNumAxes(1);

		auto* constantEffect = static_cast<OIS::ConstantEffect*>(effect->getForceEffect());
		if (constantEffect) {
			constantEffect->level = static_cast<signed short>(Clamp(magnitude, 0.0f, 1.0f) * 10000.0f);
		}

		ff->upload(effect);
		m_impl->activeEffects.push_back({ deviceId, ff, effect });
		return true;
	}
	catch (const OIS::Exception& e) {
		EError("OIS exception while playing force feedback rumble: {}", e.eText);
		return false;
	}
}

void InputSubsystem::stopAllForceFeedback(UInt32 deviceId) {
	if (deviceId >= InputState::maxGamepads()) {
		return;
	}

	for (auto& active : m_impl->activeEffects) {
		if (active.deviceId != deviceId || !active.effect || !active.ff) {
			continue;
		}
		try {
			active.ff->remove(active.effect);
		}
		catch (const OIS::Exception& e) {
			EError("OIS exception while removing force feedback effect: {}", e.eText);
		}
		delete active.effect;
		active.effect = nullptr;
		active.ff = nullptr;
	}

	m_impl->activeEffects.erase(
		std::remove_if(m_impl->activeEffects.begin(), m_impl->activeEffects.end(),
			[](const Impl::ActiveEffect& active) { return active.effect == nullptr; }),
		m_impl->activeEffects.end());
}

EE_NAMESPACE_INPUT_END
