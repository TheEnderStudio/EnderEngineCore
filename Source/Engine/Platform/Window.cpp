#include <Platform/Window.hpp>
#include <Core/Log.hpp>
#include <Core/Crash.h>

#include <GLFW/glfw3.h>
#ifdef EE_WINDOWS
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#	include <Windows.h>
#endif

EE_NAMESPACE_PLATFORM_BEGIN

static bool s_glfwInitialized = false;

Window::Window() = default;

Window::~Window() {
	close();
}

Result<void, PlatformError> Window::open(const WindowDesc& desc) {
	if (m_window) return PlatformError::AlreadyInitialized;

	if (!s_glfwInitialized) {
		if (!glfwInit()) return PlatformError::InitFailed;
		s_glfwInitialized = true;
	}

	m_desc = desc;
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

	m_window = glfwCreateWindow((int)desc.width, (int)desc.height, desc.title.c_str(), nullptr, nullptr);
	if (!m_window) return PlatformError::WindowCreationFailed;

	m_savedW = desc.width;
	m_savedH = desc.height;

	EInfo("Window created: {} ({}x{})", desc.title, desc.width, desc.height);
	return {};
}

void Window::close() {
	if (m_window) {
		glfwDestroyWindow(m_window);
		m_window = nullptr;
	}
}

bool Window::isOpen() const { return m_window != nullptr; }

bool Window::shouldClose() const { return m_window && glfwWindowShouldClose(m_window); }

void Window::pollEvents() { glfwPollEvents(); }

WindowMode Window::mode() const { return m_mode; }

void Window::setMode(WindowMode m) {
	if (!m_window || m == m_mode) return;

	if (m_mode == WindowMode::Windowed)
		saveWindowedState();

	switch (m) {
	case WindowMode::Windowed:          applyWindowed(); break;
	case WindowMode::BorderlessFullscreen: applyBorderless(); break;
	case WindowMode::ExclusiveFullscreen:  applyExclusiveFullscreen(); break;
	}

	m_mode = m;
}

void Window::setCursorVisible(bool visible) {
	if (!m_window) return;
	m_cursorVisible = visible;
#ifdef EE_WINDOWS
	if (visible) { while (ShowCursor(TRUE) < 0) {} }
	else { while (ShowCursor(FALSE) >= 0) {} }
#else
	glfwSetInputMode(m_window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
#endif
}

bool Window::isCursorVisible() const {
	if (!m_window) return true;
#ifdef EE_WINDOWS
	// Win32 ShowCursor doesn't have a direct query; track our own state
	return m_cursorVisible;
#else
	return glfwGetInputMode(m_window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED;
#endif
}

Vec2 Window::getCursorPos() const {
	double x, y;
	glfwGetCursorPos(m_window, &x, &y);
	return Vec2((float)x, (float)y);
}

bool Window::isMouseButtonDown(UInt8 type) const {
	switch (type)
	{
	case 0:
		return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
	case 1:
		return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
	case 2:
		return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
	default:
		EError("Wrong parameter 'type': {}, requires 0 for left button, 1 for middle, 2 for right.", type);
		return false;
	}
}

void Window::getFramebufferSize(UInt32& w, UInt32& h) const {
	if (!m_window) { w = 0; h = 0; return; }
	int fw, fh; glfwGetFramebufferSize(m_window, &fw, &fh);
	w = (UInt32)fw; h = (UInt32)fh;
}

void Window::getWindowSize(UInt32& w, UInt32& h) const {
	if (!m_window) { w = 0; h = 0; return; }
	int iw, ih; glfwGetWindowSize(m_window, &iw, &ih);
	w = (UInt32)iw; h = (UInt32)ih;
}

void Window::getWindowPos(Int32& x, Int32& y) const {
	if (!m_window) { x = 0; y = 0; return; }
	glfwGetWindowPos(m_window, &x, &y);
}

void Window::setTitle(const String& title) {
	if (m_window) glfwSetWindowTitle(m_window, title.c_str());
}

void* Window::nativeHandle() const {
	if (!m_window) return nullptr;
#ifdef EE_WINDOWS
	return glfwGetWin32Window(m_window);
#else
	return nullptr;
#endif
}

void* Window::glfwWindow() const { return m_window; }

// -------------------------------------------------------------------
// Internal mode switching
// -------------------------------------------------------------------

void Window::saveWindowedState() {
	glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
	int w, h; glfwGetWindowSize(m_window, &w, &h);
	m_savedW = (UInt32)w; m_savedH = (UInt32)h;
}

void Window::restoreWindowedState() {
	glfwSetWindowAttrib(m_window, GLFW_DECORATED, true);
	glfwSetWindowMonitor(m_window, nullptr, m_savedX, m_savedY, (int)m_savedW, (int)m_savedH, GLFW_DONT_CARE);
}

void Window::applyWindowed() {
	restoreWindowedState();
}

void Window::applyBorderless() {
	GLFWmonitor* mon = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(mon);
	glfwSetWindowAttrib(m_window, GLFW_DECORATED, false);
	glfwSetWindowMonitor(m_window, nullptr, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);
}

void Window::applyExclusiveFullscreen() {
	GLFWmonitor* mon = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(mon);
	glfwSetWindowMonitor(m_window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
}

EE_NAMESPACE_PLATFORM_END
