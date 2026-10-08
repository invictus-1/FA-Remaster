#include "sim/sim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Random() -> [0,1); Random(max) -> 1..max; Random(min, max) -> min..max (integers)
int l_Random(lua_State* L) {
  float r = S(L)->Random();
  int n = lua_gettop(L);
  if (n == 0) {
    lua_pushnumber(L, r);
  } else if (n == 1) {
    int hi = static_cast<int>(luaL_checknumber(L, 1));
    lua_pushnumber(L, 1 + static_cast<int>(r * hi));
  } else {
    int lo = static_cast<int>(luaL_checknumber(L, 1)), hi = static_cast<int>(luaL_checknumber(L, 2));
    lua_pushnumber(L, lo + static_cast<int>(r * (hi - lo + 1)));
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

// Economy type argument: "MASS" / "ENERGY" (any case)
bool IsMass(lua_State* L, int idx) {
  const char* t = luaL_checkstring(L, idx);
  return (t[0] | 0x20) == 'm';
}
int l_brain_GetEconomyStored(lua_State* L) {
  Army* a = Brain(L)->army;
  lua_pushnumber(L, IsMass(L, 2) ? a->massStored : a->energyStored);
  return 1;
}
int l_brain_GetEconomyStoredRatio(lua_State* L) {
  Army* a = Brain(L)->army;
  bool m = IsMass(L, 2);
  float max = m ? a->massMax : a->energyMax;
  lua_pushnumber(L, max > 0 ? (m ? a->massStored : a->energyStored) / max : 0);
  return 1;
}
int l_brain_EconomyZero(lua_State* L) {  // income, requested, usage, trend: no economy yet (M4)
  Brain(L);
  lua_pushnumber(L, 0);
  return 1;
}
int l_SetArmyEconomy(lua_State* L) {  // SetArmyEconomy(army, mass, energy)
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->massStored = static_cast<float>(luaL_checknumber(L, 2));
  a->energyStored = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
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
int l_brain_GetBlueprintStat(lua_State* L) {
  Brain(L);
  lua_pushnumber(L, 0);  // TODO(M4): per-blueprint army stats
  return 1;
}

}  // namespace

void RegisterSimBindings(lua_State* L) {
  SetGlobal(L, "SetArmyEconomy", l_SetArmyEconomy);
  SetMethod(L, "CAiBrain", "GetEconomyStored", l_brain_GetEconomyStored);
  SetMethod(L, "CAiBrain", "GetEconomyStoredRatio", l_brain_GetEconomyStoredRatio);
  for (const char* m : {"GetEconomyIncome", "GetEconomyRequested", "GetEconomyUsage", "GetEconomyTrend"})
    SetMethod(L, "CAiBrain", m, l_brain_EconomyZero);
  SetMethod(L, "CAiBrain", "GetArmyStat", l_brain_GetArmyStat);
  SetMethod(L, "CAiBrain", "SetArmyStat", l_brain_SetArmyStat);
  SetMethod(L, "CAiBrain", "AddArmyStat", l_brain_AddArmyStat);
  SetMethod(L, "CAiBrain", "GetBlueprintStat", l_brain_GetBlueprintStat);
  SetGlobal(L, "ListArmies", l_ListArmies);
  SetGlobal(L, "GetArmyBrain", l_GetArmyBrain);
  SetGlobal(L, "GetFocusArmy", l_GetFocusArmy);
  SetGlobal(L, "ArmyIsCivilian", l_ArmyIsCivilian);
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
  armies_.clear();
  threads_.reset();
  state_.reset();
}

lua_State* Sim::L() const { return state_ ? state_->L() : nullptr; }

Sim* Sim::From(lua_State* L) { return static_cast<Sim*>(lua_getextra(L, 1)); }

float Sim::Random() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }

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
    entities_.erase(e->id);
    if (e->kind == Entity::Kind::Unit) {
      ForgetUnitCommands(static_cast<Unit*>(e));
      RemoveUnitFromLists(static_cast<Unit*>(e));
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
  for (Entity* e : done) e->UnbindLua();
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

void Sim::AddUnitToLists(Unit* u) {
  InsertById(units_, u);
  if (u->army) InsertById(u->army->units, u);
}
void Sim::RemoveUnitFromLists(Unit* u) {
  EraseById(units_, u);
  if (u->army) EraseById(u->army->units, u);
}

// Order inside a beat (from the oracle probe): unit commands start, units move, finished moves
// end (and queued moves continue in the same beat), then the script threads run.
void Sim::Tick() {
  ++tick_;
  CommandsBeforeMotion(*this);
  CollisionTick(*this);
  for (size_t i = 0; i < units_.size(); ++i)  // (motion may create or destroy nothing)
    if (!units_[i]->destroyQueued) MotionTick(*this, units_[i]);
  CommandsAfterMotion(*this);
  threads_->RunTick(tick_);
  ProcessDestroyQueue();
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
