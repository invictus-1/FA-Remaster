// Small helpers for reading blueprint tables (sim/combat.cpp and friends).
#pragma once
#include <string>

#include "script/lua.hpp"

namespace moho::lu {

// Push t[key] (nil if t is not a table); t is an absolute index.
inline void Field(lua_State* L, int t, const char* key) {
  if (!lua_istable(L, t)) {
    lua_pushnil(L);
    return;
  }
  lua_pushstring(L, key);
  lua_gettable(L, t);
}
inline float Num(lua_State* L, int t, const char* key, float def) {
  Field(L, t, key);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
inline bool Bool(lua_State* L, int t, const char* key, bool def = false) {
  Field(L, t, key);
  bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}
inline std::string Str(lua_State* L, int t, const char* key, const char* def = "") {
  Field(L, t, key);
  std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
  lua_pop(L, 1);
  return v;
}
// Push t[a] and leave it on the stack; returns its absolute index.
inline int Sub(lua_State* L, int t, const char* key) {
  Field(L, t, key);
  return lua_gettop(L);
}

}  // namespace moho::lu
