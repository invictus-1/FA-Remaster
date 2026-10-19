// Root frames, the cursor, input capture and draggers.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

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
    c->FrameUpdate(L, seconds);
  }
  PurgeDeleted();
}

void Control::FrameUpdate(lua_State* L, float seconds) {
  lua_pushnumber(L, seconds);
  RunScript(L, "OnFrame", 1, 0);
}

void Control::AbandonKeyboardFocus(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  if (ui.focus == this) ui.SetFocus(L, nullptr);
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
  frame->renderPass = 8;  // (UIRP_PostGlow: 0x84ce94, after CMauiFrame::Create; every control made on it takes it)
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

// Drawing a head (FA exe 0x786fa0, 0x7870f0): the frame and everything below it, depth first,
// makes one list ordered by Depth (Depth is only asked of controls that are not hidden; INFERRED:
// controls of equal depth keep their tree order). The list is drawn in render passes: in each, the
// controls that are not hidden and whose render pass has the pass's bit draw, lowest Depth first
// (CMauiFrame::DoRender 0x7870f0: renderPass & pass, then the control's Draw); a control with several
// bits draws in each of them. WRenViewport::Render (0x7f90d0) draws, in this order: pass 1
// (UIRP_UnderWorld: IUIManager::RenderFrames 0x84d550), the world of each world view in its
// rectangle (the views' scenes, then the cartographic ones, 0x7f97b2), pass 4 (UIRP_Glow: DrawUI
// 0x84d5d0), the bloom (0x7f983a), pass 8 (UIRP_PostGlow: RenderUI 0x7f88b0 -> DrawHead 0x84d650).
// No pass has bit 2 (UIRP_World, "reserved for world views"): a control with none of 1, 4 and 8 is
// never drawn. So a control of pass 8 (a root frame's, which every control takes from its parent
// unless a script sets another) draws over one of pass 1 or 4 whatever their Depths, and anything
// of pass 1 lies under the world: FAF's minimap window keeps its middle (window_m) there.
// A world view: the original's draws only its viewport's rectangle in pass 1 and its marks in its
// other passes (0x86ef40), its world comes between. Here a world view's picture is a control's
// Draw, made at the world's place whatever the view's passes: the main view first (DrawsBeneathAll),
// the others (minimaps) by Depth; its marks with it (INFERRED: in the original they are drawn in
// its pass 4 or 8 among the other controls). What pass 1 draws inside the main view is covered by
// its world in the original: here it is left out where it lies wholly inside the main view (the
// picture would cover it, and where the renderer lays the world under the interface it would show
// through; INFERRED for a control only partly inside: drawn, the picture covering that part).
// (INFERRED that the original's world lies under all controls of pass 4 and 8: read in the order
// above, and seen in FAF's reclaim labels showing over it at a Depth below the view's.)
namespace {
// Where drawing the interface's picture goes, by kind of control (with MOHO64UI_DRAW_PROFILE set;
// logged by LogDrawProfile at the end of a window run): for finding what to draw less often.
struct DrawCost {
  double seconds = 0;
  long long draws = 0;
};
std::map<std::string, DrawCost>& DrawCosts() {
  static std::map<std::string, DrawCost> costs;
  return costs;
}
long long g_renders = 0;

// The controls' drawing with every colour's alpha multiplied by a factor (SetUIControlsAlpha: the
// original sets its batcher's AlphaMultiplier for the interface's pass, 0x84d512, and its shader
// takes color.a times it). The pictures put in whole (the world's, the minimap's, a film's) as they are.
class AlphaScaledTarget : public DrawTarget {
 public:
  AlphaScaledTarget(DrawTarget& to, float factor) : to_(to), factor_(factor) {}
  int Width() const override { return to_.Width(); }
  int Height() const override { return to_.Height(); }
  void SetClip(const Rect& r) override { to_.SetClip(r); }
  void ClearClip() override { to_.ClearClip(); }
  void FillRect(const Rect& dst, uint32_t color) override { to_.FillRect(dst, Scaled(color)); }
  void DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override {
    to_.DrawImage(image, src, dst, Scaled(color));
  }
  void DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override {
    to_.DrawImageTiled(image, src, dst, Scaled(color));
  }
  void DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) override { to_.DrawGlyph(glyph, x, y, Scaled(color)); }
  void DrawPixels(const uint32_t* pixels, int width, int height, int x, int y) override { to_.DrawPixels(pixels, width, height, x, y); }

 private:
  uint32_t Scaled(uint32_t color) const {
    const float a = static_cast<float>(color >> 24) * factor_;
    const uint32_t alpha = !(a > 0) ? 0u : a >= 255 ? 255u : static_cast<uint32_t>(a + 0.5f);  // (NaN: 0)
    return (color & 0x00ffffffu) | (alpha << 24);
  }
  DrawTarget& to_;
  float factor_;
};
}  // namespace

