// The economy (see economy.h for what the original does).
#include "sim/economy.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

const char* const kStatNames[2][12] = {
    {"Economy_TotalProduced_Energy", "Economy_TotalConsumed_Energy", "Economy_Income_Energy",
     "Economy_Output_Energy", "Economy_Stored_Energy", "Economy_Reclaimed_Energy", "Economy_MaxStorage_Energy",
     "Economy_PeakStorage_Energy", "Economy_Shared_Energy", "Economy_Trend_Energy", "Economy_Ratio_Energy",
     "Economy_AccumExcess_Energy"},
    {"Economy_TotalProduced_Mass", "Economy_TotalConsumed_Mass", "Economy_Income_Mass", "Economy_Output_Mass",
     "Economy_Stored_Mass", "Economy_Reclaimed_Mass", "Economy_MaxStorage_Mass", "Economy_PeakStorage_Mass",
     "Economy_Shared_Mass", "Economy_Trend_Mass", "Economy_Ratio_Mass", "Economy_AccumExcess_Mass"}};
enum { kTotalProduced, kTotalConsumed, kIncome, kOutput, kStored, kReclaimed, kMaxStorage, kPeak, kShared,
       kTrend, kRatio, kAccumExcess };

float GetNum(lua_State* L, int t, const char* k, float def) {
  if (!lua_istable(L, t)) return def;
  lua_pushstring(L, k);
  lua_rawget(L, t);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
bool GetBool(lua_State* L, int t, const char* k) {
  if (!lua_istable(L, t)) return false;
  lua_pushstring(L, k);
  lua_rawget(L, t);
  bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}
// Push t[k] (nil when t is not a table).
void PushField(lua_State* L, int t, const char* k) {
  if (!lua_istable(L, t)) {
    lua_pushnil(L);
    return;
  }
  lua_pushstring(L, k);
  lua_rawget(L, t);
}

int LayerBits(const char* s) {  // "Land", "Seabed", "Sub", "Water", "Air", or a decimal mask
  if (!s) return 0;
  static const std::map<std::string, int> names = {{"Land", 1}, {"Seabed", 2}, {"Sub", 4}, {"Water", 8}, {"Air", 16}};
  auto it = names.find(s);
  if (it != names.end()) return it->second;
  return std::atoi(s);
}

// ---- army economy: the share-out at the start of a beat (ArmyProcessEconomy 0x771b50) --------

void ProcessArmyEconomy(Sim& sim, Army& army) {
  ArmyEconomy& e = army.econ;
  auto& reqs = e.requests;
  reqs.erase(std::remove_if(reqs.begin(), reqs.end(), [](const std::shared_ptr<EconRequest>& r) { return !r->live; }),
             reqs.end());
  float single[2] = {0, 0}, dual[2] = {0, 0};
  for (auto& r : reqs) {
    float rem[2];
    int n = 0;
    for (int i = 0; i < 2; ++i) {
      float d = r->requested[i] - r->granted[i];
      rem[i] = d > 0 ? d : 0;
      if (rem[i] != 0) ++n;
    }
    float* acc = n < 2 ? single : dual;
    for (int i = 0; i < 2; ++i) acc[i] += rem[i];
  }
  float income[2] = {e.income[kEnergy], e.income[kMass]};
  float available[2];
  for (int i = 0; i < 2; ++i) available[i] = e.stored[i] + income[i];
  float total[2] = {single[0] + dual[0], single[1] + dual[1]};
  // the scarcer resource sets the ratio for every request that needs it
  int limiting = 0;
  float f = 1.0f;
  for (int i = 0; i < 2; ++i)
    if (available[i] < total[i] * f) {
      f = available[i] / total[i];
      limiting = i;
    }
  float remain[2];
  for (int i = 0; i < 2; ++i) {
    float d = available[i] - dual[i] * f;
    remain[i] = d > 0 ? d : 0;
  }
  float g = 1.0f;  // requests needing only the other resource share what is left of it
  for (int i = 0; i < 2; ++i)
    if (i != limiting && remain[i] < single[i] * g) g = remain[i] / single[i];
  float granted[2] = {0, 0};
  for (auto& r : reqs) {
    float rem[2];
    for (int i = 0; i < 2; ++i) {
      float d = r->requested[i] - r->granted[i];
      rem[i] = d > 0 ? d : 0;
    }
    float k = rem[limiting] == 0 ? g : f;
    for (int i = 0; i < 2; ++i) {
      float give = rem[i] * k;
      granted[i] += give;
      float a = available[i] - give;
      available[i] = a > 0 ? a : 0;
      r->granted[i] += give;
    }
  }
  for (int i = 0; i < 2; ++i) {
    e.requested[i] = total[i];
    e.usage[i] = single[i] * g + dual[i] * f;
    e.incomeStat[i] = e.income[i];
  }
  // overflow above the maximum: to allies with room (resource sharing), the rest is lost
  float excess[2], shared[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    float max = static_cast<float>(e.maxStorage[i]);
    excess[i] = available[i] <= max ? 0.0f : available[i] - max;
  }
  if (e.sharing) {
    std::vector<Army*> allies;
    for (auto& other : sim.armies()) {
      Army* o = other.get();
      if (o == &army || o->outOfGame) continue;
      if (army.alliance.size() > static_cast<size_t>(o->index - 1) && army.alliance[o->index - 1] == 2) {
        for (int i = 0; i < 2; ++i)
          if (o->econ.stored[i] < static_cast<float>(o->econ.maxStorage[i])) {
            allies.push_back(o);
            break;
          }
      }
    }
    for (int i = 0; i < 2 && !allies.empty(); ++i) {
      if (excess[i] == 0) continue;
      int n = static_cast<int>(allies.size());
      for (Army* o : allies) {
        float give = excess[i] * (1.0f / static_cast<float>(n));
        float room = static_cast<float>(o->econ.maxStorage[i]) - o->econ.stored[i];
        give = room < 0 ? 0 : std::min(give, room);
        o->econ.income[i] += give;
        o->econ.incomeTrend[i] += give;
        shared[i] += give;
        float left = excess[i] - give;
        excess[i] = left > 0 ? left : 0;
        --n;
        if (n == 0) break;
      }
    }
  }
  for (int i = 0; i < 2; ++i) {
    float max = static_cast<float>(e.maxStorage[i]);
    float s = std::min(available[i], max);
    e.stored[i] = s < 0 ? 0 : s;
  }
  // army statistics (the original updates them here)
  auto& st = army.stats;
  for (int i = 0; i < 2; ++i) {
    const char* const* n = kStatNames[i];
    st[n[kTotalProduced]] += e.income[i];
    st[n[kTotalConsumed]] += granted[i];
    st[n[kIncome]] = e.income[i];
    st[n[kOutput]] = granted[i];
    st[n[kStored]] = e.stored[i];
    st[n[kMaxStorage]] = static_cast<float>(e.maxStorage[i]);
    st[n[kReclaimed]] = e.reclaimed[i];
    st[n[kShared]] += shared[i];
    st[n[kTrend]] = (e.incomeTrend[i] - e.requested[i]) * 10.0f;
    float max = static_cast<float>(e.maxStorage[i]);
    st[n[kRatio]] = max > 0 ? e.stored[i] / max : 0.0f;
    float& peak = st[n[kPeak]];
    if (e.stored[i] > peak) peak = e.stored[i];
    st[n[kAccumExcess]] += excess[i];
  }
  for (int i = 0; i < 2; ++i) {
    e.income[i] = 0;
    e.incomeTrend[i] = 0;
  }
}

bool Alive(const Unit* u) { return u && !u->dead && !u->destroyQueued; }

// Unit::SetConsumptionActive (0x6aa900): the request asks for the per-second rates / 10 while
// active; turning it off returns the unused grant to the army.
void ApplyConsumption(Unit* u, bool active) {
  bool was = u->consumptionActive;
  u->consumptionActive = active;
  float req[2] = {u->consumption[kEnergy] * 0.1f, u->consumption[kMass] * 0.1f};
  if (!u->request && u->army) u->request = u->army->econ.NewRequest();
  if (!u->request) return;
  if (!active) {
    // The original hands the unused grant back but keeps it in the request as well (so a unit
    // switched off and on again starts with it); kept as is.
    if (u->army)
      for (int i = 0; i < 2; ++i) u->army->econ.income[i] += u->request->granted[i];
    req[0] = req[1] = 0;
  }
  u->request->requested[0] = req[0];
  u->request->requested[1] = req[1];
  u->consumptionShown[0] = req[0];
  u->consumptionShown[1] = req[1];
  if (active != was) {
    lua_State* L = u->luaState();
    if (L) S(L)->CallMethod(L, u, active ? "OnConsumptionActive" : "OnConsumptionInActive", 0);
  }
}

void DropStorage(Unit* u) {
  if (!u->storageOn) return;
  if (u->army) u->army->econ.ChangeStorage(-1, u->storageCounted);
  u->storageOn = false;
}

// CEconomyEvent tick (0x775270): the whole per-tick amount or nothing.
void EconomyEventTick(Sim& sim, lua_State* L, EconomyEvent* ev) {
  Unit* u = ev->unit;
  if (ev->ticksLeft == 0 || !ev->request || !u) {
    if (u) u->consumptionShown[0] = u->consumptionShown[1] = 0;
    return;
  }
  u->consumptionShown[0] = ev->perTick[0];
  u->consumptionShown[1] = ev->perTick[1];
  float got[2] = {ev->request->granted[0], ev->request->granted[1]};
  if (!(ev->perTick[0] <= got[0] && ev->perTick[1] <= got[1])) return;
  ev->request->granted[0] = ev->request->granted[1] = 0;
  u->consumedTick[0] += got[0];
  u->consumedTick[1] += got[1];
  --ev->ticksLeft;
  if (ev->callbackRef != LUA_NOREF) {
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ev->callbackRef);
    if (lua_isfunction(L, -1)) {
      lua_pushcfunction(L, ScriptTraceback);
      lua_insert(L, -2);
      PushObject(L, u);
      lua_pushnumber(L, 1.0f - static_cast<float>(ev->ticksLeft) / static_cast<float>(ev->ticksTotal));
      if (lua_pcall(L, 2, 0, top + 1) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_settop(L, top);
  }
  if (ev->ticksLeft == 0) {
    ev->done = true;
    ev->request->requested[0] = ev->request->requested[1] = 0;
  }
  (void)sim;
}

}  // namespace

// ---- blueprint values ------------------------------------------------------------------------

const UnitBpData& GetUnitBpData(lua_State* L, const BlueprintInfo& bp) {
  static std::map<const BlueprintInfo*, UnitBpData> cache;
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  UnitBpData d;
  Sim* sim = S(L);
  int top = lua_gettop(L);
  sim->blueprints().PushTable(L, bp);
  int t = lua_gettop(L);
  d.sizeX = GetNum(L, t, "SizeX", 1);
  d.sizeY = GetNum(L, t, "SizeY", 1);
  d.sizeZ = GetNum(L, t, "SizeZ", 1);
  PushField(L, t, "Economy");
  int ec = lua_gettop(L);
  d.buildCostEnergy = GetNum(L, ec, "BuildCostEnergy", 0);
  d.buildCostMass = GetNum(L, ec, "BuildCostMass", 0);
  d.buildTime = GetNum(L, ec, "BuildTime", 0);
  d.buildRate = GetNum(L, ec, "BuildRate", 0);
  d.storage[kEnergy] = GetNum(L, ec, "StorageEnergy", 0);
  d.storage[kMass] = GetNum(L, ec, "StorageMass", 0);
  d.naturalProducer = GetBool(L, ec, "NaturalProducer");
  d.needToFaceTargetToBuild = GetBool(L, ec, "NeedToFaceTargetToBuild");
  d.maxBuildDistance = GetNum(L, ec, "MaxBuildDistance", 0);
  d.initialRallyX = GetNum(L, ec, "InitialRallyX", 0);
  d.initialRallyZ = GetNum(L, ec, "InitialRallyZ", 0);
  d.buildable.assign(static_cast<size_t>(CategoryWordCount()), 0);
  PushField(L, ec, "BuildableCategory");
  if (lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      std::vector<uint64_t> bits;
      if (ParseCategory(L, lua_tostring(L, -1), bits))
        for (size_t w = 0; w < d.buildable.size() && w < bits.size(); ++w) d.buildable[w] |= bits[w];
      lua_pop(L, 1);
    }
  }
  for (uint64_t w : d.buildable)
    if (w) d.hasBuilder = true;
  lua_settop(L, t);
  PushField(L, t, "General");
  d.capCost = GetNum(L, lua_gettop(L), "CapCost", 1);
  lua_settop(L, t);
  PushField(L, t, "Defense");
  d.regenRate = GetNum(L, lua_gettop(L), "RegenRate", 0);
  d.maxHealth = GetNum(L, lua_gettop(L), "MaxHealth", 1);
  lua_settop(L, t);
  PushField(L, t, "Physics");
  int ph = lua_gettop(L);
  PushField(L, ph, "BuildRestriction");
  if (lua_isstring(L, -1)) d.buildRestriction = lua_tostring(L, -1);
  lua_pop(L, 1);
  PushField(L, ph, "BuildOnLayerCaps");
  if (lua_isstring(L, -1)) d.buildOnLayerCaps = LayerBits(lua_tostring(L, -1));
  lua_pop(L, 1);
  PushField(L, ph, "MotionType");
  d.structure = !lua_isstring(L, -1) || std::string(lua_tostring(L, -1)) == "RULEUMT_None";
  lua_pop(L, 1);
  d.maxGroundVariation = GetNum(L, ph, "MaxGroundVariation", 1);
  d.flattenSkirt = GetBool(L, ph, "FlattenSkirt");
  d.skirtOffsetX = GetNum(L, ph, "SkirtOffsetX", 0);
  d.skirtOffsetZ = GetNum(L, ph, "SkirtOffsetZ", 0);
  d.skirtSizeX = GetNum(L, ph, "SkirtSizeX", 0);
  d.skirtSizeZ = GetNum(L, ph, "SkirtSizeZ", 0);
  lua_settop(L, top);
  return cache.emplace(&bp, std::move(d)).first->second;
}

