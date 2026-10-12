// The user (interface) Lua state's engine functions.
//
// The original gives the user state 461 functions of its own plus the set every state has. All of
// them exist here from the start: a function that is not built yet is a stub that logs
// `moho64ui: stub <name>` the first time a script calls it and is counted, so the log of a run
// says what the scripts needed that we do not have.
#pragma once
#include <string>

#include "script/lua.hpp"

namespace moho::ui {

// Create the `moho` table with a class table for every engine class of the user state, and a stub
// for every engine function that is not defined yet. Call after the functions that exist already
// (core, threads) are registered.
void RegisterUserClasses(lua_State* L);

// Replace a stub with the real function.
void SetGlobal(lua_State* L, const char* name, lua_CFunction f);
void SetMethod(lua_State* L, const char* cppClass, const char* name, lua_CFunction f);

// Push moho.<name> for an engine class ("CMauiBitmap" -> moho.bitmap_methods).
void PushUserClassTable(lua_State* L, const char* cppClass);

// Log every stub that was called, with its count.
void ReportStubCalls();
// Log every engine function of the user state as `built` or `stub`, and the totals.
void ListBindings();

}  // namespace moho::ui
