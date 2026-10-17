#include "maui/lazyvar.h"

#include <string>

#include "core/log.h"
#include "maui/control.h"
#include "script/script_state.h"

namespace moho::ui {
namespace {
const Control* g_lazyContext = nullptr;
}  // namespace
void SetLazyVarContext(const Control* control) { g_lazyContext = control; }

bool LazyVar::Create(lua_State* L, float initial) {
  Release();
  L_ = ScriptState::From(L)->L();
  int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/lazyvar.lua");
  bool ok = lua_pcall(L, 1, 1, 0) == 0;
  if (ok) {
    lua_pushstring(L, "Create");
    lua_gettable(L, -2);
    lua_pushnumber(L, initial);
    ok = lua_pcall(L, 1, 1, 0) == 0;
  }
  if (!ok) {
    Logf(LogLevel::Warning, "Error in lazyvar.Create(): %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_settop(L, top);
    return false;
  }
  ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_settop(L, top);
  return true;
}

void LazyVar::Release() {
  if (L_ && ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
  ref_ = LUA_NOREF;
}

void LazyVar::Push(lua_State* L) const {
  if (ref_ == LUA_NOREF) lua_pushnil(L);
  else lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
}

float LazyVar::Get() const {
  if (ref_ == LUA_NOREF) return 0.0f;
  lua_State* L = L_;
  int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
  lua_rawgeti(L, -1, 1);  // the cached value
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    if (lua_pcall(L, 0, 1, 0) != 0) {
      std::string where;
      if (const Control* c = owner_ ? owner_ : g_lazyContext) {  // (its control: kind, name and what it shows)
        where = std::string(" [of the control ") + c->ClassName() + (c->name.empty() ? "" : " \"" + c->name + "\"");
        std::string shows;
        c->Describe(shows);
        if (!shows.empty()) where += ": " + shows.substr(0, 80);
        for (const Control* p = c->parent(); p; p = p->parent()) {  // (and where it hangs: its parents' kinds and names)
          where += std::string(" < ") + p->ClassName() + (p->name.empty() ? "" : " \"" + p->name + "\"");
          std::string theirs;
          p->Describe(theirs);
          if (!theirs.empty()) where += " (" + theirs.substr(0, 60) + ")";
        }
        where += "]";
      }
      Logf(LogLevel::Warning, "Evaluating LazyVar failed: %s%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?", where.c_str());
      lua_pop(L, 1);
      lua_pushnumber(L, 0);
      lua_rawseti(L, top + 1, 1);  // it reads as 0 from now on, until a script sets it again
      lua_settop(L, top);
      return 0.0f;
    }
  }
  if (lua_type(L, -1) != LUA_TNUMBER) {
    Logf(LogLevel::Warning, "LazyVar has non-number value.");
    lua_pushnumber(L, 0);
    lua_rawseti(L, top + 1, 1);
    lua_settop(L, top);
    return 0.0f;
  }
  float v = static_cast<float>(lua_tonumber(L, -1));
  lua_settop(L, top);
  return v;
}

bool LazyVar::TryGet(float* out) const {
  if (ref_ == LUA_NOREF) return false;
  lua_State* L = L_;
  int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
  bool ok = lua_pcall(L, 0, 1, 0) == 0 && lua_type(L, -1) == LUA_TNUMBER;
  if (ok) *out = static_cast<float>(lua_tonumber(L, -1));
  lua_settop(L, top);
  return ok;
}

void LazyVar::Set(float v) {
  if (ref_ == LUA_NOREF) return;
  lua_State* L = L_;
  int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
  lua_pushstring(L, "SetValue");
  lua_gettable(L, -2);
  lua_pushvalue(L, -2);
  lua_pushnumber(L, v);
  if (lua_pcall(L, 2, 0, 0) != 0)
    Logf(LogLevel::Warning, "Setting LazyVar failed: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
  lua_settop(L, top);
}

}  // namespace moho::ui
