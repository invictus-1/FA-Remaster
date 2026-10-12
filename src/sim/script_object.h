// Engine objects seen from Lua: the simulation's classes.
//
// The handle itself (ScriptObject, BindObject, GetObject, CheckObject) is script/script_handle.h. Here: the
// simulation's class tables in the global `moho` table (moho.unit_methods, ...), on which the Lua classes are
// built (units use their blueprint's script class), and the fast type bits of its most used classes.
#pragma once
#include <cstdint>
#include <string>
#include <typeinfo>

#include "script/lua.hpp"
#include "script/script_handle.h"

namespace moho {

// ScriptTypeOf bits of the engine classes the simulation's scripts use most.
enum ScriptTypeBit : uint32_t {
  kTypeEntity = 1u << 0, kTypeUnit = 1u << 1, kTypeProp = 1u << 2, kTypeProjectile = 1u << 3,
  kTypeShield = 1u << 4, kTypeWeapon = 1u << 5, kTypeBrain = 1u << 6, kTypePlatoon = 1u << 7,
  kTypeNavigator = 1u << 8,
};

// Push moho.<luaName> for an engine class name ("Unit" -> moho.unit_methods).
void PushClassTable(lua_State* L, const char* cppClass);
// New table with metatable moho.<class of cppClass>, bound to obj; pushed.
void CreateObject(lua_State* L, ScriptObject* obj, const char* cppClass);

// Create the `moho` table with a class table for every engine class of the sim (methods are
// logged stubs until implemented) and register stub sim globals.
void RegisterSimClasses(lua_State* L);
// Replace a method of an engine class (and keep it in derived classes that copied it).
void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f);
void SetGlobal(lua_State* L, const char* name, lua_CFunction f);

// Every stub that was called, with a count (reported at exit).
void ReportStubCalls();

}  // namespace moho
