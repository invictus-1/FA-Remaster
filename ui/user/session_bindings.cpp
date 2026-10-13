// A running game as its interface's state sees it: the world interface provider the game script
// makes, the session's facts as functions (scenario, armies, focus army, economy, time), and the
// simulation's sync table after every beat.
//
// The facts come from the engine's view functions (sim/sim_view.h); the state's configuration
// names the simulation. Without one (the front end's state) the functions here are not registered
// and the front end's answers stay (no session, tick 0).
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/replay.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "maui/world_source.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

Sim* TheSim(lua_State* L) { return UserState::From(L)->config().sim; }

void SetField(lua_State* L, const char* key, lua_Number v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
}
void SetField(lua_State* L, const char* key, bool v) {
  lua_pushstring(L, key);
  lua_pushboolean(L, v);
  lua_rawset(L, -3);
}
void SetField(lua_State* L, const char* key, const std::string& v) {
  lua_pushstring(L, key);
  lua_pushstring(L, v.c_str());
  lua_rawset(L, -3);
}

// --- the world interface provider ---
//
// The game script makes one object (InternalCreateWldUIProvider, from gamemain.CreateWldUIProvider)
// and the engine calls its methods as a session goes through its stages: StartLoadingDialog,
// UpdateLoadingDialog(elapsed), StopLoadingDialog, StartWaitingDialog, CreateGameInterface(isReplay),
// DestroyGameInterface (FA exe 0x86a640..: one small function per call).

class WldUIProvider : public ScriptObject {};

int l_InternalCreateWldUIProvider(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  WldUIProvider* p = new WldUIProvider;
  BindObject(L, 1, p);
  UserState::From(L)->provider.reset(p);
  return 0;
}

int l_ProviderDestroy(lua_State* L) {
  UserState* user = UserState::From(L);
  if (user->provider) user->provider->UnbindLua();
  user->provider.reset();
  return 0;
}

// --- the session's facts ---

int l_True(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}
int l_False(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

int l_GameTick(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(TheSim(L)->tick()));
  return 1;
}
// GetGameTimeSeconds(): the game's time; ten ticks are a second.
int l_GetGameTimeSeconds(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(TheSim(L)->tick()) * 0.1f);
  return 1;
}
int l_Zero(lua_State* L) {
  lua_pushnumber(L, 0);
  return 1;
}
// GetGameSpeed() (FA exe 0x88e260) and GetSimRate(): how much faster or slower than normal the
// game runs, in the steps the speed keys count. (The original's GetSimRate is the speed the
// simulation manages; here both are the speed asked for.)
int l_GetGameSpeed(lua_State* L) {
  lua_pushnumber(L, GameSpeedOf(L));
  return 1;
}
// SetGameSpeed(rate) (FA exe 0x88e360): as given, without bounds.
int l_SetGameSpeed(lua_State* L) {
  SetGameSpeedTo(L, static_cast<int>(luaL_checknumber(L, 1)));
  return 0;
}
// SessionRequestPause() / SessionResume() (FA exe 0x897800 / 0x8978d0). The game script hears of
// the pause from the simulation's sync table (PausedBy), as in the original.
int l_SessionRequestPause(lua_State* L) {
  RequestSessionPause(L, true);
  return 0;
}
int l_SessionResume(lua_State* L) {
  RequestSessionPause(L, false);
  return 0;
}
// SessionGetCommandSourceNames() (FA exe 0x897af0): the names of the session's command sources
// (the players who can give orders), by their number from 1. SessionGetLocalCommandSource()
// (0x897c70): the number of the one this player speaks for, 0 when he can give no orders. The
// game's script names who paused by them.
int l_SessionGetCommandSourceNames(lua_State* L) {
  lua_newtable(L);
  int n = 0;
  for (const auto& source : TheSim(L)->cmdSources) {
    lua_pushstring(L, source.name.c_str());
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
int l_SessionGetLocalCommandSource(lua_State* L) {
  int source = UserState::From(L)->config().commandSource;
  bool ours = TheSim(L)->focusArmy >= 0 && source >= 0 && source < static_cast<int>(TheSim(L)->cmdSources.size());
  lua_pushnumber(L, ours ? source + 1 : 0);
  return 1;
}
// SessionEndGame(): the game is over. As in the original it is an order like any other (the
// command stream's EndGame, FA exe 0x74b100): the simulation runs one more beat, in which its
// scripts hand over the final score, and then stands. SessionIsGameOver() says so from the
// moment it is asked for.
int l_SessionEndGame(lua_State* L) {
  if (GamePace* pace = UserState::From(L)->config().pace) pace->ended = true;
  RequestSessionEnd(L);
  return 0;
}
int l_SessionIsGameOver(lua_State* L) {
  const GamePace* pace = UserState::From(L)->config().pace;
  lua_pushboolean(L, (pace && pace->ended) || TheSim(L)->endGame);
  return 1;
}
int l_GetSimTicksPerSecond(lua_State* L) {
  lua_pushnumber(L, 10);
  return 1;
}
int l_SessionIsPaused(lua_State* L) {
  lua_pushboolean(L, TheSim(L)->pauseOwner != -1);
  return 1;
}
// GetFocusArmy(): the army the player looks through, counted from 1; -1 for an observer.
int l_GetFocusArmy(lua_State* L) {
  int a = TheSim(L)->focusArmy;
  lua_pushnumber(L, a < 0 ? -1 : a + 1);
  return 1;
}

// SessionGetScenarioInfo(): the scenario table the session was launched with; the same table on
// every call.
int l_SessionGetScenarioInfo(lua_State* L) {
  lua_pushstring(L, "moho64ui.scenario");
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return 1;
  lua_pop(L, 1);
  Sim* sim = TheSim(L);
  if (sim->sessionScenario.empty() || !PushSerializedLua(L, sim->sessionScenario) || !lua_istable(L, -1)) {
    lua_settop(L, 0);
    lua_newtable(L);
  }
  lua_pushstring(L, "moho64ui.scenario");
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
  return 1;
}

// A colour of /lua/GameColors.lua by its list and index (1-based), as the "aarrggbb" string scripts
// use; empty when there is none.
std::string GameColor(lua_State* L, const char* list, int index) {
  std::string out;
  int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/GameColors.lua");
  if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "GameColors");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, list);
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) lua_rawgeti(L, -1, index);
      if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return out;
}