// ---- beat --------------------------------------------------------------------------------------

void EconomyBeginBeat(Sim& sim) {
  for (Unit* u : sim.units()) {
    if (u->dead) continue;
    for (int i = 0; i < 2; ++i) u->producedTick[i] = u->consumedTick[i] = 0;
  }
  for (auto& a : sim.armies()) ProcessArmyEconomy(sim, *a);
}

void UnitEconomyInit(lua_State* L, Unit* u) {
  u->bpData = &GetUnitBpData(L, *u->blueprint);
  u->buildRate = u->bpData->buildRate;
  u->regenRate = u->bpData->regenRate;
  u->buildAllowed.assign(static_cast<size_t>(CategoryWordCount()), ~uint64_t(0));
}

void UnitEconomyRelease(Unit* u) {
  SiloRelease(u);
  if (u->request) {
    u->request->live = false;
    u->request.reset();
  }
  DropStorage(u);
  for (EconomyEvent* ev : u->econEvents) {
    if (ev->request) ev->request->live = false;
    ev->request.reset();
    ev->unit = nullptr;
    ev->done = true;
  }
  u->econEvents.clear();
}

float Materialize(Sim& sim, lua_State* L, Unit* u, float delta) {
  if (delta >= 0) u->lastMaterializeTick = sim.tick();
  if (delta == 0) return 0;
  float old = u->fractionComplete;
  float f = old + delta;
  if (delta <= 0) {
    f = std::clamp(f, 0.0f, 1.0f);
  } else {
    f = std::min(f, 1.0f);
    if (f < 0) f = 0;
    if (u->maxHealth > 0 && f < u->health / u->maxHealth) f = u->health / u->maxHealth;
  }
  u->fractionComplete = f;
  EntityAdjustHealth(L, u, u, u->maxHealth * delta);
  if (u->beingBuilt && u->fractionComplete == 1.0f) {
    u->beingBuilt = false;
    u->unitStates.erase("BeingBuilt");
    Entity* builder = u->builderId ? sim.FindEntity(u->builderId) : nullptr;
    PushObject(L, builder && !builder->destroyQueued ? builder : nullptr);
    lua_pushstring(L, u->layer.c_str());
    sim.CallMethod(L, u, "OnStopBeingBuilt", 2);
    if (u->army) {
      auto& st = u->army->stats;
      st["Units_Active"] += 1;
      st["Units_History"] += 1;
      st["Units_MassValue_Built"] += u->bpData->buildCostMass;
      st["Units_EnergyValue_Built"] += u->bpData->buildCostEnergy;
      st["Units_BeingBuilt"] -= 1;
    }
    UnitFinishedBuilding(sim, L, u);
  }
  return u->fractionComplete - old;
}

