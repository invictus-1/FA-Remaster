#include "maui/control.h"

#include <algorithm>

#include "core/log.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {

// --- Control ---

void Control::AddLazyVar(lua_State* L, LazyVar& var, const char* field) {
  var.Create(L, 0.0f);
  PushObject(L, this);
  lua_pushstring(L, field);
  var.Push(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

void Control::Create(lua_State* L, int luaObj, Control* parent) {
  if (luaObj < 0) luaObj = lua_gettop(L) + luaObj + 1;
  LazyVar* vars[] = {&left, &right, &top, &bottom, &width, &height, &depth};
  for (LazyVar* v : vars) v->Create(L, 0.0f);
  BindObject(L, luaObj, this);
  if (parent) {
    parent_ = parent;
    parent->children_.push_back(this);
    SetHidden(L, parent->IsHidden());
    renderPass = parent->renderPass;
    root_ = parent->root_;
  }
  static const char* const kFields[] = {"Left", "Right", "Top", "Bottom", "Width", "Height", "Depth"};
  for (int i = 0; i < 7; ++i) {
    lua_pushstring(L, kFields[i]);
    vars[i]->Push(L);
    lua_rawset(L, luaObj);
  }
}

bool Control::RunScript(lua_State* L, const char* fn, int nargs, int nresults) {
  int base = lua_gettop(L) - nargs;
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  PushObject(L, this);
  if (!lua_istable(L, -1)) {
    lua_settop(L, base);
    return false;
  }
  lua_pushstring(L, fn);
  lua_gettable(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, base);
    return false;
  }
  lua_insert(L, base + 2);  // handler, function, args..., self
  lua_insert(L, base + 3);  // handler, function, self, args...
  if (lua_pcall(L, nargs + 1, nresults, base + 1) != 0) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    lua_settop(L, base);
    return false;
  }
  lua_remove(L, base + 1);
  return true;
}

void Control::Destroy(lua_State* L) {
  if (parent_) {
    auto& sib = parent_->children_;
    sib.erase(std::remove(sib.begin(), sib.end(), this), sib.end());
  }
  parent_ = nullptr;
  if (!dead_ && root_ && root_ != this) root_->deadList_.push_back(this);
  dead_ = true;
  if (UserState* us = UserState::From(L)) us->ui().Forget(this);
  RunScript(L, "OnDestroy");
  ClearChildren(L);
}

void Control::ClearChildren(lua_State* L) {
  while (!children_.empty()) {
    Control* c = children_.back();  // newest first
    children_.pop_back();
    c->Destroy(L);
  }
}

void Control::SetHidden(lua_State* L, bool hidden) {
  lua_pushboolean(L, hidden);
  if (RunScript(L, "OnHide", 1, 1)) {
    bool handled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (handled) return;
  }
  hidden_ = hidden;
  // A script's OnHide may add or remove children while we walk them.
  std::vector<Control*> kids = children_;
  for (Control* c : kids)
    if (std::find(children_.begin(), children_.end(), c) != children_.end()) c->SetHidden(L, hidden);
}

void Control::SetHitTestDisabled(bool disabled, bool recursive) {
  hitTestDisabled_ = disabled;
  if (recursive)
    for (Control* c : children_) c->SetHitTestDisabled(disabled, true);
}

bool Control::HitTest(float x, float y) const {
  return x >= left.Get() && x < right.Get() && y >= top.Get() && y < bottom.Get();
}

// Inferred: the control joins the new parent's children as the newest and takes its root frame;
// its hidden state stays as it is.
void Control::SetParent(Control* parent) {
  if (parent == parent_ || !parent) return;
  if (parent_) {
    auto& sib = parent_->children_;
    sib.erase(std::remove(sib.begin(), sib.end(), this), sib.end());
  }
  parent_ = parent;
  parent->children_.push_back(this);
  struct Walk {
    static void SetRoot(Control* c, Frame* root) {
      c->root_ = root;
      for (Control* k : c->children_) SetRoot(k, root);
    }
  };
  Walk::SetRoot(this, parent->root_);
}

void Control::SetAlpha(float a, bool children) {
  alpha = a;
  if (children)
    for (Control* c : children_) c->SetAlpha(a, true);
}

