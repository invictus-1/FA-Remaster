// A running game as its interface's state sees it: the world interface provider the game script
// makes, the session's facts as functions (scenario, armies, focus army, economy, time), and the
// simulation's sync table after every beat.
//
// The facts come from the engine's view functions (sim/sim_view.h); the state's configuration
// names the simulation. Without one (the front end's state) the functions here are not registered
// and the front end's answers stay (no session, tick 0).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "draw/draw.h"
#include "script/script_handle.h"
#include "script/script_state.h"
#include "sim/replay.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/terrain.h"
#include "maui/world_source.h"
#include "user/bindings.h"
#include "user/user_state.h"

namespace moho::ui {
namespace {

Sim* TheSim(lua_State* L) { return UserState::From(L)->LiveSim(); }

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

// The simulation's tick as the interface knows it: the copy of the last beat's sync once there is
// one (asked every frame, also while a beat runs on its own thread), else asked now.
uint32_t TickNow(lua_State* L) {
  UserState* user = UserState::From(L);
  return user->economyCopied ? user->tickCopy : TheSim(L)->tick();
}
// The game's time in seconds as the interface's clock shows it: the tick of the last sync and the part
// of the next one gone by since (FA exe GameTime 0x8be320, GetGameTimeSeconds 0x843400, GetGameTime
// 0x843250: (CWldSession+0x458 + +0x460) * 0.1).
double SecondsNow(lua_State* L) {
  UserState* user = UserState::From(L);
  return (static_cast<double>(TickNow(L)) + (user->economyCopied ? user->tickFraction : 0.0)) * 0.1;
}
int l_GameTick(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(TickNow(L)));
  return 1;
}
// GetGameTimeSeconds(): the game's time; ten ticks are a second.
int l_GetGameTimeSeconds(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(SecondsNow(L)));
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
// SessionIsReplay(): whether the game being shown is a replay played back (the simulation's own flag).
int l_SessionIsReplay(lua_State* L) {
  UserState* user = UserState::From(L);
  lua_pushboolean(L, user->economyCopied ? user->isReplayCopy : TheSim(L)->isReplay);
  return 1;
}
int l_SessionIsPaused(lua_State* L) {
  // (in a replay also while its viewer holds it: ReplayHeld)
  lua_pushboolean(L, TheSim(L)->pauseOwner != -1 || (TheSim(L)->isReplay && ReplayHeld()));
  return 1;
}
// GetFocusArmy(): the army the player looks through, counted from 1; -1 for an observer.
int l_GetFocusArmy(lua_State* L) {
  UserState* user = UserState::From(L);
  int a = user->economyCopied ? user->focusArmyCopy : TheSim(L)->focusArmy;  // (the beat's copy, once there is one)
  lua_pushnumber(L, a < 0 ? -1 : a + 1);
  return 1;
}

// SetFocusArmy(n) (FA exe 0x842ee0; the replay's army list, NoteGameOver's -1, the score screen):
// look through army n (1-based) or, with -1, at every army. Checked as the original's
// (CWldSession::RequestFocusArmy 0x8965e0, ValidateFocusArmyRequest 0x896670): allowed with no
// command source of one's own (a replay's viewer), with cheats on, for one's own army, or once the
// game is over; else only where observing is allowed and the player's source belongs to no army at
// all (FAF's exe makes the original's "no army still in the game" that): never in our games, where
// it always belongs to the player's army. A request the checks refuse is dropped quietly. The
// engine applies it at its next Sync (Sim::RequestFocusArmy, engine request 29); this state takes
// the change from that beat's copy (CopyFromSimulation).
int l_SetFocusArmy(lua_State* L) {
  if (lua_gettop(L) != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", "SetFocusArmy", 1, lua_gettop(L));
  const int n = static_cast<int>(luaL_checknumber(L, 1));
  UserState* user = UserState::From(L);
  Sim* sim = TheSim(L);
  if (!sim) return luaL_error(L, "No session started.");
  const int armies = static_cast<int>(user->economyCopied ? user->armiesCopy.size() : ArmyViews(*sim).size());
  if (n != -1 && (n < 1 || n > armies))
    return luaL_error(L, "Invalid army index of %d; must be between 1 and %d inclusive.", n, armies);
  const int wanted = n == -1 ? -1 : n - 1;
  const bool replay = user->economyCopied ? user->isReplayCopy : sim->isReplay;
  bool cheats = false;
  {
    int top = lua_gettop(L);
    lua_getglobal(L, "SessionGetScenarioInfo");
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "Options");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "CheatsEnabled");
        lua_gettable(L, -2);
        cheats = lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "true";
      }
    }
    lua_settop(L, top);
  }
  const GamePace* pace = user->config().pace;
  const bool over = (pace && pace->ended) || sim->endGame;
  const int own = user->config().ownArmy;
  const bool allowed = replay || own < 0 || cheats || (wanted != -1 && wanted == own) || over;
  if (!allowed) return 0;
  sim->RequestFocusArmy(wanted);
  return 0;
}

