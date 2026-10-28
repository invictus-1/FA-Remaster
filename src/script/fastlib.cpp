// The engine's own generic Lua helpers, the global table `Remastered` (script/fastlib.h).
//
// Each function does what a few lines of plain Lua would do, with the same answers: it reads tables raw or through
// lua_gettable exactly where the Lua would, does float arithmetic in the VM's own type and order (lua_Number is
// float; the build has -ffp-contract=off), makes no Lua object, calls no script and no Random. They exist only
// because a Lua function call costs ~90 ns in this VM and a C call ~30 ns.
#include "script/fastlib.h"

#include <cstdlib>

namespace moho {
namespace {

// Is the table at `t` deep-empty: no entry, or every value a table that is deep-empty one level further down; a
// table that is not empty at `level` >= `maxLevel` counts as not empty. (`level` is checked only for a table with an
// entry, after that test.) Values in lua_next order; the answer is the AND over them, so the order cannot change it.
bool TableDeepEmpty(lua_State* L, int t, bool haveLevel, lua_Number level, lua_Number maxLevel) {
  lua_checkstack(L, 4);
  lua_pushnil(L);
  if (!lua_next(L, t)) return true;  // (lua_next popped the key and pushed nothing)
  if (haveLevel && maxLevel <= level) {  // (the VM's `level >= maxLevel` for numbers: OP_LE maxLevel <= level)
    lua_pop(L, 2);
    return false;
  }
  const lua_Number next = (haveLevel ? level : 0) + 1;  // (`(level or 0) + 1`, a float add as the VM's)
  for (;;) {
    if (lua_type(L, -1) != LUA_TTABLE || !TableDeepEmpty(L, lua_gettop(L), true, next, maxLevel)) {
      lua_pop(L, 2);
      return false;
    }
    lua_pop(L, 1);  // the value; the key stays for lua_next
    if (!lua_next(L, t)) return true;
  }
}

// The closure MakeDeepEmptyCheck returns: f(value, nonTableResult, level).
//   value a table: TableDeepEmpty, level nil / false meaning none (any other value that is not a number is compared
//   with maxLevel as the VM compares, which raises its error);
//   value nil (or none): true; any other value: nonTableResult itself when it is not nil, else true.
int l_DeepEmpty(lua_State* L) {
  if (lua_type(L, 1) != LUA_TTABLE) {
    // (an argument the caller did not pass counts as nil, as for the Lua function: lua_isnoneornil. lua_isnil is
    // false for one not passed, and IsTableEmpty(IsTableEmpty(t)) then gave nil, not true: b8 beat 3798)
    if (lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2)) lua_pushboolean(L, 1);
    else lua_pushvalue(L, 2);
    return 1;
  }
  const lua_Number maxLevel = lua_tonumber(L, lua_upvalueindex(1));
  bool haveLevel = lua_toboolean(L, 3) != 0;
  lua_Number level = 0;
  if (haveLevel) {
    if (lua_type(L, 3) == LUA_TNUMBER) {
      level = lua_tonumber(L, 3);
    } else {
      // Not reached by any caller known: the VM's `level >= maxLevel` on a non-number errors ("attempt to compare
      // ..."), but only once the table is known to have an entry.
      lua_pushnil(L);
      if (lua_next(L, 1)) {
        lua_pop(L, 2);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, 3);
        lua_lessthan(L, -2, -1);  // raises for a string / table / ... as the comparison would
        lua_pop(L, 2);
      }
      haveLevel = false;  // (an empty table: true, the level never looked at)
    }
  }
  lua_pushboolean(L, TableDeepEmpty(L, 1, haveLevel, level, maxLevel));
  return 1;
}

// Remastered.MakeDeepEmptyCheck(maxLevel): the deep-empty check above as a C closure over maxLevel.
int l_MakeDeepEmptyCheck(lua_State* L) {
  lua_pushnumber(L, luaL_checknumber(L, 1));
  lua_pushcclosure(L, l_DeepEmpty, 1);
  return 1;
}

// Remastered.Noop(...): does nothing and returns nothing (for a script function that, as things stand, would do
// nothing: a C call costs a third of a Lua one).
int l_Noop(lua_State*) { return 0; }

