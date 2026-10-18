// Edit: a one-line text field with a caret, a selection and a window onto text that is wider
// than the control. Typing, the clipboard keys, mouse selection and drawing follow the original
// (FA exe: key handling 0x791780, clicks 0x7915c0, dragging 0x7913a0, drawing 0x78f820, starting
// values 0x78f040). Positions (caret, selection, window) count characters, not bytes.
#include <algorithm>
#include <string>

#include "maui/clipboard.h"
#include "maui/control.h"
#include "maui/font.h"
#include "maui/input.h"
#include "user/bindings.h"
#include "user/keymap.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

int Utf8Len(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xc0) != 0x80) ++n;
  return n;
}

// The byte offset of character `index` (the string's size when there are fewer characters).
size_t Utf8Offset(const std::string& s, int index) {
  size_t i = 0;
  for (int n = 0; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80 && n++ == index) break;
  return i;
}

std::string Utf8Sub(const std::string& s, int first, int count) {
  if (count <= 0) return std::string();
  size_t a = Utf8Offset(s, first), b = Utf8Offset(s, first + count);
  return s.substr(a, b - a);
}

std::string Utf8Encode(unsigned code) {
  std::string out;
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xc0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3f));
  } else {
    out += static_cast<char>(0xe0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (code & 0x3f));
  }
  return out;
}

// Word boundaries for Ctrl with the arrow keys and for a double click. The original asks its
// string library (0x9384a0, 0x938570) with a set of separating characters that is filled in at
// run time; inferred: blanks separate words.
bool IsSeparator(const std::string& s, int index) {
  size_t o = Utf8Offset(s, index);
  return o < s.size() && (s[o] == ' ' || s[o] == '\t');
}
// The start of the word at or before `pos`.
int WordStart(const std::string& s, int pos) {
  if (Utf8Len(s) <= 1) return 0;
  while (pos > 0 && IsSeparator(s, pos - 1)) --pos;
  while (pos > 0 && !IsSeparator(s, pos - 1)) --pos;
  return pos;
}
// The start of the word after `pos`: past the rest of this word, then past the separators.
int NextWordStart(const std::string& s, int pos) {
  int len = Utf8Len(s);
  while (pos < len && !IsSeparator(s, pos)) ++pos;
  while (pos < len && IsSeparator(s, pos)) ++pos;
  return pos;
}

enum : int { kBackspace = 8, kEnter = 13, kEscape = 27, kDelete = 127, kEnd = 314, kHome = 315, kLeft = 316, kRight = 318, kInsert = 324 };

class Edit : public Control {
 public:
  const char* ClassName() const override { return "edit"; }
  void Describe(std::string& out) const override {
    out += "\"" + text + "\" " + family + " " + std::to_string(size) + " caret " + std::to_string(caret);
    if (selStart != selEnd) out += " selection " + std::to_string(selStart) + ".." + std::to_string(selEnd);
    if (clipOffset) out += " from " + std::to_string(clipOffset);
    if (!enabled) out += " input off";
  }

  // The original's starting values (0x78f040): white text on a shown black background, the
  // selection black on white, input on, no caret until the field has the focus, a caret cycle of
  // 1.5 seconds between alpha 62 and 255, at most 1024 characters, and frame updates on. And a
  // font, Courier New at 14 (READ 2026-10-18, the constructor 0x78efe0 at 0x78f134: CD3DFont::Create of
  // "Courier New" 0xe33358 and 14, given to ApplyFontAndRefreshClip 0x78f620), so a new field has
  // a height (GetFontHeight) before its script sets a font: the saved games' dialog sizes its
  // filename field by it before its thread sets Arial (/lua/ui/controls/filepicker.lua).
  Edit() { needsFrameUpdate = true; }

  std::string text, family = "Courier New";
  int size = 14;
  int maxChars = 1024;
  int caret = 0;
  int clipOffset = 0, visible = 0;  // the window onto the text: its first character and how many are shown
  int selStart = 0, selEnd = 0;     // equal when nothing is selected
  int dragAnchor = 0;               // where a mouse selection began
  std::string foreground = "ffffffff", background = "ff000000", caretColor = "fffefefe";
  std::string highlightForeground = "ff000000", highlightBackground = "ffffffff";
  bool showBackground = true, showCaret = false, enabled = true, dropShadow = false;
  float caretCycleSeconds = 1.5f, caretTimer = 0;
  int caretMinAlpha = 62, caretMaxAlpha = 255, caretAlpha = 255;
  bool inTextChanged = false;

