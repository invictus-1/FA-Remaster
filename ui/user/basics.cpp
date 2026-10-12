// Small engine functions of the user state that need no control tree: time, the command line,
// the interface state, front-end data, random numbers.
#include <cctype>
#include <random>
#include <string>

#include "core/log.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

// CurrentTime() / GetSystemTimeSeconds(): seconds since the application started.
int l_CurrentTime(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(UserState::From(L)->now()));
  return 1;
}

// GameTick(): the session's tick; 0 while no session runs.
int l_GameTick(lua_State* L) {
  lua_pushnumber(L, 0);
  return 1;
}

bool SameOption(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// HasCommandLineArg("/option")
int l_HasCommandLineArg(lua_State* L) {
  std::string opt = luaL_checkstring(L, 1);
  bool has = false;
  for (const auto& a : UserState::From(L)->config().gameArgs) has = has || SameOption(a, opt);
  lua_pushboolean(L, has);
  return 1;
}

// GetCommandLineArg("/option", n): the n arguments after the option as a table, or false when the
// option (or enough arguments) is not there.
int l_GetCommandLineArg(lua_State* L) {
  std::string opt = luaL_checkstring(L, 1);
  int n = static_cast<int>(luaL_optnumber(L, 2, 1));
  const auto& args = UserState::From(L)->config().gameArgs;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!SameOption(args[i], opt)) continue;
    if (n < 0 || i + static_cast<size_t>(n) >= args.size() + (n == 0)) break;  // not enough arguments after it
    lua_newtable(L);
    for (int k = 1; k <= n; ++k) {
      lua_pushstring(L, args[i + static_cast<size_t>(k)].c_str());
      lua_rawseti(L, -2, k);
    }
    return 1;
  }
  lua_pushboolean(L, 0);
  return 1;
}

int l_GetCurrentUIState(lua_State* L) {
  lua_pushstring(L, UserState::From(L)->uiState().c_str());
  return 1;
}

int l_DebugFacilitiesEnabled(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

// SetFrontEndData(key, value) / GetFrontEndData(key): values the front end and a session's
// interface hand each other (the original keeps them across its user states).
const char* kFrontEndKey = "moho64ui.frontenddata";

void PushFrontEndData(lua_State* L) {
  lua_pushstring(L, kFrontEndKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushstring(L, kFrontEndKey);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

int l_SetFrontEndData(lua_State* L) {
  luaL_checkany(L, 1);
  lua_settop(L, 2);
  PushFrontEndData(L);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_rawset(L, -3);
  return 0;
}

int l_GetFrontEndData(lua_State* L) {
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  PushFrontEndData(L);
  lua_pushvalue(L, 1);
  lua_rawget(L, -2);
  return 1;
}

// Random(): [0, 1); Random(n): 1..n; Random(a, b): a..b. The interface's own generator: nothing
// here may draw from the simulation's.
int l_Random(lua_State* L) {
  static std::mt19937 rng(std::random_device{}());
  int n = lua_gettop(L);
  if (n == 0) {
    lua_pushnumber(L, static_cast<lua_Number>(std::uniform_real_distribution<double>(0.0, 1.0)(rng)));
    return 1;
  }
  int lo = 1, hi = static_cast<int>(luaL_checknumber(L, 1));
  if (n >= 2) {
    lo = hi;
    hi = static_cast<int>(luaL_checknumber(L, 2));
  }
  if (hi < lo) return luaL_error(L, "Random: interval is empty");
  lua_pushnumber(L, static_cast<lua_Number>(std::uniform_int_distribution<int>(lo, hi)(rng)));
  return 1;
}

}  // namespace

void RegisterBasicBindings(lua_State* L) {
  SetGlobal(L, "CurrentTime", l_CurrentTime);
  SetGlobal(L, "GetSystemTimeSeconds", l_CurrentTime);
  SetGlobal(L, "GameTick", l_GameTick);
  SetGlobal(L, "HasCommandLineArg", l_HasCommandLineArg);
  SetGlobal(L, "GetCommandLineArg", l_GetCommandLineArg);
  SetGlobal(L, "GetCurrentUIState", l_GetCurrentUIState);
  SetGlobal(L, "DebugFacilitiesEnabled", l_DebugFacilitiesEnabled);
  SetGlobal(L, "SetFrontEndData", l_SetFrontEndData);
  SetGlobal(L, "GetFrontEndData", l_GetFrontEndData);
  SetGlobal(L, "Random", l_Random);
}

}  // namespace moho::ui
