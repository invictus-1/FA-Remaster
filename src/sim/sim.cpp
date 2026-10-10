#include "sim/entity_grid.h"
#include "sim/prop_motor.h"
#include "sim/landnav.h"
#include "sim/sim.h"
#include "sim/formation.h"
#include "sim/transport.h"
#include "sim/air.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/intel.h"
#include "sim/build.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "script/threads.h"
#include "sim/commands.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

const char kSimKey = 0;

Sim* S(lua_State* L) { return Sim::From(L); }

// ---- globals -------------------------------------------------------------------------------

int l_ListArmies(lua_State* L) {
  lua_newtable(L);
  int i = 0;
  for (const auto& a : S(L)->armies()) {
    lua_pushstring(L, a->name.c_str());
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}

int l_GetArmyBrain(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army %s", lua_tostring(L, 1) ? lua_tostring(L, 1) : "?");
  PushObject(L, a->brain);
  return 1;
}

int l_GetFocusArmy(lua_State* L) {
  lua_pushnumber(L, S(L)->focusArmy < 0 ? -1 : S(L)->focusArmy + 1);
  return 1;
}

int l_ArmyIsCivilian(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  lua_pushboolean(L, a && a->civilian);
  return 1;
}

// The army's damage handicap (army +0x1e0 when its flag +0x1dc is set; 0 otherwise). Nothing in
// the lobby sets one, so it is 0 (sim/damage.cpp applies none).
int l_ArmyGetHandicap(lua_State* L) {
  S(L)->GetArmy(L, 1);
  lua_pushnumber(L, 0);
  return 1;
}

int l_ArmyIsOutOfGame(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  lua_pushboolean(L, a && a->outOfGame);
  return 1;
}

int l_SetArmyOutOfGame(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1)) a->outOfGame = true;
  return 0;
}

int l_GetGameTick(lua_State* L) {
  lua_pushnumber(L, S(L)->tick());
  return 1;
}

int l_GetGameTimeSeconds(lua_State* L) {
  lua_pushnumber(L, S(L)->tick() * 0.1f);
  return 1;
}

int l_GetSystemTimeSecondsOnlyForProfileUse(lua_State* L) {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  lua_pushnumber(L, duration<float>(steady_clock::now() - t0).count());
  return 1;
}

int l_GetMapSize(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->width() : 0);
  lua_pushnumber(L, m ? m->height() : 0);
  return 2;
}

int l_GetTerrainHeight(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->TerrainHeight(luaL_checknumber(L, 1), luaL_checknumber(L, 2)) : 0);
  return 1;
}

// Terrain types: /lua/TerrainTypes.lua's TerrainTypes list, by TypeCode (loaded on first use).
const char kTerrainKey = 0;
void PushTerrainTypes(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kTerrainKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);  // code -> entry
  int byCode = lua_gettop(L);
  lua_newtable(L);  // env with access to globals
  int env = lua_gettop(L);
  lua_newtable(L);
  lua_pushstring(L, "__index");
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_rawset(L, -3);
  lua_setmetatable(L, env);
  lua_getglobal(L, "doscript");
  lua_pushstring(L, "/lua/TerrainTypes.lua");
  lua_pushvalue(L, env);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    LogScriptError(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pushstring(L, "TerrainTypes");
  lua_rawget(L, env);
  if (lua_istable(L, -1))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      lua_pushstring(L, "TypeCode");
      lua_rawget(L, -2);
      int code = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : -1;
      lua_pop(L, 1);
      lua_pushstring(L, "Name");
      lua_rawget(L, -2);
      bool isDefault = lua_isstring(L, -1) && !std::strcmp(lua_tostring(L, -1), "Default");
      lua_pop(L, 1);
      if (isDefault) {
        lua_pushstring(L, "Default");
        lua_pushvalue(L, -2);
        lua_rawset(L, byCode);
      }
      if (code >= 0) lua_rawseti(L, byCode, code);
      else lua_pop(L, 1);
    }
  lua_settop(L, byCode);
  lua_pushlightuserdata(L, const_cast<char*>(&kTerrainKey));
  lua_pushvalue(L, byCode);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

// Push the terrain type entry at map cell (x, z); (-1, -1) and unknown codes give 'Default'.
void PushTerrainTypeAt(lua_State* L, float x, float z) {
  PushTerrainTypes(L);
  const TerrainMap* m = S(L)->map();
  int code = -1;
  if (m && x >= 0 && z >= 0) code = m->TerrainType(static_cast<int>(x), static_cast<int>(z));
  lua_rawgeti(L, -1, code);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushstring(L, "Default");
    lua_rawget(L, -2);
  }
  lua_remove(L, -2);
}

int l_GetTerrainType(lua_State* L) {
  PushTerrainTypeAt(L, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
  return 1;
}

int l_GetTerrainTypeOffset(lua_State* L) {
  PushTerrainTypeAt(L, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
  float off = 0;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "HeightOffset");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1)) off = static_cast<float>(lua_tonumber(L, -1));
  }
  lua_pushnumber(L, off);
  return 1;
}

int l_GetSurfaceHeight(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->SurfaceHeight(luaL_checknumber(L, 1), luaL_checknumber(L, 2)) : 0);
  return 1;
}

int l_CheatsEnabled(lua_State* L) {
  lua_pushboolean(L, S(L)->cheats);
  return 1;
}

