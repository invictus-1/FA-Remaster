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
  lua_rawset(L, LUA_GLOBALSINDEX);
}

}  // namespace moho