// IsObserver() (FA exe 0x843150): true when there is no focus army (a replay or an observer looking
// at every army); as GetFocusArmy, from the beat's copy.
int l_IsObserver(lua_State* L) {
  UserState* user = UserState::From(L);
  const int a = user->economyCopied ? user->focusArmyCopy : TheSim(L)->focusArmy;
  lua_pushboolean(L, a < 0);
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
  UserState* user = UserState::From(L);
  const bool copied = user->economyCopied;  // (the beat's copies, once there are: no waiting for a beat)
  Sim* sim = copied ? nullptr : TheSim(L);
  const std::vector<ArmyView> armies = copied ? user->armiesCopy : ArmyViews(*sim);
  const int focus = copied ? user->focusArmyCopy : sim->focusArmy;
  lua_newtable(L);
  SetField(L, "numArmies", static_cast<lua_Number>(armies.size()));
  SetField(L, "focusArmy", static_cast<lua_Number>(focus < 0 ? -1 : focus + 1));
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
  // (The copy of the last beat's sync: the economy panel asks every frame, also while a beat runs.)
  UserState* user = UserState::From(L);
  EconomyTotals e;
  static_assert(sizeof(EconomyTotals) == sizeof user->economyCopy, "EconomyTotals: twelve floats");
  if (user->economyCopied) {
    std::memcpy(&e, user->economyCopy, sizeof e);
  } else {
    Sim* sim = TheSim(L);
    e = EconomyTotalsOf(*sim, sim->focusArmy + 1);
  }
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
  lua_pushnumber(L, static_cast<lua_Number>(SecondsNow(L)));
  return 1;
}
int l_GetGameTime(lua_State* L) {
  lua_pushstring(L, TimeText(SecondsNow(L)).c_str());
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

// TeamColorMode(on | "aarrggbb,aarrggbb,...") (FA exe 0x847e10 with FAF's patch 0x12919a6): a
// boolean switches the team colours of the strategic icons; anything else is read as FAF's list of
// colours, one per army counted from 0 (hex digits; any other character ends a colour), and leaves
// the switch as it is. (The original's own rule, GameColors.TeamColorMode's Self / Ally / Enemy /
// Neutral by how each army stands to the player's, colours an army the list leaves out.)
int l_TeamColorMode(lua_State* L) {
  UserState* user = UserState::From(L);
  if (lua_type(L, 1) == LUA_TBOOLEAN) {
    user->teamColors = lua_toboolean(L, 1) != 0;
    return 0;
  }
  if (!lua_isstring(L, 1)) return 0;  // (the original's crashes on nil)
  // (As FAF's parser: every character that is not a hex digit, and the end, stores the colour so
  // far for the next army; FAF's table holds 32.)
  const std::string text = lua_tostring(L, 1);
  size_t army = 0;
  uint32_t color = 0;
  for (size_t i = 0; i <= text.size() && army < 32; ++i) {
    const char ch = i < text.size() ? static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))) : '\0';
    if (std::isxdigit(static_cast<unsigned char>(ch))) {
      color = color << 4 | static_cast<uint32_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
      continue;
    }
    if (user->teamColorTable.size() <= army) user->teamColorTable.resize(army + 1, 0);
    user->teamColorTable[army++] = color;
    color = 0;
  }
  return 0;
}

