// Input: mouse events from the host to the control under the cursor.
//
// The original (FA exe 0x7a4960, its mouse handler): the event goes to the control that holds the
// input capture, or else to the control under the cursor. That is (0x7872e0) the control with the
// highest Depth among those that are not hidden, have hit testing on and contain the point; of
// equal depths the first in tree order. A control gets the event as a table through its script's
// HandleEvent(self, event) (0x7873a0, table made at 0x795bd0); when that returns false the parent
// gets it, and so on up (0x787370).
#pragma once
#include "script/lua.hpp"

namespace moho::ui {

class Control;
class UiManager;

// The original's event types (EMauiEventType); scripts see the name without "MET_".
enum class EventType {
  MouseMotion = 1, MouseEnter, MouseHover, MouseExit, ButtonPress, ButtonDClick, ButtonRelease, WheelRotation,
  KeyUp, KeyDown, Char,
};

struct UiEvent {
  EventType type = EventType::MouseMotion;
  float x = 0, y = 0;
  int wheelRotation = 0, wheelDelta = 0;
  int keyCode = 0, rawKeyCode = 0;
  bool shift = false, ctrl = false, alt = false;
  bool left = false, middle = false, right = false;
  bool repeat = false;  // a key down that comes from the key being held
};

// self:HandleEvent(event) on one control: true when the script handled it.
bool SendEvent(lua_State* L, Control* c, const UiEvent& e);
// The control first, then its parents until one handles the event.
void PostEvent(lua_State* L, Control* c, const UiEvent& e);
// The control an event at (x, y) goes to on a head: nullptr when there is none.
Control* ControlAt(UiManager& ui, int head, float x, float y);

// What the host reports. `e` carries the position and the modifier and button states; its type is set here.
void MouseMove(lua_State* L, UiManager& ui, UiEvent e);
void MouseButton(lua_State* L, UiManager& ui, UiEvent e, bool pressed, bool doubleClick);
void MouseWheel(lua_State* L, UiManager& ui, UiEvent e);

// A key event: `e` carries its type (KeyDown, KeyUp or Char), the key code and the modifiers.
// It goes (FA exe 0x7a4ef0 key down, 0x7a4fd0 key up, 0x7a50b0 character) to the control with the
// keyboard focus alone, not on to its parents; without one, to the control on top of the input
// capture stack. Returns false when nothing took the key, which is when the original lets its
// window system go on with it: a key down nobody handled becomes a character event.
//
// Key codes are the original's (those of its window library): characters by their code, letters
// in a key down or up as capitals, 8 Backspace, 9 Tab, 13 Enter, 27 Escape, 127 Delete, and from
// 300 the keys without a character (312 PageUp, 313 PageDown, 314 End, 315 Home, 316 Left, 317 Up,
// 318 Right, 319 Down, 324 Insert, 342.. F1..). A character typed with Ctrl arrives as its
// control code (Ctrl+C is 3).
bool KeyEvent(lua_State* L, UiManager& ui, UiEvent e);
// One whole key press as a host makes it: key down; when nothing handled it, the character;
// key up. `charCode` is the character, or the key code of a key without one.
void KeyPress(lua_State* L, UiManager& ui, int charCode, bool shift, bool ctrl, bool alt);
// The event as scripts get it, pushed on the stack (FA exe 0x795bd0).
void PushEventTable(lua_State* L, Control* c, const UiEvent& e);
// The host's window came to the front or left it. The original's IsKeyDown (MAUI_KeyIsDown 0x79cb70, READ) says no
// key is held while its window is not the foreground one (WindowIsForeground 0x79cb10: GetForegroundWindow against
// the heads' windows) and else asks Windows (GetKeyState, 0x79cba8), so a key let go in another window is not held
// when the player comes back. Ours keeps the states from the key events, and a window in the back gets no key up:
// leaving the front, every key held is let go (KeyMapAllKeysUp), and until it is back IsKeyDown says no. INFERRED: a
// key still held when the window comes back counts from its next key down (its repeats), where the original's
// GetKeyState would have it at once.
void WindowFocus(lua_State* L, UiManager& ui, bool inFront);

// The host's own debugging functions for tests (not engine functions of the original):
// moho64ui_MouseMove(x, y), moho64ui_MouseButton(button, pressed, x, y), moho64ui_ControlAt(x, y),
// moho64ui_Key(code [, "Shift Ctrl Alt"]), moho64ui_Type(text), moho64ui_WindowFocus(inFront).
void RegisterInputBindings(lua_State* L);

}  // namespace moho::ui