// GetArmiesTable(): {numArmies, focusArmy, armiesTable = {[army] = {name, nickname, faction, human,
// An army's two colours as "aarrggbb" strings: the player's and the icons'.
void ArmyColors(lua_State* L, const ArmyView& a, std::string* color, std::string* icon) {
  if (a.rgb[0] >= 0) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "ff%02x%02x%02x", a.rgb[0] & 255, a.rgb[1] & 255, a.rgb[2] & 255);
    *color = *icon = buf;
    return;
  }
  if (a.playerColor > 0) *color = GameColor(L, "PlayerColors", a.playerColor);
  if (a.armyColor > 0) *icon = GameColor(L, "ArmyColors", a.armyColor);
  if (color->empty()) *color = *icon;
  if (icon->empty()) *icon = *color;
  if (color->empty()) *color = *icon = "ff808080";
}

// civilian, color, iconColor, showScore, outOfGame, authorizedCommandSources}}}. Field names as the
// game's scripts read them; an army's colours are the lobby's choices looked up in the game's colour
// lists. (Inferred: which list `color` and which `iconColor` takes.)
int l_GetArmiesTable(lua_State* L) {
  Sim* sim = TheSim(L);
  std::vector<ArmyView> armies = ArmyViews(*sim);
  lua_newtable(L);
  SetField(L, "numArmies", static_cast<lua_Number>(armies.size()));
  SetField(L, "focusArmy", static_cast<lua_Number>(sim->focusArmy < 0 ? -1 : sim->focusArmy + 1));
  lua_pushstring(L, "armiesTable");
  lua_newtable(L);
  for (const ArmyView& a : armies) {
    lua_newtable(L);
    SetField(L, "name", a.name);
    SetField(L, "nickname", a.nickname);
    SetField(L, "faction", static_cast<lua_Number>(a.faction));
    SetField(L, "human", a.human);
    SetField(L, "civilian", a.civilian);
    SetField(L, "showScore", a.showScore);
    SetField(L, "outOfGame", a.outOfGame);
    std::string color, icon;
    ArmyColors(L, a, &color, &icon);
    SetField(L, "color", color);
    SetField(L, "iconColor", icon);
    lua_pushstring(L, "authorizedCommandSources");
    lua_newtable(L);
    if (a.human) {
      lua_pushnumber(L, 1);
      lua_rawseti(L, -2, 1);
    }
    lua_rawset(L, -3);
    lua_rawseti(L, -2, a.index);
  }
  lua_rawset(L, -3);
  return 1;
}

