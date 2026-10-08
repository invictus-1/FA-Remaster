// Engine objects seen from Lua.
//
// The original exposes each engine object (unit, brain, manipulator, ...) as a Lua table whose
// `_c_object` field holds a handle to the C++ object; methods find their object through it.
// The table's class comes from the global `moho` table (moho.unit_methods, ...), or from a Lua
// class built on top of it (units use their blueprint's script class).
#pragma once
#include <string>
#include <typeinfo>

#include "script/lua.hpp"

namespace moho {

class ScriptObject {
 public:
  virtual ~ScriptObject();

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

// Make the table at `idx` the Lua side of `obj` (sets its `_c_object`).
void BindObject(lua_State* L, int idx, ScriptObject* obj);
// Push the object's table (nil if it has none).
void PushObject(lua_State* L, ScriptObject* obj);
// Push moho.<luaName> for an engine class name ("Unit" -> moho.unit_methods).
void PushClassTable(lua_State* L, const char* cppClass);
// New table with metatable moho.<class of cppClass>, bound to obj; pushed.
void CreateObject(lua_State* L, ScriptObject* obj, const char* cppClass);

// The engine object behind the value at idx: nullptr if it is not a game object or was destroyed.
ScriptObject* GetObject(lua_State* L, int idx);
// As the original: errors "Expected a game object..." / "Game object has been destroyed" /
// "Incorrect type of game object...".
ScriptObject* CheckAnyObject(lua_State* L, int idx);
template <class T>
T* CheckObject(lua_State* L, int idx) {
  ScriptObject* o = CheckAnyObject(L, idx);
  T* t = dynamic_cast<T*>(o);
  if (!t) luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of ':'?)");
  return t;
}
template <class T>
T* ToObject(lua_State* L, int idx) {
  return dynamic_cast<T*>(GetObject(L, idx));
}

// Create the `moho` table with a class table for every engine class of the sim (methods are
// logged stubs until implemented) and register stub sim globals.
void RegisterSimClasses(lua_State* L);
// Replace a method of an engine class (and keep it in derived classes that copied it).
void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f);
void SetGlobal(lua_State* L, const char* name, lua_CFunction f);

// Every stub that was called, with a count (reported at exit).
void ReportStubCalls();

}  // namespace moho
