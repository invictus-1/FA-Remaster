// Scrolling: what a control answers a scrollbar, and the scrollbar itself.
#include <cmath>
#include <string>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "maui/input.h"
#include "script/script_state.h"
#include "user/bindings.h"
#include "user/user_state.h"

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

  // The thumb's ends along the bar (FA exe 0x7a0a01, the same in HandleEvent 0x7a1235): the visible part of
  // the connected control's range laid over the bar, from `lo` to `hi`; the whole bar when the range is
  // empty (also when no control is connected: its values are all 0). The original takes Top to Bottom
  // for the vertical axis, and Right to Left for the horizontal one, as it reads.
  void Thumb(lua_State* L, float& lo, float& hi, float& t0, float& t1, float v[4]) {
    if (axis == 0) lo = top.Get(), hi = bottom.Get();
    else lo = right.Get(), hi = left.Get();
    v[0] = v[1] = v[2] = v[3] = 0;
    if (Control* c = Target()) c->GetScrollValues(L, axis, v);
    t0 = lo, t1 = hi;
    if (v[1] > v[0]) {
      t0 = (hi - lo) * ((v[2] - v[0]) / (v[1] - v[0])) + lo;
      t1 = (hi - lo) * ((v[3] - v[0]) / (v[1] - v[0])) + lo;
    }
  }

  // As the original draws it (FA exe 0x7a0840): the background stretched over the bar; then, when the thumb's
  // three textures are all set, the thumb from t0 to t1 (each rounded down), also when everything is visible
  // (then it fills the bar). A thumb shorter than its two end pieces grows about its centre to their size.
  // The top piece from t0 down by its height, the bottom piece up from t1 by its height, and, only when the
  // thumb is longer than the two, the middle piece stretched from 1 pixel inside the top piece to the bottom
  // piece. Only a vertical bar's thumb is drawn here (the horizontal one is not done; no script makes one).
  void Draw(lua_State* L, DrawTarget& target) override {
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    const Vfs* vfs = ScriptState::From(L)->vfs();
    uint32_t tint = WithAlpha(0xffffffffu, alpha);
    auto put = [&](const std::string& file, const Rect& dst) {
      const Image& img = LoadTexture(vfs, file);
      if (img.valid())
        target.DrawImage(img, Rect{1, 1, static_cast<float>(img.width - 1), static_cast<float>(img.height - 1)}, dst, tint);
    };
    if (!background.empty()) put(background, r);
    if (thumbTop.empty() || thumbBottom.empty() || thumbMiddle.empty() || axis != 0) return;
    float lo, hi, t0, t1, v[4];
    Thumb(L, lo, hi, t0, t1, v);
    t0 = std::floor(t0), t1 = std::floor(t1);
    // a piece's size is its file's less the 1 pixel border (as GetTextureDimensions gives it)
    auto length = [&](const std::string& file) {
      const Image& img = LoadTexture(vfs, file);
      return img.valid() ? static_cast<float>(img.height - 2) : 0.0f;
    };
    float capTop = length(thumbTop), capBottom = length(thumbBottom);
    if (capTop + capBottom >= t1 - t0) {
      float mid = t1 - (t1 - t0) * 0.5f;
      t0 = mid - capTop, t1 = mid + capBottom;
    }
    put(thumbTop, Rect{r.x0, t0, r.x1, t0 + capTop});
    put(thumbBottom, Rect{r.x0, t1 - capBottom, r.x1, t1});
    if (std::fabs(t1 - t0) > capTop + capBottom) put(thumbMiddle, Rect{r.x0, t0 + capTop - 1, r.x1, t1 - capBottom});
  }

  // A press (FA exe 0x7a11c0; a double click counts as one) of the left button (the event's KeyCode 1): with a
  // control connected, before the thumb a page back (ScrollPages -1), on the thumb the drag starts (the press's
  // place and the first visible value are kept, and the bar is posted as the dragger for that button), past
  // it a page on (+1); before the bar's start nothing. The wheel scrolls a vertical bar's control a line per
  // notch. The bar does not ask its script (no self:HandleEvent): other events go on to its parent.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    if (dead()) return true;
    if ((e.type == EventType::ButtonPress || e.type == EventType::ButtonDClick) && e.keyCode == 1) {
      Control* c = Target();
      if (!c) return true;
      float lo, hi, t0, t1, v[4];
      Thumb(L, lo, hi, t0, t1, v);
      float at = axis == 0 ? e.y : e.x;
      if (lo > at) return true;
      if (t0 > at) {
        c->ScrollPages(L, axis, -1.0f);
      } else if (t1 > at) {
        pressAt = at, pressVisible = v[2];
        UserState::From(L)->ui().PostEngineDragger(L, this, e.keyCode);
      } else if (hi > at) {
        c->ScrollPages(L, axis, 1.0f);
      }
      return true;
    }
    if (e.type == EventType::WheelRotation) {
      if (Control* c = Target(); c && axis == 0) c->ScrollLines(L, 0, e.wheelRotation <= 0 ? 1.0f : -1.0f);
      return true;
    }
    return false;
  }

  // The drag (FA exe 0x7a1410): the control's first visible value goes with the mouse, the bar's length
  // standing for the control's whole range: ScrollSetTop(first visible at the press + (range / (Bottom - Top))
  // * the mouse's way since the press). The original takes Top and Bottom for both axes. The release and a
  // cancel do nothing more (0x7a1500, 0x7a1510).
  void DragMove(lua_State* L, const UiEvent& e) override {
    Control* c = Target();
    if (!c || dead()) return;
    float way = (axis == 0 ? e.y : e.x) - pressAt;
    float v[4];
    c->GetScrollValues(L, axis, v);
    float scale = (v[1] - v[0]) / (bottom.Get() - top.Get());
    c->ScrollSetTop(L, axis, scale * way + pressVisible);
  }

  int axis = 0;
  WeakControl scrollable;  // the control this bar scrolls
  std::string background, thumbMiddle, thumbTop, thumbBottom;
  float pressAt = 0, pressVisible = 0;  // the thumb's press: its place along the bar, the first visible value (+0x14c, +0x150)

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
