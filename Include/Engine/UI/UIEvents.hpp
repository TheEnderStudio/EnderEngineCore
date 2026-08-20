#pragma once

#include <Core/Types.hpp>
#include <Core/Event.hpp>

EE_NAMESPACE_UI_BEGIN

// ===================================================================
// Events
// ===================================================================

/**
 * @brief Emitted per control during the UI draw phase.
 *
 * Set `handled = true` to skip the default drawing for this control.
 */
class UIDrawEvent : public Event {
public:
	UIDrawEvent() = default;
	EE_DEFAULT_COPY(UIDrawEvent)
	EE_DEFAULT_MOVE(UIDrawEvent)
	class Object* control = nullptr;  ///< The control being drawn.
	bool afterPostProcess = false;    ///< Whether 2D renders after post-processing.
	mutable bool handled = false;     ///< Set to true if you handled the drawing (valid even in const& handler).
};

/**
 * @brief Emitted when a control is clicked or released.
 */
class UIClickEvent : public Event {
public:
	UIClickEvent() = default;
	EE_DEFAULT_COPY(UIClickEvent)
	EE_DEFAULT_MOVE(UIClickEvent)
	class Object* control = nullptr;
	bool pressed = false;  ///< true = mouse down, false = mouse up.
};

/**
 * @brief Emitted when mouse hover state changes on a control.
 */
class UIHoverEvent : public Event {
public:
	UIHoverEvent() = default;
	EE_DEFAULT_COPY(UIHoverEvent)
	EE_DEFAULT_MOVE(UIHoverEvent)
	class Object* control = nullptr;
	bool hovered = false;
};

/**
 * @brief Emitted when a UITextInput accepts user input (Enter key).
 */
class UITextSubmitEvent : public Event {
public:
	UITextSubmitEvent() = default;
	EE_DEFAULT_COPY(UITextSubmitEvent)
	EE_DEFAULT_MOVE(UITextSubmitEvent)
	class Object* control = nullptr;
	String     text;  ///< The submitted text value.
};

EE_NAMESPACE_UI_END
