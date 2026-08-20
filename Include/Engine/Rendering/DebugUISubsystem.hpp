#pragma once

#include <Core/Subsystem.hpp>
#include "RenderTypes.hpp"
#include "DebugUIEvents.hpp"
#include <spdlog/common.h>

EE_NAMESPACE_RENDERING_BEGIN

/**
 * @brief Debug UI subsystem backed by ImGui.
 *
 * Only active in EE_DEBUG builds. All ImGui calls happen inside the DLL
 * to avoid ODR violations.
 *
 * Built-in panel: setDebugData() provides pointers for the PostProcess panel.
 *
 * Custom widgets: subscribe to DebugUIRenderEvent with a registered Object.
 * In the event handler, use the ImGui wrapper methods (beginWindow, sliderFloat,
 * etc.) — these call through to ImGui inside the DLL where the context is valid.
 *
 * Lifecycle:
 *   1. Create, setDevice/setContext, initialize()
 *   2. Register an Object, subscribe<DebugUIRenderEvent>(obj, handler)
 *   3. In render loop: beginFrame() (emits event) → draw → endFrameAndRender()
 */
class EE_API DebugUISubsystem : public Subsystem {
public:
	/// @brief Default constructor.
	DebugUISubsystem();
	/// @brief Destructor.
	~DebugUISubsystem() override;

	EE_NO_COPY(DebugUISubsystem)
	EE_NO_MOVE(DebugUISubsystem)

	/// @brief Set the Diligent device pointer for ImGui initialization.
	void setDevice(DevicePtr device);
	/// @brief Set the Diligent immediate context pointer for ImGui initialization.
	void setContext(ContextPtr context);
	/// @brief Set the native window handle for ImGui input.
	void setWindowHandle(void* hwnd);

	/**
	 * @brief Set data pointers for the built-in post-process debug panel.
	 * @param ppCfg            Pointer to PostProcessConfig.
	 * @param custom           Pointer to bool for CRT shader toggle.
	 * @param customShaderSrc  Pointer to custom shader source string pointer.
	 */
	void setDebugData(void* ppCfg, bool* custom, const char** customShaderSrc);

	// ---------------------------------------------------------------
	// ImGui wrapper API (call between beginFrame and endFrameAndRender)
	// ---------------------------------------------------------------

	/// @brief Begin an ImGui child window.
	bool beginWindow(const char* name, bool* open = nullptr);
	/// @brief End the current ImGui child window.
	void endWindow();
	/// @brief Display raw text in the current ImGui window.
	void textImpl(const char* str);

	/**
	 * @brief Display formatted text (fmt-style).
	 * @tparam Args Format argument types.
	 * @param fmt  Format string (fmtlib syntax).
	 * @param args Format arguments.
	 */
	template <typename... Args>
	void text(const spdlog::format_string_t<Args...>& fmt, Args&&... args) {
		String str = fmt::format(fmt, std::forward<Args>(args)...);
		textImpl(str.c_str());
	}
	/// @brief Display an ImGui button.
	bool button(const char* label);
	/// @brief Display an ImGui float slider.
	bool sliderFloat(const char* label, float* v, float min, float max);
	/// @brief Display an ImGui checkbox.
	bool checkbox(const char* label, bool* v);

	// ---------------------------------------------------------------
	// Frame lifecycle
	// ---------------------------------------------------------------

	/**
	 * @brief Begin a new ImGui frame.
	 *
	 * Calls ImGui::NewFrame(), renders the built-in panel, then emits
	 * DebugUIRenderEvent for subscribed objects to draw custom widgets.
	 */
	void beginFrame(UInt32 width, UInt32 height);

	/**
	 * @brief End the ImGui frame and render to the current target.
	 */
	void endFrameAndRender();

	/**
	 * @brief Resize the ImGui display area (call on window resize).
	 */
	void resize(UInt32 width, UInt32 height);

	/**
	 * @brief Feed mouse input (must be in window-client coordinates).
	 */
	void setMousePos(float x, float y);
	/// @brief Set mouse button state for ImGui input.
	void setMouseButton(Int32 button, bool down);
	/// @brief Set mouse wheel delta for ImGui input.
	void setMouseWheel(float delta);

	/// @brief Check if the debug UI is currently active.
	bool isActive() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