int l_IsGameOver(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

// Random() -> [0,1]; Random(max) -> 1..max; Random(min, max) -> min..max (integers; 0x759010, sim_random.md 3)
int32_t RandomArg(lua_State* L, int i) {
  if (lua_type(L, i) != LUA_TNUMBER) luaL_typerror(L, i, "number");
  float f = static_cast<float>(lua_tonumber(L, i));
  if (!(f > -2147483904.0f && f < 2147483648.0f)) return INT32_MIN;  // MSVC cvttss2si: NaN / out of range
  return static_cast<int32_t>(f);
}
int l_Random(lua_State* L) {
  Sim* sim = S(L);
  int n = lua_gettop(L);
  if (n > 2) return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", "Random", 0, 2, n);
  if (sim->rngTrace_) {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar))
      Logf(LogLevel::Info, "rnglua %u %s:%d n%d %g %g", sim->tick(), ar.short_src, ar.currentline, n,
           n >= 1 ? lua_tonumber(L, 1) : 0.0, n >= 2 ? lua_tonumber(L, 2) : 0.0);
    if (getenv("MOHO64_DEBUG_RNGTB")) {
      std::string tb;
      for (int lv = 1; lv < 12 && lua_getstack(L, lv, &ar); ++lv)
        if (lua_getinfo(L, "Sl", &ar)) tb += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
      Logf(LogLevel::Info, "rngtb %u%s", sim->tick(), tb.c_str());
    }
  }
  if (n == 0) {
    lua_pushnumber(L, sim->U01());
  } else if (n == 1) {
    uint32_t hi = static_cast<uint32_t>(RandomArg(L, 1));
    uint32_t u = sim->NextUInt32();
    lua_pushnumber(L, static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>((static_cast<uint64_t>(u) * hi) >> 32) + 1u)));
  } else {
    uint32_t hp1 = static_cast<uint32_t>(RandomArg(L, 2)) + 1u;
    uint32_t lo = static_cast<uint32_t>(RandomArg(L, 1));
    uint32_t u = sim->NextUInt32();
    lua_pushnumber(L, static_cast<float>(static_cast<int32_t>(
                          static_cast<uint32_t>((static_cast<uint64_t>(u) * (hp1 - lo)) >> 32) + lo)));
  }
  return 1;
}

// Alliances: 0 enemy, 1 neutral, 2 ally.
int Relation(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  if (!a || !b) return luaL_error(L, "Invalid army");
  if (a == b) return 2;
  return a->alliance[b->index - 1];
}
int l_IsAlly(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 2);
  return 1;
}
int l_IsEnemy(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 0);
  return 1;
}
int l_IsNeutral(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 1);
  return 1;
}
int ParseRelation(lua_State* L, int idx) {
  const char* s = luaL_checkstring(L, idx);
  if (!std::strcmp(s, "Ally")) return 2;
  if (!std::strcmp(s, "Neutral")) return 1;
  if (!std::strcmp(s, "Enemy")) return 0;
  return luaL_error(L, "Unknown alliance type %s", s);
}
int l_SetAlliance(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  int r = ParseRelation(L, 3);
  if (!a || !b) return luaL_error(L, "Invalid army");
  a->alliance[b->index - 1] = r;
  b->alliance[a->index - 1] = r;
  return 0;
}
int l_SetAllianceOneWay(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  int r = ParseRelation(L, 3);
  if (!a || !b) return luaL_error(L, "Invalid army");
  a->alliance[b->index - 1] = r;
  return 0;
}

int l_SetArmyPlans(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1)) a->plans = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}

// InitializeArmyAI(army): the brain's own set-up (CAiBrain::Initialize in the original) -
// brain:OnCreateHuman(planName) or brain:OnCreateAI(planName); planName "None" if unset.
// FAF's OnCreateArmyBrain calls this after choosing the brain's class.
int l_InitializeArmyAI(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushstring(L, a->plans.empty() ? "None" : a->plans.c_str());
  S(L)->CallMethod(L, a->brain, a->human ? "OnCreateHuman" : "OnCreateAI", 1);
  return 0;
}

int l_GetArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushnumber(L, a->unitCap);
  return 1;
}
int l_GetArmyUnitCostTotal(lua_State* L) {  // the army's units counted against the cap (General.CapCost)
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushnumber(L, a->unitCost);
  return 1;
}
int l_SetArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->unitCap = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_SetIgnoreArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->ignoreUnitCap = lua_toboolean(L, 2);
  return 0;
}

int l_ShouldCreateInitialArmyUnits(lua_State* L) {
  lua_pushboolean(L, 1);  // a new session (not a loaded save)
  return 1;
}

// ---- CAiBrain --------------------------------------------------------------------------------

AiBrain* Brain(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

int l_brain_GetArmyIndex(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->index);
  return 1;
}
int l_brain_GetFactionIndex(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->faction + 1);
  return 1;
}
int l_brain_IsDefeated(lua_State* L) {
  lua_pushboolean(L, Brain(L)->army->outOfGame);
  return 1;
}

