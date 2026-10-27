// ItemList: a list of text rows with a selection, scrolled by whole rows. Rows count from 0.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "maui/control.h"
#include "maui/font.h"
#include "maui/input.h"
#include "user/bindings.h"

namespace moho::ui {
namespace {

class ItemList : public Control {
 public:
  const char* ClassName() const override { return "itemlist"; }
  void Describe(std::string& out) const override {
    out += std::to_string(items.size()) + " items, top " + std::to_string(top) + ", selection " + std::to_string(selection);
    if (!items.empty()) out += ", first \"" + items[0] + "\"";
  }

  // As the original draws it (FA exe 0x799a50): nothing without a font; the background colour over
  // the rectangle; the mouse-over row's band, then the selected row's band, each from the row's top
  // down by the font's height (the external leading stays out, so bands of neighbouring rows do
  // not touch); then the shown rows' text on their baselines (Top + i * row height + ascent),
  // limited to the control's width, where the mouse-over colour goes before the selected one.
  // Every colour is drawn as it is (no colour counts as "not set": the list starts with the
  // constructor's, 0x799340), so a list that sets only four colours, as the game's drop-downs do
  // (combo.lua), still shows the mouse-over row in the constructor's ff202020 / ffa0a0a0.
  // Checked against a screenshot of the original's map list: same rows, same band. The original
  // makes its solid colours as 16-bit textures (the band's 7f a8 a4 shows as 7b aa a5 there);
  // that rounding is left out.
  void Draw(lua_State*, DrawTarget& target) override {
    if (family.empty()) return;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    FontMetrics m = GetFontMetrics(family, size);
    float row = m.height + m.externalLeading;
    int lines = LinesVisible();
    auto band = [&](int index, uint32_t color) {
      if (index < this->top || index >= this->top + lines) return;
      float y = r.y0 + static_cast<float>(index - this->top) * row;
      target.FillRect(Rect{r.x0, y, r.x1, y + m.height}, WithAlpha(color, alpha));
    };
    target.FillRect(r, WithAlpha(colors[1], alpha));
    if (showMouseover) band(mouseover, colors[5]);
    if (showSelection) band(selection, colors[3]);
    target.SetClip(r);
    for (int i = 0; i < lines && this->top + i < Count(); ++i) {
      int index = this->top + i;
      float y = r.y0 + static_cast<float>(i) * row;
      uint32_t fg = colors[0];
      if (showMouseover && index == mouseover) fg = colors[4];
      else if (showSelection && index == selection) fg = colors[2];
      DrawString(target, family, size, items[static_cast<size_t>(index)], r.x0, y + m.ascent, WithAlpha(fg, alpha));
    }
    target.ClearClip();
  }

  std::vector<std::string> items;
  int selection = -1;
  int mouseover = -1;  // the row under the mouse while the mouse-over row is shown
  int top = 0;  // the first row shown
  std::string family;
  int size = 0;
  // foreground, background, selected fg/bg, mouseover fg/bg (+0x120..+0x134), as the constructor
  // (FA exe 0x799340) sets them
  uint32_t colors[6] = {0xff808080u, 0xff000000u, 0xff000000u, 0xff808080u, 0xffa0a0a0u, 0xff202020u};
  bool showSelection = true, showMouseover = false;

  int Count() const { return static_cast<int>(items.size()); }

  // A row's height (FA exe 0x79c280): the font's height plus its external leading.
  float RowHeight() const {
    if (family.empty()) return 0;
    FontMetrics m = GetFontMetrics(family, size);
    return m.height + m.externalLeading;
  }

  // The rows that fit (FA exe 0x79a730): floor((Height + leading) / row height), at most the
  // number of items. Asking also brings the first shown row back into range.
  int LinesVisible() {
    int lines = 0;
    if (!family.empty()) {
      FontMetrics m = GetFontMetrics(family, size);
      float row = m.height + m.externalLeading;
      if (row > 0) lines = static_cast<int>(std::floor((height.Get() + m.externalLeading) / row));
    }
    if (lines > Count()) {
      top = 0;
      return Count();
    }
    if (top + lines > Count()) top = Count() - lines;
    return lines;
  }

