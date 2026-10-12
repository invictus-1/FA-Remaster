// Root frames, the cursor, input capture and draggers.
#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {

// --- Frame ---

void Frame::Destroy(lua_State*) {}

namespace {
// Everything below `c`, depth first, parents before children, older children first.
void Collect(Control* c, std::vector<Control*>& out) {
  for (Control* k : c->children()) {
    out.push_back(k);
    Collect(k, out);
  }
}
}  // namespace

void Frame::Update(lua_State* L, float seconds) {
  std::vector<Control*> all;
  Collect(this, all);
  for (Control* c : all) {
    if (c->dead() || !c->needsFrameUpdate) continue;
    lua_pushnumber(L, seconds);
    c->RunScript(L, "OnFrame", 1, 0);
  }
  PurgeDeleted();
}

void Frame::PurgeDeleted() {
  std::vector<Control*> dead;
  dead.swap(deadList_);
  for (Control* c : dead) delete c;  // the Lua table stays; it reads as destroyed from here on
}

// Inferred: the largest Depth among the frame's living controls.
float Frame::TopmostDepth() const {
  std::vector<Control*> all;
  Collect(const_cast<Frame*>(this), all);
  float best = 0.0f, d;
  for (Control* c : all)
    if (!c->dead() && c->depth.TryGet(&d)) best = std::max(best, d);
  return best;
}

// --- UiManager ---

UiManager::~UiManager() {
  // The Lua state goes away with us: nothing is called back, the objects are just freed.
  for (Frame* f : frames_) {
    std::vector<Control*> all;
    Collect(f, all);
    for (Control* c : all) delete c;
    f->PurgeDeleted();
    delete f;
  }
}

bool UiManager::CreateRootFrames(lua_State* L, int width, int height) {
  width_ = width;
  height_ = height;
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/maui/frame.lua");
  bool ok = lua_pcall(L, 1, 1, top + 1) == 0;
  if (ok) {
    lua_pushstring(L, "Frame");
    lua_gettable(L, -2);
    ok = lua_pcall(L, 0, 1, top + 1) == 0;
  }
  Frame* frame = ok ? dynamic_cast<Frame*>(GetObject(L, -1)) : nullptr;
  if (!frame) {
    Logf(LogLevel::Warning, "Error in CMauiFrame::Create(): %s",
         !ok && lua_isstring(L, -1) ? lua_tostring(L, -1) : "the Frame class did not make a frame");
    lua_settop(L, top);
    return false;
  }
  lua_settop(L, top);
  frame->head = static_cast<int>(frames_.size());
  frames_.push_back(frame);
  Resize(width, height);
  return true;
}

void UiManager::Resize(int width, int height) {
  width_ = width;
  height_ = height;
  for (Frame* f : frames_) {
    f->left.Set(0.0f);
    f->top.Set(0.0f);
    f->width.Set(static_cast<float>(width));
    f->height.Set(static_cast<float>(height));
  }
}

Frame* UiManager::GetFrame(int head) const {
  return head >= 0 && head < static_cast<int>(frames_.size()) ? frames_[static_cast<size_t>(head)] : nullptr;
}

void UiManager::Update(lua_State* L, float seconds) {
  for (Frame* f : frames_) f->Update(L, seconds);
}

void UiManager::Forget(Control* c) {
  if (focus == c) focus = nullptr;
  inputCapture.erase(std::remove(inputCapture.begin(), inputCapture.end(), c), inputCapture.end());
}

void UiManager::SetFocus(lua_State* L, Control* c) {
  if (focus == c) return;
  Control* old = focus;
  focus = c;
  if (old) old->RunScript(L, "OnLoseKeyboardFocus");
  if (c && focus == c) c->RunScript(L, "OnKeyboardFocusChange");
}

// --- WeakControl ---

void WeakControl::Set(lua_State* L, int idx) {
  Clear();
  L_ = ScriptState::From(L)->L();
  lua_pushvalue(L, idx);
  ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
}

void WeakControl::Clear() {
  if (L_ && ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
  ref_ = LUA_NOREF;
}

Control* WeakControl::Get() const {
  if (ref_ == LUA_NOREF) return nullptr;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);
  Control* c = dynamic_cast<Control*>(GetObject(L_, -1));
  lua_pop(L_, 1);
  return c && !c->dead() ? c : nullptr;
}

int UiManager::CountControls() const {
  int n = 0;
  for (Frame* f : frames_) {
    std::vector<Control*> all;
    Collect(f, all);
    n += 1 + static_cast<int>(all.size());
  }
  return n;
}