// GetArmyStat(name, default) -> { Value = ... }
int l_brain_GetArmyStat(lua_State* L) {
  Army* a = Brain(L)->army;
  std::string name = luaL_checkstring(L, 2);
  auto it = a->stats.find(name);
  float v = it != a->stats.end() ? it->second : static_cast<float>(luaL_optnumber(L, 3, 0));
  lua_newtable(L);
  lua_pushstring(L, "Value");
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
  return 1;
}
int l_brain_SetArmyStat(lua_State* L) {
  Brain(L)->army->stats[luaL_checkstring(L, 2)] = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
int l_brain_AddArmyStat(lua_State* L) {
  Brain(L)->army->stats[luaL_checkstring(L, 2)] += static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
bool IEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
// ETriggerOperator enum parse (0x8d9670): case-insensitive, optional TRIGGER_ prefix, decimal, a|b
int ParseTriggerOp(lua_State* L, const char* s) {
  static const char* names[4] = {"GreaterThan", "GreaterThanOrEqual", "LessThan", "LessThanOrEqual"};
  int v = 0;
  std::string all = s ? s : "";
  size_t pos = 0;
  bool any = false;
  while (pos <= all.size()) {
    size_t bar = all.find('|', pos);
    std::string t = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
    if (t.size() >= 8 && IEquals(t.substr(0, 8), "TRIGGER_")) t = t.substr(8);
    int k = -1;
    for (int i = 0; i < 4; ++i)
      if (IEquals(t, names[i])) k = i;
    if (k < 0 && !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
      k = std::atoi(t.c_str());
    if (k < 0)
      luaL_error(L, "Invalid enum value %s\nValid Options are:\n   GreaterThan\n   GreaterThanOrEqual\n   LessThan\n   LessThanOrEqual", s);
    v |= k;
    any = true;
    if (bar == std::string::npos) break;
    pos = bar + 1;
  }
  return any ? v : 0;
}
// SetArmyStatsTrigger(statName, triggerName, compareType, value [, category]) 0x587c00
int l_brain_SetArmyStatsTrigger(lua_State* L) {
  int n = lua_gettop(L);
  if (n < 5 || n > 6) return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", "SetArmyStatsTrigger", 5, 6, n);
  Army* a = Brain(L)->army;
  std::string stat = luaL_checkstring(L, 2), name = luaL_checkstring(L, 3);
  int op = ParseTriggerOp(L, luaL_checkstring(L, 4));
  if (lua_type(L, 5) != LUA_TNUMBER) luaL_typerror(L, 5, "number");
  float thr = static_cast<float>(lua_tonumber(L, 5));
  bool cat = n > 5 && !lua_isnil(L, 6);
  std::shared_ptr<Army::StatTrigger> t;
  for (auto& x : a->statTriggers)
    if (IEquals(x->name, name)) {
      t = x;
      break;
    }
  if (!t) {
    t = std::make_shared<Army::StatTrigger>();
    t->name = name;
    a->statTriggers.push_back(t);
  }
  if (!a->stats.count(stat)) {
    Logf(LogLevel::Warning, "ArmyStatItem %s does not exist.", stat.c_str());
    return 0;
  }
  t->conds.push_back({stat, op, thr, cat});
  return 0;
}
// RemoveArmyStatsTrigger(statName, triggerName) 0x588020: the first trigger of that name (the stat is ignored)
int l_brain_RemoveArmyStatsTrigger(lua_State* L) {
  if (lua_gettop(L) != 3) return luaL_error(L, "%s\n  expected %d args, but got %d", "RemoveArmyStatsTrigger", 3, lua_gettop(L));
  Army* a = Brain(L)->army;
  luaL_checkstring(L, 2);
  std::string name = luaL_checkstring(L, 3);
  for (auto it = a->statTriggers.begin(); it != a->statTriggers.end(); ++it)
    if (IEquals((*it)->name, name)) {
      a->statTriggers.erase(it);
      break;
    }
  return 0;
}
int l_brain_GetBlueprintStat(lua_State* L) {
  Brain(L);
  lua_pushnumber(L, 0);  // TODO(M4): per-blueprint army stats
  return 1;
}

}  // namespace

void RegisterSimBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "GetArmyStat", l_brain_GetArmyStat);
  SetMethod(L, "CAiBrain", "SetArmyStat", l_brain_SetArmyStat);
  SetMethod(L, "CAiBrain", "AddArmyStat", l_brain_AddArmyStat);
  SetMethod(L, "CAiBrain", "GetBlueprintStat", l_brain_GetBlueprintStat);
  SetMethod(L, "CAiBrain", "SetArmyStatsTrigger", l_brain_SetArmyStatsTrigger);
  SetMethod(L, "CAiBrain", "RemoveArmyStatsTrigger", l_brain_RemoveArmyStatsTrigger);
  SetGlobal(L, "ListArmies", l_ListArmies);
  SetGlobal(L, "GetArmyBrain", l_GetArmyBrain);
  SetGlobal(L, "GetFocusArmy", l_GetFocusArmy);
  SetGlobal(L, "ArmyIsCivilian", l_ArmyIsCivilian);
  SetGlobal(L, "ArmyGetHandicap", l_ArmyGetHandicap);
  SetGlobal(L, "ArmyIsOutOfGame", l_ArmyIsOutOfGame);
  SetGlobal(L, "SetArmyOutOfGame", l_SetArmyOutOfGame);
  SetGlobal(L, "GetGameTick", l_GetGameTick);
  SetGlobal(L, "GetGameTimeSeconds", l_GetGameTimeSeconds);
  SetGlobal(L, "GetSystemTimeSecondsOnlyForProfileUse", l_GetSystemTimeSecondsOnlyForProfileUse);
  SetGlobal(L, "GetMapSize", l_GetMapSize);
  SetGlobal(L, "GetTerrainHeight", l_GetTerrainHeight);
  SetGlobal(L, "GetSurfaceHeight", l_GetSurfaceHeight);
  SetGlobal(L, "GetTerrainType", l_GetTerrainType);
  SetGlobal(L, "GetTerrainTypeOffset", l_GetTerrainTypeOffset);
  SetGlobal(L, "CheatsEnabled", l_CheatsEnabled);
  SetGlobal(L, "IsGameOver", l_IsGameOver);
  SetGlobal(L, "Random", l_Random);
  SetGlobal(L, "IsAlly", l_IsAlly);
  SetGlobal(L, "IsEnemy", l_IsEnemy);
  SetGlobal(L, "IsNeutral", l_IsNeutral);
  SetGlobal(L, "SetAlliance", l_SetAlliance);
  SetGlobal(L, "SetAllianceOneWay", l_SetAllianceOneWay);
  SetGlobal(L, "SetArmyPlans", l_SetArmyPlans);
  SetGlobal(L, "ShouldCreateInitialArmyUnits", l_ShouldCreateInitialArmyUnits);
  SetGlobal(L, "InitializeArmyAI", l_InitializeArmyAI);
  SetGlobal(L, "GetArmyUnitCap", l_GetArmyUnitCap);
  SetGlobal(L, "GetArmyUnitCostTotal", l_GetArmyUnitCostTotal);
  SetGlobal(L, "SetArmyUnitCap", l_SetArmyUnitCap);
  SetGlobal(L, "SetIgnoreArmyUnitCap", l_SetIgnoreArmyUnitCap);
  SetMethod(L, "CAiBrain", "GetArmyIndex", l_brain_GetArmyIndex);
  SetMethod(L, "CAiBrain", "GetFactionIndex", l_brain_GetFactionIndex);
  SetMethod(L, "CAiBrain", "IsDefeated", l_brain_IsDefeated);
}

// ---- Sim -----------------------------------------------------------------------------------

Sim::Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids)
    : vfs_(vfs), hookDirs_(std::move(hookDirs)), modUids_(std::move(modUids)),
      skeletons_(std::make_unique<SkeletonCache>(vfs)) {}

Sim::~Sim() {
  owned_.clear();  // unbind every engine object while the Lua state still exists
  ReleaseEffectObjects();
  ReleaseAnimators();
  armies_.clear();
  threads_.reset();
  state_.reset();
}

lua_State* Sim::L() const { return state_ ? state_->L() : nullptr; }

Sim* Sim::From(lua_State* L) { return static_cast<Sim*>(lua_getextra(L, 1)); }

