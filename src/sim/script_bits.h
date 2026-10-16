// Unit script bits and live toggle caps (sim/script_bits.cpp, engine-ref script_bits.md).
#pragma once
#include <cstdint>

#include "script/lua.hpp"

namespace moho {

class Sim;
class Unit;

// The unit's live toggle caps (u+0x48c), initialised from the blueprint on first use.
uint32_t UnitToggleCaps(Sim& sim, Unit* u);
// A General.ToggleCaps value at idx as RULEUTC_* bits (a number, the lexical names, or a { RULEUTC_x = true } table).
uint32_t ToggleCapsOfValue(lua_State* L, int idx);
// Unit::ToggleScriptBit 0x6a7490 (bit = bit number; the callback gets it as given).
void UnitToggleScriptBit(Sim& sim, Unit* u, long bit);
void RegisterScriptBitBindings(lua_State* L);

}  // namespace moho