  int Len() const { return Utf8Len(text); }
  float Advance(const std::string& s) const { return StringAdvance(family, size, s); }
  float CharAdvance(int index) const { return Advance(Utf8Sub(text, index, 1)); }
  std::string Shown() const { return Utf8Sub(text, clipOffset, visible); }
  std::string Selection() const { return Utf8Sub(text, selStart, selEnd - selStart); }

  // self:OnTextChanged(newText, oldText); a change made from inside that callback does not call
  // it again (the original's guard at +0x190).
  void Changed(lua_State* L, const std::string& old) {
    if (inTextChanged) return;
    inTextChanged = true;
    lua_pushstring(L, text.c_str());
    lua_pushstring(L, old.c_str());
    RunScript(L, "OnTextChanged", 2, 0);
    inTextChanged = false;
  }
  // self:<name>(text): true when the script says it handled the key.
  bool AskScript(lua_State* L, const char* fn) {
    lua_pushstring(L, text.c_str());
    if (!RunScript(L, fn, 1, 1)) return false;
    bool handled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return handled;
  }

  // The window, set from its right end (0x790d20): text that fits is shown whole; otherwise the
  // window ends `fromEnd` characters before the text's end and reaches left as far as whole
  // characters fit into the control's width.
  void ClipRight(int fromEnd) {
    if (family.empty()) return;
    int len = Len();
    if (text.empty()) {  // nothing to fit: the width is not asked for (it may not be laid out yet)
      visible = 0;
      return;
    }
    float w = 0;
    if (!width.TryGet(&w) || !(Advance(text) > w)) {  // a field not laid out yet counts as wide enough
      visible = len;
      return;
    }
    int end = len - std::min(fromEnd, len), start = end;
    float sum = 0;
    while (start > 0) {
      sum += CharAdvance(start - 1);
      if (sum > w) break;
      --start;
    }
    clipOffset = start;
    visible = end - start;
  }
  // The window, set from its left end (0x790f90): it starts at `offset` and takes the characters
  // up to and with the first one that reaches past the control's width.
  void ClipLeft(int offset) {
    clipOffset = offset;
    if (family.empty()) return;
    int len = Len();
    if (text.empty()) {  // nothing to fit: the width is not asked for (it may not be laid out yet)
      visible = 0;
      return;
    }
    float w = 0;
    if (!width.TryGet(&w) || !(Advance(text) > w)) {  // a field not laid out yet counts as wide enough
      visible = len;
      return;
    }
    float sum = 0;
    int n = 0;
    for (int i = std::min(offset, len); i < len; ++i) {
      sum += CharAdvance(i);
      ++n;
      if (sum > w) break;
    }
    visible = n;
  }

  // Move the caret (0x7911b0); the window follows when the caret leaves it.
  void SetCaret(int pos) {
    if (pos < caret) {
      caret = pos;
      if (pos < clipOffset) ClipLeft(pos);
    } else if (pos > caret) {
      caret = std::min(pos, Len());
      if (caret >= clipOffset + visible) ClipRight(Len() - caret);
    }
  }
  void MoveCaretLeft(int n) {
    if (caret) SetCaret(caret - std::min(n, caret));
  }
  // Shift with a movement key (0x791290, 0x791310): the selection starts at the caret when there
  // is none, and its end that the caret is on moves with the caret.
  void MoveSelectionLeft(int n) {
    if (selStart == selEnd) selStart = selEnd = caret;
    int old = caret;
    if (caret) SetCaret(caret - std::min(n, caret));
    if (old == caret) return;
    if (selStart == caret) selStart = selEnd = 0;
    else if (selStart >= caret) selStart = caret;
    else selEnd = caret;
  }
  void MoveSelectionRight(int n) {
    if (selStart == selEnd) selStart = selEnd = caret;
    int old = caret;
    SetCaret(caret + n);
    if (old == caret) return;
    if (selEnd == caret) selStart = selEnd = 0;
    else if (selEnd <= caret) selEnd = caret;
    else selStart = caret;
  }

