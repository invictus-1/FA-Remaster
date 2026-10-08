// Building: where structures may go, and the tasks that carry out build-like commands (the
// original's CUnitMobileBuildTask, CFactoryBuildTask, CUnitUpgradeTask, CUnitRepairTask and the
// CBuildTaskHelper they share).
//
// What the original does (FA exe, read 2026-10-08; FINDINGS "Rebuild M4"):
// - A structure's footprint origin is round-half-even(position - size/2); its footprint covers
//   [origin, origin + size) and its skirt [origin + SkirtOffset, ... + SkirtSize) (the footprint
//   when SkirtSize is 0). OCCUPY_Check (0x5652e0): the skirt lies inside the map; the terrain under
//   it is flat enough (height range <= MaxGroundVariation over the skirt vertices, or over the
//   ring around it with FlattenSkirt) for the land layers; land is lost where the ground is under
//   water, water layers where the ground is above water - MinWaterDepth; extractors need a deposit
//   of their kind under their footprint, other structures may not touch one.
// - CAiBrain::CanBuildStructureAt (0x57cbb0): the location is free (OCCUPY_Check and no structure
//   occupying the footprint) and no structure's footprint overlaps the new skirt.
// - Unit::CanBuild (0x6a9e50): the target is in the builder's Economy.BuildableCategory, minus the
//   unit's and the army's build restrictions.
// - A builder works on its focus every tick (CBuildTaskHelper::UpdateWorkProgress 0x5f5bf0):
//   delta = 1 / (BuildTime / BuildRate) * GetResourceConsumed() * 0.1, then Unit::Materialize(delta)
//   adds it to the target's fraction complete and maxHealth * delta to its health. OnBuildProgress /
//   OnBeingBuiltProgress fire when the fraction crosses 0.25, 0.5 and 0.75; the work is done at 1.
//   Repair heals a finished unit the same way until its health is full.
// - Mobile builders (engineers) first drive until distance - their footprint size - the target's
//   skirt size <= MaxBuildDistance; the new unit is created with health 1 and OnStartBuild(unit,
//   'MobileBuild') runs on the builder; a unit of the same kind already being built there is joined
//   instead. Factories create the unit at their own position ('FactoryBuild'); the finished unit
//   takes over the factory's other commands (its rally point: a move to InitialRallyX/Z turned to
//   the factory's heading, set when the factory is finished). 'Upgrade' builds the upgrade in
//   place; the scripts remove the old unit.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "core/dmath.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/commands.h"
#include "sim/economy.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }

bool Alive(const Entity* e) { return e && !e->dead && !e->destroyQueued; }

Unit* FindUnit(Sim& sim, uint32_t id) {
  if (!id) return nullptr;
  Entity* e = sim.FindEntity(id);
  return e && e->kind == Entity::Kind::Unit && Alive(e) ? static_cast<Unit*>(e) : nullptr;
}

const NamedFootprint& Footprint(const BlueprintInfo& bp) {
  static NamedFootprint one{"", 1, 1, 1, 0, 0, 0, 0};
  return bp.hasFootprint ? bp.footprint : one;
}

struct FRect {
  float x0, z0, x1, z1;
};

FRect SkirtRect(const BlueprintInfo& bp, const UnitBpData& d, float x, float z) {
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  FRect r;
  if (d.skirtSizeX == 0.0f) {
    r.x0 = static_cast<float>(ox);
    r.x1 = static_cast<float>(ox + fp.sizeX);
  } else {
    r.x0 = d.skirtOffsetX + static_cast<float>(ox);
    r.x1 = r.x0 + d.skirtSizeX;
  }
  if (d.skirtSizeZ == 0.0f) {
    r.z0 = static_cast<float>(oz);
    r.z1 = static_cast<float>(oz + fp.sizeZ);
  } else {
    r.z0 = d.skirtOffsetZ + static_cast<float>(oz);
    r.z1 = r.z0 + d.skirtSizeZ;
  }
  return r;
}

int DepositType(const std::string& restriction) {
  if (restriction == "RULEUBR_OnMassDeposit") return 1;
  if (restriction == "RULEUBR_OnHydrocarbonDeposit") return 2;
  return 0;
}

bool DepositOverlaps(const Sim& sim, int type, float x0, float z0, float x1, float z1) {
  for (const auto& d : sim.deposits)
    if (d.type == type && x0 < static_cast<float>(d.x1) && static_cast<float>(d.x0) < x1 &&
        z0 < static_cast<float>(d.z1) && static_cast<float>(d.z0) < z1)
      return true;
  return false;
}

// OCCUPY_Check for a structure blueprint at (x, z): the layers it could be built on (0 = none).
int StructureLayers(Sim& sim, const BlueprintInfo& bp, const UnitBpData& d, float x, float z) {
  const TerrainMap* map = sim.map();
  if (!map) return 0;
  FRect sk = SkirtRect(bp, d, x, z);
  int x0 = static_cast<int>(std::floor(sk.x0)), z0 = static_cast<int>(std::floor(sk.z0));
  int x1 = static_cast<int>(std::ceil(sk.x1)), z1 = static_cast<int>(std::ceil(sk.z1));
  if (x0 < 0 || z0 < 0 || x1 > map->width() - 1 || z1 > map->height() - 1) return 0;
  int caps = Footprint(bp).caps ? Footprint(bp).caps : d.buildOnLayerCaps;
  // flatness: the skirt's vertices, or the ring just outside it (FlattenSkirt)
  float lo = 1e30f, hi = -1e30f;
  auto take = [&](int vx, int vz) {
    vx = std::clamp(vx, 0, map->width());
    vz = std::clamp(vz, 0, map->height());
    float h = map->HeightAt(vx, vz);
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  };
  if (!d.flattenSkirt) {
    for (int vz = z0; vz <= z1; ++vz)
      for (int vx = x0; vx <= x1; ++vx) take(vx, vz);
  } else {
    for (int vx = x0 - 1; vx <= x1 + 1; ++vx) {
      take(vx, z0 - 1);
      take(vx, z1 + 1);
    }
    for (int vz = z0; vz <= z1; ++vz) {
      take(x0 - 1, vz);
      take(x1 + 1, vz);
    }
  }
  if (hi - lo > d.maxGroundVariation) caps &= ~3;
  float water = map->hasWater ? map->waterElevation : -10000.0f;
  if (water > lo) caps &= ~1;
  if (hi > water - Footprint(bp).minWaterDepth) caps &= ~0xe;
  if (!caps) return 0;
  // blocking terrain types under the footprint
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  Navigation& nav = sim.navigation();
  for (int cz = oz; cz < oz + fp.sizeZ; ++cz)
    for (int cx = ox; cx < ox + fp.sizeX; ++cx)
      if (nav.IsBlockingType(map->TerrainType(cx, cz))) return 0;
  // deposits: extractors need theirs; nothing else may touch one
  int dep = DepositType(d.buildRestriction);
  if (dep) {
    if (!DepositOverlaps(sim, dep, static_cast<float>(ox), static_cast<float>(oz), static_cast<float>(ox + fp.sizeX),
                         static_cast<float>(oz + fp.sizeZ)))
      return 0;
  } else if (d.buildRestriction.empty() || d.buildRestriction == "RULEUBR_None") {
    if (DepositOverlaps(sim, 1, sk.x0, sk.z0, sk.x1, sk.z1) || DepositOverlaps(sim, 2, sk.x0, sk.z0, sk.x1, sk.z1))
      return 0;
  }
  return caps;
}

}  // namespace

// ---- placement --------------------------------------------------------------------------------

Vec3 SnapStructurePosition(Sim& sim, const BlueprintInfo& bp, Vec3 p) {
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(p.x - fp.sizeX * 0.5f), oz = RoundEven(p.z - fp.sizeZ * 0.5f);
  Vec3 r{ox + fp.sizeX * 0.5f, p.y, oz + fp.sizeZ * 0.5f};
  if (sim.map()) r.y = sim.map()->SurfaceHeight(r.x, r.z);
  return r;
}

