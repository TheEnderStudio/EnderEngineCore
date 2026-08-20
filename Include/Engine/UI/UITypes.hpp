#pragma once

#include <Core/Types.hpp>
#include <Core/Object.hpp>

EE_NAMESPACE_UI_BEGIN

/// @brief Screen-space anchor point for UI element positioning.
enum class UIAnchor : UInt8 {
	TopLeft, TopCenter, TopRight,
	MidLeft, Center, MidRight,
	BotLeft, BotCenter, BotRight,
};

/// @brief Descriptor for a UI element's layout, color, and visibility.
struct UIElementDesc {
	Vec2  position = Vec2(0);
	Vec2  size     = Vec2(100);
	Vec4  color    = Vec4(1);
	UIAnchor anchor = UIAnchor::TopLeft;
	bool  visible = true;
	bool  afterPostProcess = false;
};

/**
 * @brief Base class for UI controls (inherits Object for Event registration).
 */
class UIControl : public Object {
public:
	/// @brief Enum of supported UI control types.
	enum class Type : UInt8 { Rect, Button, Label, Picture, TextInput, Crosshair };
	Type ctrlType = Type::Rect;    ///< The type of this control.
	UIElementDesc desc;            ///< Layout and style descriptor.
};

/// @brief A simple filled rectangle UI control.
class UIRect : public UIControl { public: UIRect() { ctrlType = Type::Rect; } };
/// @brief A clickable button UI control with a text label.
class UIButton : public UIControl {
public:
	UIButton() { ctrlType = Type::Button; }
	String label;
	void* fontSRV = nullptr;     ///< Font atlas texture SRV for label rendering.
	void* fontData = nullptr;    ///< Pointer to FontData for glyph metrics.
};

/**
 * @brief A text label using a loaded font.
 */
class UILabel : public UIControl {
public:
	UILabel() { ctrlType = Type::Label; }
	String text;                 ///< Display text.
	void* fontSRV = nullptr;     ///< Font atlas texture SRV.
	void* fontData = nullptr;    ///< Pointer to FontData for glyph metrics.
	F32   fontSize = 16.0f;      ///< Font size.
};

/**
 * @brief A picture/image control.
 */
class UIPicture : public UIControl {
public:
	UIPicture() { ctrlType = Type::Picture; }
	void* textureSRV = nullptr;  ///< Diligent ITextureView*.
};

/**
 * @brief A simple single-line text input (English only).
 */
class UITextInput : public UIControl {
public:
	UITextInput() { ctrlType = Type::TextInput; }
	String  buffer;              ///< Current text content.
	size_t  cursorPos = 0;        ///< Cursor position in bytes (English-only, 1 byte = 1 char).
	void*   fontSRV = nullptr;    ///< Font atlas texture SRV.
	void*   fontData = nullptr;   ///< Pointer to FontData for glyph metrics.
	F32     fontSize = 20.0f;
	bool    focused = false;      ///< Set by click; cleared when clicking elsewhere.
	F32     cursorBlink = 0.1f;   ///< Internal blink timer (seconds).
	Vec4    textColor = Vec4(1);   ///< Color of the text.
	Vec4    borderFocused = Vec4(1);     ///< Border color when focused (default white).
	Vec4    borderUnfocused = Vec4(0.3f,0.3f,0.3f,1); ///< Border color when unfocused (default dark gray).
	F32     borderWidth = 2.0f;          ///< Border thickness in pixels.
	F32     padding = 4.0f;        ///< Horizontal padding inside the input rect.
};

/// @brief Crosshair draw style.
enum class CrosshairStyle : UInt8 { None, Filled, Outline };

/// @brief A crosshair indicator drawn at screen center.
class UICrosshair : public UIControl {
public:
	UICrosshair() { ctrlType = Type::Crosshair; }
	CrosshairStyle style = CrosshairStyle::Outline;
	F32     radius    = 8.0f;
	F32     thickness = 2.0f;
};

// ===================================================================
// Layer stack
// ===================================================================

/**
 * @brief A UI layer — a drawable container with background color and ordered controls.
 *
 * Layers are stacked in z-order within UISubsystem. Each layer can have
 * a background color and holds its own list of registered controls.
 */
class UILayer : public Object {
public:
	String   name;                    ///< Layer display name.
	Vec4     bgColor = Vec4(0, 0, 0, 0); ///< Background (transparent by default).
	bool     visible = true;          ///< Whether this layer is drawn.
	bool     afterPostProcess = false; ///< Draw after post-processing pass.

	/**
	 * @brief Add a control to this layer.
	 */
	void addControl(UIControl* ctrl) { controls.push_back(ctrl); }

	/**
	 * @brief Remove a control from this layer.
	 */
	void removeControl(UIControl* ctrl) {
		auto it = std::find(controls.begin(), controls.end(), ctrl);
		if (it != controls.end()) controls.erase(it);
	}

	/**
	 * @brief Get all controls in this layer.
	 */
	const Vector<UIControl*>& getControls() const { return controls; }

private:
	Vector<UIControl*> controls;
};

EE_NAMESPACE_UI_END
