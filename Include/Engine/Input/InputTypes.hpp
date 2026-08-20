#pragma once

#include <Engine/Core/Macros.h>
#include <Engine/Core/Types.hpp>

EE_NAMESPACE_INPUT_BEGIN

/**
 * @brief Keyboard scan codes.
 *
 * Values are kept identical to the OIS backend KeyCode enumeration so that
 * conversion is a simple static_cast. Upper layers never include OIS headers.
 */
enum class KeyCode : UInt32 {
	Unassigned = 0x00,
	Escape = 0x01,
	Num1 = 0x02,
	Num2 = 0x03,
	Num3 = 0x04,
	Num4 = 0x05,
	Num5 = 0x06,
	Num6 = 0x07,
	Num7 = 0x08,
	Num8 = 0x09,
	Num9 = 0x0A,
	Num0 = 0x0B,
	Minus = 0x0C,
	Equals = 0x0D,
	Backspace = 0x0E,
	Tab = 0x0F,
	Q = 0x10,
	W = 0x11,
	E = 0x12,
	R = 0x13,
	T = 0x14,
	Y = 0x15,
	U = 0x16,
	I = 0x17,
	O = 0x18,
	P = 0x19,
	LeftBracket = 0x1A,
	RightBracket = 0x1B,
	Return = 0x1C,
	LeftControl = 0x1D,
	A = 0x1E,
	S = 0x1F,
	D = 0x20,
	F = 0x21,
	G = 0x22,
	H = 0x23,
	J = 0x24,
	K = 0x25,
	L = 0x26,
	Semicolon = 0x27,
	Apostrophe = 0x28,
	Grave = 0x29,
	LeftShift = 0x2A,
	Backslash = 0x2B,
	Z = 0x2C,
	X = 0x2D,
	C = 0x2E,
	V = 0x2F,
	B = 0x30,
	N = 0x31,
	M = 0x32,
	Comma = 0x33,
	Period = 0x34,
	Slash = 0x35,
	RightShift = 0x36,
	Multiply = 0x37,
	LeftAlt = 0x38,
	Space = 0x39,
	Capital = 0x3A,
	F1 = 0x3B,
	F2 = 0x3C,
	F3 = 0x3D,
	F4 = 0x3E,
	F5 = 0x3F,
	F6 = 0x40,
	F7 = 0x41,
	F8 = 0x42,
	F9 = 0x43,
	F10 = 0x44,
	NumLock = 0x45,
	ScrollLock = 0x46,
	Numpad7 = 0x47,
	Numpad8 = 0x48,
	Numpad9 = 0x49,
	Subtract = 0x4A,
	Numpad4 = 0x4B,
	Numpad5 = 0x4C,
	Numpad6 = 0x4D,
	Add = 0x4E,
	Numpad1 = 0x4F,
	Numpad2 = 0x50,
	Numpad3 = 0x51,
	Numpad0 = 0x52,
	Decimal = 0x53,
	Oem102 = 0x56,
	F11 = 0x57,
	F12 = 0x58,
	F13 = 0x64,
	F14 = 0x65,
	F15 = 0x66,
	Kana = 0x70,
	AbntC1 = 0x73,
	Convert = 0x79,
	NoConvert = 0x7B,
	Yen = 0x7D,
	AbntC2 = 0x7E,
	NumpadEquals = 0x8D,
	PrevTrack = 0x90,
	At = 0x91,
	Colon = 0x92,
	Underline = 0x93,
	Kanji = 0x94,
	Stop = 0x95,
	Ax = 0x96,
	Unlabeled = 0x97,
	NextTrack = 0x99,
	NumpadEnter = 0x9C,
	RightControl = 0x9D,
	Mute = 0xA0,
	Calculator = 0xA1,
	PlayPause = 0xA2,
	MediaStop = 0xA4,
	TwoSuperior = 0xAA,
	VolumeDown = 0xAE,
	VolumeUp = 0xB0,
	WebHome = 0xB2,
	NumpadComma = 0xB3,
	Divide = 0xB5,
	SysRq = 0xB7,
	RightAlt = 0xB8,
	Pause = 0xC5,
	Home = 0xC7,
	Up = 0xC8,
	PageUp = 0xC9,
	Left = 0xCB,
	Right = 0xCD,
	End = 0xCF,
	Down = 0xD0,
	PageDown = 0xD1,
	Insert = 0xD2,
	Delete = 0xD3,
	LeftWin = 0xDB,
	RightWin = 0xDC,
	Apps = 0xDD,
	Power = 0xDE,
	Sleep = 0xDF,
	Wake = 0xE3,
	WebSearch = 0xE5,
	WebFavorites = 0xE6,
	WebRefresh = 0xE7,
	WebStop = 0xE8,
	WebForward = 0xE9,
	WebBack = 0xEA,
	MyComputer = 0xEB,
	Mail = 0xEC,
	MediaSelect = 0xED,
	Count = 0xFF,
};

inline constexpr UInt32 KeyCodeCount = static_cast<UInt32>(KeyCode::Count);

/**
 * @brief Mouse button identifiers.
 */
enum class MouseButton : UInt8 {
	Left = 0,
	Right,
	Middle,
	Button3,
	Button4,
	Button5,
	Button6,
	Button7,
	Count,
};

inline constexpr UInt32 MouseButtonCount = static_cast<UInt32>(MouseButton::Count);

/**
 * @brief Gamepad button identifiers.
 */
enum class GamepadButton : UInt8 {
	A = 0,
	B,
	X,
	Y,
	LeftBumper,
	RightBumper,
	Back,
	Start,
	Guide,
	LeftThumb,
	RightThumb,
	DpadUp,
	DpadDown,
	DpadLeft,
	DpadRight,
	Count,
};

inline constexpr UInt32 GamepadButtonCount = static_cast<UInt32>(GamepadButton::Count);

/**
 * @brief Gamepad axis identifiers.
 */
enum class GamepadAxis : UInt8 {
	LeftX = 0,
	LeftY,
	RightX,
	RightY,
	LeftTrigger,
	RightTrigger,
	Count,
};

inline constexpr UInt32 GamepadAxisCount = static_cast<UInt32>(GamepadAxis::Count);

/**
 * @brief Categories of input devices managed by the Input subsystem.
 */
enum class InputDeviceType : UInt8 {
	Keyboard,
	Mouse,
	Gamepad,
};

EE_NAMESPACE_INPUT_END