Control* CheckParent(lua_State* L) { return CheckControl<Control>(L, 2); }

namespace {

UiManager& Ui(lua_State* L) { return UserState::From(L)->ui(); }

// --- moho.control_methods ---

// Control:Destroy() (0x787ad0): a control that is destroyed already is left alone, without an
// error; the root frame cannot be destroyed.
int l_Destroy(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "Control:Destroy()", 1, lua_gettop(L));
  if (!lua_istable(L, 1)) return luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  Control* c = dynamic_cast<Control*>(GetObject(L, 1));
  if (!c) return 0;
  if (c == c->root()) return luaL_error(L, "Cannot destroy the root frame");
  c->Destroy(L);
  return 0;
}

int l_GetParent(lua_State* L) {
  PushObject(L, CheckControl(L, 1)->parent());
  return 1;
}

int l_ClearChildren(lua_State* L) {
  CheckControl(L, 1)->ClearChildren(L);
  return 0;
}

int l_SetParent(lua_State* L) {
  CheckControl(L, 1)->SetParent(CheckControl(L, 2));
  return 0;
}

int l_DisableHitTest(lua_State* L) {
  CheckControl(L, 1)->SetHitTestDisabled(true, lua_toboolean(L, 2) != 0);
  return 0;
}
int l_EnableHitTest(lua_State* L) {
  CheckControl(L, 1)->SetHitTestDisabled(false, lua_toboolean(L, 2) != 0);
  return 0;
}
int l_IsHitTestDisabled(lua_State* L) {
  lua_pushboolean(L, CheckControl(L, 1)->IsHitTestDisabled());
  return 1;
}

int l_Hide(lua_State* L) {
  CheckControl(L, 1)->SetHidden(L, true);
  return 0;
}
int l_Show(lua_State* L) {
  CheckControl(L, 1)->SetHidden(L, false);
  return 0;
}
int l_SetHidden(lua_State* L) {
  CheckControl(L, 1)->SetHidden(L, lua_toboolean(L, 2) != 0);
  return 0;
}
int l_IsHidden(lua_State* L) {
  lua_pushboolean(L, CheckControl(L, 1)->IsHidden());
  return 1;
}

int l_SetRenderPass(lua_State* L) {
  CheckControl(L, 1)->renderPass = static_cast<int>(luaL_checknumber(L, 2));
  return 0;
}
int l_GetRenderPass(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl(L, 1)->renderPass));
  return 1;
}

int l_NeedsFrameUpdate(lua_State* L) {
  lua_pushboolean(L, CheckControl(L, 1)->needsFrameUpdate);
  return 1;
}
int l_SetNeedsFrameUpdate(lua_State* L) {
  CheckControl(L, 1)->needsFrameUpdate = lua_toboolean(L, 2) != 0;
  return 0;
}

int l_SetAlpha(lua_State* L) {
  CheckControl(L, 1)->SetAlpha(static_cast<float>(luaL_checknumber(L, 2)), lua_toboolean(L, 3) != 0);
  return 0;
}
int l_GetAlpha(lua_State* L) {
  lua_pushnumber(L, CheckControl(L, 1)->alpha);
  return 1;
}

int l_GetName(lua_State* L) {
  lua_pushstring(L, CheckControl(L, 1)->name.c_str());
  return 1;
}
int l_SetName(lua_State* L) {
  CheckControl(L, 1)->name = luaL_checkstring(L, 2);
  return 0;
}

int l_GetRootFrame(lua_State* L) {
  PushObject(L, CheckControl(L, 1)->root());
  return 1;
}

// ApplyFunction(func): func(control) for this control and everything below it.
void Apply(lua_State* L, Control* c) {
  lua_pushvalue(L, 2);
  PushObject(L, c);
  lua_call(L, 1, 0);
  std::vector<Control*> kids = c->children();
  for (Control* k : kids)
    if (!k->dead()) Apply(L, k);
}
int l_ApplyFunction(lua_State* L) {
  Control* c = CheckControl(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  Apply(L, c);
  return 0;
}

int l_HitTest(lua_State* L) {
  Control* c = CheckControl(L, 1);
  lua_pushboolean(L, c->HitTest(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3))));
  return 1;
}

