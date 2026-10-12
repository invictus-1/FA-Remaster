// Edit: a one-line text field. This is its state as scripts see it; typing arrives with input.
#include <algorithm>
#include <string>

#include "maui/control.h"
#include "maui/font.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

int Utf8Len(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xc0) != 0x80) ++n;
  return n;
}

// The first `count` characters of a UTF-8 string.
std::string Utf8Prefix(const std::string& s, int count) {
  size_t i = 0;
  for (int n = 0; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80 && n++ == count) break;
  return s.substr(0, i);
}

class Edit : public Control {
 public:
  const char* ClassName() const override { return "edit"; }
  void Describe(std::string& out) const override {
    out += "\"" + text + "\" " + family + " " + std::to_string(size) + (enabled ? "" : " input off");
  }

  std::string text, family;
  int size = 0;
  int maxChars = 512;  // inferred default; scripts set their own
  int caret = 0;       // in characters
  std::string foreground, background, caretColor, highlightForeground, highlightBackground;
  bool showBackground = false, showCaret = true, enabled = true, dropShadow = false;
  float caretCycleSeconds = 0;
  int caretMinAlpha = 0, caretMaxAlpha = 255;
  bool inTextChanged = false;

  // SetText (FA exe 0x78f380): the text is cut to the maximum length, the caret goes to its end,
  // and the script hears self:OnTextChanged(newText, oldText), also when nothing changed. A
  // SetText from inside that callback does not call it again.
  void SetText(lua_State* L, const std::string& value) {
    std::string old = text;
    text = Utf8Prefix(value, maxChars);
    caret = Utf8Len(text);
    if (inTextChanged) return;
    inTextChanged = true;
    lua_pushstring(L, text.c_str());
    lua_pushstring(L, old.c_str());
    RunScript(L, "OnTextChanged", 2, 0);
    inTextChanged = false;
  }
};

int l_InternalCreateEdit(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  Edit* c = new Edit;
  c->Create(L, 1, parent);
  c->RunInit(L);
  return 0;
}

Edit* Self(lua_State* L) { return CheckControl<Edit>(L, 1); }

int l_SetText(lua_State* L) {
  Edit* c = Self(L);
  c->SetText(L, luaL_checkstring(L, 2));
  return 0;
}
int l_ClearText(lua_State* L) {
  Self(L)->SetText(L, "");
  return 0;
}
int l_GetText(lua_State* L) {
  lua_pushstring(L, Self(L)->text.c_str());
  return 1;
}

int l_SetNewFont(lua_State* L) {
  Edit* c = Self(L);
  c->family = luaL_checkstring(L, 2);
  c->size = static_cast<int>(luaL_checknumber(L, 3));
  return 0;
}
// int GetFontHeight(): the font's height (inferred: the same height an item list's rows use).
int l_GetFontHeight(lua_State* L) {
  Edit* c = Self(L);
  lua_pushnumber(L, c->family.empty() ? 0 : GetFontMetrics(c->family, c->size).height);
  return 1;
}
int l_GetStringAdvance(lua_State* L) {
  Edit* c = Self(L);
  lua_pushnumber(L, StringAdvance(c->family, c->size, luaL_checkstring(L, 2)));
  return 1;
}

int l_SetMaxChars(lua_State* L) {
  Self(L)->maxChars = std::max(0, static_cast<int>(luaL_checknumber(L, 2)));
  return 0;
}
int l_GetMaxChars(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(Self(L)->maxChars));
  return 1;
}
int l_SetCaretPosition(lua_State* L) {
  Edit* c = Self(L);
  c->caret = std::max(0, std::min(static_cast<int>(luaL_checknumber(L, 2)), Utf8Len(c->text)));
  return 0;
}
int l_GetCaretPosition(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(Self(L)->caret));
  return 1;
}

template <std::string Edit::*Field>
int l_SetColor(lua_State* L) {
  Self(L)->*Field = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}
template <std::string Edit::*Field>
int l_GetColor(lua_State* L) {
  lua_pushstring(L, (Self(L)->*Field).c_str());
  return 1;
}
template <bool Edit::*Field>
int l_SetFlag(lua_State* L) {
  Self(L)->*Field = lua_toboolean(L, 2) != 0;
  return 0;
}
template <bool Edit::*Field>
int l_GetFlag(lua_State* L) {
  lua_pushboolean(L, Self(L)->*Field);
  return 1;
}

