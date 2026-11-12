#include "maui/input.h"

#include <string>
#include <vector>

#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

const char* TypeName(EventType t) {
  static const char* const kNames[] = {"",            "MouseMotion",  "MouseEnter",    "MouseHover",    "MouseExit", "ButtonPress",
                                       "ButtonDClick", "ButtonRelease", "WheelRotation", "KeyUp",         "KeyDown",   "Char"};
  return kNames[static_cast<int>(t)];
}

void SetBool(lua_State* L, const char* key, bool v) {
  lua_pushstring(L, key);
  lua_pushboolean(L, v);
  lua_rawset(L, -3);
}
void SetNumber(lua_State* L, const char* key, lua_Number v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}

// The event table (FA exe 0x795bd0).
void PushEvent(lua_State* L, Control* c, const UiEvent& e) {
  lua_newtable(L);
  lua_pushstring(L, "Type");
  lua_pushstring(L, TypeName(e.type));
  lua_rawset(L, -3);
  SetNumber(L, "MouseX", e.x);
  SetNumber(L, "MouseY", e.y);
  SetNumber(L, "WheelRotation", static_cast<lua_Number>(e.wheelRotation));
  SetNumber(L, "WheelDelta", static_cast<lua_Number>(e.wheelDelta));
  SetNumber(L, "KeyCode", static_cast<lua_Number>(e.keyCode));
  SetNumber(L, "RawKeyCode", static_cast<lua_Number>(e.rawKeyCode));
  lua_pushstring(L, "Modifiers");
  lua_newtable(L);
  SetBool(L, "Shift", e.shift);
  SetBool(L, "Ctrl", e.ctrl);
  SetBool(L, "Alt", e.alt);
  SetBool(L, "Left", e.left);
  SetBool(L, "Middle", e.middle);
  SetBool(L, "Right", e.right);
  lua_rawset(L, -3);
  lua_pushstring(L, "Control");
  PushObject(L, c);
  lua_rawset(L, -3);
}

void Collect(Control* c, std::vector<Control*>& out) {
  out.push_back(c);
  for (Control* k : c->children()) Collect(k, out);
}

Control* TopmostIn(Control* root, float x, float y);

// Where a mouse event goes (FA exe 0x7a4a77..0x7a4b12). Without input capture: the control under
// the cursor on the frame of the head the event is on (the event mapper's own root frame, [mapper+0x2c] at 0x7a4ab2;
// INFERRED: one mapper per head window, so head 1's window hits head 1's frame in its own pixels). With it: the
// control under the cursor among the capturing control and everything below it, and when the cursor is over none of
// those, the capturing control itself; the capture stack is one for every head (0x10bdbac, 0x7a4a77).
Control* Target(UiManager& ui, const UiEvent& e) {
  if (!ui.inputCapture.empty()) {
    // (ours: the re-test panel under the cursor comes before the capture, control.h overCapture; head 0 only, where it is)
    if (ui.overCapture && e.head == 0)
      if (Control* own = TopmostIn(ui.overCapture, e.x, e.y)) return own;
    Control* capture = ui.inputCapture.back();
    Control* hit = TopmostIn(capture, e.x, e.y);
    return hit ? hit : capture;
  }
  return ControlAt(ui, e.head, e.x, e.y);
}

// The cursor moved onto another control: the old one hears MouseExit, the new one MouseEnter.
// (Read as structure from 0x7a4b7d..0x7a4bc6; which of the two comes first is inferred.)
void UpdateHover(lua_State* L, UiManager& ui, Control* target, UiEvent e) {
  if (target == ui.hover) return;
  Control* old = ui.hover;
  ui.hover = target;
  if (old && !old->dead()) {
    e.type = EventType::MouseExit;
    PostEvent(L, old, e);
  }
  if (target && ui.hover == target && !target->dead()) {
    e.type = EventType::MouseEnter;
    PostEvent(L, target, e);
  }
}