void UnitEconomyTick(Sim& sim, Unit* u) {
  lua_State* L = sim.L();
  SiloTick(sim, u);
  for (size_t i = 0; i < u->econEvents.size(); ++i) EconomyEventTick(sim, L, u->econEvents[i]);
  // regeneration, or decay of an abandoned construction
  if (!u->beingBuilt) {
    if (u->health < u->maxHealth && u->regenRate > 0.0f) EntityAdjustHealth(L, u, u, u->regenRate * 0.1f);
  } else if (sim.tick() - u->lastMaterializeTick > 1) {
    const UnitBpData& d = *u->bpData;
    float m = std::max(std::max(d.buildCostEnergy, d.buildCostMass), d.buildTime);
    if (m > 0.0f) {
      Materialize(sim, L, u, -0.1f / m);
      if (u->health <= 0.0f) sim.CallMethod(L, u, "OnDecayed", 0);
    }
  }
  if (u->destroyQueued) return;
  // Unit::HandleResourceManagement
  u->resourceConsumed = 0;
  float prodMult = 1.0f;
  if (!u->bpData->naturalProducer && u->request) prodMult = u->request->LimitingRate();
  if (!u->dead && u->consumptionActive && u->request) {
    float rate = u->request->LimitingRate();
    u->resourceConsumed = rate;
    float want[2] = {u->request->requested[0] * rate, u->request->requested[1] * rate}, took[2];
    u->request->Take(want, took);
    u->consumedTick[0] += took[0];
    u->consumedTick[1] += took[1];
  }
  if (u->beingBuilt || u->dead || !u->productionActive) {
    DropStorage(u);
    return;
  }
  const float* st = u->bpData->storage;
  if (st[0] != 0.0f || st[1] != 0.0f) {
    if (!u->storageOn && u->army) {
      u->army->econ.ChangeStorage(+1, st);
      u->storageCounted[0] = st[0];
      u->storageCounted[1] = st[1];
      u->storageOn = true;
    }
  }
  float add[2] = {u->production[kEnergy] * prodMult * 0.1f, u->production[kMass] * prodMult * 0.1f};
  if (u->army) {
    for (int i = 0; i < 2; ++i) {
      u->army->econ.income[i] += add[i];
      u->army->econ.incomeTrend[i] += add[i];
    }
  }
  u->producedTick[0] += add[0];
  u->producedTick[1] += add[1];
}