bool LocationIsFree(Sim& sim, const BlueprintInfo& bp, float x, float z) {
  lua_State* L = sim.L();
  const UnitBpData& d = GetUnitBpData(L, bp);
  if (!StructureLayers(sim, bp, d, x, z)) return false;
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  return !sim.navigation().AnyStructureIn(ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}

bool CanBuildStructureAt(Sim& sim, const BlueprintInfo& bp, float x, float z) {
  if (!LocationIsFree(sim, bp, x, z)) return false;
  const UnitBpData& d = GetUnitBpData(sim.L(), bp);
  FRect sk = SkirtRect(bp, d, x, z);
  for (const auto& r : sim.navigation().Structures())
    if (sk.x0 < static_cast<float>(r.x1) && static_cast<float>(r.x0) < sk.x1 && sk.z0 < static_cast<float>(r.z1) &&
        static_cast<float>(r.z0) < sk.z1)
      return false;
  return true;
}

bool UnitCanBuild(const Unit* u, const BlueprintInfo& bp) {
  if (!u->bpData || bp.entityIndex < 0) return false;
  size_t w = static_cast<size_t>(bp.entityIndex) >> 6;
  uint64_t bit = uint64_t(1) << (bp.entityIndex & 63);
  if (w >= u->bpData->buildable.size() || !(u->bpData->buildable[w] & bit)) return false;
  if (w < u->buildAllowed.size() && !(u->buildAllowed[w] & bit)) return false;
  if (u->army && w < u->army->buildRestricted.size() && (u->army->buildRestricted[w] & bit)) return false;
  return true;
}

// A structure enters / leaves the occupancy grid.
void OccupyStructure(Sim& sim, Unit* u) {
  if (!u->bpData || !u->bpData->structure || !u->blueprint) return;
  const NamedFootprint& fp = Footprint(*u->blueprint);
  int ox = RoundEven(u->position.x - fp.sizeX * 0.5f), oz = RoundEven(u->position.z - fp.sizeZ * 0.5f);
  sim.navigation().AddStructure(u->id, ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}
void ReleaseStructure(Sim& sim, Unit* u) {
  if (u->bpData && u->bpData->structure) sim.navigation().RemoveStructure(u->id);
}

// ---- build helper (CBuildTaskHelper) -----------------------------------------------------------

namespace {

void SetFocus(Sim& sim, lua_State* L, Unit* builder, Unit* target, BuildTask& t);

// OnStopBuild(success): the builder's OnStopBuild(target, order) (and on failure OnFailedToBuild /
// the target's OnFailedToBeBuilt first); the builder loses its focus.
void StopBuild(Sim& sim, lua_State* L, Unit* builder, BuildTask& t, bool success) {
  Unit* target = FindUnit(sim, t.targetId);
  if (t.started && Alive(builder)) {
    if (!success) {
      sim.CallMethod(L, builder, "OnFailedToBuild", 0);
      if (target) sim.CallMethod(L, target, "OnFailedToBeBuilt", 0);
    }
    PushObject(L, target);
    lua_pushstring(L, t.order.c_str());
    sim.CallMethod(L, builder, "OnStopBuild", 2);
  }
  if (builder->focusId == t.targetId) builder->focusId = 0;
  t.started = false;
}

void SetFocus(Sim& sim, lua_State* L, Unit* builder, Unit* target, BuildTask& t) {
  if (t.started && t.targetId == target->id) return;
  if (t.started) StopBuild(sim, L, builder, t, false);
  t.targetId = target->id;
  builder->focusId = target->id;
  t.started = true;
  t.lastFraction = target->fractionComplete;
  PushObject(L, target);
  lua_pushstring(L, t.order.c_str());
  sim.CallMethod(L, builder, "OnStartBuild", 2);
}

// One tick of work on the focus. Returns true when the work is done.
bool UpdateWorkProgress(Sim& sim, lua_State* L, Unit* builder, BuildTask& t) {
  Unit* target = FindUnit(sim, t.targetId);
  if (!target) return true;
  if (builder->paused) {  // (FAF patch 0x1291a27) a paused builder only shows the target's progress
    builder->workProgress = target->fractionComplete;
    return false;
  }
  float rate = builder->resourceConsumed;
  // assisting an enhancement (the target works on a script work item)
  if (target->unitStates.count("Enhancing")) {
    PushObjectValue(L, target, "WorkProgress");
    lua_pop(L, 1);
    builder->workProgress = target->workProgress;
    return target->workProgress >= 1.0f;
  }
  const UnitBpData& td = *target->bpData;
  float delta = 0;
  if (rate != 0.0f && td.buildTime > 0.0f && builder->buildRate > 0.0f)
    delta = 1.0f / (td.buildTime / builder->buildRate) * rate * 0.1f;
  Materialize(sim, L, target, delta);
  if (!Alive(target)) return true;
  if (t.order == "Repair" && !target->beingBuilt) {
    float f = target->maxHealth > 0 ? target->health / target->maxHealth : 1.0f;
    builder->workProgress = f;
    return f == 1.0f;
  }
  float f = target->fractionComplete, old = t.lastFraction;
  for (float q : {0.25f, 0.5f, 0.75f})
    if (old < q && q <= f) {
      PushObject(L, target);
      lua_pushnumber(L, old);
      lua_pushnumber(L, f);
      sim.CallMethod(L, builder, "OnBuildProgress", 3);
      PushObject(L, builder);
      lua_pushnumber(L, old);
      lua_pushnumber(L, f);
      sim.CallMethod(L, target, "OnBeingBuiltProgress", 3);
      break;
    }
  t.lastFraction = f;
  builder->workProgress = f;
  return f == 1.0f;
}

// Distance check of the build tasks: distance - builder footprint - target skirt <= MaxBuildDistance.
bool InBuildRange(const Unit* builder, const BlueprintInfo& targetBp, const Vec3& site) {
  const UnitBpData& td = GetUnitBpData(builder->luaState(), targetBp);
  float dx = builder->position.x - site.x, dz = builder->position.z - site.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  const NamedFootprint& bf = Footprint(*builder->blueprint);
  float own = static_cast<float>(std::max(bf.sizeX, bf.sizeZ));
  float skirt = std::max(td.skirtSizeX, td.skirtSizeZ);
  return dist - own - skirt <= builder->bpData->maxBuildDistance;
}

bool CanMove(const Unit* u) { return u->motion.bp && u->motion.bp->mobile() && !u->immobile; }

void MoveToward(Sim& sim, Unit* u, const Vec3& goal) {
  std::vector<Vec3> path;
  if (u->motion.bp->motionType == kMotionAir) path.push_back(goal);
  else if (!sim.navigation().FindPath(u->motion.bp->footprint, u->position, goal, &path) || path.empty()) {
    u->motion.failed = true;
    return;
  }
  u->motion.failed = false;
  MotionSetGoal(sim, u, path, false, sim.tick() + 3);
  u->unitStates.insert("Moving");
}

void StopMoving(Unit* u) {
  if (u->motion.hasGoal) MotionStop(u);
  u->unitStates.erase("Moving");
}

// A unit of the same kind being built at the site (join it instead of starting another).
Unit* ExistingAtSite(Sim& sim, const Unit* builder, const BlueprintInfo& bp, const Vec3& site) {
  Unit* found = nullptr;
  sim.ForUnitsInRect(site.x - 0.5f, site.z - 0.5f, site.x + 0.5f, site.z + 0.5f, [&](Unit* u) {
    if (found || !Alive(u) || u->blueprint != &bp || !u->beingBuilt) return;
    if (u->army != builder->army) return;
    found = u;
  });
  return found;
}

bool UnderUnitCap(const Army* a, const UnitBpData& d) {
  return !a || a->ignoreUnitCap || a->unitCost + d.capCost <= a->unitCap;
}

}  // namespace

// ---- tasks ---------------------------------------------------------------------------------------

namespace {

int TickMobileBuild(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  switch (t.state) {
    case 0:
      if (!t.bp || !UnitCanBuild(u, *t.bp)) return kTaskFailed;
      t.site = SnapStructurePosition(sim, *t.bp, t.site);
      if (CanMove(u) && !InBuildRange(u, *t.bp, t.site)) MoveToward(sim, u, t.site);
      t.state = 1;
      return kTaskRunning;
    case 1:
      if (!InBuildRange(u, *t.bp, t.site)) {
        if (!CanMove(u)) return kTaskFailed;
        if (u->motion.failed || (!u->motion.hasGoal && u->motion.arrived)) {
          u->motion.arrived = false;
          if (!u->motion.failed) MoveToward(sim, u, t.site);
          if (u->motion.failed) return kTaskFailed;
        } else if (!u->motion.hasGoal) {
          MoveToward(sim, u, t.site);
          if (u->motion.failed) return kTaskFailed;
        }
        return kTaskRunning;
      }
      StopMoving(u);
      u->unitStates.insert("Building");
      t.state = 3;  // (2: turning to face the site; NeedToFaceTargetToBuild units turn on the spot)
      return kTaskRunning;
    case 3: {
      if (sim.tick() < t.waitUntil) return kTaskRunning;
      if (Unit* ex = ExistingAtSite(sim, u, *t.bp, t.site)) {
        SetFocus(sim, L, u, ex, t);
        t.state = 4;
        return kTaskRunning;
      }
      if (!LocationIsFree(sim, *t.bp, t.site.x, t.site.z)) return kTaskFailed;
      const UnitBpData& d = GetUnitBpData(L, *t.bp);
      if (!UnderUnitCap(u->army, d)) {
        if (++t.tries > 10) return kTaskFailed;
        t.waitUntil = sim.tick() + 10;
        return kTaskRunning;
      }
      float h = dmath::Atan2(u->position.x - t.site.x, u->position.z - t.site.z);  // (structures face the builder? kept level)
      (void)h;
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, t.site, Quat{}, false, u);
      if (!nu || !Alive(nu)) return kTaskFailed;
      nu->unitStates.insert("NoReclaim");
      SetFocus(sim, L, u, nu, t);
      t.state = 4;
      return kTaskRunning;
    }
    case 4: {
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) return kTaskFailed;
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      target = FindUnit(sim, t.targetId);
      if (target) target->unitStates.erase("NoReclaim");
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

void SetUpInitialRally(Sim& sim, Unit* f);

int TickFactoryBuild(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  switch (t.state) {
    case 0: {
      if (!t.bp || !UnitCanBuild(u, *t.bp)) return kTaskFailed;
      if (u->busy) return kTaskRunning;
      const UnitBpData& d = GetUnitBpData(L, *t.bp);
      if (!UnderUnitCap(u->army, d)) {
        if (++t.tries > 10) return kTaskFailed;
        return kTaskRunning;
      }
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, u->position, u->orientation, false, u);
      if (!nu || !Alive(nu)) return kTaskFailed;
      SetFocus(sim, L, u, nu, t);
      u->unitStates.insert("Building");
      t.state = 1;
      [[fallthrough]];
    }
    case 1: {
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) {
        StopBuild(sim, L, u, t, false);
        t.state = 0;
        return kTaskRunning;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      target = FindUnit(sim, t.targetId);
      StopBuild(sim, L, u, t, true);
      u->unitStates.erase("Building");
      u->workProgress = 0;
      if (target) InheritFactoryCommands(sim, u, target);
      t.state = 3;
      return kTaskRunning;
    }
    case 3:
      if (u->busy) return kTaskRunning;  // roll-off: the scripts keep the factory busy
      return kTaskDone;
  }
  return kTaskFailed;
}

int TickUpgrade(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  switch (t.state) {
    case 0: {
      if (!t.bp) return kTaskFailed;
      u->unitStates.insert("Upgrading");
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, u->position, u->orientation, false, u);
      if (!nu || !Alive(nu)) {
        u->unitStates.erase("Upgrading");
        return kTaskFailed;
      }
      nu->unitStates.insert("BeingUpgraded");
      SetFocus(sim, L, u, nu, t);
      t.state = 1;
      return kTaskRunning;
    }
    case 1: {
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) {
        StopBuild(sim, L, u, t, false);
        u->unitStates.erase("Upgrading");
        return kTaskFailed;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      u->unitStates.erase("Upgrading");
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

// Repair / assist: work on a damaged or unfinished unit (CUnitRepairTask 0x5f9370).
int TickRepair(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Unit* target = FindUnit(sim, t.targetId ? t.targetId : t.goalId);
  if (!target || target == u) return kTaskFailed;
  if (!target->beingBuilt && target->health >= target->maxHealth && t.state < 3) return kTaskDone;
  switch (t.state) {
    case 0:
      if (!InBuildRange(u, *target->blueprint, target->position)) {
        if (!CanMove(u)) return kTaskFailed;
        MoveToward(sim, u, target->position);
        if (u->motion.failed) return kTaskFailed;
      }
      t.state = 1;
      return kTaskRunning;
    case 1:
      if (!InBuildRange(u, *target->blueprint, target->position)) {
        if (!CanMove(u) || u->motion.failed) return kTaskFailed;
        if (!u->motion.hasGoal) MoveToward(sim, u, target->position);
        return kTaskRunning;
      }
      StopMoving(u);
      t.state = 3;
      return kTaskRunning;
    case 3:
      SetFocus(sim, L, u, target, t);
      u->unitStates.insert("Repairing");
      t.state = 4;
      return kTaskRunning;
    case 4: {
      if (!InBuildRange(u, *target->blueprint, target->position)) {
        StopBuild(sim, L, u, t, false);
        u->unitStates.erase("Repairing");
        t.state = 0;
        return kTaskRunning;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      u->unitStates.erase("Repairing");
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

// Guard / assist: follow the guarded unit; work on what it builds or repairs, or repair it.
int TickGuard(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Unit* g = FindUnit(sim, t.goalId);
  if (!g) {
    if (t.started) StopBuild(sim, L, u, t, true);
    u->unitStates.erase("Repairing");
    return kTaskDone;
  }
  u->guardedId = g->id;
  bool builder = u->bpData && u->bpData->hasBuilder;
  Unit* work = nullptr;
  if (builder) {
    Unit* f = FindUnit(sim, g->focusId);
    if (f && (f->beingBuilt || f->health < f->maxHealth)) work = f;
    else if (g->health < g->maxHealth && !g->beingBuilt) work = g;
  }
  if (work) {
    if (!InBuildRange(u, *work->blueprint, work->position)) {
      if (t.started) {
        StopBuild(sim, L, u, t, true);
        u->unitStates.erase("Repairing");
      }
      if (CanMove(u) && !u->motion.hasGoal) MoveToward(sim, u, work->position);
      return kTaskRunning;
    }
    StopMoving(u);
    if (!t.started || t.targetId != work->id) {
      t.order = "Repair";
      SetFocus(sim, L, u, work, t);
      u->unitStates.insert("Repairing");
    }
    if (UpdateWorkProgress(sim, L, u, t)) {
      StopBuild(sim, L, u, t, true);
      u->unitStates.erase("Repairing");
    }
    return kTaskRunning;
  }
  if (t.started) {
    StopBuild(sim, L, u, t, true);
    u->unitStates.erase("Repairing");
  }
  // stay near the guarded unit
  if (CanMove(u)) {
    float dx = g->position.x - u->position.x, dz = g->position.z - u->position.z;
    float keep = 4.0f + (g->bpData ? std::max(g->bpData->sizeX, g->bpData->sizeZ) : 1.0f);
    if (dx * dx + dz * dz > keep * keep && !u->motion.hasGoal) MoveToward(sim, u, g->position);
  }
  return kTaskRunning;
}

// Reclaim (CUnitReclaimTask 0x61f000): GetReclaimCosts(reclaimer) on the target gives time,
// energy and mass; a prop loses 1/(time*10) of itself per tick (scaled by the reclaimer's
// resource fraction) and yields that share of its value to the army's income and reclaim stat;
// a unit loses that share of its maximum health. At nothing left the target's OnReclaimed runs.
int TickReclaim(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Entity* e = t.goalId ? sim.FindEntity(t.goalId) : nullptr;
  if (!e || e->destroyQueued || e->dead || e == u) return t.state >= 2 ? kTaskDone : kTaskFailed;
  Unit* tu = e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
  if (tu && tu->unitStates.count("NoReclaim")) return kTaskFailed;
  // range: like building, with the target's size
  auto inRange = [&]() {
    float sx = 1, sz = 1;
    if (tu && tu->bpData) {
      sx = tu->bpData->sizeX;
      sz = tu->bpData->sizeZ;
    } else if (e->blueprint) {
      int top = lua_gettop(L);
      sim.blueprints().PushTable(L, *e->blueprint);
      lua_pushstring(L, "SizeX");
      lua_rawget(L, -2);
      if (lua_isnumber(L, -1)) sx = static_cast<float>(lua_tonumber(L, -1));
      lua_pushstring(L, "SizeZ");
      lua_rawget(L, -3);
      if (lua_isnumber(L, -1)) sz = static_cast<float>(lua_tonumber(L, -1));
      lua_settop(L, top);
    }
    float dx = u->position.x - e->position.x, dz = u->position.z - e->position.z;
    const NamedFootprint& bf = Footprint(*u->blueprint);
    float d = std::sqrt(dx * dx + dz * dz) - static_cast<float>(std::max(bf.sizeX, bf.sizeZ)) -
              std::max(sx * e->scale[0], sz * e->scale[2]) * 0.5f;
    return d <= u->bpData->maxBuildDistance;
  };
  switch (t.state) {
    case 0: {
      // costs
      int top = lua_gettop(L);
      PushObject(L, e);
      int obj = lua_gettop(L);
      lua_pushstring(L, "GetReclaimCosts");
      lua_gettable(L, obj);
      if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return kTaskFailed;
      }
      lua_pushcfunction(L, ScriptTraceback);
      lua_insert(L, -2);
      lua_pushvalue(L, obj);
      PushObject(L, u);
      if (lua_pcall(L, 2, 3, top + 2) != 0) {
        LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_settop(L, top);
        return kTaskFailed;
      }
      float time = static_cast<float>(lua_tonumber(L, -3)), energy = static_cast<float>(lua_tonumber(L, -2)),
            mass = static_cast<float>(lua_tonumber(L, -1));
      lua_settop(L, top);
      float ticks = time * 10.0f;
      if (ticks < 1.0f) ticks = 1.0f;
      float step = 1.0f / ticks;
      t.reclaimStep = step;
      if (energy < 0 || mass < 0) return kTaskFailed;
      t.reclaimPerTick[kEnergy] = energy * step;
      t.reclaimPerTick[kMass] = mass * step;
      u->unitStates.insert("Reclaiming");
      u->focusId = e->id;
      t.state = 1;
      [[fallthrough]];
    }
    case 1:
      if (!inRange()) {
        if (!CanMove(u)) return kTaskFailed;
        if (t.reclaimStarted) {
          t.reclaimStarted = false;
          PushObject(L, e);
          sim.CallMethod(L, u, "OnStopReclaim", 1);
        }
        if (u->motion.failed) return kTaskFailed;
        if (!u->motion.hasGoal) MoveToward(sim, u, e->position);
        if (u->motion.failed) return kTaskFailed;
        return kTaskRunning;
      }
      StopMoving(u);
      t.state = 2;
      [[fallthrough]];
    case 2: {
      if (!inRange()) {
        t.state = 1;
        return kTaskRunning;
      }
      if (!t.reclaimStarted) {
        t.reclaimStarted = true;
        PushObject(L, e);
        sim.CallMethod(L, u, "OnStartReclaim", 1);
        if (!Alive(e)) return kTaskDone;
      }
      if (u->paused) return kTaskRunning;
      float rate = u->request ? u->request->LimitingRate() : 1.0f;
      float taken = 0;
      if (tu) {  // a unit: health
        float dmg = tu->maxHealth * t.reclaimStep * rate;
        if (tu->health <= dmg + tu->regenRate * 0.1f) {
          taken = t.reclaimStep;
          PushObject(L, u);
          sim.CallMethod(L, tu, "OnReclaimed", 1);
          if (Alive(tu)) sim.QueueDestroy(tu);
        } else {
          EntityAdjustHealth(L, tu, u, -dmg);
          taken = t.reclaimStep * rate;
        }
      } else {  // a prop: its fraction, with health following it
        float old = e->fractionComplete;
        float f = std::max(0.0f, old - t.reclaimStep * rate);
        e->fractionComplete = f;
        taken = old - f;
        float h = e->maxHealth * f;
        if (h != e->health) EntityAdjustHealth(L, e, u, h - e->health);
        u->workProgress = 1.0f - f;
      }
      if (u->army && t.reclaimStep > 0) {
        float k = taken / t.reclaimStep;
        for (int i = 0; i < 2; ++i) {
          float got = k * t.reclaimPerTick[i];
          u->army->econ.income[i] += got;
          u->army->econ.reclaimed[i] += got;
        }
      }
      if (!tu && e->fractionComplete <= 0.0f && !e->destroyQueued) {
        PushObject(L, u);
        sim.CallMethod(L, e, "OnReclaimed", 1);
        if (!e->destroyQueued) sim.QueueDestroy(e);
      }
      if (!Alive(e)) {
        if (t.reclaimStarted) {
          PushObject(L, e);
          sim.CallMethod(L, u, "OnStopReclaim", 1);
          t.reclaimStarted = false;
        }
        return kTaskDone;
      }
      return kTaskRunning;
    }
  }
  return kTaskFailed;
}

// Script tasks (CUnitScriptTask): the class is /lua/sim/tasks/<TaskName>.lua's <TaskName>
// (ScriptTask when missing); OnCreate(commandData) when created, then TaskTick() whenever due:
// -1 done, -2 suspend, -3 abort (both stay on the command), -4 delay, 0 again at once, n wait n
// ticks.
BuildTask* StartScriptTask(Sim& sim, Unit* u, const UnitCommand& c) {
  lua_State* L = sim.L();
  int top = lua_gettop(L);
  std::string name = "ScriptTask";
  if (c.scriptRef != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, c.scriptRef);
    lua_pushstring(L, "TaskName");
    lua_gettable(L, -2);
    if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
    lua_settop(L, top);
  }
  auto pushClass = [&](const std::string& module, const std::string& cls) -> bool {
    lua_getglobal(L, "import");
    lua_pushstring(L, module.c_str());
    if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
      lua_settop(L, top);
      return false;
    }
    lua_pushstring(L, cls.c_str());
    lua_gettable(L, -2);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) {
      lua_settop(L, top);
      return false;
    }
    return true;
  };
  if (!pushClass("/lua/sim/tasks/" + name + ".lua", name)) {
    Logf(LogLevel::Info, "Can't find task %s, using ScriptTask directly", name.c_str());
    if (!pushClass("/lua/sim/scripttask.lua", "ScriptTask")) return nullptr;
  }
  int cls = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushvalue(L, cls);
  if (lua_pcall(L, 0, 1, cls + 1) != 0 || !lua_istable(L, -1)) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "script task class did not create an object");
    lua_settop(L, top);
    return nullptr;
  }
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Script;
  t->scriptTask = true;
  t->unitId = u->id;
  t->order = name;
  BindObject(L, -1, t.get());
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  lua_settop(L, top);
  if (c.scriptRef != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, c.scriptRef);
  else lua_newtable(L);
  sim.CallMethod(L, r, "OnCreate", 1);
  return r;
}

int TickScriptTask(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  (void)u;
  if (sim.tick() < t.nextTick) return kTaskRunning;
  for (int guard = 0; guard < 64; ++guard) {
    if (!t.HasLuaObject()) return kTaskFailed;
    int top = lua_gettop(L);
    PushObject(L, &t);
    int obj = lua_gettop(L);
    lua_pushstring(L, "TaskTick");
    lua_gettable(L, obj);
    if (!lua_isfunction(L, -1)) {
      lua_settop(L, top);
      return kTaskDone;
    }
    lua_pushcfunction(L, ScriptTraceback);
    lua_insert(L, -2);
    lua_pushvalue(L, obj);
    int status = -1;
    if (lua_pcall(L, 1, 1, top + 2) == 0) {
      status = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : -1;
    } else {
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_settop(L, top);
    if (status == -1) return kTaskDone;
    if (status == -2 || status == -3) {
      t.nextTick = 0xffffffffu;  // stays on the command until it is cleared
      return kTaskRunning;
    }
    if (status == -4) {
      t.nextTick = sim.tick() + 1;
      return kTaskRunning;
    }
    if (status > 0) {
      t.nextTick = sim.tick() + static_cast<uint32_t>(status);
      return kTaskRunning;
    }
    // 0: again at once
  }
  t.nextTick = sim.tick() + 1;
  return kTaskRunning;
}

}  // namespace

BuildTask* StartBuildTask(Sim& sim, Unit* u, const UnitCommand& c) {
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->site = c.pos;
  switch (c.type) {
    case CommandType::BuildMobile:
      t->order = "MobileBuild";
      t->bp = sim.blueprints().Find(c.blueprintId);
      break;
    case CommandType::BuildFactory:
      t->order = "FactoryBuild";
      t->bp = sim.blueprints().Find(c.blueprintId);
      t->count = c.count;
      break;
    case CommandType::Upgrade:
      t->order = "Upgrade";
      t->bp = sim.blueprints().Find(c.blueprintId);
      break;
    case CommandType::Repair:
      if (!u->bpData || !u->bpData->hasBuilder) return nullptr;
      t->order = "Repair";
      t->goalId = c.targetId;
      break;
    case CommandType::Script:
      return StartScriptTask(sim, u, c);
    case CommandType::Reclaim:
      if (!u->bpData || !u->bpData->hasBuilder || !c.targetId) return nullptr;
      t->order = "Reclaim";
      t->goalId = c.targetId;
      break;
    case CommandType::Guard:
    case CommandType::BuildAssist:
    case CommandType::AssistCommander:
      if (!u->bpData || !u->bpData->hasBuilder) return nullptr;
      t->order = "Repair";
      t->goalId = c.targetId;
      u->unitStates.insert(c.type == CommandType::AssistCommander ? "AssistingCommander" : "Guarding");
      break;
    default:
      return nullptr;
  }
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int TickBuildTask(Sim& sim, Unit* u, BuildTask& t) {
  lua_State* L = sim.L();
  int r = kTaskFailed;
  switch (t.type) {
    case CommandType::BuildMobile: r = TickMobileBuild(sim, L, u, t); break;
    case CommandType::BuildFactory: r = TickFactoryBuild(sim, L, u, t); break;
    case CommandType::Upgrade: r = TickUpgrade(sim, L, u, t); break;
    case CommandType::Repair: r = TickRepair(sim, L, u, t); break;
    case CommandType::Reclaim: r = TickReclaim(sim, L, u, t); break;
    case CommandType::Script: r = TickScriptTask(sim, L, u, t); break;
    case CommandType::Guard:
    case CommandType::BuildAssist:
    case CommandType::AssistCommander: r = TickGuard(sim, L, u, t); break;
    default: break;
  }
  if (r != kTaskRunning) {
    static const bool dbg = getenv("MOHO64_DEBUG_TASKS") != nullptr;
    if (dbg)
      Logf(LogLevel::Debug, "moho64: task %d %s unit %u (%s) state %d bp %s -> %s", static_cast<int>(t.type),
           t.order.c_str(), u->id, u->blueprint ? u->blueprint->id.c_str() : "?", t.state, t.bp ? t.bp->id.c_str() : "-",
           r == kTaskDone ? "done" : "FAILED");
    EndBuildTask(sim, u, t, r == kTaskDone);
  }
  return r;
}

void EndBuildTask(Sim& sim, Unit* u, BuildTask& t, bool success) {
  lua_State* L = sim.L();
  if (t.ended) return;
  t.ended = true;
  if (t.scriptTask) {  // the task object's OnDestroy, then it is gone
    if (t.HasLuaObject()) sim.CallMethod(L, &t, "OnDestroy", 0);
    t.UnbindLua();
    return;
  }
  if (t.started && Alive(u)) StopBuild(sim, L, u, t, success);
  if (t.reclaimStarted && Alive(u)) {
    Entity* e = sim.FindEntity(t.goalId);
    PushObject(L, e && !e->destroyQueued ? e : nullptr);
    sim.CallMethod(L, u, "OnStopReclaim", 1);
    t.reclaimStarted = false;
  }
  if (Alive(u)) {
    u->unitStates.erase("Reclaiming");
    if (t.type == CommandType::Reclaim && u->focusId == t.goalId) u->focusId = 0;
    u->unitStates.erase("Building");
    u->unitStates.erase("Repairing");
    u->unitStates.erase("Guarding");
    u->unitStates.erase("AssistingCommander");
    if (t.type != CommandType::BuildFactory) u->workProgress = 0;
    StopMovingIfTask(u, t);
  }
  u->guardedId = 0;
}

void StopMovingIfTask(Unit* u, const BuildTask& t) {
  if (t.type == CommandType::BuildFactory || t.type == CommandType::Upgrade) return;
  if (u->motion.hasGoal) MotionStop(u);
  u->unitStates.erase("Moving");
}

// ---- factories -----------------------------------------------------------------------------------

void InheritFactoryCommands(Sim& sim, Unit* factory, Unit* built) {
  (void)sim;
  for (auto& c : factory->factoryCommands) {
    built->commands.push_back(c);
    c->units.insert(built);
  }
}

namespace {
void SetUpInitialRally(Sim& sim, Unit* f) {
  const UnitBpData& d = *f->bpData;
  const Quat& q = f->orientation;
  // the factory's x and z axes
  float x = q.x, y = q.y, z = q.z, w = q.w;
  Vec3 ax{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
  Vec3 az{2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
  Vec3 p{f->position.x + ax.x * d.initialRallyX + az.x * d.initialRallyZ,
         f->position.y + ax.y * d.initialRallyX + az.y * d.initialRallyZ,
         f->position.z + ax.z * d.initialRallyX + az.z * d.initialRallyZ};
  auto c = std::make_shared<UnitCommand>();
  c->id = sim.nextCommandId++;
  c->type = CommandType::Move;
  c->pos = p;
  c->hasPos = true;
  sim.commandsById[c->id] = c;
  f->factoryCommands.clear();
  f->factoryCommands.push_back(c);
  f->rallyPoint = p;
  f->hasRallyPoint = true;
}
}  // namespace

void UnitFinishedBuilding(Sim& sim, lua_State* L, Unit* u) {
  // a factory gets its first rally point
  if (u->bpData && u->bpData->structure && u->bpData->hasBuilder) SetUpInitialRally(sim, u);
  if (u->bpData && u->bpData->structure) AdjacencyGained(sim, L, u);
}

// ---- adjacency (Unit::CollectAllOverlapping 0x62d460 / OverlapsWith 0x62d2b0) --------------------

namespace {
struct IRect {
  float x0, z0, x1, z1;
};
IRect UnitRect(const Unit* u) {
  const NamedFootprint& fp = Footprint(*u->blueprint);
  int ox = RoundEven(u->position.x - fp.sizeX * 0.5f), oz = RoundEven(u->position.z - fp.sizeZ * 0.5f);
  return {static_cast<float>(ox), static_cast<float>(oz), static_cast<float>(ox + fp.sizeX),
          static_cast<float>(oz + fp.sizeZ)};
}
bool OverlapsWith(const Unit* a, const Unit* b) {
  IRect r = UnitRect(a), q = UnitRect(b);
  float d1 = std::fabs(r.x0 - q.x1), d2 = std::fabs(r.x1 - q.x0);
  if ((d1 >= 0 && d1 < 1) || (d2 >= 0 && d2 < 1))
    return (q.z0 <= r.z0 && r.z1 <= q.z1) || (r.z0 <= q.z0 && q.z1 <= r.z1);
  float e1 = std::fabs(r.z0 - q.z1), e2 = std::fabs(r.z1 - q.z0);
  if ((e1 >= 0 && e1 < 1) || (e2 >= 0 && e2 < 1))
    return (q.x0 <= r.x0 && r.x1 <= q.x1) || (r.x0 <= q.x0 && q.x1 <= r.x1);
  return false;
}
std::vector<Unit*> Overlapping(Sim& sim, Unit* u) {
  std::vector<Unit*> out;
  const float R = 20.0f;
  sim.ForUnitsInRect(u->position.x - R, u->position.z - R, u->position.x + R, u->position.z + R, [&](Unit* o) {
    if (o == u || !Alive(o) || o->beingBuilt || !o->bpData || !o->bpData->structure) return;
    if (o->army != u->army || o->layer != u->layer) return;
    if (OverlapsWith(u, o)) out.push_back(o);
  });
  std::sort(out.begin(), out.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  return out;
}
}  // namespace

void AdjacencyGained(Sim& sim, lua_State* L, Unit* u) {
  for (Unit* o : Overlapping(sim, u)) {
    PushObject(L, u);
    sim.CallMethod(L, o, "OnAdjacentTo", 1);
    PushObject(L, o);
    sim.CallMethod(L, u, "OnAdjacentTo", 1);
  }
}

void AdjacencyLost(Sim& sim, lua_State* L, Unit* u) {
  if (!u->bpData || !u->bpData->structure || u->beingBuilt) return;
  for (Unit* o : Overlapping(sim, u)) {
    PushObject(L, u);
    sim.CallMethod(L, o, "OnNotAdjacentTo", 1);
    PushObject(L, o);
    sim.CallMethod(L, u, "OnNotAdjacentTo", 1);
  }
}

}  // namespace moho

// ---- bindings ------------------------------------------------------------------------------------

namespace moho {
namespace {

Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

const BlueprintInfo* BlueprintArg(lua_State* L, int idx) {
  if (lua_isstring(L, idx)) return S(L)->blueprints().Find(lua_tostring(L, idx));
  if (lua_istable(L, idx)) {
    lua_pushstring(L, "BlueprintId");
    lua_rawget(L, idx);
    const BlueprintInfo* bp = lua_isstring(L, -1) ? S(L)->blueprints().Find(lua_tostring(L, -1)) : nullptr;
    lua_pop(L, 1);
    return bp;
  }
  return nullptr;
}

bool PosArg(lua_State* L, int idx, Vec3* p) {
  if (!lua_istable(L, idx)) return false;
  float v[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, idx, i + 1);
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  *p = {v[0], v[1], v[2]};
  return true;
}

// unit:CanBuild(blueprintId)
int l_CanBuild(lua_State* L) {
  Unit* u = U(L);
  const BlueprintInfo* bp = BlueprintArg(L, 2);
  lua_pushboolean(L, bp && UnitCanBuild(u, *bp));
  return 1;
}
// brain:CanBuildStructureAt(blueprintId, position)
int l_CanBuildStructureAt(lua_State* L) {
  CheckObject<AiBrain>(L, 1);
  const BlueprintInfo* bp = BlueprintArg(L, 2);
  Vec3 p;
  if (!bp || !PosArg(L, 3, &p)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, CanBuildStructureAt(*S(L), *bp, p.x, p.z));
  return 1;
}
// CreateResourceDeposit(type, x, y, z, size): "Mass" or "Hydrocarbon"
int l_CreateResourceDeposit(lua_State* L) {
  std::string type = luaL_checkstring(L, 1);
  float x = static_cast<float>(luaL_checknumber(L, 2)), z = static_cast<float>(luaL_checknumber(L, 4));
  int size = static_cast<int>(luaL_checknumber(L, 5));
  ResourceDeposit d;
  d.type = type == "Mass" ? 1 : type == "Hydrocarbon" ? 2 : 0;
  d.x0 = RoundEven(x - static_cast<float>(size) * 0.5f);
  d.z0 = RoundEven(z - static_cast<float>(size) * 0.5f);
  d.x1 = d.x0 + size;
  d.z1 = d.z0 + size;
  S(L)->deposits.push_back(d);
  return 0;
}

void CategoryBits(lua_State* L, int idx, std::vector<uint64_t>& out) {
  out.assign(static_cast<size_t>(CategoryWordCount()), 0);
  if (const uint64_t* c = ToCategory(L, idx))
    for (size_t i = 0; i < out.size(); ++i) out[i] = c[i];
}
// AddBuildRestriction(army, category) / RemoveBuildRestriction(army, category)
template <bool Add>
int l_ArmyBuildRestriction(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return 0;
  std::vector<uint64_t> bits;
  CategoryBits(L, 2, bits);
  if (a->buildRestricted.size() != bits.size()) a->buildRestricted.assign(bits.size(), 0);
  for (size_t i = 0; i < bits.size(); ++i) a->buildRestricted[i] = Add ? a->buildRestricted[i] | bits[i] : a->buildRestricted[i] & ~bits[i];
  return 0;
}
template <bool Add>
int l_UnitBuildRestriction(lua_State* L) {
  Unit* u = U(L);
  std::vector<uint64_t> bits;
  CategoryBits(L, 2, bits);
  if (u->buildAllowed.size() != bits.size()) u->buildAllowed.assign(bits.size(), ~uint64_t(0));
  for (size_t i = 0; i < bits.size(); ++i) u->buildAllowed[i] = Add ? u->buildAllowed[i] & ~bits[i] : u->buildAllowed[i] | bits[i];
  return 0;
}
int l_RestoreBuildRestrictions(lua_State* L) {
  Unit* u = U(L);
  u->buildAllowed.assign(static_cast<size_t>(CategoryWordCount()), ~uint64_t(0));
  return 0;
}

int l_GetFocusUnit(lua_State* L) {
  Unit* u = U(L);
  Unit* f = FindUnit(*S(L), u->focusId);
  PushObject(L, f);
  return 1;
}
int l_SetFocusEntity(lua_State* L) {
  Unit* u = U(L);
  Entity* e = ToObject<Entity>(L, 2);
  u->focusId = e ? e->id : 0;
  return 0;
}
int l_ClearFocusEntity(lua_State* L) {
  U(L)->focusId = 0;
  return 0;
}
int l_GetGuardedUnit(lua_State* L) {
  Unit* u = U(L);
  Unit* g = FindUnit(*S(L), u->guardedId);
  if (!g && !u->commands.empty()) {
    const UnitCommand& c = *u->commands.front();
    if (c.type == CommandType::Guard || c.type == CommandType::AssistCommander || c.type == CommandType::BuildAssist)
      g = FindUnit(*S(L), c.targetId);
  }
  PushObject(L, g);
  return 1;
}
int l_GetGuards(lua_State* L) {
  Unit* u = U(L);
  lua_newtable(L);
  int n = 0;
  for (Unit* o : S(L)->units()) {
    if (!Alive(o) || o == u || o->commands.empty()) continue;
    const UnitCommand& c = *o->commands.front();
    if ((c.type == CommandType::Guard || c.type == CommandType::AssistCommander || c.type == CommandType::BuildAssist) &&
        c.targetId == u->id) {
      PushObject(L, o);
      lua_rawseti(L, -2, ++n);
    }
  }
  return 1;
}
int l_SetBusy(lua_State* L) {
  Unit* u = U(L);
  u->busy = lua_toboolean(L, 2) != 0;
  if (u->busy) u->unitStates.insert("Busy");
  else u->unitStates.erase("Busy");
  return 0;
}
int l_SetBlockCommandQueue(lua_State* L) {
  Unit* u = U(L);
  u->blockCommandQueue = lua_toboolean(L, 2) != 0;
  if (u->blockCommandQueue) u->unitStates.insert("BlockCommandQueue");
  else u->unitStates.erase("BlockCommandQueue");
  return 0;
}

// ---- attachment: a factory holds what it builds at its build bone ----
int BoneIndex(lua_State* L, Entity* e, int arg) {
  if (lua_isnumber(L, arg)) return static_cast<int>(lua_tonumber(L, arg));
  if (lua_isstring(L, arg) && e->skeleton) return e->skeleton->Find(lua_tostring(L, arg));
  return -1;
}
// entity:AttachBoneTo(selfBone, parent, parentBone)
int l_AttachBoneTo(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  Entity* parent = ToObject<Entity>(L, 3);
  if (!u || !parent) return 0;
  u->parentId = parent->id;
  u->ownBone = BoneIndex(L, u, 2);
  u->parentBone = BoneIndex(L, parent, 4);
  u->unitStates.insert("Attached");
  if (u->motion.hasGoal) MotionStop(u);
  u->position = EntityBonePosition(parent, u->parentBone);
  S(L)->MarkUnitsMoved();
  return 0;
}
int l_AttachTo(lua_State* L) {  // entity:AttachTo(parent, bone)
  Unit* u = ToObject<Unit>(L, 1);
  Entity* parent = ToObject<Entity>(L, 2);
  if (!u || !parent) return 0;
  u->parentId = parent->id;
  u->ownBone = -1;
  u->parentBone = BoneIndex(L, parent, 3);
  u->unitStates.insert("Attached");
  if (u->motion.hasGoal) MotionStop(u);
  u->position = EntityBonePosition(parent, u->parentBone);
  S(L)->MarkUnitsMoved();
  return 0;
}
void Detach(Sim& sim, Unit* u) {
  if (!u->parentId) return;
  u->parentId = 0;
  u->unitStates.erase("Attached");
  u->motion.needSnap = true;
  sim.MarkUnitsMoved();
}
int l_DetachFrom(lua_State* L) {
  if (Unit* u = ToObject<Unit>(L, 1)) Detach(*S(L), u);
  return 0;
}
int l_DetachAll(lua_State* L) {  // entity:DetachAll(bone)
  Entity* e = ToObject<Entity>(L, 1);
  if (!e) return 0;
  int bone = lua_isnoneornil(L, 2) ? -2 : BoneIndex(L, e, 2);
  for (Unit* o : S(L)->units())
    if (o->parentId == e->id && (bone == -2 || o->parentBone == bone)) Detach(*S(L), o);
  return 0;
}
int l_GetParent(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Entity* p = nullptr;
  if (e->kind == Entity::Kind::Unit) p = S(L)->FindEntity(static_cast<Unit*>(e)->parentId);
  PushObject(L, p ? p : e);
  return 1;
}

// ---- factory commands ----
std::vector<Unit*> UnitList(lua_State* L, int idx) {
  std::vector<Unit*> out;
  if (Unit* u = ToObject<Unit>(L, idx)) {
    out.push_back(u);
    return out;
  }
  if (!lua_istable(L, idx)) return out;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    if (Unit* u = ToObject<Unit>(L, -1))
      if (Alive(u)) out.push_back(u);
    lua_pop(L, 1);
  }
  return out;
}
// IssueFactoryRallyPoint(factories, position)
int l_IssueFactoryRallyPoint(lua_State* L) {
  auto units = UnitList(L, 1);
  Vec3 p;
  if (!PosArg(L, 2, &p)) return 0;
  Sim* sim = S(L);
  auto c = std::make_shared<UnitCommand>();
  c->id = sim->nextCommandId++;
  c->type = CommandType::Move;
  c->pos = p;
  c->hasPos = true;
  sim->commandsById[c->id] = c;
  for (Unit* u : units) {
    u->factoryCommands.clear();
    u->factoryCommands.push_back(c);
    u->rallyPoint = p;
    u->hasRallyPoint = true;
  }
  return 0;
}
int l_IssueClearFactoryCommands(lua_State* L) {
  for (Unit* u : UnitList(L, 1)) u->factoryCommands.clear();
  return 0;
}
// IssueMoveOffFactory(units, position)
int l_IssueMoveOffFactory(lua_State* L) {
  auto units = UnitList(L, 1);
  Vec3 p;
  if (!PosArg(L, 2, &p)) return 0;
  Sim* sim = S(L);
  auto c = std::make_shared<UnitCommand>();
  c->id = sim->nextCommandId++;
  c->type = CommandType::Move;
  c->pos = p;
  c->hasPos = true;
  sim->commandsById[c->id] = c;
  for (Unit* u : units) {
    u->commands.push_front(c);
    c->units.insert(u);
    u->headState = 0;
  }
  return 0;
}
int l_GetRallyPoint(lua_State* L) {
  Unit* u = U(L);
  for (auto& c : u->factoryCommands)
    if (c->hasPos) {
      PushVector(L, c->pos.x, c->pos.y, c->pos.z);
      return 1;
    }
  lua_pushnil(L);
  return 1;
}
// unit:GetNumBuildOrders(category): build commands in its queue for blueprints in the category
int l_GetNumBuildOrders(lua_State* L) {
  Unit* u = U(L);
  const uint64_t* cat = ToCategory(L, 2);
  int n = 0;
  for (auto& c : u->commands) {
    if (c->type != CommandType::BuildFactory && c->type != CommandType::BuildMobile) continue;
    const BlueprintInfo* bp = S(L)->blueprints().Find(c->blueprintId);
    if (bp && (!cat || (bp->entityIndex >= 0 && CategoryHas(cat, bp->entityIndex)))) n += std::max(1, c->count);
  }
  lua_pushnumber(L, n);
  return 1;
}

}  // namespace

namespace {
int l_task_GetUnit(lua_State* L) {
  BuildTask* t = CheckObject<BuildTask>(L, 1);
  PushObject(L, FindUnit(*S(L), t->unitId));
  return 1;
}
int l_task_SetAIResult(lua_State* L) {
  CheckObject<BuildTask>(L, 1);
  return 0;
}
}  // namespace

void RegisterBuildBindings(lua_State* L) {
  SetMethod(L, "Unit", "CanBuild", l_CanBuild);
  SetMethod(L, "CAiBrain", "CanBuildStructureAt", l_CanBuildStructureAt);
  SetGlobal(L, "CreateResourceDeposit", l_CreateResourceDeposit);
  SetGlobal(L, "AddBuildRestriction", l_ArmyBuildRestriction<true>);
  SetGlobal(L, "RemoveBuildRestriction", l_ArmyBuildRestriction<false>);
  SetMethod(L, "Unit", "AddBuildRestriction", l_UnitBuildRestriction<true>);
  SetMethod(L, "Unit", "RemoveBuildRestriction", l_UnitBuildRestriction<false>);
  SetMethod(L, "Unit", "RestoreBuildRestrictions", l_RestoreBuildRestrictions);
  SetMethod(L, "Unit", "GetFocusUnit", l_GetFocusUnit);
  SetMethod(L, "Unit", "SetFocusEntity", l_SetFocusEntity);
  SetMethod(L, "Unit", "ClearFocusEntity", l_ClearFocusEntity);
  SetMethod(L, "Unit", "GetGuardedUnit", l_GetGuardedUnit);
  SetMethod(L, "Unit", "GetGuards", l_GetGuards);
  SetMethod(L, "Unit", "SetBusy", l_SetBusy);
  SetMethod(L, "Unit", "SetBlockCommandQueue", l_SetBlockCommandQueue);
  SetMethod(L, "Entity", "AttachBoneTo", l_AttachBoneTo);
  SetMethod(L, "Entity", "AttachTo", l_AttachTo);
  SetMethod(L, "Entity", "DetachFrom", l_DetachFrom);
  SetMethod(L, "Entity", "DetachAll", l_DetachAll);
  SetMethod(L, "Entity", "GetParent", l_GetParent);
  SetGlobal(L, "IssueFactoryRallyPoint", l_IssueFactoryRallyPoint);
  SetGlobal(L, "IssueClearFactoryCommands", l_IssueClearFactoryCommands);
  SetGlobal(L, "IssueMoveOffFactory", l_IssueMoveOffFactory);
  SetMethod(L, "Unit", "GetRallyPoint", l_GetRallyPoint);
  SetMethod(L, "Unit", "GetNumBuildOrders", l_GetNumBuildOrders);
  RegisterSiloBindings(L);
  SetMethod(L, "CUnitScriptTask", "GetUnit", l_task_GetUnit);
  SetMethod(L, "CUnitScriptTask", "SetAIResult", l_task_SetAIResult);
}

}  // namespace moho

// ---- missile silos (CAiSiloBuildImpl::SiloTick 0x5cf1e0) -------------------------------------
//
// A unit whose weapons have CountedProjectile keeps one silo per kind (0 tactical, 1 nuke: the
// weapon's NukeWeapon flag) holding up to MaxProjectileStorage missiles. Queued builds (IssueSilo-
// Build*) or auto mode start a build: ticks = BuildTime * 10 / build rate (GetEconomyBuildRate,
// at least 0.1), each tick asks the army for BuildCost * the Energy/MassBuildAdjMod / ticks and
// advances only when that is granted in full (as economy events); OnSiloBuildStart(weapon) /
// OnSiloBuildEnd(weapon) and OnNukeArmed(weapon) frame it; the state SiloBuildingAmmo is on while
// it builds.

namespace moho {

struct SiloState : public ScriptObject {
  int weapon[2] = {-1, -1};  // index into unit->weapons
  int maxStore[2] = {0, 0};
  std::deque<int> queue;     // kinds waiting to be built
  int stage = 0, kind = 0;
  int ticksTotal = 0, ticksLeft = 0;
  float perTick[2] = {0, 0};
  std::shared_ptr<EconRequest> request;
};

namespace {

float FieldNum(lua_State* L, int t, const char* k, float def) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
bool FieldBool(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}

SiloState* GetSilo(Sim& sim, Unit* u) {
  if (u->silo) return u->silo;
  lua_State* L = sim.L();
  auto s = std::make_unique<SiloState>();
  int top = lua_gettop(L);
  for (size_t i = 0; i < u->weapons.size(); ++i) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, u->weapons[i]->bpRef);
    int w = lua_gettop(L);
    if (lua_istable(L, w) && FieldBool(L, w, "CountedProjectile")) {
      int kind = FieldBool(L, w, "NukeWeapon") ? 1 : 0;
      if (s->weapon[kind] < 0) {
        s->weapon[kind] = static_cast<int>(i);
        s->maxStore[kind] = static_cast<int>(FieldNum(L, w, "MaxProjectileStorage", 0));
      }
    }
    lua_settop(L, top);
  }
  u->silo = s.get();
  sim.Own(std::move(s));
  return u->silo;
}

float CallNumber(Sim& sim, lua_State* L, Unit* u, const char* method, float def) {
  int top = lua_gettop(L);
  PushObject(L, u);
  lua_pushstring(L, method);
  lua_gettable(L, -2);
  float v = def;
  if (lua_isfunction(L, -1)) {
    lua_pushcfunction(L, ScriptTraceback);
    lua_insert(L, -2);
    lua_pushvalue(L, top + 1);
    if (lua_pcall(L, 1, 1, top + 2) == 0) {
      if (lua_isnumber(L, -1)) v = static_cast<float>(lua_tonumber(L, -1));
    } else {
      LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
  }
  lua_settop(L, top);
  (void)sim;
  return v;
}

bool SiloAddBuild(Sim& sim, Unit* u, int kind) {
  SiloState* s = GetSilo(sim, u);
  if (s->weapon[kind] < 0) return false;
  int pending = 0;
  for (int k : s->queue)
    if (k == kind) ++pending;
  if (s->stage != 0 && s->kind == kind) ++pending;
  if (u->siloAmmo[kind] + pending >= s->maxStore[kind]) return false;
  s->queue.push_back(kind);
  return true;
}

void CallWeaponMethod(Sim& sim, lua_State* L, Unit* u, const char* method, int weaponIndex) {
  if (weaponIndex < 0 || weaponIndex >= static_cast<int>(u->weapons.size())) return;
  PushObject(L, u->weapons[weaponIndex]);
  sim.CallMethod(L, u, method, 1);
}

}  // namespace

void SiloRelease(Unit* u) {
  if (u->silo && u->silo->request) {
    u->silo->request->live = false;
    u->silo->request.reset();
  }
}

void SiloTick(Sim& sim, Unit* u) {
  if (!u->silo && !u->autoMode) return;
  if (u->beingBuilt || u->dead || u->paused) return;
  lua_State* L = sim.L();
  SiloState* s = GetSilo(sim, u);
  switch (s->stage) {
    case 0:
      if (!s->queue.empty()) {
        s->stage = 1;
        return;
      }
      if (u->autoMode)
        for (int k = 0; k < 2; ++k)
          if (SiloAddBuild(sim, u, k)) return;
      return;
    case 1: {
      int kind = s->queue.front();
      s->kind = kind;
      int wi = s->weapon[kind];
      if (wi < 0) {
        s->queue.pop_front();
        s->stage = 0;
        return;
      }
      // the projectile's build costs
      UnitWeapon* w = u->weapons[wi];
      int top = lua_gettop(L);
      lua_rawgeti(L, LUA_REGISTRYINDEX, w->bpRef);
      lua_pushstring(L, "ProjectileId");
      lua_rawget(L, -2);
      const BlueprintInfo* pbp = lua_isstring(L, -1) ? sim.blueprints().Find(lua_tostring(L, -1)) : nullptr;
      lua_settop(L, top);
      float bt = 0, be = 0, bm = 0;
      if (pbp) {
        sim.blueprints().PushTable(L, *pbp);
        lua_pushstring(L, "Economy");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
          int ec = lua_gettop(L);
          bt = FieldNum(L, ec, "BuildTime", 0);
          be = FieldNum(L, ec, "BuildCostEnergy", 0);
          bm = FieldNum(L, ec, "BuildCostMass", 0);
        }
        lua_settop(L, top);
      }
      float rate = std::max(0.1f, CallNumber(sim, L, u, "GetEconomyBuildRate", u->buildRate));
      float eAdj = CallNumber(sim, L, u, "GetEnergyBuildAdjMod", 1);
      float mAdj = CallNumber(sim, L, u, "GetMassBuildAdjMod", 1);
      float ticks = bt * 10.0f / rate;
      s->ticksTotal = std::max(1, static_cast<int>(ticks));
      s->ticksLeft = s->ticksTotal;
      s->perTick[kEnergy] = ticks > 0 ? be * eAdj / ticks : 0;
      s->perTick[kMass] = ticks > 0 ? bm * mAdj / ticks : 0;
      if (s->request) s->request->live = false;
      s->request = u->army ? u->army->econ.NewRequest() : nullptr;
      if (s->request) {
        s->request->requested[kEnergy] = s->perTick[kEnergy];
        s->request->requested[kMass] = s->perTick[kMass];
      }
      u->consumptionShown[0] = s->perTick[0];
      u->consumptionShown[1] = s->perTick[1];
      u->unitStates.insert("SiloBuildingAmmo");
      s->stage = 2;
      CallWeaponMethod(sim, L, u, "OnSiloBuildStart", wi);
      return;
    }
    case 2: {
      if (s->request) {
        if (!(s->request->granted[0] >= s->perTick[0] && s->request->granted[1] >= s->perTick[1])) return;
        u->consumedTick[0] += s->request->granted[0];
        u->consumedTick[1] += s->request->granted[1];
        s->request->granted[0] = s->request->granted[1] = 0;
      }
      --s->ticksLeft;
      u->workProgress = 1.0f - static_cast<float>(s->ticksLeft) / static_cast<float>(s->ticksTotal);
      if (s->ticksLeft > 0) return;
      s->stage = 3;
      return;
    }
    case 3: {
      if (s->request) {
        s->request->live = false;
        s->request.reset();
      }
      u->consumptionShown[0] = u->consumptionShown[1] = 0;
      u->unitStates.erase("SiloBuildingAmmo");
      u->workProgress = 0;
      int wi = s->weapon[s->kind];
      CallWeaponMethod(sim, L, u, "OnSiloBuildEnd", wi);
      CallWeaponMethod(sim, L, u, "OnNukeArmed", wi);
      ++u->siloAmmo[s->kind];
      if (!s->queue.empty()) s->queue.pop_front();
      s->stage = 0;
      return;
    }
  }
}

namespace {

template <int Kind>
int l_GetSiloAmmo(lua_State* L) {
  lua_pushnumber(L, CheckObject<Unit>(L, 1)->siloAmmo[Kind]);
  return 1;
}
template <int Kind>
int l_GiveSiloAmmo(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->siloAmmo[Kind] += static_cast<int>(luaL_checknumber(L, 2));
  if (u->silo && u->silo->maxStore[Kind] > 0) u->siloAmmo[Kind] = std::min(u->siloAmmo[Kind], u->silo->maxStore[Kind]);
  return 0;
}
template <int Kind>
int l_RemoveSiloAmmo(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->siloAmmo[Kind] = std::max(0, u->siloAmmo[Kind] - static_cast<int>(luaL_checknumber(L, 2)));
  return 0;
}
int l_StopSiloBuild(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (u->silo) {
    u->silo->queue.clear();
    if (u->silo->stage == 2) {
      if (u->silo->request) {
        u->silo->request->live = false;
        u->silo->request.reset();
      }
      u->unitStates.erase("SiloBuildingAmmo");
      u->silo->stage = 0;
    }
  }
  return 0;
}
int l_SetAutoMode(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  bool on = lua_toboolean(L, 2) != 0;
  bool was = u->autoMode;
  u->autoMode = on;
  if (on != was) S(L)->CallMethod(L, u, on ? "OnAutoModeOn" : "OnAutoModeOff", 0);
  return 0;
}
int l_SetPaused(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  bool on = lua_toboolean(L, 2) != 0;
  bool was = u->paused;
  u->paused = on;
  if (on != was) S(L)->CallMethod(L, u, on ? "OnPaused" : "OnUnpaused", 0);
  return 0;
}
int l_IsPaused(lua_State* L) {
  lua_pushboolean(L, CheckObject<Unit>(L, 1)->paused);
  return 1;
}
int l_weapon_GetProjectileBlueprint(lua_State* L) {
  UnitWeapon* w = CheckObject<UnitWeapon>(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, w->bpRef);
  lua_pushstring(L, "ProjectileId");
  lua_rawget(L, -2);
  const BlueprintInfo* bp = lua_isstring(L, -1) ? S(L)->blueprints().Find(lua_tostring(L, -1)) : nullptr;
  if (!bp) {
    lua_pushnil(L);
    return 1;
  }
  S(L)->blueprints().PushTable(L, *bp);
  return 1;
}

}  // namespace

void RegisterSiloBindings(lua_State* L) {
  SetMethod(L, "Unit", "GetTacticalSiloAmmoCount", l_GetSiloAmmo<0>);
  SetMethod(L, "Unit", "GetNukeSiloAmmoCount", l_GetSiloAmmo<1>);
  SetMethod(L, "Unit", "GiveTacticalSiloAmmo", l_GiveSiloAmmo<0>);
  SetMethod(L, "Unit", "GiveNukeSiloAmmo", l_GiveSiloAmmo<1>);
  SetMethod(L, "Unit", "RemoveTacticalSiloAmmo", l_RemoveSiloAmmo<0>);
  SetMethod(L, "Unit", "RemoveNukeSiloAmmo", l_RemoveSiloAmmo<1>);
  SetMethod(L, "Unit", "StopSiloBuild", l_StopSiloBuild);
  SetMethod(L, "Unit", "SetAutoMode", l_SetAutoMode);
  SetMethod(L, "Unit", "SetPaused", l_SetPaused);
  SetMethod(L, "Unit", "IsPaused", l_IsPaused);
  SetMethod(L, "UnitWeapon", "GetProjectileBlueprint", l_weapon_GetProjectileBlueprint);
}

// IssueSiloBuildTactical / IssueSiloBuildNuke: a missile joins the silo's queue (the command is
// done at once).
bool SiloCommand(Sim& sim, Unit* u, const UnitCommand& c) {
  if (c.type != CommandType::BuildSiloTactical && c.type != CommandType::BuildSiloNuke) return false;
  int kind = c.type == CommandType::BuildSiloNuke ? 1 : 0;
  for (int i = 0; i < std::max(1, c.count); ++i) SiloAddBuild(sim, u, kind);
  return true;
}

}  // namespace moho
