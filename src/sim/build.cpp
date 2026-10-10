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
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/formation.h"
#include "sim/transport.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/economy.h"
#include "sim/motion.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"
#include "sim/air.h"
#include <limits>
#include <set>

namespace moho {
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }

bool Alive(const Entity* e) { return e && !e->dead && !e->destroyQueued; }

Unit* FindUnit(Sim& sim, uint32_t id) {
  if (!id) return nullptr;  // 0: none
  Entity* e = sim.FindEntity(id);
  return e && e->kind == Entity::Kind::Unit && Alive(e) ? static_cast<Unit*>(e) : nullptr;
}
// A weak pointer's view: the unit until it is freed (dead and destroy-queued units included).
Unit* FindUnitAny(Sim& sim, uint32_t id) {
  if (!id) return nullptr;
  Entity* e = sim.FindEntity(id);
  return e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
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

// CSimResources::AreaHasDeposit 0x546860: a deposit of the type, grown by 0.5 (mass) / 1.5 (hydro) on every
// side, strictly overlaps the rect.
bool DepositOverlaps(const Sim& sim, int type, float x0, float z0, float x1, float z1) {
  float g = type == 1 ? 0.5f : 1.5f;
  for (const auto& d : sim.deposits)
    if (d.type == type && x0 < static_cast<float>(d.x1) + g && static_cast<float>(d.x0) - g < x1 &&
        z0 < static_cast<float>(d.z1) + g && static_cast<float>(d.z0) - g < z1)
      return true;
  return false;
}

// CSimResources::DepositIsInArea 0x546650: the rect contains a deposit of the type, or one contains the rect.
bool DepositInArea(const Sim& sim, int type, int x0, int z0, int x1, int z1) {
  for (const auto& d : sim.deposits) {
    if (d.type != type) continue;
    if ((x0 <= d.x0 && d.x1 <= x1 && z0 <= d.z0 && d.z1 <= z1) || (d.x0 <= x0 && x1 <= d.x1 && d.z0 <= z0 && z1 <= d.z1))
      return true;
  }
  return false;
}

// OCCUPY_Check for a structure blueprint at (x, z): the layers it could be built on (0 = none).
int StructureLayers(Sim& sim, const BlueprintInfo& bp, const UnitBpData& d, float x, float z) {
  const TerrainMap* map = sim.map();
  if (!map) return 0;
  FRect sk = SkirtRect(bp, d, x, z);
  // OCCUPY_Check 0x5652e0 (engine-ref structure_placement.md): the skirt within the vertex bounds
  if (std::floor(sk.x0) < 0 || std::floor(sk.z0) < 0 || std::ceil(sk.x1) > map->width() ||
      std::ceil(sk.z1) > map->height())
    return 0;
  // the flatness samples: the skirt truncated to vertices
  int x0 = static_cast<int>(sk.x0), z0 = static_cast<int>(sk.z0);
  int x1 = static_cast<int>(sk.x1), z1 = static_cast<int>(sk.z1);
  int caps = d.buildOnLayerCaps;  // Physics.BuildOnLayerCaps (bp+0x2f4)
  float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
  auto take = [&](int vx, int vz) {
    vx = std::clamp(vx, 0, map->width());
    vz = std::clamp(vz, 0, map->height());
    float h = map->HeightAt(vx, vz);
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  };
  bool flat;
  if (!d.flattenSkirt) {  // AreaFlatness 0x5651f0
    for (int vz = z0; vz <= z1; ++vz)
      for (int vx = x0; vx <= x1; ++vx) take(vx, vz);
    flat = hi - lo <= d.maxGroundVariation;
  } else {  // EdgeFlatness 0x564f80: the ring just outside, measured from the ceiling of its lowest point
    for (int vx = x0 - 1; vx <= x1 + 1; ++vx) {
      take(vx, z0 - 1);
      take(vx, z1 + 1);
    }
    for (int vz = z0; vz <= z1; ++vz) {
      take(x0 - 1, vz);
      take(x1 + 1, vz);
    }
    float c = static_cast<float>(std::ceil(static_cast<double>(lo)));
    flat = std::max(hi - c, c - lo) <= d.maxGroundVariation;
  }
  if (!flat) caps &= ~3;
  float water = map->hasWater ? map->waterElevation : -10000.0f;
  if (water > lo) caps &= ~1;
  if (hi > water - Footprint(bp).minWaterDepth) caps &= ~0xe;
  if (!caps) return 0;
  // deposits (no terrain-type test on this path): extractors need theirs; nothing else may come near one
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  int dep = DepositType(d.buildRestriction);
  if (dep) {
    if (!DepositInArea(sim, dep, ox, oz, ox + fp.sizeX, oz + fp.sizeZ)) return 0;
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
  if (BpInCategory(sim, u->blueprint, "FERRYBEACON")) return;  // Unit::Unit skips ExecuteOccupyGround
  const NamedFootprint& fp = Footprint(*u->blueprint);
  int ox = RoundEven(u->position.x - fp.sizeX * 0.5f), oz = RoundEven(u->position.z - fp.sizeZ * 0.5f);
  sim.navigation().AddStructure(EntityRef(u), ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
  LandNavDirty(sim, ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}
void ReleaseStructure(Sim& sim, Unit* u) {
  if (!u->bpData || !u->bpData->structure) return;
  for (const auto& r : sim.navigation().Structures())
    if (r.entity == EntityRef(u)) {
      LandNavDirty(sim, r.x0, r.z0, r.x1, r.z1);
      break;
    }
  sim.navigation().RemoveStructure(EntityRef(u));
}

// ---- build helper (CBuildTaskHelper) -----------------------------------------------------------

namespace {

void SetFocus(Sim& sim, lua_State* L, Unit* builder, Unit* target, BuildTask& t);

// OnStopBuild(success): the builder's OnStopBuild(target, order) (and on failure OnFailedToBuild /
// the target's OnFailedToBeBuilt first); the builder loses its focus.
void StopBuild(Sim& sim, lua_State* L, Unit* builder, BuildTask& t, bool success) {
  Unit* target = FindUnitAny(sim, t.targetId);  // (the helper's weak pointer: non-null until freed)
  if (t.started && Alive(builder)) {
    if (!success) {
      sim.CallMethod(L, builder, "OnFailedToBuild", 0);
      if (target && target->HasLuaObject()) sim.CallMethod(L, target, "OnFailedToBeBuilt", 0);
    }
    PushObject(L, target && !target->destroyQueued ? target : nullptr);
    lua_pushstring(L, t.order.c_str());
    sim.CallMethod(L, builder, "OnStopBuild", 2);
  }
  if (builder->focusId == t.targetId) builder->focusId = 0;
  t.started = false;
}

void SetFocus(Sim& sim, lua_State* L, Unit* builder, Unit* target, BuildTask& t) {
  if (!target) return;
  if (t.started && t.targetId == EntityRef(target)) return;
  if (t.started) StopBuild(sim, L, builder, t, true);  // switching work: the old target is not failed
  t.targetId = EntityRef(target);
  builder->focusId = EntityRef(target);  // Unit::SetFocusEntity, then OnAssignedFocusEntity (no args)
  sim.CallMethod(L, builder, "OnAssignedFocusEntity", 0);
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
  path.push_back(goal);  // land units: the navigator plans (sim/landnav.cpp)
  u->motion.failed = false;
  MotionSetGoal(sim, u, path, false, sim.tick());
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

BuildTask* NewChild(Sim& sim, CommandType type, const char* order);
bool PrepareMoveFor(Sim& sim, Unit* u, Vec3* p, const float excl[4], int spacing = 0);

// A prop blueprint's float field (default when missing).
float PropBpNum(Sim& sim, lua_State* L, const BlueprintInfo& bp, const char* sect, const char* key, float def) {
  int top = lua_gettop(L);
  sim.blueprints().PushTable(L, bp);
  if (sect) {
    lua_pushstring(L, sect);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
      lua_settop(L, top);
      return def;
    }
  }
  lua_pushstring(L, key);
  lua_rawget(L, -2);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_settop(L, top);
  return v;
}

// 0x5f6ea0 (mobile_build.md 8.2): the obstructing prop nearest the builder in the target's footprint, unless it is
// the matching wreck exactly on the site (kept: destroyed when the unit is created).
Entity* FindSiteProp(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  const NamedFootprint& fp = Footprint(*t.bp);
  int x0 = RoundEven(t.site.x - fp.sizeX * 0.5f), z0 = RoundEven(t.site.z - fp.sizeZ * 0.5f);
  Vec3 mn{static_cast<float>(x0), t.site.y - 1000.0f, static_cast<float>(z0)};
  Vec3 mx{static_cast<float>(x0 + fp.sizeX), t.site.y + 1000.0f, static_cast<float>(z0 + fp.sizeZ)};
  std::vector<Entity*> cand;
  sim.entityGrid().GatherBox(mn.x, mn.z, mx.x, mx.z, 2, &cand);
  Entity* best = nullptr;
  float bestD = std::numeric_limits<float>::infinity();
  for (Entity* p : cand) {
    if (p->kind != Entity::Kind::Prop || !p->blueprint) continue;
    WorldShape ws;
    if (!GetWorldShape(p, &ws)) continue;
    Vec3 a, b;
    ShapeBounds(ws, &a, &b);
    if (a.x > mx.x || b.x < mn.x || a.y > mx.y || b.y < mn.y || a.z > mx.z || b.z < mn.z) continue;
    if (!ShapeOverlapsAABox(ws, mn, mx)) continue;
    if (!BpInCategory(sim, p->blueprint, "OBSTRUCTSBUILDING")) continue;
    float dx = p->position.x - u->position.x, dy = p->position.y - u->position.y, dz = p->position.z - u->position.z;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < bestD) {
      bestD = d2;
      best = p;
    }
  }
  if (!best) return nullptr;
  // a wreck the target rebuilds (Economy.RebuildBonusIds), centred on the site, is kept
  int top = lua_gettop(L);
  PushObject(L, best);
  lua_pushstring(L, "AssociatedBP");
  lua_gettable(L, -2);
  std::string assoc = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_settop(L, top);
  if (assoc.empty()) return best;
  bool listed = false;
  sim.blueprints().PushTable(L, *t.bp);
  lua_pushstring(L, "Economy");
  lua_rawget(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "RebuildBonusIds");
    lua_rawget(L, -2);
    if (lua_istable(L, -1))
      for (int i = 1;; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        if (lua_type(L, -1) == LUA_TSTRING && !strcasecmp(lua_tostring(L, -1), assoc.c_str())) listed = true;
        lua_pop(L, 1);
      }
  }
  lua_settop(L, top);
  if (!listed) return best;
  float dx = t.site.x - best->position.x, dz = t.site.z - best->position.z;
  if (!(dx * dx + dz * dz < 1e-6f)) return best;
  t.rebuildWreck = EntityRef(best);  // FAF's GetRebuildBonus returns 0: no engine bonus
  return nullptr;
}

// CUnitReclaimTask for a prop target (mobile_build.md 8.4), the child clearing a build site.
void EndPropReclaim(Sim& sim, lua_State* L, Unit* u, BuildTask& c) {
  Entity* e = c.goalId ? sim.FindEntity(c.goalId) : nullptr;
  if (c.reclaimStarted) {
    c.reclaimStarted = false;
    PushObject(L, e);
    sim.CallMethod(L, u, "OnStopReclaim", 1);
  }
  if (!u->builderArms.empty()) SetArmAimTarget(sim, u, Vec3{0, 0, 0});
  u->unitStates.erase("Reclaiming");
  u->focusId = 0;
  u->workProgress = 0;
  if (c.moving) StopMoving(u);
}

int TickPropReclaim(Sim& sim, lua_State* L, Unit* u, BuildTask& c) {
  Entity* e = c.goalId ? sim.FindEntity(c.goalId) : nullptr;
  if (!e || e->destroyQueued) return kTaskFailed;
  Vec3 tp = e->position;
  float dx = u->position.x - tp.x, dz = u->position.z - tp.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  const NamedFootprint& bf = Footprint(*u->blueprint);
  float pfx = PropBpNum(sim, L, *e->blueprint, "Footprint", "SizeX", 1), pfz = PropBpNum(sim, L, *e->blueprint, "Footprint", "SizeZ", 1);
  float gap = dist - static_cast<float>(std::max(bf.sizeX, bf.sizeZ)) - std::max(pfx, pfz);
  float maxBD = u->bpData->maxBuildDistance;
  for (;;) switch (c.state) {
    case 0:
      if (!BpInCategory(sim, e->blueprint, "RECLAIMABLE") || e == u) return kTaskFailed;
      if (gap > maxBD || dist < 1.0f) {  // PrepareMove next to the prop's footprint rect, reserve, move
        if (!CanMove(u)) return kTaskFailed;
        int px0 = RoundEven(tp.x - pfx * 0.5f), pz0 = RoundEven(tp.z - pfz * 0.5f);
        float excl[4] = {static_cast<float>(px0), static_cast<float>(pz0), static_cast<float>(px0) + pfx,
                         static_cast<float>(pz0) + pfz};
        Vec3 a = tp;
        PrepareMoveFor(sim, u, &a, excl, 1);  // spacing 1 (air_nav.md 7.1)
        const NamedFootprint& ufp = Footprint(*u->blueprint);
        int x0 = static_cast<int>(std::nearbyint(a.x - ufp.sizeX * 0.5f));
        int z0 = static_cast<int>(std::nearbyint(a.z - ufp.sizeZ * 0.5f));
        int rr[4] = {x0, z0, x0 + ufp.sizeX, z0 + ufp.sizeZ};
        GroundReserveRect(sim, u, rr);
        MoveToward(sim, u, Vec3{x0 + ufp.sizeX * 0.5f, a.y, z0 + ufp.sizeZ * 0.5f});
        c.moving = true;
      }
      c.state = 1;
      if (c.moving) return kTaskRunning;
      continue;
    case 1:
      if (c.moving) {
        if (u->motion.hasGoal && !u->motion.arrived && !u->motion.failed) return kTaskRunning;
        c.moving = false;
        StopMoving(u);
        GroundFreeRect(sim, u);
      }
      if (gap > maxBD) return kTaskFailed;
      if (!u->builderArms.empty()) SetArmAimTarget(sim, u, tp);
      if (!c.reclaimStarted) {
        c.reclaimStarted = true;
        PushObject(L, e);
        sim.CallMethod(L, u, "OnStartReclaim", 1);
      }
      c.state = 2;
      continue;
    case 2:
      c.state = 3;
      return kTaskRunning;
    case 3: {
      if (!u->builderArms.empty() && dist > 1.0f && !u->armReady) return kTaskRunning;
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
      float rate = std::max(1.0f, time * 10.0f);
      c.reclaimStep = 1.0f / rate;
      if (energy < 0 || mass < 0) return kTaskFailed;
      c.reclaimPerTick[kEnergy] = energy / rate;
      c.reclaimPerTick[kMass] = mass / rate;
      u->unitStates.insert("Reclaiming");
      u->focusId = EntityRef(e);
      if (e->maxHealth > 0) e->fractionComplete = std::min(e->fractionComplete, e->health / e->maxHealth);
      c.state = 4;
      return kTaskRunning;
    }
    case 4: {
      if (gap > maxBD) return kTaskFailed;
      if (!c.reclaimStarted) {
        c.reclaimStarted = true;
        PushObject(L, e);
        sim.CallMethod(L, u, "OnStartReclaim", 1);
      }
      if (u->paused) return kTaskRunning;
      float k = u->request ? u->request->LimitingRate() : 1.0f;
      float old = e->fractionComplete;
      float f = std::clamp(old - c.reclaimStep * k, 0.0f, 1.0f);
      e->fractionComplete = f;
      float taken = old - f;
      float h = e->maxHealth * f;
      if (h != e->health) EntityAdjustHealth(L, e, u, h - e->health);
      u->workProgress = 1.0f - f;
      if (u->army && c.reclaimStep > 0) {
        float q = taken / c.reclaimStep;
        for (int i = 0; i < 2; ++i) {
          float got = q * c.reclaimPerTick[i];
          u->army->econ.income[i] += got;
          u->army->econ.reclaimed[i] += got;
        }
      }
      if (f <= 0.0f && !e->destroyQueued) {
        PushObject(L, u);
        sim.CallMethod(L, e, "OnReclaimed", 1);
        if (!e->destroyQueued) sim.QueueDestroy(e);
      }
      return kTaskRunning;  // the end is the next beat's pre-check
    }
    default:
      return kTaskFailed;
  }
}

// CUnitMobileBuildTask::Tick 0x5f7440 (engine-ref mobile_build.md): states 1 -> 2 -> 3 run in one beat (the
// original's return 0), so a builder whose arm is already on target creates the structure the beat after the
// order; joining a structure at the site works in the same beat.
int TickMobileBuild(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (t.sub) {  // the reclaim child clearing the site runs on top of this task
    if (sim.tick() < t.subFrom) return kTaskRunning;
    int r = TickPropReclaim(sim, L, u, *t.sub);
    if (r == kTaskRunning) return kTaskRunning;
    EndPropReclaim(sim, L, u, *t.sub);
    t.sub = nullptr;  // its result is not read; state 0 runs again now
  }
  for (;;) switch (t.state) {
    case 0:
      if (!t.bp || !UnitCanBuild(u, *t.bp)) return kTaskFailed;
      t.site = SnapStructurePosition(sim, *t.bp, t.site);
      if (Entity* p = FindSiteProp(sim, L, u, t)) {  // 0x5f6ea0: clear the site first
        BuildTask* c = NewChild(sim, CommandType::Reclaim, "Reclaim");
        c->goalId = EntityRef(p);
        t.sub = c;
        t.subFrom = sim.tick() + 1;
        return kTaskRunning;
      }
      if (CanMove(u)) {  // 0x5f7440 state 0: move when it does not fit, is out of range or stands on the site
        const NamedFootprint& sfp = Footprint(*t.bp);
        int sx0 = RoundEven(t.site.x - sfp.sizeX * 0.5f), sz0 = RoundEven(t.site.z - sfp.sizeZ * 0.5f);
        const NamedFootprint& ufp = Footprint(*u->blueprint);
        int ux0 = RoundEven(u->position.x - ufp.sizeX * 0.5f), uz0 = RoundEven(u->position.z - ufp.sizeZ * 0.5f);
        bool overlap = ux0 <= sx0 + sfp.sizeX && sx0 <= ux0 + ufp.sizeX && uz0 <= sz0 + sfp.sizeZ && sz0 <= uz0 + ufp.sizeZ;
        if (!UnitFitsAt(sim, u, u->position.x, u->position.z) || !InBuildRange(u, *t.bp, t.site) || overlap) {
          float excl[4] = {static_cast<float>(sx0) - 1.0f, static_cast<float>(sz0) - 1.0f,
                           static_cast<float>(sx0 + sfp.sizeX) + 1.0f, static_cast<float>(sz0 + sfp.sizeZ) + 1.0f};
          Vec3 a = t.site;
          PrepareMoveFor(sim, u, &a, excl, 1);  // spacing 1 (air_nav.md 7.1)
          int x0 = static_cast<int>(std::nearbyint(a.x - ufp.sizeX * 0.5f));
          int z0 = static_cast<int>(std::nearbyint(a.z - ufp.sizeZ * 0.5f));
          int rr[4] = {x0, z0, x0 + ufp.sizeX, z0 + ufp.sizeZ};
          GroundReserveRect(sim, u, rr);
          MoveToward(sim, u, Vec3{x0 + ufp.sizeX * 0.5f, a.y, z0 + ufp.sizeZ * 0.5f});
          t.moving = true;
        }
      }
      t.state = 1;
      return kTaskRunning;
    case 1:
      if (t.moving) {  // the move child: state 1 runs in the beat it ends
        if (u->motion.hasGoal && !u->motion.arrived && !u->motion.failed) return kTaskRunning;
        t.moving = false;
        GroundFreeRect(sim, u);
      }
      if (!InBuildRange(u, *t.bp, t.site)) return kTaskFailed;  // no re-move
      StopMoving(u);
      {  // aim the build arm at the middle of the structure (0x5f78bf)
        const UnitBpData& sd = GetUnitBpData(L, *t.bp);
        SetArmAimTarget(sim, u, Vec3{t.site.x, t.site.y + sd.sizeY * 0.5f + sd.collisionOffsetY, t.site.z});
      }
      u->unitStates.insert("Building");
      t.state = 2;
      continue;  // return 0
    case 2:  // NeedToFaceTargetToBuild units turn on the spot first (TODO: no FA structure builder sets it)
      t.state = 3;
      continue;  // return 0
    case 3: {
      if (u->paused) return kTaskRunning;      // (return 10)
      if (sim.tick() < t.waitUntil) return kTaskRunning;
      if (Unit* ex = ExistingAtSite(sim, u, *t.bp, t.site)) {  // join: no arm wait, work this beat
        SetFocus(sim, L, u, ex, t);
        t.state = 4;
        continue;
      }
      if (!LocationIsFree(sim, *t.bp, t.site.x, t.site.z)) return kTaskFailed;
      if (!u->armReady) return kTaskRunning;  // 0x5f7ba7: wait for the build arm (after the location checks)
      const UnitBpData& d = GetUnitBpData(L, *t.bp);
      if (!UnderUnitCap(u->army, d)) {
        if (++t.tries > 10) return kTaskFailed;
        t.waitUntil = sim.tick() + 10;
        return kTaskRunning;
      }
      float h = dmath::Atan2(u->position.x - t.site.x, u->position.z - t.site.z);  // (structures face the builder? kept level)
      (void)h;
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, t.site, Quat{}, false, u);
      if (Entity* w = t.rebuildWreck ? sim.FindEntity(t.rebuildWreck) : nullptr)  // the rebuilt wreck goes
        if (!w->destroyQueued) sim.QueueDestroy(w);
      t.rebuildWreck = 0;
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
      t.completed = true;
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

// CFactoryBuildTask::Execute 0x5fa790 (factory_handoff.md 1). Waits ("return r") are kept in waitUntil.
int TickFactoryBuild(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (sim.tick() < t.waitUntil) return kTaskRunning;
  auto wait = [&](int r) {
    t.waitUntil = sim.tick() + static_cast<uint32_t>(r - 1);
    return kTaskRunning;
  };
  switch (t.state) {
    case 0: {
      if (!t.bp || !UnitCanBuild(u, *t.bp)) return kTaskFailed;
      const UnitBpData& d = GetUnitBpData(L, *t.bp);
      if (!UnderUnitCap(u->army, d)) {
        if (++t.tries > 10) return kTaskFailed;
        return wait(1);
      }
      if (u->paused) return wait(10);
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, u->position, u->orientation, false, u);
      if (!nu || !Alive(nu)) return wait(10);
      nu->fireState = u->fireState;
      SetFocus(sim, L, u, nu, t);
      u->unitStates.insert("Building");
      t.state = 1;
      [[fallthrough]];
    }
    case 1: {
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) {  // helper not good: OnStopBuild(false), build again after 10 ticks
        StopBuild(sim, L, u, t, false);
        u->unitStates.erase("Building");
        t.state = 0;
        return wait(10);
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      target = FindUnit(sim, t.targetId);  // (read before OnStopBuild clears the focus)
      StopBuild(sim, L, u, t, true);       // Lua F:OnStopBuild(u): IssueMoveOffFactory, FinishBuildThread
      u->unitStates.erase("Building");
      t.state = 2;
      if (target) FactoryHandOff(sim, u, target, FindUnit(sim, t.inheritFrom));
      // state 2 (same pass): no rebuild -> workProgress 0, state 3, return 1
      u->workProgress = 0;
      t.state = 3;
      return wait(1);
    }
    case 3:
      if (u->busy) return wait(10);  // the Lua roll-off keeps the factory Busy
      t.completed = true;
      return kTaskDone;
  }
  return kTaskFailed;
}

int TickUpgrade(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  switch (t.state) {
    case 0: {
      if (!t.bp) return kTaskFailed;
      if (sim.tick() < t.waitUntil) return kTaskRunning;  // (restarting after a lost upgrade unit)
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
      if (!target) {  // CUnitUpgradeTask state 2 (0x5f8b2a): fail, then start over after 10 ticks
        StopBuild(sim, L, u, t, false);
        t.targetId = 0;
        t.state = 0;
        t.waitUntil = sim.tick() + 10;
        return kTaskRunning;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      u->unitStates.erase("Upgrading");
      t.completed = true;
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

int TickRepair(Sim& sim, lua_State* L, Unit* u, BuildTask& t);  // (below: CUnitRepairTask)

// BuildAssist / AssistCommander (not CUnitGuardTask; the original's are not read yet): follow the
// assisted unit; work on what it builds or repairs, or repair it.
int TickAssistLegacy(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Unit* g = FindUnit(sim, t.goalId);
  if (!g) {
    if (t.started) StopBuild(sim, L, u, t, true);
    u->unitStates.erase("Repairing");
    return kTaskDone;
  }
  if (u->guardedId != EntityRef(g)) SetGuardedUnit(sim, u, g);
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
    if (!t.started || t.targetId != EntityRef(work)) {
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
      u->focusId = EntityRef(e);
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
  t->unitId = EntityRef(u);
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

// ---- the guard task (CUnitGuardTask 0x6111e0 / 0x6141a0; engine-ref/specs/guard_task.md) ---------
//
// One task per Guard command. Every run (every 6 ticks; every tick for an engineer assisting a factory
// or an engineer) it re-reads the guarded unit, ends when that unit is gone, and otherwise takes the
// first of: refuel at a staging platform, ferry (a transport guarding a factory or a beacon), the
// assisted factory's build orders (a factory guarding a factory), an enemy within GuardScanRadius,
// the guarded builder's structure, what the guarded unit reclaims, something to repair or assist;
// with nothing to do it keeps station. Work is done by child tasks (the guard waits for them).

namespace combat {
bool IsAlly(const Army* a, const Army* b);
}

struct GuardData {
  bool hasUnit = false, factoryGuard = false, engAssistFactory = false, engAssistEngineer = false;
  bool ferryFactory = false, ferryTarget = false, endWhenIdle = false;
  uint32_t G = 0;                     // +0x7c
  Vec3 pos;                           // +0x84
  Vec3 anchor;                        // +0x90 (zero: none)
  int goal[4] = {0, 0, 0, 0};         // +0x9c: the goal last handed to the navigator
  bool goalSet = false;
  std::weak_ptr<UnitCommand> cmd;     // +0x4c: the Guard command
  std::weak_ptr<UnitCommand> ownCmd;  // +0x44: an own-queue factory command being built
  BuildTask* child = nullptr;         // a pushed child task (its parent: the guard or the dispatcher)
  bool childToGuard = false;          // the child's result lands in the guard (+0x2c)
  uint32_t childFrom = 0;             // its first tick (the thread's wait counter)
  bool childMove = false;             // state 0's move task (the thread is suspended until it ends)
  int result = 0;                     // +0x2c: 1 success, 2 failure
};

namespace {

bool IsZero(const Vec3& v) { return v.x == 0 && v.y == 0 && v.z == 0; }
bool Cat(Sim& sim, const Unit* u, const char* c) { return u && BpInCategory(sim, u->blueprint, c); }
bool Mobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }
bool Moved(const Unit* u) {
  return u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y ||
         u->position.z != u->lastPosition.z;
}
const NamedFootprint& Fp(const Unit* u) { return Footprint(*u->blueprint); }

// RUnitBlueprint::GetSkirtRect 0x51ec50 at (x, z)
void SkirtRect(const Unit* B, float excl[4]) {
  const NamedFootprint& bf = Fp(B);
  float bx0 = static_cast<float>(static_cast<int>(std::nearbyint(B->position.x - bf.sizeX * 0.5f)));
  float bz0 = static_cast<float>(static_cast<int>(std::nearbyint(B->position.z - bf.sizeZ * 0.5f)));
  const UnitBpData& d = *B->bpData;
  if (d.skirtSizeX == 0) {
    excl[0] = bx0;
    excl[2] = bx0 + bf.sizeX;
  } else {
    excl[0] = bx0 + d.skirtOffsetX;
    excl[2] = excl[0] + d.skirtSizeX;
  }
  if (d.skirtSizeZ == 0) {
    excl[1] = bz0;
    excl[3] = bz0 + bf.sizeZ;
  } else {
    excl[1] = bz0 + d.skirtOffsetZ;
    excl[3] = excl[1] + d.skirtSizeZ;
  }
}

bool PrepareMoveFor(Sim& sim, Unit* u, Vec3* p, const float excl[4], int spacing) {
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) return AirPrepareMove(sim, u, p);
  return GroundPrepareMove(sim, u, p, excl, spacing);
}

// a Lua method returning a boolean (RunScript_Bool 0x5f48a0)
bool ScriptBool(Sim& sim, Unit* u, const char* method) {
  lua_State* L = sim.L();
  if (!u->HasLuaObject()) return false;
  int top = lua_gettop(L);
  PushObject(L, u);
  lua_pushstring(L, method);
  lua_gettable(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return false;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, -2);
  lua_pushvalue(L, top + 1);
  bool r = false;
  if (lua_pcall(L, 1, 1, top + 2) == 0) r = lua_toboolean(L, -1) != 0;
  else LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  lua_settop(L, top);
  return r;
}

BuildTask* NewChild(Sim& sim, CommandType type, const char* order) {
  auto t = std::make_unique<BuildTask>();
  t->type = type;
  t->order = order;
  t->child = true;
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int NavStatus(const Unit* u) {  // navigator GetStatus: 0 idle, 1 thinking, 2 steering
  if (!u->motion.hasGoal) return 0;
  if (u->motion.navDriven) return LandNavStatus(u);
  return 2;
}

// GuardAbort 0x614170: GuardBusy, and the navigator's AbortMove
void GuardAbort(Unit* u) {
  u->unitStates.insert("GuardBusy");
  if (u->motion.hasGoal) MotionStop(u);
}

// RefreshGuardedUnitFromTarget 0x611a40 (never clears anything)
void Refresh(Sim& sim, Unit* U, GuardData& g, const UnitCommand& c) {
  Unit* gu = c.targetId ? FindUnit(sim, c.targetId) : nullptr;
  if (gu && (Mobile(gu) || !Cat(sim, U, "REBUILDER"))) {
    g.hasUnit = true;
    g.G = EntityRef(gu);
    if (Cat(sim, U, "TRANSPORTATION")) {
      if (Cat(sim, gu, "FACTORY") && gu->bpData && gu->bpData->hasBuilder && !Mobile(gu)) g.ferryFactory = true;
      else if (Cat(sim, gu, "TRANSPORTATION") || Cat(sim, gu, "FERRYBEACON")) g.ferryTarget = true;
    }
    SetGuardedUnit(sim, U, gu);
  }
  g.pos = c.targetId ? (gu ? gu->position : c.pos) : c.pos;
}

// EnsureReservedGuardMoveAnchorPosition 0x611da0
Vec3 EnsureAnchor(Sim& sim, Unit* U, GuardData& g) {
  if (IsZero(g.anchor)) {
    if (Unit* G = FindUnit(sim, g.G)) {
      g.anchor = G->position;
      float excl[4];
      SkirtRect(G, excl);
      PrepareMoveFor(sim, U, &g.anchor, excl);
    }
    const NamedFootprint& fp = Fp(U);
    int x0 = static_cast<int>(std::nearbyint(g.anchor.x - fp.sizeX * 0.5f));
    int z0 = static_cast<int>(std::nearbyint(g.anchor.z - fp.sizeZ * 0.5f));
    int r[4] = {x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ};
    GroundReserveRect(sim, U, r);
  }
  return g.anchor;
}

// ResolveGuardReferencePosition 0x612220. The guard formation (G+0x520) is not carried out: a land
// guard uses G's position instead of its formation slot.
Vec3 ReferencePos(Sim& sim, Unit* U, GuardData& g) {
  Unit* G = g.hasUnit ? FindUnit(sim, g.G) : nullptr;
  if (G) {
    if (Cat(sim, U, "ENGINEER")) return EnsureAnchor(sim, U, g);
    if (G->guardForm) return Cat(sim, U, "AIR") ? G->position : U->formSlot;  // land: the formation slot
    if (Mobile(G)) return U->position;  // (no formation yet)
  }
  if (auto c = g.cmd.lock()) {
    const NamedFootprint& fp = Fp(U);
    int cx = static_cast<int>(std::nearbyint(c->pos.x - fp.sizeX * 0.5f));
    int cz = static_cast<int>(std::nearbyint(c->pos.z - fp.sizeZ * 0.5f));
    return Vec3{cx + fp.sizeX * 0.5f, c->pos.y, cz + fp.sizeZ * 0.5f};
  }
  return g.pos;
}

// UpdateGuardFollowMoveGoal 0x613c40: keep station (the navigator's goal; no move task)
void Follow(Sim& sim, Unit* U, GuardData& g) {
  if (!Mobile(U) || U->immobile) return;
  Unit* G = g.hasUnit ? FindUnit(sim, g.G) : nullptr;
  if (Cat(sim, U, "ENGINEER") && G) {
    float dx, dz;
    if (Moved(G) && !IsZero(g.anchor)) {
      dx = g.anchor.x - G->position.x;
      dz = g.anchor.z - G->position.z;
    } else {
      dx = U->position.x - G->position.x;
      dz = U->position.z - G->position.z;
    }
    float d = std::sqrt(dx * dx + dz * dz);
    double mbd = U->bpData ? U->bpData->maxBuildDistance : 0;
    if (mbd + mbd > d) return;
    if (!IsZero(g.anchor)) {
      g.anchor = {};
      GroundFreeRect(sim, U);
    }
  }
  Vec3 ref = ReferencePos(sim, U, g);
  float excl[4] = {0, 0, 0, 0};
  if (G) SkirtRect(G, excl);
  bool forced = false;
  if (!(U->motion.bp->motionType == kMotionAir && Cat(sim, U, "EXPERIMENTAL"))) {
    if (!PrepareMoveFor(sim, U, &ref, excl)) {
      const TerrainMap* m = sim.map();
      float w = m ? static_cast<float>(m->width()) : ref.x, h = m ? static_cast<float>(m->height()) : ref.z;
      ref.x = std::min(std::max(ref.x, 0.0f), w);
      ref.z = std::min(std::max(ref.z, 0.0f), h);
      forced = true;
    }
  }
  const NamedFootprint& fp = Fp(U);
  int cx = static_cast<int>(std::nearbyint(ref.x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(ref.z - fp.sizeZ * 0.5f));
  int goal[4] = {cx, cz, cx + 1, cz + 1};
  int ux = static_cast<int>(std::nearbyint(U->position.x - fp.sizeX * 0.5f));
  int uz = static_cast<int>(std::nearbyint(U->position.z - fp.sizeZ * 0.5f));
  if (goal[0] <= ux && ux <= goal[2] && goal[1] <= uz && uz <= goal[3]) return;
  int st = NavStatus(U);
  if (st == 1) return;
  if (!(G && Mobile(G)) && st != 0) return;
  bool same = g.goalSet && std::equal(goal, goal + 4, g.goal);
  if (same && !forced && st == 2) return;
  Vec3 p{cx + fp.sizeX * 0.5f, ref.y, cz + fp.sizeZ * 0.5f};
  MotionSetGoal(sim, U, std::vector<Vec3>{p}, U->motion.bp->motionType == kMotionAir, sim.tick());
  std::copy(goal, goal + 4, g.goal);
  g.goalSet = true;
  AirSpeedThroughEvent(sim, U);
}

// IsOutsideGuardReferenceRange 0x612480
bool OutsideRange(Sim& sim, Unit* U, GuardData& g, float r) {
  Unit* G = g.hasUnit ? FindUnit(sim, g.G) : nullptr;
  float size = G ? static_cast<float>(std::max(Fp(G).sizeX, Fp(G).sizeZ)) : 1.0f;
  Vec3 ref = ReferencePos(sim, U, g);
  double dx = U->position.x - ref.x, dy = U->position.y - ref.y, dz = U->position.z - ref.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz) > size + r;
}

// ResolveGuardCommandSourceUnit 0x611cd0: a factory with work along U's guard chain
Unit* CommandSourceUnit(Sim& sim, Unit* U) {
  std::set<Unit*> visited;
  Unit* u = U;
  while (u && !visited.count(u)) {
    int n = 0, total = 0;
    for (auto& c : u->commands)
      if (c && c->type == CommandType::BuildFactory) {
        ++n;
        total += c->count;
      }
    if ((u == U && n > 0) || n > 1 || total > 1) return u;
    visited.insert(u);
    u = FindUnit(sim, u->guardedId);
  }
  return U;
}

// TryDispatchFactoryOrUpgradeFromGuardQueues 0x6127f0 (a factory assisting a factory)
BuildTask* FactoryWork(Sim& sim, Unit* U, GuardData& g, Unit* G) {
  if (!G || U->unitStates.count("Building") || U->unitStates.count("Repairing")) return nullptr;
  for (auto& c : U->commands) {
    if (!c || (c->type != CommandType::BuildFactory && c->type != CommandType::Upgrade)) continue;
    const BlueprintInfo* bp = sim.blueprints().Find(c->blueprintId);
    if (!bp) continue;
    g.ownCmd = c;
    BuildTask* t = NewChild(sim, c->type, c->type == CommandType::Upgrade ? "Upgrade" : "FactoryBuild");
    t->bp = bp;
    return t;
  }
  const int n = static_cast<int>(G->commands.size());
  for (int i = 0; i < n; ++i) {
    auto c = G->commands[static_cast<size_t>(i)];
    if (!c || c->type != CommandType::BuildFactory) continue;
    if (i == 0 && c->count <= 1) continue;  // leave G's own head item to G (no repeat queues)
    const BlueprintInfo* bp = sim.blueprints().Find(c->blueprintId);
    if (!bp || !UnitCanBuild(U, *bp)) continue;
    if (c->count > 1) {
      --c->count;
    } else {
      G->commands.erase(G->commands.begin() + i);
      ReleaseCommand(G, *c);
    }
    BuildTask* t = NewChild(sim, CommandType::BuildFactory, "FactoryBuild");
    t->bp = bp;
    t->inheritFrom = EntityRef(G);
    return t;
  }
  return nullptr;
}

// TryResolveGuardBuildBlueprint 0x612bb0: the structure the root of G's guard chain is building
const BlueprintInfo* GuardBuildBp(Sim& sim, Unit* U, GuardData& g, Vec3* site) {
  if (!U->bpData || !U->bpData->hasBuilder || Cat(sim, U, "REBUILDER")) return nullptr;  // (rebuild lists: not yet)
  Unit* R = FindUnit(sim, g.G);
  if (!R) return nullptr;
  std::set<Unit*> visited;
  while (Unit* n = FindUnit(sim, R->guardedId)) {
    if (visited.count(R)) return nullptr;
    visited.insert(R);
    R = n;
  }
  if (R->commands.empty() || !R->commands.front() || R->commands.front()->type != CommandType::BuildMobile) return nullptr;
  const UnitCommand& c = *R->commands.front();
  const BlueprintInfo* bp = sim.blueprints().Find(c.blueprintId);
  if (!bp || !UnitCanBuild(U, *bp)) return nullptr;
  const UnitBpData& bd = GetUnitBpData(sim.L(), *bp);
  if (!bd.structure) return nullptr;  // structures only
  const NamedFootprint& fp = Footprint(*bp);
  int cx = static_cast<int>(std::nearbyint(c.pos.x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(c.pos.z - fp.sizeZ * 0.5f));
  *site = Vec3{cx + fp.sizeX * 0.5f, c.pos.y, cz + fp.sizeZ * 0.5f};
  return bp;
}

// SelectAssistOrCaptureCandidateUnit 0x613110
Unit* AssistCandidate(Sim& sim, Unit* U, GuardData& g) {
  bool rebuilder = Cat(sim, U, "REBUILDER");
  if (!rebuilder && !Cat(sim, U, "REPAIR")) return nullptr;
  Unit* G = g.hasUnit ? FindUnit(sim, g.G) : nullptr;
  if (g.hasUnit && G) {
    if (Moved(G)) return nullptr;
    bool shieldOn = ScriptBool(sim, G, "ShieldIsOn");
    if (G->maxHealth > G->health) return G;
    if (G->motion.fuelUseTime > 1.0f && 1.0f > G->fuelRatio) return G;
    if (shieldOn && Cat(sim, G, "SHIELD"))
      if (Entity* s = sim.FindEntity(G->focusId))
        if (s->maxHealth > s->health) return G;
    if (!G->paused && G->unitStates.count("Enhancing")) return G;
    Entity* f = G->focusId ? sim.FindEntity(G->focusId) : nullptr;
    if (f && !G->unitStates.count("Reclaiming")) {
      if (f->kind != Entity::Kind::Unit) return nullptr;
      Unit* fu = static_cast<Unit*>(f);
      if (fu->army != U->army) return fu;
      if (fu->maxHealth > fu->health) return fu;
      if (fu->paused) return nullptr;
      if (fu->unitStates.count("Enhancing")) return fu;
      if (fu->unitStates.count("SiloBuildingAmmo")) return fu;
      return nullptr;
    }
    if (G->paused) return nullptr;
    return G->unitStates.count("SiloBuildingAmmo") ? G : nullptr;
  }
  if (g.engAssistFactory || g.engAssistEngineer) return nullptr;
  float r = GuardScanRadiusOf(U);
  Unit* best = nullptr;
  float bestD2 = std::numeric_limits<float>::infinity();
  for (Unit* u : sim.units()) {
    if (!Alive(u) || u == U || !u->blueprint) continue;
    float rad = static_cast<float>(std::max(Fp(u).sizeX, Fp(u).sizeZ)) * 0.5f;
    float ex = u->position.x - g.pos.x, ey = u->position.y - g.pos.y, ez = u->position.z - g.pos.z;
    if (std::sqrt(ex * ex + ey * ey + ez * ez) > r + rad) continue;  // (collision primitive vs sphere)
    if (Moved(u) || u->layer == "Air") continue;
    Unit* cand = nullptr;
    if (rebuilder) continue;  // (rebuilders: guard-command positions, not carried out)
    if (u->army != U->army && !combat::IsAlly(u->army, U->army)) continue;
    if (u->beingBuilt || u->health < u->maxHealth) cand = u;
    if (!cand) {
      Entity* f = u->focusId ? sim.FindEntity(u->focusId) : nullptr;
      if (f && f->kind == Entity::Kind::Unit) cand = static_cast<Unit*>(f);
      else if (u->unitStates.count("Enhancing") || u->unitStates.count("SiloBuildingAmmo")) cand = u;
      else continue;
    }
    float dx = cand->position.x - U->position.x, dy = cand->position.y - U->position.y, dz = cand->position.z - U->position.z;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (bestD2 > d2) {
      best = cand;
      bestD2 = d2;
    }
  }
  return best;
}

// ---- CUnitRepairTask (ids_repair_placement.md 2) --------------------------------------------------

// ctor 0x5f8c80
void InitRepair(Sim& sim, Unit* B, BuildTask& t, Unit* target, bool silo) {
  t.order = "Repair";
  t.goalId = EntityRef(target);
  t.silo = silo;
  t.assist = B->unitStates.count("Guarding") || B->unitStates.count("AssistingCommander");
  if (target && target->unitStates.count("Enhancing")) {
    PushObject(sim.L(), target);
    sim.CallMethod(sim.L(), B, "InheritWork", 1);
    t.inheritWork = true;
  }
}

// NothingToRepair 0x5f9230
bool NothingToRepair(Sim& sim, Unit* t) {
  if (!t) return true;
  if (t->motion.fuelUseTime > 0 && t->fuelRatio < 1.0f) return false;
  if (Cat(sim, t, "SHIELD"))
    if (Entity* f = sim.FindEntity(t->focusId))
      if (f->health < f->maxHealth) return false;
  return true;
}

// gap(R): distance - the builder's footprint - R's skirt
float RepairGap(const Unit* B, const Unit* R) {
  float dx = B->position.x - R->position.x, dz = B->position.z - R->position.z;
  const NamedFootprint& bf = Fp(B);
  return std::sqrt(dx * dx + dz * dz) - static_cast<float>(std::max(bf.sizeX, bf.sizeZ)) -
         std::max(R->bpData->skirtSizeX, R->bpData->skirtSizeZ);
}

Unit* FactoryOf(Sim& sim, Unit* t) {  // a unit being built inside a factory: the factory
  if (!t->beingBuilt) return nullptr;
  Unit* c = FindUnit(sim, t->creatorId);
  return c && Cat(sim, c, "FACTORY") ? c : nullptr;
}

// TaskTick 0x5f9370. "return 0" (go on in the same dispatch) is the loop; return 1 / 10 wait.
int TickRepair(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (t.moving) {  // the move child: the task runs again when it ends
    if (u->motion.hasGoal) return kTaskRunning;
    t.moving = false;
    u->unitStates.erase("Moving");
  } else if (sim.tick() < t.waitUntil) {
    return kTaskRunning;
  }
  const float mbd = u->bpData->maxBuildDistance;
  for (int guard = 0; guard < 8; ++guard) {
    Unit* tg = FindUnit(sim, t.goalId);
    if (!tg || tg == u) return kTaskFailed;
    if (tg->health == tg->maxHealth) {
      if (!t.assist) {
        if ((!tg->beingBuilt || tg->fractionComplete == 1.0f) && NothingToRepair(sim, tg)) return kTaskFailed;
      } else if (t.state < 4 && !t.silo && !tg->unitStates.count("Upgrading") && NothingToRepair(sim, tg)) {
        return kTaskFailed;
      }
    }
    const bool flies = tg->motion.air && tg->motion.air->bp && tg->motion.air->bp->canFly;
    if (!flies || tg->beingBuilt) {
      if (Moved(tg)) return kTaskFailed;  // the target moved this tick
    } else if (tg->layer == "Air") {
      return kTaskFailed;
    }
    Unit* R = tg;
    switch (t.state) {
      case 0: {
        if (t.assist) {
          if (Unit* f = FactoryOf(sim, tg)) {
            R = f;
          } else if (tg->unitStates.count("Upgrading")) {
            if (Unit* f2 = FindUnit(sim, tg->focusId)) {
              t.goalId = EntityRef(f2);
              tg = R = f2;
            }
          }
        }
        t.state = 1;
        if (!t.noMove && CanMove(u) && (!UnitFitsAt(sim, u, u->position.x, u->position.z) || RepairGap(u, R) > mbd)) {
          float excl[4];
          SkirtRect(R, excl);
          excl[0] -= 1.0f;
          excl[1] -= 1.0f;
          excl[2] += 1.0f;
          excl[3] += 1.0f;
          Vec3 a = R->position;
          PrepareMoveFor(sim, u, &a, excl, 1);  // spacing 1 (air_nav.md 7.1)
          const NamedFootprint& fp = Fp(u);
          int x0 = static_cast<int>(std::nearbyint(a.x - fp.sizeX * 0.5f));
          int z0 = static_cast<int>(std::nearbyint(a.z - fp.sizeZ * 0.5f));
          int rr[4] = {x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ};
          GroundReserveRect(sim, u, rr);
          TaskMoveToward(sim, u, Vec3{x0 + fp.sizeX * 0.5f, a.y, z0 + fp.sizeZ * 0.5f});
          u->unitStates.insert("Moving");
          t.moving = true;
          return kTaskRunning;
        }
        continue;
      }
      case 1: {
        t.state = 2;
        if (!t.noMove) {
          GroundFreeRect(sim, u);
          if (Unit* f = FactoryOf(sim, tg)) R = f;
          if (RepairGap(u, R) > mbd) return kTaskFailed;  // could not get in range
        }
        StopMoving(u);
        t.workId = t.goalId;
        if (Unit* w = FindUnit(sim, t.workId)) SetArmAimTarget(sim, u, w->position);
        [[fallthrough]];
      }
      case 2:
        // NeedToFaceTargetToBuild: turn until dot(forward, toward R) > 0.95 (the turn itself: not yet)
        t.state = 3;
        continue;
      case 3: {
        if (!u->armReady) return kTaskRunning;
        if (u->paused) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        Unit* w = FindUnit(sim, t.workId);
        if (!w) return kTaskFailed;
        w->unitStates.insert("NoReclaim");
        SetFocus(sim, L, u, w, t);
        u->unitStates.insert("Repairing");
        t.state = 4;
        continue;
      }
      case 4: {
        if (!tg->unitStates.count("Attached") && RepairGap(u, tg) > 2.0f * mbd) return kTaskFailed;
        if (!UpdateWorkProgress(sim, L, u, t)) {
          Unit* w = FindUnit(sim, t.workId);
          if (!t.inheritWork || !w || w->unitStates.count("Enhancing")) return kTaskRunning;
        }
        t.state = 5;
        t.completed = true;
        return kTaskDone;
      }
      default:
        return kTaskFailed;
    }
  }
  return kTaskRunning;
}

// DispatchAssistOrCaptureTask 0x613a80 (capture: not carried out yet)
BuildTask* AssistTask(Sim& sim, Unit* U, Unit* c) {
  bool shieldOn = ScriptBool(sim, c, "ShieldIsOn");
  bool ally = c->army == U->army || combat::IsAlly(U->army, c->army);
  if (!ally) return nullptr;
  bool shieldDamaged = false;
  if (shieldOn && Cat(sim, c, "SHIELD"))
    if (Entity* s = sim.FindEntity(c->focusId)) shieldDamaged = s->maxHealth > s->health;
  if (c->maxHealth > c->health || c->unitStates.count("Enhancing") || shieldDamaged ||
      c->unitStates.count("SiloBuildingAmmo")) {
    if (!U->bpData || !U->bpData->hasBuilder) return nullptr;
    BuildTask* t = NewChild(sim, CommandType::Repair, "Repair");
    InitRepair(sim, U, *t, c, c->unitStates.count("SiloBuildingAmmo") != 0);
    return t;
  }
  return nullptr;
}

// ShouldAbortGuardForBuilderContext 0x612600
bool ShouldAbort(Sim& sim, Unit* U, GuardData& g) {
  if (!(Cat(sim, U, "ENGINEER") || Cat(sim, U, "POD")) || Cat(sim, U, "REBUILDER")) return false;
  if (U->commands.size() >= 2 && U->commands[1]) return true;
  Unit* G = FindUnit(sim, g.G);
  if (g.endWhenIdle && G && G->commands.empty()) return true;
  return false;
}

}  // namespace

BuildTask* StartGuardTask(Sim& sim, Unit* u, const UnitCommand& c) {
  if (!c.targetId && !c.hasPos) return nullptr;  // no target: Stop
  if (c.targetId && !FindUnit(sim, c.targetId) && !c.hasPos) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Guard;
  t->order = "Guard";
  t->goalId = c.targetId;
  auto gd = std::make_shared<GuardData>();
  GuardData& g = *gd;
  u->unitStates.insert("Guarding");
  if (!u->commands.empty()) g.cmd = u->commands.front();
  Refresh(sim, u, g, c);
  Unit* G = FindUnit(sim, g.G);
  if (G && (G->beingBuilt || G->unitStates.count("Upgrading")) && !Cat(sim, G, "FACTORY") && !Cat(sim, G, "SHIELD") &&
      !Cat(sim, G, "SILO") && !Mobile(G))
    g.endWhenIdle = true;
  t->state = 3;
  if (!(g.ferryFactory || Cat(sim, u, "NOFORMATION"))) {
    if (Cat(sim, u, "FACTORY") && !Mobile(u)) {
      g.factoryGuard = true;
    } else if (Cat(sim, u, "ENGINEER") && G) {
      if (Cat(sim, G, "ENGINEER")) {
        g.engAssistEngineer = true;
      } else if (Cat(sim, G, "FACTORY")) {
        g.engAssistFactory = true;
        t->state = 0;
      }
    }
  }
  t->gdata = gd;
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

// CUnitGuardTask::TaskTick 0x6141a0
int TickGuard(Sim& sim, lua_State* L, Unit* U, BuildTask& t) {
  (void)L;
  GuardData& g = *t.gdata;
  bool resumed = false;
  if (g.child) {  // the child runs; when it ends the guard runs again in the same tick
    if (sim.tick() < g.childFrom) return kTaskRunning;
    BuildTask* c = g.child;
    int r = TickBuildTask(sim, U, *c);
    if (r == kTaskRunning) return kTaskRunning;
    if (g.childToGuard) g.result = r == kTaskDone ? 1 : 2;
    g.child = nullptr;
    resumed = true;
  } else if (g.childMove) {
    if (U->motion.hasGoal) return kTaskRunning;
    g.childMove = false;
    U->unitStates.erase("Moving");
    resumed = true;
  }
  if (!resumed && sim.tick() < t.waitUntil) return kTaskRunning;
  // (a) an own-queue factory command finished
  if (auto c = g.ownCmd.lock()) {
    if (g.result == 1) {
      if (c->count > 1) {
        --c->count;
      } else {
        auto it = std::find(U->commands.begin(), U->commands.end(), c);
        if (it != U->commands.end()) {
          U->commands.erase(it);
          ReleaseCommand(U, *c);
        }
      }
    }
    g.ownCmd.reset();
  }
  g.result = 0;
  // (b) a changed U+0x4e0 (upgrade hand-over)
  if (g.G != U->guardedId) {
    g.G = U->guardedId;
    if (auto c = g.cmd.lock())
      if (g.G) c->targetId = g.G;
  }
  U->unitStates.erase("GuardBusy");
  // (c) validity
  Unit* G = FindUnit(sim, g.G);
  if (g.hasUnit) {
    if (!G || G->dead || G->destroyQueued || (G->unitStates.count("Attached") && !G->beingBuilt)) return kTaskDone;
  }
  // (d)
  Unit* src = g.factoryGuard ? CommandSourceUnit(sim, U) : G;
  if (g.hasUnit && G) g.pos = G->position;
  const float r = GuardScanRadiusOf(U);
  BuildTask* child = nullptr;
  bool toGuard = true;
  switch (t.state) {
    case 0: {  // an engineer assisting a factory walks next to it first
      Vec3 a = EnsureAnchor(sim, U, g);
      const NamedFootprint& fp = Fp(U);
      int cx = static_cast<int>(std::nearbyint(a.x - fp.sizeX * 0.5f)), cz = static_cast<int>(std::nearbyint(a.z - fp.sizeZ * 0.5f));
      TaskMoveToward(sim, U, Vec3{cx + fp.sizeX * 0.5f, a.y, cz + fp.sizeZ * 0.5f});
      U->unitStates.insert("Moving");
      g.childMove = true;
      t.state = 1;
      break;
    }
    case 1:  // the move ended
      GroundFreeRect(sim, U);
      t.state = 3;
      break;
    case 2:  // after a fight: come back first
      if (OutsideRange(sim, U, g, r * 0.5f)) Follow(sim, U, g);
      else t.state = 3;
      break;
    case 3: {
      if (Unit* P = FindPlatform(sim, U)) {
        GuardAbort(U);
        child = MakeRefuelTask(sim, U, P);
        toGuard = false;
        break;
      }
      if (g.ferryFactory || (g.ferryTarget && G && Cat(sim, G, "FERRYBEACON"))) {
        GuardAbort(U);
        child = MakeGuardFerryTask(sim, U, G);
        toGuard = false;
        break;
      }
      if (g.factoryGuard) {
        GuardAbort(U);
        child = FactoryWork(sim, U, g, src);
        break;
      }
      if (!g.engAssistFactory && !g.engAssistEngineer && !U->weapons.empty()) {
        if (Unit* e = GuardBestEnemy(sim, U)) {
          if (!IsZero(g.anchor)) {
            g.anchor = {};
            GroundFreeRect(sim, U);
          }
          GuardAbort(U);
          child = MakeAttackTaskOn(sim, U, e);
          t.state = 2;
          break;
        }
      }
      Vec3 site;
      if (const BlueprintInfo* bp = GuardBuildBp(sim, U, g, &site)) {
        g.anchor = {};
        GuardAbort(U);
        child = NewChild(sim, CommandType::BuildMobile, "MobileBuild");
        child->bp = bp;
        child->site = site;
        break;
      }
      if (g.hasUnit && G && U->bpData && U->bpData->hasBuilder && Cat(sim, U, "RECLAIM") &&
          G->unitStates.count("Reclaiming") && G->focusId) {
        GuardAbort(U);
        child = NewChild(sim, CommandType::Reclaim, "Reclaim");
        child->goalId = G->focusId;
        break;
      }
      if (Unit* c = AssistCandidate(sim, U, g)) {
        GuardAbort(U);
        child = AssistTask(sim, U, c);
        break;
      }
      if (ShouldAbort(sim, U, g)) return kTaskDone;
      Follow(sim, U, g);
      break;
    }
  }
  const int ret = (g.engAssistEngineer || g.engAssistFactory) && G ? 1 : 7;
  const uint32_t next = sim.tick() + static_cast<uint32_t>(ret >= 2 ? ret - 1 : 1);
  t.waitUntil = next;
  if (child) {
    g.child = child;
    g.childToGuard = toGuard;
    g.childFrom = next;
  }
  return kTaskRunning;
}

// CUnitGuardTask dtor 0x611850
void EndGuard(Sim& sim, Unit* U, BuildTask& t) {
  GuardData& g = *t.gdata;
  if (g.child) {
    EndBuildTask(sim, U, *g.child, false);
    g.child = nullptr;
  }
  if (!Alive(U)) return;
  U->unitStates.erase("GuardBusy");
  U->unitStates.erase("Guarding");
  if (g.childMove) U->unitStates.erase("Moving");
  SetGuardedUnit(sim, U, nullptr);
  if (!IsZero(g.anchor)) GroundFreeRect(sim, U);
  if (U->motion.bp && Moved(U) && U->motion.hasGoal) MotionStop(U);
  AirSpeedThroughEvent(sim, U);
}

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
      InitRepair(sim, u, *t, FindUnit(sim, c.targetId), false);
      t->goalId = c.targetId;
      break;
    case CommandType::Script:
      return StartScriptTask(sim, u, c);
    case CommandType::TransportLoadUnits:
    case CommandType::TransportUnloadUnits:
    case CommandType::TransportUnloadSpecificUnits:
    case CommandType::Ferry:
      return StartTransportTask(sim, u, c);
    case CommandType::Attack:
    case CommandType::FormAttack:
      return StartAttackTask(sim, u, c);
    case CommandType::Reclaim:
      if (!u->bpData || !u->bpData->hasBuilder || !c.targetId) return nullptr;
      t->order = "Reclaim";
      t->goalId = c.targetId;
      break;
    case CommandType::Guard:
      return StartGuardTask(sim, u, c);
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
    case CommandType::TransportLoadUnits:
    case CommandType::TransportUnloadUnits:
    case CommandType::TransportUnloadSpecificUnits:
    case CommandType::Ferry: r = TickTransportTask(sim, u, t); break;
    case CommandType::Guard: r = t.gdata ? TickGuard(sim, L, u, t) : kTaskFailed; break;
    case CommandType::BuildAssist:
    case CommandType::AssistCommander: r = TickAssistLegacy(sim, L, u, t); break;
    case CommandType::Attack:
    case CommandType::FormAttack: r = TickAttack(sim, L, u, t); break;
    case CommandType::Patrol:
    case CommandType::FormPatrol:
    case CommandType::AggressiveMove:
    case CommandType::FormAggressiveMove: r = t.pdata ? TickPatrol(sim, u, t) : kTaskFailed; break;
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
  if (t.sub) {  // a mobile build's site-clearing reclaim is popped first
    EndPropReclaim(sim, L, u, *t.sub);
    t.sub = nullptr;
  }
  if (t.scriptTask) {  // the task object's OnDestroy, then it is gone
    if (t.HasLuaObject()) sim.CallMethod(L, &t, "OnDestroy", 0);
    t.UnbindLua();
    return;
  }
  if (t.tdata) {
    EndTransportTask(sim, u, t, success);
    return;
  }
  if (t.gdata) {
    EndGuard(sim, u, t);
    return;
  }
  if (t.pdata) {
    EndPatrol(sim, u, t);
    return;
  }
  if (t.type == CommandType::Attack || t.type == CommandType::FormAttack) {
    EndAttack(sim, u, t);
    if (Alive(u)) StopMovingIfTask(u, t);
    return;
  }
  (void)success;  // build-like tasks end by their own state (the task dtors), not the caller's view
  if (t.type == CommandType::BuildMobile) {
    // CUnitMobileBuildTask dtor (0x5f6ac0): OnStopBuild(true) - the structure is left as it is -
    // then the builder's OnFailedToBuild when the task did not finish (also before it started).
    if (Alive(u)) {
      SetArmAimTarget(sim, u, Vec3{});
      if (t.started) StopBuild(sim, L, u, t, true);
      if (!t.completed) sim.CallMethod(L, u, "OnFailedToBuild", 0);
    }
  } else if (t.type == CommandType::BuildFactory) {
    // CFactoryBuildTask dtor (0x5fa010): OnStopBuild(true) when done, else OnStopBuild(false)
    // (a cancelled product gets OnFailedToBeBuilt; the factory script destroys it)
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, t.completed);
  } else if (t.type == CommandType::Upgrade) {
    // CUnitUpgradeTask dtor (0x5f84c0): done -> OnStopBuild(true); otherwise the engine destroys the
    // upgrade unit itself (its ground released, the builder's taken back), then OnStopBuild(false)
    u->unitStates.erase("Upgrading");
    if (t.completed) {
      if (t.started && Alive(u)) StopBuild(sim, L, u, t, true);
    } else {
      Unit* target = FindUnit(sim, t.targetId);
      if (target) {
        ReleaseStructure(sim, target);
        sim.QueueDestroy(target);
        bool occupied = false;
        for (const auto& r : sim.navigation().Structures()) occupied = occupied || r.entity == EntityRef(u);
        if (!occupied && Alive(u)) OccupyStructure(sim, u);
      }
      if (t.started && Alive(u)) StopBuild(sim, L, u, t, false);
    }
  } else if (t.type == CommandType::Repair || t.type == CommandType::Guard || t.type == CommandType::BuildAssist ||
             t.type == CommandType::AssistCommander) {
    // the repair task's dtor (0x5f8e20): ClearWork, Repairing off, workProgress 0, the arm reset, the
    // reserved cell (still moving), NoReclaim off, OnStopBuild(true); never a failure
    if (t.type == CommandType::Repair) {
      if (Alive(u) && u->HasLuaObject()) sim.CallMethod(L, u, "ClearWork", 0);
      if (Alive(u)) {
        u->unitStates.erase("Repairing");
        u->workProgress = 0;
        SetArmAimTarget(sim, u, Vec3{});
        if (t.state == 1 && !t.noMove) GroundFreeRect(sim, u);
        if (t.moving) {
          t.moving = false;
          StopMoving(u);
        }
      }
      if (Unit* w = FindUnitAny(sim, t.workId)) w->unitStates.erase("NoReclaim");
    }
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, true);
  } else if (t.started && Alive(u)) {
    StopBuild(sim, L, u, t, success);
  }
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
    if (!t.child && (t.type == CommandType::BuildAssist || t.type == CommandType::AssistCommander)) {
      u->unitStates.erase("Guarding");  // (the legacy assist tasks; the guard's own dtor does this for it)
      u->unitStates.erase("AssistingCommander");
    }
    if (t.type != CommandType::BuildFactory) u->workProgress = 0;
    StopMovingIfTask(u, t);
  }
  if (!t.child && (t.type == CommandType::BuildAssist || t.type == CommandType::AssistCommander) && u->guardedId)
    SetGuardedUnit(sim, u, nullptr);
}

bool TaskCanMove(const Unit* u) { return CanMove(u); }
void TaskMoveToward(Sim& sim, Unit* u, const Vec3& goal) { MoveToward(sim, u, goal); }
void TaskStopMoving(Unit* u) { StopMoving(u); }

void StopMovingIfTask(Unit* u, const BuildTask& t) {
  if (t.type == CommandType::BuildFactory || t.type == CommandType::Upgrade) return;
  if (u->motion.hasGoal) MotionStop(u);
  u->unitStates.erase("Moving");
}

// ---- factories -----------------------------------------------------------------------------------

// ---- the factory's rally queue (CAiBuilderImpl, factory_handoff.md 3) and the hand-off (1.3, 2) ----

bool IsFactoryBuilder(Sim& sim, const Unit* u) {
  return u->bpData && u->bpData->hasBuilder && BpInCategory(sim, u->blueprint, "FACTORY");
}
bool IsImmobileFactory(Sim& sim, const Unit* u) {
  return BpInCategory(sim, u->blueprint, "FACTORY") && !(u->motion.bp && u->motion.bp->mobile());
}
void AddFactoryCommand(Unit* f, const std::shared_ptr<UnitCommand>& c) {  // vf+0x28 (append)
  if (!c->units.insert(f).second) return;
  f->factoryCommands.push_back(c);
}
void ClearFactoryCommandQueue(Unit* f) {  // vf+0x38: RemoveUnit from the back until empty
  while (!f->factoryCommands.empty()) {
    auto c = f->factoryCommands.back();
    f->factoryCommands.pop_back();
    ReleaseCommand(f, *c);
  }
}
// UNIT_IssueFactoryCommand 0x6f14d0: one command (flag +0x142) appended to every factory's rally queue.
std::shared_ptr<UnitCommand> IssueFactoryCommand(Sim& sim, const std::vector<Unit*>& units, CommandType type,
                                                 const Vec3& pos, uint32_t targetId, bool clear) {
  std::shared_ptr<UnitCommand> c;
  for (Unit* x : units) {
    if (x->dead || x->transportedBy) continue;
    if (!IsFactoryBuilder(sim, x)) continue;
    if (!c) {
      c = std::make_shared<UnitCommand>();
      c->id = sim.nextCommandId++;
      c->type = type;
      c->pos = pos;
      c->hasPos = true;
      c->targetId = targetId;
      c->factoryIssued = true;
      sim.commandsById[c->id] = c;
    }
    if (clear) ClearFactoryCommandQueue(x);
    AddFactoryCommand(x, c);
  }
  return c;
}

namespace {
Vec3 FactoryCmdPos(Sim& sim, const UnitCommand& c) {
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) return e->position;
  return c.pos;
}
// InheritQueuedCommandsTo 0x5fa340 / InheritCommandsTo 0x5fa550: the same command objects, appended;
// TransportLoadUnits skipped for air and naval units; the ferry case drops the leading Moves.
void InheritRally(Sim& sim, Unit* f, Unit* inheritFrom, Unit* u, bool ferry) {
  if (!f->bpData || !f->bpData->hasBuilder || (f->motion.bp && f->motion.bp->mobile())) return;
  std::vector<std::shared_ptr<UnitCommand>> L(f->factoryCommands.begin(), f->factoryCommands.end());
  if (inheritFrom && Alive(inheritFrom) && inheritFrom->bpData && inheritFrom->bpData->hasBuilder)
    L.insert(L.end(), inheritFrom->factoryCommands.begin(), inheritFrom->factoryCommands.end());
  bool airNaval = BpInCategory(sim, u->blueprint, "AIR") || BpInCategory(sim, u->blueprint, "NAVAL");
  bool leading = true;
  for (auto& c : L) {
    if (!c) continue;
    if (c->type == CommandType::TransportLoadUnits && airNaval) continue;
    if (ferry) {
      if (c->type == CommandType::Move) {
        if (leading) continue;  // the ferry flies these
      } else {
        leading = false;
      }
    }
    if (!c->units.insert(u).second) continue;  // CUnitCommand::AddUnit: once per unit
    u->commands.push_back(c);
    AirSpeedThroughEvent(sim, u);  // queue event 0
  }
}
}  // namespace

// CFactoryBuildTask finish block 0x5fa9cb: the root of the factory's guard chain, a ferrying
// transport among its guards (the first not Moving, else the first), then the unit's orders.
void FactoryHandOff(Sim& sim, Unit* f, Unit* u, Unit* inheritFrom) {
  Unit* R = f;
  for (;;) {
    Unit* g = FindUnit(sim, R->guardedId);
    if (!g || g == f) break;
    R = g;
  }
  Unit* chosen = nullptr;
  for (Unit* X : Guards(sim, R)) {
    if (!BpInCategory(sim, X->blueprint, "TRANSPORTATION") || !X->unitStates.count("Ferrying")) continue;
    if (!chosen || !X->unitStates.count("Moving")) chosen = X;
    if (!chosen->unitStates.count("Moving")) break;
  }
  if (chosen) {
    IssueTransportLoadOne(sim, u, R);
    InheritRally(sim, f, inheritFrom, u, true);
  } else {
    InheritRally(sim, f, inheritFrom, u, false);
  }
}

void SetGuardedUnit(Sim& sim, Unit* u, Unit* g) {
  if (Unit* old = FindUnitAny(sim, u->guardedId)) {
    auto& v = old->guarders;
    v.erase(std::remove(v.begin(), v.end(), EntityRef(u)), v.end());
    ReleaseGuardFormation(old);  // rebuilt at its next MotionTick
  }
  u->guardedId = g ? EntityRef(g) : 0;
  if (g) {
    ReleaseGuardFormation(g);
    auto& v = g->guarders;
    uint32_t id = EntityRef(u);
    auto it = std::lower_bound(v.begin(), v.end(), id);
    if (it == v.end() || *it != id) v.insert(it, id);
  }
}

// The units guarding g (unit+0x4f8), ascending entity id.
std::vector<Unit*> Guards(Sim& sim, const Unit* g) {
  std::vector<Unit*> out;
  for (uint32_t id : g->guarders)
    if (Unit* x = FindUnit(sim, id)) out.push_back(x);
  std::sort(out.begin(), out.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  return out;
}

// SetUpInitialRally 0x59eef0: position + local X * InitialRallyX + local Z * InitialRallyZ.
void SetUpInitialRally(Sim& sim, Unit* f) {
  const UnitBpData& d = *f->bpData;
  const Quat& q = f->orientation;
  float x = q.x, y = q.y, z = q.z, w = q.w;
  Vec3 ax{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
  Vec3 az{2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
  Vec3 t{ax.x * d.initialRallyX, ax.y * d.initialRallyX, ax.z * d.initialRallyX};
  Vec3 p{f->position.x + (az.x * d.initialRallyZ + t.x), f->position.y + (az.y * d.initialRallyZ + t.y),
         f->position.z + (az.z * d.initialRallyZ + t.z)};
  IssueFactoryCommand(sim, {f}, CommandType::Move, p, 0, true);
}

// ValidateFactoryCommandQueue 0x59f220 (end of every MotionTick): drop TransportLoadUnits whose
// target is gone or not a beacon / transport / staging platform; an empty queue gets the initial rally.
void ValidateFactoryCommandQueue(Sim& sim, Unit* f) {
  for (size_t i = 0; i < f->factoryCommands.size();) {
    auto c = f->factoryCommands[i];
    if (c->type == CommandType::TransportLoadUnits) {
      Unit* E = FindUnit(sim, c->targetId);
      if (!E || !(BpInCategory(sim, E->blueprint, "FERRYBEACON") || BpInCategory(sim, E->blueprint, "TRANSPORTATION") ||
                  BpInCategory(sim, E->blueprint, "AIRSTAGINGPLATFORM"))) {
        f->factoryCommands.erase(f->factoryCommands.begin() + static_cast<long>(i));
        ReleaseCommand(f, *c);
        continue;
      }
    }
    ++i;
  }
  if (f->factoryCommands.empty()) SetUpInitialRally(sim, f);
}

void UnitFinishedBuilding(Sim& sim, lua_State* L, Unit* u) {
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
  u->focusId = EntityRef(e);
  return 0;
}
int l_ClearFocusEntity(lua_State* L) {
  U(L)->focusId = 0;
  return 0;
}
int l_GetGuardedUnit(lua_State* L) {  // 0x6cd380: unit+0x4e0
  Unit* u = U(L);
  PushObject(L, FindUnit(*S(L), u->guardedId));
  return 1;
}
int l_GetGuards(lua_State* L) {  // 0x6cd4e0: unit+0x500, ascending entity id
  Unit* u = U(L);
  lua_newtable(L);
  int n = 0;
  for (Unit* o : Guards(*S(L), u)) {
    PushObject(L, o);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
// NotifyUpgrade(from, to) 0x6cce70: the upgrade takes over the old unit's guard links.
int l_NotifyUpgrade(lua_State* L) {
  Sim& sim = *S(L);
  Unit* from = ToObject<Unit>(L, 1);
  Unit* to = ToObject<Unit>(L, 2);
  if (!from || !to) return 0;
  SetGuardedUnit(sim, to, FindUnit(sim, from->guardedId));
  for (Unit* g : Guards(sim, from)) SetGuardedUnit(sim, g, to);
  return 0;
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
  u->parentId = EntityRef(parent);
  u->ownBone = BoneIndex(L, u, 2);
  u->parentBone = BoneIndex(L, parent, 4);
  u->unitStates.insert("Attached");
  if (u->motion.hasGoal) MotionStop(u);
  {
    Vec3 bp;
    Quat bq;
    BoneWorld(parent, u->parentBone, &bp, &bq);
    u->position = bp;
    u->orientation = bq;
  }
  S(L)->MarkUnitsMoved();
  return 0;
}
int l_AttachTo(lua_State* L) {  // entity:AttachTo(parent, bone)
  Unit* u = ToObject<Unit>(L, 1);
  Entity* parent = ToObject<Entity>(L, 2);
  if (!u || !parent) return 0;
  u->parentId = EntityRef(parent);
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
  {  // the motion's facing: the unit's heading now (CUnitMotion::NotifyDetached)
    Vec3 f = vm::Forward(u->orientation);
    float l = std::sqrt(f.x * f.x + f.z * f.z);
    if (l > 1e-6f) {
      u->motion.fx = u->motion.bx = f.x / l;
      u->motion.fz = u->motion.bz = f.z / l;
    }
  }
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
    if (o->parentId == EntityRef(e) && (bone == -2 || o->parentBone == bone)) Detach(*S(L), o);
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
// IssueFactoryRallyPoint(factories, target) 0x6f22c0: a Move (entity or position) appended to the rally queues.
int l_IssueFactoryRallyPoint(lua_State* L) {
  Sim& sim = *S(L);
  std::vector<Unit*> units;
  for (Unit* u : UnitList(L, 1))
    if (UnitCommandCaps(L, *u->blueprint) & 0x1u) units.push_back(u);  // RULEUCC_Move
  Vec3 p;
  uint32_t target = 0;
  if (Entity* e = ToObject<Entity>(L, 2)) {
    p = e->position;
    target = EntityRef(e);
  } else if (!PosArg(L, 2, &p)) {
    return 0;
  }
  auto c = IssueFactoryCommand(sim, units, CommandType::Move, p, target, false);
  if (!c) return 0;
  PushUnitCommand(L, c);
  return 1;
}
int l_IssueClearFactoryCommands(lua_State* L) {  // 0x6f2580 (the initial rally comes back next MotionTick)
  for (Unit* u : UnitList(L, 1))
    if (u->bpData && u->bpData->hasBuilder) ClearFactoryCommandQueue(u);
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
  c->factoryIssued = true;  // IssueMoveOffFactory 0x6f29d0 = IssueMove (append) + cmd+0x142
  for (Unit* u : units) {
    if (!c->units.insert(u).second) continue;
    u->commands.push_back(c);
    AirSpeedThroughEvent(*S(L), u);  // queue event 0
  }
  return 0;
}
int l_GetRallyPoint(lua_State* L) {  // 0x6d0650: the target of rally command 0, or nil
  Unit* u = U(L);
  if (!u->factoryCommands.empty()) {
    Vec3 p = FactoryCmdPos(*S(L), *u->factoryCommands.front());
    PushVector(L, p.x, p.y, p.z);
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
  SetGlobal(L, "NotifyUpgrade", l_NotifyUpgrade);
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
int l_IsPaused(lua_State* L) {  // exe 0x6c5f30: a destroyed unit is not paused (no error)
  if (lua_istable(L, 1) && !GetObject(L, 1)) {
    lua_rawgetcobject(L, 1);
    bool handle = lua_touserdata(L, -1) != nullptr;
    lua_pop(L, 1);
    if (handle) {
      lua_pushboolean(L, 0);
      return 1;
    }
  }
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