  void SetTop(int t) {
    int maxTop = Count() - LinesVisible();
    top = std::max(0, std::min(t, maxTop));
  }

  // The scroll answers (FA exe 0x7991e0, 0x79a560, 0x79a5f0, 0x79a650, 0x79a6d0): vertical only;
  // the range is the rows, the visible part starts at the first shown row.
  bool IsScrollable(lua_State*, int axis) override { return axis == 0; }
  void GetScrollValues(lua_State*, int, float out[4]) override {
    int lines = LinesVisible();
    out[0] = 0;
    out[1] = static_cast<float>(Count());
    out[2] = static_cast<float>(top);
    out[3] = static_cast<float>(top + lines);
  }
  void ScrollLines(lua_State*, int, float lines) override { SetTop(top + static_cast<int>(std::nearbyint(lines))); }
  void ScrollPages(lua_State*, int, float pages) override {
    SetTop(top + static_cast<int>(std::nearbyint(pages * static_cast<float>(LinesVisible()))));
  }
  void ScrollSetTop(lua_State*, int, float t) override { SetTop(static_cast<int>(std::nearbyint(t))); }

  // The row at a height on screen (FA exe 0x79a0b0): -1 outside the rows, and in the strip the
  // external leading leaves under a row's text.
  int RowAt(float y) {
    if (family.empty() || items.empty()) return -1;
    FontMetrics m = GetFontMetrics(family, size);
    float row = m.height + m.externalLeading;
    if (row <= 0) return -1;
    Rect r;
    GetRect(&r.x0, &r.y0, &r.x1, &r.y1);
    float rel = y - r.y0;
    int index = static_cast<int>(rel / row) + this->top;
    if (index < 0 || index >= Count()) return -1;
    return m.height <= std::fmod(rel, row) ? -1 : index;
  }
  void Select(int row) { selection = row >= 0 && row < Count() ? row : -1; }  // 0x799a10
  // ShowItem (0x79a8c0): a row outside the shown ones becomes the first shown row.
  void ShowItem(lua_State* L, int row) {
    int lines = LinesVisible();
    if (row < this->top || row >= this->top + lines) ScrollSetTop(L, 0, static_cast<float>(row));
  }
  void CallWithRow(lua_State* L, const char* fn, int row) {
    lua_pushnumber(L, static_cast<lua_Number>(row));
    RunScript(L, fn, 1, 0);
  }