#if defined(__linux__)
#include <execinfo.h>
extern "C" char __executable_start;
#endif
// MOHO64_DEBUG_RNG: one line per draw (tick, value, caller address relative to the image; addr2line on Linux)
__attribute__((noinline)) void Sim::RngTrace(uint32_t r) {
#if defined(__linux__)
  const char* a = static_cast<const char*>(__builtin_return_address(0));
  Logf(LogLevel::Info, "rngdbg %u %08x %lx", tick_, r, static_cast<unsigned long>(a - &__executable_start));
  static const char* bt = getenv("MOHO64_DEBUG_RNGBT");
  if (bt && tick_ == static_cast<uint32_t>(atoi(bt))) {
    void* fr[12];
    int n = backtrace(fr, 12);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %lx", static_cast<unsigned long>(static_cast<char*>(fr[i]) - &__executable_start));
      s += b;
    }
    Logf(LogLevel::Info, "rngbt%s", s.c_str());
  }
#else
  Logf(LogLevel::Info, "rngdbg %u %08x %p", tick_, r, __builtin_return_address(0));
#endif
}

float Sim::U01() { return static_cast<float>(static_cast<double>(NextUInt32()) * 0x1p-32); }
double Sim::FRand(float lo, float hi) {
  uint32_t u = NextUInt32();
  return (static_cast<double>(u) * (static_cast<double>(hi) - static_cast<double>(lo))) * 0x1p-32 + static_cast<double>(lo);
}
float Sim::BpUniform(float base, float range) {
  float lo = -0.0f - range;
  uint32_t u = NextUInt32();
  return static_cast<float>((static_cast<double>(u) * (static_cast<double>(range) - static_cast<double>(lo))) * 0x1p-32 +
                            static_cast<double>(lo) + static_cast<double>(base));
}
double Sim::Gauss() {
  if (hasGauss_) {
    hasGauss_ = false;
    return gauss_;
  }
  float x, y, q;
  do {
    x = static_cast<float>(static_cast<double>(NextUInt32()) * 0x1p-31 - 1.0);
    y = static_cast<float>(static_cast<double>(NextUInt32()) * 0x1p-31 - 1.0);
    q = y * y + x * x;
  } while (!(q < 1.0f));
  float t = static_cast<float>((-2.0 * std::log(static_cast<double>(q))) / static_cast<double>(q));
  double f = std::sqrt(static_cast<double>(t));
  gauss_ = static_cast<float>(static_cast<double>(y) * f);
  hasGauss_ = true;
  return static_cast<double>(x) * f;
}

Army* Sim::GetArmy(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TNUMBER) {
    int i = static_cast<int>(lua_tonumber(L, idx));
    return i >= 1 && i <= static_cast<int>(armies_.size()) ? armies_[i - 1].get() : nullptr;
  }
  if (lua_isstring(L, idx)) {
    const char* n = lua_tostring(L, idx);
    for (auto& a : armies_)
      if (a->name == n) return a.get();
  }
  return nullptr;
}

bool Sim::LoadRules() {
  rules_ = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
  rules_->SetHookDirs(hookDirs_);
  SimBlueprints::StartRecording(*rules_);
  SetModsGlobal(*rules_);
  return rules_->DoScript("/lua/ruleinit.lua");
}

bool Sim::RunString(const std::string& code, const std::string& chunkName) {
  return state_->DoString(code, chunkName);
}

void Sim::SetModsGlobal(ScriptState& st) {
  lua_State* L = st.L();
  int top = lua_gettop(L);
  if (sessionMods.empty() || !PushSerializedLua(L, sessionMods) || !lua_istable(L, -1)) {
    lua_settop(L, top);
    SetActiveMods(st, modUids_);
    return;
  }
  // The mounted mods (hook folders) come from the uid list: say so if it differs from the session.
  std::vector<std::string> uids;
  for (int i = 1;; ++i) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_pushstring(L, "uid");
    lua_rawget(L, -2);
    uids.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
    lua_pop(L, 2);
  }
  if (uids != modUids_)
    Logf(LogLevel::Warning, "moho64: the session's %zu mods differ from the %zu mounted mods", uids.size(),
         modUids_.size());
  lua_setglobal(L, "__active_mods");
}

bool Sim::CallGlobal(const char* fn, int nargs) {
  lua_State* L = state_->L();
  int base = lua_gettop(L) - nargs;
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  lua_pushstring(L, fn);
  lua_rawget(L, LUA_GLOBALSINDEX);
  lua_insert(L, base + 2);
  bool ok = lua_pcall(L, nargs, 0, base + 1) == 0;
  if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, base);
  return ok;
}