  // Remove the selected text (0x790590); the caret goes to where it was.
  void DeleteSelection(lua_State* L, bool quiet) {
    if (selStart == selEnd) return;
    std::string old = text;
    selEnd = std::min(selEnd, Len());
    caret = selStart;
    size_t a = Utf8Offset(text, selStart), b = Utf8Offset(text, selEnd);
    text.erase(a, b - a);
    if (caret < clipOffset) clipOffset = caret;
    selStart = selEnd = 0;
    if (!quiet) Changed(L, old);
  }
  // Delete and Backspace (0x790b40): the selection when there is one, else the character after
  // or before the caret. The script hears OnTextChanged also when nothing could be deleted, and
  // twice when a selection went (once from the selection's removal, once from here).
  void DeleteChar(lua_State* L, bool forward) {
    std::string old = text;
    if (selStart != selEnd) {
      DeleteSelection(L, false);
      ClipLeft(clipOffset);
    } else if (forward) {
      if (caret != Len()) {
        size_t a = Utf8Offset(text, caret), b = Utf8Offset(text, caret + 1);
        text.erase(a, b - a);
      }
      ClipLeft(clipOffset);
    } else if (caret != 0) {
      size_t a = Utf8Offset(text, caret - 1), b = Utf8Offset(text, caret);
      text.erase(a, b - a);
      SetCaret(caret - 1);
      if (caret != 0 && caret == clipOffset) ClipLeft(clipOffset - 1);
    }
    Changed(L, old);
  }
  // Typed and pasted text (0x790830): it takes the selection's place, cut to what the maximum
  // length leaves (measured before the selection goes); a full field takes nothing.
  void ReplaceSelection(lua_State* L, std::string ins) {
    int len = Len();
    if (len + Utf8Len(ins) > maxChars) {
      if (len >= maxChars) return;
      ins = Utf8Sub(ins, 0, maxChars - len);
    }
    DeleteSelection(L, false);
    std::string old = text;
    if (!ins.empty()) {
      int n = Utf8Len(ins);
      if (caret == Len()) {
        text += ins;
        caret += n;
        visible += n;
        ClipRight(0);
      } else {
        text.insert(Utf8Offset(text, caret), ins);
        caret += n;
        if (caret < clipOffset + visible) ClipLeft(clipOffset);
        else ClipRight(Len() - caret);
      }
    }
    Changed(L, old);
  }

  // SetText (0x78f380): the text is cut to the maximum length, the caret goes to its end, and
  // the script hears self:OnTextChanged(newText, oldText), also when nothing changed. Inferred:
  // the selection goes and the window shows the text's end.
  void SetText(lua_State* L, const std::string& value) {
    std::string old = text;
    text = Utf8Sub(value, 0, maxChars);
    caret = Len();
    selStart = selEnd = 0;
    clipOffset = 0;
    visible = caret;
    ClipRight(0);
    Changed(L, old);
  }