void UiManager::Render(lua_State* L, DrawTarget& target, int head) {
  Frame* frame = GetFrame(head);
  if (!frame) return;
  if (controlsAlpha != 1.0f) {
    AlphaScaledTarget scaled(target, controlsAlpha);
    const float keep = controlsAlpha;
    controlsAlpha = 1.0f;  // (once: the scaled target is the target now)
    Render(L, scaled, head);
    controlsAlpha = keep;
    return;
  }
  static const bool profile = std::getenv("MOHO64UI_DRAW_PROFILE") != nullptr;
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  std::vector<Control*> all{frame};
  Collect(frame, all);
  std::vector<std::pair<float, Control*>> order;
  for (Control* c : all)
    if (!c->dead() && !c->IsHidden()) order.emplace_back(c->DrawsBeneathAll() ? -1e30f : c->depth.Get(), c);
  std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  if (profile) {
    ++g_renders;
    DrawCost& sorting = DrawCosts()["(collecting and sorting the controls)"];
    sorting.seconds += std::chrono::duration<double>(Clock::now() - t0).count(), sorting.draws += static_cast<long long>(all.size());
  }
  auto draw = [&](Control* c) {
    if (c->dead()) return;  // (a script run by an earlier Draw may have destroyed it)
    SetLazyVarContext(c);
    if (!profile) {
      c->Draw(L, target);
      return;
    }
    const auto a = Clock::now();
    c->Draw(L, target);
    DrawCost& cost = DrawCosts()[c->ClassName()];
    cost.seconds += std::chrono::duration<double>(Clock::now() - a).count();
    ++cost.draws;
  };
  // The rectangles of the main world views (their world covers what pass 1 drew there).
  std::vector<Rect> worlds;
  for (auto& [depth, c] : order) {
    Rect r;
    if (!c->IsWorldView() || !c->DrawsBeneathAll()) continue;
    SetLazyVarContext(c);
    c->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (r.x1 > r.x0 && r.y1 > r.y0) worlds.push_back(r);
  }
  auto underWorld = [&](const Control* c) {
    if (worlds.empty()) return false;
    Rect r;
    SetLazyVarContext(c);
    c->GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (r.x0 > r.x1) std::swap(r.x0, r.x1);  // (a bitmap laid out turned over draws between its edges)
    if (r.y0 > r.y1) std::swap(r.y0, r.y1);
    for (const Rect& w : worlds)
      if (r.x0 >= w.x0 && r.y0 >= w.y0 && r.x1 <= w.x1 && r.y1 <= w.y1) return true;
    return false;
  };
  for (auto& [depth, c] : order)
    if (!c->IsWorldView() && (c->renderPass & 1) && !underWorld(c)) draw(c);
  for (auto& [depth, c] : order)
    if (c->IsWorldView()) draw(c);
  for (const int pass : {4, 8})
    for (auto& [depth, c] : order)
      if (!c->IsWorldView() && (c->renderPass & pass)) draw(c);
  SetLazyVarContext(nullptr);
}

void LogDrawProfile() {
  if (g_renders == 0) return;
  std::vector<std::pair<double, std::string>> rows;
  for (const auto& [name, cost] : DrawCosts()) rows.emplace_back(cost.seconds, name);
  std::sort(rows.rbegin(), rows.rend());
  for (const auto& [seconds, name] : rows) {
    const DrawCost& cost = DrawCosts()[name];
    Logf(LogLevel::Info, "moho64ui: drawing: %-40s %7.3f ms a frame, %6.1f drawn a frame", name.c_str(), seconds * 1000.0 / g_renders,
         static_cast<double>(cost.draws) / g_renders);
  }
}

void UiManager::Forget(Control* c) {
  if (focus == c) focus = nullptr;
  if (hover == c) hover = nullptr;
  if (cursorView == c) cursorView = nullptr;
  if (engineDragger == c) engineDragger = nullptr;
  inputCapture.erase(std::remove(inputCapture.begin(), inputCapture.end(), c), inputCapture.end());
}

void UiManager::SetFocus(lua_State* L, Control* c, bool blocksKeyDown) {
  Control* old = focus;
  focus = c;
  focusBlocksKeyDown = blocksKeyDown;
  if (old && !old->dead()) old->RunScript(L, "OnKeyboardFocusChange");
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

}  // namespace

bool CurrentCursor(lua_State* L, std::string* texture, int* hotX, int* hotY, bool* hidden) {
  lua_pushstring(L, kCursorKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  Cursor* c = lua_istable(L, -1) ? dynamic_cast<Cursor*>(GetObject(L, lua_gettop(L))) : nullptr;
  lua_pop(L, 1);
  if (!c) return false;
  *texture = c->texture, *hotX = c->hotX, *hotY = c->hotY, *hidden = c->hidden;
  return true;
}

namespace {

// moho64ui_Cursor() -> texture, hotX, hotY, hidden: the mouse pointer as it stands (for tests)
int l_CursorNow(lua_State* L) {
  std::string texture;
  int hotX = 0, hotY = 0;
  bool hidden = false;
  if (!CurrentCursor(L, &texture, &hotX, &hotY, &hidden)) return 0;
  lua_pushstring(L, texture.c_str());
  lua_pushnumber(L, hotX);
  lua_pushnumber(L, hotY);
  lua_pushboolean(L, hidden);
  return 4;
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

// moho64ui_PixelAt(x, y): not an engine function of the original. The host's own function for tests:
// the first head's controls drawn now over a clear picture (no world, no mouse pointer), and the colour
// of the pixel at (x, y) as "aarrggbb"; nil outside the head.
int l_PixelAt(lua_State* L) {
  UiManager& ui = UserState::From(L)->ui();
  const int x = static_cast<int>(luaL_checknumber(L, 1)), y = static_cast<int>(luaL_checknumber(L, 2));
  if (x < 0 || y < 0 || x >= ui.width() || y >= ui.height()) {
    lua_pushnil(L);
    return 1;
  }
  SoftwareTarget target(ui.width(), ui.height());
  target.Clear(0);
  ui.Render(L, target, 0);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08x", target.pixels()[static_cast<size_t>(y) * static_cast<size_t>(ui.width()) + static_cast<size_t>(x)]);
  lua_pushstring(L, buf);
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
  lua_register(L, "moho64ui_Cursor", l_CursorNow);

  SetGlobal(L, "InternalCreateDragger", l_InternalCreateDragger);
  SetMethod(L, "CMauiLuaDragger", "Destroy", l_Dragger_Destroy);

  lua_register(L, "moho64ui_Describe", l_Describe);
  lua_register(L, "moho64ui_PixelAt", l_PixelAt);
}

}  // namespace moho::ui