bool Sim::Start(const ReplayHeader& replay) {
  rng_.seed(replay.seed);
  hasGauss_ = false;
  rngTrace_ = getenv("MOHO64_DEBUG_RNG") != nullptr;
  cheats = replay.cheats;

  // Map
  {
    std::string mapPath = replay.map;
    auto data = vfs_->ReadFile(mapPath);
    map_ = std::make_unique<TerrainMap>();
    std::string err;
    if (!data || !map_->Load(*data, &err)) {
      Logf(LogLevel::Error, "moho64: cannot load map %s: %s", mapPath.c_str(), data ? err.c_str() : "file not found");
      return false;
    }
    Logf(LogLevel::Info, "Background task \"Map loader %s\" finished.", Vfs::Normalize(mapPath).c_str());
  }

  // Sim Lua state
  state_ = std::make_unique<ScriptState>(ScriptState::Kind::Sim, vfs_);
  lua_State* L = state_->L();
  state_->SetHookDirs(hookDirs_);
  lua_pushlightuserdata(L, const_cast<char*>(&kSimKey));
  lua_pushlightuserdata(L, this);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_setextra(L, 1, this);
  threads_ = std::make_unique<ThreadScheduler>(L);
  RegisterSimClasses(L);
  RegisterThreadBindings(L, threads_.get());
  RegisterSimBindings(L);
  RegisterEntityBindings(L);
  RegisterCommandBindings(L);
  RegisterEffectBindings(L);
  RegisterEconomyBindings(L);
  RegisterBuildBindings(L);
  RegisterCollisionBindings(L);
  RegisterMotorBindings(L);
  RegisterCombatBindings(L);
  RegisterAnimBindings(L);
  RegisterAirBindings(L);
  RegisterTransportBindings(L);
  RegisterIntelBindings(L);
  SetModsGlobal(*state_);
  // The user layer's language (prefs 'options_overrides.language', default '') - set by the engine.
  lua_pushstring(L, "");
  lua_setglobal(L, "__language");
  bps_.fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  bps_.CopyToSim(rules_->L(), L);
  RegisterCategoryBindings(L, &bps_);
  rules_.reset();  // the rules state is done

  if (!state_->DoScript("/lua/simInit.lua")) return false;

  // ScenarioInfo from the session; ArmySetup from the armies' player options.
  if (!PushSerializedLua(L, replay.scenario) || !lua_istable(L, -1)) {
    Logf(LogLevel::Error, "moho64: bad scenario info in replay");
    return false;
  }
  lua_pushstring(L, "ArmySetup");
  lua_newtable(L);
  for (const auto& a : replay.armies) {
    if (!PushSerializedLua(L, a.options) || !lua_istable(L, -1)) {
      Logf(LogLevel::Error, "moho64: bad army options in replay");
      return false;
    }
    lua_pushstring(L, "ArmyName");
    lua_rawget(L, -2);
    lua_insert(L, -2);
    lua_rawset(L, -3);
  }
  lua_rawset(L, -3);
  lua_setglobal(L, "ScenarioInfo");

  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  auto lap = [&](const char* what) {
    auto t1 = clk::now();
    Logf(LogLevel::Debug, "moho64: %s %.2f s", what, std::chrono::duration<double>(t1 - t0).count());
    t0 = t1;
  };
  if (!CallGlobal("SetupSession", 0)) return false;
  lap("SetupSession");
  if (!CreateArmies(replay)) return false;
  lap("CreateArmies");
  CreateMapProps();  // Prop::PROP_Create in the original, then the count is logged
  Logf(LogLevel::Warning, " NUM PROPS = %zu", map_->props.size());
  lap("map props");
  if (!CallGlobal("BeginSession", 0)) return false;
  lap("BeginSession");
  return true;
}

bool Sim::CreateArmies(const ReplayHeader& replay) {
  lua_State* L = state_->L();
  // Armies of the session, in session order.
  int saveArmies = lua_gettop(L) + 1;
  lua_pushnil(L);
  lua_getglobal(L, "ScenarioInfo");
  lua_pushstring(L, "ArmySetup");
  lua_gettable(L, -2);
  int setup = lua_gettop(L);
  for (const auto& ra : replay.armies) {
    if (!PushSerializedLua(L, ra.options)) continue;
    int opts = lua_gettop(L);
    lua_pushstring(L, "ArmyName");
    lua_rawget(L, opts);
    std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    lua_pop(L, 1);  // every army of the session exists, also those the map's save lacks (probe: ARMY_9)
    auto army = std::make_unique<Army>();
    army->index = static_cast<int>(armies_.size()) + 1;
    army->name = name;
    lua_pushstring(L, name.c_str());
    lua_gettable(L, setup);  // the ArmySetup entry (shared with Lua)
    int entry = lua_gettop(L);
    lua_pushstring(L, "ArmyIndex");
    lua_pushnumber(L, army->index);
    lua_rawset(L, entry);
    auto field = [&](const char* k) {
      lua_pushstring(L, k);
      lua_rawget(L, entry);
    };
    field("PlayerName");
    army->nickname = lua_isstring(L, -1) ? lua_tostring(L, -1) : name;
    field("Faction");
    army->faction = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) - 1 : 0;
    field("Civilian");
    army->civilian = lua_toboolean(L, -1);
    field("Human");
    army->human = lua_toboolean(L, -1);
    lua_settop(L, setup);
    armies_.push_back(std::move(army));
  }
  lua_settop(L, saveArmies - 1);
  float cap = 1000;  // the lobby's unit cap (ScenarioInfo.Options.UnitCap)
  lua_getglobal(L, "ScenarioInfo");
  lua_pushstring(L, "Options");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "UnitCap");
    lua_gettable(L, -2);
    if (lua_isstring(L, -1)) cap = static_cast<float>(std::atof(lua_tostring(L, -1)));
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
  for (auto& a : armies_) {
    a->alliance.assign(armies_.size(), 0);
    a->unitCap = cap;
  }

  InitIntelGrids(*this, L);  // each army's recon DB (CArmyImpl ctor)

  // Brains: an instance of /lua/aibrain.lua's AIBrain bound to the engine brain.
  for (auto& a : armies_) {
    int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/aibrain.lua");
    if (lua_pcall(L, 1, 1, top + 1) != 0) {
      LogScriptError(lua_tostring(L, -1));
      lua_settop(L, top);
      return false;
    }
    lua_pushstring(L, "AIBrain");
    lua_gettable(L, -2);
    int cls = lua_gettop(L);
    GenerateArmyStart(a.get());  // the army ctor's two draws (0x6fe842), before its OnCreateArmyBrain
    auto* brain = new AiBrain;
    brain->army = a.get();
    a->brain = brain;
    lua_newtable(L);
    lua_pushvalue(L, cls);
    lua_setmetatable(L, -2);
    BindObject(L, -1, brain);
    owned_.emplace_back(brain);
    lua_settop(L, top);
    a->pool = CreatePlatoon(L, a.get(), "ArmyPool", "");
    lua_pushnumber(L, a->index);
    PushObject(L, brain);
    lua_pushstring(L, a->name.c_str());
    lua_pushstring(L, a->nickname.c_str());
    if (!CallGlobal("OnCreateArmyBrain", 4)) return false;
    lua_settop(L, top);
  }
  return true;
}

// CArmyImpl::GenerateArmyStart 0x6ffcb0 (sim_random.md 4.2): a provisional start from two draws.
void Sim::GenerateArmyStart(Army* a) {
  uint32_t cb = 0x2f4ccccc;
  float c;
  std::memcpy(&c, &cb, 4);
  float fx = static_cast<float>(static_cast<double>(NextUInt32()) * c + 0.1f);
  double fz = static_cast<double>(NextUInt32()) * c + 0.1f;
  uint32_t w = map_ ? static_cast<uint32_t>(map_->width()) : 0, h = map_ ? static_cast<uint32_t>(map_->height()) : 0;
  a->startX = static_cast<float>(static_cast<double>(w) * fx);
  a->startZ = static_cast<float>(static_cast<double>(h) * fz);
}