// uimain.OnMouseButtonPress({Type =, x =, y =}) (FA exe 0x7a4d3d): every button press, before the
// control hears of it. Inferred: the two numbers are named x and y, as the script reads them.
void CallOnMouseButtonPress(lua_State* L, const UiEvent& e) {
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/uimain.lua");
  bool ok = lua_pcall(L, 1, 1, top + 1) == 0;
  if (ok) {
    lua_pushstring(L, "OnMouseButtonPress");
    lua_gettable(L, -2);
    lua_newtable(L);
    lua_pushstring(L, "Type");
    lua_pushstring(L, TypeName(e.type));
    lua_rawset(L, -3);
    SetNumber(L, "x", e.x);
    SetNumber(L, "y", e.y);
    ok = lua_pcall(L, 1, 0, top + 1) == 0;
  }
  if (!ok) Logf(LogLevel::Warning, "Error running '/lua/ui/uimain.lua:OnMouseButtonPress': %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
  lua_settop(L, top);
}

}  // namespace

bool Control::HandleEvent(lua_State* L, const UiEvent& e) {
  PushEvent(L, this, e);
  if (!RunScript(L, "HandleEvent", 1, 1)) return false;
  bool handled = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return handled;
}

void PushEventTable(lua_State* L, Control* c, const UiEvent& e) { PushEvent(L, c, e); }

bool SendEvent(lua_State* L, Control* c, const UiEvent& e) { return c->HandleEvent(L, e); }

bool KeyEvent(lua_State* L, UiManager& ui, UiEvent e) {
  e.x = e.y = -1;
  // The key map comes first and never keeps the key from the controls (FA exe 0x838d10: the mapped command is run at
  // once, UI_Lua's text by SCR_LuaDoString in its handler 0x835370, and the event is passed on). But a control that
  // the key's own action made and gave the keyboard does not get the key press that made it (2026-11-11): Escape on
  // the main menu runs uimain.EscapeHandler, the menu's exit question comes up as an input-modal dialog whose Escape
  // means "No" (uiutil.MakeInputModal), and ours handed it the same key down: the question was answered and gone
  // within the frame, so Escape on the menu seemed to do nothing. In the original the question stays (Chris's picture
  // of that day). INFERRED how the original keeps the key from the new control (its handlers' order is not read): the
  // controls that had the keyboard before the key map ran are the ones the key down goes to.
  const Control* focusBefore = ui.focus && !ui.focus->dead() ? ui.focus : nullptr;
  const Control* captureBefore = ui.inputCapture.empty() ? nullptr : ui.inputCapture.back();
  if (e.type == EventType::KeyDown) KeyMapKeyDown(L, e.rawKeyCode, e.shift, e.ctrl, e.alt, e.repeat);
  else if (e.type == EventType::KeyUp) KeyMapKeyUp(L, e.rawKeyCode);
  if (ui.focus && !ui.focus->dead()) {
    if (e.type == EventType::KeyDown && ui.focus != focusBefore) return true;  // (focused by the key's own action)
    return ui.focus->HandleEvent(L, e);
  }
  if (ui.inputCapture.empty()) return false;
  Control* c = ui.inputCapture.back();
  if (e.type == EventType::KeyDown && c != captureBefore) return true;  // (made input-modal by the key's own action)
  if (c && !c->dead()) c->HandleEvent(L, e);
  return true;
}

void KeyPress(lua_State* L, UiManager& ui, int charCode, bool shift, bool ctrl, bool alt, int windowsKey) {
  UiEvent e;
  e.shift = shift, e.ctrl = ctrl, e.alt = alt;
  e.keyCode = charCode >= 'a' && charCode <= 'z' ? charCode - 32 : charCode;
  e.rawKeyCode = windowsKey >= 0 ? windowsKey : KeycodeMauiToMSW(e.keyCode);
  e.type = EventType::KeyDown;
  if (!KeyEvent(L, ui, e)) {
    e.keyCode = ctrl && e.keyCode >= 'A' && e.keyCode <= 'Z' ? e.keyCode - 64 : charCode;
    e.type = EventType::Char;
    KeyEvent(L, ui, e);
  }
  e.keyCode = charCode >= 'a' && charCode <= 'z' ? charCode - 32 : charCode;
  e.type = EventType::KeyUp;
  KeyEvent(L, ui, e);
}

