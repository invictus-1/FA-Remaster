// A session started from a lobby's game configuration instead of a replay: what the original builds in
// CLobby::LaunchGame (FA exe 0x7c38c0) from the table the lobby script hands it. The result is the same set of
// things a replay's header holds (map, mods, scenario info, command sources, the armies' options, the seed), so
// Sim::Start takes it the same way.
#pragma once
#include <cstdint>
#include <string>

#include "script/lua.hpp"
#include "sim/replay.h"

namespace moho {

// What the lobby object itself knows at launch (not part of the script's table).
struct LaunchParams {
  // The session's random seed. The original's host takes it from its timer when it starts hosting
  // (CLobby::HostGame 0x7c1c54 -> lobby+0xc4) and LaunchGame copies it into the launch info (+0xa0).
  uint32_t seed = 0;
  // The lobby's own peer id (lobby+0xac): the human army whose OwnerID is this one becomes the focus army.
  // -1 = take the first human army's OwnerID (a local game has one player).
  int localOwner = -1;
  // A connector with peers (lobby+0x78 vtable+8 != 0): only then the Timeouts option limits each player's
  // pauses and observers get none; a local game leaves both unlimited (-1).
  bool network = false;
};

struct SessionDescription {
  ReplayHeader header;  // streamOffset stays 0: there is no command stream
  bool replay = false;  // true when the header was read from a replay file (the simulation's SessionIsReplay)
  // Sim::fixes (docs/deviations.md) for this session: -1 = by `replay` (off for a replay, on for a new game);
  // 0 / 1 = as given. A replay of a game played with this engine sets 1 when the game was played with the fixes
  // (the interface host marks such games in the scenario's Options: RemasterFixes = "on"). MOHO64_FIXES still wins.
  int fixes = -1;
  int focusArmy = -1;   // 0-based index into header.armies, -1 = none (launch info +0x68)
  // The GameSpeed option as the client manager gets it (0x53faf0): 'fast' -> 4, otherwise 0; 'adjustable' ->
  // the players may change the speed. The simulation does not use either.
  int gameSpeed = 0;
  bool adjustableSpeed = false;
  // The single-player table's createReplay (the session info +0x24, WLD_SetupSessionInfo 0x88cfc6): whether the
  // host records the game. A lobby's game is always recorded. The simulation does not use it.
  bool createReplay = true;
};

// What LaunchSinglePlayerSession (FA exe 0x88d3c0) knows besides the script's table.
struct SinglePlayerParams {
  // The seed when the table has no RandomSeed: the original then takes the low 32 bits of the system timer's
  // cycle count (WLD_SetupSessionInfo 0x88ce62).
  uint32_t seed = 0;
  // Cheats: the preference debug.enable_debug_facilities (USER_DebugFacilitiesEnabled 0x8cae20 -> launch info
  // +0x88, 0x88cc36), not a game option.
  bool cheats = false;
};

// Build the session from the single-player launch table at stack index `info`, the way LaunchSinglePlayerSession
// does (WLD_SetupSessionInfo 0x88cbc0 and 0x88bbb0): what FAF's SinglePlayerLaunch.lua makes (playerName,
// createReplay, scenarioInfo, teamInfo, scenarioMods, RandomSeed). Unlike a lobby's launch nothing is added or
// looked up: the scenario info is scenarioInfo as given (with its Options, campaignInfo, tutorial, file), the mods
// are scenarioMods, the armies are teamInfo's tables in table order. One command source (playerName, pauses not
// limited) commands every army; the focus army is the first. On failure returns false and sets *error.
bool SessionFromSinglePlayerInfo(lua_State* L, int info, const SinglePlayerParams& params, SessionDescription* out,
                                 std::string* error);

// Build the session from the game configuration table at stack index `config` (GameOptions, PlayerOptions,
// Observers, GameMods). The scenario file is run in `L` (it needs the core functions, `doscript` above all) and
// PlayerOptions' tables get their ArmyName, as in the original. On failure returns false and sets *error to the
// reason the original would report through the script's LaunchFailed ("NoConfig", "StartSpots"), or a description.
bool SessionFromGameConfig(lua_State* L, int config, const LaunchParams& params, SessionDescription* out,
                           std::string* error);

// Run a file that holds `GameConfig = { ... }` (what the interface host records at Launch) and push the table.
// The script's `repr` output puts "-- table: <address> (<n> bytes)" after every opening brace, which swallows the
// closing brace of an empty table: those comments are removed before the file is compiled.
bool PushGameConfigFile(lua_State* L, const std::string& hostPath, std::string* error);

// The engine's byte form of a Lua value (the inverse of PushSerializedLua): 0 float, 1 string, 2 nil, 3 bool,
// 4 table ... 5. Values of other types are written as nil; a table entry with such a key or value is left out.
void SerializeLua(lua_State* L, int idx, std::string* out);

}  // namespace moho
