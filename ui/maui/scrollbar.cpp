// Scrolling: what a control answers a scrollbar, and the scrollbar itself.
#include <string>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "script/script_state.h"
#include "user/bindings.h"

namespace moho::ui {

namespace {
const char* AxisName(int axis) { return axis == 0 ? "Vert" : "Horz"; }
}  // namespace

// --- a control's scroll answers come from its script unless a class overrides them ---

bool Control::IsScrollable(lua_State* L, int axis) {
  lua_pushstring(L, AxisName(axis));
  if (!RunScript(L, "IsScrollable", 1, 1)) return false;
  bool yes = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return yes;
}

void Control::GetScrollValues(lua_State* L, int axis, float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  lua_pushstring(L, AxisName(axis));
  if (!RunScript(L, "GetScrollValues", 1, 4)) return;
  bool ok = true;
  for (int i = 0; i < 4; ++i) ok = ok && lua_type(L, -4 + i) == LUA_TNUMBER;
  if (ok)
    for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(lua_tonumber(L, -4 + i));
  else
    Logf(LogLevel::Warning, "GetScrollValues must return 4 values, minRange, maxRange, minVisible, maxVisible)");
  lua_pop(L, 4);
}

void Control::ScrollLines(lua_State* L, int axis, float lines) {
  lua_pushstring(L, AxisName(axis));
  lua_pushnumber(L, lines);
  RunScript(L, "ScrollLines", 2, 0);
}

void Control::ScrollPages(lua_State* L, int axis, float pages) {
  lua_pushstring(L, AxisName(axis));
  lua_pushnumber(L, pages);
  RunScript(L, "ScrollPages", 2, 0);
}

void Control::ScrollSetTop(lua_State* L, int axis, float top) {
  lua_pushstring(L, AxisName(axis));
  lua_pushnumber(L, top);
  RunScript(L, "ScrollSetTop", 2, 0);
}

namespace {

class Scrollbar : public Control {
 public:
  const char* ClassName() const override { return "scrollbar"; }
  void Describe(std::string& out) const override {
    out += axis == 0 ? "vertical" : "horizontal";
    if (!scrollable.Get()) out += ", not connected";
  }

  // Inferred (the original's 0x7a0840 is not read yet): the background stretched over the bar; the
  // thumb covers the visible part of the connected control's range, its end pieces at their own
  // size and the middle piece stretched between them. Nothing but the background is drawn when
  // everything is visible.
  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    const Vfs* vfs = ScriptState::From(L)->vfs();
    uint32_t tint = WithAlpha(0xffffffffu, alpha);
    auto put = [&](const std::string& file, const Rect& dst) {
      if (file.empty()) return;
      const Image& img = LoadTexture(vfs, file);
      if (img.valid())
        target.DrawImage(img, Rect{1, 1, static_cast<float>(img.width - 1), static_cast<float>(img.height - 1)}, dst, tint);
    };
    put(background, r);
    Control* c = Target();
    if (!c || axis != 0) return;
    float v[4];
    c->GetScrollValues(L, axis, v);
    float range = v[1] - v[0];
    if (range <= 0 || (v[2] <= v[0] && v[3] >= v[1])) return;
    float len = r.y1 - r.y0;
    float t0 = r.y0 + len * (v[2] - v[0]) / range, t1 = r.y0 + len * (v[3] - v[0]) / range;
    float capTop = 0, capBottom = 0;
    if (!thumbTop.empty() && LoadTexture(vfs, thumbTop).valid()) capTop = static_cast<float>(LoadTexture(vfs, thumbTop).height - 2);
    if (!thumbBottom.empty() && LoadTexture(vfs, thumbBottom).valid()) capBottom = static_cast<float>(LoadTexture(vfs, thumbBottom).height - 2);
    if (t1 - t0 < capTop + capBottom) t1 = t0 + capTop + capBottom;
    put(thumbTop, Rect{r.x0, t0, r.x1, t0 + capTop});
    put(thumbMiddle, Rect{r.x0, t0 + capTop, r.x1, t1 - capBottom});
    put(thumbBottom, Rect{r.x0, t1 - capBottom, r.x1, t1});
  }

  int axis = 0;
  WeakControl scrollable;  // the control this bar scrolls
  std::string background, thumbMiddle, thumbTop, thumbBottom;

  Control* Target() const { return scrollable.Get(); }
};

// InternalCreateScrollbar(luaobj, parent, axis): axis is "Vert" or "Horz"
int l_InternalCreateScrollbar(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  const char* axis = lua_tostring(L, 3);
  Scrollbar* s = new Scrollbar;
  s->axis = axis && std::string(axis) == "Horz" ? 1 : 0;
  s->Create(L, 1, parent);
  s->RunInit(L);
  return 0;
}

// Scrollbar:SetScrollable(scrollable): the control connected to this scrollbar
int l_SetScrollable(lua_State* L) {
  Scrollbar* s = CheckControl<Scrollbar>(L, 1);
  if (lua_gettop(L) < 2 || lua_isnil(L, 2)) {
    s->scrollable.Clear();
  } else {
    CheckControl(L, 2);
    s->scrollable.Set(L, 2);
  }
  return 0;
}

// Scrollbar:SetNewTextures(background, thumbMiddle, thumbTop, thumbBottom): nil keeps a texture
int l_SetNewTextures(lua_State* L) {
  Scrollbar* s = CheckControl<Scrollbar>(L, 1);
  std::string* slots[] = {&s->background, &s->thumbMiddle, &s->thumbTop, &s->thumbBottom};
  for (int i = 0; i < 4; ++i)
    if (lua_type(L, 2 + i) == LUA_TSTRING) *slots[i] = lua_tostring(L, 2 + i);
  return 0;
}

int l_DoScrollLines(lua_State* L) {
  Scrollbar* s = CheckControl<Scrollbar>(L, 1);
  float n = static_cast<float>(luaL_checknumber(L, 2));
  if (Control* c = s->Target()) c->ScrollLines(L, s->axis, n);
  return 0;
}
int l_DoScrollPages(lua_State* L) {
  Scrollbar* s = CheckControl<Scrollbar>(L, 1);
  float n = static_cast<float>(luaL_checknumber(L, 2));
  if (Control* c = s->Target()) c->ScrollPages(L, s->axis, n);
  return 0;
}

}  // namespace

void RegisterScrollbarBindings(lua_State* L) {
  SetGlobal(L, "InternalCreateScrollbar", l_InternalCreateScrollbar);
  SetMethod(L, "CMauiScrollbar", "SetScrollable", l_SetScrollable);
  SetMethod(L, "CMauiScrollbar", "SetNewTextures", l_SetNewTextures);
  SetMethod(L, "CMauiScrollbar", "DoScrollLines", l_DoScrollLines);
  SetMethod(L, "CMauiScrollbar", "DoScrollPages", l_DoScrollPages);
}

}  // namespace moho::ui