void WindowFocus(lua_State* L, UiManager& ui, bool inFront) {
  if (ui.windowInFront == inFront) return;
  ui.windowInFront = inFront;
  if (!inFront) KeyMapAllKeysUp(L);
}

void PostEvent(lua_State* L, Control* c, const UiEvent& e) {
  while (c) {
    Control* parent = c->parent();  // taken first: the script may destroy or move the control
    if (c->dead() || SendEvent(L, c, e)) return;
    c = parent;
  }
}

Control* ControlAt(UiManager& ui, int head, float x, float y) {
  Frame* frame = ui.GetFrame(head);
  return frame ? TopmostIn(frame, x, y) : nullptr;
}

namespace {

// GetTopmostControl (FA exe 0x7872e0): of `root` and everything below it, the control with the
// highest Depth among those not hidden, with hit testing on, that contain the point; of equal
// depths the first in tree order.
Control* TopmostIn(Control* root, float x, float y) {
  if (!root || root->dead()) return nullptr;
  std::vector<Control*> all;
  Collect(root, all);
  Control* best = nullptr;
  float bestDepth = 0;
  for (Control* c : all) {
    if (c->dead() || c->IsHidden() || c->IsHitTestDisabled()) continue;
    SetLazyVarContext(c);
    const bool in = c->HitTest(x, y);
    SetLazyVarContext(nullptr);
    if (!in) continue;
    float d = c->depth.Get();  // the original compares the depths of the last drawn frame
    if (!best || d > bestDepth) best = c, bestDepth = d;
  }
  return best;
}

}  // namespace

// --- draggers ---
//
// A script posts a dragger while a mouse button is held (a button being clicked, a window being
// moved): PostDragger(originFrame, keycode, dragger). From then on (FA exe 0x7a4c00..0x7a4cb6)
// the mouse's moves go to dragger:OnMove(x, y) instead of the controls, and the release of that
// button to dragger:OnRelease(x, y), which ends it. Inferred: posting nil, or another dragger,
// cancels the current one through dragger:OnCancel().

namespace {

// The KeyCode of a button event: the button's number, 1 left, 2 middle, 3 right (READ: the mouse handler,
// 0x7a4c03 and 0x7a4e85, puts the window event's button there, 0x979560: the first of the buttons 1..3 the
// event is about; the scrollbar takes a press only with KeyCode 1, 0x7a11e1, and FAF's scripts skip a movie on
// "a mouse click" by KeyCode 1 or 3, credits.lua; that 2 is the middle one is INFERRED from the window
// library's numbering). The names IsKeyDown takes (LBUTTON 301, RBUTTON 302, MBUTTON 304) are another list.
int ButtonKeyCode(const UiEvent& e) { return e.left ? 1 : e.middle ? 2 : e.right ? 3 : 0; }

// Push the active dragger's table; false (nothing pushed) when there is none or it was destroyed.
bool PushDragger(lua_State* L, UiManager& ui) {
  if (ui.draggerRef == LUA_NOREF) return false;
  lua_rawgeti(L, LUA_REGISTRYINDEX, ui.draggerRef);
  if (GetObject(L, -1)) return true;
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, ui.draggerRef);
  ui.draggerRef = LUA_NOREF;
  return false;
}

// dragger:<fn>(x, y) for the table on top of the stack (popped); `args` of the two numbers are passed.
void CallDragger(lua_State* L, const char* fn, int args, float x, float y) {
  int self = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushstring(L, fn);
  lua_gettable(L, self);
  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, self);
    if (args) {
      lua_pushnumber(L, x);
      lua_pushnumber(L, y);
    }
    if (lua_pcall(L, 1 + args, 0, self + 1) != 0)
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  }
  lua_settop(L, self - 1);
}

