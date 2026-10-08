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
    uint32_t id;
  };
  std::vector<Source> sources;
  bool cheats = false;
  struct Army {
    std::string options;  // serialized Lua table: the army's player options
    int source = 255;     // command source, 255 = none
  };
  std::vector<Army> armies;
  uint32_t seed = 0;
  size_t streamOffset = 0;
};

std::optional<ReplayHeader> ReadReplayHeader(const std::string& data, std::string* error);

// Push the Lua value serialized in `blob` (the engine's format: 0 float, 1 string, 2 nil,
// 3 bool, 4 table start ... 5 table end). Returns false if the blob is malformed.
bool PushSerializedLua(lua_State* L, const std::string& blob);

}  // namespace moho