int l_EnableInput(lua_State* L) {
  Self(L)->enabled = true;
  return 0;
}
int l_DisableInput(lua_State* L) {
  Self(L)->enabled = false;
  return 0;
}

// SetCaretCycle(seconds, minAlpha, maxAlpha): how the caret blinks
int l_SetCaretCycle(lua_State* L) {
  Edit* c = Self(L);
  c->caretCycleSeconds = static_cast<float>(luaL_checknumber(L, 2));
  c->caretMinAlpha = static_cast<int>(luaL_optnumber(L, 3, 0));
  c->caretMaxAlpha = static_cast<int>(luaL_optnumber(L, 4, 255));
  return 0;
}

int l_AcquireFocus(lua_State* L) {
  UserState::From(L)->ui().SetFocus(L, Self(L));
  return 0;
}
int l_AbandonFocus(lua_State* L) {
  Edit* c = Self(L);
  UiManager& ui = UserState::From(L)->ui();
  if (ui.focus == c) ui.SetFocus(L, nullptr);
  return 0;
}

}  // namespace

void RegisterEditBindings(lua_State* L) {
  const char* cls = "CMauiEdit";
  SetGlobal(L, "InternalCreateEdit", l_InternalCreateEdit);
  SetMethod(L, cls, "SetText", l_SetText);
  SetMethod(L, cls, "ClearText", l_ClearText);
  SetMethod(L, cls, "GetText", l_GetText);
  SetMethod(L, cls, "SetNewFont", l_SetNewFont);
  SetMethod(L, cls, "GetFontHeight", l_GetFontHeight);
  SetMethod(L, cls, "GetStringAdvance", l_GetStringAdvance);
  SetMethod(L, cls, "SetMaxChars", l_SetMaxChars);
  SetMethod(L, cls, "GetMaxChars", l_GetMaxChars);
  SetMethod(L, cls, "SetCaretPosition", l_SetCaretPosition);
  SetMethod(L, cls, "GetCaretPosition", l_GetCaretPosition);
  SetMethod(L, cls, "SetNewForegroundColor", l_SetColor<&Edit::foreground>);
  SetMethod(L, cls, "GetForegroundColor", l_GetColor<&Edit::foreground>);
  SetMethod(L, cls, "SetNewBackgroundColor", l_SetColor<&Edit::background>);
  SetMethod(L, cls, "GetBackgroundColor", l_GetColor<&Edit::background>);
  SetMethod(L, cls, "SetNewCaretColor", l_SetColor<&Edit::caretColor>);
  SetMethod(L, cls, "GetCaretColor", l_GetColor<&Edit::caretColor>);
  SetMethod(L, cls, "SetNewHighlightForegroundColor", l_SetColor<&Edit::highlightForeground>);
  SetMethod(L, cls, "GetHighlightForegroundColor", l_GetColor<&Edit::highlightForeground>);
  SetMethod(L, cls, "SetNewHighlightBackgroundColor", l_SetColor<&Edit::highlightBackground>);
  SetMethod(L, cls, "GetHighlightBackgroundColor", l_GetColor<&Edit::highlightBackground>);
  SetMethod(L, cls, "ShowBackground", l_SetFlag<&Edit::showBackground>);
  SetMethod(L, cls, "IsBackgroundVisible", l_GetFlag<&Edit::showBackground>);
  SetMethod(L, cls, "ShowCaret", l_SetFlag<&Edit::showCaret>);
  SetMethod(L, cls, "IsCaretVisible", l_GetFlag<&Edit::showCaret>);
  SetMethod(L, cls, "SetDropShadow", l_SetFlag<&Edit::dropShadow>);
  SetMethod(L, cls, "IsEnabled", l_GetFlag<&Edit::enabled>);
  SetMethod(L, cls, "EnableInput", l_EnableInput);
  SetMethod(L, cls, "DisableInput", l_DisableInput);
  SetMethod(L, cls, "SetCaretCycle", l_SetCaretCycle);
  SetMethod(L, cls, "AcquireFocus", l_AcquireFocus);
  SetMethod(L, cls, "AbandonFocus", l_AbandonFocus);
}

}  // namespace moho::ui