  // A character event (0x791780). Key downs and ups are left alone, so the host turns the key
  // down into this character event.
  void HandleKey(lua_State* L, const UiEvent& e) {
    int key = e.keyCode;
    switch (key) {
      case kBackspace:
        if (e.alt) return;
        if (e.ctrl) {
          int n = caret - WordStart(text, caret);
          if (n) MoveSelectionLeft(n);
        } else {
          DeleteChar(L, false);
        }
        DeleteSelection(L, true);
        return;
      case kDelete:
        if (e.shift) {
          CopyToClipboard(Selection());
        } else if (e.ctrl) {
          int n = NextWordStart(text, caret) - caret;
          if (n) MoveSelectionRight(n);
        } else {
          DeleteChar(L, true);
        }
        DeleteSelection(L, true);
        return;
      case kEnter:
        if (!AskScript(L, "OnEnterPressed")) SetText(L, "");
        return;
      case kEscape:
        if (AskScript(L, "OnEscPressed")) return;
        if (text.empty()) AbandonKeyboardFocus(L);
        else SetText(L, "");
        return;
      case kEnd:
        if (e.shift) {
          if (int n = Len() - caret) MoveSelectionRight(n);
        } else {
          caret = Len();
          selStart = selEnd = 0;
          ClipRight(0);
        }
        return;
      case kHome:
        if (e.shift) {
          if (caret) MoveSelectionLeft(caret);
        } else {
          caret = 0;
          selStart = selEnd = 0;
          ClipLeft(0);
        }
        return;
      case kLeft:
        if (e.shift && e.ctrl) {
          if (int n = caret - WordStart(text, caret)) MoveSelectionLeft(n);
        } else if (e.shift) {
          MoveSelectionLeft(1);
        } else {
          if (e.ctrl) SetCaret(WordStart(text, caret));
          else MoveCaretLeft(1);
          selStart = selEnd = 0;
        }
        return;
      case kRight:
        if (e.shift && e.ctrl) {
          if (int n = NextWordStart(text, caret) - caret) MoveSelectionRight(n);
        } else if (e.shift) {
          MoveSelectionRight(1);
        } else {
          SetCaret(e.ctrl ? NextWordStart(text, caret) : caret + 1);
          selStart = selEnd = 0;
        }
        return;
      case kInsert:
        if (e.shift) ReplaceSelection(L, ClipboardText());
        else if (e.ctrl) CopyToClipboard(Selection());
        return;
      default:
        break;
    }
    if (e.ctrl) {
      if (key == 3) {  // Ctrl+C
        CopyToClipboard(Selection());
      } else if (key == 22) {  // Ctrl+V
        ReplaceSelection(L, ClipboardText());
      } else if (key == 24) {  // Ctrl+X
        CopyToClipboard(Selection());
        DeleteSelection(L, true);
      }
      return;
    }
    // A key without a character: self:OnNonTextKeyPressed(code, event), the code as Windows has it
    // (READ 0x791ed6: a key code above 300 goes through wxCharCodeWXToMSW 0x96afc0, our
    // KeycodeMauiToMSW, before 0x790510 calls the script; the event keeps the original's own code).
    // So Up arrives as 38, not 317: what FAF's scripts compare with (UIUtil.VK_UP, the lobby's chat
    // line recalling what was sent, the console's history). A code the table does not know (a
    // character above 300 typed in another script) comes as it is. (A typed character comes with
    // its UTF-16 code, READ 0x96cd80: the window's WM_CHAR hands it on as it is. So one the table
    // knows, as Polish 's' with acute 347, comes as a key's code, F6 117, in the original too, and
    // FAF's AddUnicodeCharToEditText, which takes codes above 127 for characters, drops it.)
    if (key > 300) {
      lua_pushnumber(L, static_cast<lua_Number>(KeycodeMauiToMSW(key)));
      PushEventTable(L, this, e);
      RunScript(L, "OnNonTextKeyPressed", 2, 0);
      return;
    }
    // self:OnCharPressed(charCode): true keeps the character out of the text
    lua_pushnumber(L, static_cast<lua_Number>(key));
    bool swallowed = false;
    if (RunScript(L, "OnCharPressed", 1, 1)) {
      swallowed = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
    }
    if (!swallowed) ReplaceSelection(L, Utf8Encode(static_cast<unsigned>(key) & 0xffffu));
  }

  // The character of the shown text that a horizontal position falls on (0x426680): characters
  // are counted while their summed advances stay within the distance.
  int CharAt(float x) const {
    std::string shown = Shown();
    int n = Utf8Len(shown), index = 0;
    float sum = 0;
    for (; index < n; ++index) {
      sum += Advance(Utf8Sub(shown, index, 1));
      if (sum > x) break;
    }
    return index;
  }

  // A left click (0x7915c0): the field takes the keyboard focus and shows its caret; a press puts
  // the caret at the click and starts a mouse selection; a double click selects the word.
  void HandleClick(lua_State* L, const UiEvent& e) {
    if (!e.left) return;
    UiManager& ui = UserState::From(L)->ui();
    if (enabled) {
      showCaret = true;
      ui.SetFocus(L, this, true);
    }
    int index = CharAt(e.x - left.Get());
    if (e.type == EventType::ButtonPress) {
      ui.PostEngineDragger(L, this, e.keyCode);
      dragAnchor = clipOffset + index;
      SetCaret(dragAnchor);
    } else {
      std::string shown = Shown();
      int a = WordStart(shown, index), b = a, n = Utf8Len(shown);
      while (b < n && !IsSeparator(shown, b)) ++b;
      selStart = clipOffset + a;
      selEnd = clipOffset + b;
      SetCaret(selEnd);
    }
  }
  // The mouse moves with the button held (0x7913a0): the selection runs from where the press was
  // to the character under the cursor.
  void DragMove(lua_State*, const UiEvent& e) override {
    int index = clipOffset + CharAt(e.x - left.Get());
    if (index == dragAnchor) {
      selStart = selEnd = 0;
      return;
    }
    selStart = std::min(index, dragAnchor);
    selEnd = std::max(index, dragAnchor);
    SetCaret(index);
  }
  void DragRelease(lua_State*, const UiEvent& e) override {
    if (clipOffset + CharAt(e.x - left.Get()) == dragAnchor) selStart = selEnd = 0;
  }

