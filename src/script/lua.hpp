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
