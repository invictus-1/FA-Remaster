// Small engine functions of the user state that need no control tree: time, the command line,
// the interface state, front-end data, random numbers.
#include <cctype>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/log.h"
#include "maui/control.h"
#include "sim/sim.h"
#include "user/bindings.h"
#include "user/user_state.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#elif defined(MOHO64UI_SDL)
#include <SDL3/SDL.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

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

// GetSystemTime() (FA exe 0x843500): not the clock on the wall but the time since the program
// started (the clock GetSystemTimeSeconds gives), as hours:minutes:seconds, the hours counted on
// past 24 ("01:23:45").
int l_GetSystemTime(lua_State* L) {
  if (lua_gettop(L) != 0)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "string GetSystemTime() - returns a formatted string displaying the System time", 0, lua_gettop(L));
  const long seconds = static_cast<long>(UserState::From(L)->now());
  char text[48];
  std::snprintf(text, sizeof text, "%02ld:%02ld:%02ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
  lua_pushstring(L, text);
  return 1;
}

// SetUIControlsAlpha(alpha), GetUIControlsAlpha() (FA exe 0x84df70, 0x84e080): the factor on the
// alpha of every control drawn (UiManager::controlsAlpha).
int l_SetUIControlsAlpha(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "SetUIControlsAlpha(float alpha) -- set the alpha multiplier for 2d UI controls", 1, lua_gettop(L));
  UserState::From(L)->ui().controlsAlpha = static_cast<float>(luaL_checknumber(L, 1));
  return 0;
}
int l_GetUIControlsAlpha(lua_State* L) {
  if (lua_gettop(L) != 0)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "float GetUIControlsAlpha() -- get the alpha multiplier for 2d UI controls", 0, lua_gettop(L));
  lua_pushnumber(L, UserState::From(L)->ui().controlsAlpha);
  return 1;
}

// OpenURL(url) (FA exe 0x848050, read 2026-10-16): the text is taken as a web address the way the
// original's library (wxURL) takes one: the part before the first ':' exactly "http", "ftp" or
// "file", and "//" after it for the first two (so an https address does nothing: FAF's links.lua
// sends http for that reason); and that part must be one the init file's `protocols` table names
// (the case not counting). Then the system opens it (the default browser); otherwise nothing
// happens, and nothing is said either way. (moho64ui_OpenURLDryRun(true): the tests' way to see what
// would be opened, moho64ui_OpenedURL(), without opening it.)
bool g_openDryRun = false;
std::string g_opened;
bool SameNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
void OpenWithSystem(const std::string& url) {
#if defined(_WIN32)
  const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
  std::wstring wide(static_cast<size_t>(n > 0 ? n : 1), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), n);
  ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(MOHO64UI_SDL)
  SDL_OpenURL(url.c_str());
#else
#if defined(__APPLE__)
  const char* opener = "open";
#else
  const char* opener = "xdg-open";
#endif
  char* argv[] = {const_cast<char*>(opener), const_cast<char*>(url.c_str()), nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0) waitpid(pid, nullptr, WNOHANG);
#endif
}
int l_OpenURL(lua_State* L) {
  if (lua_gettop(L) != 1)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "OpenURL(string) - open the default browser window to the specified URL", 1, lua_gettop(L));
  const std::string url = luaL_checkstring(L, 1);
  const size_t colon = url.find(':');
  if (colon == std::string::npos) return 0;
  const std::string scheme = url.substr(0, colon);
  if (scheme != "http" && scheme != "ftp" && scheme != "file") return 0;
  if (scheme != "file" && url.compare(colon + 1, 2, "//") != 0) return 0;
  bool listed = false;
  for (const std::string& p : UserState::From(L)->config().protocols) listed = listed || SameNoCase(p, scheme);
  if (!listed) return 0;
  g_opened = url;
  if (g_openDryRun) Logf(LogLevel::Info, "moho64ui: OpenURL %s (not opened: a test)", url.c_str());
  else OpenWithSystem(url);
  return 0;
}
int l_OpenURLDryRun(lua_State* L) {
  g_openDryRun = lua_toboolean(L, 1) != 0;
  g_opened.clear();
  return 0;
}
int l_OpenedURL(lua_State* L) {
  lua_pushstring(L, g_opened.c_str());
  return 1;
}

// GetArmyScore(army) (FA exe 0x843ed0): the original's is empty: one argument, never read; no session
// is an error ("No session started."); else nothing comes back (nil).
int l_GetArmyScore(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "int GetArmyScore(armyIndex)", 1, lua_gettop(L));
  if (!UserState::From(L)->config().liveSim) return luaL_error(L, "No session started.");
  return 0;
}
// SessionCanRestart() (FA exe 0x8981b0): no session is an error ("no active session."); else
// whether the session keeps what it was started from, which only a single-player game started anew
// does (FA exe 0x88c293: not a multiplayer game, not a replay).
int l_SessionCanRestart(lua_State* L) {
  if (lua_gettop(L) != 0)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "Return true iff the active session can be restarted.", 0, lua_gettop(L));
  Sim* sim = UserState::From(L)->LiveSim("SessionCanRestart");
  if (!sim) return luaL_error(L, "no active session.");
  lua_pushboolean(L, !sim->isReplay);
  return 1;
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
  SetGlobal(L, "GetSystemTime", l_GetSystemTime);
  SetGlobal(L, "SetUIControlsAlpha", l_SetUIControlsAlpha);
  SetGlobal(L, "GetUIControlsAlpha", l_GetUIControlsAlpha);
  SetGlobal(L, "OpenURL", l_OpenURL);
  SetGlobal(L, "GetArmyScore", l_GetArmyScore);
  SetGlobal(L, "SessionCanRestart", l_SessionCanRestart);
  lua_register(L, "moho64ui_OpenURLDryRun", l_OpenURLDryRun);
  lua_register(L, "moho64ui_OpenedURL", l_OpenedURL);
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
