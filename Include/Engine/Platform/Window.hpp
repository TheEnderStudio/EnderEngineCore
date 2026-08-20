#pragma once

#include "Errors.hpp"
#include <Engine/Core/Types.hpp>

// Forward-declare GLFW types
struct GLFWwindow;
struct GLFWmonitor;

EE_NAMESPACE_PLATFORM_BEGIN

/**
 * @brief Window display mode.
 */
enum class WindowMode : UInt8 {
	/// Normal windowed mode with decorations.
	Windowed,
	/// Borderless window covering the entire monitor (no exclusive fullscreen).
	BorderlessFullscreen,
	/// Exclusive fullscreen mode (changes display mode).
	ExclusiveFullscreen,
};

/**
 * @brief Window creation descriptor.
 */
struct WindowDesc {
	String  title = "EnderEngine";
	UInt32  width = 1280;
	UInt32  height = 720;
	bool    resizable = true;
};

/**
 * @brief Platform window abstraction (wraps GLFW).
 *
 * Supports:
 *   - Window creation & destruction
 *   - Three display modes: windowed / borderless fullscreen / exclusive fullscreen
 *   - Mouse cursor visibility toggle
 *   - Framebuffer size and window position queries
 *   - Native window handle access (for rendering backend)
 */
class EE_API Window {
public:
	Window();
	~Window();

	EE_NO_COPY(Window)
	EE_NO_MOVE(Window)

	/**
	 * @brief Create the window.
	 * @param desc Window creation parameters.
	 * @return Result indicating success or failure.
	 */
	Result<void, PlatformError> open(const WindowDesc& desc = {});

	/**
	 * @brief Close and destroy the window.
	 */
	void close();

	/**
	 * @brief Check whether the window is currently open.
	 */
	bool isOpen() const;

	/**
	 * @brief Check whether the window should close (e.g., user clicked X).
	 */
	bool shouldClose() const;

	/**
	 * @brief Poll pending window events.
	 */
	void pollEvents();

	// -------------------------------------------------------------------
	// Display mode
	// -------------------------------------------------------------------

	/**
	 * @brief Get the current display mode.
	 */
	WindowMode mode() const;

	/**
	 * @brief Switch to the specified display mode.
	 *
	 * Windowed <-> BorderlessFullscreen: preserves position/size.
	 * Windowed <-> ExclusiveFullscreen: changes video mode.
	 * @param m Target mode.
	 */
	void setMode(WindowMode m);

	// -------------------------------------------------------------------
	// Mouse cursor
	// -------------------------------------------------------------------

	/**
	 * @brief Show or hide the mouse cursor.
	 * @param visible true to show, false to hide.
	 */
	void setCursorVisible(bool visible);

	/**
	 * @brief Check whether the mouse cursor is visible.
	 */
	bool isCursorVisible() const;

	/// @brief Get the cursor position in window coordinates.
	Vec2 getCursorPos() const;

	/// @brief Check whether the specified mouse button is currently pressed.
	bool isMouseButtonDown(UInt8 type) const;

	// -------------------------------------------------------------------
	// Queries
	// -------------------------------------------------------------------

	/**
	 * @brief Get the framebuffer size in pixels.
	 */
	void getFramebufferSize(UInt32& outWidth, UInt32& outHeight) const;

	/**
	 * @brief Get the window size in screen coordinates.
	 */
	void getWindowSize(UInt32& outWidth, UInt32& outHeight) const;

	/**
	 * @brief Get the window position.
	 */
	void getWindowPos(Int32& outX, Int32& outY) const;

	/**
	 * @brief Set the window title.
	 */
	void setTitle(const String& title);

	/**
	 * @brief Get the native window handle (HWND on Windows).
	 */
	void* nativeHandle() const;

	/**
	 * @brief Get the underlying GLFW window pointer (for RenderSubsystem).
	 */
	void* glfwWindow() const;

private:
	void saveWindowedState();
	void restoreWindowedState();
	void applyWindowed();
	void applyBorderless();
	void applyExclusiveFullscreen();

	GLFWwindow* m_window = nullptr;
	WindowMode m_mode = WindowMode::Windowed;
	bool m_cursorVisible = true;

	// Saved windowed state (for restore)
	Int32  m_savedX = 0, m_savedY = 0;
	UInt32 m_savedW = 1280, m_savedH = 720;

	WindowDesc m_desc;
};

EE_NAMESPACE_PLATFORM_END