namespace {
void DumpControl(const Control* c, int indent) {
  float v[5] = {0, 0, 0, 0, 0};
  const LazyVar* vars[5] = {&c->left, &c->top, &c->right, &c->bottom, &c->depth};
  bool ok = true;
  for (int i = 0; i < 5; ++i) ok = vars[i]->TryGet(&v[i]) && ok;
  char rect[96];
  if (ok) std::snprintf(rect, sizeof rect, "[%g,%g %g,%g] depth %g", v[0], v[1], v[2], v[3], v[4]);
  else std::snprintf(rect, sizeof rect, "[layout not set] depth %g", v[4]);
  std::string own;
  c->Describe(own);
  Logf(LogLevel::Info, "ui: %*s%s \"%s\" %s%s%s%s%s%s%s", indent * 2, "", c->ClassName(), c->name.c_str(), rect,
       c->IsHidden() ? " hidden" : "", c->IsHitTestDisabled() ? " nohit" : "", c->needsFrameUpdate ? " frame" : "",
       c->alpha != 1.0f ? (" alpha " + std::to_string(c->alpha)).c_str() : "", own.empty() ? "" : " ", own.c_str());
  for (const Control* k : c->children()) DumpControl(k, indent + 1);
}
}  // namespace

void UiManager::Dump() const {
  for (Frame* f : frames_) {
    Logf(LogLevel::Info, "ui: head %d: %dx%d", f->head, width_, height_);
    DumpControl(f, 0);
  }
  Logf(LogLevel::Info, "ui: %d controls", CountControls());
}

// InternalCreateFrame(luaobj) (0x796790): called by the script class Frame's __init.
int l_InternalCreateFrame(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Frame* f = new Frame;
  f->Create(L, 1, nullptr);
  f->MakeRoot();
  f->RunInit(L);
  return 0;
}

namespace {

UiManager& Ui(lua_State* L) { return UserState::From(L)->ui(); }

// GetFrame(head) (0x83f4f0)
int l_GetFrame(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "frame GetFrame(int head)", 1, lua_gettop(L));
  UiManager& ui = Ui(L);
  if (!ui.initialized()) return luaL_error(L, "UI layer has not been initialized.");
  PushObject(L, ui.GetFrame(static_cast<int>(luaL_checknumber(L, 1))));
  return 1;
}

int l_GetNumRootFrames(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(Ui(L).NumFrames()));
  return 1;
}

// ClearFrame(head): destroy all controls in the frame; nil clears every frame.
int l_ClearFrame(lua_State* L) {
  UiManager& ui = Ui(L);
  for (int h = 0; h < ui.NumFrames(); ++h)
    if (lua_isnil(L, 1) || lua_gettop(L) == 0 || static_cast<int>(lua_tonumber(L, 1)) == h) ui.GetFrame(h)->ClearChildren(L);
  return 0;
}

int l_Frame_GetTopmostDepth(lua_State* L) {
  lua_pushnumber(L, CheckControl<Frame>(L, 1)->TopmostDepth());
  return 1;
}
int l_Frame_GetTargetHead(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(CheckControl<Frame>(L, 1)->targetHead));
  return 1;
}
int l_Frame_SetTargetHead(lua_State* L) {
  CheckControl<Frame>(L, 1)->targetHead = static_cast<int>(luaL_checknumber(L, 2));
  return 0;
}

// --- input capture: a stack of controls; the last one gets the input ---

