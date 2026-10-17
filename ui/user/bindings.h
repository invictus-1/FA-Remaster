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
// The binding profile (MOHO64UI_BINDING_PROFILE=1): whether it is on, and a part of an engine
// function's work counted under its own name ("(GetRightMouseButtonOrder: UnitUnder)").
bool BindingProfileOn();
void AddProfileCost(const char* part, double seconds);
// The engine function a script is in now ("" when none, or the profile is off).
const char* CurrentBindingName();

// Push moho.<name> for an engine class ("CMauiBitmap" -> moho.bitmap_methods).
void PushUserClassTable(lua_State* L, const char* cppClass);

// Log every stub that was called, with its count.
void ReportStubCalls();
// Log every engine function of the user state as `built` or `stub`, and the totals.
void ListBindings();
// The engine functions put into a state after its stubs, by the shared script core (the categories'
// functions with a game's blueprints): counted as built when they are no longer the stub.
void NoteRealGlobals(lua_State* L);

}  // namespace moho::ui
