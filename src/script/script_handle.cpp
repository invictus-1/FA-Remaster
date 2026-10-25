#include "script/script_handle.h"

#include <cstdlib>

#include "script/script_state.h"

namespace moho {
namespace {

const char* kBoxMeta = "moho64.cobject";

}  // namespace

void RegisterObjectHandle(lua_State* L) {
  luaL_newmetatable(L, kBoxMeta);
  // (the registry keeps the table for the state's life, so its address stays good; host pointer 3 is free: 0 the
  // ScriptState, 1 the Sim, 2 the SimBlueprints)
  lua_setextra(L, 3, const_cast<void*>(lua_topointer(L, -1)));
  lua_pop(L, 1);
}

bool IsObjectHandleMeta(lua_State* L, int idx) {
  const void* meta = lua_getextra(L, 3);
  return meta && lua_type(L, idx) == LUA_TTABLE && lua_topointer(L, idx) == meta;
}

ScriptObject::~ScriptObject() { UnbindLua(); }

void ScriptObject::UnbindLua() {
  if (box_) *box_ = nullptr;
  box_ = nullptr;
  if (L_ && ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, ref_);
  ref_ = LUA_NOREF;
}

void BindObject(lua_State* L, int idx, ScriptObject* obj) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  obj->UnbindLua();
  void** box = static_cast<void**>(lua_newuserdata(L, sizeof(void*)));
  *box = obj;
  luaL_getmetatable(L, kBoxMeta);
  lua_setmetatable(L, -2);
  lua_pushcobjectkey(L);
  lua_insert(L, -2);
  lua_rawset(L, idx);
  lua_pushvalue(L, idx);
  // Keep the reference with the main state (L may be a coroutine that goes away).
  obj->L_ = ScriptState::From(L)->L();
  obj->ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  obj->box_ = box;
}

void PushObject(lua_State* L, ScriptObject* obj) {
  if (!obj || obj->ref_ == LUA_NOREF) {
    lua_pushnil(L);
    return;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, obj->ref_);
}

ScriptObject* GetObject(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) return nullptr;
  lua_rawgetcobject(L, idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return box ? static_cast<ScriptObject*>(*box) : nullptr;
}

ScriptObject* OptAnyObject(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  lua_rawgetcobject(L, idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!box) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  return static_cast<ScriptObject*>(*box);
}

ScriptObject* CheckAnyObject(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  lua_rawgetcobject(L, idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!box) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  if (!*box) {
    static const bool dbg = getenv("MOHO64_DEBUG_DESTROY") != nullptr;
    if (dbg) {  // which object (sim.cpp tags destroyed units)
      lua_pushstring(L, "_moho64_destroyed");
      lua_rawget(L, idx);
      const char* why = lua_tostring(L, -1);
      if (why) luaL_error(L, "Game object has been destroyed (%s)", why);
      lua_pop(L, 1);
    }
    luaL_error(L, "Game object has been destroyed");
  }
  return static_cast<ScriptObject*>(*box);
}

}  // namespace moho