  // Events (FA exe 0x79a160). The script's HandleEvent comes first; when it does not handle the
  // event the list does its own work: the row under the mouse (OnMouseoverItem(row), -1 when the
  // mouse leaves), a press or double click on a row (OnClick / OnDoubleClick(row, event)), the
  // wheel (one row per notch), and the keys that move the selection (OnKeySelect(row)), which
  // arrive as character events.
  bool HandleEvent(lua_State* L, const UiEvent& e) override {
    if (Control::HandleEvent(L, e)) return true;
    if (dead()) return true;
    switch (e.type) {
      case EventType::MouseMotion: {
        if (!showMouseover) return false;
        int old = mouseover;
        mouseover = RowAt(e.y);
        if (old != mouseover) CallWithRow(L, "OnMouseoverItem", mouseover);
        return true;
      }
      case EventType::MouseExit:
        if (mouseover != -1) {
          mouseover = -1;
          CallWithRow(L, "OnMouseoverItem", -1);
        }
        return true;
      case EventType::ButtonPress:
      case EventType::ButtonDClick: {
        int row = RowAt(e.y);
        if (row >= 0) {
          lua_pushnumber(L, static_cast<lua_Number>(row));
          PushEventTable(L, this, e);
          RunScript(L, e.type == EventType::ButtonPress ? "OnClick" : "OnDoubleClick", 2, 0);
        }
        return true;
      }
      case EventType::WheelRotation:
        ScrollLines(L, 0, e.wheelRotation <= 0 ? 1.0f : -1.0f);
        return true;
      case EventType::Char: {
        int oldTop = this->top, offset = selection - this->top;
        switch (e.keyCode) {
          case 317:  // Up
            Select(std::max(selection - 1, 0));
            ShowItem(L, selection);
            break;
          case 319:  // Down
            Select(std::min(selection + 1, Count() - 1));
            ShowItem(L, selection);
            break;
          case 312:
          case 368:  // Page up: the selection keeps its place among the shown rows
            ScrollPages(L, 0, -1.0f);
            Select(this->top == oldTop ? 0 : offset + this->top);
            break;
          case 313:
          case 369:  // Page down
            ScrollPages(L, 0, 1.0f);
            Select(this->top == oldTop ? Count() - 1 : std::min(offset + this->top, Count() - 1));
            break;
          case 315:  // Home
            ScrollSetTop(L, 0, 0.0f);
            Select(0);
            break;
          case 314:  // End
            SetTop(Count());
            Select(Count() - 1);
            break;
          default:
            return true;
        }
        CallWithRow(L, "OnKeySelect", selection);
        return true;
      }
      default:
        return false;
    }
  }
};

int l_InternalCreateItemList(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Control* parent = CheckParent(L);
  ItemList* c = new ItemList;
  c->Create(L, 1, parent);
  c->RunInit(L);
  return 0;
}

ItemList* Self(lua_State* L) { return CheckControl<ItemList>(L, 1); }

// The methods that change the list return the list, so calls can be chained.
int ReturnSelf(lua_State* L) {
  lua_pushvalue(L, 1);
  return 1;
}

int l_SetNewFont(lua_State* L) {
  ItemList* c = Self(L);
  c->family = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
  c->size = static_cast<int>(lua_tonumber(L, 3));
  return 0;
}

// SetNewColors(foreground, background, selected_foreground, selected_background, mouseover_foreground,
// mouseover_background) (FA exe 0x79adb0): seven arguments; a nil one keeps its colour.
int l_SetNewColors(lua_State* L) {
  ItemList* c = Self(L);
  for (int i = 0; i < 6; ++i)
    if (lua_gettop(L) >= 2 + i && !lua_isnil(L, 2 + i)) {
      const char* text = lua_tostring(L, 2 + i);
      c->colors[i] = ParseColor(text ? text : "");
    }
  return 0;
}

int l_AddItem(lua_State* L) {
  Self(L)->items.push_back(luaL_checkstring(L, 2));
  return ReturnSelf(L);
}

// Inferred: a row outside the list is an error for ModifyItem and DeleteItem, and nil for GetItem.
int Row(lua_State* L, ItemList* c, int idx) {
  int row = static_cast<int>(luaL_checknumber(L, idx));
  if (row < 0 || row >= c->Count()) luaL_error(L, "ItemList: row %d is out of range (the list has %d items)", row, c->Count());
  return row;
}

int l_ModifyItem(lua_State* L) {
  ItemList* c = Self(L);
  c->items[static_cast<size_t>(Row(L, c, 2))] = luaL_checkstring(L, 3);
  return ReturnSelf(L);
}

// Inferred: deleting the selected row clears the selection; rows after it move up with their selection.
int l_DeleteItem(lua_State* L) {
  ItemList* c = Self(L);
  int row = Row(L, c, 2);
  c->items.erase(c->items.begin() + row);
  if (c->selection == row) c->selection = -1;
  else if (c->selection > row) --c->selection;
  c->SetTop(c->top);
  return ReturnSelf(L);
}

int l_DeleteAllItems(lua_State* L) {
  ItemList* c = Self(L);
  c->items.clear();
  c->selection = -1;
  c->top = 0;
  return ReturnSelf(L);
}

int l_GetItem(lua_State* L) {
  ItemList* c = Self(L);
  int row = static_cast<int>(luaL_checknumber(L, 2));
  if (row < 0 || row >= c->Count()) lua_pushnil(L);
  else lua_pushstring(L, c->items[static_cast<size_t>(row)].c_str());
  return 1;
}

int l_GetItemCount(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(Self(L)->Count()));
  return 1;
}
int l_Empty(lua_State* L) {
  lua_pushboolean(L, Self(L)->items.empty());
  return 1;
}
int l_GetSelection(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(Self(L)->selection));
  return 1;
}
// SetSelection(row) (FA exe 0x79ba00): a row of the list becomes the selection, a row past its end
// clears it (-1), a negative row leaves it as it is; the list is returned. It does not scroll.
int l_SetSelection(lua_State* L) {
  ItemList* c = Self(L);
  int row = static_cast<int>(luaL_checknumber(L, 2));
  if (row >= 0) c->selection = row < c->Count() ? row : -1;
  return ReturnSelf(L);
}

