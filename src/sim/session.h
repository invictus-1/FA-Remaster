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
  // Sim::fixes (docs/deviations.md) for this session: -1 = by `replay` (off for a replay, on for a new game; a
  // replay whose scenario carries the mark of a game played with the fixes, Options.RemasterFixes = "on", gets
  // them too: Sim::Start reads it, replay.h ScenarioMarksFixes); 0 / 1 = as given. MOHO64_FIXES still wins.
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

// The game's preferences file as the rules read it. FAF's Blueprints.lua (LoadPreGameData, line 126) runs the
// first /preferences/Game.prefs and takes its PreGameData; the rules then load every *.bp under
// PreGameData.CurrentMapDir, "Blueprints from current map" (line 1233), before the mods' own. In the original that
// file is the program's own preferences file, whose PreGameData the lobby set for the game being launched
// (lobby.lua 2449: CurrentMapDir = Dirname(GameOptions.ScenarioFile)) and which DoPreload (0x88bee0 ->
// USER_SavePreferences 0x8c91a0) saved before the rules load. The lab's init file mounts the original's
// preferences folder at /preferences, whose Game.prefs names the map of the last game played in FAF: an outside
// input to the blueprint set. So the hosts mount a Game.prefs of their own there first, held in memory
// (Vfs::MountMemoryFile before the init file runs, Vfs::SetMemoryFile once the session is known), with the
// session's map folder (ui/REQUESTS.md 40). The folder's other files stay visible.
inline constexpr char kGamePrefsPath[] = "/preferences/Game.prefs";

// The folder of a scenario file or a map file ("/maps/x/x_scenario.lua" -> "/maps/x"): what precedes the last '/'
// or '\', as the engine's Dirname gives it to the lobby; empty when there is none.
std::string MapFolderOf(const std::string& scenarioOrMapPath);

// The session's map folder as the lobby writes it into PreGameData: the folder of the scenario info's
// Options.ScenarioFile (header.scenario: a lobby's game, a replay of one, a campaign launch all carry it); without
// that option the folder of the map file (header.map, beside which every scenario file lies). For a replay this
// is the replay's own scenario.
std::string SessionMapFolder(const ReplayHeader& header);

// The text of a Game.prefs that holds only PreGameData with that CurrentMapDir, in the form the original's
// preferences writer gives it (Lua 5.0 GPG dialect):
//   PreGameData = {
//       CurrentMapDir = '/maps/x'
//   }
// With an empty mapFolder the table is empty: no map's own blueprints load.
std::string PreGameDataPrefs(const std::string& mapFolder);

}  // namespace moho