// GetEconomyTotals(): the focus army's economy, each entry a table with ENERGY and MASS. The rates
// are per tick, as the engine keeps them.
int l_GetEconomyTotals(lua_State* L) {
  Sim* sim = TheSim(L);
  EconomyTotals e = EconomyTotalsOf(*sim, sim->focusArmy + 1);
  auto pair = [&](const char* key, const float v[2]) {
    lua_pushstring(L, key);
    lua_newtable(L);
    SetField(L, "ENERGY", static_cast<lua_Number>(v[0]));
    SetField(L, "MASS", static_cast<lua_Number>(v[1]));
    lua_rawset(L, -3);
  };
  lua_newtable(L);
  pair("income", e.income);
  pair("lastUseRequested", e.lastUseRequested);
  pair("lastUseActual", e.lastUseActual);
  pair("stored", e.stored);
  pair("maxStorage", e.maxStorage);
  pair("reclaimed", e.reclaimed);
  return 1;
}

// GameTime(): the game's time in seconds. GetGameTime(): the same as text.
std::string TimeText(double seconds) {
  int s = seconds > 0 ? static_cast<int>(seconds) : 0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  return buf;
}
int l_GameTime(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(TheSim(L)->tick()) * 0.1f);
  return 1;
}
int l_GetGameTime(lua_State* L) {
  lua_pushstring(L, TimeText(TheSim(L)->tick() * 0.1).c_str());
  return 1;
}
// FormatTime(seconds): hours, minutes and seconds as "HH:MM:SS". (Inferred form.)
int l_FormatTime(lua_State* L) {
  lua_pushstring(L, TimeText(luaL_checknumber(L, 1)).c_str());
  return 1;
}

// ValidateUnitsList(units): the units of a list that still exist. The interface has no units of
// its own yet, so none do.
int l_ValidateUnitsList(lua_State* L) {
  lua_newtable(L);
  return 1;
}

// IsAlly(army1, army2), IsEnemy, IsNeutral: how the first army stands towards the second
// (armies counted from 1). An army stands as an ally towards itself.
int Stance(lua_State* L) {
  int a = static_cast<int>(luaL_checknumber(L, 1)), b = static_cast<int>(luaL_checknumber(L, 2));
  if (a == b) return 2;
  for (const ArmyView& v : ArmyViews(*TheSim(L)))
    if (v.index == a && b >= 1 && b <= static_cast<int>(v.alliance.size())) return v.alliance[static_cast<size_t>(b - 1)];
  return 0;
}
int l_IsAlly(lua_State* L) {
  lua_pushboolean(L, Stance(L) == 2);
  return 1;
}
int l_IsNeutral(lua_State* L) {
  lua_pushboolean(L, Stance(L) == 1);
  return 1;
}
int l_IsEnemy(lua_State* L) {
  lua_pushboolean(L, Stance(L) == 0);
  return 1;
}

// GetSessionClients(): the one local player. (Field names as the scripts read them.)
int l_GetSessionClients(lua_State* L) {
  Sim* sim = TheSim(L);
  std::string name;
  for (const ArmyView& a : ArmyViews(*sim))
    if (a.index == sim->focusArmy + 1) name = a.nickname;
  lua_newtable(L);
  lua_newtable(L);
  SetField(L, "name", name);
  SetField(L, "connected", true);
  SetField(L, "local", true);
  SetField(L, "quiet", static_cast<lua_Number>(0));
  SetField(L, "uid", std::string("0"));
  lua_pushstring(L, "authorizedCommandSources");
  lua_newtable(L);
  lua_pushnumber(L, 1);
  lua_rawseti(L, -2, 1);
  lua_rawset(L, -3);
  lua_pushstring(L, "ejectedBy");
  lua_newtable(L);
  lua_rawset(L, -3);
  lua_rawseti(L, -2, 1);
  return 1;
}

}  // namespace