int l_ScrollToTop(lua_State* L) {
  Self(L)->SetTop(0);
  return 0;
}
int l_ScrollToBottom(lua_State* L) {
  ItemList* c = Self(L);
  c->SetTop(c->Count());
  return 0;
}
// ShowItem(index): a row that is not shown becomes the first shown row
int l_ShowItem(lua_State* L) {
  ItemList* c = Self(L);
  c->ShowItem(L, static_cast<int>(luaL_checknumber(L, 2)));
  return 0;
}

int l_GetRowHeight(lua_State* L) {
  lua_pushnumber(L, Self(L)->RowHeight());
  return 1;
}
int l_GetStringAdvance(lua_State* L) {
  ItemList* c = Self(L);
  lua_pushnumber(L, StringAdvance(c->family, c->size, luaL_checkstring(L, 2)));
  return 1;
}
// NeedsScrollBar(): more rows than fit (inferred from its name; FA exe 0x79a8f0 not read).
int l_NeedsScrollBar(lua_State* L) {
  ItemList* c = Self(L);
  lua_pushboolean(L, c->Count() > c->LinesVisible());
  return 1;
}
int l_ShowMouseoverItem(lua_State* L) {
  Self(L)->showMouseover = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_ShowSelection(lua_State* L) {
  Self(L)->showSelection = lua_toboolean(L, 2) != 0;
  return 0;
}

}  // namespace

void RegisterItemListBindings(lua_State* L) {
  const char* cls = "CMauiItemList";
  SetGlobal(L, "InternalCreateItemList", l_InternalCreateItemList);
  SetMethod(L, cls, "SetNewFont", l_SetNewFont);
  SetMethod(L, cls, "SetNewColors", l_SetNewColors);
  SetMethod(L, cls, "AddItem", l_AddItem);
  SetMethod(L, cls, "ModifyItem", l_ModifyItem);
  SetMethod(L, cls, "DeleteItem", l_DeleteItem);
  SetMethod(L, cls, "DeleteAllItems", l_DeleteAllItems);
  SetMethod(L, cls, "GetItem", l_GetItem);
  SetMethod(L, cls, "GetItemCount", l_GetItemCount);
  SetMethod(L, cls, "Empty", l_Empty);
  SetMethod(L, cls, "GetSelection", l_GetSelection);
  SetMethod(L, cls, "SetSelection", l_SetSelection);
  SetMethod(L, cls, "ScrollToTop", l_ScrollToTop);
  SetMethod(L, cls, "ScrollToBottom", l_ScrollToBottom);
  SetMethod(L, cls, "ShowItem", l_ShowItem);
  SetMethod(L, cls, "GetRowHeight", l_GetRowHeight);
  SetMethod(L, cls, "GetStringAdvance", l_GetStringAdvance);
  SetMethod(L, cls, "NeedsScrollBar", l_NeedsScrollBar);
  SetMethod(L, cls, "ShowMouseoverItem", l_ShowMouseoverItem);
  SetMethod(L, cls, "ShowSelection", l_ShowSelection);
}

}  // namespace moho::ui