// GetSessionClients(): the one local player. (Field names as the scripts read them.)
int l_GetSessionClients(lua_State* L) {
  UserState* user = UserState::From(L);
  std::string name = user->focusNickname;  // (the beat's copy; before the first: asked now)
  if (!user->economyCopied) {
    Sim* sim = TheSim(L);
    for (const ArmyView& a : ArmyViews(*sim))
      if (a.index == sim->focusArmy + 1) name = a.nickname;
  }
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

// SessionSendChatMessage([clients,] message): a message to the session's clients, which the
// game's script uses for chat lines, pings, pause notices, taunts and more; each receiver's
// script hears it as gamemain.ReceiveChat(sender's name, message), and so does the sender's own
// when it is among the receivers (so the script expects: a ping shows by its own echo, and
// "Engine doesn't bounce private messages back to the sender" is the one case it works round).
// `clients`: a client's number or a list of them, as GetSessionClients lists them; left out,
// everyone. Here there is one client, the player's own (number 1): the message comes back to
// it on the next frame. Written from the scripts' use; the original's code is not read.
int l_SessionSendChatMessage(lua_State* L) {
  if (!TheSim(L)) return 0;
  const int msg = lua_gettop(L) >= 2 ? 2 : 1;
  if (!lua_istable(L, msg) && !lua_isstring(L, msg)) return 0;
  bool toSelf = msg == 1;
  if (msg == 2) {
    if (lua_isnumber(L, 1)) toSelf = lua_tonumber(L, 1) == 1;
    else if (lua_istable(L, 1)) {
      lua_pushnil(L);
      while (lua_next(L, 1) != 0) {
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) == 1) toSelf = true;
        lua_pop(L, 1);
      }
    } else if (lua_isnil(L, 1)) toSelf = true;
  }
  if (!toSelf) return 0;
  // (kept in the registry until the frame's turn: registry.moho64ui_chat = {message, ...})
  lua_pushstring(L, "moho64ui_chat");
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, "moho64ui_chat");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  lua_pushvalue(L, msg);
  lua_rawseti(L, -2, luaL_getn(L, -2) + 1);
  lua_pop(L, 1);
  return 0;
}

}  // namespace

// The chat messages sent since the last frame reach the game's script (see above).
void DeliverChat(lua_State* L) {
  const int top = lua_gettop(L);
  lua_pushstring(L, "moho64ui_chat");
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (!lua_istable(L, -1) || luaL_getn(L, -1) == 0 || !UserState::From(L)->HasSim()) {
    lua_settop(L, top);
    return;
  }
  Sim* sim = UserState::From(L)->LiveSim("chat");  // (only when there is chat: it may wait for a beat)
  const int list = lua_gettop(L);
  lua_pushstring(L, "moho64ui_chat");  // (a fresh list: a handler may send again)
  lua_pushnil(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
  std::string name;
  for (const ArmyView& a : ArmyViews(*sim))
    if (a.index == sim->focusArmy + 1) name = a.nickname;
  const int n = luaL_getn(L, list);
  for (int i = 1; i <= n; ++i) {
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/ui/game/gamemain.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_pushstring(L, "ReceiveChat");
      lua_gettable(L, -2);
      if (lua_isfunction(L, -1)) {
        lua_pushstring(L, name.c_str());
        lua_rawgeti(L, list, i);
        if (lua_pcall(L, 2, 0, 0) != 0) Logf(LogLevel::Warning, "ReceiveChat: %s", lua_tostring(L, -1));
      }
    }
    lua_settop(L, list);
  }
  lua_settop(L, top);
}

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
void UserState::CopyFromSimulation() {
  Sim* sim = LiveSim("the beat's copies");  // (between beats: no wait)
  if (!sim) return;
  EconomyTotals e = EconomyTotalsOf(*sim, sim->focusArmy + 1);
  std::memcpy(economyCopy, &e, sizeof e);
  // A new focus army (SetFocusArmy, applied by the engine at this beat's Sync): the selection is
  // emptied, as the original's CWldSession::DoBeat does (0x8945b9 .. 0x894663); GetFocusArmy,
  // IsObserver and what goes by the focus army read the copy from here on.
  const bool focusChanged = economyCopied && focusArmyCopy != sim->focusArmy;
  focusArmyCopy = sim->focusArmy;
  if (focusChanged) {
    Logf(LogLevel::Info, "moho64ui: the focus army is now %d (0: none)", focusArmyCopy + 1);
    SelectUnitsById(state_->L(), {}, false);
  }
  // (a sync whose beat ran takes its ticks off the clock's part of a tick; a paused one leaves it)
  const uint32_t ticks = economyCopied && sim->tick() > tickCopy ? sim->tick() - tickCopy : 0;
  lastSyncTicked = !economyCopied || ticks > 0;
  tickFraction = std::clamp(tickFraction - static_cast<double>(ticks), 0.0, 1.0);
  tickCopy = sim->tick();
  isReplayCopy = sim->isReplay;
  armiesCopy = ArmyViews(*sim);
  // (the deposits: the map's script makes them at the start, CreateResourceDeposit; a script may add more)
  if (depositsCopy.size() != sim->deposits.size()) {
    depositsCopy.clear();
    for (const ResourceDeposit& d : sim->deposits) depositsCopy.push_back({d.x0, d.z0, d.x1, d.z1, d.type});
  }
  if (const TerrainMap* map = sim->map()) hasWaterCopy = map->hasWater, waterElevationCopy = map->waterElevation;
  focusNickname.clear();
  for (const ArmyView& a : armiesCopy)
    if (a.index == sim->focusArmy + 1) focusNickname = a.nickname;
  economyCopied = true;
}

