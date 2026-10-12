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
