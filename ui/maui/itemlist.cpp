// ItemList: a list of text rows with a selection, scrolled by whole rows. Rows count from 0.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "maui/control.h"
#include "maui/font.h"
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

  std::vector<std::string> items;
  int selection = -1;
  int top = 0;  // the first row shown
  std::string family;
  int size = 0;
  std::string colors[6];  // foreground, background, selected fg/bg, mouseover fg/bg
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
// mouseover_background): nil keeps a colour
int l_SetNewColors(lua_State* L) {
  ItemList* c = Self(L);
  for (int i = 0; i < 6; ++i)
    if (!lua_isnil(L, 2 + i) && lua_gettop(L) >= 2 + i) c->colors[i] = lua_tostring(L, 2 + i) ? lua_tostring(L, 2 + i) : "";
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
// Inferred: a row outside the list clears the selection (-1).
int l_SetSelection(lua_State* L) {
  ItemList* c = Self(L);
  int row = static_cast<int>(luaL_checknumber(L, 2));
  c->selection = row >= 0 && row < c->Count() ? row : -1;
  return 0;
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
// ShowItem(index): scroll just far enough that the row is shown (inferred).
int l_ShowItem(lua_State* L) {
  ItemList* c = Self(L);
  int row = static_cast<int>(luaL_checknumber(L, 2));
  int lines = c->LinesVisible();
  if (row < c->top) c->SetTop(row);
  else if (row >= c->top + lines) c->SetTop(row - lines + 1);
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
