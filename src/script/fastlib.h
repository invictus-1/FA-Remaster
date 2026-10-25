// The engine's own generic Lua helpers (not the original's: FA Remastered only), in one global table `Remastered`.
// Made for the sim state of a session with the engine's script fixes (Sim::fixes), never while the original's replays
// play back (fixes off), so that the globals the original's scripts see stay the original's. Scripts look for them
// with rawget(_G, 'Remastered') and keep their own Lua code for any engine without them.
#pragma once
#include <cmath>

#include "script/lua.hpp"

namespace moho {

// VDist2's arithmetic (script/core_bindings.cpp l_VDist2), shared with Remastered.VDist2Pos so the two cannot drift.
inline lua_Number VDist2Of(lua_Number x1, lua_Number y1, lua_Number x2, lua_Number y2) {
  float dx = x1 - x2;
  float dy = y1 - y2;
  return std::sqrt(dx * dx + dy * dy);
}

// Sets the global `Remastered` (MOHO64_NO_REMASTERED=1: not made, for tests of a script's fallback path).
void RegisterRemasteredLib(lua_State* L);

}  // namespace moho
