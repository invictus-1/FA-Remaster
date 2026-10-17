// C++ include of the GPG-dialect Lua 5.0 core (compiled as C), plus small helpers the
// Lua 5.0 API lacks.
#pragma once
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// lua_getfield as in Lua 5.1: pushes t[k] where t is at `idx`.
inline void lua_getfield(lua_State* L, int idx, const char* k) {
  if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = lua_gettop(L) + idx + 1;
  lua_pushstring(L, k);
  lua_gettable(L, idx);
}

namespace moho {

// The length of a list as the original's engine bindings take it: LuaPlus::LuaObject::GetCount 0x907f50 -> lua_getn
// 0x90ad30 (some bindings call lua_getn itself). That is t.n (raw) when it is a number, else the largest numeric key
// that holds a value (the array part's last non-nil slot, then every numeric key of the hash part), truncated
// (cvttss2si). Unlike table.getn (luaL_getn 0x90e090, LuaObject::GetN 0x907e50) it does not stop at the first nil:
// M28's zone lists keep holes where a unit was taken out ({[1] = a, [3] = b} counts 3). `t` must be a table (the
// original reads anything else as one and would fault).
inline int ObjectCount(lua_State* L, int t) {
  if (t < 0 && t > LUA_REGISTRYINDEX) t = lua_gettop(L) + t + 1;
  lua_pushstring(L, "n");
  lua_rawget(L, t);
  if (lua_type(L, -1) == LUA_TNUMBER) {
    const int n = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return n;
  }
  lua_pop(L, 1);
  lua_Number best = 0;
  lua_pushnil(L);
  while (lua_next(L, t)) {
    if (lua_type(L, -2) == LUA_TNUMBER && lua_tonumber(L, -2) > best) best = lua_tonumber(L, -2);
    lua_pop(L, 1);
  }
  return static_cast<int>(best);
}

}  // namespace moho
