// Replay files (.SCFAReplay): the header holds everything needed to start the same session
// (map, mods, scenario info with lobby options, the armies' player options, the random seed);
// the command stream follows.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {

struct ReplayHeader {
  std::string version;  // "Supreme Commander v1.50.3839"
  std::string replayVersion;
  std::string map;      // "/maps/SCMP_009/SCMP_009.scmap"
  std::string mods;     // serialized Lua values (see PushSerializedLua)
  std::string scenario;
  struct Source {
    std::string name;
    int32_t pausesAllowed = -1;  // source+0x20: pauses it may request (-1 = unlimited) (command_stream.md 2)
  };
  std::vector<Source> sources;
  bool cheats = false;
  struct Army {
    std::string options;  // serialized Lua table: the army's player options
    std::vector<int> sources;  // the command sources allowed to command it (army+0x130); u8 list ending in 0xFF
    int source = 255;     // the first of them, 255 = none
  };
  std::vector<Army> armies;
  uint32_t seed = 0;
  size_t streamOffset = 0;
};

std::optional<ReplayHeader> ReadReplayHeader(const std::string& data, std::string* error);

// Push the Lua value serialized in `blob` (the engine's format: 0 float, 1 string, 2 nil,
// 3 bool, 4 table start ... 5 table end). Returns false if the blob is malformed.
bool PushSerializedLua(lua_State* L, const std::string& blob);

// The mark a game played with this engine's script fixes (Sim::fixes, docs/deviations.md) leaves in its replay:
// Options.RemasterFixes = "on" in the scenario info of the header (the interface host writes it,
// GameSession::StartReplayFile; the original ignores option keys it does not know). True when `scenario` (as
// the header holds it, serialized) carries it.
bool ScenarioMarksFixes(const std::string& scenario);

// The simulation order a game was launched with (docs/parallel-mode.md 5), in the scenario info of the header (the
// host's launch writes it, the same on every client; the replay keeps it): Options.RemasterSimOrder = "parallel" or
// "original", and Options.RemasterSimOrderVersion, an integer. 1 for "parallel"; 0 for "original", a missing field
// or anything else (every recording of the original, every FAF replay). *version: the version, 0 when missing.
int ScenarioSimOrder(const std::string& scenario, int* version);

// The FAF game version a session plays by (engine-ref/specs/faf3840.md): the number at the end of the header's
// version line, "Supreme Commander v1.50.3839" -> 3839 (VCR_CreateReplay 0x876190 writes the exe's own, 0x87612c;
// FAF 3840's exe is the same build with 0xf00 there). This engine knows the rules of two: 3839 (everything was read
// from it) and 3840 (a hotfix of the exe only: repairs of shields, aircraft leaving a platform, shells under the map,
// the replay client's command sources). Sim::gameVersion keeps the one a session plays by.
constexpr int kGameVersion3839 = 3839, kGameVersion3840 = 3840;
// The version a NEW game is made with, written into its header (SessionFromGameConfig / SessionFromSinglePlayerInfo)
// so that its replay plays by the same rules. The single switch for new games: 3840 (Chris, 2026-11-04: its changes
// are all repairs). MOHO64_GAME_VERSION=3839 | 3840 overrides it (and, in Sim::Start, the version of any session, a
// replay's too: for tests).
constexpr int kNewGameVersion = kGameVersion3840;
// The number at the end of a header's version line; 0 when it has none.
int HeaderGameVersion(const std::string& versionLine);
// The version line of a header made by this engine for a game of `version`.
std::string GameVersionLine(int version);
// kNewGameVersion, or MOHO64_GAME_VERSION when it names a version.
int NewGameVersion();

}  // namespace moho