void UserState::CallProvider(const char* fn, int nargs) {
  lua_State* L = state_->L();
  int base = lua_gettop(L) - nargs;
  if (!provider || !provider->HasLuaObject()) {
    lua_settop(L, base);
    return;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  PushObject(L, provider.get());
  lua_pushstring(L, fn);
  lua_gettable(L, -2);
  if (lua_isfunction(L, -1)) {
    lua_insert(L, base + 2);  // function, then the arguments, with self to go before them
    lua_insert(L, base + 3);
    bool ok = lua_pcall(L, nargs + 1, 0, base + 1) == 0;
    if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    Logf(LogLevel::Info, "moho64ui: provider:%s %s", fn, ok ? "ok" : "FAILED");
  }
  lua_settop(L, base);
}

// After a beat (the original's hand-over, as /lua/UserSync.lua expects it): the table the
// simulation's scripts filled becomes the global Sync, the one before it PreviousSync, and
// OnSync() runs.
void UserState::DeliverSync(const std::string& blob) {
  lua_State* L = state_->L();
  RefreshUserUnits(L);
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "Sync");
  lua_setglobal(L, "PreviousSync");
  if (!PushSerializedLua(L, blob) || !lua_istable(L, -1)) {
    lua_settop(L, top + 1);
    lua_newtable(L);
  }
  lua_setglobal(L, "Sync");
  lua_getglobal(L, "OnSync");
  if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, top + 1) != 0)
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, top + 1);
  // Then the beat itself: gamemain.OnBeat() runs what the panels registered to do every beat
  // (the original calls it by name, FA exe 0x83dd08). Inferred: that it follows the sync.
  if (gameInterface) {
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/gamemain.lua");
    if (lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "OnBeat");
      lua_gettable(L, -2);
      if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, top + 1) != 0)
        LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
    }
  }
  lua_settop(L, top);
}

std::string ArmyColorOf(lua_State* L, int army) {
  std::string color, icon;
  for (const ArmyView& a : ArmyViews(*TheSim(L)))
    if (a.index == army) ArmyColors(L, a, &color, &icon);
  return color.empty() ? "ff808080" : color;
}

int GameSpeedOf(lua_State* L) {
  const GamePace* pace = UserState::From(L)->config().pace;
  return pace ? pace->rate : 0;
}
void SetGameSpeedTo(lua_State* L, int rate) {
  GamePace* pace = UserState::From(L)->config().pace;
  if (pace && pace->adjustable) pace->rate = rate;
}

void RegisterSessionBindings(lua_State* L) {
  SetGlobal(L, "InternalCreateWldUIProvider", l_InternalCreateWldUIProvider);
  SetMethod(L, "CLuaWldUIProvider", "Destroy", l_ProviderDestroy);
  SetGlobal(L, "FormatTime", l_FormatTime);
  if (!TheSim(L)) return;
  SetGlobal(L, "GameTime", l_GameTime);
  SetGlobal(L, "GetGameTime", l_GetGameTime);
  SetGlobal(L, "ValidateUnitsList", l_ValidateUnitsList);
  for (const char* name : {"SessionIsActive", "WorldIsPlaying"}) SetGlobal(L, name, l_True);
  for (const char* name : {"SessionIsReplay", "WorldIsLoading", "SessionIsMultiplayer",
                           "SessionIsBeingRecorded", "SessionIsObservingAllowed", "IsObserver"})
    SetGlobal(L, name, l_False);
  SetGlobal(L, "GameTick", l_GameTick);
  SetGlobal(L, "GetGameTimeSeconds", l_GetGameTimeSeconds);
  SetGlobal(L, "GetSimTicksPerSecond", l_GetSimTicksPerSecond);
  SetGlobal(L, "GetSimRate", l_GetGameSpeed);
  SetGlobal(L, "GetGameSpeed", l_GetGameSpeed);
  SetGlobal(L, "SetGameSpeed", l_SetGameSpeed);
  SetGlobal(L, "SessionRequestPause", l_SessionRequestPause);
  SetGlobal(L, "SessionResume", l_SessionResume);
  SetGlobal(L, "SessionEndGame", l_SessionEndGame);
  SetGlobal(L, "SessionGetCommandSourceNames", l_SessionGetCommandSourceNames);
  SetGlobal(L, "SessionGetLocalCommandSource", l_SessionGetLocalCommandSource);
  SetGlobal(L, "SessionIsGameOver", l_SessionIsGameOver);
  SetGlobal(L, "SessionIsPaused", l_SessionIsPaused);
  SetGlobal(L, "GetFocusArmy", l_GetFocusArmy);
  SetGlobal(L, "SessionGetScenarioInfo", l_SessionGetScenarioInfo);
  SetGlobal(L, "GetArmiesTable", l_GetArmiesTable);
  SetGlobal(L, "GetEconomyTotals", l_GetEconomyTotals);
  SetGlobal(L, "GetSessionClients", l_GetSessionClients);
  SetGlobal(L, "IsAlly", l_IsAlly);
  SetGlobal(L, "IsNeutral", l_IsNeutral);
  SetGlobal(L, "IsEnemy", l_IsEnemy);
}

}  // namespace moho::ui