void EndDragger(lua_State* L, UiManager& ui) {
  if (ui.draggerRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ui.draggerRef);
  ui.draggerRef = LUA_NOREF;
  ui.draggerKey = 0;
}

// PostDragger(originFrame, keycode, dragger)
int l_PostDragger(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  lua_settop(L, 3);
  if (Control* engine = ui.engineDragger) {  // (the engine's own dragger is cancelled too: its slot 3)
    ui.engineDragger = nullptr;
    if (!engine->dead()) engine->DragCancel(L);
  }
  if (PushDragger(L, ui)) {
    bool same = lua_rawequal(L, -1, 3) != 0;
    if (same) {
      lua_pop(L, 1);
    } else {
      EndDragger(L, ui);
      CallDragger(L, "OnCancel", 0, 0, 0);
    }
  }
  EndDragger(L, ui);
  if (lua_istable(L, 3)) {
    lua_pushvalue(L, 3);
    ui.draggerRef = luaL_ref(L, LUA_REGISTRYINDEX);
    ui.draggerKey = static_cast<int>(lua_tonumber(L, 2));
  }
  return 0;
}

}  // namespace

void UiManager::PostEngineDragger(lua_State* L, Control* control, int key) {
  if (PushDragger(L, *this)) {
    EndDragger(L, *this);
    CallDragger(L, "OnCancel", 0, 0, 0);
  }
  // The engine's dragger before is cancelled (0x78dde0: the dragger posted over another calls its slot 3), also when
  // it is the same control starting another drag of its own (in the original a dragger object of its own each).
  if (Control* before = engineDragger) {
    engineDragger = nullptr;
    if (!before->dead()) before->DragCancel(L);
  }
  engineDragger = control;
  draggerKey = key;
}

void MouseMove(lua_State* L, UiManager& ui, UiEvent e) {
  ui.mouseX = e.x, ui.mouseY = e.y, ui.mouseHead = e.head;
  if (ui.engineDragger) {
    ui.engineDragger->DragMove(L, e);
    return;
  }
  if (PushDragger(L, ui)) {
    CallDragger(L, "OnMove", 2, e.x, e.y);
    return;
  }
  Control* target = Target(ui, e);
  UpdateHover(L, ui, target, e);
  if (!target || target->dead()) return;
  e.type = EventType::MouseMotion;
  PostEvent(L, target, e);
}

void MouseButton(lua_State* L, UiManager& ui, UiEvent e, bool pressed, bool doubleClick) {
  e.keyCode = ButtonKeyCode(e);
  ui.mouseX = e.x, ui.mouseY = e.y, ui.mouseHead = e.head;
  if (!pressed && ui.draggerKey == e.keyCode && ui.engineDragger) {
    Control* c = ui.engineDragger;
    ui.engineDragger = nullptr;
    ui.draggerKey = 0;
    c->DragRelease(L, e);
    return;
  }
  if (!pressed && ui.draggerKey == e.keyCode && PushDragger(L, ui)) {
    EndDragger(L, ui);
    CallDragger(L, "OnRelease", 2, e.x, e.y);
    return;
  }
  Control* target = Target(ui, e);
  UpdateHover(L, ui, target, e);
  e.type = !pressed ? EventType::ButtonRelease : doubleClick ? EventType::ButtonDClick : EventType::ButtonPress;
  if (pressed) {
    // A click away from the control with the keyboard focus tells it so (0x7a4d09).
    if (ui.focus && ui.focus != target && !ui.focus->dead()) ui.focus->LosingKeyboardFocus(L);
    CallOnMouseButtonPress(L, e);
    target = Target(ui, e);  // the scripts above may have changed what is there
  }
  if (target && !target->dead()) PostEvent(L, target, e);
}

