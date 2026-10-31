// Unit script bits (the toggle buttons: shield, weapon, jamming, intel, production, stealth, generic,
// special, cloak) and the live toggle caps (engine-ref script_bits.md).
//
// - The mask (u+0x498) changes only through Unit::ToggleScriptBit 0x6a7490: refused silently without the live
//   toggle cap, or while the unit rides a TRANSPORTATION parent; the bit is written first, then Lua
//   OnScriptBitClear(self, bit) / OnScriptBitSet(self, bit) runs.
// - Lua names are ERuleBPUnitToggleCaps values (masks): 'RULEUTC_ShieldToggle' = 1 = bit 0, ... A number is read
//   as the mask value too. The command stream's ProcessInfoPair "ToggleScriptBit" carries the bit NUMBER.
// - The live caps (u+0x48c) start as the blueprint's General.ToggleCaps; Add/Remove/RestoreToggleCaps change
//   them; TestToggleCaps reads the blueprint.
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/combat.h"
#include "sim/enum_arg.h"
#include "sim/script_bits.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

const char* const kToggleNames[] = {"RULEUTC_ShieldToggle", "RULEUTC_WeaponToggle", "RULEUTC_JammingToggle",
                                    "RULEUTC_IntelToggle", "RULEUTC_ProductionToggle", "RULEUTC_StealthToggle",
                                    "RULEUTC_GenericToggle", "RULEUTC_SpecialToggle", "RULEUTC_CloakToggle"};

bool ParseToggleCaps(const char* s, uint32_t* out);

// The blueprint's General.ToggleCaps as a mask. bp_reflect turns the table into the enum's lexical value: the
// NAME when exactly one cap is set ("RULEUTC_JammingToggle"), else the mask as a decimal string.
uint32_t ToggleCapsAt(lua_State* L, int idx) {
  if (idx < 0) idx = lua_gettop(L) + idx + 1;
  uint32_t caps = 0;
  if (lua_isnumber(L, idx)) caps = static_cast<uint32_t>(lua_tonumber(L, idx));
  else if (lua_type(L, idx) == LUA_TSTRING) ParseToggleCaps(lua_tostring(L, idx), &caps);
  else if (lua_istable(L, idx)) {  // unconverted { RULEUTC_x = true, ... }
    for (int i = 0; i < 9; ++i) {
      lua_pushstring(L, kToggleNames[i]);
      lua_gettable(L, idx);
      if (lua_toboolean(L, -1)) caps |= 1u << i;
      lua_pop(L, 1);
    }
  }
  return caps;
}

uint32_t BlueprintToggleCaps(Sim& sim, const BlueprintInfo& bp) {
  lua_State* L = sim.L();
  int top = lua_gettop(L);
  uint32_t caps = 0;
  sim.blueprints().PushTable(L, bp);
  lua_pushstring(L, "General");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "ToggleCaps");
    lua_gettable(L, -2);
    caps = ToggleCapsAt(L, -1);
  }
  lua_settop(L, top);
  return caps;
}

