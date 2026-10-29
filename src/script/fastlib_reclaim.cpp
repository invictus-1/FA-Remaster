// The reclaim grid's helpers in `Remastered` (script/fastlib.h): one cell's update of FAF's AI reclaim grid
// (/lua/AI/GridReclaim.lua, AIGridReclaim.UpdateCell) as the engine's script fix data/fixes/hook/lua/AI/GridReclaim.lua
// (docs/deviations.md 10) does it, in C; and the grid's sort of its cells by TotalMass (FAF's SortLambda, GridReclaim.lua
// 59-61) with table.sort's own routine and the comparison in C.
//
// What it follows: FAF's UpdateCell (GridReclaim.lua 134-155), whose sum loop is by Willem 'Jip' Wijnia under the MIT
// licence; its notice, as the file carries it:
//
//   Copyright (c) 2022  Willem 'Jip' Wijnia
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// and the fix's own re-sum in EntityId order without destroyed props (FA Remastered, MIT).
#include <algorithm>
#include <cstdint>
#include <vector>

#include "script/fastlib.h"

namespace moho {
namespace {

// A value read as `v or 0` and then used in float arithmetic: nil / false give 0; a number itself. Anything else (true,
// a string the VM would convert, a table with __mul, ...) is not handled here: false, and the caller leaves the cell
// to the Lua code, which does what the VM does with it (its result or its error).
bool OrZero(lua_State* L, int idx, lua_Number* out) {
  const int t = lua_type(L, idx);
  if (t == LUA_TNIL || (t == LUA_TBOOLEAN && !lua_toboolean(L, idx))) {
    *out = 0;
    return true;
  }
  if (t != LUA_TNUMBER) return false;
  *out = lua_tonumber(L, idx);
  return true;
}

// The three fields of the reclaim object at `obj`, read through lua_gettable (metamethods as the VM's `reclaim.X`) in
// the Lua's order: ReclaimLeft, MaxMassReclaim, MaxEnergyReclaim. Keys at stack slots k..k+2. Leaves the stack as it was.
bool ReadReclaim(lua_State* L, int obj, int k, lua_Number* fraction, lua_Number* mass, lua_Number* energy) {
  bool ok = true;
  for (int i = 0; i < 3 && ok; ++i) {
    lua_pushvalue(L, k + i);
    lua_gettable(L, obj);
    ok = OrZero(L, -1, i == 0 ? fraction : i == 1 ? mass : energy);
    lua_pop(L, 1);
  }
  return ok;
}

// One entry of a cell: its id's number and what the re-sum takes from it.
struct CellEntry {
  uint64_t id;  // the id's digits as a number (no leading zero, at most 12 digits: below 2^40)
  bool live;    // not destroyed (IsDestroyed's test)
  lua_Number fraction, mass, energy;  // ReclaimLeft or 0, MaxMassReclaim or 0, MaxEnergyReclaim or 0
};

// The id key at `idx` as the fix's SortPadded takes it: a string of digits without a leading zero, at most 12 long.
// Its order, the fix's (shorter first, then by text), is then the numbers' order. Anything else: false (the Lua code
// sorts it with KeyLess).
bool ParseId(lua_State* L, int idx, uint64_t* id) {
  if (lua_type(L, idx) != LUA_TSTRING) return false;
  const char* s = lua_tostring(L, idx);
  const size_t n = lua_strlen(L, idx);
  if (n == 0 || n > 12 || s[0] < '1' || s[0] > '9') return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  *id = v;
  return true;
}

// Remastered.ReclaimCellTotals(reclaims): count, totalMass, totalEnergy of a reclaim cell's table `reclaims` (id ->
// reclaim object), as the fix's GridReclaim.UpdateCell leaves them with FAF's debug view off:
//   count: the entries, as FAF's loop counts them (count + 1 in floats);
//   totalMass, totalEnergy: over the ids in EntityId order, each entry not destroyed (IsDestroyed's test), with
//     fraction = ReclaimLeft or 0: totalMass + fraction * (MaxMassReclaim or 0) and totalEnergy + fraction *
//     (MaxEnergyReclaim or 0), the same float operations in the same order as the Lua (lua_Number is float;
//     -ffp-contract=off). FAF's own totals are not made: the fix replaces them.
// The table is walked once (lua_next). Each value's three fields are read once, through lua_gettable (metamethods as
// the VM's `reclaim.X`), in the Lua's order ReclaimLeft, MaxMassReclaim, MaxEnergyReclaim; FAF's loop and the re-sum
// read them twice and the re-sum reads reclaims[id] again: the same values, as nothing in between writes, and a
// prop's fields are its own or its class table's (no __index function).
// Returns nothing (the caller runs its Lua code) when `reclaims` is not a table, a value is not a table, an id is not
// one SortPadded takes, or a field is not nil / false / a number: those cases stay with the Lua code, which gives what
// it always gave (or raises its error). Makes no Lua object and calls no function that collects.
int l_ReclaimCellTotals(lua_State* L) {
  if (lua_type(L, 1) != LUA_TTABLE) return 0;
  lua_settop(L, 1);
  lua_checkstack(L, 8);
  lua_pushstring(L, "ReclaimLeft");       // 2
  lua_pushstring(L, "MaxMassReclaim");    // 3
  lua_pushstring(L, "MaxEnergyReclaim");  // 4
  static thread_local std::vector<CellEntry> entries;
  entries.clear();
  lua_Number count = 0;
  lua_pushnil(L);
  while (lua_next(L, 1)) {  // key -2, value -1
    count = count + 1;
    CellEntry c;
    if (lua_type(L, -1) != LUA_TTABLE || !ParseId(L, -2, &c.id)) return 0;
    const int v = lua_gettop(L);
    if (!ReadReclaim(L, v, 2, &c.fraction, &c.mass, &c.energy)) return 0;
    c.live = !IsDestroyedValue(L, v, false);
    entries.push_back(c);
    lua_pop(L, 1);
  }
  std::sort(entries.begin(), entries.end(), [](const CellEntry& a, const CellEntry& b) { return a.id < b.id; });
  lua_Number totalMass = 0, totalEnergy = 0;
  for (const CellEntry& c : entries) {
    if (!c.live) continue;
    totalMass = totalMass + c.fraction * c.mass;
    totalEnergy = totalEnergy + c.fraction * c.energy;
  }
  lua_settop(L, 0);
  lua_pushnumber(L, count);
  lua_pushnumber(L, totalMass);
  lua_pushnumber(L, totalEnergy);
  return 3;
}

// FAF's SortLambda(a, b), `return a.TotalMass > b.TotalMass`, as table.sort's comparison for the values at a and b:
// a.TotalMass, then b.TotalMass, each through lua_gettable (metamethods as the VM's `a.TotalMass`; the key, the string
// "TotalMass", at stack index 2), then the VM's `>`: OP_LT with the operands swapped (lua_lessthan, numbers, strings and
// __lt as the VM's). lua_gpg_checkgc: the collector's check that luaD_call makes when the script comparison returns.
int TotalMassGreater(lua_State* L, int a, int b) {
  lua_pushvalue(L, 2);
  lua_gettable(L, a - 1);
  lua_pushvalue(L, 2);
  lua_gettable(L, b - 2);
  const int r = lua_lessthan(L, -1, -2);
  lua_pop(L, 2);
  lua_gpg_checkgc(L);
  return r;
}

// Remastered.SortCellsByTotalMass(t): table.sort(t, SortLambda) with SortLambda in C: the same routine (ltablib.c
// luaB_sortwith: the length as table.sort takes it, the same quicksort, the same comparisons in the same order, the
// same swaps), so the same order, ties included. The key string is the closure's upvalue (made once, when Remastered is
// made): nothing is made while sorting.
int l_SortCellsByTotalMass(lua_State* L) {
  lua_settop(L, 1);
  lua_pushvalue(L, lua_upvalueindex(1));
  luaB_sortwith(L, TotalMassGreater);
  return 0;
}

}  // namespace

void RegisterRemasteredReclaim(lua_State* L) {
  lua_pushstring(L, "ReclaimCellTotals");
  lua_pushcfunction(L, l_ReclaimCellTotals);
  lua_rawset(L, -3);
  lua_pushstring(L, "SortCellsByTotalMass");
  lua_pushstring(L, "TotalMass");
  lua_pushcclosure(L, l_SortCellsByTotalMass, 1);
  lua_rawset(L, -3);
}

}  // namespace moho