void MouseWheel(lua_State* L, UiManager& ui, UiEvent e) {
  Control* target = Target(ui, e);
  if (!target || target->dead()) return;
  e.type = EventType::WheelRotation;
  PostEvent(L, target, e);
}

namespace {

// (a test function's event at the numbers at xIdx, xIdx + 1; on the head at headIdx when a number is there, else 0)
UiEvent EventAt(lua_State* L, int xIdx, int headIdx = 0) {
  UiEvent e;
  e.x = static_cast<float>(luaL_checknumber(L, xIdx));
  e.y = static_cast<float>(luaL_checknumber(L, xIdx + 1));
  if (headIdx > 0 && lua_isnumber(L, headIdx)) e.head = static_cast<int>(lua_tonumber(L, headIdx));
  return e;
}

// moho64ui_Resize(width, height [, head]): the head (the first without `head`) takes a new size, as when its window is
// resized (UiManager::OnResize: its frame, then uimain.OnApplicationResize). The host's own function for tests.
int l_Resize(lua_State* L) {
  UserState::From(L)->ui().OnResize(L, static_cast<int>(luaL_optnumber(L, 3, 0)), static_cast<int>(luaL_checknumber(L, 1)),
                                    static_cast<int>(luaL_checknumber(L, 2)));
  return 0;
}
int l_MouseMove(lua_State* L) {
  MouseMove(L, UserState::From(L)->ui(), EventAt(L, 1, 3));
  return 0;
}

// moho64ui_MouseButton(button, pressed, x, y [, modifiers [, head]]): button is "Left", "Middle" or
// "Right"; modifiers names any of Shift, Ctrl, Alt, and "Double" for the second press of a double click
int l_MouseButton(lua_State* L) {
  std::string button = luaL_checkstring(L, 1);
  bool pressed = lua_toboolean(L, 2) != 0;
  UiEvent e = EventAt(L, 3, 6);
  e.left = button == "Left";
  e.middle = button == "Middle";
  e.right = button == "Right";
  // (the fifth argument may name "Double" and the keys held with the click: Shift, Ctrl, Alt)
  const std::string with = lua_isstring(L, 5) ? lua_tostring(L, 5) : "";
  bool twice = with.find("Double") != std::string::npos;
  if (with.find("Shift") != std::string::npos) e.shift = true;
  if (with.find("Ctrl") != std::string::npos) e.ctrl = true;
  if (with.find("Alt") != std::string::npos) e.alt = true;
  MouseButton(L, UserState::From(L)->ui(), e, pressed, twice);
  return 0;
}

// moho64ui_MouseWheel(x, y, notches [, head]): the wheel turned at a place; positive is away from the user
int l_MouseWheel(lua_State* L) {
  UiEvent e = EventAt(L, 1, 4);
  e.wheelDelta = 120;
  e.wheelRotation = static_cast<int>(luaL_checknumber(L, 3)) * 120;
  MouseWheel(L, UserState::From(L)->ui(), e);
  return 0;
}

int l_ControlAt(lua_State* L) {
  UiEvent e = EventAt(L, 1, 3);
  PushObject(L, ControlAt(UserState::From(L)->ui(), e.head, e.x, e.y));
  return 1;
}

// moho64ui_Key(code [, modifiers]): one key pressed and let go. `code` is a character ("a"), or
// a key code as events carry it (13 Enter, 27 Escape, 8 Backspace, 127 Delete, 314 End, 315 Home,
// 316 Left, 318 Right, 324 Insert); modifiers is a string naming any of Shift, Ctrl, Alt.
// moho64ui_Key(code, modifiers, "down" or "up"): only that half, for a key that is held.
// moho64ui_Key(code, modifiers, half, windowsKey): with the Windows key code a real window's event carries, for a key
// whose code KeycodeMauiToMSW does not turn back into it (the punctuation keys: "[" with 219, as FAF's "LeftBracket");
// half "" for a whole press.
int l_Key(lua_State* L) {
  int code = lua_type(L, 1) == LUA_TSTRING ? static_cast<unsigned char>(lua_tostring(L, 1)[0])
                                           : static_cast<int>(luaL_checknumber(L, 1));
  std::string mods = luaL_optstring(L, 2, "");
  std::string half = luaL_optstring(L, 3, "");
  const int windowsKey = lua_isnumber(L, 4) ? static_cast<int>(lua_tonumber(L, 4)) : -1;
  if (half == "down" || half == "up") {
    UiEvent e;
    e.shift = mods.find("Shift") != std::string::npos, e.ctrl = mods.find("Ctrl") != std::string::npos,
    e.alt = mods.find("Alt") != std::string::npos;
    e.keyCode = code >= 'a' && code <= 'z' ? code - 32 : code;
    e.rawKeyCode = windowsKey >= 0 ? windowsKey : KeycodeMauiToMSW(e.keyCode);
    e.type = half == "down" ? EventType::KeyDown : EventType::KeyUp;
    KeyEvent(L, UserState::From(L)->ui(), e);
    return 0;
  }
  KeyPress(L, UserState::From(L)->ui(), code, mods.find("Shift") != std::string::npos,
           mods.find("Ctrl") != std::string::npos, mods.find("Alt") != std::string::npos, windowsKey);
  return 0;
}

// moho64ui_WindowFocus(inFront): the host's window leaves the front (false) or comes back (true), as a live window
// tells it (WindowFocus).
int l_WindowFocus(lua_State* L) {
  WindowFocus(L, UserState::From(L)->ui(), lua_toboolean(L, 1) != 0);
  return 0;
}

// moho64ui_EdgeScroll(on): the screen's edge moves the main view, as a full-screen window sets it (UiManager::
// edgeScroll; run_window.cpp sets it every frame of a live window). For tests without a window.
int l_EdgeScroll(lua_State* L) {
  UserState::From(L)->ui().edgeScroll = lua_toboolean(L, 1) != 0;
  return 0;
}

// moho64ui_DragState(): for tests: whether the pointer is held and hidden (a camera drag: UiManager::holdPointer) and
// whether a control drags by itself now (UiManager::engineDragger), as "held dragging", "held", "dragging" or "".
int l_DragState(lua_State* L) {
  const UiManager& ui = UserState::From(L)->ui();
  std::string state = ui.holdPointer ? "held" : "";
  if (ui.engineDragger) state += state.empty() ? "dragging" : " dragging";
  lua_pushstring(L, state.c_str());
  return 1;
}

// moho64ui_Type(text): the characters of an ASCII text typed one after the other.
int l_Type(lua_State* L) {
  std::string text = luaL_checkstring(L, 1);
  for (unsigned char ch : text) KeyPress(L, UserState::From(L)->ui(), ch, false, false, false);
  return 0;
}

}  // namespace

void SetGlobal(lua_State* L, const char* name, lua_CFunction f);  // user/bindings.h

void RegisterInputBindings(lua_State* L) {
  SetGlobal(L, "PostDragger", l_PostDragger);
  lua_register(L, "moho64ui_Resize", l_Resize);
  lua_register(L, "moho64ui_MouseMove", l_MouseMove);
  lua_register(L, "moho64ui_MouseButton", l_MouseButton);
  lua_register(L, "moho64ui_ControlAt", l_ControlAt);
  lua_register(L, "moho64ui_MouseWheel", l_MouseWheel);
  lua_register(L, "moho64ui_Key", l_Key);
  lua_register(L, "moho64ui_Type", l_Type);
  lua_register(L, "moho64ui_WindowFocus", l_WindowFocus);
  lua_register(L, "moho64ui_EdgeScroll", l_EdgeScroll);
  lua_register(L, "moho64ui_DragState", l_DragState);
}

}  // namespace moho::ui