// The map's own props (trees, rocks, wrecks placed in the editor).
void Sim::CreateMapProps() {
  int missing = 0;
  for (const auto& mp : map_->props) {
    const BlueprintInfo* bp = bps_.Find(mp.blueprint);
    if (!bp) {
      if (++missing <= 5) Logf(LogLevel::Debug, "moho64: map prop blueprint not found: %s", mp.blueprint.c_str());
      continue;
    }
    // rotation rows are the prop's x, y, z axes -> quaternion
    const float* m = mp.rotation;
    float r00 = m[0], r01 = m[3], r02 = m[6], r10 = m[1], r11 = m[4], r12 = m[7], r20 = m[2], r21 = m[5], r22 = m[8];
    Quat q;
    float tr = r00 + r11 + r22;
    if (tr > 0) {
      float s = std::sqrt(tr + 1.0f) * 2;
      q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
      float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
      q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
      float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
      q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
      float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
      q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    CreateProp(state_->L(), *bp, mp.position, q, mp.scale);
  }
  if (missing) Logf(LogLevel::Debug, "moho64: %d map props have no blueprint", missing);
}

void Sim::QueueDestroy(Entity* e) {
  if (e->destroyQueued) return;
  e->destroyQueued = true;
  destroyQueue_.push_back(e);
}

// Entities destroyed by script: OnDestroy, then they leave the world (FA exe Entity::Destroy queues,
// Entity::OnDestroy runs the script callback later).
// The original (Sim::AdvanceBeat) drains the queue calling each entity's OnDestroy; the engine
// objects are deleted only afterwards, so an OnDestroy can still use an entity destroyed earlier in
// the same pass (a shield's OnDestroy calls Owner:SetShieldRatio after its unit's OnDestroy).
void Sim::ProcessDestroyQueue() {
  lua_State* L = state_->L();
  std::vector<Entity*> done;
  for (size_t i = 0; i < destroyQueue_.size(); ++i) {  // OnDestroy may destroy more
    Entity* e = destroyQueue_[i];
    CallMethod(L, e, "OnDestroy", 0);
    entities_.erase(e->handle);
    byId_.erase(e->id);
    ReleaseEntityIntel(*this, e);
    GridRemove(*this, e);
    if (e->kind == Entity::Kind::Projectile) {
      auto& v = projectiles;
      v.erase(std::remove(v.begin(), v.end(), static_cast<Projectile*>(e)), v.end());
    } else if (e->kind == Entity::Kind::Shield) {
      shields.erase(std::remove(shields.begin(), shields.end(), static_cast<ShieldEntity*>(e)), shields.end());
    } else if (e->kind == Entity::Kind::Beam) {
      beams.erase(std::remove(beams.begin(), beams.end(), static_cast<CollisionBeam*>(e)), beams.end());
    } else if (e->kind == Entity::Kind::Prop) {
      props.erase(std::remove(props.begin(), props.end(), static_cast<Prop*>(e)), props.end());
      propGridDirty_ = true;
    }
    if (e->kind == Entity::Kind::Unit) {
      Unit* u = static_cast<Unit*>(e);
      ReleaseUnitCombat(*this, u);
      AdjacencyLost(*this, L, u);
      ForgetUnitCommands(u);
      UnitEconomyRelease(u);
      ReleaseStructure(*this, u);
      RemoveUnitFromLists(u);
    }
    if (e->kind == Entity::Kind::Unit && e->army) e->army->unitCost -= static_cast<Unit*>(e)->capCost;
    if (e->army && e->army->pool) {
      auto& v = e->army->pool->units;
      v.erase(std::remove(v.begin(), v.end(), static_cast<Unit*>(nullptr)), v.end());
      for (size_t k = 0; k < v.size(); ++k)
        if (static_cast<Entity*>(v[k]) == e) {
          v.erase(v.begin() + static_cast<long>(k));
          break;
        }
    }
    done.push_back(e);
  }
  destroyQueue_.clear();
  for (Entity* e : done)
    if (e->kind != Entity::Kind::Blip) ReleaseId(e->id);  // (~Entity in EntityDb::Purge)
  static const bool dbg = getenv("MOHO64_DEBUG_DESTROY") != nullptr;
  for (Entity* e : done) {
    if (dbg && e->kind == Entity::Kind::Unit && e->HasLuaObject()) {
      Unit* u = static_cast<Unit*>(e);
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s of army %d, destroyed tick %u, dead %d, complete %.2f, built tick %u",
                    e->blueprint ? e->blueprint->id.c_str() : "?", e->army ? e->army->index : 0, tick_, e->dead ? 1 : 0,
                    e->fractionComplete, u->lastMaterializeTick);
      PushObject(L, e);
      lua_pushstring(L, "_moho64_destroyed");
      lua_pushstring(L, buf);
      lua_rawset(L, -3);
      lua_pop(L, 1);
    }
    e->UnbindLua();
  }
}