int l_Dump(lua_State* L) {
  CheckControl(L, 1);
  Ui(L).Dump();
  return 0;
}

// Keyboard focus: AcquireKeyboardFocus(blocksKeyDown) (the control's virtual slot 14, 0x787460)
// and AbandonKeyboardFocus() (slot 15); see UiManager::SetFocus for who hears of it.
int l_AcquireKeyboardFocus(lua_State* L) {
  Ui(L).SetFocus(L, CheckControl(L, 1), lua_toboolean(L, 2) != 0);
  return 0;
}
int l_AbandonKeyboardFocus(lua_State* L) {
  CheckControl(L, 1)->AbandonKeyboardFocus(L);
  return 0;
}
int l_GetCurrentFocusControl(lua_State* L) {
  PushObject(L, Ui(L).focus);
  return 1;
}

// --- creation ---

template <class T, class... A>
T* MakeControl(lua_State* L, A... args) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  T* c = new T(args...);
  c->Create(L, 1, parent);
  return c;
}

// InternalCreateGroup(luaobj, parent) (0x797390)
int l_InternalCreateGroup(lua_State* L) {
  if (lua_gettop(L) != 2)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "InternalCreateGroup(luaobj,parent)", 2, lua_gettop(L));
  MakeControl<Group>(L)->RunInit(L);
  return 0;
}

// The control classes that are not built yet: they exist, lay out and answer the control methods.
template <const char* ClassName>
int l_InternalCreatePlain(lua_State* L) {
  MakeControl<PlainControl>(L, ClassName)->RunInit(L);
  return 0;
}

constexpr char kMesh[] = "mesh", kMapPreview[] = "mappreview";

}  // namespace

void RegisterControlBindings(lua_State* L) {
  const char* cls = "CMauiControl";
  SetMethod(L, cls, "Destroy", l_Destroy);
  SetMethod(L, cls, "GetParent", l_GetParent);
  SetMethod(L, cls, "ClearChildren", l_ClearChildren);
  SetMethod(L, cls, "SetParent", l_SetParent);
  SetMethod(L, cls, "DisableHitTest", l_DisableHitTest);
  SetMethod(L, cls, "EnableHitTest", l_EnableHitTest);
  SetMethod(L, cls, "IsHitTestDisabled", l_IsHitTestDisabled);
  SetMethod(L, cls, "Hide", l_Hide);
  SetMethod(L, cls, "Show", l_Show);
  SetMethod(L, cls, "SetHidden", l_SetHidden);
  SetMethod(L, cls, "IsHidden", l_IsHidden);
  SetMethod(L, cls, "SetRenderPass", l_SetRenderPass);
  SetMethod(L, cls, "GetRenderPass", l_GetRenderPass);
  SetMethod(L, cls, "NeedsFrameUpdate", l_NeedsFrameUpdate);
  SetMethod(L, cls, "SetNeedsFrameUpdate", l_SetNeedsFrameUpdate);
  SetMethod(L, cls, "SetAlpha", l_SetAlpha);
  SetMethod(L, cls, "GetAlpha", l_GetAlpha);
  SetMethod(L, cls, "GetName", l_GetName);
  SetMethod(L, cls, "SetName", l_SetName);
  SetMethod(L, cls, "GetRootFrame", l_GetRootFrame);
  SetMethod(L, cls, "ApplyFunction", l_ApplyFunction);
  SetMethod(L, cls, "HitTest", l_HitTest);
  SetMethod(L, cls, "Dump", l_Dump);
  SetMethod(L, cls, "AcquireKeyboardFocus", l_AcquireKeyboardFocus);
  SetMethod(L, cls, "AbandonKeyboardFocus", l_AbandonKeyboardFocus);
  SetMethod(L, cls, "GetCurrentFocusControl", l_GetCurrentFocusControl);

  SetGlobal(L, "InternalCreateGroup", l_InternalCreateGroup);
  // Not built yet: the 3D mesh control and the map preview.
  SetGlobal(L, "InternalCreateMesh", l_InternalCreatePlain<kMesh>);
  SetGlobal(L, "InternalCreateMapPreview", l_InternalCreatePlain<kMapPreview>);
}

}  // namespace moho::ui
