// Small engine functions of the user state that need no control tree: time, the command line,
// the interface state, front-end data, random numbers.
#include <cctype>
#include <map>
#include <random>
#include <string>
#include <vector>

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
// interface hand each other (the next operation's briefing after a won operation, a replay's
// name), kept across the user states as the original keeps them: a copy of plain values (nil,
// booleans, numbers, strings, and tables of those; functions and engine objects are not kept),
// made again in the state that asks. (Until 2026-10-16 they lived in one state's registry and were
// lost when the game's state gave way to the front end's.)
struct FrontEndValue {
  int type = LUA_TNIL;
  bool boolean = false;
  lua_Number number = 0;
  std::string text;
  std::vector<std::pair<FrontEndValue, FrontEndValue>> table;
};
std::map<std::string, FrontEndValue>& FrontEndStore() {  // by the key as text (FAF's keys are strings)
  static std::map<std::string, FrontEndValue> store;
  return store;
}
FrontEndValue ReadFrontEndValue(lua_State* L, int idx, int depth) {
  FrontEndValue v;
  idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: v.type = LUA_TBOOLEAN, v.boolean = lua_toboolean(L, idx) != 0; break;
    case LUA_TNUMBER: v.type = LUA_TNUMBER, v.number = lua_tonumber(L, idx); break;
    case LUA_TSTRING: v.type = LUA_TSTRING, v.text = lua_tostring(L, idx); break;
    case LUA_TTABLE:
      v.type = LUA_TTABLE;
      if (depth > 16) break;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        FrontEndValue key = ReadFrontEndValue(L, -2, depth + 1), value = ReadFrontEndValue(L, -1, depth + 1);
        if (key.type != LUA_TNIL && value.type != LUA_TNIL) v.table.emplace_back(std::move(key), std::move(value));
        lua_pop(L, 1);
      }
      break;
    default: break;  // (nil, functions, engine objects: nothing)
  }
  return v;
}
void PushFrontEndValue(lua_State* L, const FrontEndValue& v) {
  switch (v.type) {
    case LUA_TBOOLEAN: lua_pushboolean(L, v.boolean); break;
    case LUA_TNUMBER: lua_pushnumber(L, v.number); break;
    case LUA_TSTRING: lua_pushlstring(L, v.text.data(), v.text.size()); break;
    case LUA_TTABLE:
      lua_newtable(L);
      for (const auto& [key, value] : v.table) {
        PushFrontEndValue(L, key);
        PushFrontEndValue(L, value);
        lua_rawset(L, -3);
      }
      break;
    default: lua_pushnil(L); break;
  }
}
std::string FrontEndKey(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TSTRING || lua_type(L, idx) == LUA_TNUMBER) return lua_tostring(L, idx);
  return std::string();
}

int l_SetFrontEndData(lua_State* L) {
  luaL_checkany(L, 1);
  lua_settop(L, 2);
  const std::string key = FrontEndKey(L, 1);
  if (lua_isnil(L, 2)) FrontEndStore().erase(key);
  else FrontEndStore()[key] = ReadFrontEndValue(L, 2, 0);
  return 0;
}

int l_GetFrontEndData(lua_State* L) {
  luaL_checkany(L, 1);
  auto it = FrontEndStore().find(FrontEndKey(L, 1));
  if (it == FrontEndStore().end()) lua_pushnil(L);
  else PushFrontEndValue(L, it->second);
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

bool g_exitRequested = false;
// ExitApplication(): the program ends (the main menu's Exit, the game menu's "exit to Windows").
int l_ExitApplication(lua_State*) {
  g_exitRequested = true;
  Logf(LogLevel::Info, "moho64ui: ExitApplication");
  return 0;
}

int g_sessionRequest = 0;
// ExitGame(): the game ends and the front end comes back. RestartSession(): the same game starts
// over. Both are the host's to do, between two frames.
int l_ExitGame(lua_State*) {
  g_sessionRequest = 1;
  return 0;
}
int l_RestartSession(lua_State*) {
  g_sessionRequest = 2;
  return 0;
}

}  // namespace

bool ExitRequested() { return g_exitRequested; }
int TakeSessionRequest() {
  int request = g_sessionRequest;
  g_sessionRequest = 0;
  return request;
}

void RegisterBasicBindings(lua_State* L) {
  SetGlobal(L, "ExitApplication", l_ExitApplication);
  SetGlobal(L, "ExitGame", l_ExitGame);
  SetGlobal(L, "RestartSession", l_RestartSession);
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

  // The front end: no session runs, no world loads, and no GPGNet is connected (sending to it
  // does nothing).
  static const lua_CFunction kFalse = [](lua_State* S) -> int {
    lua_pushboolean(S, 0);
    return 1;
  };
  static const lua_CFunction kNothing = [](lua_State*) -> int { return 0; };
  for (const char* name : {"SessionIsActive", "SessionIsReplay", "WorldIsLoading", "WorldIsPlaying", "GpgNetActive"})
    SetGlobal(L, name, kFalse);
  SetGlobal(L, "GpgNetSend", kNothing);
  // GetAntiAliasingOptions(): the anti-aliasing sample counts the graphics card offers (the game's
  // Fidelity Presets take the last as Ultra's, the middle one as High's). Here the renderer's:
  // 2 and 4 samples (its scene draws with 1, 2 or 4).
  SetGlobal(L, "GetAntiAliasingOptions", [](lua_State* L) -> int {
    lua_newtable(L);
    lua_pushnumber(L, 2);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 4);
    lua_rawseti(L, -2, 2);
    return 1;
  });
}

}  // namespace moho::ui