// One beat: the tick counter advances, then the script threads due at that tick run.
namespace {
void InsertById(std::vector<Unit*>& v, Unit* u) {
  auto it = std::lower_bound(v.begin(), v.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  v.insert(it, u);
}
void EraseById(std::vector<Unit*>& v, Unit* u) {
  auto it = std::lower_bound(v.begin(), v.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  if (it != v.end() && *it == u) v.erase(it);
}
}  // namespace

void Sim::RebuildUnitGrid() {
  int w = map_ ? (map_->width() + 15) / 16 + 1 : 64, h = map_ ? (map_->height() + 15) / 16 + 1 : 64;
  if (w != gridW_ || h != gridH_) {
    gridW_ = w;
    gridH_ = h;
    grid_.assign(static_cast<size_t>(w) * h, {});
  } else {
    for (auto& c : grid_) c.clear();
  }
  for (Unit* u : units_) {
    if (u->destroyQueued) continue;
    int cx = std::clamp(static_cast<int>(u->position.x) >> 4, 0, gridW_ - 1);
    int cz = std::clamp(static_cast<int>(u->position.z) >> 4, 0, gridH_ - 1);
    grid_[static_cast<size_t>(cz) * gridW_ + cx].push_back(u);
  }
  gridDirty_ = false;
}

static int g_unitsCreated = 0;
const std::vector<Unit*>& Sim::CommandOrder() {
  if (cmdOrderDirty_) {
    cmdOrder_ = units_;
    std::sort(cmdOrder_.begin(), cmdOrder_.end(), [](const Unit* a, const Unit* b) { return a->cmdSeq < b->cmdSeq; });
    cmdOrderDirty_ = false;
  }
  return cmdOrder_;
}
void Sim::ResumeCommandThread(Unit* u) {
  u->cmdSeq = ++cmdSeqNext_;
  cmdOrderDirty_ = true;
}
void Sim::AddUnitToLists(Unit* u) {
  ++g_unitsCreated;
  gridDirty_ = true;
  u->cmdSeq = ++cmdSeqNext_;
  cmdOrderDirty_ = true;
  InsertById(units_, u);
  if (u->army) InsertById(u->army->units, u);
}
void Sim::RemoveUnitFromLists(Unit* u) {
  gridDirty_ = true;
  cmdOrderDirty_ = true;
  EraseById(units_, u);
  if (u->army) EraseById(u->army->units, u);
}

namespace {
// MOHO64_PROFILE=1: time spent per phase of the tick, logged every 1000 ticks.
struct PhaseTimer {
  static constexpr int kN = 12;
  const char* names[kN] = {"economy", "recon+cleanup", "commands", "collision", "motion+aim", "anim+unitecon",
                           "projectiles", "beams", "weapons", "navigators+paths", "threads", "destroy"};
  double total[kN] = {};
  std::chrono::steady_clock::time_point t;
  bool on = getenv("MOHO64_PROFILE") != nullptr;
  void Start() {
    if (on) t = std::chrono::steady_clock::now();
  }
  void Lap(int i) {
    if (!on) return;
    auto n = std::chrono::steady_clock::now();
    total[i] += std::chrono::duration<double, std::milli>(n - t).count();
    t = n;
  }
  void Report(uint32_t tick) {
    if (!on || tick % 1000 != 0) return;
    std::string s;
    char buf[64];
    for (int i = 0; i < kN; ++i) {
      std::snprintf(buf, sizeof buf, " %s %.2f", names[i], total[i] / 1000.0);
      s += buf;
      total[i] = 0;
    }
    Logf(LogLevel::Info, "moho64 profile tick %u (ms/tick):%s", tick, s.c_str());
  }
};
PhaseTimer g_prof;
}  // namespace

// Order (Sim::AdvanceBeat 0x749f40, engine-ref beat_order.md): the tick count, then per army the
// economy, navigators and path queues and weapons; then the three task stages: command threads
// (0x958, in thread order), Lua threads (0x944), entities (0x930: unit motion with each unit's
// aim controllers and own beat, then projectiles and beams, so a shot moves in the beat it is
// fired and an impact found in beat N reaches OnImpact in beat N+1's move); then recon, killed
// units' clean-up, intel coordinates and the destroy queue. Lua at tick N sees the positions of
// beat N-1's motion; callbacks raised during motion are labelled N.
// CArmyStats::Update 0x70bea0 (army_stats.md 2.3): after the army's economy pass, from tick 11 on
void EvaluateStatTriggers(Sim& sim, Army& a) {
  std::vector<std::shared_ptr<Army::StatTrigger>> fired;
  for (auto it = a.statTriggers.begin(); it != a.statTriggers.end();) {
    const Army::StatTrigger& t = **it;
    bool ok = !t.conds.empty();
    for (const Army::StatCond& c : t.conds) {
      float v = 0;
      if (!c.category) {
        auto f = a.stats.find(c.stat);
        v = f != a.stats.end() ? f->second : 0.0f;
      }
      switch (c.op) {
        case 0: ok = v > c.thr; break;
        case 1: ok = v >= c.thr; break;
        case 2: ok = c.thr > v; break;
        case 3: ok = c.thr >= v; break;
        default: break;
      }
      if (!ok) break;
    }
    if (ok) {
      fired.push_back(*it);
      it = a.statTriggers.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& t : fired) {
    if (!a.brain || !a.brain->HasLuaObject()) continue;
    lua_State* L = sim.L();
    lua_pushstring(L, t->name.c_str());
    sim.CallMethod(L, a.brain, "OnStatsTrigger", 1);
  }
}

void Sim::Tick() {
  ++tick_;
  g_prof.Start();
  // the armies' stage: economy, navigators and path queues, weapons (acquire, fire)
  EconomyBeginBeat(*this);
  g_prof.Lap(0);
  LandNavTickAll(*this);
  g_prof.Lap(9);
  WeaponsTick(*this);
  g_prof.Lap(8);
  CommandStage(*this);  // stage 0x958 (arrivals from last beat's motion end their commands here)
  g_prof.Lap(2);
  threads_->RunTick(tick_);  // stage 0x944: Lua threads
  g_prof.Lap(10);
  SteeringTickAll(*this);  // the steering stage (spline points, predicted collisions)
  g_prof.Lap(3);
  for (size_t i = 0; i < units_.size(); ++i) {  // (motion may create or destroy nothing)
    Unit* u = units_[i];
    if (getenv("MOHO64_DEBUG_PROPHIT") && tick_ < 3 && u->id == 9437226)
      Logf(LogLevel::Info, "loop %u dq %d par %u attach %d bb %d", tick_, (int)u->destroyQueued, u->parentId, (int)u->attachFull, (int)u->beingBuilt);
    if (u->destroyQueued) continue;
    u->lastPosition = u->position;
    if (!u->beingBuilt) FuelTick(*this, u);  // CUnitMotion::ProcessFuelLevels (attached units too)
    if (u->parentId && u->attachFull) continue;  // transport cargo: after every unit moved
    if (!u->guarders.empty() && !u->guardForm) UpdateGuardFormation(*this, u);  // MotionTick order
    UpdateInfoCache(*this, u);  // formation fields and the speed cap
    const Quat prevOri = u->orientation;
    if (u->parentId) {  // attached (a factory's product): held at the parent's bone
      Entity* p = FindEntity(u->parentId);
      if (p && !p->destroyQueued) {  // the child takes the bone's transform (Entity attach)
        Vec3 bp;
        Quat bq;
        BoneWorld(p, u->parentBone, &bp, &bq);
        u->position = bp;
        u->orientation = bq;
      }
      u->motion.vel = {};
    } else {
      MotionTick(*this, u);
    }
    if (u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y ||
        u->position.z != u->lastPosition.z)
      u->lastMoveTick = tick_;
    // CAniActor::UpdateManipulators 0x63aa80, after CUnitMotion::MotionTick (0x6a908c before 0x6a90d5): the pose
    // is built on the pending transform this motion just wrote; the aim bone is read from last beat's pose
    if (!u->builderArms.empty()) BuilderArmsTick(*this, u, u->lastPosition, prevOri);
    UnitAimTick(*this, u);
    if (u->isFactoryBuilder) ValidateFactoryCommandQueue(*this, u);  // end of Unit::MotionTick
  }
  // transport cargo follows its attach bone (Entity::TaskTick: after the parents moved)
  if (anyAttached) {
    AttachedUnitsTick(*this);
    anyAttached = false;
    for (size_t i = 0; i < units_.size(); ++i) {
      Unit* u = units_[i];
      if (u->destroyQueued || !u->transportedBy) continue;
      anyAttached = true;
      if (u->parentId && u->attachFull) UnitAimTick(*this, u);
    }
  }
  MotorsTick(*this);  // the entity stage: props with a motor (falling trees, sinking)
  g_prof.Lap(4);
  AnimTick(*this);
  gridDirty_ = true;
  // the units' own beat: economy events, regeneration or decay, consumption and production
  for (size_t i = 0; i < units_.size(); ++i)
    if (!units_[i]->destroyQueued) UnitEconomyTick(*this, units_[i]);
  g_prof.Lap(5);
  // Projectiles and beams move after every task stage (Sim::AdvanceBeat runs the three task
  // stages first), so a shot moves in the beat it was fired, and Lua sees the move next beat.
  ProjectilesTick(*this);
  g_prof.Lap(6);
  BeamsTick(*this);
  g_prof.Lap(7);
  ReconBeat(*this);  // army (tick % armies) updates its blips
  FormationsTick(*this);  // step 11: the formation DB
  KillCleanupTick(*this);
  GridAdvanceCoords(*this);    // step 13: moved units take their (widened) cells in the entity grid
  MotorsAdvanceCoords(*this);  // and moved props theirs
  AdvanceIntelCoords(*this);  // moved intel sources move their circles
  g_prof.Lap(1);
  ProcessDestroyQueue();
  UpdateIdPools();  // EntityDb::Purge (Sim::Sync)
  g_prof.Lap(11);
  g_prof.Report(tick_);
  static const bool stats = getenv("MOHO64_STATS") != nullptr;
  if (stats && tick_ % 600 == 0) {  // a quick health line per game minute
    int n = 0, air = 0, flying = 0, dead = 0;
    for (Unit* u : units_) {
      if (u->destroyQueued) continue;
      ++n;
      if (u->dead) ++dead;
      if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {
        ++air;
        if (u->layer == "Air") ++flying;
      }
    }
    Logf(LogLevel::Info, "moho64 stats tick %u: units %d (aircraft %d, flying %d, dying %d), created %d, projectiles %zu", tick_,
         n, air, flying, dead, g_unitsCreated, projectiles.size());
  }
}

// EntityDB::DoReserveId 0x684480: the lowest free serial, else a new one
uint32_t Sim::ReserveId(Army* army, uint32_t family) {
  uint32_t a = army ? static_cast<uint32_t>(army->index - 1) : 0xffu;
  uint32_t key = ((family << 8) | a) << 20;
  IdPool& p = idPools_[key];
  uint32_t s;
  if (!p.free.empty()) {
    s = *p.free.begin();
    p.free.erase(p.free.begin());
  } else {
    s = p.next++;
  }
  return key | s;
}
// EntityDB::ReleaseId 0x684690: into the newest quarantine slot
void Sim::ReleaseId(uint32_t id) {
  auto it = idPools_.find(id & 0xFFF00000u);
  if (it == idPools_.end()) return;
  IdPool& p = it->second;
  p.ring[(p.tail + 99) % 100].push_back(id & 0xFFFFFu);
}
// IdPool::Update 0x403a30, once per beat (EntityDb::Purge)
void Sim::UpdateIdPools() {
  for (auto& [key, p] : idPools_) {
    if ((p.tail + 1) % 100 == p.head) {
      for (uint32_t s : p.ring[p.head]) p.free.insert(s);
      p.ring[p.head].clear();
      while (p.next > 0 && p.free.count(p.next - 1)) {
        p.free.erase(p.next - 1);
        --p.next;
      }
      p.head = (p.head + 1) % 100;
    }
    p.ring[p.tail].clear();
    p.tail = (p.tail + 1) % 100;
  }
}

void Sim::RebuildPropGrid() {
  if (gridW_ == 0) RebuildUnitGrid();
  propGrid_.assign(static_cast<size_t>(gridW_) * gridH_, {});
  for (Prop* p : props) {
    if (p->destroyQueued) continue;
    int cx = std::clamp(static_cast<int>(p->position.x) >> 4, 0, gridW_ - 1);
    int cz = std::clamp(static_cast<int>(p->position.z) >> 4, 0, gridH_ - 1);
    propGrid_[static_cast<size_t>(cz) * gridW_ + cx].push_back(p);
  }
  propGridDirty_ = false;
}

EntityGrid& Sim::entityGrid() {
  if (!egrid_) egrid_ = std::make_unique<EntityGrid>();
  if (!egrid_->ready() && map_) egrid_->Init(map_->width(), map_->height());
  return *egrid_;
}

Navigation& Sim::navigation() {
  if (!nav_) {
    nav_ = std::make_unique<Navigation>(map_.get());
    // blocking terrain types (TerrainTypes.lua: Blocking = true)
    lua_State* L = state_->L();
    int top = lua_gettop(L);
    PushTerrainTypes(L);
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        if (lua_isnumber(L, -2) && lua_istable(L, -1)) {
          lua_pushstring(L, "Blocking");
          lua_rawget(L, -2);
          if (lua_toboolean(L, -1)) nav_->SetBlockingTerrainType(static_cast<int>(lua_tonumber(L, -3)), true);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
    }
    lua_settop(L, top);
  }
  return *nav_;
}

}  // namespace moho
