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
  int focusArmy = -1;   // 0-based index into header.armies, -1 = none (launch info +0x68)
  // The GameSpeed option as the client manager gets it (0x53faf0): 'fast' -> 4, otherwise 0; 'adjustable' ->
  // the players may change the speed. The simulation does not use either.
  int gameSpeed = 0;
  bool adjustableSpeed = false;
};

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
