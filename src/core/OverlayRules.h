/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

namespace ts4_mod::overlay
{

/// What changed in the visible state of the menu bar in one frame.
struct VisibilityChange
{
	bool visible = false; ///< The menu bar shows in this frame.
	bool shown = false;   ///< The menu bar did not show in the last frame.
	bool hidden = false;  ///< The menu bar showed in the last frame and does not show now.

	friend bool operator==(const VisibilityChange&, const VisibilityChange&) = default;
};

/// When the menu bar shows. It shows while the menu of the game is open, or while the user pins it with the hotkey.
/// The main thread calls `Update` one time in each frame.
class OverlayVisibility
{
public:
	/// Calculate the state of this frame. `menuOpen` tells if the menu of the game is open. `togglePin` is true in the
	/// frame where the user pressed the hotkey of the menu bar.
	VisibilityChange Update(bool menuOpen, bool togglePin)
	{
		if (togglePin)
		{
			_pinned = !_pinned;
		}
		const bool visible = _enabled && (menuOpen || _pinned);
		const VisibilityChange change {.visible = visible, .shown = visible && !_visible, .hidden = !visible && _visible};
		_visible = visible;
		return change;
	}

	/// Stop the menu bar, for example after an error of the renderer. The next `Update` hides it.
	void SetEnabled(bool enabled) { _enabled = enabled; }

	[[nodiscard]] bool Visible() const { return _visible; }
	[[nodiscard]] bool Pinned() const { return _pinned; }
	[[nodiscard]] bool Enabled() const { return _enabled; }

private:
	bool _enabled = true;
	bool _pinned = false;
	bool _visible = false;
};

/// The kinds of window messages that the menu bar must know about.
enum class InputKind
{
	MouseMove,
	MouseButtonDown,
	MouseButtonUp,
	MouseWheel,
	KeyDown,
	KeyUp,
	Character,
	RawInput,
	Focus,
	Cursor, ///< The window asks for the shape of the cursor. Only the game sets it.
	Other,
};

/// The values of the Windows messages that `ClassifyMessage` knows. The Windows ABI fixes these values.
namespace wm
{
constexpr uint32_t k_SetFocus = 0x0007;
constexpr uint32_t k_KillFocus = 0x0008;
constexpr uint32_t k_SetCursor = 0x0020;
constexpr uint32_t k_ActivateApp = 0x001C;
constexpr uint32_t k_Input = 0x00FF;
constexpr uint32_t k_KeyDown = 0x0100;
constexpr uint32_t k_KeyUp = 0x0101;
constexpr uint32_t k_Char = 0x0102;
constexpr uint32_t k_SysKeyDown = 0x0104;
constexpr uint32_t k_SysKeyUp = 0x0105;
constexpr uint32_t k_MouseMove = 0x0200;
constexpr uint32_t k_LeftButtonDown = 0x0201;
constexpr uint32_t k_LeftButtonUp = 0x0202;
constexpr uint32_t k_LeftButtonDoubleClick = 0x0203;
constexpr uint32_t k_RightButtonDown = 0x0204;
constexpr uint32_t k_RightButtonUp = 0x0205;
constexpr uint32_t k_RightButtonDoubleClick = 0x0206;
constexpr uint32_t k_MiddleButtonDown = 0x0207;
constexpr uint32_t k_MiddleButtonUp = 0x0208;
constexpr uint32_t k_MiddleButtonDoubleClick = 0x0209;
constexpr uint32_t k_MouseWheel = 0x020A;
constexpr uint32_t k_ExtraButtonDown = 0x020B;
constexpr uint32_t k_ExtraButtonUp = 0x020C;
constexpr uint32_t k_ExtraButtonDoubleClick = 0x020D;
constexpr uint32_t k_MouseHorizontalWheel = 0x020E;
constexpr uint32_t k_MouseLeave = 0x02A3;
constexpr uint32_t k_EscapeKey = 0x1B;
} // namespace wm

/// Find the kind of a window message.
[[nodiscard]] constexpr InputKind ClassifyMessage(uint32_t message)
{
	switch (message)
	{
	case wm::k_MouseMove:
	case wm::k_MouseLeave:
		return InputKind::MouseMove;
	case wm::k_LeftButtonDown:
	case wm::k_LeftButtonDoubleClick:
	case wm::k_RightButtonDown:
	case wm::k_RightButtonDoubleClick:
	case wm::k_MiddleButtonDown:
	case wm::k_MiddleButtonDoubleClick:
	case wm::k_ExtraButtonDown:
	case wm::k_ExtraButtonDoubleClick:
		return InputKind::MouseButtonDown;
	case wm::k_LeftButtonUp:
	case wm::k_RightButtonUp:
	case wm::k_MiddleButtonUp:
	case wm::k_ExtraButtonUp:
		return InputKind::MouseButtonUp;
	case wm::k_MouseWheel:
	case wm::k_MouseHorizontalWheel:
		return InputKind::MouseWheel;
	case wm::k_KeyDown:
	case wm::k_SysKeyDown:
		return InputKind::KeyDown;
	case wm::k_KeyUp:
	case wm::k_SysKeyUp:
		return InputKind::KeyUp;
	case wm::k_Char:
		return InputKind::Character;
	case wm::k_Input:
		return InputKind::RawInput;
	case wm::k_SetFocus:
	case wm::k_KillFocus:
	case wm::k_ActivateApp:
		return InputKind::Focus;
	case wm::k_SetCursor:
		return InputKind::Cursor;
	default:
		return InputKind::Other;
	}
}

/// What the window procedure of the menu bar does with one message.
struct InputDecision
{
	bool feedOverlay = false; ///< Give the message to Dear ImGui.
	bool swallow = false;     ///< Do not give the message to the game.

	friend bool operator==(const InputDecision&, const InputDecision&) = default;
};

/// The rules of the input:
/// - While the menu bar does not show, the game gets every message and Dear ImGui gets none.
/// - While it shows, Dear ImGui gets every message.
/// - The game does not get a mouse move, a button press, a wheel turn or raw input while the mouse is on the menu bar.
/// - The game does not get a key press or a character while a control of the menu bar has the keyboard. The Escape
///   key always goes to the game, so that the game can close its menu.
/// - The game always gets the release of a button or of a key, and the focus messages. Thus no key or button of the
///   game stays down.
/// - Only the game gets the message that sets the shape of the cursor. The bar never shows the cursor of Windows; the
///   cursor that the game shows is the only cursor.
[[nodiscard]] constexpr InputDecision DecideInput(InputKind kind, uint64_t key, bool visible, bool wantMouse, bool wantKeyboard)
{
	if (!visible || kind == InputKind::Cursor)
	{
		return {};
	}
	InputDecision decision {.feedOverlay = true};
	switch (kind)
	{
	case InputKind::MouseMove:
	case InputKind::MouseButtonDown:
	case InputKind::MouseWheel:
	case InputKind::RawInput:
		decision.swallow = wantMouse;
		break;
	case InputKind::KeyDown:
	case InputKind::Character:
		decision.swallow = wantKeyboard && key != wm::k_EscapeKey;
		break;
	case InputKind::MouseButtonUp:
	case InputKind::KeyUp:
	case InputKind::Focus:
	case InputKind::Cursor:
	case InputKind::Other:
		break;
	}
	return decision;
}

} // namespace ts4_mod::overlay