int l_AddInputCapture(lua_State* L) {
  Ui(L).inputCapture.push_back(CheckControl(L, 1));
  return 0;
}
// RemoveInputCapture(control): the last entry for that control goes.
int l_RemoveInputCapture(lua_State* L) {
  Control* c = CheckControl(L, 1);
  auto& cap = Ui(L).inputCapture;
  for (size_t i = cap.size(); i-- > 0;)
    if (cap[i] == c) {
      cap.erase(cap.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  return 0;
}
int l_GetInputCapture(lua_State* L) {
  auto& cap = Ui(L).inputCapture;
  PushObject(L, cap.empty() ? nullptr : cap.back());
  return 1;
}
int l_AnyInputCapture(lua_State* L) {
  lua_pushboolean(L, !Ui(L).inputCapture.empty());
  return 1;
}

// --- the cursor: not a control; one object the scripts make and hand to SetCursor ---

class Cursor : public ScriptObject {
 public:
  std::string texture, defaultTexture;
  int hotX = 0, hotY = 0, defaultHotX = 0, defaultHotY = 0;
  bool hidden = false;
};

const char* kCursorKey = "moho64ui.cursor";

// _c_CreateCursor(luaobj, spec)
int l_CreateCursor(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  BindObject(L, 1, new Cursor);  // lives as long as the state
  lua_pushvalue(L, 1);
  return 1;
}
int l_SetCursor(lua_State* L) {
  lua_settop(L, 1);
  lua_pushstring(L, kCursorKey);
  lua_pushvalue(L, 1);
  lua_rawset(L, LUA_REGISTRYINDEX);
  return 0;
}
int l_GetCursor(lua_State* L) {
  lua_pushstring(L, kCursorKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  return 1;
}
int l_Cursor_SetNewTexture(lua_State* L) {
  Cursor* c = CheckObject<Cursor>(L, 1);
  c->texture = luaL_checkstring(L, 2);
  c->hotX = static_cast<int>(luaL_optnumber(L, 3, 0));
  c->hotY = static_cast<int>(luaL_optnumber(L, 4, 0));
  return 0;
}
int l_Cursor_SetDefaultTexture(lua_State* L) {
  Cursor* c = CheckObject<Cursor>(L, 1);
  c->defaultTexture = luaL_checkstring(L, 2);
  c->defaultHotX = static_cast<int>(luaL_optnumber(L, 3, 0));
  c->defaultHotY = static_cast<int>(luaL_optnumber(L, 4, 0));
  return 0;
}
int l_Cursor_ResetToDefault(lua_State* L) {
  Cursor* c = CheckObject<Cursor>(L, 1);
  c->texture = c->defaultTexture;
  c->hotX = c->defaultHotX;
  c->hotY = c->defaultHotY;
  return 0;
}
int l_Cursor_Hide(lua_State* L) {
  CheckObject<Cursor>(L, 1)->hidden = true;
  return 0;
}
int l_Cursor_Show(lua_State* L) {
  CheckObject<Cursor>(L, 1)->hidden = false;
  return 0;
}

// --- draggers: an object the scripts make and post while a mouse button is held ---

class Dragger : public ScriptObject {};

int l_InternalCreateDragger(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  BindObject(L, 1, new Dragger);
  return 0;
}
int l_Dragger_Destroy(lua_State* L) {
  if (Dragger* d = dynamic_cast<Dragger*>(GetObject(L, 1))) delete d;
  return 0;
}

// moho64ui_Describe(control): not an engine function of the original. The host's own debugging
// aid for tests: the control's class and state as --dump-ui prints them.
int l_Describe(lua_State* L) {
  Control* c = CheckControl(L, 1);
  std::string own = c->ClassName();
  own += " ";
  c->Describe(own);
  lua_pushstring(L, own.c_str());
  return 1;
}

}  // namespace

void RegisterFrameBindings(lua_State* L) {
  SetGlobal(L, "InternalCreateFrame", l_InternalCreateFrame);
  SetGlobal(L, "GetFrame", l_GetFrame);
  SetGlobal(L, "GetNumRootFrames", l_GetNumRootFrames);
  SetGlobal(L, "ClearFrame", l_ClearFrame);
  SetMethod(L, "CMauiFrame", "GetTopmostDepth", l_Frame_GetTopmostDepth);
  SetMethod(L, "CMauiFrame", "GetTargetHead", l_Frame_GetTargetHead);
  SetMethod(L, "CMauiFrame", "SetTargetHead", l_Frame_SetTargetHead);

  SetGlobal(L, "AddInputCapture", l_AddInputCapture);
  SetGlobal(L, "RemoveInputCapture", l_RemoveInputCapture);
  SetGlobal(L, "GetInputCapture", l_GetInputCapture);
  SetGlobal(L, "AnyInputCapture", l_AnyInputCapture);

  SetGlobal(L, "_c_CreateCursor", l_CreateCursor);
  SetGlobal(L, "SetCursor", l_SetCursor);
  SetGlobal(L, "GetCursor", l_GetCursor);
  SetMethod(L, "CMauiCursor", "SetNewTexture", l_Cursor_SetNewTexture);
  SetMethod(L, "CMauiCursor", "SetDefaultTexture", l_Cursor_SetDefaultTexture);
  SetMethod(L, "CMauiCursor", "ResetToDefault", l_Cursor_ResetToDefault);
  SetMethod(L, "CMauiCursor", "Hide", l_Cursor_Hide);
  SetMethod(L, "CMauiCursor", "Show", l_Cursor_Show);

  SetGlobal(L, "InternalCreateDragger", l_InternalCreateDragger);
  SetMethod(L, "CMauiLuaDragger", "Destroy", l_Dragger_Destroy);

  lua_register(L, "moho64ui_Describe", l_Describe);
}

}  // namespace moho::ui