// Remastered.VDist2Pos(a, b): VDist2(a[1], a[3], b[1], b[3]). The four reads in that order through lua_gettable
// (metamethods and the error for indexing a non-table as the VM's), each value taken as VDist2 takes it
// (luaL_checknumber: a numeric string converts, anything else is VDist2's argument error), VDist2's own arithmetic.
int l_VDist2Pos(lua_State* L) {
  lua_settop(L, 2);
  lua_pushnumber(L, 1);
  lua_gettable(L, 1);
  lua_pushnumber(L, 3);
  lua_gettable(L, 1);
  lua_pushnumber(L, 1);
  lua_gettable(L, 2);
  lua_pushnumber(L, 3);
  lua_gettable(L, 2);
  const lua_Number x1 = luaL_checknumber(L, 3), y1 = luaL_checknumber(L, 4);
  const lua_Number x2 = luaL_checknumber(L, 5), y2 = luaL_checknumber(L, 6);
  lua_pushnumber(L, VDist2Of(x1, y1, x2, y2));
  return 1;
}

// --- From FAF's /lua/sim/NavGenerator.lua (gamedata lua.nx2), whose header says, and which applies to the two
// --- functions l_NavGridLeafXZ follows:
//
// Copyright (c) 2022  Willem 'Jip' Wijnia
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// The leaf of a navigational grid at a point: FAF's navigational mesh (Jip's /lua/sim/navgenerator.lua, MIT), its
// NavGrid.FindLeafXZ(self, x, z) and the CompressedLabelTree.FindLeafXZ(self, bx, bz, size, x, z) it ends in, as
// those two Lua functions do it:
//   grid:  if x > 0 and z > 0 then size = self.TreeSize; trees = self.Trees; bx = floor(x / size); bz = floor(z /
//          size); labelTree = trees[bz][bx]; if labelTree then return labelTree:FindLeafXZ(bx * size, bz * size, size,
//          x, z) end end; return nil
//   tree:  if x < bx or bx + size < x or z < bz or bz + size < z then return nil end; a descent from instance =
//          self[1] while type(instance) ~= 'table': hc = 0.5 * hc; hx = iox + hc; hz = ioz + hc; then self[instance],
//          self[instance + 1] (iox = hx), self[instance + 2] (ioz = hz) or self[instance + 3] (both) by lz < hz and
//          lx < hx; the leaf (or whatever non-number ends the walk) is the answer.
// Every read goes through lua_gettable where the Lua indexes (metamethods, and the GPG default metatables of nil and
// numbers, as the VM's); float arithmetic in the VM's type and order. Wherever the Lua would do something else (a
// coordinate, TreeSize or tree slot that is not a number, a tree whose FindLeafXZ is not the one given, a `type'
// that is not the base library's in the tree function's environment), the Lua function itself is called with the
// same arguments, so its answer and its error are the Lua's own (the reads have no side effects to repeat).
// Upvalues: 1 the Lua NavGrid.FindLeafXZ, 2 the Lua CompressedLabelTree.FindLeafXZ, 3 the base library's `type'.
int l_NavGridLeafXZ(lua_State* L) {
  lua_settop(L, 3);
  const auto callLua = [L](int fn, int first, int n) {  // fn(args first..first+n-1): its results (one) are the answer
    lua_pushvalue(L, lua_upvalueindex(fn));
    for (int i = 0; i < n; ++i) lua_pushvalue(L, first + i);
    lua_call(L, n, 1);
    return 1;
  };
  if (lua_type(L, 2) != LUA_TNUMBER || lua_type(L, 3) != LUA_TNUMBER) return callLua(1, 1, 3);
  const lua_Number x = lua_tonumber(L, 2), z = lua_tonumber(L, 3);
  if (!(0 < x) || !(0 < z)) {  // (`x > 0 and z > 0': OP_LT 0 < x, then 0 < z)
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, "TreeSize");
  lua_gettable(L, 1);  // 4: size
  lua_pushstring(L, "Trees");
  lua_gettable(L, 1);  // 5: trees
  if (lua_type(L, 4) != LUA_TNUMBER) {
    lua_settop(L, 3);
    return callLua(1, 1, 3);
  }
  const lua_Number size = lua_tonumber(L, 4);
  const lua_Number bx = static_cast<lua_Number>(std::floor(x / size));  // (MathFloor = math.floor: floor of the float)
  const lua_Number bz = static_cast<lua_Number>(std::floor(z / size));
  lua_pushnumber(L, bz);
  lua_gettable(L, 5);  // 6: trees[bz]
  lua_pushnumber(L, bx);
  lua_gettable(L, 6);  // 7: labelTree
  if (!lua_toboolean(L, 7)) {
    lua_pushnil(L);
    return 1;
  }
  const lua_Number tbx = bx * size, tbz = bz * size;
  lua_pushnumber(L, tbx);  // 8..12: the tree call's arguments after self (bx * size, bz * size, size, x, z)
  lua_pushnumber(L, tbz);
  lua_pushnumber(L, size);
  lua_pushnumber(L, x);
  lua_pushnumber(L, z);
  lua_pushstring(L, "FindLeafXZ");
  lua_gettable(L, 7);  // 13: labelTree.FindLeafXZ (the `:' call's method)
  const bool ownTree = lua_rawequal(L, 13, lua_upvalueindex(2)) && lua_type(L, 7) == LUA_TTABLE;
  if (ownTree) {  // the tree function's own `type' (its environment's global, read at each call)
    lua_getfenv(L, lua_upvalueindex(2));
    lua_pushstring(L, "type");
    lua_gettable(L, -2);
    const bool baseType = lua_rawequal(L, -1, lua_upvalueindex(3)) != 0;
    lua_pop(L, 2);
    if (baseType) {
      if (x < tbx || tbx + size < x || z < tbz || tbz + size < z) {
        lua_pushnil(L);
        return 1;
      }
      lua_Number iox = 0, ioz = 0, hc = size;
      const lua_Number lx = x - tbx, lz = z - tbz;
      lua_pushnumber(L, 1);
      lua_gettable(L, 7);  // 14: instance = self[1]
      while (lua_type(L, 14) != LUA_TTABLE) {
        if (lua_type(L, 14) != LUA_TNUMBER) {  // (not a number: the Lua's own walk, from the start, decides)
          lua_pushvalue(L, lua_upvalueindex(2));
          for (int i = 7; i <= 12; ++i) lua_pushvalue(L, i);  // labelTree, bx * size, bz * size, size, x, z
          lua_call(L, 6, 1);
          return 1;
        }
        const lua_Number instance = lua_tonumber(L, 14);
        hc = static_cast<lua_Number>(0.5f) * hc;
        const lua_Number hx = iox + hc, hz = ioz + hc;
        lua_Number key;
        if (lz < hz) {
          if (lx < hx) {
            key = instance;
          } else {
            key = instance + 1;
            iox = hx;
          }
        } else {
          if (lx < hx) {
            key = instance + 2;
            ioz = hz;
          } else {
            key = instance + 3;
            iox = hx;
            ioz = hz;
          }
        }
        lua_pushnumber(L, key);
        lua_gettable(L, 7);
        lua_replace(L, 14);
      }
      return 1;  // the leaf (stack top)
    }
  }
  // labelTree:FindLeafXZ(...) through whatever FindLeafXZ it has: `return' passes its results on
  lua_settop(L, 13);
  lua_insert(L, 7);  // 7: the method, 8: labelTree, 9..13: arguments
  lua_call(L, 6, 1);
  return 1;
}

