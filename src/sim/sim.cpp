#include "sim/sim.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "script/threads.h"
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

}  // namespace

void RegisterSimBindings(lua_State* L) {
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
  SetMethod(L, "CAiBrain", "GetArmyIndex", l_brain_GetArmyIndex);
  SetMethod(L, "CAiBrain", "GetFactionIndex", l_brain_GetFactionIndex);
  SetMethod(L, "CAiBrain", "IsDefeated", l_brain_IsDefeated);
}

// ---- Sim -----------------------------------------------------------------------------------

Sim::Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids)
    : vfs_(vfs), hookDirs_(std::move(hookDirs)), modUids_(std::move(modUids)) {}

Sim::~Sim() {
  owned_.clear();  // unbind every engine object while the Lua state still exists
  ReleaseEffectObjects();
  armies_.clear();
  threads_.reset();
  state_.reset();
}

lua_State* Sim::L() const { return state_ ? state_->L() : nullptr; }

Sim* Sim::From(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kSimKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* s = static_cast<Sim*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return s;
}

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
  SetActiveMods(*rules_, modUids_);
  return rules_->DoScript("/lua/ruleinit.lua");
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
  threads_ = std::make_unique<ThreadScheduler>(L);
  RegisterSimClasses(L);
  RegisterThreadBindings(L, threads_.get());
  RegisterSimBindings(L);
  RegisterEntityBindings(L);
  RegisterEffectBindings(L);
  SetActiveMods(*state_, modUids_);
  // The user layer's language (prefs 'options_overrides.language', default '') - set by the engine.
  lua_pushstring(L, "");
  lua_setglobal(L, "__language");
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
  Logf(LogLevel::Warning, " NUM PROPS = %zu", map_->props.size());
  CreateMapProps();
  lap("map props");
  if (!CallGlobal("BeginSession", 0)) return false;
  lap("BeginSession");
  return true;
}

bool Sim::CreateArmies(const ReplayHeader& replay) {
  lua_State* L = state_->L();
  // Armies of the session that the map's save file defines (Scenario.Armies), in session order.
  lua_getglobal(L, "Scenario");
  lua_pushstring(L, "Armies");
  lua_gettable(L, -2);
  int saveArmies = lua_gettop(L);
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
    lua_pushstring(L, name.c_str());
    lua_gettable(L, saveArmies);
    bool inSave = lua_istable(L, -1);
    lua_pop(L, 2);
    if (!inSave) continue;
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
  for (auto& a : armies_) {
    a->alliance.assign(armies_.size(), 0);
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
    // The brain's own set-up runs first (it sets BrainType, which OnCreateArmyBrain hooks read).
    PushObject(L, brain);
    int b = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_pushstring(L, a->human ? "OnCreateHuman" : "OnCreateAI");
    lua_gettable(L, b);
    if (lua_isfunction(L, -1)) {
      lua_pushvalue(L, b);
      lua_pushstring(L, a->plans.c_str());
      if (lua_pcall(L, 2, 0, b + 1) != 0) LogScriptError(lua_tostring(L, -1));
    }
    lua_settop(L, top);
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
      ++missing;
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

void Sim::Tick() {
  threads_->RunTick(tick_);
  ++tick_;
}

}  // namespace moho