void UserState::AdvanceTickFraction(double seconds) {
  if (!economyCopied) return;
  if (!lastSyncTicked) {
    tickFraction = 1.0;
    return;
  }
  const GamePace* pace = config_.pace;
  const double rate = std::pow(10.0, (pace ? pace->rate : 0) / 10.0);  // (WLD_GetSimRate: the speed asked for)
  tickFraction = std::min(1.0, tickFraction + rate * seconds * 10.0);
}

void UserState::DeliverSync(const std::string& blob) {
  lua_State* L = state_->L();
  ++syncCount;
  RefreshUserUnits(L);
  CopyFromSimulation();
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
  // The selection after the sync (CWldSession::DoBeat: OnSync() at 0x8950a3, CheckForNecessaryUIRefresh at 0x89570a):
  // its dead units leave it, and a unit's request to make its panels again reaches the script (user_units.cpp).
  CheckForNecessaryUIRefresh(L);
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

void IconColorsForWorld(lua_State* L, std::vector<uint32_t>* out) {
  UserState* user = UserState::From(L);
  const int focus = user->economyCopied ? user->focusArmyCopy : -1;
  if (!user->teamColors || focus < 0 || user->armiesCopy.empty()) return;
  // The original's own colours (struct_TeamColors 0x85b2a0, from /lua/gameColors.lua's
  // GameColors.TeamColorMode), for an army FAF's list leaves out.
  uint32_t own[4] = {0xff4169e1u, 0xff006400u, 0xffe80a0au, 0xffdaa520u};  // Self, Ally, Enemy, Neutral
  int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/GameColors.lua");
  if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_pushstring(L, "GameColors");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "TeamColorMode");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        const char* keys[4] = {"Self", "Ally", "Enemy", "Neutral"};
        for (int k = 0; k < 4; ++k) {
          lua_pushstring(L, keys[k]);
          lua_gettable(L, -2);
          if (lua_isstring(L, -1)) own[k] = ParseColor(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
    }
  }
  lua_settop(L, top);
  const ArmyView* mine = nullptr;
  for (const ArmyView& a : user->armiesCopy)
    if (a.index == focus + 1) mine = &a;
  int count = 0;
  for (const ArmyView& a : user->armiesCopy) count = std::max(count, a.index);
  for (int army = 0; army < count; ++army) {
    if (static_cast<size_t>(army) < user->teamColorTable.size() && user->teamColorTable[static_cast<size_t>(army)] != 0) {
      out->push_back(user->teamColorTable[static_cast<size_t>(army)]);
      continue;
    }
    const int stance = army == focus ? 3 : mine && static_cast<size_t>(army) < mine->alliance.size() ? mine->alliance[static_cast<size_t>(army)] : 1;
    out->push_back(stance == 3 ? own[0] : stance == 2 ? own[1] : stance == 0 ? own[2] : own[3]);
  }
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
  SetGlobal(L, "SessionIsReplay", l_SessionIsReplay);
  for (const char* name : {"WorldIsLoading", "SessionIsMultiplayer",
                           "SessionIsBeingRecorded", "SessionIsObservingAllowed"})
    SetGlobal(L, name, l_False);
  SetGlobal(L, "IsObserver", l_IsObserver);
  SetGlobal(L, "TeamColorMode", l_TeamColorMode);
  SetGlobal(L, "SetFocusArmy", l_SetFocusArmy);
  // moho64ui_IconColors() -> the icons' colours by army as handed over now (empty: the armies'
  // own); the host's own function for tests.
  lua_register(L, "moho64ui_IconColors", [](lua_State* L) -> int {
    std::vector<uint32_t> colors;
    IconColorsForWorld(L, &colors);
    lua_newtable(L);
    for (size_t i = 0; i < colors.size(); ++i) {
      lua_pushnumber(L, static_cast<double>(colors[i]));
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
  });
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
  SetGlobal(L, "SessionSendChatMessage", l_SessionSendChatMessage);
  SetGlobal(L, "IsAlly", l_IsAlly);
  SetGlobal(L, "IsNeutral", l_IsNeutral);
  SetGlobal(L, "IsEnemy", l_IsEnemy);
}

}  // namespace moho::ui