// ERuleBPUnitToggleCaps' lexical value (Reflection::SetLexical 0x8d9670, sim/enum_arg.h: no prefix, names in any
// case, numbers, '|'); false when a part is neither a name nor a number
bool ParseToggleCaps(const char* s, uint32_t* out) {
  int v = 0;
  if (!EnumLexical(kEnumToggleCaps, s, &v)) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// value -> bit (value must be 1..256; bit = floor(log2(value)))
bool ToggleBit(lua_State* L, int idx, int* bit) {
  const char* s = lua_tostring(L, idx);
  if (!s) luaL_typerror(L, idx, "string");
  uint32_t v = 0;
  if (!ParseToggleCaps(s, &v)) return false;
  if (v - 1 > 0xffu) return false;
  int b = 0;
  while (v != 1) {
    v >>= 1;
    ++b;
  }
  *bit = b;
  return true;
}

void CheckArgs(lua_State* L, int n, const char* what) {
  if (lua_gettop(L) != n) luaL_error(L, "%s\n  expected %d args, but got %d", what, n, lua_gettop(L));
}

int l_GetScriptBit(lua_State* L) {
  CheckArgs(L, 2, "Unit:GetScriptBit(bit)");
  Unit* u = CheckObject<Unit>(L, 1);
  int bit;
  if (!ToggleBit(L, 2, &bit)) return 0;
  lua_pushboolean(L, (u->scriptBits >> bit) & 1u);
  return 1;
}

int l_SetScriptBit(lua_State* L) {
  CheckArgs(L, 3, "Unit:SetScriptBit(bit, state)");
  Unit* u = CheckObject<Unit>(L, 1);
  if (!lua_isnil(L, 3) && !lua_isboolean(L, 3)) luaL_typerror(L, 3, "boolean");
  bool on = lua_toboolean(L, 3) != 0;
  int bit;
  if (!ToggleBit(L, 2, &bit)) return 0;
  if ((((u->scriptBits >> bit) & 1u) != 0) != on) UnitToggleScriptBit(*Sim::From(L), u, bit);
  return 0;
}

int l_ToggleScriptBit(lua_State* L) {
  CheckArgs(L, 2, "Unit:ToggleScriptBit(bit)");
  Unit* u = CheckObject<Unit>(L, 1);
  int bit;
  if (ToggleBit(L, 2, &bit)) UnitToggleScriptBit(*Sim::From(L), u, bit);
  return 0;
}

template <int Op>  // 0 add, 1 remove
int l_ChangeToggleCap(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  // (SCR_GetEnum 0x4d2550: 0x6c8cd3 / 0x6c8e73: a name it does not take raises "Invalid enum value")
  const uint32_t v = static_cast<uint32_t>(EnumValue(L, kEnumToggleCaps, luaL_checkstring(L, 2)));
  uint32_t caps = UnitToggleCaps(*Sim::From(L), u);
  u->toggleCaps = Op == 0 ? (caps | v) : (caps & ~v);
  return 0;
}

int l_RestoreToggleCaps(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->toggleCaps = BlueprintToggleCaps(*Sim::From(L), *u->blueprint);
  return 0;
}

int l_TestToggleCaps(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  // (SCR_GetEnum 0x4d2550: 0x6c9153)
  const uint32_t v = static_cast<uint32_t>(EnumValue(L, kEnumToggleCaps, luaL_checkstring(L, 2)));
  lua_pushboolean(L, (BlueprintToggleCaps(*Sim::From(L), *u->blueprint) & v) != 0);
  return 1;
}

}  // namespace

uint32_t ToggleCapsOfValue(lua_State* L, int idx) { return ToggleCapsAt(L, idx); }

uint32_t UnitToggleCaps(Sim& sim, Unit* u) {
  if (u->toggleCaps < 0) u->toggleCaps = BlueprintToggleCaps(sim, *u->blueprint);
  return static_cast<uint32_t>(u->toggleCaps);
}

void UnitToggleScriptBit(Sim& sim, Unit* u, long bit) {
  uint32_t m = 1u << (static_cast<unsigned long>(bit) & 31u);
  if (!(UnitToggleCaps(sim, u) & m)) return;
  if (u->unitStates.Has(US("Attached")) && u->parentId) {  // cargo of a transport cannot toggle
    Entity* p = sim.FindEntity(u->parentId);
    if (p && p->blueprint && BpInCategory(sim, p->blueprint, "TRANSPORTATION")) return;
  }
  bool clear = (u->scriptBits & m) != 0;
  if (clear) u->scriptBits &= ~m;
  else u->scriptBits |= m;
  static const bool dbgBits = getenv("MOHO64_DEBUG_BITS") != nullptr;  // (read once)
  if (dbgBits) {  // who toggles: tick, unit, bit, new value, the Lua callers
    lua_State* L = sim.L();
    std::string tb;
    lua_Debug ar;
    for (int lv = 0; lv < 8 && lua_getstack(L, lv, &ar); ++lv)
      if (lua_getinfo(L, "Sl", &ar)) tb += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
    Logf(LogLevel::Info, "bitdbg %u unit %u %s bit %ld -> %d%s", sim.tick(), u->id,
         u->blueprint ? u->blueprint->id.c_str() : "?", bit, clear ? 0 : 1, tb.c_str());
  }
  if (!u->HasLuaObject()) return;
  lua_pushnumber(sim.L(), static_cast<float>(bit));
  sim.CallMethod(sim.L(), u, clear ? "OnScriptBitClear" : "OnScriptBitSet", 1);
}

void RegisterScriptBitBindings(lua_State* L) {
  SetMethod(L, "Unit", "GetScriptBit", l_GetScriptBit);
  SetMethod(L, "Unit", "SetScriptBit", l_SetScriptBit);
  SetMethod(L, "Unit", "ToggleScriptBit", l_ToggleScriptBit);
  SetMethod(L, "Unit", "AddToggleCap", l_ChangeToggleCap<0>);
  SetMethod(L, "Unit", "RemoveToggleCap", l_ChangeToggleCap<1>);
  SetMethod(L, "Unit", "RestoreToggleCaps", l_RestoreToggleCaps);
  SetMethod(L, "Unit", "TestToggleCaps", l_TestToggleCaps);
}

}  // namespace moho
