#include "maui/input.h"

#include <string>
#include <vector>

#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"
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

// Where a mouse event goes: the capturing control, or the one under the cursor.
Control* Target(UiManager& ui, const UiEvent& e) {
  if (!ui.inputCapture.empty()) return ui.inputCapture.back();
  return ControlAt(ui, 0, e.x, e.y);
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

bool SendEvent(lua_State* L, Control* c, const UiEvent& e) {
  PushEvent(L, c, e);
  if (!c->RunScript(L, "HandleEvent", 1, 1)) return false;
  bool handled = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return handled;
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
  if (!frame) return nullptr;
  std::vector<Control*> all;
  Collect(frame, all);
  Control* best = nullptr;
  float bestDepth = 0;
  for (Control* c : all) {
    if (c->dead() || c->IsHidden() || c->IsHitTestDisabled() || !c->HitTest(x, y)) continue;
    float d = c->depth.Get();  // the original compares the depths of the last drawn frame
    if (!best || d > bestDepth) best = c, bestDepth = d;
  }
  return best;
}

void MouseMove(lua_State* L, UiManager& ui, UiEvent e) {
  Control* target = Target(ui, e);
  UpdateHover(L, ui, target, e);
  if (!target || target->dead()) return;
  e.type = EventType::MouseMotion;
  PostEvent(L, target, e);
}

void MouseButton(lua_State* L, UiManager& ui, UiEvent e, bool pressed, bool doubleClick) {
  Control* target = Target(ui, e);
  UpdateHover(L, ui, target, e);
  e.type = !pressed ? EventType::ButtonRelease : doubleClick ? EventType::ButtonDClick : EventType::ButtonPress;
  if (pressed) {
    // A click away from the control with the keyboard focus tells it so (0x7a4d09).
    if (ui.focus && ui.focus != target && !ui.focus->dead()) ui.focus->RunScript(L, "OnLoseKeyboardFocus");
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

UiEvent EventAt(lua_State* L, int xIdx) {
  UiEvent e;
  e.x = static_cast<float>(luaL_checknumber(L, xIdx));
  e.y = static_cast<float>(luaL_checknumber(L, xIdx + 1));
  return e;
}

int l_MouseMove(lua_State* L) {
  MouseMove(L, UserState::From(L)->ui(), EventAt(L, 1));
  return 0;
}

// moho64ui_MouseButton(button, pressed, x, y): button is "Left", "Middle" or "Right"
int l_MouseButton(lua_State* L) {
  std::string button = luaL_checkstring(L, 1);
  bool pressed = lua_toboolean(L, 2) != 0;
  UiEvent e = EventAt(L, 3);
  e.left = button == "Left";
  e.middle = button == "Middle";
  e.right = button == "Right";
  MouseButton(L, UserState::From(L)->ui(), e, pressed, false);
  return 0;
}

int l_ControlAt(lua_State* L) {
  UiEvent e = EventAt(L, 1);
  PushObject(L, ControlAt(UserState::From(L)->ui(), 0, e.x, e.y));
  return 1;
}

}  // namespace

void RegisterInputBindings(lua_State* L) {
  lua_register(L, "moho64ui_MouseMove", l_MouseMove);
  lua_register(L, "moho64ui_MouseButton", l_MouseButton);
  lua_register(L, "moho64ui_ControlAt", l_ControlAt);
}

}  // namespace moho::ui