// ---- bindings ------------------------------------------------------------------------------

namespace {

int ResourceArg(lua_State* L, int idx) {  // "ENERGY" / "MASS" (case ignored)
  const char* t = luaL_checkstring(L, idx);
  return (t[0] | 0x20) == 'm' ? kMass : kEnergy;
}
AiBrain* Brain(lua_State* L) { return CheckObject<AiBrain>(L, 1); }
Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

int l_brain_GetEconomyStored(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->econ.stored[ResourceArg(L, 2)]);
  return 1;
}
int l_brain_GetEconomyStoredRatio(lua_State* L) {
  ArmyEconomy& e = Brain(L)->army->econ;
  int r = ResourceArg(L, 2);
  float max = static_cast<float>(e.maxStorage[r]);
  lua_pushnumber(L, max > 0 ? e.stored[r] / max : 0.0f);
  return 1;
}
int l_brain_GetEconomyIncome(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->econ.incomeStat[ResourceArg(L, 2)]);
  return 1;
}
int l_brain_GetEconomyUsage(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->econ.usage[ResourceArg(L, 2)]);
  return 1;
}
int l_brain_GetEconomyRequested(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->econ.requested[ResourceArg(L, 2)]);
  return 1;
}
int l_brain_GetEconomyTrend(lua_State* L) {
  ArmyEconomy& e = Brain(L)->army->econ;
  int r = ResourceArg(L, 2);
  lua_pushnumber(L, e.incomeStat[r] - e.usage[r]);
  return 1;
}
// GiveResource(type, amount): added to the income of the next share-out
int l_brain_GiveResource(lua_State* L) {
  ArmyEconomy& e = Brain(L)->army->econ;
  e.income[ResourceArg(L, 2)] += static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
// taken = TakeResource(type, amount): straight from what is stored
int l_brain_TakeResource(lua_State* L) {
  ArmyEconomy& e = Brain(L)->army->econ;
  int r = ResourceArg(L, 2);
  float want = static_cast<float>(luaL_checknumber(L, 3));
  float take = want < e.stored[r] ? want : e.stored[r];
  float left = e.stored[r] - take;
  e.stored[r] = left > 0 ? left : 0;
  lua_pushnumber(L, take);
  return 1;
}
// GiveStorage(type, amount): the army's maximum storage (whole numbers)
int l_brain_GiveStorage(lua_State* L) {
  ArmyEconomy& e = Brain(L)->army->econ;
  int r = ResourceArg(L, 2);
  e.maxStorage[r] += static_cast<int64_t>(static_cast<float>(luaL_checknumber(L, 3)));
  return 0;
}
int l_brain_SetResourceSharing(lua_State* L) {
  Brain(L)->army->econ.sharing = lua_toboolean(L, 2) != 0;
  return 0;
}
// SetArmyEconomy(army, mass, energy): added to the next share-out's income
int l_SetArmyEconomy(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->econ.income[kMass] += static_cast<float>(luaL_checknumber(L, 2));
  a->econ.income[kEnergy] += static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}

template <int R, bool Prod>
int l_SetRate(lua_State* L) {
  Unit* u = U(L);
  float v = static_cast<float>(luaL_checknumber(L, 2));
  if (v < 0) v = 0;
  if (Prod) {
    u->production[R] = v;
  } else {
    u->consumption[R] = v;
    if (u->consumptionActive) ApplyConsumption(u, true);
  }
  return 0;
}
template <int R, bool Prod>
int l_GetRate(lua_State* L) {
  Unit* u = U(L);
  lua_pushnumber(L, Prod ? u->production[R] : u->consumption[R]);
  return 1;
}
int l_SetConsumptionActive(lua_State* L) {
  ApplyConsumption(U(L), lua_toboolean(L, 2) != 0);
  return 0;
}
int l_SetProductionActive(lua_State* L) {
  Unit* u = U(L);
  bool on = lua_toboolean(L, 2) != 0;
  u->productionActive = on;
  S(L)->CallMethod(L, u, on ? "OnProductionActive" : "OnProductionInActive", 0);
  return 0;
}
int l_GetResourceConsumed(lua_State* L) {
  lua_pushnumber(L, U(L)->resourceConsumed);
  return 1;
}
int l_SetBuildRate(lua_State* L) {
  U(L)->buildRate = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_GetBuildRate(lua_State* L) {
  lua_pushnumber(L, U(L)->buildRate);
  return 1;
}
int l_SetRegenRate(lua_State* L) {
  U(L)->regenRate = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_RevertRegenRate(lua_State* L) {
  Unit* u = U(L);
  u->regenRate = u->bpData ? u->bpData->regenRate : 0;
  return 0;
}
int l_SetWorkProgress(lua_State* L) {
  U(L)->workProgress = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_GetWorkProgress(lua_State* L) {
  lua_pushnumber(L, U(L)->workProgress);
  return 1;
}

// event = CreateEconomyEvent(unit, energy, mass, seconds [, callback(unit, progress)])
int l_CreateEconomyEvent(lua_State* L) {
  Unit* u = U(L);
  float energy = static_cast<float>(luaL_checknumber(L, 2));
  float mass = static_cast<float>(luaL_checknumber(L, 3));
  float seconds = static_cast<float>(luaL_checknumber(L, 4));
  auto ev = std::make_unique<EconomyEvent>();
  ev->unit = u;
  int ticks = static_cast<int>(seconds * 10.0f);
  if (ticks < 1) ticks = 1;
  ev->ticksLeft = ev->ticksTotal = ticks;
  float k = 1.0f / static_cast<float>(ticks);
  ev->perTick[kEnergy] = k * energy;
  ev->perTick[kMass] = k * mass;
  if (lua_isfunction(L, 5)) {
    lua_pushvalue(L, 5);
    ev->callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  if (u->army) {
    ev->request = u->army->econ.NewRequest();
    ev->request->requested[kEnergy] = ev->perTick[kEnergy];
    ev->request->requested[kMass] = ev->perTick[kMass];
  }
  u->econEvents.push_back(ev.get());
  CreateObject(L, ev.get(), "CEconomyEvent");
  S(L)->Own(std::move(ev));
  return 1;
}
// RemoveEconomyEvent(unit, event): its unused grant goes back to the army
int l_RemoveEconomyEvent(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  EconomyEvent* ev = ToObject<EconomyEvent>(L, 2);
  if (!ev) return 0;
  if (ev->request) {
    if (u && u->army)
      for (int i = 0; i < 2; ++i) u->army->econ.income[i] += ev->request->granted[i];
    ev->request->live = false;
    ev->request.reset();
  }
  if (ev->unit) {
    auto& v = ev->unit->econEvents;
    v.erase(std::remove(v.begin(), v.end(), ev), v.end());
    ev->unit->consumptionShown[0] = ev->unit->consumptionShown[1] = 0;
  }
  ev->unit = nullptr;
  ev->UnbindLua();
  return 0;
}
int l_EconomyEventIsDone(lua_State* L) {
  EconomyEvent* ev = ToObject<EconomyEvent>(L, 1);
  lua_pushboolean(L, !ev || ev->done);
  return 1;
}

}  // namespace

void RegisterEconomyBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "GetEconomyStored", l_brain_GetEconomyStored);
  SetMethod(L, "CAiBrain", "GetEconomyStoredRatio", l_brain_GetEconomyStoredRatio);
  SetMethod(L, "CAiBrain", "GetEconomyIncome", l_brain_GetEconomyIncome);
  SetMethod(L, "CAiBrain", "GetEconomyUsage", l_brain_GetEconomyUsage);
  SetMethod(L, "CAiBrain", "GetEconomyRequested", l_brain_GetEconomyRequested);
  SetMethod(L, "CAiBrain", "GetEconomyTrend", l_brain_GetEconomyTrend);
  SetMethod(L, "CAiBrain", "GiveResource", l_brain_GiveResource);
  SetMethod(L, "CAiBrain", "TakeResource", l_brain_TakeResource);
  SetMethod(L, "CAiBrain", "GiveStorage", l_brain_GiveStorage);
  SetMethod(L, "CAiBrain", "SetResourceSharing", l_brain_SetResourceSharing);
  SetGlobal(L, "SetArmyEconomy", l_SetArmyEconomy);
  SetGlobal(L, "CreateEconomyEvent", l_CreateEconomyEvent);
  SetGlobal(L, "RemoveEconomyEvent", l_RemoveEconomyEvent);
  SetGlobal(L, "EconomyEventIsDone", l_EconomyEventIsDone);
  SetMethod(L, "Unit", "SetConsumptionPerSecondEnergy", l_SetRate<kEnergy, false>);
  SetMethod(L, "Unit", "SetConsumptionPerSecondMass", l_SetRate<kMass, false>);
  SetMethod(L, "Unit", "SetProductionPerSecondEnergy", l_SetRate<kEnergy, true>);
  SetMethod(L, "Unit", "SetProductionPerSecondMass", l_SetRate<kMass, true>);
  SetMethod(L, "Unit", "GetConsumptionPerSecondEnergy", l_GetRate<kEnergy, false>);
  SetMethod(L, "Unit", "GetConsumptionPerSecondMass", l_GetRate<kMass, false>);
  SetMethod(L, "Unit", "GetProductionPerSecondEnergy", l_GetRate<kEnergy, true>);
  SetMethod(L, "Unit", "GetProductionPerSecondMass", l_GetRate<kMass, true>);
  SetMethod(L, "Unit", "SetConsumptionActive", l_SetConsumptionActive);
  SetMethod(L, "Unit", "SetProductionActive", l_SetProductionActive);
  SetMethod(L, "Unit", "GetResourceConsumed", l_GetResourceConsumed);
  SetMethod(L, "Unit", "SetBuildRate", l_SetBuildRate);
  SetMethod(L, "Unit", "GetBuildRate", l_GetBuildRate);
  SetMethod(L, "Unit", "SetRegenRate", l_SetRegenRate);
  SetMethod(L, "Unit", "RevertRegenRate", l_RevertRegenRate);
  SetMethod(L, "Unit", "SetWorkProgress", l_SetWorkProgress);
  SetMethod(L, "Unit", "GetWorkProgress", l_GetWorkProgress);
}

}  // namespace moho