// Remastered.MakeNavGridLeafXZ(gridFindLeafXZ, treeFindLeafXZ): l_NavGridLeafXZ over FAF's two Lua functions (and the
// base library's `type' as it is now), for a script to put in NavGrid's place.
int l_MakeNavGridLeafXZ(lua_State* L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_settop(L, 2);
  lua_pushstring(L, "type");
  lua_rawget(L, LUA_GLOBALSINDEX);
  lua_pushcclosure(L, l_NavGridLeafXZ, 3);
  return 1;
}

}  // namespace

void RegisterRemasteredLib(lua_State* L) {
  if (const char* e = std::getenv("MOHO64_NO_REMASTERED"); e && e[0] == '1') return;
  lua_pushstring(L, "Remastered");
  lua_newtable(L);
  lua_pushstring(L, "version");
  lua_pushnumber(L, 1);
  lua_rawset(L, -3);
  lua_pushstring(L, "MakeDeepEmptyCheck");
  lua_pushcfunction(L, l_MakeDeepEmptyCheck);
  lua_rawset(L, -3);
  lua_pushstring(L, "Noop");
  lua_pushcfunction(L, l_Noop);
  lua_rawset(L, -3);
  lua_pushstring(L, "VDist2Pos");
  lua_pushcfunction(L, l_VDist2Pos);
  lua_rawset(L, -3);
  lua_pushstring(L, "MakeNavGridLeafXZ");
  lua_pushcfunction(L, l_MakeNavGridLeafXZ);
  lua_rawset(L, -3);
  RegisterRemasteredReclaim(L);
  lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace moho
