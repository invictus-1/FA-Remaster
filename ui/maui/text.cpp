// Text: one line of text in one font and colour.
#include "maui/control.h"
#include "maui/font.h"
#include "user/bindings.h"

namespace moho::ui {
namespace {

class Text : public Control {
 public:
  const char* ClassName() const override { return "text"; }
  void Describe(std::string& out) const override {
    out += "\"" + text + "\" " + family + " " + std::to_string(pointSize) + (color.empty() ? "" : " " + color);
  }

  // Draw (FA exe 0x7a3040). The pen starts at Left, or with horizontal centring at
  // Left + Width/2 - advance/2. The baseline is Top + ascent, or with vertical centring
  // Top + Height/2 + (ascent - internal leading)/2. A drop shadow is the same text in black, one
  // pixel right and down, drawn first. With clip-to-width only the characters that fit are drawn.
  void Draw(lua_State*, DrawTarget& target) override {
    if (family.empty() || text.empty()) return;
    FontMetrics m = GetFontMetrics(family, pointSize);
    float w = width.Get();
    float x = left.Get();
    if (centeredHorizontally) x = x + w * 0.5f - StringAdvance(family, pointSize, text) * 0.5f;
    float baseline = centeredVertically ? top.Get() + height.Get() * 0.5f + (m.ascent - m.internalLeading) * 0.5f
                                        : top.Get() + m.ascent;
    uint32_t c = WithAlpha(color.empty() ? 0xffffffffu : ParseColor(color), alpha);
    float maxAdvance = clipToWidth ? w : -1.0f;
    if (dropShadow) DrawString(target, family, pointSize, text, x + 1, baseline + 1, c & 0xff000000u, maxAdvance);
    DrawString(target, family, pointSize, text, x, baseline, c, maxAdvance);
  }

  LazyVar textAdvance, fontAscent, fontDescent, fontExternalLeading;
  std::string text, family, color;
  int pointSize = 0;
  bool dropShadow = false, centeredHorizontally = false, centeredVertically = false, clipToWidth = false;

  // The four LazyVars exist before OnInit runs: the script's OnInit builds Height and Width on them.
  void MakeVars(lua_State* L) {
    AddLazyVar(L, textAdvance, "TextAdvance");
    AddLazyVar(L, fontAscent, "FontAscent");
    AddLazyVar(L, fontDescent, "FontDescent");
    AddLazyVar(L, fontExternalLeading, "FontExternalLeading");
  }
  void UpdateAdvance() { textAdvance.Set(StringAdvance(family, pointSize, text)); }
};

// InternalCreateText(luaobj, parent)
int l_InternalCreateText(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Text* t = new Text;
  t->Create(L, 1, parent);
  t->MakeVars(L);
  t->RunInit(L);
  return 0;
}

// Text:SetNewFont(family, pointsize)
int l_SetNewFont(lua_State* L) {
  Text* t = CheckControl<Text>(L, 1);
  t->family = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  t->pointSize = static_cast<int>(lua_tonumber(L, 3));
  FontMetrics m = GetFontMetrics(t->family, t->pointSize);
  t->fontAscent.Set(m.ascent);
  t->fontDescent.Set(m.descent);
  t->fontExternalLeading.Set(m.externalLeading);
  t->UpdateAdvance();
  return 0;
}

int l_SetText(lua_State* L) {
  Text* t = CheckControl<Text>(L, 1);
  lua_getglobal(L, "tostring");  // scripts pass numbers too
  lua_pushvalue(L, 2);
  lua_call(L, 1, 1);
  t->text = lua_isnil(L, 2) ? "" : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
  t->UpdateAdvance();
  return 0;
}
int l_GetText(lua_State* L) {
  lua_pushstring(L, CheckControl<Text>(L, 1)->text.c_str());
  return 1;
}
int l_SetNewColor(lua_State* L) {
  CheckControl<Text>(L, 1)->color = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}
int l_SetDropShadow(lua_State* L) {
  CheckControl<Text>(L, 1)->dropShadow = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetCenteredHorizontally(lua_State* L) {
  CheckControl<Text>(L, 1)->centeredHorizontally = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetCenteredVertically(lua_State* L) {
  CheckControl<Text>(L, 1)->centeredVertically = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetNewClipToWidth(lua_State* L) {
  CheckControl<Text>(L, 1)->clipToWidth = lua_toboolean(L, 2) != 0;
  return 0;
}
// number Text:GetStringAdvance(string): the width of a string in this control's font
int l_GetStringAdvance(lua_State* L) {
  Text* t = CheckControl<Text>(L, 1);
  lua_pushnumber(L, StringAdvance(t->family, t->pointSize, luaL_checkstring(L, 2)));
  return 1;
}

}  // namespace

void RegisterTextBindings(lua_State* L) {
  const char* cls = "CMauiText";
  SetGlobal(L, "InternalCreateText", l_InternalCreateText);
  SetMethod(L, cls, "SetNewFont", l_SetNewFont);
  SetMethod(L, cls, "SetText", l_SetText);
  SetMethod(L, cls, "GetText", l_GetText);
  SetMethod(L, cls, "SetNewColor", l_SetNewColor);
  SetMethod(L, cls, "SetDropShadow", l_SetDropShadow);
  SetMethod(L, cls, "SetCenteredHorizontally", l_SetCenteredHorizontally);
  SetMethod(L, cls, "SetCenteredVertically", l_SetCenteredVertically);
  SetMethod(L, cls, "SetNewClipToWidth", l_SetNewClipToWidth);
  SetMethod(L, cls, "GetStringAdvance", l_GetStringAdvance);
}

}  // namespace moho::ui
