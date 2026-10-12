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

Control* TopmostIn(Control* root, float x, float y);

// Where a mouse event goes (FA exe 0x7a4a77..0x7a4b12). Without input capture: the control under
// the cursor on the frame. With it: the control under the cursor among the capturing control and
// everything below it, and when the cursor is over none of those, the capturing control itself.
Control* Target(UiManager& ui, const UiEvent& e) {
  if (!ui.inputCapture.empty()) {
    Control* capture = ui.inputCapture.back();
    Control* hit = TopmostIn(capture, e.x, e.y);
    return hit ? hit : capture;
  }
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
    if (c->dead() || c->IsHidden() || c->IsHitTestDisabled() || !c->HitTest(x, y)) continue;
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

// The key codes of the mouse buttons as events carry them. Inferred: the original's key code
// names follow wxWidgets' list in order, where the buttons are 301, 302 and 304.
int ButtonKeyCode(const UiEvent& e) { return e.left ? 301 : e.right ? 302 : e.middle ? 304 : 0; }

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

void MouseMove(lua_State* L, UiManager& ui, UiEvent e) {
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

void SetGlobal(lua_State* L, const char* name, lua_CFunction f);  // user/bindings.h

void RegisterInputBindings(lua_State* L) {
  SetGlobal(L, "PostDragger", l_PostDragger);
  lua_register(L, "moho64ui_MouseMove", l_MouseMove);
  lua_register(L, "moho64ui_MouseButton", l_MouseButton);
  lua_register(L, "moho64ui_ControlAt", l_ControlAt);
}

}  // namespace moho::ui
