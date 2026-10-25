// Engine objects seen from Lua: the handle.
//
// The original exposes each engine object (unit, brain, manipulator, a control of the interface, ...) as a
// Lua table whose `_c_object` field holds a handle to the C++ object; methods find their object through it.
// This is the part every Lua state shares. Which classes exist, and their tables in `moho`, is the business
// of whoever owns the state (the simulation: sim/script_object.h).
#pragma once
#include <cstdint>

#include "script/lua.hpp"

namespace moho {

// Fast type tests for the classes a state's scripts use most (CheckObject/ToObject without dynamic_cast):
// a class that specialises this with a non-zero bit is recognised by ScriptObject::typeBits; other classes
// fall back to dynamic_cast.
template <class T>
struct ScriptTypeOf {
  static constexpr uint32_t bit = 0;
};

class ScriptObject {
 public:
  virtual ~ScriptObject();
  uint32_t typeBits = 0;  // the ScriptTypeOf bits of the object's class and its bases
  // Engine events scripts can WaitFor: -1 not an event, 0 pending, 1 signalled.
  virtual int EventState() const { return -1; }

  // The object's Lua table (registry reference), set by BindObject.
  lua_State* luaState() const { return L_; }
  bool HasLuaObject() const { return ref_ != LUA_NOREF; }

  // Detach from Lua: the table stays, but IsDestroyed() and methods see a destroyed object.
  void UnbindLua();

 private:
  friend void BindObject(lua_State*, int, ScriptObject*);
  friend void PushObject(lua_State*, ScriptObject*);
  lua_State* L_ = nullptr;
  int ref_ = LUA_NOREF;
  void** box_ = nullptr;  // the _c_object userdata's payload
};

// Once per Lua state, before the first BindObject: the handle's metatable ("moho64.cobject", which
// IsDestroyed looks for).
void RegisterObjectHandle(lua_State* L);
// Whether the value at idx is that metatable (as lua_rawequal with luaL_getmetatable(L, "moho64.cobject"), without
// making the name's string or touching the heap: the table's address, kept by RegisterObjectHandle in the state's
// host pointer 3; false before RegisterObjectHandle, as the nil the name gave then).
bool IsObjectHandleMeta(lua_State* L, int idx);

// Make the table at `idx` the Lua side of `obj` (sets its `_c_object`).
void BindObject(lua_State* L, int idx, ScriptObject* obj);
// Push the object's table (nil if it has none).
void PushObject(lua_State* L, ScriptObject* obj);

// The engine object behind the value at idx: nullptr if it is not a game object or was destroyed.
ScriptObject* GetObject(lua_State* L, int idx);
// As the original: errors "Expected a game object..." / "Game object has been destroyed" /
// "Incorrect type of game object...".
ScriptObject* CheckAnyObject(lua_State* L, int idx);
template <class T>
T* CheckObject(lua_State* L, int idx) {
  ScriptObject* o = CheckAnyObject(L, idx);
  T* t;
  if constexpr (ScriptTypeOf<T>::bit != 0) t = (o->typeBits & ScriptTypeOf<T>::bit) ? static_cast<T*>(o) : nullptr;
  else t = dynamic_cast<T*>(o);
  if (!t) luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of ':'?)");
  return t;
}
// As the original's "Opt" getters (func_GetUnitOpt 0x593970, func_GetCUnitCommandOpt 0x6f8d80): a value that is no
// game object (a nil hole included) is the error "Expected a game object...", a destroyed one nullptr, one of
// another class the error "Incorrect type of game object...".
ScriptObject* OptAnyObject(lua_State* L, int idx);
template <class T>
T* OptObject(lua_State* L, int idx) {
  ScriptObject* o = OptAnyObject(L, idx);
  if (!o) return nullptr;
  T* t;
  if constexpr (ScriptTypeOf<T>::bit != 0) t = (o->typeBits & ScriptTypeOf<T>::bit) ? static_cast<T*>(o) : nullptr;
  else t = dynamic_cast<T*>(o);
  if (!t) luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of ':'?)");
  return t;
}
template <class T>
T* ToObject(lua_State* L, int idx) {
  ScriptObject* o = GetObject(L, idx);
  if constexpr (ScriptTypeOf<T>::bit != 0) return (o && (o->typeBits & ScriptTypeOf<T>::bit)) ? static_cast<T*>(o) : nullptr;
  else return dynamic_cast<T*>(o);
}

}  // namespace moho
