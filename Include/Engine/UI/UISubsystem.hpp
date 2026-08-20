#pragma once

#include <Core/Subsystem.hpp>
#include "UITypes.hpp"
#include "UIEvents.hpp"
#include <Rendering/Render2DSubsystem.hpp>

EE_NAMESPACE_UI_BEGIN

/// @brief Manages the UI layer stack, input routing, and 2D rendering via Render2DSubsystem.
class EE_API UISubsystem : public Subsystem {
public:
	/// @brief Construct a UISubsystem. Call initialize() before use.
	UISubsystem();
	/// @brief Destroy the UISubsystem and release resources.
	~UISubsystem() override;

	EE_NO_COPY(UISubsystem)
	EE_NO_MOVE(UISubsystem)

	/// @brief Initialize the UI subsystem with a 2D renderer and screen dimensions.
	void initialize(Rendering::Render2DSubsystem* r2d, UInt32 screenW, UInt32 screenH);
	/// @brief Update the logical screen size for layout calculations.
	void setScreenSize(UInt32 w, UInt32 h);

	// ---------------------------------------------------------------
	// Layer stack
	// ---------------------------------------------------------------

	/**
	 * @brief Push a layer onto the stack. Its controls are auto-registered.
	 */
	void pushLayer(const UILayer& layer);

	/**
	 * @brief Pop the top layer. Its controls are auto-unregistered.
	 */
	void popLayer();

	/// @brief Push a scissor mask rect. Subsequent UI draws are clipped to this region.
	void pushMask(Vec2 pos, Vec2 size);

	/// @brief Pop the topmost mask, restoring the previous clip region.
	void popMask();

	/**
	 * @brief Get the topmost layer, or nullptr if stack is empty.
	 */
	UILayer* topLayer();

	// ---------------------------------------------------------------
	// Object-Subsystem-Event
	// ---------------------------------------------------------------
	/// @brief Register a control with the subsystem for event delivery and rendering.
	Result<void, CoreError> registerControl(UIControl* control);
	/// @brief Unregister a previously registered control.
	void unregisterControl(UIControl* control);

	// ---------------------------------------------------------------
	// Frame lifecycle
	// ---------------------------------------------------------------

	/**
	 * @brief Begin UI frame. Processes input only for the top visible layer.
	 */
	void beginFrame(bool afterPostProcess = true);

	/**
	 * @brief End UI frame. Draws all visible layers bottom-to-top.
	 */
	void endFrame();

	// ---------------------------------------------------------------
	// Mouse input
	// ---------------------------------------------------------------

	/// @brief Set the current mouse position in screen coordinates.
	void setMousePos(float x, float y);
	/// @brief Set whether the left mouse button is currently pressed.
	void setMouseDown(bool down);

	// ---------------------------------------------------------------
	// Text input (routed to focused UITextInput on the top layer)
	// ---------------------------------------------------------------

	/// @brief Insert a character at the current cursor position.
	void inputChar(char c);
	/// @brief Delete the character before the cursor.
	void inputBackspace();
	/// @brief Delete the character after the cursor.
	void inputDelete();
	/// @brief Move the cursor one character left.
	void inputCursorLeft();
	/// @brief Move the cursor one character right.
	void inputCursorRight();
	/// @brief Submit the current text input (e.g. Enter key).
	void inputSubmit();

	/**
	 * @brief Get the currently focused text input, or nullptr.
	 */
	UITextInput* focusedTextInput() const;

	/**
	 * @brief Programmatically set focus to a text input (pass nullptr to clear).
	 */
	void focusTextInput(UITextInput* ti);

	/**
	 * @brief Set frame delta time for cursor blink animation.
	 */
	void setDeltaTime(F32 dt);

	/**
	 * @brief Register a crosshair control for rendering at screen center.
	 */
	void setCrosshair(UICrosshair* ch);

	/**
	 * @brief Set the crosshair appearance.
	 */
	void setCrosshairStyle(CrosshairStyle style);

	// ---------------------------------------------------------------
	// Legacy convenience API
	// ---------------------------------------------------------------

	/// @brief Draw a filled rectangle from a descriptor. Legacy convenience helper.
	void drawRect(const UIElementDesc& desc);
	/// @brief Draw a button and return true if clicked this frame. Legacy convenience helper.
	bool drawButton(const UIElementDesc& desc, const String& label);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_UI_END