  // An edit field does its own work with events and passes none to its script (0x790470): button
  // presses and double clicks, and characters. It never says it handled one.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    if (e.type == EventType::ButtonPress || e.type == EventType::ButtonDClick) HandleClick(L, e);
    else if (e.type == EventType::Char) HandleKey(L, e);
    return false;
  }

  // Each frame (0x78f720): the script's OnFrame, then the caret's alpha, which runs from its
  // lowest value up to its highest and back once per cycle.
  void FrameUpdate(lua_State* L, float seconds) override {
    Control::FrameUpdate(L, seconds);
    if (dead()) return;
    caretTimer += seconds;
    if (caretTimer > caretCycleSeconds) caretTimer = 0;
    if (caretCycleSeconds <= 0) {
      caretAlpha = caretMaxAlpha;
      return;
    }
    float f = caretTimer > caretCycleSeconds * 0.5f ? (caretCycleSeconds - caretTimer) / caretCycleSeconds * 2.0f
                                                    : caretTimer / caretCycleSeconds * 2.0f;
    caretAlpha = static_cast<int>(f * static_cast<float>(caretMaxAlpha - caretMinAlpha) + static_cast<float>(caretMinAlpha));
  }

  void AbandonKeyboardFocus(lua_State* L) override {  // 0x78f330
    showCaret = false;
    Control::AbandonKeyboardFocus(L);
  }
  void LosingKeyboardFocus(lua_State* L) override {  // 0x7915a0
    AbandonKeyboardFocus(L);
    RunScript(L, "OnLoseKeyboardFocus");
  }

  // Drawing (0x78f820): nothing without a font; the background when shown; the shown text in up
  // to three runs (before the selection, the selection on its own background and in its own
  // colour, after it), each from Left on at the baseline Top + ascent, with a black shadow one
  // pixel right and down when asked; then the caret, one pixel wide from Top down by the font's
  // height, in the caret colour with the alpha of its cycle.
  void Draw(lua_State*, DrawTarget& target) override {
    if (family.empty()) return;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    if (showBackground) target.FillRect(r, WithAlpha(ParseColor(background), alpha));
    FontMetrics m = GetFontMetrics(family, size);
    float x = r.x0, baseline = r.y0 + m.ascent;
    int end = clipOffset + visible;
    auto run = [&](int from, int to, const std::string& color, bool highlighted) {
      if (to <= from) return;
      std::string s = Utf8Sub(text, from, to - from);
      float adv = Advance(s);
      if (highlighted && showBackground)
        target.FillRect(Rect{x, r.y0, x + adv, r.y1}, WithAlpha(ParseColor(highlightBackground), alpha));
      uint32_t c = WithAlpha(ParseColor(color), alpha);
      if (dropShadow) DrawString(target, family, size, s, x + 1, baseline + 1, c & 0xff000000u);
      DrawString(target, family, size, s, x, baseline, c);
      x += adv;
    };
    run(clipOffset, std::min(selStart, end), foreground, false);
    run(std::max(selStart, clipOffset), std::min(selEnd, end), highlightForeground, true);
    run(std::max(selEnd, clipOffset), end, foreground, false);
    if (showCaret && caret >= clipOffset && caret <= end) {
      float cx = r.x0 + Advance(Utf8Sub(text, clipOffset, caret - clipOffset));
      uint32_t c = (ParseColor(caretColor) & 0x00ffffffu) | (static_cast<uint32_t>(caretAlpha & 0xff) << 24);
      target.FillRect(Rect{cx, r.y0, cx + 1, r.y0 + m.ascent + m.descent}, c);
    }
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
  c->ClipLeft(c->clipOffset);
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
  c->SetCaret(std::max(0, std::min(static_cast<int>(luaL_checknumber(L, 2)), c->Len())));
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

// AcquireFocus(): the field takes the keyboard focus and shows its caret (inferred from what a
// click does, 0x7915f2). AbandonFocus(): it gives the focus up and hides the caret.
int l_AcquireFocus(lua_State* L) {
  Edit* c = Self(L);
  c->showCaret = true;
  UserState::From(L)->ui().SetFocus(L, c, true);
  return 0;
}
int l_AbandonFocus(lua_State* L) {
  Self(L)->AbandonKeyboardFocus(L);
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
