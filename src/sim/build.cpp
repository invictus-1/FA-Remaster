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
//   occupying the footprint) and no structure's SKIRT overlaps the new skirt (so nothing can be set
//   inside a factory's square; two skirts may touch).
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
#include "sim/ai_build.h"
#include "sim/ai_platoon.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/formation.h"
#include "sim/intel.h"
#include "sim/transport.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/economy.h"
#include "sim/motion.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
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
  static const NamedFootprint one{"", 1, 1, 1, 0, 0, 0, 0};
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

// The deposits by cells of 32 x 32 map units (speed only, bind 2026-10-28; B1: 116 deposits looked at per test). Both
// tests below ask whether ANY deposit of a type meets a rect, so the answer does not depend on which deposits are
// looked at or in what order, only that every one that could meet the rect is: a deposit is listed in each cell its
// rect grown by 1.5 (the larger margin) touches, a test looks in each cell its rect touches (cell numbers clamped to
// the grid at both ends, so a rect or deposit past the map's edge falls into the edge cells, and two rects that share
// a point share a cell). Made again whenever the list's length, its last entry or the map's size is not what it was
// made for (the list only grows: CreateResourceDeposit appends, nothing edits or removes a deposit; the last entry's
// check catches a list replaced or cut and refilled to the same length). A rect that is no proper range
// (x0 > x1, NaN) takes the plain walk. (Two ranges [x0, x1] and [lo - 1.5, hi + 1.5] that both tests' conditions
// let meet share a point, for a deposit given either way round too: so they share a cell.)
struct DepositCells {
  size_t count = static_cast<size_t>(-1);
  ResourceDeposit last;  // sim.deposits.back() when made (count > 0)
  int mapW = -1, mapH = -1, cw = 0, ch = 0;
  std::vector<std::vector<int>> cells;  // deposit indices, in list order
  int CellOf(double v, int n) const { return static_cast<int>(std::clamp(std::floor(v / 32.0), 0.0, static_cast<double>(n - 1))); }
};
const DepositCells* DepositIndex(const Sim& sim) {
  static const SimCache<DepositCells> slot;
  const TerrainMap* map = sim.map();
  if (!map || map->width() <= 0 || map->height() <= 0) return nullptr;
  DepositCells& c = slot.For(sim);
  auto same = [](const ResourceDeposit& a, const ResourceDeposit& b) {
    return a.x0 == b.x0 && a.z0 == b.z0 && a.x1 == b.x1 && a.z1 == b.z1 && a.type == b.type;
  };
  if (c.count != sim.deposits.size() || (c.count && !same(c.last, sim.deposits.back())) || c.mapW != map->width() ||
      c.mapH != map->height()) {
    c.count = sim.deposits.size();
    if (c.count) c.last = sim.deposits.back();
    c.mapW = map->width();
    c.mapH = map->height();
    c.cw = (c.mapW + 31) / 32;
    c.ch = (c.mapH + 31) / 32;
    c.cells.assign(static_cast<size_t>(c.cw) * c.ch, {});
    for (size_t i = 0; i < sim.deposits.size(); ++i) {
      const auto& d = sim.deposits[i];
      // (from the lower to the higher edge: a deposit given with x0 > x1 is still found where the tests find it)
      const int a0 = c.CellOf(std::min(d.x0, d.x1) - 1.5, c.cw), a1 = c.CellOf(std::max(d.x0, d.x1) + 1.5, c.cw);
      const int b0 = c.CellOf(std::min(d.z0, d.z1) - 1.5, c.ch), b1 = c.CellOf(std::max(d.z0, d.z1) + 1.5, c.ch);
      for (int b = b0; b <= b1; ++b)
        for (int a = a0; a <= a1; ++a) c.cells[static_cast<size_t>(b) * c.cw + a].push_back(static_cast<int>(i));
    }
  }
  return &c;
}
// Any deposit d (by index, every one the cells the rect touches list) with test(d)?
template <class F>
bool AnyDepositNear(const Sim& sim, double x0, double z0, double x1, double z1, F&& test) {
  const DepositCells* c = (x0 <= x1 && z0 <= z1) ? DepositIndex(sim) : nullptr;
  if (!c) {
    for (const auto& d : sim.deposits)
      if (test(d)) return true;
    return false;
  }
  const int a0 = c->CellOf(x0, c->cw), a1 = c->CellOf(x1, c->cw), b0 = c->CellOf(z0, c->ch), b1 = c->CellOf(z1, c->ch);
  for (int b = b0; b <= b1; ++b)
    for (int a = a0; a <= a1; ++a)
      for (int i : c->cells[static_cast<size_t>(b) * c->cw + a])
        if (test(sim.deposits[static_cast<size_t>(i)])) return true;
  return false;
}

// CSimResources::AreaHasDeposit 0x546860: a deposit of the type, grown by 0.5 (mass) / 1.5 (hydro) on every
// side, strictly overlaps the rect.
bool DepositOverlaps(const Sim& sim, int type, float x0, float z0, float x1, float z1) {
  float g = type == 1 ? 0.5f : 1.5f;
  return AnyDepositNear(sim, x0, z0, x1, z1, [&](const ResourceDeposit& d) {
    return d.type == type && x0 < static_cast<float>(d.x1) + g && static_cast<float>(d.x0) - g < x1 &&
           z0 < static_cast<float>(d.z1) + g && static_cast<float>(d.z0) - g < z1;
  });
}

// CSimResources::DepositIsInArea 0x546650: the rect contains a deposit of the type, or one contains the rect.
bool DepositInArea(const Sim& sim, int type, int x0, int z0, int x1, int z1) {
  return AnyDepositNear(sim, x0, z0, x1, z1, [&](const ResourceDeposit& d) {
    return d.type == type && ((x0 <= d.x0 && d.x1 <= x1 && z0 <= d.z0 && d.z1 <= z1) ||
                              (d.x0 <= x0 && x1 <= d.x1 && d.z0 <= z0 && z1 <= d.z1));
  });
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
  // The samples' lowest and highest point taken on the stored heights (HeightAt = height * scale; with the scale > 0
  // the product keeps their order, rounding included, so min / max of the products are the products of min / max):
  // the same lo / hi, one multiplication each instead of one per vertex (speed only, bind 2026-10-28). The vertices
  // a sample loop clamps to are the clamped range (a range past the edge gives its edge vertex, as each take does).
  const uint16_t* hd = map->heightData();
  const size_t stride = static_cast<size_t>(map->width()) + 1;
  const bool raw = map->heightScale() > 0.0f;
  uint16_t rlo = 0xffff, rhi = 0;
  bool any = false;
  auto rawRow = [&](int vz, int xa, int xb) {  // take(vx, vz) for vx = xa .. xb
    if (xa > xb) return;
    vz = std::clamp(vz, 0, map->height());
    xa = std::clamp(xa, 0, map->width());
    xb = std::clamp(xb, 0, map->width());
    const uint16_t* r = hd + static_cast<size_t>(vz) * stride;
    for (int vx = xa; vx <= xb; ++vx) {
      rlo = std::min(rlo, r[vx]);
      rhi = std::max(rhi, r[vx]);
    }
    any = true;
  };
  auto rawDone = [&] {
    if (!any) return;
    lo = static_cast<float>(rlo) * map->heightScale();
    hi = static_cast<float>(rhi) * map->heightScale();
  };
  bool flat;
  if (!d.flattenSkirt) {  // AreaFlatness 0x5651f0
    if (raw) {
      for (int vz = z0; vz <= z1; ++vz) rawRow(vz, x0, x1);
      rawDone();
    } else {
      for (int vz = z0; vz <= z1; ++vz)
        for (int vx = x0; vx <= x1; ++vx) take(vx, vz);
    }
    flat = hi - lo <= d.maxGroundVariation;
  } else {  // EdgeFlatness 0x564f80: the ring just outside, measured from the ceiling of its lowest point
    if (raw) {
      rawRow(z0 - 1, x0 - 1, x1 + 1);
      rawRow(z1 + 1, x0 - 1, x1 + 1);
      for (int vz = z0; vz <= z1; ++vz) {
        rawRow(vz, x0 - 1, x0 - 1);
        rawRow(vz, x1 + 1, x1 + 1);
      }
      rawDone();
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
  // COORDS_ToWorldPos 0x50b0f0 (the mobile build task's ctor snaps its site with it, 0x5f6749, with the blueprint's
  // Footprint.OccupancyCaps bp+0xda): the height of the ground (0x44fb90), or of the water above it, but not when
  // the footprint may occupy the Seabed (caps bit 2, 0x50b151): then the sea bottom. Seen: MP2 1321, army 1's UEF
  // mass extractor under water at (245.5, 20.5): the original's stands on the bottom (y 50.7656, unturned); ours
  // was made at the surface (56.0), and FAF's StructureUnit OnCreate then turned it to the "slope" below it.
  if (sim.map()) r.y = (fp.caps & 2) ? sim.map()->TerrainHeight(r.x, r.z) : sim.map()->SurfaceHeight(r.x, r.z);
  return r;
}

namespace {
// 0x720770: does the skirt rect R meet (strictly) the skirt rect of a unit that is not mobile? The candidates are
// the units the collision grid holds in R's cells (0x4fcb40: the rect truncated, 4 x 4 cells, at least one cell
// each way); a dead one that is still there counts.
bool AnyStructureSkirtOverlaps(Sim& sim, lua_State* L, const FRect& R) {
  auto lo = [](float v) { return std::clamp(static_cast<int>(v) >> 2, 0, 0xffff); };
  auto len = [](float v, int c0) { return std::max(1, std::min(((static_cast<int>(v) + 3) >> 2) - c0, 0xffff - c0)); };
  const int cx0 = lo(R.x0), cz0 = lo(R.z0);
  const int cw = len(R.x1, cx0), ch = len(R.z1, cz0);
  const bool grid = sim.entityGrid().ready();
  bool hit = false;
  sim.ForUnitsInRect(R.x0 - 32, R.z0 - 32, R.x1 + 32, R.z1 + 32, [&](Unit* e) {
    if (hit || !e->blueprint) return;
    if (e->motion.bp && e->motion.bp->mobile()) return;
    if (grid && e->gridSeen && !(e->gridX0 < cx0 + cw && cx0 < e->gridX0 + e->gridW && e->gridZ0 < cz0 + ch && cz0 < e->gridZ0 + e->gridH))
      return;
    // (e->bpData: GetUnitBpData's entry of e's blueprint, kept at its init, UnitEconomyInit; the look-up without it)
    const FRect r = SkirtRect(*e->blueprint, e->bpData ? *e->bpData : GetUnitBpData(L, *e->blueprint), e->position.x, e->position.z);
    if (r.x1 > R.x0 && R.x1 > r.x0 && r.z1 > R.z0 && R.z1 > r.z0 && R.x1 > R.x0 && R.z0 < R.z1 && r.x1 > r.x0 && r.z0 < r.z1)
      hit = true;
  });
  return hit;
}
}  // namespace

// FootprintFitsStructure 0x720d90 (structure_placement.md 4): the ground (OCCUPY_Check), then no standing
// structure's SKIRT may meet the new one's skirt, then the footprint against the occupancy bits. The mobile build
// task asks this before it creates the unit (0x5f7aec), and CanBuildStructureAt begins with it.
bool LocationIsFree(Sim& sim, const BlueprintInfo& bp, float x, float z) {
  return LocationIsFree(sim, bp, GetUnitBpData(sim.L(), bp), x, z);
}
// (d: GetUnitBpData(L, bp), read by the caller)
bool LocationIsFree(Sim& sim, const BlueprintInfo& bp, const UnitBpData& d, float x, float z) {
  lua_State* L = sim.L();
  if (!d.structure) {
    // A mobile blueprint (RUnitBlueprint::IsMobile 0x51e460: MotionType bp+0x290 != None): OCCUPY_Check 0x5652e0
    // takes its mobile path (0x5652f9 -> 0x5652ff..0x565372), OCCUPY_MobileCheck 0x564ab0 at the footprint's origin
    // cell (fistp(z - SizeZ/2), fistp(x - SizeX/2), kept as int16), and FootprintFitsStructure 0x720d90 then only
    // clears Land+Seabed / Water where a structure holds the land / water grid over the footprint (0x720e58..
    // 0x720eba; GetRectOr 0x8d8460). No BuildOnLayerCaps, flatness, deposit or skirt test on this path. Our
    // occupancy grid does not tell land structures from water ones: a structure there clears both (as SquadFitsAt).
    // Seen (Skirmish): M28AI asks brain:CanBuildStructureAt('mis0401', ...) (a Mitsurugi naval experimental it takes
    // as its size-11 "building") on land; the original answers false, ours took the structure path and said true,
    // so M28's buildable-location lists differed from 2.2 s on.
    const NamedFootprint& fp = Footprint(bp);
    const int oz = static_cast<int16_t>(RoundEven(z - fp.sizeZ * 0.5f));
    const int ox = static_cast<int16_t>(RoundEven(x - fp.sizeX * 0.5f));
    unsigned caps = sim.navigation().MobileCaps(fp, ox, oz);
    caps = sim.navigation().FootprintFits(static_cast<uint8_t>(caps), fp, ox, oz);
    return caps != 0;
  }
  if (!StructureLayers(sim, bp, d, x, z)) return false;
  const NamedFootprint& fp = Footprint(bp);
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  if (AnyStructureSkirtOverlaps(sim, L, SkirtRect(bp, d, x, z))) return false;
  return !sim.navigation().AnyStructureIn(ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}

// CAiBrain::CanBuildStructureAt 0x57cbb0 after FootprintFitsStructure passed (0x57cda3..0x57cf9f): the candidates
// are the units GatherUnmarkedUnitsInBox 0x721c00 finds in the new skirt's box (from the position as given; y
// -1000..1000): those whose collision AABB (entity+0x240..+0x254) touches it, in grid cells it shares. Of them, the
// ones alive (vt+0x28, +0x2c) and not mobile (vt+0x30) whose skirt (0x6a7600) strictly overlaps the new skirt
// (0x416c90) forbid the spot. So a structure whose skirt reaches the new skirt but whose collision box does not is
// not found. For a structure FootprintFitsStructure tested the skirts already (0x720770); for a mobile blueprint
// this is the only skirt test. Seen (Skirmish, 5.3 s): xrl0403 next to a factory just started at (351.5, 681.5)
// stays buildable at (343.99, 685.99) in the original (their skirts overlap, the factory's box is 0.5 away).
bool LiveStructureSkirtOverlaps(Sim& sim, const BlueprintInfo& bp, float x, float z,
                                const std::function<bool(const Unit*)>& skip) {
  lua_State* L = sim.L();
  const FRect R = SkirtRect(bp, GetUnitBpData(L, bp), x, z);
  const Vec3 bmn{R.x0, -1000.0f, R.z0}, bmx{R.x1, 1000.0f, R.z1};
  bool hit = false;
  sim.ForUnitsInRect(R.x0 - 32, R.z0 - 32, R.x1 + 32, R.z1 + 32, [&](Unit* e) {
    if (hit || !e->blueprint) return;
    WorldShape s;
    if (!GetWorldShape(e, &s)) return;
    Vec3 mn, mx;
    ShapeBounds(s, &mn, &mx);
    if (bmn.x > mx.x || mn.x > bmx.x || bmn.y > mx.y || mn.y > bmx.y || bmn.z > mx.z || mn.z > bmx.z) return;
    if (!GridCellsMeet(bmn, bmx, mn, mx)) return;
    if (!Alive(e)) return;
    if (skip && skip(e)) return;  // (relation != 3: the units of the given relation, 0x57cef3..0x57cf1d)
    if (e->motion.bp && e->motion.bp->mobile()) return;
    const FRect r = SkirtRect(*e->blueprint, GetUnitBpData(L, *e->blueprint), e->position.x, e->position.z);
    if (r.x1 > R.x0 && R.x1 > r.x0 && r.z1 > R.z0 && R.z1 > r.z0 && R.x1 > R.x0 && R.z0 < R.z1 && r.x1 > r.x0 && r.z0 < r.z1)
      hit = true;
  });
  return hit;
}

// CAiBrain::CanBuildStructureAt 0x57cbb0 with relation 3: FootprintFitsStructure, then the skirt against the skirts
// of the live structures once more (for a structure the same rule, so nothing new for ours). NOT MODELLED: the
// brain's own pending structure plans (brain+0x98), which the original also keeps the new skirt off.
bool CanBuildStructureAt(Sim& sim, const BlueprintInfo& bp, float x, float z) {
  const UnitBpData& d = GetUnitBpData(sim.L(), bp);  // (once: LocationIsFree's read, the first)
  if (!LocationIsFree(sim, bp, d, x, z)) return false;
  return d.structure || !LiveStructureSkirtOverlaps(sim, bp, x, z);
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
// Unit::ExecuteOccupyGround 0x6a7770: the footprint's cells at the unit's place go on in the occupancy grid. It leaves
// the "occupies ground" byte +0x560 alone (no write to it in 0x6a7770): Unit::Unit sets that after its call (0x6a5f71).
void ExecuteOccupyGround(Sim& sim, Unit* u) {
  if (!u->bpData || !u->bpData->structure || !u->blueprint) return;
  const NamedFootprint& fp = Footprint(*u->blueprint);
  // A blueprint with Physics.OccupyRects (bp+0x310, read by 0x5267a0 at 0x6a77b7) holds those rects instead of its
  // footprint (0x6a77e2: a non-empty list; 0x6a77ea: count = floats / 4, none whole: nothing is held): each entry is
  // (centre x offset a, centre z offset b, half size x c, half size z d) and covers [floor(a + x - c + 0.5),
  // floor(x + c + a + 0.5)) x [floor(z + b - d + 0.5), floor(z + d + b + 0.5)) (0x6a7819..0x6a7924, x and z the
  // position, frndint and one down when above: floor), set in the land grid for footprint caps & 7 and in the water
  // grid for & 8 (FillRect 0x8d8370 at 0x6a7963 / 0x6a798e), then the path maps told of the rect (0x8e3620). The
  // quantum gateways have two pillars and the passage between them free. Seen: MP1 14747, army 0's gateway 0x119
  // (uab0304, 832.5 235.5) rolls its SACU 0x12f off to (832.5, 238.5): the original's goal is the cell (832, 238) in
  // the passage (it drives straight out, the Move ends at z 238.03); ours had the whole 7 x 7 held and took the free
  // spot (831, 239), turning 45 degrees left.
  if (!u->bpData->occupyRects.empty()) {
    const std::vector<float>& R = u->bpData->occupyRects;
    const size_t n = R.size() / 4;
    for (size_t k = 0; k < n; ++k) {
      const float a = R[4 * k], b = R[4 * k + 1], c = R[4 * k + 2], d = R[4 * k + 3];
      const Vec3& p = u->position;
      const int x0 = static_cast<int>(std::floor(a + p.x - c + 0.5f));
      const int x1 = static_cast<int>(std::floor(p.x + c + a + 0.5f));
      const int z0 = static_cast<int>(std::floor(p.z + b - d + 0.5f));
      const int z1 = static_cast<int>(std::floor(p.z + d + b + 0.5f));
      if (sim.navigation().WouldChangeCells(x0, z0, x1, z1, fp.caps)) LandNavBeforeOccupancy(sim, x0, z0, x1, z1);
      sim.navigation().AddStructure(EntityRef(u), x0, z0, x1, z1, fp.caps, k > 0);
      LandNavDirty(sim, x0, z0, x1, z1);
    }
    return;
  }
  int ox = RoundEven(u->position.x - fp.sizeX * 0.5f), oz = RoundEven(u->position.z - fp.sizeZ * 0.5f);
  // The stale clusters' data is made before the grids change (LandNavBeforeOccupancy: from the grid as it still is).
  // When no cell's layer bit changes, the grid stays as it is and that data comes out the same when it is made later,
  // so it is left to then (speed only; the dirtying below, the original's accounting, is kept). Seen: a whole army
  // handed over under ShareUntilDeath (Chris's game 2, 19665): its 189 structures are made again on the cells the
  // old ones still hold, and making the stale data was 35 of the beat's 500 ms.
  if (sim.navigation().WouldChangeCells(ox, oz, ox + fp.sizeX, oz + fp.sizeZ, fp.caps))
    LandNavBeforeOccupancy(sim, ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
  sim.navigation().AddStructure(EntityRef(u), ox, oz, ox + fp.sizeX, oz + fp.sizeZ, fp.caps);
  LandNavDirty(sim, ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}
void OccupyStructure(Sim& sim, Unit* u) {
  if (!u->bpData || !u->bpData->structure || !u->blueprint) return;
  if (BpInCategory(sim, u->blueprint, "FERRYBEACON")) return;  // Unit::Unit skips ExecuteOccupyGround
  ExecuteOccupyGround(sim, u);
  u->occupiesGround = true;  // (0x6a5f71)
}
void ReleaseStructure(Sim& sim, Unit* u) {
  // Unit::ReleaseOccupyGround 0x6a7ab0 does nothing for a unit that no longer counts as occupying ground (+0x560 == 0,
  // 0x6a7ab3): an upgraded structure (0x5f8581) leaves its cells to the new one, whose bits it would otherwise clear.
  // Seen: Skirmish 1863, army 6's air factory 0x600016 upgraded into 0x60001f on the same cells.
  if (!u->occupiesGround) {
    sim.navigation().ForgetStructure(EntityRef(u));
    return;
  }
  u->occupiesGround = false;  // (ReleaseOccupyGround 0x6a7b81)
  if (!u->bpData || !u->bpData->structure) return;
  // ReleaseOccupyGround 0x6a7ab0 clears the blueprint FOOTPRINT's rect at the unit's position (0x6a7acb..0x6a7b7b ->
  // ReleaseOccupy 0x721b30), OccupyRects or not: a gateway frees its passage cells too.
  if (!u->bpData->occupyRects.empty() && u->blueprint) {
    const NamedFootprint& fp = Footprint(*u->blueprint);
    const int ox = RoundEven(u->position.x - fp.sizeX * 0.5f), oz = RoundEven(u->position.z - fp.sizeZ * 0.5f);
    const int r[4] = {ox, oz, ox + fp.sizeX, oz + fp.sizeZ};
    LandNavBeforeOccupancy(sim, r[0], r[1], r[2], r[3]);
    LandNavDirty(sim, r[0], r[1], r[2], r[3]);
    sim.navigation().RemoveStructure(EntityRef(u), r);
    return;
  }
  for (const auto& r : sim.navigation().Structures())
    if (r.entity == EntityRef(u)) {
      LandNavBeforeOccupancy(sim, r.x0, r.z0, r.x1, r.z1);
      LandNavDirty(sim, r.x0, r.z0, r.x1, r.z1);
      break;
    }
  sim.navigation().RemoveStructure(EntityRef(u));
}

// COGrid::ExecuteOccupy 0x721a90 / ReleaseOccupy 0x721b30 for a prop: the rect is the blueprint footprint at
// (fistp(x - SizeX/2), fistp(z - SizeZ/2)) (0x6f9f4f..0x6f9fda; the destructor the same at the prop's position then,
// 0x6fa067..0x6fa105). The layer bits go on only for OccupancyCaps & 7 (land) / & 8 (water); the clusters of every
// footprint's path map around the rect are dirtied in any case (0x721ae0 / 0x721b80 -> 0x76bbd0).
void PropOccupancy(Sim& sim, Prop* p, const Vec3& pos, bool occupy) {
  const int sx = p->occSizeX, sz = p->occSizeZ;
  const int x0 = static_cast<int16_t>(RoundEven(pos.x - sx * 0.5f));
  const int z0 = static_cast<int16_t>(RoundEven(pos.z - sz * 0.5f));
  static const bool dbg = getenv("MOHO64_DEBUG_NAV") != nullptr;
  if (dbg)
    Logf(LogLevel::Info, "prop %s %08x %s caps %d rect (%d,%d)-(%d,%d)", occupy ? "occupy" : "release", p->id,
         p->blueprint ? p->blueprint->id.c_str() : "?", p->occCaps, x0, z0, x0 + sx, z0 + sz);
  // The layer bits: a prop with OccupancyCaps & 7 (land) or & 8 (water) blocks its rect as a structure does.
  // Seton's Clutch has 14 such props (fieldstone rocks, OccupancyCaps 3). Seen: Skirmish 1263, engineer 0x300015
  // walking to reclaim a rock at (152.2, 558.3): the original's path does not cross the rock at (153.9, 567.6)
  // (cells 152..154 x 565..569), its first target stops at (156, 568); ours went through it to (154, 566).
  if (p->occCaps & 15) {
    const int r[4] = {x0, z0, x0 + sx, z0 + sz};
    LandNavBeforeOccupancy(sim, r[0], r[1], r[2], r[3]);
    if (occupy) sim.navigation().AddStructure(EntityRef(p), r[0], r[1], r[2], r[3], p->occCaps);
    else sim.navigation().RemoveStructure(EntityRef(p), r);
    // (Seen, MP3 2988 (probe mp3_boulder): two des_boulder01 rocks share cells, 0x2ff0196b on (263,250)-(266,254) and
    // 0x2ff00bf2 on (264,250)-(267,254). The first is reclaimed at 2642; from then on the original's land grid holds
    // only x 266 of the second one's rect (words x 264..265 are 0), and army 3's guard 0x30002d takes its slot cells
    // (264,252), (264,251), (265,250) as goals; ours still held them and sent it beside the rock.)
  }
  LandNavDirty(sim, x0, z0, x0 + sx, z0 + sz);
}

// ---- build helper (CBuildTaskHelper) -----------------------------------------------------------

// A number field of the object's OWN Lua table (what a script writes as self.Key = v), as the engine reads and writes
// it. CScriptObject::GetLuaValue 0x5fca70: the object's LuaObject (+0x20), LuaPlus GetByName 0x90a160 (0x908f60: a
// raw lookup of the string key in the table's nodes, no __index), lua_tonumber (0 for nil or a non-number); an
// exception (not a table) gives 0. CScriptObject::SetLuaValue 0x5fcb70: LuaPlus SetNumber 0x908320 ->
// SetTableHelper 0x9074b0 -> luaV_settable (0x929450). (Not PushObjectValue / SetObjectValue: those keep the engine's
// own Set<X> values in a side table, which no script reads as a field.)
float ScriptFieldNumber(lua_State* L, ScriptObject* o, const char* key) {
  const int top = lua_gettop(L);
  PushObject(L, o);
  float v = 0.0f;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, key);
    lua_rawget(L, -2);
    v = static_cast<float>(lua_tonumber(L, -1));
  }
  lua_settop(L, top);
  return v;
}
void SetScriptFieldNumber(lua_State* L, ScriptObject* o, const char* key, float v) {
  const int top = lua_gettop(L);
  PushObject(L, o);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_settable(L, -3);
  }
  lua_settop(L, top);
}

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
  // the helper's target is a weak pointer (0x5f5bf0 reads helper+8 with no IsDead test): a killed unit is
  // still worked on until it is freed. The build tasks test their target before they get here; the repair task
  // does not (0x5f939d), so it keeps "repairing" a dead unit (whose health no longer rises, Entity::AdjustHealth)
  Unit* target = FindUnitAny(sim, t.targetId);
  if (!target) return true;
  if (builder->paused) {  // (FAF patch 0x1291a27) a paused builder only shows the target's progress
    t.lastFraction = target->fractionComplete;  // helper+0x3c
    builder->workProgress = target->fractionComplete;
    // back in the original at 0x5f5c26: done when the builder is not Repairing (state 16) and the target is whole;
    // else the target's Materialize(0.0) (vt+0x74, 0x5f5c72..0x5f5c80), which marks it worked on this beat (the
    // tick at Entity+0x520, 0x6a9fa3), so an unfinished target of a paused builder does not decay (Unit::MotionTick
    // 0x6a910f: decay when more than a beat has passed). Seen: Skirmish 2071, M28 pauses three upgrades of armies 0
    // and 2 (0x200031, 0x35 and 0x34's builders): the original's health stays where it was; ours decayed from 2073.
    if (!builder->unitStates.Has(US("Repairing")) && target->fractionComplete >= 1.0f) return true;
    Materialize(sim, L, target, 0.0f);
    return false;
  }
  float rate = builder->resourceConsumed;
  // helping a silo build its missile (helper+0x40, 0x5f5cb6): what the builder's own request was granted this
  // beat goes to the silo, which turns it into progress; done when the silo no longer builds
  if (t.silo) {
    if (!(LiveCommandCaps(L, target) & 0xc000u)) return true;  // RULEUCC_SiloBuildTactical | SiloBuildNuke
    if (!target->unitStates.Has(US("SiloBuildingAmmo"))) return true;
    if (target->silo) {
      float amount[2] = {0, 0};
      if (builder->request) {
        amount[0] = builder->request->requested[0] * rate;
        amount[1] = builder->request->requested[1] * rate;
      }
      SiloAssist(sim, target, amount);
      builder->workProgress = target->workProgress;
    }
    return false;
  }
  // assisting an enhancement (the target works on a script work item: unit state 0x26, 0x5f5dc6..0x5f5ee4): the
  // target's script WorkProgress (0x5fca70) at 1 or more is done (0x5f6320); else the helper ADDS its own share,
  // 1 / (the target's WorkItemBuildTime / the builder's BuildRate, attributes+0x58) x what the builder's request was
  // granted (+0x53c) x 0.1, capped at 1, to that script value (0x5fcb70; only when the grant is not 0), and shows
  // it as its own work progress (+0x2ac). FAF's EnhanceTask adds the enhancing unit's own share to the same
  // WorkProgress each beat, so an assisted enhancement ends sooner. Ours added nothing. Seen: MP2 9550, army 1's
  // commander 0x100000 enhances from 8891, its engineer 0x1000c9 assists from 9264: the original's ends at 9550,
  // ours ran on to 9626.
  if (target->unitStates.Has(US("Enhancing"))) {
    float p = ScriptFieldNumber(L, target, "WorkProgress");
    if (p >= 1.0f) return true;
    if (rate != 0.0f) {
      const float bt = ScriptFieldNumber(L, target, "WorkItemBuildTime");
      const float q = 1.0f / (bt / builder->buildRate) * rate * 0.1f + p;
      p = q > 1.0f ? 1.0f : q;
      SetScriptFieldNumber(L, target, "WorkProgress", p);
    }
    builder->workProgress = p;
    return false;
  }
  const UnitBpData& td = *target->bpData;
  float delta = 0;
  if (rate != 0.0f && td.buildTime > 0.0f && builder->buildRate > 0.0f)
    delta = 1.0f / (td.buildTime / builder->buildRate) * rate * 0.1f;
  const bool wasAlive = Alive(target);
  Materialize(sim, L, target, delta);
  if (wasAlive && !Alive(target)) return true;
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
  u->unitStates.Insert(US("Moving"));
}

// A task's walk (NewMoveTask 0x6190a0 -> CUnitMoveTask ctor 0x6180e0). The ctor asks 0x618a00 (read): not
// TransportLoading (8), Teleporting (19) or Guarding (4), and the unit's command queue (unit+0x4b4) holds exactly one
// command (0x6edc20: no second), made by a factory (cmd+0x142). Then it does not walk to the goal it was given: it
// takes the HEAD COMMAND's target position (0x5e2a90 on cmd+0x11c), x and z rounded (fistp), puts it through
// PrepareMove (0x618521: spacing 0, no exclusion rect), hands the navigator the footprint cell there (0x618d44-like
// SNavGoal, vt+8) and reserves the footprint rect there in the o-grid (0x618622 -> ReserveOgridRect 0x6ab6f0, +0x92).
// So a factory-made engineer still on its rally patrol walks to a free spot by the PATROL POINT, not to the prop it
// was sent to reclaim (the reclaim then works from where it stops: patrolling, its range is GuardScanRadius). Seen
// in the oracle: MP3 556, army 2's engineer 0x200002 goes to (70.5, 446.5), the free spot two cells from its patrol
// point (69, 448) beside the factory's reserved skirt, instead of (69.5, 452.5) by the rock; the MP2 probe at 720
// shows the ctor's PrepareMove called with the patrol point while the reclaim's goal was another cell.
void MoveTaskWalk(Sim& sim, Unit* u, const Vec3& goal) {
  const bool override = !u->unitStates.Has(US("TransportLoading")) && !u->unitStates.Has(US("Teleporting")) &&
                        !u->unitStates.Has(US("Guarding")) && u->commands.size() == 1 && u->commands.front() &&
                        u->commands.front()->factoryIssued && u->motion.bp;
  if (!override) {
    TaskMoveToward(sim, u, goal);
    AirSpeedThroughEvent(sim, u);  // the ctor's end (0x6186a1): Unit::UpdateSpeedThroughStatus 0x6ac940
    return;
  }
  // NewMoveTask 0x6190a0 hands the navigator the goal it was given first (vt+8 at 0x6190c9, before the ctor), and the
  // ctor's override then sets its own (0x6185f3). The first SetGoal's SetCurrentPosition 0x5a415d renews the cell,
  // and the second's ResetPathState takes that cell for the target. (Without the override both goals are the same
  // and the second returns at once, 0x5a3edc.) Seen (probe cp_wnpatrol, MP1): at 3156 army 7's patrolling engineer
  // 0x700027 gets SetGoal (362, 636) from 0x6190cb, then (356, 602) from 0x6185f5; the first moved its cell from
  // (343, 655) to (343, 654), so its target at the new goal is (343, 654); ours, with only the second, kept (343, 655)
  // (watch line wn 3156; the same at 3630 for army 0's 0x3c / 0x44 / 0x46 / 0x49).
  if (!u->motion.air && u->motion.bp->motionType != kMotionAir) {
    const bool through = u->motion.nav && u->motion.nav->speedThroughGoal;  // (SetGoal keeps the request)
    LandNavSetGoal(sim, u, goal, through);
  }
  const UnitCommand& c = *u->commands.front();
  const Entity* te = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
  const Vec3 tp = te ? te->position : c.pos;
  Vec3 p{static_cast<float>(RoundEven(tp.x)), tp.y, static_cast<float>(RoundEven(tp.z))};
  if (vm::BitsZero(p)) {  // (0x6184ce, memcmp with the zero vector: a zero position keeps the goal it was given)
    TaskMoveToward(sim, u, goal);
    AirSpeedThroughEvent(sim, u);
    return;
  }
  const float none[4] = {0, 0, 0, 0};
  if (u->motion.bp->motionType == kMotionAir) AirPrepareMove(sim, u, &p);
  else GroundPrepareMove(sim, u, &p, none, 0);
  const NamedFootprint& fp = Footprint(*u->blueprint);
  const int x0 = RoundEven(p.x - fp.sizeX * 0.5f), z0 = RoundEven(p.z - fp.sizeZ * 0.5f);
  TaskMoveToward(sim, u, Vec3{x0 + fp.sizeX * 0.5f, p.y, z0 + fp.sizeZ * 0.5f});
  u->walkPrepared = true;  // (+0x95)
  const int r[4] = {x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ};
  GroundReserveRect(sim, u, r);
  // and at its end (0x6186a1) Unit::UpdateSpeedThroughStatus 0x6ac940: a patrol with points queued behind it walks
  // with speed-through on (head and next move-like). Seen (MP3 1318): army 0's engineer 2 on a three-point patrol.
  AirSpeedThroughEvent(sim, u);
}

void StopMoving(Unit* u) {
  if (u->motion.hasGoal) MotionStop(u);
  u->unitStates.Erase(US("Moving"));
}

// The o-grid rect a rally-overridden walk reserved (MoveTaskWalk, +0x92) goes with the walk's CUnitMoveTask: its dtor
// frees it whatever the abort table says and whether the unit lives or not (0x618886..0x618891: task+0x92 set ->
// FreeOgridRect 0x6ab760). Seen: MP1 12165, army 2's engineer 0x2000bc (url0105) is killed on its patrol's reclaim
// walk, whose override reserved (212,356) at 12161; the original's kill (queue cleared, tasks destroyed) gives the cell
// back, so at 12176 the ship 0x2000e5's override PrepareMove from (212,356) takes that very cell and its path runs
// along z 356 (target (213,356), drive-through 1 at 12178). Ours kept the dead engineer's rect (a mobile unit's
// OnDestroy frees nothing: unit+0x560 is a structure's, 0x6a5f71), took (214,354) beside it, drove to that goal cell
// itself with speed-through 0 and turned twice as far.
void FreeWalkRect(Unit* u) {
  if (!u->walkPrepared) return;
  GroundFreeRect(*Sim::From(u->luaState()), u);
  u->walkPrepared = false;
}

// A task's move child torn down with the task (CUnitMoveTask's dtor 0x618700): Moving off (0x6187bd), then the
// navigator's AbortMove (0x618884) unless the queue is being cleared for an order its table 0x618998 keeps
// (u->clearKeepsMove, set by IssueFromStream). (A move that ends by itself goes through StopMoving: queue+0x1c is 0
// then, and the table aborts for 0.) The dtor asks nothing of the goal (0x618855..0x618884: a navigator and the
// table's byte +0x90): a navigator that has already arrived is aborted too, and its MakeIdle -> SetWaypoints(0, 0)
// makes a brake spline from whatever spline the steering holds by then. Seen: MP1 4803, army 5's engineer 0x50002c,
// guarding, reclaims for its guarded unit; its walk arrives in the navigator stage (Stop: no spline), its steering
// then takes a side-step from a neighbour's record (0x50002a), and in the command stage its reclaim ends with the walk
// still on it (TickReclaim's target test, before the walk's own end): the original's move dtor brakes from that
// side-step spline along the old heading (0.152 -> 0.114 -> 0.076 -> 0.038); ours, testing the goal, did not abort,
// and took the side-step (0.190, turning).
void EndMoveChild(Unit* u) {
  u->unitStates.Erase(US("Moving"));
  if (!u->clearKeepsMove && u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
  FreeWalkRect(u);
}

// The end of a task's walk that ended by itself (the navigator's event): the walk is a CUnitMoveTask child
// (NewMoveTask 0x6190a0, from CUnitMobileBuildTask 0x5f7763, CUnitRepairTask 0x5f9829, CUnitSacrificeTask 0x5fb809,
// CUnitCaptureTask 0x60477d, CUnitGuardTask 0x614549, CUnitReclaimTask 0x61f4c0), and its dtor 0x618700 clears Moving
// (0x6187bd), then calls the navigator's AbortMove (vt+0x10, 0x618884) whenever its table 0x618998 says so for
// queue+0x1c, which is 0 here (0x61884e: abort). There is no test of the goal: AbortMove runs on a navigator that is
// already idle (land 0x5a3750: SetSpeedThroughGoal(0) -> CAiSteeringImpl 0x5d2390, then MakeIdle -> SetWaypoints(0,
// 0), which makes a brake spline only while the steering still has one, 0x5d2ae3). After an arrival on a waypoint in
// the goal the motion's speed-through byte so goes to 0 in the beat the walk ends. Seen: MP3 2867, army 0's
// patrolling engineer 0xf, whose repair walk ends at 2867 (and the repair with it): the original's byte is 0 from
// that beat, ours kept 1 until its next walk ended at 2875.
void EndWalk(Unit* u) {
  u->unitStates.Erase(US("Moving"));
  MotionStop(u);
}

// A unit of the same kind being built at the site (join it instead of starting another): 0x5f6c70, the units
// around the site (0x721fb0) that are not dead (vt+0x28), being built (vt+0x34), stand exactly on the site
// (0x5f6dc9: the x / z distance is not above 0) and have the same blueprint id (0x5f6e0e). Whose they are is not
// asked. (Seen: Skirmish 3023, army 1's engineer 0x100023 sent to build an xsb2104 where army 5's engineer has
// one under way: the original's goes Building at once; ours, looking for its own army's only, found the site
// taken and dropped the order.)
Unit* ExistingAtSite(Sim& sim, const Unit* builder, const BlueprintInfo& bp, const Vec3& site) {
  (void)builder;
  Unit* found = nullptr;
  sim.ForUnitsInRect(site.x - 0.5f, site.z - 0.5f, site.x + 0.5f, site.z + 0.5f, [&](Unit* u) {
    if (found || !Alive(u) || u->blueprint != &bp || !u->beingBuilt) return;
    const float dx = u->position.x - site.x, dz = u->position.z - site.z;
    if (std::sqrt(dz * dz + dx * dx) > 0.0f) return;
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

// CUnitReclaimTask ctor 0x61eb60 (0x61ecde..0x61ecf0; reached only through IssueReclaimTask 0x61ef60, which makes
// the task only for a live entity): the builder's navigator (unit+0x54c) gets vt+0x10 = AbortMove 0x5a3750,
// whatever it is doing. An idle navigator whose steering still holds a brake spline makes that spline afresh from
// the speed and facing of now (MakeIdle -> SetWaypoints(0, 0) -> CAiPathSpline::Update(3)) and hands out its first
// point at once. Seen (Skirmish 471): an engineer braking after its Move was cleared at 470 gets a mobile build
// whose site has a tree group on it; the reclaim child made in state 0 at 471 rebuilds the brake: the original moves
// 0.1107 in motion 471 (0.1512 laid along its pitched facing, 0.1487, less the brake 0.0380), not the old spline's
// second point 0.1132.
void ReclaimTaskAbortMove(Unit* u) {
  if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
}

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
  // MOHO64_DEBUG_PREPMOVE=<unit id>: the site, every prop gathered and what became of it
  static const long dbg = getenv("MOHO64_DEBUG_PREPMOVE") ? atol(getenv("MOHO64_DEBUG_PREPMOVE")) : -1;
  const bool say = dbg == static_cast<long>(u->id);
  if (say)
    Logf(LogLevel::Info, "siteprop %u unit %u bp %s site %.3f %.3f rect %d %d %d %d, %zu gathered", sim.tick(), u->id,
         t.bp->id.c_str(), t.site.x, t.site.z, x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ, cand.size());
  for (Entity* p : cand) {
    if (p->kind != Entity::Kind::Prop || !p->blueprint) continue;
    WorldShape ws;
    if (!GetWorldShape(p, &ws)) continue;
    Vec3 a, b;
    ShapeBounds(ws, &a, &b);
    if (say)
      Logf(LogLevel::Info, "siteprop %u   prop %s at %.3f %.3f shape %d bounds x %.3f..%.3f z %.3f..%.3f overlap %d obstructs %d",
           sim.tick(), p->blueprint->id.c_str(), p->position.x, p->position.z, static_cast<int>(ws.type), a.x, b.x, a.z,
           b.z, ShapeOverlapsAABox(ws, mn, mx) ? 1 : 0, BpInCategory(sim, p->blueprint, "OBSTRUCTSBUILDING") ? 1 : 0);
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
  SetArmAimTarget(sim, u, Vec3{0, 0, 0});  // (the dtor 0x620280: the builder unit+0x554, if any, SetAimTarget(0) +0x9f..+0xd6)
  u->unitStates.Erase(US("Reclaiming"));
  u->focusId = 0;
  u->workProgress = 0;
  if (c.moving) EndMoveChild(u);
}

// The script's answer to "what does reclaiming this cost": CUnitReclaimTask asks the BUILDER, builder:
// GetReclaimCosts(target) (0x61f79c for a unit, 0x61fbae for a prop or an unfinished unit), and takes exactly
// three numbers: seconds, energy, mass. (FAF's Unit.GetReclaimCosts hands a prop's question on to the prop.)
static bool ReclaimCosts(Sim& sim, lua_State* L, Unit* u, Entity* e, float* time, float* energy, float* mass) {
  (void)sim;
  int top = lua_gettop(L);
  PushObject(L, u);
  int obj = lua_gettop(L);
  lua_pushstring(L, "GetReclaimCosts");
  lua_gettable(L, obj);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return false;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, -2);
  lua_pushvalue(L, obj);
  PushObject(L, e);
  if (lua_pcall(L, 2, LUA_MULTRET, top + 2) != 0) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_settop(L, top);
    return false;
  }
  if (lua_gettop(L) - (top + 2) != 3) {
    Logf(LogLevel::Warning, "Failed to get valid reclaim costs from the target");
    lua_settop(L, top);
    return false;
  }
  *time = static_cast<float>(lua_tonumber(L, -3));
  *energy = static_cast<float>(lua_tonumber(L, -2));
  *mass = static_cast<float>(lua_tonumber(L, -1));
  lua_settop(L, top);
  return true;
}

// CUnitReclaimTask::Execute 0x61f000 with an entity for a target: a prop (the child clearing a build site, or a
// Reclaim order) or a unit (a Reclaim order). States as the original's (mobile_build.md 8.4 for a prop):
//   0 the target must be RECLAIMABLE (not asked of an unfinished unit); too far (gap > MaxBuildDistance, the gap
//     being the distance less the larger side of each one's FOOTPRINT) or nearer than 1: walk;
//   1 still too far: over. The arm is aimed (the script's OnPrepareArmToBuild opens it), OnStartReclaim;
//   2 A FINISHED UNIT, every beat: wait for the arm; ask the costs; the target loses MaxHealth / max(1, seconds x
//     10) health (and what it regenerates in a beat on top), is marked BeingReclaimed; when it has no more than
//     that left: its OnReclaimed, its CreateWreckageProp(0), Destroy, and the WRECK is the target from here on
//     (state 3): that is where a reclaimed unit's mass comes from. Nothing is paid out while the unit shrinks,
//     and the builder's resource shortage does not slow it.
//     Anything else: state 3 next beat;
//   3 wait for the arm, the costs, what is left := min(left, health / max health);
//   4 a step a beat, paid out as it goes; the end is the next beat's first test.
int TickPropReclaim(Sim& sim, lua_State* L, Unit* u, BuildTask& c) {
  // the move (state 0's NewMoveTask) sits on top of this task on the unit's thread: Execute, and so the target
  // test below, runs again only when the move has ended (mobile_build.md 8.4). Seen in the bench at 492: an
  // engineer walking to a site prop that another took away walks on to its spot in the original.
  if (c.moving && c.state == 1 && u->motion.hasGoal && !u->motion.arrived && !u->motion.failed) return kTaskRunning;
  // A walk under way when the command listener 0x620160 set state 0 and woke the thread: the walk's CUnitMoveTask
  // (NewMoveTask 0x61f4c0; it links the queue's head command itself, probe mp1_2701, and re-goals on the event) sits
  // on top of the thread, so it is what the wake runs; its TaskTick 0x618a70 returns -1 and its dtor 0x618700 clears
  // Moving (0x6187bd) and calls the navigator's AbortMove (0x618886); then this task runs state 0 in the same pass.
  // Seen: MP1 2040, the player drags army 0's factory rally point while its engineer 0xb walks to a rock for the rally
  // AggressiveMove; at 2041 the original's engineer is neither Moving nor Reclaiming (the dragged point is in sight:
  // the task ends), ours walked on and reclaimed the rock from 2041 to 2043.
  if (c.moving && c.state == 0) {
    c.moving = false;
    EndWalk(u);
    // (and the dtor frees the o-grid rect its ctor reserved for a rally-overridden walk, +0x92: 0x618891)
    if (u->walkPrepared) GroundFreeRect(sim, u);
  }
  // A task without an entity (+0x4c, ReclaimTaskCommandEvent). Every Execute first asks the army's recon
  // (0x61f02e..0x61f087: IArmy vt+0x20 GetReconDB, its vt+0x10 ReconCanDetect 0x5c1850 with LOSNow 8; under water by
  // the map's water height): a place in sight ends it (result 2; OnStopReclaim if started). Out of sight the place
  // stands for the target with a 2x2 footprint (0x61f1cc, 0x61f218..0x61f2e5); state 0 (0x61f3af..0x61f4c8) walks
  // next to it as for a prop (gap > MaxBuildDistance or nearer than 1: PrepareMove beside the 2x2 rect, spacing 1,
  // NewMoveTask); state 1 then ends it (0x61f4e6..0x61f4ef: result 2).
  if (c.reclaimAtPoint) {
    if (c.moving) {
      c.moving = false;
      EndWalk(u);
      GroundFreeRect(sim, u);
    }
    const TerrainMap* m = sim.map();
    const bool under = m && m->hasWater && m->waterElevation > c.site.y;
    const int a = u->army ? u->army->index - 1 : -1;
    if (ReconAtPoint(sim, a, c.site, under ? 2 : 0) & 8) return kTaskFailed;
    if (c.state != 0) return kTaskFailed;
    const float dx = u->position.x - c.site.x, dz = u->position.z - c.site.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const NamedFootprint& ufp = Footprint(*u->blueprint);
    const float gap = dist - static_cast<float>(std::max(ufp.sizeX, ufp.sizeZ)) - 2.0f;
    c.state = 1;
    if (!(gap > u->bpData->maxBuildDistance || dist < 1.0f) || !CanMove(u)) return kTaskFailed;
    const int px0 = RoundEven(c.site.x - 1.0f), pz0 = RoundEven(c.site.z - 1.0f);
    const float excl[4] = {static_cast<float>(px0), static_cast<float>(pz0), static_cast<float>(px0) + 2.0f,
                           static_cast<float>(pz0) + 2.0f};
    Vec3 a2 = c.site;
    PrepareMoveFor(sim, u, &a2, excl, 1);
    const int x0 = static_cast<int>(std::nearbyint(a2.x - ufp.sizeX * 0.5f));
    const int z0 = static_cast<int>(std::nearbyint(a2.z - ufp.sizeZ * 0.5f));
    MoveTaskWalk(sim, u, Vec3{x0 + ufp.sizeX * 0.5f, a2.y, z0 + ufp.sizeZ * 0.5f});  // NewMoveTask 0x61f4c0
    c.moving = true;
    return kTaskRunning;
  }
  Entity* e = c.goalId ? sim.FindEntity(c.goalId) : nullptr;
  if (!e || e->destroyQueued) return kTaskFailed;
  Unit* tu = e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
  // Every Execute, before the states (0x61f166..0x61f19d): a target that is not being built (Entity vt+0x2c,
  // IsBeingBuilt) and stands on the Air layer (entity+0x118 == 0x10) ends the task at once with result 2 (0x61f07e;
  // OnStopReclaim, if it was started, comes from the dtor). Seen: MP1 4716, army 0's patrolling engineers 0x3c / 0x42 /
  // 0x4c sent at 4710 to reclaim army 1's landed scout 0x100068, which takes off at 4713: at 4716 the original's
  // children end before they walk and the patrols pick a tree in the same beat (Moving at 4722); ours walked two
  // beats toward the flying scout first (Moving at 4716, the tree only from 4724).
  if (tu && !tu->beingBuilt && tu->layer == "Air") return kTaskFailed;
  Vec3 tp = e->position;
  float dx = u->position.x - tp.x, dz = u->position.z - tp.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  const NamedFootprint& bf = Footprint(*u->blueprint);
  float pfx, pfz;
  if (tu) {
    const NamedFootprint& tf = Footprint(*tu->blueprint);
    pfx = static_cast<float>(tf.sizeX);
    pfz = static_cast<float>(tf.sizeZ);
  } else {
    pfx = PropBpNum(sim, L, *e->blueprint, "Footprint", "SizeX", 1);
    pfz = PropBpNum(sim, L, *e->blueprint, "Footprint", "SizeZ", 1);
  }
  float gap = dist - static_cast<float>(std::max(bf.sizeX, bf.sizeZ)) - std::max(pfx, pfz);
  float maxBD = u->bpData->maxBuildDistance;
  for (;;) switch (c.state) {
    case 0:
      if (!(tu && tu->beingBuilt) && (!BpInCategory(sim, e->blueprint, "RECLAIMABLE") || e == u)) return kTaskFailed;
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
        // (no o-grid reservation: CUnitReclaimTask 0x61f000 reserves nothing, it is not among ReserveOgridRect's
        // callers. Seen in the Skirmish oracle's navigator lines: a second engineer is given the very cell the
        // first one is still walking to, 40 beats after the first took it.)
        MoveTaskWalk(sim, u, Vec3{x0 + ufp.sizeX * 0.5f, a.y, z0 + ufp.sizeZ * 0.5f});  // NewMoveTask 0x61f4c0
        c.moving = true;
      }
      c.state = 1;
      if (c.moving) return kTaskRunning;
      continue;
    case 1:
      if (c.moving) {
        if (u->motion.hasGoal && !u->motion.arrived && !u->motion.failed) return kTaskRunning;
        c.moving = false;
        EndWalk(u);
        GroundFreeRect(sim, u);
      }
      {
        // 0x61f4f4..0x61f54a: the range is MaxBuildDistance (bp+0x564), and for a builder in state Patrolling (12) the
        // larger of that and AI.GuardScanRadius (bp+0x460). State 0 moves by MaxBuildDistance alone, so a patrolling
        // engineer whose walk to its reclaim spot ends short (the spot cannot be reached) still reclaims from where it
        // stopped. (Read; seen in the MP3 oracle: army 2's engineer 0x200002 reclaims a rock 7.5 away at 576.)
        float range = maxBD;
        if (u->unitStates.Has(US("Patrolling"))) range = std::max(GuardScanRadiusOf(u), maxBD);
        if (gap > range) return kTaskFailed;
      }
      // In range: a target held through the army's recon blip becomes the blip's unit from here on
      // (0x61f574..0x61f5be: the target entity's vt+0x1c, a ReconBlip, -> ReconBlip::GetCreator 0x579670, and the
      // task's weak link +0x44 set to it by 0x5a6e00). Until then the task's target is the blip (IssueReclaimTask
      // 0x61ef60 with the order's CAiTarget), and its dtor's BeingReclaimed clear (CUnitReclaimTask dtor 0x620280:
      // the target's vt+0x10, no unit for a blip) leaves the unit alone.
      // The swap is made for ANY blip the task holds (vt+0x1c non-null), whatever the order said: an order on the blip
      // of a unit already gone (its command holds the blip itself, SetCommandEntityTarget) gets no creator (the blip's
      // weak link +0x270 is empty), and with no entity left the task ends here (0x61f5c3..0x61f5d1 -> 0x6200d4, -1,
      // no result written). Seen: MP1 15755, army 0's SACU 0x12f is given a second Reclaim of the blip 0x30400038 of
      // the wall 0x400038 it reclaimed at 15753: the original drops it at once and reclaims the next wall 0x40004c by
      // 15756; ours reclaimed the blip as a prop for three beats (0x40004c went at 15759).
      if (e->kind == Entity::Kind::Blip) {
        auto* b = static_cast<ReconBlip*>(e);
        Unit* cr = b->source && !b->source->destroyQueued ? b->source : nullptr;
        c.goalId = cr ? EntityRef(cr) : 0;
        if (!cr) return kTaskDone;
        e = cr;
        tu = cr;
      }
      c.targetViaBlip = false;
      // 0x61f5d7..0x61f619: the unit's builder (unit+0x554, CAiBuilderImpl) is told the target's position (vt+0x3c
      // SetAimTarget), whether or not the unit has builder arm controllers; a non-zero aim calls the script's
      // OnPrepareArmToBuild (SetArmAimTarget). Seen (probes mp1_7173 / mp1_7173b): MP1 7173, army 6's Aeon engineer
      // 0x600096 (ual0105: a builder without arm controllers) starts reclaiming a site prop while it still brakes;
      // ConstructionUnit.OnPrepareArmToBuild finds it IsMoving and calls navigator:AbortMove() (0x5a5d01), which brakes
      // it once more from where the beat found it (SetWaypoints(0, 0) -> Update(3)): its hull turns 6e-5 rad and its z
      // lands a float step lower. Ours skipped the call for an armless builder, braked only at 7172, and from 7183 the
      // original's engineer flickers in height by a float step, so army 6's 0x6000ad's continuation path at 7186 counts
      // it as a mover (UnitIsBlocked's filter 0x62eee9) and runs through its cells (MP1 7187).
      SetArmAimTarget(sim, u, tp);
      if (!c.reclaimStarted) {
        c.reclaimStarted = true;
        PushObject(L, e);
        sim.CallMethod(L, u, "OnStartReclaim", 1);
      }
      c.state = 2;
      continue;
    case 2: {
      if (!tu || tu->beingBuilt) {
        c.state = 3;
        return kTaskRunning;
      }
      // a finished unit (0x61f69f..0x61fa4d)
      if (!u->builderArms.empty() && dist > 1.0f && !u->armReady) return kTaskRunning;
      if (gap > maxBD) return kTaskFailed;
      float time, energy, mass;
      if (!ReclaimCosts(sim, L, u, e, &time, &energy, &mass)) return kTaskFailed;
      u->unitStates.Insert(US("Reclaiming"));
      u->focusId = EntityRef(e);
      const float ticks = std::max(1.0f, time * 10.0f);
      float amount = tu->maxHealth / ticks;
      if (tu->regenRate > 0.0f) amount = tu->regenRate * 0.1f + amount;
      if (tu->health > amount) {
        EntityAdjustHealth(L, tu, nullptr, -0.0f - amount);
        if (Alive(tu)) tu->unitStates.Insert(US("BeingReclaimed"));
        return kTaskRunning;
      }
      // 0x61f955..0x61f975: a builder in state Guarding (4) or AssistingCommander (0x1d) does not take the last step:
      // the task returns 1 (ebx, 0x61f71e) and leaves the target as it is, for a reclaimer of its own order. (Seen:
      // Economy 3896, SACU 0xa reclaims mex 0x15 and SACU 0xc, guarding 0xa, helps; 0xa's step leaves 23.5 of it,
      // 0xc's would kill it but the original's 0x15 is still there after the beat; 0xa kills it at 3897 and goes on
      // to reclaim the wreck until 3902. Ours let 0xc kill it at 3896, and 0xa went to its next order.)
      if (u->unitStates.Has(US("Guarding")) || u->unitStates.Has(US("AssistingCommander"))) return kTaskRunning;
      PushObject(L, u);
      sim.CallMethod(L, tu, "OnReclaimed", 1);
      // its wreck (0x620760: the script's CreateWreckageProp with an overkill of 0), then the unit goes
      Entity* wreck = nullptr;
      if (tu->HasLuaObject()) {
        int top = lua_gettop(L);
        PushObject(L, tu);
        int obj = lua_gettop(L);
        lua_pushstring(L, "CreateWreckageProp");
        lua_gettable(L, obj);
        if (lua_isfunction(L, -1)) {
          lua_pushcfunction(L, ScriptTraceback);
          lua_insert(L, -2);
          lua_pushvalue(L, obj);
          lua_pushnumber(L, 0);
          if (lua_pcall(L, 2, 1, top + 2) != 0) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
          else wreck = ToObject<Entity>(L, -1);
        }
        lua_settop(L, top);
      }
      if (!tu->destroyQueued) sim.QueueDestroy(tu);
      if (wreck && !wreck->destroyQueued) {
        c.goalId = EntityRef(wreck);
        u->focusId = EntityRef(wreck);
      }
      c.state = 3;
      return kTaskRunning;
    }
    case 3: {
      if (!u->builderArms.empty() && dist > 1.0f && !u->armReady) return kTaskRunning;
      float time, energy, mass;
      if (!ReclaimCosts(sim, L, u, e, &time, &energy, &mass)) return kTaskFailed;
      float rate = std::max(1.0f, time * 10.0f);
      c.reclaimStep = 1.0f / rate;
      if (energy < 0 || mass < 0) return kTaskFailed;
      // 0x61fc44..0x61fc9e: the step is 1 / rate (stored negated, x -1.0f), and each per-tick amount is the cost
      // TIMES that step (mulss xmm3), not the cost / rate. Seen: MP3 526, army 3's engineer 0x200002 reclaims a rock
      // worth 37.5 mass over 7.5 ticks: the original books 5.0000005 a tick (37.5 x 0.13333334), ours booked 5.0; the
      // army's mass income 5.29999971 vs 5.29999924 from 527, its stored mass and its factories' build rates a few
      // ulps apart from there (MP3 3281: a Seraphim tank's build fraction 3.8e-7 behind, its factory slider and the
      // gun's muzzle in the watch lines one float step off now and then).
      c.reclaimPerTick[kEnergy] = energy * c.reclaimStep;
      c.reclaimPerTick[kMass] = mass * c.reclaimStep;
      u->unitStates.Insert(US("Reclaiming"));
      u->focusId = EntityRef(e);
      // 0x61fccd..0x61fd41: m = min(fraction, health / MaxHealth) (0x4178c0), then 0x678e20 with the CHANGE m - fraction:
      // fraction + (m - fraction), clamped to [0, 1], nothing when the change is 0. Two roundings, so a damaged wreck
      // starts one float step above health / MaxHealth now and then. Seen: MP1 3907, army 5's 0x400000 on wreck
      // 0x2ff00006 (health 63 of 153): 1 + (0.411764711 - 1) = 0.411764741; its last step at 3910 pays 4.8322506
      // mass (ours 4.8322492), the army's mass income 8.93225002 vs 8.93224812 at 3911. MP2 5832, wreck 0x2ff00553
      // (4 / 27): the same.
      if (e->maxHealth > 0) {
        const float m = std::min(e->fractionComplete, e->health / e->maxHealth);
        const float d = m - e->fractionComplete;
        if (d != 0.0f) e->fractionComplete = std::clamp(e->fractionComplete + d, 0.0f, 1.0f);
      }
      c.state = 4;
      return kTaskRunning;
    }
    case 4: {
      // 0x61fdae..0x61fddd: the range test asks the target's GetUnit (vt+0x10) first: only a UNIT out of range ends
      // the reclaim here; a prop is reclaimed to the end from wherever the builder stands. (Seen: Economy 4483,
      // SACU 0x27 kills the T3 power generator 0x40 from 12.45 away, its footprint 5 counting, and goes on to its wreck,
      // footprint 1 (the DefaultWreckage prop has none): gap 10.45 > MaxBuildDistance 10; the original reclaims the wreck
      // until 4510, ours dropped it at once.)
      if (tu && gap > maxBD) return kTaskFailed;
      if (!c.reclaimStarted) {
        c.reclaimStarted = true;
        PushObject(L, e);
        sim.CallMethod(L, u, "OnStartReclaim", 1);
      }
      // The task has no pause test of its own (no unit+0x2a0 read in 0x61f000). FAF's patch at the prop step
      // (0x61ff7b -> 0x1291916, undone at 0x620071 -> 0x129196f) does, for a PROP: with the builder paused the
      // step is -1e-8 (0xb22bcc77) and both per-tick amounts 0 for that beat, so a paused engineer's prop still
      // loses one float step or so a beat and pays nothing. Seen: Skirmish 2963-2971, army 5's engineer 0x40000d
      // paused on a rock (804262534) at 0.25 left: the original's fraction 9 steps lower when it resumes, its last
      // step at 2974 pays 4.9999886 energy (ours 5.0), the army's energy income 55.9999886 vs 56 at 2975. (A UNIT
      // target while paused: ours keeps returning here; not read.)
      const bool pausedProp = u->paused && !tu;
      if (u->paused && !pausedProp) return kTaskRunning;
      const float step = pausedProp ? 9.99999994e-09f : c.reclaimStep;  // (the patch's -1e-8, sign as ours)
      const float perTick[2] = {pausedProp ? 0.0f : c.reclaimPerTick[0], pausedProp ? 0.0f : c.reclaimPerTick[1]};
      float k = u->request ? u->request->LimitingRate() : 1.0f;
      float old = e->fractionComplete;
      float f;
      if (tu) {
        // 0x61fe55..0x61ff32, a unit (unfinished: a finished one is taken in state 2): Materialize (vt+0x74) by the
        // step, which takes its share of health off what the unit has (Entity::AdjustHealth), then the health is
        // brought down to MaxHealth x min(fraction, health / MaxHealth) when that differs (0x4178c0, SetHealth
        // 0x679940). So damage the unit took while it was being reclaimed stays taken; ours set the health to
        // MaxHealth x fraction, giving it back. Seen: Skirmish 2592, army 1's unfinished xsb1103 0x100043, reclaimed
        // and shot (2 damage at 2591): the original's health is 5.474 at 2592, ours was 7.474.
        Materialize(sim, L, tu, -(c.reclaimStep * k));
        f = tu->fractionComplete;
        const float hr = tu->health / tu->maxHealth;
        const float m = hr > f ? f : hr;
        const float h = tu->maxHealth * m;
        if (h != tu->health) EntityAdjustHealth(L, tu, u, h - tu->health);
      } else {
        // a prop (0x61ff52..0x61ffbe): Prop::Materialize 0x6fa180 by the step, which moves the fraction only
        // (clamped to [0, 1]; the step is negative) and gives back the change; the prop's health stays as it is. FAF's
        // Prop.UpdateReclaimLeft counts health / MaxHealth x fraction, so a tree group partly reclaimed and then split
        // (TreeGroup.OnDamage -> Breakup -> SplitOnBonesByName) hands its fraction to its trees once, not twice. Seen:
        // MP1 3218, army 3's engineer 0x20001a reclaims a split tree (pine06_big_s3) in one beat: the original pays
        // 1.0714 E 0.1071 M, ours paid 0.2009 E 0.0201 M, 0.1875 = 3/16 times less again (inferred: its group had 3
        // of 16 steps left when it split, and ours had brought the group's health down with its fraction, so the
        // script's ReclaimLeft was 0.1875 x 0.1875). Its ally army 5 gets half of army 3's overflowing energy.
        f = std::clamp(old - step * k, 0.0f, 1.0f);
        e->fractionComplete = f;
      }
      float taken = old - f;
      u->workProgress = 1.0f - f;
      if (u->army && step > 0) {
        float q = taken / step;
        for (int i = 0; i < 2; ++i) {
          float got = q * perTick[i];
          u->army->econ.income[i] += got;
          u->army->econ.reclaimed[i] += got;
        }
      }
      // A prop at nothing left goes in its Materialize (Prop::Materialize 0x6fa25e..0x6fa28d: OnReclaimed, then
      // Destroy 0x679af0). An unfinished UNIT does not: the task's unit case (0x61fe55..0x61ff32) is Materialize and
      // SetHealth only, and neither Unit::Materialize 0x6a9f40 nor Entity::AdjustHealth / SetHealth kills or
      // destroys; the unit stays at fraction 0, health 0, for the rest of the beat. What takes it is its own decay in
      // the entity stage after the threads (Unit::MotionTick 0x6a90f6..0x6a91bb: being built, no Materialize up
      // for more than a beat -> the decay step, then health <= 0 -> OnDecayed, FAF's Destroy), so every reclaimer
      // of that beat still has its target, and each ends at the next beat's pre-check. Seen: MP2 8810, six of army
      // 1's engineers reclaim its abandoned unfinished 0x1000ed; ours destroyed it in the step that took the last
      // of it, so the two whose step came after (0x100065, 0x1000c2) dropped their order a beat early.
      if (!tu && f <= 0.0f && !e->destroyQueued) {
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

// The units COGrid::CollectEntitiesInBox 0x721dc0 (mask 0x100) gives for the box [x0, x1] x [-100, 100] x
// [z0, z1] (centre y 0, half height 100 [0xe4f744], as Sim::LocationIsFree and SIM_TryToBuild build it): the grid's
// cells, then the collision primitive's bounds and its CollideBox against the box (touching counts), in grid order.
std::vector<Unit*> UnitsMeetingBox(Sim& sim, float x0, float z0, float x1, float z1) {
  const Vec3 mn{x0, -100.0f, z0}, mx{x1, 100.0f, z1};
  std::vector<Entity*> cand;
  sim.entityGrid().GatherBox(mn.x, mn.z, mx.x, mx.z, 1, &cand);
  std::vector<Unit*> out;
  for (Entity* e : cand) {
    if (e->kind != Entity::Kind::Unit) continue;
    WorldShape ws;
    if (!GetWorldShape(e, &ws)) continue;
    Vec3 a, b;
    ShapeBounds(ws, &a, &b);
    if (a.x > mx.x || b.x < mn.x || a.y > mx.y || b.y < mn.y || a.z > mx.z || b.z < mn.z) continue;
    if (!ShapeOverlapsAABox(ws, mn, mx)) continue;
    out.push_back(static_cast<Unit*>(e));
  }
  return out;
}

// Sim::LocationIsFree 0x62da50 with flag 0 (CUnitMobileBuildTask state 3, 0x5f7b04, on the footprint rect
// task+0xc8): false when a unit in the rect's box (UnitsMeetingBox) other than `self`, alive and not on the Air
// layer, is mobile (vt+0x30: it blocks at once) or, not mobile, has a skirt rect (0x51ec50) that overlaps the rect
// strictly. Flag 1 (the factory's, 0x5fa88f) also lets a unit with a command at the head of its queue through
// (0x62dbca..0x62dbf9: unit+0x4b4 queue, its first entry's object non-null; the test of Unit::IsIdleState
// 0x6a7d60): FactorySiteBlocked. A unit the builder carries, builder+0x55c vt+0x70, does not count: not modelled,
// no mobile builder carries.
bool LocationFreeOfUnits(Sim& sim, lua_State* L, const Unit* self, const int r[4], int flag) {
  for (Unit* e : UnitsMeetingBox(sim, static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]),
                                 static_cast<float>(r[3]))) {
    if (e == self || e->dead || !e->blueprint) continue;
    if (flag && !e->commands.empty()) continue;  // 0x62dbca
    if (e->layer == "Air") continue;
    if (!(e->motion.bp && e->motion.bp->mobile())) {
      const FRect s = SkirtRect(*e->blueprint, GetUnitBpData(L, *e->blueprint), e->position.x, e->position.z);
      const float rx0 = static_cast<float>(r[0]), rz0 = static_cast<float>(r[1]);
      const float rx1 = static_cast<float>(r[2]), rz1 = static_cast<float>(r[3]);
      if (!(rx1 > s.x0 && s.x1 > rx0 && rz1 > s.z0 && s.z1 > rz0 && s.x1 > s.x0 && s.z0 < s.z1 && rx1 > rx0 &&
            rz0 < rz1))
        continue;
    }
    return false;
  }
  return true;
}

// SIM_TryToBuild 0x62dd40 with flag 0 (CUnitMobileBuildTask state 3, 0x5f7b71): the army's units in the box of
// the rect r that are mobile, not on the Air layer, alive, and stand still (position == previous, memcmp of
// u+0xb4 / u+0xd0; AIR units (u+0x68b) need not) are sent away, each in gather order:
// - along the ray from the rect's centre through the unit, from the unit's place (+0.5) out to 3 x the rect's
//   larger half size (a grid traversal 0x40d860, cell by cell), the first cell w whose footprint rect
//   (fistp(w - Size/2), integer w) does not touch r (closed), fits (terrain, structures), can be reserved and
//   Sim::LocationIsFree(that unit, rect, 0) holds;
// - else square rings around the unit stepping by 4 x its footprint size (900 candidates), the first that passes;
// - the spot is reserved (ReserveOgridRect) and the unit's navigator gets SetGoal(1x1 at that cell) (vt+8)
//   directly: no command, the move task it may run goes on listening.
// At the end every blocker's reservation is freed (0x62ea40).
// Seen: Economy 1779, commanders 36 / 38 building an Aeon land factory find commander 37 standing on its footprint
// (it is still thinking about its own way out): 37 is sent to (856, 430) and starts one beat later than with the
// goal (854, 427) its own PrepareMove gave it. (MOHO64_DEBUG_PREPMOVE=<blocker id> or -2: every call and spot.)
// Flag 1 (the factory's, 0x5fa8ae) sends only units with no command (0x62df78: Unit::IsIdleState 0x6a7d60, the
// queue unit+0x4b4 empty or its head gone); the spot tests inside are flag 0 in both cases (0x62e41d, 0x62e803).
void TryToBuild(Sim& sim, lua_State* L, const Army* army, const int r[4], int flag) {
  const float cx = static_cast<float>(r[0] + r[2]) * 0.5f, cz = static_cast<float>(r[1] + r[3]) * 0.5f;
  const float hx = static_cast<float>(r[2] - r[0]) * 0.5f, hz = static_cast<float>(r[3] - r[1]) * 0.5f;
  const float R = hz > hx ? hz : hx;
  static const long dbg = getenv("MOHO64_DEBUG_PREPMOVE") ? atol(getenv("MOHO64_DEBUG_PREPMOVE")) : -1;
  if (dbg == -2)
    Logf(LogLevel::Info, "trytobuild %u army %d rect %d %d %d %d", sim.tick(), army ? army->index : -1, r[0], r[1], r[2],
         r[3]);
  std::vector<Unit*> blockers;
  for (Unit* b : UnitsMeetingBox(sim, cx - hx, cz - hz, cx + hx, cz + hz)) {
    if (!(b->motion.bp && b->motion.bp->mobile())) continue;  // vt+0x30
    const bool moved = !vm::SameBits(b->position, b->lastPosition);  // (0x62df39..0x62df46, memcmp 0x4f0a50)
    if (dbg == -2)
      Logf(LogLevel::Info, "trytobuild %u cand %u pos %.9g %.9g %.9g last %.9g %.9g %.9g moved %d cmds %d layer %s",
           sim.tick(), b->id, b->position.x, b->position.y, b->position.z, b->lastPosition.x, b->lastPosition.y,
           b->lastPosition.z, moved, static_cast<int>(b->commands.size()), b->layer.c_str());
    if (moved && !BpInCategory(sim, b->blueprint, "AIR")) continue;
    if (b->layer == "Air" || b->dead || b->army != army) continue;
    if (flag && !b->commands.empty()) continue;  // 0x62df78 -> 0x6a7d60
    blockers.push_back(b);
  }
  for (Unit* b : blockers) {
    const NamedFootprint& fp = UnitFootprint(b);  // (Entity::GetFootprint, SIM_TryToBuild 0x62e235..)
    auto rectAt = [&](float wx, float wz, int* q) {
      q[0] = static_cast<int16_t>(RoundEven(wx - fp.sizeX * 0.5f));
      q[1] = static_cast<int16_t>(RoundEven(wz - fp.sizeZ * 0.5f));
      q[2] = q[0] + fp.sizeX;
      q[3] = q[1] + fp.sizeZ;
    };
    auto good = [&](const int* q) {
      const bool touches = q[2] >= r[0] && r[2] >= q[0] && q[3] >= r[1] && r[3] >= q[1] && r[0] < r[2] &&
                           r[1] < r[3] && q[0] < q[2] && q[1] < q[3];
      return !touches && GroundCellFits(sim, b, q[0], q[1]) && GroundCanReserveRect(sim, b, q) &&
             LocationFreeOfUnits(sim, L, b, q, 0);
    };
    const Vec3 pos = b->position;
    const float dx = pos.x - cx, dz = pos.z - cz;
    float ux = 0.0f, uz = 1.0f;  // (0, 0, 1) when the unit stands on the centre (memcmp with 0xf3d21c)
    uint32_t bx, bz;
    std::memcpy(&bx, &dx, 4);
    std::memcpy(&bz, &dz, 4);
    if (bx != 0 || bz != 0) {
      const float len = std::sqrt(dx * dx + dz * dz);
      if (len > 1e-6f) {  // 0xd71bfc
        const float inv = 1.0f / len;
        ux = dx * inv;
        uz = dz * inv;
      } else {
        ux = uz = 0.0f;
      }
    }
    const float fx = pos.x + ux * R * 3.0f, fz = pos.z + uz * R * 3.0f;  // 0xe4f718
    // InitGridTraversalLine 0x40d860 from (pos + 0.5) to (far + 0.5): mirrored (-0.0 - v, mask -1) toward -x / -z
    float sx = pos.x + 0.5f, ex = fx + 0.5f, sz = pos.z + 0.5f, ez = fz + 0.5f;
    int mxk = 0, mzk = 0;
    if (ex < sx) {
      sx = -0.0f - sx;
      ex = -0.0f - ex;
      mxk = -1;
    }
    if (ez < sz) {
      sz = -0.0f - sz;
      ez = -0.0f - ez;
      mzk = -1;
    }
    const float ldx = ex - sx, ldz = ez - sz;
    int gx = static_cast<int>(std::floor(sx)), gz = static_cast<int>(std::floor(sz));
    bool found = false;
    float wx = 0, wz = 0;
    int q[4];
    for (;;) {
      wx = static_cast<float>(gx ^ mxk);
      wz = static_cast<float>(gz ^ mzk);
      rectAt(wx, wz, q);
      if (good(q)) {
        found = true;
        break;
      }
      const int nx = gx + 1, nz = gz + 1;
      const float tx = (static_cast<float>(nx) - ex) * ldz, tz = (static_cast<float>(nz) - ez) * ldx;
      if (tz > tx) gx = nx;
      else gz = nz;
      if (static_cast<float>(gx) > ex || static_cast<float>(gz) > ez) break;
    }
    if (!found) {  // 0x62e4b1: rings of 4 x the footprint size around the unit, the first spot that passes
      const int step = 4 * std::max<int>(fp.sizeX, fp.sizeZ);
      int count = 0;
      for (int ring = 1; !found && count < 900; ++ring)
        for (int i = -ring; i <= ring && !found; ++i) {
          const int jStep = (i == -ring || i == ring) ? 1 : 2 * ring;
          for (int j = -ring; j <= ring; j += jStep) {
            wx = b->position.x + static_cast<float>(i * step);
            wz = b->position.z + static_cast<float>(j * step);
            ++count;
            rectAt(wx, wz, q);
            if (good(q)) {
              found = true;
              break;
            }
          }
        }
    }
    if (dbg == -2 || dbg == static_cast<long>(b->id))
      Logf(LogLevel::Info, "trytobuild %u unit %u at %.3f %.3f rect %d %d %d %d: %s (%d,%d)", sim.tick(), b->id, pos.x,
           pos.z, r[0], r[1], r[2], r[3], found ? "goal" : "no spot", found ? q[0] : 0, found ? q[1] : 0);
    if (!found) continue;
    GroundReserveRect(sim, b, q);
    if (b->motion.bp->motionType == kMotionAir) {  // (the air navigator's SetGoal: the cell's world point)
      MotionSetGoal(sim, b, std::vector<Vec3>{Vec3{q[0] + fp.sizeX * 0.5f, b->position.y, q[1] + fp.sizeZ * 0.5f}},
                    b->motion.passThrough, sim.tick());
    } else {  // CAiNavigatorLand::SetGoal 0x5a3ed0 (it keeps the speed-through request)
      const bool through = b->motion.nav && b->motion.nav->speedThroughGoal;
      LandNavSetGoalRect(sim, b, q[0], q[1], q[0] + 1, q[1] + 1, through);
    }
  }
  for (Unit* b : blockers) GroundFreeRect(sim, b);
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
      // A walk under way when the command's listener (MobileBuildCommandEvent, OnEvent 0x5f80c0) set state 0 and woke
      // the thread: the walk's CUnitMoveTask on top of the thread is what the wake runs; it ends (TaskTick 0x618a70
      // returns -1), its dtor clears Moving and aborts the navigator, and this task runs state 0 in the same pass, from
      // where the builder stands. (The o-grid cell state 0 reserved stays, as in the original, until the dtor or the
      // next reservation.) Seen: MP1 5685, army 0's player sets the target of the BuildMobile 0x1f (three T3 engineers,
      // a uab1301 at (930.5, 116.5)) again, to the same place: the original's 0x7c, walking to (924, 111), stops at 5686
      // (in range already) and builds from there, and the two that waited 49 beats behind a unit on the site start
      // again at once; the structure 0x84 is there from 5687. Ours walked on and made it at 5697.
      if (t.moving) {
        t.moving = false;
        EndWalk(u);
      }
      if (!t.bp || !UnitCanBuild(u, *t.bp)) return kTaskFailed;
      // (No snap here: the site was snapped by the task's ctor 0x5f6749, the command's OnEvent 0x5f8222 or the
      // guard's DispatchMobileBuildTask 0x6139de, each with the ground as it was then. Seen: MP1 3171, the Seraphim
      // commander 0x300000, guarding, starts the pgen 0x300009 that its engineer made at 3170; the making flattened
      // the ground at (99.5, 908.5) from 25.7050781 to 25.6875, and ours, snapping again here, aimed the build arm
      // 0.0176 lower than the original: the arm's pitch goal -0.3197 for -0.3161, the muzzle 2e-5 aside until 3177.)
      if (Entity* p = FindSiteProp(sim, L, u, t)) {  // 0x5f6ea0: clear the site first
        BuildTask* c = NewChild(sim, CommandType::Reclaim, "Reclaim");
        c->goalId = EntityRef(p);
        ReclaimTaskAbortMove(u);  // (the reclaim task's ctor)
        t.sub = c;
        t.subFrom = sim.tick() + 1;
        return kTaskRunning;
      }
      if (CanMove(u)) {  // 0x5f7440 state 0: move when it does not fit, is out of range or stands on the site
        // the site rect (task+0xd8) is the blueprint's SKIRT rect at the site (GetSkirtRect 0x51ec50: the
        // footprint only where SkirtSize is 0), and the builder's own rect is its skirt rect too (0x6a7600)
        const FRect sr = SkirtRect(*t.bp, GetUnitBpData(L, *t.bp), t.site.x, t.site.z);
        const FRect ur = SkirtRect(*u->blueprint, *u->bpData, u->position.x, u->position.z);
        const NamedFootprint& ufp = Footprint(*u->blueprint);
        bool overlap = ur.x0 <= sr.x1 && sr.x0 <= ur.x1 && ur.z0 <= sr.z1 && sr.z0 <= ur.z1;
        if (!UnitFitsAt(sim, u, u->position.x, u->position.z) || !InBuildRange(u, *t.bp, t.site) || overlap) {
          float excl[4] = {sr.x0 - 1.0f, sr.z0 - 1.0f, sr.x1 + 1.0f, sr.z1 + 1.0f};
          Vec3 a = t.site;
          PrepareMoveFor(sim, u, &a, excl, 1);  // spacing 1 (air_nav.md 7.1)
          int x0 = static_cast<int>(std::nearbyint(a.x - ufp.sizeX * 0.5f));
          int z0 = static_cast<int>(std::nearbyint(a.z - ufp.sizeZ * 0.5f));
          int rr[4] = {x0, z0, x0 + ufp.sizeX, z0 + ufp.sizeZ};
          GroundReserveRect(sim, u, rr);
          MoveTaskWalk(sim, u, Vec3{x0 + ufp.sizeX * 0.5f, a.y, z0 + ufp.sizeZ * 0.5f});  // NewMoveTask 0x5f7763
          t.moving = true;
        }
      }
      t.state = 1;
      return kTaskRunning;
    case 1:
      if (t.moving) {  // the move child: state 1 runs in the beat it ends
        if (u->motion.hasGoal && !u->motion.arrived && !u->motion.failed) return kTaskRunning;
        t.moving = false;
        EndWalk(u);
        // The cell state 0 reserved (ReserveOgridRect 0x6ab6f0 at 0x5f7723) stays reserved while the builder works:
        // only the task's dtor gives it back (FreeOgridRect 0x6ab760 at 0x5f6bd6, EndBuildTask), or the builder's
        // next reservation; the walk's own dtor frees only a cell it reserved itself (+0x92, 0x618891). Seen: MP1
        // 7595, army 6's engineer 0x60009c looks for a spot by the torpedo launcher at (951.5, 726.5): (952, 723) is
        // still held by 0x6000d1, building there since 7558, so the original takes (951, 723); its search from
        // (958, 716) then ends at (956, 720), and at 7623 it heads there; ours had freed the cell when 0x6000d1's
        // walk ended, took (952, 723) and drove north past the builders.
      }
      if (!InBuildRange(u, *t.bp, t.site)) return kTaskFailed;  // no re-move
      // (no stop here: CUnitMobileBuildTask's state 1 0x5f778a..0x5f78db tests the range (0x5f782d) and the site
      // (0x5f784f), aims the arm (0x5f78bf), steps the state and sets Building (0x5f78ce); the navigator is not asked.
      // A goal left from the order before goes on. Seen: MP1 10711, army 0's Aeon engineer 0x57 (hover) starts a
      // storage of FAF's RingWithStorages where it stands, with its reclaim walk's goal still on: ours stopped it,
      // the brake point turned its hull to the brake heading and its wobble tilt went 1e-6 off the original's, which
      // grew until army 0's 0x2 took another way round it at 10797.)
      {  // aim the build arm at the middle of the structure (0x5f78bf)
        const UnitBpData& sd = GetUnitBpData(L, *t.bp);
        SetArmAimTarget(sim, u, Vec3{t.site.x, t.site.y + sd.sizeY * 0.5f + sd.collisionOffsetY, t.site.z});
      }
      u->unitStates.Insert(US("Building"));
      t.state = 2;
      continue;  // return 0
    case 2:  // NeedToFaceTargetToBuild units turn on the spot first (TODO: no FA structure builder sets it)
      t.state = 3;
      continue;  // return 0
    case 3: {
      if (sim.tick() < t.waitUntil) return kTaskRunning;  // (a task that returned 10 or 50 sleeps first)
      if (t.tries > 10) return kTaskFailed;  // 0x5f7a0b (tries: the "units in the way" rounds below)
      if (Unit* ex = ExistingAtSite(sim, u, *t.bp, t.site)) {  // join: no arm wait, work this beat
        SetFocus(sim, L, u, ex, t);
        t.state = 4;
        continue;
      }
      if (!LocationIsFree(sim, *t.bp, t.site.x, t.site.z)) return kTaskFailed;
      {  // 0x5f7af9: Sim::LocationIsFree(builder, footprint rect +0xc8, 0); a unit there: SIM_TryToBuild on the
         // footprint rect (0x51ed80) with z0 - 1 and x1 + 1 (x0, z1 unchanged, as compiled), tries + 1, return 50.
         // Seen: bench 561, engineer 0x600004 does not start the hydro at (902.5, 80.5) while 0x600008 walks across
         // its footprint (a moving unit blocks, but is not sent away); ours used to create it at once.
        const NamedFootprint& sfp = Footprint(*t.bp);
        const int sx0 = RoundEven(t.site.x - sfp.sizeX * 0.5f), sz0 = RoundEven(t.site.z - sfp.sizeZ * 0.5f);
        const int fr[4] = {sx0, sz0, sx0 + sfp.sizeX, sz0 + sfp.sizeZ};
        if (!LocationFreeOfUnits(sim, L, u, fr, 0)) {
          const int rr[4] = {fr[0], fr[1] - 1, fr[2] + 1, fr[3]};
          TryToBuild(sim, L, u->army, rr, 0);
          ++t.tries;
          t.waitUntil = sim.tick() + 49;  // return 50: runs again 49 beats later
          return kTaskRunning;
        }
      }
      if (!u->armReady) return kTaskRunning;  // 0x5f7ba7: wait for the build arm (after the location checks)
      // 0x5f7bb7 -> 0x5f7bc0: a paused builder returns 10, after the arm's wait: the 10-beat rounds start when the
      // arm is ready. (Seen: Skirmish 3024, army 0's engineer 0x3c, paused through its arm's turn at 2858-2861 and
      // let go at 3017: asked every tenth beat from 2862, it starts its structure 0x5c at 3024; ours asked the
      // pause first, every beat, and started at 3025.)
      if (u->paused) {
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      }
      const UnitBpData& d = GetUnitBpData(L, *t.bp);
      if (!UnderUnitCap(u->army, d)) {
        if (++t.tries > 10) return kTaskFailed;
        t.waitUntil = sim.tick() + 10;
        return kTaskRunning;
      }
      float h = dmath::Atan2(u->position.x - t.site.x, u->position.z - t.site.z);  // (structures face the builder? kept level)
      (void)h;
      // (SUnitConstructionParams 0x585ab0 from 0x5f7ca9: the layer is the builder's (+0x120), not 0, so +0x25 = 1:
      // the site's height is kept)
      // ... and that layer (+0x28) is the wanted one, GetStartingLayer takes it when the footprint allows it. Seen: MP1
      // 8769, army 7's hover T3 engineer 0x700099 (layer Water) builds the Seraphim mobile sonar 0x700039 (xsb3202,
      // SurfacingSub) at (293, 622): the original's starts on the Water layer at the surface (y 17.5), ours chose Sub.
      const std::string& bl = u->layer;
      const uint8_t want = bl == "Land" ? 1 : bl == "Seabed" ? 2 : bl == "Sub" ? 4 : bl == "Water" ? 8 : bl == "Air" ? 0x10 : 0;
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, t.site, Quat{}, false, u, true, want);
      if (Entity* w = t.rebuildWreck ? sim.FindEntity(t.rebuildWreck) : nullptr)  // the rebuilt wreck goes
        if (!w->destroyQueued) sim.QueueDestroy(w);
      t.rebuildWreck = 0;
      if (!nu || !Alive(nu)) return kTaskFailed;
      nu->unitStates.Insert(US("NoReclaim"));
      SetFocus(sim, L, u, nu, t);
      t.state = 4;
      return kTaskRunning;
    }
    case 4: {
      if (sim.tick() < t.waitUntil) return kTaskRunning;
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) return kTaskFailed;
      // 0x5f7d9e..0x5f7dbb (state 4, the work): with the builder paused (unit+0x2a0) the task returns 10 (0x5f7bc0)
      // BEFORE the work helper 0x5f5bf0, so the structure is not worked on, not even the helper's paused
      // Materialize(0) that keeps a paused upgrade's unit from decaying (Skirmish 2071), and it decays
      // (Unit::MotionTick 0x6a910f: more than a beat since it was last worked on). Seen: Skirmish 2486, army 5's
      // engineer 0x50000b paused while building 0x500042: the original's structure loses health and progress from
      // 2487; ours held it.
      if (u->paused) {
        t.waitUntil = sim.tick() + 9;
        return kTaskRunning;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      target = FindUnit(sim, t.targetId);
      if (target) target->unitStates.Erase(US("NoReclaim"));
      t.completed = true;
      return kTaskDone;
    }
  }
  return kTaskFailed;
}

// Sim::LocationIsFree 0x62da50 for a factory's own footprint rect fr (0x51ed80 at the factory's place) with flag 1
// (CFactoryBuildTask state 0, 0x5fa88f): a live unit other than the factory, not on the Air layer and not carried,
// with no command at the head of its queue, whose collision primitive meets the rect's box (y -100 .. 100) keeps
// the factory from starting if it is mobile (vt+0x30), or, not mobile, if its skirt rect overlaps the rect
// (strictly). Seen: a ferry beacon set down on a factory; bench 649, Seraphim factory 0x200001 (footprint x 864..869)
// with its idle engineer 0x200008 at x 863.667 (box out to 864.02): the original starts the next unit at 698,
// 649 + 49 (return 50), and makes it 0x20000d; the unit rolling off with its Move (0x20000a) does not count.
// TryToBuild sends nobody there: 0x200008 hovers, its y moved by one float step from 647 to 648, so it "moved".
// (Units attached to a parent are let through as before: the carrier test builder+0x55c vt+0x70 is not modelled.)
bool FactorySiteBlocked(Sim& sim, lua_State* L, Unit* F, int fr[4]) {
  const NamedFootprint& fp = Footprint(*F->blueprint);
  fr[0] = RoundEven(F->position.x - fp.sizeX * 0.5f);
  fr[1] = RoundEven(F->position.z - fp.sizeZ * 0.5f);
  fr[2] = fr[0] + fp.sizeX;
  fr[3] = fr[1] + fp.sizeZ;
  for (Unit* e : UnitsMeetingBox(sim, static_cast<float>(fr[0]), static_cast<float>(fr[1]), static_cast<float>(fr[2]),
                                 static_cast<float>(fr[3]))) {
    // (IsDead vt+0x28 only, 0x62dbae: a destroy-queued unit still counts. Seen: Skirmish 1863, Chloe's Aeon T2 air factory
    // 0x60001f finishes its upgrade over the T1 one, 0x600016, which is destroy-queued in the same command stage: the
    // original's new factory waits (return 50) under the old one, ours started its first unit at once.)
    if (e == F || e->dead || !e->blueprint) continue;
    if (!e->commands.empty()) continue;  // flag 1: 0x62dbca
    if (e->parentId || e->unitStates.Has(US("Attached")) || e->layer == "Air") continue;
    if (!(e->motion.bp && e->motion.bp->mobile())) {
      const FRect r = SkirtRect(*e->blueprint, GetUnitBpData(L, *e->blueprint), e->position.x, e->position.z);
      const float qx0 = static_cast<float>(fr[0]), qz0 = static_cast<float>(fr[1]);
      const float qx1 = static_cast<float>(fr[2]), qz1 = static_cast<float>(fr[3]);
      if (!(qx1 > r.x0 && r.x1 > qx0 && qz1 > r.z0 && r.z1 > qz0 && r.x1 > r.x0 && r.z0 < r.z1)) continue;
    }
    return true;
  }
  return false;
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
      int fr[4];
      if (FactorySiteBlocked(sim, L, u, fr)) {  // 0x62da50 false: SIM_TryToBuild(army, rect, 1) 0x5fa8ba, return 50
        TryToBuild(sim, L, u->army, fr, 1);
        return wait(50);
      }
      // (the params of 0x5f54d0: no orientation given, so the unit's own heading; the script attaches it to the
      // factory's build bone in OnStartBuild.) The layer asked for (0x5fa8e5..0x5fa8f3, passed in edx) is 1 for a
      // blueprint with Air.CanFly (ubp+0x368), else 0, and 0x5f54d0 sets +0x25 (keep the height) only for a layer
      // asked for (0x5f551e, 0x5f552d): so the Unit ctor puts every other unit at CalcSpawnElevation 0x541540 of its
      // starting layer (0x6a5be7), a submarine at the water + its Physics.Elevation, not at the factory's height.
      // Seen (watch lines, "wn" / "ww" of a unit's first beat): Skirmish 1565, army 5's xss0203 0x500022 from the
      // naval factory 0x50001c at (458, 17.5, 851) stands at y 15 in the original until the build bone takes it at
      // the beat's end; ours at 17.5. (Ours keeps the height of a flyer as before; its layer request is not taken.)
      const bool flyer = GetMotionBlueprint(L, *t.bp, sim.blueprints()).canFly;
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, u->position, Quat{}, false, u, flyer, 0, true);
      if (!nu || !Alive(nu)) return wait(10);
      nu->fireState = u->fireState;
      SetFocus(sim, L, u, nu, t);
      u->unitStates.Insert(US("Building"));
      t.state = 1;
      [[fallthrough]];
    }
    case 1: {
      // CBuildTaskHelper::IsGood 0x5f5630: the helper's weak pointer (+8) set and the unit not dead (vt+0x28); a unit
      // whose Destroy is only queued is still good and still built (its dead flag comes with the destroy queue's drain).
      // Seen: MP1 15589, army 0 gives its Tempest 0x169 away (GiveUnitsToPlayer); FAF's TransferUnitsOwnership
      // Destroys the cruiser 0x1ec its external factory 0x1d0 is building: the original's factory builds on through
      // the beat (no OnStopBuild: no SetConsumptionActive call for 0x1d0 in probe mp1_15590) and its grant is spent;
      // ours stopped the build in the command stage and OnStopBuild handed the beat's whole grant (50.625 E, 5.625 M)
      // back to army 0 (economy 15590).
      Unit* target = FindUnitAny(sim, t.targetId);
      if (target && target->dead) target = nullptr;
      if (!target) {  // helper not good: OnStopBuild(false), build again after 10 ticks
        // (0x5fa97e..0x5fa989: OnStopBuild(false), state 0, return 10, and nothing else: the factory stays Building
        // while it waits, unlike the finish block's 0x5fa9da. Seen: MP3 2660, army 0's factory 0x1 whose engineer
        // was killed half built: the original's factory is Building throughout, ours dropped it for ten beats.)
        StopBuild(sim, L, u, t, false);
        t.state = 0;
        return wait(10);
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      target = FindUnit(sim, t.targetId);  // (read before OnStopBuild clears the focus)
      StopBuild(sim, L, u, t, true);       // Lua F:OnStopBuild(u): IssueMoveOffFactory, FinishBuildThread
      u->unitStates.Erase(US("Building"));
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
      if (sim.tick() < t.waitUntil) return kTaskRunning;  // (restarting after a lost upgrade unit, or paused)
      u->unitStates.Insert(US("Upgrading"));  // (the ctor 0x5f8484: +0x4a0 |= 0x40)
      // State 0 (0x5f88e2..0x5f8932): a mobile upgrader (IsMobile, vt+0x30) that moved in the last beat (its position,
      // u+0xb4, against the one before, u+0xd0, by memcmp 0x4f0a50) and has a navigator (u+0x54c) is told AbortMove
      // (vt+0x10) and asks again a beat later (return 1); otherwise state 1 in the same call. So a unit still rolling
      // when the order comes in brakes to a stand first, and the new unit is made where it stopped.
      if (!t.upgradeMoveDone) {
        if (u->motion.bp && u->motion.bp->mobile() && !vm::SameBits(u->position, u->lastPosition)) {
          MotionStop(u);
          t.waitUntil = sim.tick() + 1;
          return kTaskRunning;
        }
        t.upgradeMoveDone = true;
      }
      // State 1 (0x5f8936): a paused upgrader (unit+0x2a0) returns 10 before anything else: no NoCost, no unit. Seen:
      // MP1 2660, army 3 orders its mex 0x300018 to upgrade and pauses it in the same beat; the original's record has
      // no upgrade unit and the mex is Upgrading only (not NoCost) while it stays paused; ours made 0x300033 at 2661.
      if (u->paused) {
        t.waitUntil = sim.tick() + 9;  // return 10
        return kTaskRunning;
      }
      u->unitStates.Insert(US("NoCost"));  // (0x5f89c3, before the unit is made; cleared by the task's dtor 0x5f855a)
      // (0x5f8a11: the params of 0x5f54d0 as the factory's, so the new unit is not given the old one's orientation;
      // the layer asked for is the old unit's (+0x120, 0x5f89f1), so +0x25 = 1: its height is kept)
      Unit* nu = sim.CreateUnit(L, *t.bp, u->army, u->position, Quat{}, false, u, true, 0, true);
      if (!nu || !Alive(nu)) {
        u->unitStates.Erase(US("Upgrading"));
        return kTaskFailed;
      }
      nu->unitStates.Insert(US("BeingUpgraded"));
      SetFocus(sim, L, u, nu, t);
      t.state = 1;
      // CUnitUpgradeTask::TaskTick 0x5f8890 goes on to its next state in the same call (0x5f8afd -> 0x5f8b01):
      // the first work step is in the beat the unit is made. (Seen: a mex upgrade in the Upgrades replay, the new
      // unit at 3.2 health in its first beat in the original, 1.0 in ours.)
      [[fallthrough]];
    }
    case 1: {
      Unit* target = FindUnit(sim, t.targetId);
      if (!target) {  // CUnitUpgradeTask state 2 (0x5f8b2a): fail, then start over after 10 ticks
        StopBuild(sim, L, u, t, false);
        t.targetId = 0;
        t.state = 0;
        t.upgradeMoveDone = false;  // (0x5f8b30: state 0 again, its step included)
        t.waitUntil = sim.tick() + 10;
        return kTaskRunning;
      }
      if (!UpdateWorkProgress(sim, L, u, t)) return kTaskRunning;
      u->unitStates.Erase(US("Upgrading"));
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
    u->unitStates.Erase(US("Repairing"));
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
        u->unitStates.Erase(US("Repairing"));
      }
      if (CanMove(u) && !u->motion.hasGoal) MoveToward(sim, u, work->position);
      return kTaskRunning;
    }
    StopMoving(u);
    if (!t.started || t.targetId != EntityRef(work)) {
      t.order = "Repair";
      SetFocus(sim, L, u, work, t);
      u->unitStates.Insert(US("Repairing"));
    }
    if (UpdateWorkProgress(sim, L, u, t)) {
      StopBuild(sim, L, u, t, true);
      u->unitStates.Erase(US("Repairing"));
    }
    return kTaskRunning;
  }
  if (t.started) {
    StopBuild(sim, L, u, t, true);
    u->unitStates.Erase(US("Repairing"));
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
  // (CUnitReclaimTask::Execute 0x61f02e has a first case for a task without an entity (task+0x4c, the place at
  // +0x50: the place in the army's sight -> result 2, else on to the body). The ctor 0x61eb60 sets +0x4c only for a
  // null entity (0x61ecf2..0x61ed0a), and its one caller, IssueReclaimTask 0x61ef60 (0x61efc0), makes the task only
  // for a live one; but the task's command listener 0x620160 sets it when its command's new target names a place
  // (ReclaimTaskCommandEvent): TickPropReclaim takes that case.)
  // The original's task, state by state (above). (MOHO64_OLD_RECLAIM=1: the shorter form this had before, kept
  // for comparing the two on a replay.)
  static const bool oldForm = getenv("MOHO64_OLD_RECLAIM") != nullptr;
  // the move child on top of the task (TickPropReclaim state 1) holds it: the target test below waits for the
  // move's end too. Seen: Engineers 2629, engineers 2 and 3 share three prop reclaims; 3 takes the second prop
  // while 2 walks to it; the original's 2 walks on (Reclaim x2, Moving) until it arrives at 2688 and then drops
  // both orders (3 took the third at 2632); ours dropped the order at once and turned to the third
  if (!oldForm && t.moving && t.state == 1 && u->motion.hasGoal && !u->motion.arrived && !u->motion.failed)
    return kTaskRunning;
  if (!oldForm && t.reclaimAtPoint) return TickPropReclaim(sim, L, u, t);
  Entity* e = t.goalId ? sim.FindEntity(t.goalId) : nullptr;
  if (!e || e->destroyQueued || e->dead || e == u) return t.state >= 2 ? kTaskDone : kTaskFailed;
  Unit* tu = e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
  if (tu && tu->unitStates.Has(US("NoReclaim"))) return kTaskFailed;
  if (!oldForm) return TickPropReclaim(sim, L, u, t);
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
      u->unitStates.Insert(US("Reclaiming"));
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

// the guard's anchor (+0x90) against the zero vector, and a unit's position against the one before: memcmps of
// 12 bytes in the original (0x4f0a50: the dtor 0x611934, UpdateGuardFollowMoveGoal 0x613d1b / 0x613d31 / 0x613de9,
// SelectAssistOrCaptureCandidateUnit 0x61324d, the repair task 0x5f947c)
bool IsZero(const Vec3& v) { return vm::BitsZero(v); }
bool Cat(Sim& sim, const Unit* u, const char* c) { return u && BpInCategory(sim, u->blueprint, c); }
bool Mobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }
bool Moved(const Unit* u) { return !vm::SameBits(u->position, u->lastPosition); }
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
  // (one function, 0x62b780: a flyer is tested against the exclusion rect and takes the spacing as a ground unit does;
  // seen in MP1 1937: an interceptor guarding a transport is sent outside the transport's skirt, 4 cells off)
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) return AirPrepareMove(sim, u, p, excl, spacing);
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
// GuardAbort 0x614170: GuardBusy, then navigator.AbortMove() whenever the unit has a navigator (no goal test:
// an aircraft re-targets a point 1 s ahead every time, guard_task.md 4 / air_nav.md 4.7)
void GuardAbort(Unit* u) {
  u->unitStates.Insert(US("GuardBusy"));
  if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
}

// RefreshGuardedUnitFromTarget 0x611a40 (never clears anything)
void Refresh(Sim& sim, Unit* U, GuardData& g, const UnitCommand& c) {
  // (0x611a67..0x611a86: the target's entity through its weak link, CAiTarget::GetEntity 0x5e2ce0, and its GetUnit:
  // no test of the unit's life, so a unit destroyed in this beat (still linked until the drain) is G)
  Unit* gu = c.targetId ? FindUnitAny(sim, c.targetId) : nullptr;
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
    // CUnitCommand::GetPosition 0x6e8a30 (called at 0x612428): a command of more than one unit with a formation
    // (cmd+0x118) gives the unit's own formation cell (the formation's vt+0x1c); otherwise the target's position
    // (0x5e2a90) in the unit's footprint cell. Then the cell's centre (COORDS_ToWorldPos 0x50b0f0). Seen: MP3 3941,
    // eleven army-1 engineers guarding one ground point in a formation; the original sends each to its own cell,
    // ours sent all of them to the point's cell (315, 94).
    // The target's position is GetTargetPosGun with the centre flag clear (0x6e8a6a, CommandPosGun): an entity's
    // target point (its AI.TargetBones pick, or without one its collision centre), not its origin. Seen: MP1 6470,
    // army 0's uaa0303 0x8a given a Guard of the blip of army 2's power generator 0x200052 (origin 672.5, 182.5,
    // collision centre 672.4985, 182.528): the original's air target is (676, 183), from the cell (671, 182);
    // ours took the origin's cell (672, 182) and PrepareMove's ring gave (677, 183).
    Vec3 p = c->pos;
    if (!FormationGoal(sim, *c, U, &p)) p = CommandPosGun(sim, *c);
    const NamedFootprint& fp = Fp(U);
    int cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
    int cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
    return Vec3{cx + fp.sizeX * 0.5f, p.y, cz + fp.sizeZ * 0.5f};
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
      // 0x613f68: into the playable rect (whatever the army's flag), max(x0, x) then min(x1, that)
      const int* r = sim.playableRect;
      float v = ref.x > static_cast<float>(r[0]) ? ref.x : static_cast<float>(r[0]);
      ref.x = static_cast<float>(r[2]) > v ? v : static_cast<float>(r[2]);
      v = ref.z > static_cast<float>(r[1]) ? ref.z : static_cast<float>(r[1]);
      ref.z = static_cast<float>(r[3]) > v ? v : static_cast<float>(r[3]);
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
  // all float (x87 at PC_24): the squares summed x, z, then y (0x612506..0x612520), the root (0x452fc0) against
  // size + r (0x61252e..0x61253b)
  const float dx = U->position.x - ref.x, dy = U->position.y - ref.y, dz = U->position.z - ref.z;
  return std::sqrt((dx * dx + dz * dz) + dy * dy) > size + r;
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
  if (!G || U->unitStates.Has(US("Building")) || U->unitStates.Has(US("Repairing"))) return nullptr;
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
    // G's own head item is left to G (0x6129aa..0x6129df), unless it is G's only order and G repeats its queue
    // (G+0x2a2)
    if (i == 0 && c->count <= 1 && (n > 1 || !G->repeatQueue)) continue;
    const BlueprintInfo* bp = sim.blueprints().Find(c->blueprintId);
    if (!bp || !UnitCanBuild(U, *bp)) continue;
    if (c->count > 1) {
      --c->count;
    } else if (G->repeatQueue) {
      // 0x612a88..0x612ab1: on a repeating G the order taken gets its count back (cmd+0xe4 = cmd+0xe0) and goes to
      // the end of G's queue (0x6ee220 -> 0x6ee0b0), as a finished one does. Seen: MP3 2274, army 2's land factory
      // told to assist a repeating one took a single order; the original's queue kept its 19 orders, ours lost one.
      c->count = c->maxCount;
      G->commands.erase(G->commands.begin() + i);
      G->commands.push_back(c);
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
    if (!G->paused && G->unitStates.Has(US("Enhancing"))) return G;
    Entity* f = G->focusId ? sim.FindEntity(G->focusId) : nullptr;
    if (f && !G->unitStates.Has(US("Reclaiming"))) {
      if (f->kind != Entity::Kind::Unit) return nullptr;
      Unit* fu = static_cast<Unit*>(f);
      if (fu->army != U->army) return fu;
      if (fu->maxHealth > fu->health) return fu;
      if (fu->paused) return nullptr;
      if (fu->unitStates.Has(US("Enhancing"))) return fu;
      if (fu->unitStates.Has(US("SiloBuildingAmmo"))) return fu;
      return nullptr;
    }
    if (G->paused) return nullptr;
    return G->unitStates.Has(US("SiloBuildingAmmo")) ? G : nullptr;
  }
  if (g.engAssistFactory || g.engAssistEngineer) return nullptr;
  float r = GuardScanRadiusOf(U);
  Unit* best = nullptr;
  float bestD2 = std::numeric_limits<float>::infinity();
  for (Unit* u : sim.units()) {
    if (!Alive(u) || u == U || !u->blueprint) continue;
    // 0x61368d: inside the playable rect with a margin of 1 (the whole map if the guard's army may use it)
    if (!sim.IsWithin(u->position.x, u->position.z, 1.0f, Sim::UseWholeMap(U->army))) continue;
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
      else if (u->unitStates.Has(US("Enhancing")) || u->unitStates.Has(US("SiloBuildingAmmo"))) cand = u;
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
  t.linkSeq = ++sim.walkSeqNext;  // (the ctor links its listener at the end of the head command's list)
  t.order = "Repair";
  t.goalId = EntityRef(target);
  t.silo = silo;
  t.assist = B->unitStates.Has(US("Guarding")) || B->unitStates.Has(US("AssistingCommander"));
  if (target && target->unitStates.Has(US("Enhancing"))) {
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
  // a walk under way when the command's event set state 0 (GuardRepairChildrenCommandEvent): the woken move task ends
  if (t.moving && t.state == 0) {
    t.moving = false;
    EndWalk(u);
    if (u->walkPrepared) GroundFreeRect(sim, u);  // (the dtor's +0x92 free, 0x618891)
  }
  if (t.moving) {  // the move child: the task runs again when it ends
    if (u->motion.hasGoal) return kTaskRunning;
    t.moving = false;
    EndWalk(u);
  } else if (sim.tick() < t.waitUntil) {
    return kTaskRunning;
  }
  const float mbd = u->bpData->maxBuildDistance;
  for (int guard = 0; guard < 8; ++guard) {
    // target +0x88 is a weak pointer and the prologue tests only "null or the builder" (0x5f939d..0x5f93b5, no
    // IsDead): a killed target is kept until it is freed. Seen: Engineers 1709, two engineers guarding engineer 7
    // repair it while it dies (hp 0); the original keeps the repair (Repairing, 7 NoReclaim at 1712) and the guard
    // until 7 is gone at 1715; ours ended both at 1710
    Unit* tg = FindUnitAny(sim, t.goalId);
    if (!tg || tg == u) return kTaskFailed;
    if (tg->health == tg->maxHealth) {
      if (!t.assist) {
        if ((!tg->beingBuilt || tg->fractionComplete == 1.0f) && NothingToRepair(sim, tg)) return kTaskFailed;
      } else if (t.state < 4 && !t.silo && !tg->unitStates.Has(US("Upgrading")) && NothingToRepair(sim, tg)) {
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
        // 0x5f949c: a repairer whose army keeps to the playable rect waits (TaskTick returns 1) while its target
        // stands outside it (STIMap::IsWithin, margin 0)
        if (!Sim::UseWholeMap(u->army) && !sim.IsWithin(tg->position.x, tg->position.z, 0.0f, false)) return kTaskRunning;
        if (t.assist) {
          if (Unit* f = FactoryOf(sim, tg)) {
            R = f;
          } else if (tg->unitStates.Has(US("Upgrading"))) {
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
          MoveTaskWalk(sim, u, Vec3{x0 + fp.sizeX * 0.5f, a.y, z0 + fp.sizeZ * 0.5f});  // NewMoveTask 0x5f9829
          u->unitStates.Insert(US("Moving"));
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
        // 0x5f99ab: navigator.AbortMove() whenever there is a navigator (no goal test: an aircraft re-targets a
        // point 1 s ahead)
        if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
        u->unitStates.Erase(US("Moving"));
        t.workId = t.goalId;
        // aim = the target's bone -1 world transform (Unit::GetBoneWorldTransform 0x6aa5c0): half its SizeY
        // straight up from its position, in the world's axes (0x6aa637)
        if (Unit* w = FindUnitAny(sim, t.workId)) {
          float hy = w->bpData ? w->bpData->sizeY * 0.5f : 0.0f;
          SetArmAimTarget(sim, u, Vec3{w->position.x, w->position.y + hy, w->position.z});
        }
        [[fallthrough]];
      }
      case 2:
        // 0x5f9a27..0x5f9b2f: a builder whose blueprint says NeedToFaceTargetToBuild (bp+0x558) first turns to R:
        // its forward axis (VAxes3 0x4ec590, z axis, y dropped, normalised 0x44f7e0) against the way to R (y dropped,
        // normalised 0x452af0); a dot above 0.95 (0xe4f99c) clears the motion's facing (0x6b8590 with the zero
        // vector) and goes on (state 3, return 0); otherwise the facing is that way (0x6b8590) and the task asks
        // again next beat (return 1). R is the target, or its factory when state 1 ran in this same call. Seen: MP2
        // 2221, army 0's Mantis (NeedToFaceTargetToBuild) guarding an engineer turns on the spot before its build
        // beams (CreateCybranBuildBeams' Random) start; ours started them at once.
        if (u->bpData && u->bpData->needToFaceTargetToBuild) {
          Vec3 ax[3];
          BoxAxes(u->orientation, ax);
          // f: the normalised copy 0x44f7e0 (0x5f9a82: times 1 / length, zero for none); d: Normalize 0x452af0
          // (0x5f9ac9); SetFacing 0x6b8590 normalises what it is given once more (0x6b85b5) before it stores it
          Vec3 f{ax[2].x, 0.0f, ax[2].z};
          if (const float fl = std::sqrt((f.x * f.x + f.y * f.y) + f.z * f.z); fl > 0.0f) {
            const float fi = 1.0f / fl;
            f = {f.x * fi, fi * f.y, fi * f.z};
          } else {
            f = {};
          }
          Vec3 d{R->position.x - u->position.x, 0.0f, R->position.z - u->position.z};
          vm::NormalizeExe(d);
          const float dot = (f.z * d.z + f.y * d.y) + f.x * d.x;
          if (!(dot > 0.95f)) {
            vm::NormalizeExe(d);
            u->motion.faceDir = d;
            return kTaskRunning;
          }
          u->motion.faceDir = {};
        }
        t.state = 3;
        continue;
      case 3: {
        if (!u->armReady) return kTaskRunning;
        if (u->paused) {
          t.waitUntil = sim.tick() + 9;
          return kTaskRunning;
        }
        Unit* w = FindUnitAny(sim, t.workId);
        if (!w) return kTaskFailed;
        w->unitStates.Insert(US("NoReclaim"));
        SetFocus(sim, L, u, w, t);
        u->unitStates.Insert(US("Repairing"));
        t.state = 4;
        continue;
      }
      case 4: {
        if (!tg->unitStates.Has(US("Attached")) && RepairGap(u, tg) > 2.0f * mbd) return kTaskFailed;
        if (!UpdateWorkProgress(sim, L, u, t)) {
          Unit* w = FindUnitAny(sim, t.workId);
          if (!t.inheritWork || !w || w->unitStates.Has(US("Enhancing"))) return kTaskRunning;
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
  if (c->maxHealth > c->health || c->unitStates.Has(US("Enhancing")) || shieldDamaged ||
      c->unitStates.Has(US("SiloBuildingAmmo"))) {
    // 0x613a80: new CUnitRepairTask for any guard in REPAIR (the selection's test, 0x613110), with or without a
    // CAiBuilder (unit+0x554, made only for a blueprint with Economy.BuildableCategory or a REBUILDER, Unit ctor
    // 0x6a600f..0x6a60c2); the repair task asks for the builder only to aim it (0x5f99d5, 0x5f9b37). Seen: MP2 1972,
    // army 0's Mantis 0x1a (REPAIR, no BuildableCategory) guarding engineer 0x9 walks off to help build (Moving at
    // 1978); ours started nothing and stood.
    if (!U->bpData) return nullptr;
    BuildTask* t = NewChild(sim, CommandType::Repair, "Repair");
    InitRepair(sim, U, *t, c, c->unitStates.Has(US("SiloBuildingAmmo")) != 0);
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

// ---- capture (CUnitCaptureTask: ctor 0x603f90, TaskTick 0x6043e0, DoCallback 0x604e10, dtor 0x6050f0) ----

namespace {

// DoCallback: the start (the target counts one more capturer and both scripts hear it) or its undoing, which is
// also the failure report: when the target still lives it counts one less, stops being BeingCaptured with nobody
// left, and both scripts hear OnFailed...
void CaptureCallback(Sim& sim, lua_State* L, Unit* u, BuildTask& t, bool start) {
  if (start == t.capStarted) return;
  t.capStarted = start;
  Entity* e = t.goalId ? sim.FindEntity(t.goalId) : nullptr;
  Unit* T = e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
  if (!T) return;
  if (start) {
    T->unitStates.Insert(US("BeingCaptured"));
    ++T->capturers;
    PushObject(L, u);
    sim.CallMethod(L, T, "OnStartBeingCaptured", 1);
    PushObject(L, T);
    sim.CallMethod(L, u, "OnStartCapture", 1);
    return;
  }
  if (T->dead || T->destroyQueued) return;
  if (T->capturers > 0) --T->capturers;
  if (T->capturers == 0) T->unitStates.Erase(US("BeingCaptured"));
  PushObject(L, u);
  sim.CallMethod(L, T, "OnFailedBeingCaptured", 1);
  PushObject(L, T);
  sim.CallMethod(L, u, "OnFailedCapture", 1);
}

// capturer:GetCaptureCosts(target) -> time (seconds), energy, mass; false unless it gives three values
bool CaptureCosts(Sim& sim, lua_State* L, Unit* u, Unit* T, float out[3]) {
  int top = lua_gettop(L);
  PushObject(L, u);
  int obj = lua_gettop(L);
  lua_pushstring(L, "GetCaptureCosts");
  lua_gettable(L, obj);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return false;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, -2);
  lua_pushvalue(L, obj);
  PushObject(L, T);
  if (lua_pcall(L, 2, 3, top + 2) != 0) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_settop(L, top);
    return false;
  }
  bool ok = lua_isnumber(L, -3) && lua_isnumber(L, -2) && lua_isnumber(L, -1);
  if (ok)
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(lua_tonumber(L, -3 + i));
  lua_settop(L, top);
  (void)sim;
  return ok;
}

// One pass of TaskTick 0x6043e0 (the checks, then the state): kCaptureAgain where the original returns 0
constexpr int kCaptureAgain = 2;

int CaptureStep(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Entity* e = t.goalId ? sim.FindEntity(t.goalId) : nullptr;
  if (e && e->destroyQueued) e = nullptr;
  Unit* T = e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
  if (!e || (T && !ScriptBool(sim, T, "IsCapturable"))) {  // gone, or no longer capturable: over, as a success
    PushObject(L, u);
    sim.CallMethod(L, u, "OnStopCapture", 1);
    return kTaskDone;
  }
  if (!e->army || (T && T->layer == "Air") || e->army == u->army || combat::IsAlly(u->army, e->army)) return kTaskFailed;
  // the gap: the distance less the larger side of each footprint
  const NamedFootprint& uf = Fp(u);
  float tfp = 0;
  if (T) {
    const NamedFootprint& tf = Fp(T);
    tfp = static_cast<float>(std::max(tf.sizeX, tf.sizeZ));
  }
  const float dx = e->position.x - u->position.x, dz = e->position.z - u->position.z;
  const float gap = std::sqrt(dx * dx + dz * dz) - static_cast<float>(std::max(uf.sizeX, uf.sizeZ)) - tfp;
  if (T && T->unitStates.Has(US("BeingCaptured")) && gap > 10.0f) return kTaskFailed;
  switch (t.state) {
    case 0:  // within 5: stay; otherwise a move to a free spot beside the target
      t.state = 1;
      if (gap > 5.0f && CanMove(u)) {
        // 0x604663..0x6046be: the rect kept free is the target's skirt (Unit::GetSkirtRect 0x6a7600) only when the
        // task's CAiTarget holds a unit (vt+0x10); a target held through the army's recon blip (a player's order:
        // the interface only knows blips) gives the empty rect, so the spot may lie beside the target's footprint.
        // Seen: MP3 7921, army 0's engineer 0xb9 is ordered to capture the blip of army 1's radar 0x100122 at
        // (221.5, 230.5): the original's PrepareMove takes (220,231) (probe mp3_7789: SetGoal (220,231,221,232),
        // the batch target (220.5, 231.5)); ours kept the 2x2 skirt free and took (219,232).
        float excl[4] = {0, 0, 0, 0};
        if (T && !t.targetViaBlip) SkirtRect(T, excl);
        Vec3 a = e->position;
        PrepareMoveFor(sim, u, &a, excl, 1);
        int x0 = static_cast<int>(std::nearbyint(a.x - uf.sizeX * 0.5f));
        int z0 = static_cast<int>(std::nearbyint(a.z - uf.sizeZ * 0.5f));
        int rr[4] = {x0, z0, x0 + uf.sizeX, z0 + uf.sizeZ};
        GroundReserveRect(sim, u, rr);
        MoveTaskWalk(sim, u, Vec3{x0 + uf.sizeX * 0.5f, a.y, z0 + uf.sizeZ * 0.5f});  // NewMoveTask 0x60477d
        u->unitStates.Insert(US("Moving"));
        t.moving = true;
        return kTaskRunning;  // the move child runs; this task again when it ends
      }
      return kCaptureAgain;
    case 1: {
      if (gap > 10.0f) return kTaskFailed;
      if (!T || T->dead) return kTaskDone;
      GroundFreeRect(sim, u);
      float hy = T->bpData ? T->bpData->sizeY * 0.5f : 0.0f;  // the arm aims at the target's bone -1 (0x6aa637: straight up)
      SetArmAimTarget(sim, u, Vec3{T->position.x, T->position.y + hy, T->position.z});
      u->unitStates.Insert(US("Capturing"));
      t.state = 2;
      return kCaptureAgain;  // 0x6048d4
    }
    case 2: {  // the costs: the target's, and those of the units it carries (the carried part: inferred)
      if (!T) return kTaskDone;
      float c[3];
      if (!CaptureCosts(sim, L, u, T, c)) {
        Logf(LogLevel::Warning, "Failed to get valid capture costs from the target");
        return kTaskDone;
      }
      float energy = c[1], mass = c[2];
      float ticks = c[0] * 10.0f;
      if (1.0f > ticks) ticks = 1.0f;
      t.capTotal = static_cast<int>(static_cast<float>(t.capTotal) + ticks);
      for (Unit* x : sim.units()) {
        if (x->transportedBy != EntityRef(T) || x->dead || x->destroyQueued) continue;
        float cc[3];
        if (!CaptureCosts(sim, L, u, x, cc)) continue;
        float tk = cc[0] * 10.0f;
        if (1.0f > tk) tk = 1.0f;
        t.capTotal = static_cast<int>(static_cast<float>(t.capTotal) + tk);
        energy += cc[1];
        mass += cc[2];
      }
      if (t.capTotal < 1) t.capTotal = 1;
      t.capPerTick[0] = (0.0f > energy ? 0.0f : energy) / static_cast<float>(t.capTotal);
      t.capPerTick[1] = (0.0f > mass ? 0.0f : mass) / static_cast<float>(t.capTotal);
      if (t.capRequest) t.capRequest->live = false;
      t.capRequest = u->army ? u->army->econ.NewRequest() : nullptr;
      if (t.capRequest) {
        t.capRequest->requested[0] = t.capPerTick[0];
        t.capRequest->requested[1] = t.capPerTick[1];
      }
      CaptureCallback(sim, L, u, t, true);
      t.state = 3;
      return kCaptureAgain;  // 0x604c4b: the first beat's payment is tested at once
    }
    case 3: {  // a beat's worth when it is paid in full: every capturer of the target counts for each of them
      if (!T) return kTaskDone;
      const bool paid = !t.capRequest || (t.capRequest->granted[0] >= t.capPerTick[0] &&
                                          t.capRequest->granted[1] >= t.capPerTick[1]);
      if (paid) {
        if (t.capRequest) {
          u->consumedTick[0] += t.capRequest->granted[0];
          u->consumedTick[1] += t.capRequest->granted[1];
          t.capRequest->granted[0] = t.capRequest->granted[1] = 0;
        }
        t.capProgress = std::min(t.capTotal, t.capProgress + T->capturers);
        u->workProgress = static_cast<float>(t.capProgress) / static_cast<float>(t.capTotal);
      }
      static const bool dbgCap = getenv("MOHO64_DEBUG_CAPTURE") != nullptr;
      if (dbgCap)
        Logf(LogLevel::Info, "capture %u unit %u: %d of %d beats, per beat E %.3f M %.3f, granted E %.3f M %.3f, capturers %d",
             sim.tick(), u->id, t.capProgress, t.capTotal, t.capPerTick[0], t.capPerTick[1],
             t.capRequest ? t.capRequest->granted[0] : -1.0f, t.capRequest ? t.capRequest->granted[1] : -1.0f,
             T->capturers);
      if (t.capProgress < t.capTotal) return kTaskRunning;  // 0x604d37: return 1
      t.state = 4;  // 0x604d31 -> 0x604785: the end in the same beat
      return kCaptureAgain;
    }
    default: {  // done: the capturer stops, the target hears it was captured (its script changes sides)
      PushObject(L, e);
      sim.CallMethod(L, u, "OnStopCapture", 1);
      if (T) {
        PushObject(L, u);
        sim.CallMethod(L, T, "OnStopBeingCaptured", 1);
      }
      if (Entity* now = sim.FindEntity(t.goalId)) {
        PushObject(L, u);
        sim.CallMethod(L, now, "OnCaptured", 1);
      }
      t.completed = true;
      return kTaskDone;
    }
  }
}

}  // namespace

// TaskTick 0x6043e0. Every state but a capture still under way (state 3, return 1) and the move child returns 0
// (0x604789, 0x6048d4, 0x604c4b): the dispatch calls it again in the same beat, the checks first. Seen: Engineers
// 2370, engineer 7 arrives, is Capturing and the target BeingCaptured in the same beat (the request counted at 2371);
// ours one beat apart
int TickCapture(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (t.moving) {  // the move child: the task runs again when it ends
    if (u->motion.hasGoal) return kTaskRunning;
    t.moving = false;
    EndWalk(u);
  } else if (sim.tick() < t.waitUntil) {
    return kTaskRunning;
  }
  for (int guard = 0; guard < 8; ++guard) {
    const int r = CaptureStep(sim, L, u, t);
    if (r != kCaptureAgain) return r;
  }
  return kTaskRunning;
}

// dtor 0x6050f0: the focus goes, Capturing off, work progress 0, and the start is undone if it happened
void EndCapture(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (t.capRequest) {
    t.capRequest->live = false;
    t.capRequest.reset();
  }
  if (Alive(u)) {
    if (u->focusId == t.goalId) u->focusId = 0;
    u->unitStates.Erase(US("Capturing"));
    u->workProgress = 0;
    SetArmAimTarget(sim, u, Vec3{});
    if (t.moving) {
      t.moving = false;
      EndMoveChild(u);
    }
    GroundFreeRect(sim, u);
  }
  CaptureCallback(sim, L, u, t, false);
}

// ---- sacrifice (CUnitSacrificeTask: new 0x5fb8b0, ctor 0x5fad90, TaskTick 0x5fb180, dtor 0x5fae40) ----
// The unit walks up to its target, and ten ticks after it arrived its own build cost, times the blueprint's
// sacrifice multipliers, goes into the target's construction (Materialize) or enhancement (the script's
// WorkProgress); the engine then destroys the unit (the dtor, for a task that reached state 2).

Unit* UnitOfRef(Sim& sim, uint32_t ref) {
  Entity* e = ref ? sim.FindEntity(ref) : nullptr;
  return e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
}

int TickSacrifice(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (t.moving) {  // the move child: the task runs again when it ends
    if (u->motion.hasGoal) return kTaskRunning;
    t.moving = false;
    EndWalk(u);
  } else if (sim.tick() < t.waitUntil) {
    return kTaskRunning;
  }
  Unit* T = UnitOfRef(sim, t.goalId);
  if (!T || T->destroyQueued || T->layer == "Air" || T->dead) return kTaskFailed;
  switch (t.state) {
    case 0: {  // to a free spot beside the target; a unit in a factory: beside the factory
      SetArmAimTarget(sim, u, Vec3{});
      Unit* G = T;
      Unit* creator = T->beingBuilt ? UnitOfRef(sim, T->creatorId) : nullptr;
      if (creator && BpInCategory(sim, creator->blueprint, "FACTORY")) {
        G = creator;
      } else if (T->unitStates.Has(US("Upgrading"))) {  // the upgrade being built becomes the task's target
        if (Unit* f = UnitOfRef(sim, T->focusId)) {
          G = f;
          t.goalId = EntityRef(f);
        }
      }
      t.state = 1;
      if (CanMove(u)) {
        const NamedFootprint& uf = Fp(u);
        float excl[4] = {0, 0, 0, 0};
        SkirtRect(G, excl);
        Vec3 a = G->position;
        PrepareMoveFor(sim, u, &a, excl, 1);
        int x0 = static_cast<int>(std::nearbyint(a.x - uf.sizeX * 0.5f));
        int z0 = static_cast<int>(std::nearbyint(a.z - uf.sizeZ * 0.5f));
        int rr[4] = {x0, z0, x0 + uf.sizeX, z0 + uf.sizeZ};
        GroundReserveRect(sim, u, rr);
        MoveTaskWalk(sim, u, Vec3{x0 + uf.sizeX * 0.5f, a.y, z0 + uf.sizeZ * 0.5f});  // NewMoveTask 0x5fb809
        u->unitStates.Insert(US("Moving"));
        t.moving = true;
      }
      return kTaskRunning;
    }
    case 1: {
      // the gap: the distance less the larger side of each footprint
      const NamedFootprint& uf = Fp(u);
      const NamedFootprint& tf = Fp(T);
      const float dx = u->position.x - T->position.x, dz = u->position.z - T->position.z;
      const float gap = std::sqrt(dx * dx + dz * dz) - static_cast<float>(std::max(uf.sizeX, uf.sizeZ)) -
                        static_cast<float>(std::max(tf.sizeX, tf.sizeZ));
      const float reach = u->bpData ? u->bpData->maxBuildDistance : 0.0f;
      if (gap > reach) return kTaskFailed;
      if (T->unitStates.Has(US("Enhancing"))) {
        // T+0x2a0 set, or nothing asked of the economy (T+0x534): no work to add to. (That +0x2a0 is the
        // pause flag and +0x534 the unit's request: inferred.)
        if (T->paused) return kTaskFailed;
        if (!T->request || 0.0f >= std::max(T->request->requested[0], T->request->requested[1])) return kTaskFailed;
      }
      t.state = 2;
      PushObject(L, T);
      sim.CallMethod(L, u, "OnStartSacrifice", 1);
      t.waitUntil = sim.tick() + 9;  // return 10
      return kTaskRunning;
    }
    default: {
      const UnitBpData* d = u->bpData;
      const float mass = d ? d->buildCostMass * d->sacrificeMult[kMass] : 0.0f;
      const float energy = d ? d->buildCostEnergy * d->sacrificeMult[kEnergy] : 0.0f;
      // each as a fraction of the target's cost; a cost of 0 counts as a half (0xe4f724), the smaller one is given
      auto share = [](float given, float cost) { return cost > 0.0f ? given / cost : 0.5f; };
      if (T->unitStates.Has(US("Enhancing"))) {
        // (unit state 0x26, 0x5fb27d..0x5fb3a9) the target's own Lua fields, as the scripts write them: WorkProgress
        // (0x5fb2aa), WorkItemBuildCostMass (0x5fb2d3), WorkItemBuildCostEnergy (0x5fb31f), then WorkProgress
        // written back (0x5fb392 / 0x5fb3a4). (Ours read and wrote its side table of engine values: the scripts'
        // fields read 0 and the sacrifice's share never reached FAF's EnhanceTask.)
        float p = ScriptFieldNumber(L, T, "WorkProgress");
        const float rm = share(mass, ScriptFieldNumber(L, T, "WorkItemBuildCostMass"));
        const float re = share(energy, ScriptFieldNumber(L, T, "WorkItemBuildCostEnergy"));
        p += rm > re ? re : rm;
        if (p > 1.0f) p = 1.0f;
        SetScriptFieldNumber(L, T, "WorkProgress", p);
      } else {
        const UnitBpData* td = T->bpData;
        float rm = share(mass, td ? td->buildCostMass : 0.0f), re = share(energy, td ? td->buildCostEnergy : 0.0f);
        Materialize(sim, L, T, rm > re ? re : rm);
      }
      PushObject(L, T);
      sim.CallMethod(L, u, "OnStopSacrifice", 1);
      return kTaskDone;
    }
  }
}

void EndSacrifice(Sim& sim, Unit* u, BuildTask& t) {
  if (u && !u->destroyQueued) {
    u->unitStates.Erase(US("Repairing"));  // the dtor clears unit state 16
    if (t.moving) {
      t.moving = false;
      EndMoveChild(u);
    }
    if (t.state == 1) GroundFreeRect(sim, u);
    // a task that got as far as state 2 takes the unit with it, however it ends (0x5faeb1)
    if (t.state == 2) sim.QueueDestroy(u);
  }
}

// ---- teleport (CUnitTeleportTask: new 0x60aac0, ctor 0x60ab20, TaskTick 0x60ac00, dtor 0x60aec0) ----
// The engine only finds the landing spot and hands over to the script: OnTeleportUnit(teleporter, location,
// orientation) starts the script's teleport, which holds the unit Immobile while it charges; the task waits for
// that to end. Ended early (a new order): the script's OnFailedTeleport. The teleporter (+0x50, a weak pointer) is
// the unit itself for a Teleport order (the dispatch 0x6096fc), or a TELEPORTATION unit whose beacon the unit goes
// to (CUnitCallTeleport, MakeTeleportChild). The ctor aborts the unit's move (0x60abd0: navigator vt+0x10).
void TeleportTaskAbortMove(Unit* u) {
  if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
}
int TickTeleport(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  Unit* tp = UnitOfRef(sim, t.teleporterId);  // (the weak pointer: null once the teleporter is gone)
  if (t.state == 0) {
    Vec3 p = t.site;
    if (!LandFreeSpot(sim, u, &p)) return kTaskFailed;  // FindFreeSpot 0x62b200 (whole map)
    u->unitStates.Insert(US("Teleporting"));
    if (!tp || tp->dead) return kTaskFailed;  // 0x60ad94..0x60adbf (the dtor clears Teleporting)
    PushObject(L, tp);
    PushVector(L, p.x, 0.0f, p.z);  // the spot's x and z; y is 0
    PushVector(L, 0, 0, 0);
    const float q[4] = {t.orderOri.x, t.orderOri.y, t.orderOri.z, t.orderOri.w};
    for (int i = 0; i < 4; ++i) {
      lua_pushnumber(L, q[i]);
      lua_rawseti(L, -2, i + 1);
    }
    t.state = 1;
    sim.CallMethod(L, u, "OnTeleportUnit", 3);
    return kTaskRunning;
  }
  // state 1 (0x60ac39): the teleporter gone, dead (IUnit vt+0x28) or being destroyed (vt+0x2c) ends it; so does
  // another unit's beacon that is no longer ready (TransportIsTeleportBeaconReady, vt+0xa8)
  if (!tp || tp->dead || tp->destroyQueued) return kTaskFailed;
  if (tp != u && tp->transport && !TeleportBeaconReady(sim, tp)) return kTaskFailed;
  if (u->immobile || u->unitStates.Has(US("Immobile"))) return kTaskRunning;  // (Unit:SetImmobile keeps a flag here)
  t.state = 2;
  return kTaskDone;
}

// CUnitCallTeleport state 3 (0x601836..0x601862): new CUnitTeleportTask(parent = the call task, teleporter = T, the
// unit's transform of now, a ground target at dest, BuildGroundTeleportTarget 0x5e2340)
BuildTask* MakeTeleportChild(Sim& sim, Unit* u, Unit* teleporter, const Vec3& dest) {
  BuildTask* t = NewChild(sim, CommandType::Teleport, "Teleport");
  t->site = dest;
  t->orderOri = u->orientation;
  t->teleporterId = EntityRef(teleporter);
  TeleportTaskAbortMove(u);  // (the ctor)
  return t;
}

void EndTeleport(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  if (!u || u->destroyQueued) return;
  u->unitStates.Erase(US("Teleporting"));
  if (t.state != 2) sim.CallMethod(L, u, "OnFailedTeleport", 0);
  u->motion.surfaceNext = true;  // motion+0x90 = 1: the next motion tick takes the ground again
}

BuildTask* MakeRepairChild(Sim& sim, Unit* u, Unit* target, bool silo) {
  BuildTask* t = NewChild(sim, CommandType::Repair, "Repair");
  InitRepair(sim, u, *t, target, silo);
  return t;
}
BuildTask* MakeReclaimChild(Sim& sim, Unit* u, Entity* target) {
  BuildTask* t = NewChild(sim, CommandType::Reclaim, "Reclaim");
  t->goalId = EntityRef(target);
  ReclaimTaskAbortMove(u);  // (the reclaim task's ctor)
  return t;
}

BuildTask* StartGuardTask(Sim& sim, Unit* u, const UnitCommand& c) {
  if (!c.targetId && !c.hasPos) return nullptr;  // no target: Stop
  // An entity order whose unit is gone has no target: CAiTarget::HasTarget 0x5e2a10 asks a type-1 target for its live
  // entity only (a unit: not dead, vt+0x28), never for a place; DispatchTask's Guard case then goes the way of Stop
  // (0x609775). The place ours keeps beside the entity counts only for a blip-held target (kept as it was). Seen: MP2
  // 6205, army 1's engineers 0x10006a / 0x10000a reach the end of a Move with a Guard queued behind it on a mass
  // extractor that has since been destroyed (its id 0x10003f now another unit's): the original's queue is empty at
  // 6205; ours guarded the extractor's old place.
  // HasTarget asks the unit's IsDead (IUnit vt+0x28 = 0x6a49f0, +0xa1), which a Destroy sets only in the beat's drain
  // (Unit::OnDestroy 0x6a8a31): a unit destroyed earlier in this beat still counts, and the guard task is made. It ends
  // in its first tick (its unit destroyed: the validity test of TickGuard), and its dtor's status call (0x6ac940) sees
  // this order at the head. Seen: MP1 5417, army 6's engineers 0x60000c / 0x600017 / 0x60004c hold two Guards of the
  // structure 0x600036, whose upgrade 0x60005e finished; its OnStopBuild destroys it (structureunit.lua:1018) earlier
  // in the command stage. The first Guard ends (dtor: both Guards queued, speed-through on); the original makes the
  // second one's task, whose dtor (the second Guard, then BuildMobile) turns it off again (0 at 5417); ours took the
  // second Guard for one without a target and left it on.
  if (c.targetId && !(FindUnitAny(sim, c.targetId) && !FindUnitAny(sim, c.targetId)->dead) &&
      (!c.hasPos || !c.targetViaBlip))
    return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Guard;
  t->order = "Guard";
  t->goalId = c.targetId;
  auto gd = std::make_shared<GuardData>();
  GuardData& g = *gd;
  u->unitStates.Insert(US("Guarding"));
  if (!u->commands.empty()) g.cmd = u->commands.front();
  Refresh(sim, u, g, c);
  Unit* G = FindUnit(sim, g.G);
  if (G && (G->beingBuilt || G->unitStates.Has(US("Upgrading"))) && !Cat(sim, G, "FACTORY") && !Cat(sim, G, "SHIELD") &&
      !Cat(sim, G, "SILO") && !Mobile(G))
    g.endWhenIdle = true;
  AttackerUnpackStop(sim, u);  // 0x6115a0..: standing unpacked with AI.NeedUnpack: SetDesiredTarget(none)
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

// CUnitGuardTask::OnEvent 0x6147b0, the guard task's listener on its command (task+0x34, linked into cmd+0x34 by the
// ctor): on any event of the command (CUnitCommand::SetTarget 0x6e8820 raises event 0 through 0x6e94a0 at once, for
// every listener) it copies the command's target (cmd+0x11c -> task+0x54, 0x5d5670: no new pick, no draw) and runs
// RefreshGuardedUnitFromTarget 0x611a40: G (+0x7c) and Unit::SetGuardedUnit 0x6a76a0. The listeners are told in the
// command's listener order; ours goes by unit id (inferred).
void RefreshGuardTasks(Sim& sim, UnitCommand& c) {
  if (c.type != CommandType::Guard) return;
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* h : holders) {
    if (h->dead || !h->task || h->task->type != CommandType::Guard || !h->task->gdata) continue;
    GuardData& hg = *h->task->gdata;
    if (hg.cmd.lock().get() != &c) continue;
    Refresh(sim, h, hg, c);
  }
}

// The same SetTarget event (0x6e8820 -> 0x6e94a0) reaches the repair child of a guard, which listens to the guard's
// command too (CUnitRepairTask's OnEvent 0x5f9d60, ids_repair_placement.md 2.4: its work target dropped with
// OnStopBuild(true), target := the command's target unit, noMove = 0, state 0, the thread woken), and that child's walk
// (a CUnitMoveTask, which links the queue's head command itself: woken, its TaskTick returns -1 and its dtor aborts the
// move; TickRepair ends it). Seen (probe mp1_2701): MP1 2701, army 7's engineers guard the upgrading mex 0x700005 on
// one Guard; when the upgrade is done another engineer's guard retargets the command, and 0x700019's repair walk ends
// at once (CUnitMoveTask::OnEvent 0x618c40, its dtor's AbortMove 0x618886); the repair task runs state 0 on the
// finished unit and ends, and the guard, with more Guards queued, ends as well (3 -> 2). Ours walked on.
void GuardRepairChildrenCommandEvent(Sim& sim, UnitCommand& c, Unit* self) {
  if (c.type != CommandType::Guard) return;
  lua_State* L = sim.L();
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  // The event goes down the command's listener list in link order (0x6e94cf..0x6e954b: from the head's first node,
  // each node moved to the end before it is called), and a repair child's listener is linked at the end when the child
  // is made: the children are told, and their suspended threads re-linked, in the order they were made. Seen: MP1
  // 7503, army 0's engineers 0x5a and 0x28, whose repair children of one Guard were made in that order at 6859: the
  // original's command stage runs 0x5a before 0x28 from then on, and at 7769 0x5a takes the spot (777, 161) beside
  // 0x2f first, so 0x28 goes to (777, 153) by the south; ours went by unit id and gave 0x28 the north spot.
  auto seqOf = [](const Unit* h) -> uint64_t {
    const BuildTask* gt = h->task;
    return gt && gt->gdata && gt->gdata->child ? gt->gdata->child->linkSeq : 0;
  };
  std::stable_sort(holders.begin(), holders.end(), [&](const Unit* a, const Unit* b) { return seqOf(a) < seqOf(b); });
  for (Unit* h : holders) {
    if (h == self || !Alive(h) || h->commands.empty() || h->commands.front().get() != &c) continue;
    BuildTask* gt = h->task;
    if (!gt || !gt->gdata || !gt->gdata->child || gt->gdata->child->type != CommandType::Repair) continue;
    BuildTask& ch = *gt->gdata->child;
    const bool asleep = TaskThreadAsleepOnMove(h, &ch);  // (asked before the event changes the child)
    if (ch.workId) {
      if (Unit* w = FindUnitAny(sim, ch.workId)) w->unitStates.Erase(US("NoReclaim"));
      h->workProgress = 0;
      if (ch.started) StopBuild(sim, L, h, ch, true);
      ch.workId = 0;
    }
    Entity* e = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
    ch.goalId = e && e->kind == Entity::Kind::Unit ? EntityRef(e) : 0;
    ch.noMove = false;
    ch.state = 0;
    gt->gdata->childFrom = 0;  // (woken: counter 0)
    // the listener's tail 0x5f9e03..0x5f9e37: the thread's frame count 0 and, when it is suspended (the walking
    // child's move task suspended it), re-linked at the stage's end, as the reclaim task's listener does
    // (RetargetPatrolTasks, commands.cpp)
    if (asleep) sim.ResumeCommandThread(h);
  }
}

// The guard's walk next to G (state 0: NewMoveTask 0x6190a0 from TaskTick 0x614549 with no command and +0x94 = 0)
// is a CUnitMoveTask that links the queue's head command, the Guard order itself (ctor 0x6180e0: task+0x88 from
// unit+0x4b4's first command, its listener +0x54 in cmd+0x34). The same SetTarget event (0x6e8820 -> 0x6e94a0)
// therefore reaches it: CUnitMoveTask::OnEvent 0x618c40 (read) with +0x94 and +0x95 clear hands its navigator the
// command's position for the unit (CUnitCommand::GetPosition 0x6e8a30, the new target's place in the unit's footprint
// cell, SNavGoal 0x5a2cb0, SetGoal vt+8; no speed-through change), whatever the anchor was. GetPosition asks the target
// for its point with the centre flag clear (0x6e8a6a): a structure's target bone, not its origin. (Probe mp2_4910:
// the original's goal for both is the cell (247, 416); the structure's origin gave (247, 418).) Seen: MP2 4909, army 0's
// engineers 0x93 and 0xa5 walk to their anchors by a structure whose upgrade another guard of the same order hands
// over; the original's walks turn toward the new unit at once (speed-through 1 from 4910: the new path's first
// waypoint is not in the goal); ours walked on to the old anchors. (Land navigators only: an air walk's SetGoal is
// not modelled here.) The listeners are told in the command's listener order; ours goes by unit id (inferred).
void GuardWalksCommandEvent(Sim& sim, UnitCommand& c) {
  if (c.type != CommandType::Guard) return;
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* h : holders) {
    if (!Alive(h) || h->commands.empty() || h->commands.front().get() != &c) continue;
    BuildTask* gt = h->task;
    if (!gt || !gt->gdata || !gt->gdata->childMove || gt->gdata->child) continue;
    TaskWalkCommandEvent(sim, c, h);
  }
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
    // The walk of state 0 is a CUnitMoveTask (NewMoveTask 0x6190a0, SetGoal from 0x6190cb); when it ends, its dtor
    // 0x618700 clears Moving (0x6187bd) and then calls the navigator's AbortMove (0x618886): SetSpeedThroughGoal(0)
    // and MakeIdle -> SetWaypoints(0, 0), which rebuilds a brake spline still standing from the unit's state of now
    // and hands out its point[0] (after an arrival the navigator's Stop has removed the spline: nothing). Seen (probe
    // mp1_1958): MP1 army 1's engineer 0x10000e, its walk ended by a stream AbortNavigation (Lua navigator:AbortMove,
    // after the motion of 1957): at 1958, after its steering handed out point[1] of that brake, the move task's dtor
    // brakes again from 0.266 and the motion takes the new point[0] (0.19). Ours ended the walk without the abort and
    // stepped to point[1] (0.342).
    EndWalk(U);
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
  // (b) a changed U+0x4e0 (upgrade hand-over), 0x6142d3..0x614369: the Guard command, found in U's queue by its id
  // (FindCommandIndex 0x6edd30), is retargeted with CUnitCommandQueue::SetCommandTarget 0x6ee470: for an entity not
  // dead (+0x99) a new CAiTarget (UpdateTarget 0x5d55b0 at 0x6ee4ae: one target-point draw for a unit with target
  // bones), then CUnitCommand::SetTarget 0x6e8820. Seen: Skirmish 1863, army 6's five engineers guarding the air
  // factory 0x600016, whose upgrade 0x60001f took them over (NotifyUpgrade, 1862): the original draws five target
  // points (0x6aaf94) at the start of the beat; ours retargeted without a draw.
  if (g.G != U->guardedId) {
    g.G = U->guardedId;
    if (auto c = g.cmd.lock())
      if (Unit* nG = FindUnitAny(sim, g.G))
        if (std::find(U->commands.begin(), U->commands.end(), c) != U->commands.end() && !nG->dead) {
          c->targetPoint = TargetPointDraw(sim, nG);
          c->targetId = g.G;
          // SetTarget tells the command's listeners at once (0x6e94a0): every guard task of a shared Guard order takes
          // the new unit now, so the others find G == U+0x4e0 in their own turn and neither retarget nor draw.
          // Seen (probe mp3_tpoint): MP3 3581, army 3's engineers 0x300015, 0x300019 and 0x300012 guard a structure
          // whose upgrade 0x30004d finished; only the first retargets (one draw, from SetCommandTarget 0x6ee4b3), the
          // two after it already hold 0x30004d. Ours retargeted all three (three draws).
          RefreshGuardTasks(sim, *c);
          GuardRepairChildrenCommandEvent(sim, *c, U);
          GuardWalksCommandEvent(sim, *c);
        }
  }
  U->unitStates.Erase(US("GuardBusy"));
  // (c) validity
  Unit* G = FindUnit(sim, g.G);
  if (g.hasUnit) {
    if (!G || G->dead || G->destroyQueued || (G->unitStates.Has(US("Attached")) && !G->beingBuilt)) return kTaskDone;
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
      // NewMoveTask 0x6190a0 (from TaskTick 0x614549): the navigator's SetGoal (0x6190c9), then the CUnitMoveTask
      // ctor 0x6180e0, which ends in Unit::UpdateSpeedThroughStatus 0x6ac940 (0x6186a1) on every path. Seen: MP3
      // 4758, three of army 1's engineers on a Guard with a second Guard queued; the first ends (the builder
      // context, ShouldAbortGuardForBuilderContext 0x612600) and its dtor's status call, the queue still holding both
      // Guards, turns speed-through on; the second Guard's walk starts in the same beat and its move task's status
      // call, the queue now holding one, turns it off again (original 0 at 4758). Ours walked without that call.
      MoveTaskWalk(sim, U, Vec3{cx + fp.sizeX * 0.5f, a.y, cz + fp.sizeZ * 0.5f});
      U->unitStates.Insert(US("Moving"));
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
        child->site = SnapStructurePosition(sim, *bp, site);  // DispatchMobileBuildTask 0x6139de, the ctor 0x5f6749
        break;
      }
      // ResolveGuardedReclaimFocusEntity 0x612e80: G's focus is Unit::GetFocusEntity 0x585b10, the weak link +0x4d0,
      // null once the focus is freed (not merely dead). Seen: Engineers 2025, engineer 7 guards 0xa, whose reclaim
      // target (a tank) was reclaimed and freed in 2024; at 7's turn in 2025 0xa still reads Reclaiming in ours; the
      // original's 7 starts nothing (no GuardBusy), ours started a reclaim of the freed id (it failed at 2026)
      if (g.hasUnit && G && U->bpData && U->bpData->hasBuilder && Cat(sim, U, "RECLAIM") &&
          G->unitStates.Has(US("Reclaiming")) && G->focusId && sim.FindEntity(G->focusId)) {
        GuardAbort(U);
        child = NewChild(sim, CommandType::Reclaim, "Reclaim");
        child->goalId = G->focusId;
        ReclaimTaskAbortMove(U);  // (the reclaim task's ctor: a second AbortMove, as in the original)
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
  // The dtor has no test of the unit's life: GuardBusy and Guarding are cleared and SetGuardedUnit(U, null) is
  // called (0x611881..0x6118b6) for a unit that dies too. Seen: MP3 3754, army 3's 0x30003b, guarding its commander
  // 0x300000, is killed; the original's dying unit leaves the commander's guard formation (in formation 0, its own
  // top speed 3.8), ours kept the link and the dead unit took a slot in it (2.7455) from 3755.
  // The same for the anchor's o-grid rect (0x611929..0x611946: when the anchor, task+0x90, is set, FreeOgridRect
  // 0x6ab760 on the unit): a guard killed on its walk gives its spot back. Seen (probe mp1_4989): MP1 4415, army 5's
  // engineer 0x500035 is killed walking to its anchor (426,848) beside the structure it guards; at 4989 six engineers
  // given a Guard on that structure take their spots in thread order, the first of them (0x500027) (426,848) in the
  // original. Ours had kept the dead unit's reservation, so each took the next spot down and 0x50006b's path went
  // the other way round at 5014.
  // (The rest of the dtor is kept to living units here: the AbortMove and the speed-through status also run for a
  // dying unit in the original, 0x611947..0x611977; not modelled.)
  U->unitStates.Erase(US("GuardBusy"));
  U->unitStates.Erase(US("Guarding"));
  SetGuardedUnit(sim, U, nullptr);
  if (!IsZero(g.anchor)) GroundFreeRect(sim, U);
  if (!Alive(U)) return;
  if (g.childMove) U->unitStates.Erase(US("Moving"));
  // 0x611947..0x611972: the navigator (unit+0x54c) is told AbortMove (vt+0x10) when the unit moved this beat
  // (+0xb4 != +0xd0, 0x4f0a50). No goal test: an aircraft re-targets the point one second ahead. Seen (MP3 487):
  // Cybran build drones (POD) circling a finished mex; their guard ends and the original's targets jump to the
  // 1-s-ahead point while ours, without a goal, flew on to the old one.
  if (U->motion.bp && U->motion.bp->mobile() && Moved(U)) MotionStop(U);
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
      if (t->bp) t->site = SnapStructurePosition(sim, *t->bp, t->site);  // the ctor 0x5f6749 (COORDS_ToWorldPos 0x50b0f0)
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
      // DispatchTask 0x6099b5: the order's focus (CUnitCommand::GetFocus 0x5d5980; a blip stands for its source,
      // ReconBlip::GetCreator 0x579670) and, when that is a unit, CUnitRepairTask::operator new 0x5f9e50. No test
      // for a CAiBuilder (unit+0x554), as in DispatchAssistOrCaptureTask 0x613a80. Seen: MP2 2555, army 0's Mantis
      // 0x13 (REPAIR, no BuildableCategory) given Repair on the unfinished 0x2e walks to it and works on it from
      // 2594; ours made no task.
      if (!u->bpData) return nullptr;
      InitRepair(sim, u, *t, FindUnit(sim, c.targetId), false);
      t->goalId = c.targetId;
      break;
    case CommandType::Script:
      return StartScriptTask(sim, u, c);
    case CommandType::TransportLoadUnits:
    case CommandType::Dock:  // (DispatchTask 0x609ac6: one case for both)
    case CommandType::TransportReverseLoadUnits:  // (0x609f8a)
    case CommandType::TransportUnloadUnits:
    case CommandType::TransportUnloadSpecificUnits:
    case CommandType::Ferry:
      return StartTransportTask(sim, u, c);
    case CommandType::Attack:
    case CommandType::FormAttack:
      return StartAttackTask(sim, u, c);
    case CommandType::Nuke:
    case CommandType::Tactical:
      return StartFireAtTask(sim, u, c);  // DispatchTask 0x609692 / 0x6096a9
    case CommandType::OverCharge:
      return StartOverchargeTask(sim, u, c);
    case CommandType::Reclaim:
      if (!u->bpData || !u->bpData->hasBuilder) return nullptr;
      // (an order without a live entity, a place included, makes no task: IssueReclaimTask 0x61ef60; the
      // dispatcher pops it before this, StartHead)
      if (!c.targetId || !sim.FindEntity(c.targetId)) return nullptr;
      t->order = "Reclaim";
      t->goalId = c.targetId;
      t->targetViaBlip = c.targetViaBlip;  // (its target link holds the order's blip until it is in range: TickPropReclaim)
      ReclaimTaskAbortMove(u);  // (the reclaim task's ctor: DispatchTask 0x60989c -> IssueReclaimTask 0x61ef60)
      break;
    case CommandType::Capture:  // DispatchTask 0x6098a9 -> CUnitCaptureTask::Create; the ctor takes the focus
      if (!c.targetId) return nullptr;
      t->order = "Capture";
      t->goalId = c.targetId;
      t->targetViaBlip = c.targetViaBlip;  // (its CAiTarget holds the blip: CaptureStep's spot search)
      // the ctor 0x603f90 first makes its own CAiTarget of the order's entity (UpdateTarget 0x5d55b0 at 0x604020):
      // one Unit::PickTargetPoint draw for a unit (or a unit's blip) with AI.TargetBones, the value unused. Seen:
      // Engineers 2272, three engineers given Capture of a blip at 2271 draw 0x6aaf94 once each as their tasks start
      TargetPointDraw(sim, sim.FindEntity(c.targetId));
      u->focusId = c.targetId;
      sim.CallMethod(sim.L(), u, "OnAssignedFocusEntity", 0);
      break;
    case CommandType::Teleport: {  // DispatchTask 0x6096fc: the unit is its own teleporter
      Entity* e = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
      if (!e && !c.hasPos) return nullptr;
      t->order = "Teleport";
      t->site = e ? e->position : c.pos;
      t->orderOri = u->orientation;
      t->teleporterId = EntityRef(u);
      TeleportTaskAbortMove(u);  // (the ctor)
      break;
    }
    case CommandType::Sacrifice: {  // DispatchTask 0x609a12
      Unit* T = UnitOfRef(sim, c.targetId);
      if (!T) return nullptr;
      if (!(T->beingBuilt || T->unitStates.Has(US("Upgrading")) || T->maxHealth > T->health)) {
        // a builder at work: the sacrifice goes to what it builds
        T = T->unitStates.Has(US("Building")) ? UnitOfRef(sim, T->focusId) : nullptr;
        if (!T) return nullptr;
      }
      t->order = "Sacrifice";
      t->goalId = EntityRef(T);
      break;
    }
    case CommandType::Guard:
      return StartGuardTask(sim, u, c);
    case CommandType::AssistCommander:
      if (u->isPod) return StartPodTask(sim, u, c);  // DispatchTask 0x609ab8: CUnitPodAssist
      [[fallthrough]];
    case CommandType::BuildAssist:
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
    case CommandType::Capture: r = TickCapture(sim, L, u, t); break;
    case CommandType::Sacrifice: r = TickSacrifice(sim, L, u, t); break;
    case CommandType::Teleport: r = TickTeleport(sim, L, u, t); break;
    case CommandType::Script: r = TickScriptTask(sim, L, u, t); break;
    case CommandType::TransportLoadUnits:
    case CommandType::Dock:
    case CommandType::TransportReverseLoadUnits:
    case CommandType::TransportUnloadUnits:
    case CommandType::TransportUnloadSpecificUnits:
    case CommandType::Ferry: r = TickTransportTask(sim, u, t); break;
    case CommandType::Guard: r = t.gdata ? TickGuard(sim, L, u, t) : kTaskFailed; break;
    case CommandType::BuildAssist:
    case CommandType::AssistCommander:
      r = t.tdata ? TickTransportTask(sim, u, t) : TickAssistLegacy(sim, L, u, t);
      break;
    case CommandType::Attack:
    case CommandType::OverCharge:
    case CommandType::FormAttack: r = TickAttack(sim, L, u, t); break;
    case CommandType::Nuke:
    case CommandType::Tactical: r = TickFireAt(sim, u, t); break;
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
  // (whether the task's walking child, a CUnitMoveTask, is running: only its dtor 0x618700 aborts the move, below)
  const bool walking = t.moving;
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
  if (t.type == CommandType::Nuke || t.type == CommandType::Tactical) {
    EndFireAt(sim, u, t);
    return;
  }
  if (t.type == CommandType::Capture) {
    EndCapture(sim, L, u, t);
    return;
  }
  if (t.type == CommandType::Sacrifice) {
    EndSacrifice(sim, u, t);
    return;
  }
  if (t.type == CommandType::Teleport) {
    EndTeleport(sim, L, u, t);
    return;
  }
  if (t.type == CommandType::Attack || t.type == CommandType::FormAttack || t.type == CommandType::OverCharge) {
    EndAttack(sim, u, t);
    // ... and AbortMove (nav vt+0x10) whatever the navigator does: an idle one still rebuilds the brake spline the
    // steering holds (MakeIdle -> SetWaypoints(0, 0)), so a unit already braking brakes from its speed of now
    if (Alive(u)) {
      if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
      u->unitStates.Erase(US("Moving"));
    }
    return;
  }
  (void)success;  // build-like tasks end by their own state (the task dtors), not the caller's view
  if (t.type == CommandType::BuildMobile) {
    // CUnitMobileBuildTask dtor (0x5f6ac0): OnStopBuild(true) - the structure is left as it is -
    // then the builder's OnFailedToBuild when the task did not finish (also before it started).
    if (Alive(u)) SetArmAimTarget(sim, u, Vec3{});
    // 0x5f6b66..0x5f6b99: before OnStopBuild the structure the task made (its weak pointer +0xb8) loses NoReclaim
    // (state 40, the high dword's bit 8), finished or not. (Seen: Economy beat 2555, SACU 0x25 ordered to guard
    // from building 0x31 at 70 %: the half-built structure is reclaimable again in the original, not in ours.)
    if (Unit* w = FindUnitAny(sim, t.targetId)) w->unitStates.Erase(US("NoReclaim"));
    if (Alive(u)) {
      if (t.started) StopBuild(sim, L, u, t, true);
      if (!t.completed) sim.CallMethod(L, u, "OnFailedToBuild", 0);
    }
    // 0x5f6bd3..0x5f6bd6: then Unit::FreeOgridRect 0x6ab760 on the builder, whatever the state: the cell its walk to
    // the site reserved (state 0, ReserveOgridRect) is given back when the task ends before it got there. Seen
    // (Skirmish 812): Aeon engineer 0x600006 reserved (913, 88) beside a structure at 792, M28 gave it a Move at 801;
    // the original's engineers that came later took that cell (0x600008 at 802, so 0x600002 the next free one,
    // (914, 88), at 812: its first waypoint is then its goal and does not drive through); ours kept the cell
    // reserved and sent both elsewhere.
    GroundFreeRect(sim, u);
  } else if (t.type == CommandType::BuildFactory) {
    // CFactoryBuildTask dtor (0x5fa010): OnStopBuild(true) when done, else OnStopBuild(false)
    // (a cancelled product gets OnFailedToBeBuilt; the factory script destroys it). Before it, with no test of the
    // unit's life (0x5fa039..0x5fa06e): Busy off (unit+0x4a0 &= ~0x2000), Building off (&= ~0x20) and the work
    // progress (+0x2ac) 0. Busy is otherwise only the script's (SetBusy 0x6c6542): a factory killed, or cleared, during
    // the script's roll-off (Busy + BlockCommandQueue) is no longer Busy in that beat; BlockCommandQueue stays. Seen:
    // MP1 beat 4088, army 0's factory 0x27 self-destructed in its roll-off: the original's states that beat are NoCost
    // and BlockCommandQueue; ours kept Busy until the unit was gone.
    u->unitStates.Erase(US("Busy"));
    u->busy = false;
    u->unitStates.Erase(US("Building"));
    u->workProgress = 0;
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, t.completed);
  } else if (t.type == CommandType::Upgrade) {
    // CUnitUpgradeTask dtor (0x5f84c0): done -> OnStopBuild(true); otherwise the engine destroys the
    // upgrade unit itself (its ground released, the builder's taken back), then OnStopBuild(false)
    u->unitStates.Erase(US("Upgrading"));
    u->unitStates.Erase(US("NoCost"));  // (0x5f855a)
    // 0x5f856d: the work progress back to 0 BEFORE OnStopBuild (ours did it after, and not at all once FAF's
    // OnStopBuild had destroyed the old unit). Seen: Skirmish 2183, army 4's factory 0x400014 finishes its upgrade
    // 0x400023; M28's threads for both start in that beat's Lua: the original's old factory reads work progress 0, is
    // "ready" (IsFactoryReadyToBuild) and takes the beat's order decision, so the new one decides at 2184; ours read 1.
    u->workProgress = 0;
    if (t.completed) {
      // 0x5f8581: the old unit no longer counts as occupying ground (builder+0x560 = 0), so its OnDestroy leaves the
      // o-grid reservation alone: the new unit's skirt, reserved by its own constructor over the same cells, stays
      // (the bits are not counted). Seen: Skirmish 2197, army 6's engineer 0x60002c guarding engineer 0x60002d beside
      // the Aeon air factory 0x60001f (upgraded from 0x600016 at 1863): the original's anchor search rejects the
      // factory's skirt and takes (913.5, 87.5); ours had cleared the skirt when the old factory went and took
      // (913.5, 85.5).
      u->occupiesGround = false;
      // (0x5f859b: the new unit is no longer BeingUpgraded, before OnStopBuild)
      if (Unit* nu = FindUnit(sim, t.targetId)) nu->unitStates.Erase(US("BeingUpgraded"));
      if (t.started && Alive(u)) StopBuild(sim, L, u, t, true);
    } else {
      Unit* target = FindUnit(sim, t.targetId);
      if (target) {
        // 0x5f85d5..0x5f85f5: ReleaseOccupyGround of the upgrade, its Destroy, then the builder's ExecuteOccupyGround
        // 0x6a7770 whatever it held: the cells the upgrade's release cleared are the builder's again
        ReleaseStructure(sim, target);
        sim.QueueDestroy(target);
        // 0x5f85f2..0x5f85f5: the builder's ExecuteOccupyGround has no life test and leaves its +0x560 as it is. So a
        // builder on its way out whose +0x560 was cleared (TransferUnit, 0x7470a2) keeps its cells held for good: its
        // queue is cleared in ~Unit (0x6a6c66) before ReleaseOccupyGround (0x6a6de3), which then does nothing. Seen: MP3
        // 4036, army 3's units go to army 1; the T1 extractor 0x300008, upgrading into 0x300033, becomes 0x1000a2. The
        // original's cell (442, 69) stays held, and at 4450 army 1's tank 0x10003a cannot take the straight way to
        // (442, 68) past it (probe mp3_reach: its corridor test to (442, 68) fails, (443, 68) is taken); ours had freed
        // the cell with the upgrade's release.
        ExecuteOccupyGround(sim, u);
      }
      if (t.started && Alive(u)) StopBuild(sim, L, u, t, false);
    }
  } else if (t.type == CommandType::Repair || t.type == CommandType::Guard || t.type == CommandType::BuildAssist ||
             t.type == CommandType::AssistCommander) {
    // the repair task's dtor (0x5f8e20): ClearWork, Repairing off, workProgress 0, the arm reset, the
    // reserved cell (still moving), NoReclaim off, OnStopBuild(true); never a failure
    if (t.type == CommandType::Repair) {
      if (Alive(u) && u->HasLuaObject()) sim.CallMethod(L, u, "ClearWork", 0);
      // 0x5f8ee7..0x5f8ef9: the cell its walk reserved (state 1, the move not suppressed by +0x98) is given back
      // (Unit::FreeOgridRect 0x6ab760) with no test of the unit's life: a unit destroyed on its way to the repair (by
      // an army transfer, Sim::TransferUnit, or a kill) frees it too. Seen: MP3 4036, army 3's 0x30001e, on patrol,
      // walks to repair a structure and holds (489,181) since 3828 when army 3's units go to army 1; ours kept the
      // cell reserved for good, so at 5847 army 1's 0x100085, sent to repair 0x10004c, was
      // given (490,181) instead of (489,181), and its walk's last step at 6225 went another way.
      if (t.state == 1 && !t.noMove) GroundFreeRect(sim, u);
      if (Alive(u)) {
        u->unitStates.Erase(US("Repairing"));
        u->workProgress = 0;
        SetArmAimTarget(sim, u, Vec3{});
        if (t.moving) {
          t.moving = false;
          EndMoveChild(u);
        }
      }
      if (Unit* w = FindUnitAny(sim, t.workId)) w->unitStates.Erase(US("NoReclaim"));
    }
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, true);
  } else {
    // CFactoryBuildTask's dtor 0x5fa010: Busy and Building off, the work progress back to 0, THEN OnStopBuild.
    // (A factory stopped half way through a unit kept its last progress here: the work bar stayed under it.)
    if (t.type == CommandType::BuildFactory && Alive(u)) u->workProgress = 0;
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, success);
  }
  // CUnitReclaimTask's dtor 0x620280, first of all (0x6202cf..0x620316): a target that is a unit (the entity's
  // GetUnit, vt+0x10) loses BeingReclaimed (unit state 15: +0x4a0 &= ~0x8000), whoever else still reclaims it; the
  // flag is not counted, and only TaskTick's state 2 (0x61f940) sets it again, at that reclaimer's next step. These two
  // are the exe's only writes of the bit. Seen: Skirmish 2288 and 2298, army 4's 0x40001a reclaimed by one, then two,
  // then three engineers: the original shows it without BeingReclaimed for the one beat in which another reclaimer's
  // task ended; ours kept it on until the unit died (0x300032 to the end of the run).
  // A target the task still holds through a recon blip (TickPropReclaim: until the task finds it in range) is no unit
  // for that test: the blip keeps nothing. Seen (probe mp1_7040): MP1 7040, army 4's engineer 0x40004c, walking since
  // 7031 to reclaim army 1's frigate 0x10001f for a player's Reclaim on its blip 0x3010001f, is killed by a shell in
  // the entity pass; its task's dtor runs with the blip for its target (the other reclaimers' tasks, in range, hold
  // the frigate) and the frigate stays BeingReclaimed, as the four reclaim steps of that beat left it. Ours cleared it.
  if (t.type == CommandType::Reclaim && t.goalId && !t.targetViaBlip) {
    Entity* e = sim.FindEntity(t.goalId);
    if (e && e->kind == Entity::Kind::Unit) static_cast<Unit*>(e)->unitStates.Erase(US("BeingReclaimed"));
  }
  if (t.reclaimStarted && Alive(u)) {
    // the target as the task's weak pointer holds it (0x620110 -> 0x605600 -> 0x6057d0: the entity's Lua object while
    // the pointer is set, else nil; no test of a Destroy), so a target destroyed this beat, not yet deleted, is passed.
    // Seen: Skirmish 2300, army 3's engineers 0x300019, 0x300022 and 0x300020 reclaim army 4's 0x40001a; 0x300022
    // finishes it, 0x300020's task then ends, and M28's OnStopReclaim (ForkThread OnReclaimFinished) clears every
    // engineer in the target's reftUnitsReclaimingUs in the same beat: the original's 0x300019 and 0x300022 are idle
    // from 2300. Ours passed nil, nobody was cleared, and 0x300022 reclaimed the wreck until 2309.
    Entity* e = sim.FindEntity(t.goalId);
    PushObject(L, e);
    sim.CallMethod(L, u, "OnStopReclaim", 1);
    t.reclaimStarted = false;
  }
  // CUnitMobileBuildTask's dtor (0x5f6b16) and CFactoryBuildTask's (0x5fa05e) take Building off (unit+0x4a0 &= ~0x20)
  // with no test of the unit's life: a builder killed at work (Unit::Kill empties its queue) is no longer Building.
  // (Seen: Skirmish 2810, army 0's engineer 0x11 killed while building: the original's states lose Building.)
  if (t.type == CommandType::BuildMobile || t.type == CommandType::BuildFactory) u->unitStates.Erase(US("Building"));
  // The same for CUnitReclaimTask's dtor (Reclaiming off, 0x6203c6, and the work progress +0x2ac = 0, 0x6203d9) and
  // CUnitRepairTask's (Repairing off, 0x5f8e89, work progress 0, 0x5f8e9c): a unit killed while it reclaims or
  // repairs loses the state when Unit::KillCleanup empties its queue the beat after. (Seen: MP3 2536, army 1's
  // engineer 0x10000e killed while reclaiming; 2550, army 0's engineers 0xa and 0xc killed while repairing: the
  // original's dying units are without the state, ours kept it until they were gone.)
  if (!Alive(u)) {
    u->unitStates.Erase(US("Reclaiming"));
    u->unitStates.Erase(US("Repairing"));
    u->workProgress = 0;
    if (walking) FreeWalkRect(u);  // (the walking child's dtor, 0x618891, has no test of the unit's life)
  }
  if (Alive(u)) {
    u->unitStates.Erase(US("Reclaiming"));
    if (t.type == CommandType::Reclaim && u->focusId == t.goalId) u->focusId = 0;
    if (t.type == CommandType::Reclaim) {
      // CUnitReclaimTask's dtor 0x620280: the arm let go (SetAimTarget(0, 0, 0)); a walk still under way gives its
      // reserved ground back
      SetArmAimTarget(sim, u, Vec3{});  // (through the builder unit+0x554, 0x62031f, as the tick's aim)
      if (t.moving) {
        t.moving = false;
        GroundFreeRect(sim, u);
      }
    }
    u->unitStates.Erase(US("Building"));
    u->unitStates.Erase(US("Repairing"));
    if (!t.child && (t.type == CommandType::BuildAssist || t.type == CommandType::AssistCommander)) {
      u->unitStates.Erase(US("Guarding"));  // (the legacy assist tasks; the guard's own dtor does this for it)
      u->unitStates.Erase(US("AssistingCommander"));
    }
    u->workProgress = 0;
    // Only a walking child's dtor aborts the move (0x618884); with none running the navigator keeps what it has. (Seen:
    // Economy 3027-3029, the commander building in a pocket between storages; engineer 0x3's next site covers it, and
    // SIM_TryToBuild sends it a goal at 3027 while it still builds. Its own build ends at 3029 and the original walks
    // off to that goal from that beat; ours aborted the move with the finished task.)
    if (walking) StopMovingIfTask(u, t);
  }
  if (!t.child && (t.type == CommandType::BuildAssist || t.type == CommandType::AssistCommander) && u->guardedId)
    SetGuardedUnit(sim, u, nullptr);
}

bool TaskCanMove(const Unit* u) { return CanMove(u); }
void TaskMoveToward(Sim& sim, Unit* u, const Vec3& goal) {
  MoveToward(sim, u, goal);
  u->walkPrepared = false;
  u->walkSeq = ++sim.walkSeqNext;
}

// CUnitMoveTask::OnEvent 0x618c40 (read) for a task's walk: the move task links the queue's head command (ctor 0x6180e0:
// task+0x88 from unit+0x4b4's first command, its listener +0x54 in cmd+0x34), so a SetTarget event of that command
// (0x6e8820 -> 0x6e94a0) reaches it. With +0x94 clear it hands its navigator the command's position for the unit
// (CUnitCommand::GetPosition 0x6e8a30: the target's point with the centre flag clear, 0x6e8a6a; the unit's footprint
// cell there; SNavGoal 0x5a2cb0, SetGoal vt+8; no speed-through change); with +0x95 (the ctor's rally override) that
// cell's centre goes through PrepareMove first (spacing 0, 0x618d20) and the o-grid reservation moves to the cell found
// (+0x92: FreeOgridRect 0x6ab760, ReserveOgridRect 0x6ab6f0). The listener wakes nobody (no thread re-link). (Land
// navigators only: an air walk's SetGoal is not modelled here.)
void TaskWalkCommandEvent(Sim& sim, UnitCommand& c, Unit* h) {
  if (!h->motion.bp || !h->motion.bp->mobile() || !h->motion.nav) return;
  Vec3 p = c.pos;
  if (!FormationGoal(sim, c, h, &p)) p = CommandPosGun(sim, c);  // (the target's point)
  const NamedFootprint& fp = Fp(h);
  int cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
  if (h->walkPrepared) {
    Vec3 q{cx + fp.sizeX * 0.5f, p.y, cz + fp.sizeZ * 0.5f};
    const float none[4] = {0, 0, 0, 0};
    GroundPrepareMove(sim, h, &q, none, 0);
    cx = static_cast<int>(std::nearbyint(q.x - fp.sizeX * 0.5f));
    cz = static_cast<int>(std::nearbyint(q.z - fp.sizeZ * 0.5f));
  }
  const Vec3 goal{cx + fp.sizeX * 0.5f, p.y, cz + fp.sizeZ * 0.5f};
  h->motion.arrived = h->motion.failed = false;
  MotionSetGoal(sim, h, std::vector<Vec3>{goal}, h->motion.nav->speedThroughGoal, sim.tick());
  if (h->walkPrepared) {
    const int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
    GroundReserveRect(sim, h, r);
  }
}

// CUnitReclaimTask's command listener 0x620160 (read). The task listens to the queue's current command (its ctor
// 0x61eb60, 0x61ec82..0x61ecdb: +0x40 = the queue's first command, its listener linked into cmd+0x34). When that
// command's target changes: OnStopReclaim if started (0x62016e); the target (+0x44) := the command's target entity
// (cmd+0x120), the place (+0x50) := its GetTargetPosGun (cmd+0x11c); the builder's focus := that target
// (OnAssignedFocusEntity when there is one, 0x6201fe); +0x68e = 1; workProgress = 0; +0x4c := no entity; state 0;
// the thread woken (0x620235..0x62026d). Seen (MP1 1152): the player drags army 1's factory rally point (the rally
// AggressiveMove its fresh engineer 0x10000a is still on) while the engineer reclaims a rock for the patrol; the
// original's reclaim ends there (Reclaiming off at 1152, no more mass from 1154, the rock still there for the
// player's own Reclaim at 1161); ours went on.
void ReclaimTaskCommandEvent(Sim& sim, Unit* u, BuildTask& c, const UnitCommand& cmd) {
  lua_State* L = sim.L();
  if (c.reclaimStarted) {
    c.reclaimStarted = false;
    PushObject(L, c.goalId ? sim.FindEntity(c.goalId) : nullptr);
    sim.CallMethod(L, u, "OnStopReclaim", 1);
    if (!Alive(u)) return;
  }
  Entity* e = cmd.targetId ? sim.FindEntity(cmd.targetId) : nullptr;
  c.goalId = e ? EntityRef(e) : 0;
  c.targetViaBlip = e && cmd.targetViaBlip;  // (the command's target entity, cmd+0x120: the blip of a blip order)
  c.site = e ? e->position : cmd.pos;
  c.reclaimAtPoint = e == nullptr;
  u->focusId = c.goalId;
  if (e) sim.CallMethod(L, u, "OnAssignedFocusEntity", 0);
  u->workProgress = 0;
  c.state = 0;
}

// CUnitMobileBuildTask::OnEvent 0x5f80c0 (read; build_stop.md 2), the task's listener on its command (linked by the
// ctor; the listener is task+0x34). On any event of the command (it does not read the event code): when the task holds
// a structure (+0xb8), that loses NoReclaim (+0x4a4 bit 8), the builder's work progress goes to 0, the helper's
// OnStopBuild(true) runs (0x5f5a20) and the structure and the rebuild wreck (+0xc0) are let go (0x5f80cf..0x5f8170).
// Then the site is taken again from the command (GetTargetPosGun 0x5e2a90, snapped by the footprint 0x50b0f0, the
// footprint and skirt rects +0xc8 / +0xd8), state 0 (0x5f8321) and the thread woken: frame count 0 and, when it is
// suspended (a walk's move task), re-linked at the run list's end (0x5f8324..0x5f835d). A walk under way is then ended
// by the wake (TickMobileBuild state 0). Retries count on (+tries is not reset).
void MobileBuildCommandEvent(Sim& sim, Unit* u, BuildTask& t, const UnitCommand& cmd) {
  lua_State* L = sim.L();
  if (Unit* w = FindUnitAny(sim, t.targetId)) {
    w->unitStates.Erase(US("NoReclaim"));
    u->workProgress = 0;
    if (t.started && Alive(u)) StopBuild(sim, L, u, t, true);
    t.started = false;
    t.targetId = 0;
    t.rebuildWreck = 0;
    if (!Alive(u)) return;
  }
  t.site = cmd.targetId ? CommandPosGun(sim, cmd) : cmd.pos;
  if (t.bp) t.site = SnapStructurePosition(sim, *t.bp, t.site);  // 0x5f8222
  t.state = 0;
  t.waitUntil = 0;
}
void TaskStopMoving(Unit* u) { StopMoving(u); }

// The motion's part of a build-like task's teardown. None of these tasks' own dtors touches the navigator or the
// motion (read: CUnitMobileBuildTask 0x5f6ac0, CUnitRepairTask 0x5f8e20, CUnitReclaimTask 0x620280,
// CUnitCaptureTask 0x6050f0, CUnitSacrificeTask 0x5fae40, CUnitTeleportTask 0x60aec0, CUnitUpgradeTask 0x5f84c0,
// CFactoryBuildTask 0x5fa010: the arm's SetAimTarget(0, 0, 0), unit states, FreeOgridRect, callbacks, nothing at
// unit+0x54c). Their walks are CUnitMoveTask children (NewMoveTask 0x6190a0 from each TaskTick), deleted first
// (CTask::TaskInterruptSubtasks 0x408d70, top down), and it is that child's dtor 0x618700 that aborts the move
// (nav vt+0x10 at 0x618884), by its table 0x618998: not when the queue is cleared for an order the table keeps
// (u->clearKeepsMove). Seen (Economy 2277): a support commander (uel0301_ras, unit 0xb) whose Reclaim walk is under
// way gets a player's Move with clear, its queue holding only the Reclaim; the original keeps accelerating along the
// old turn until the new Move task sets the goal in 2278 (watch log: wn 2278 status 1, goal (708.5, 339.5)), ours
// braked in 2278-2279.
void StopMovingIfTask(Unit* u, const BuildTask& t) {
  if (t.type == CommandType::BuildFactory || t.type == CommandType::Upgrade) return;
  EndMoveChild(u);
}

// Whether the unit's command thread sleeps on a walking child (a CUnitMoveTask pushed by the head task, or by the
// guard's child, waits for the navigator's event: the thread is suspended until then, beat_order.md 2.6).
bool GuardSteersItself(const Unit* u) {
  const BuildTask* t = u->task;
  return t && t->gdata && !t->gdata->child && !t->gdata->childMove;
}

// A guard's attack child (CUnitGuardTask::TaskTick 0x6141a0 state 3 -> MakeAttackTaskOn) is a CUnitAttackTargetTask
// pushed on the dispatcher's own thread, above the guard: its in-range return -2 (0x5f3a63) suspends that thread
// as the head attack's does, and its attacker listener (0x5f3ee0, the tail 0x5f3f9f..0x5f3fd5) and the queue's
// events (OnEvent 0x599030) re-link it when suspended, whichever task suspended it.
const BuildTask* ThreadAttackTask(const Unit* u) {
  const BuildTask* t = u->task;
  if (t && t->gdata && t->gdata->child) t = t->gdata->child;
  else if (t && t->pdata && PatrolChildTask(*t)) t = PatrolChildTask(*t);  // (a patrol's attack child: the same thread)
  return t && t->adata ? t : nullptr;
}

bool TaskThreadAsleepOnMove(const Unit* u, const BuildTask* from) {
  if (!u->motion.hasGoal || u->motion.arrived || u->motion.failed) return false;  // (the event came: awake)
  for (const BuildTask* t = from ? from : u->task; t;) {
    if (t->moving) return true;
    // a transport task's landing or walking move (CUnitTransportLoad state 1's NewMoveTask, the call's walk ...):
    // the thread sleeps in that move task as well, and a clear puts it at the run list's end. Seen: MP2 4338,
    // army 1's transport 0x100084, descending to its pickup point, and its five cargo units get the load order
    // again (clear, the transport last in the message): the original runs the cargo's new call tasks before the
    // transport's new load task at 4339 (they find it not yet loading and wait 10 beats: WaitingForTransport at
    // 4350); ours had not re-linked the transport, ran its new task first, and the cargo walked from 4341.
    if (t->tdata) return TransportTaskAsleepOnMove(t);
    if (t->sub) {
      t = t->sub;
      continue;
    }
    if (t->gdata) {
      if (t->gdata->childMove) return true;
      t = t->gdata->child;
      continue;
    }
    break;
  }
  return false;
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
      AssignEngineCommandId(sim, c);
      c->type = type;
      c->pos = pos;
      c->hasPos = true;
      c->targetId = targetId;
      c->factoryIssued = true;
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
  // A factory building for the factory it assists hands on THAT factory's rally queue instead of its own: the list
  // copied from f is assigned the other one's (0x5fa3ea / 0x5fa5fa: std::vector<WeakPtr<CUnitCommand>>::operator=
  // 0x5fb910), not appended to. Seen: MP3 2435, army 2's land factory assisting another made a tank whose queue
  // held the roll-off and the assisted factory's four rally moves; ours added the assisting factory's own rally.
  if (inheritFrom && Alive(inheritFrom) && inheritFrom->bpData && inheritFrom->bpData->hasBuilder)
    L.assign(inheritFrom->factoryCommands.begin(), inheritFrom->factoryCommands.end());
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
    if (!BpInCategory(sim, X->blueprint, "TRANSPORTATION") || !X->unitStates.Has(US("Ferrying"))) continue;
    if (!chosen || !X->unitStates.Has(US("Moving"))) chosen = X;
    if (!chosen->unitStates.Has(US("Moving"))) break;
  }
  if (chosen) {
    // The order's issue data holds a CAiTarget of R made here (CAiTarget::UpdateTarget 0x5d55b0 from 0x5fab9c: one
    // target-point draw when R has target bones), whatever becomes of the order; the command it makes decodes it
    // again (IssueTransportLoadOne). Seen (probe mp1_7019b): MP1 7019, army 7's T3 land factory 0x700077 (xsb0301)
    // finishes the tank 0x700007 while its guards hold the ferrying transport 0x70002f: the original's beat opens
    // with two Unit::PickTargetPoint draws on 0x700077 (UpdateTarget callers 0x5faba1 and 0x5e2775); ours made none.
    TargetPointDraw(sim, R);
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
// 0x6a7600 -> GetSkirtRect 0x51ec50: the skirt (the footprint where SkirtSize is 0)
IRect UnitRect(const Unit* u) {
  if (u->bpData) {
    FRect r = SkirtRect(*u->blueprint, *u->bpData, u->position.x, u->position.z);
    return {r.x0, r.z0, r.x1, r.z1};
  }
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
// Unit::CollectAllOverlapping 0x62d460: the units the entity grid finds within 20 of u (COGrid::ForAllEntitiesIterator
// 0x721fb0, units 0x100, radius 0xdff25c) that are not dead (IUnit vt+0x28, unit+0xa1), immobile (vt+0x30), of u's
// army, not u, on u's layer (+0x120) and touching it (OverlapsWith 0x62d2b0). Nothing else is asked: a construction
// is taken (the scripts' OnAdjacentTo / OnNotAdjacentTo test IsBeingBuilt themselves), and so is a unit whose Destroy
// is queued: it stays in the entity grid until the destroy queue, and only Unit::OnDestroy marks it dead. Seen:
// X1CA_002 creates its wrecked Loyalist base (walls among it) and Destroys it in OnPopulate, then the M1_Walls beside
// it; leaving the destroy-queued walls out gave the new walls no AdjacentUnits, and the old walls' end called
// OnNotAdjacentTo on them (StructureUnit.lua:824 indexes nil). Here the grid's box around u stands for the sphere
// (OverlapsWith decides), and the units are taken in id order.
std::vector<Unit*> Overlapping(Sim& sim, Unit* u) {
  std::vector<Unit*> out;
  const float R = 20.0f;
  std::vector<Entity*> cand;
  sim.entityGrid().GatherBox(u->position.x - R, u->position.z - R, u->position.x + R, u->position.z + R, 1, &cand);
  for (Entity* e : cand) {
    if (e->kind != Entity::Kind::Unit || e == u || e->dead) continue;
    Unit* o = static_cast<Unit*>(e);
    if (!o->bpData || !o->bpData->structure) continue;
    if (o->army != u->army || o->layer != u->layer) continue;
    if (OverlapsWith(u, o)) out.push_back(o);
  }
  std::sort(out.begin(), out.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  return out;
}
}  // namespace

// The Unit ctor for a complete immobile unit (0x6a646d) and Unit::Materialize at completion (0x6aa272): for each unit
// it touches, u:OnAdjacentTo(o, u), then o:OnAdjacentTo(u, u) (the third argument is the unit that caused it).
void AdjacencyGained(Sim& sim, lua_State* L, Unit* u) {
  for (Unit* o : Overlapping(sim, u)) {
    PushObject(L, o);
    PushObject(L, u);
    sim.CallMethod(L, u, "OnAdjacentTo", 2);
    PushObject(L, u);
    PushObject(L, u);
    sim.CallMethod(L, o, "OnAdjacentTo", 2);
  }
}

// Unit::Kill (the units collected at 0x6a81fe, the calls at 0x6a8369) and Unit::OnDestroy for a unit not killed
// (0x6a894f, 0x6a8982), when u is immobile and finished: u:OnNotAdjacentTo(o), then o:OnNotAdjacentTo(u), then o
// takes its o-grid reservation again (Unit::ReserveOgridRect 0x6ab6f0 with its own rect, 0x6a83b9 / 0x6a89d2: u's end
// may have freed cells they share).
void AdjacencyLost(Sim& sim, lua_State* L, Unit* u) {
  if (!u->bpData || !u->bpData->structure || u->beingBuilt) return;
  for (Unit* o : Overlapping(sim, u)) {
    PushObject(L, o);
    sim.CallMethod(L, u, "OnNotAdjacentTo", 1);
    PushObject(L, u);
    sim.CallMethod(L, o, "OnNotAdjacentTo", 1);
    const int r[4] = {o->ogridRect[0], o->ogridRect[1], o->ogridRect[2], o->ogridRect[3]};
    GroundReserveRect(sim, o, r);
  }
}

}  // namespace moho

// ---- bindings ------------------------------------------------------------------------------------

namespace moho {
namespace {

Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

const BlueprintInfo* BlueprintArg(lua_State* L, int idx) {
  // a string through the name cache (FindOfName: Find of the same text); a number still as lua_tostring makes it
  if (lua_type(L, idx) == LUA_TSTRING) return S(L)->blueprints().FindOfName(lua_tostring(L, idx), lua_strlen(L, idx));
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
  AiBrain* b = CheckObject<AiBrain>(L, 1);
  const BlueprintInfo* bp = BlueprintArg(L, 2);
  Vec3 p;
  if (!bp || !PosArg(L, 3, &p)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  // (the brain's pending plans of BuildStructure, brain+0x98, also keep the skirt off: sim/ai_build.cpp)
  lua_pushboolean(L, CanBuildStructureAt(*S(L), *bp, p.x, p.z) && !AiBuildPlansBlock(*S(L), b, *bp, p.x, p.z));
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
  // (appended only: build.cpp's deposit index, DepositIndex, is made for the list as it grows; a remover or an edit in
  // place must drop that index, or keep the count / last-entry check it makes again on)
  S(L)->deposits.push_back(d);
  return 0;
}

void CategoryBits(lua_State* L, int idx, std::vector<uint64_t>& out) {
  out.assign(static_cast<size_t>(CategoryWordCount(L)), 0);
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
  u->buildAllowed.assign(static_cast<size_t>(CategoryWordCount(L)), ~uint64_t(0));
  return 0;
}

int l_GetFocusUnit(lua_State* L) {  // 0x6c46f0
  Unit* u = U(L);
  // the weak focus +0x4d0 as a unit (vt+0x10), no IsDead test (0x6c4778..0x6c47b2): a killed focus is returned until
  // it is freed (Engineers 1713: UpdateConsumptionValues of two engineers repairing a dying engineer requests the
  // repair cost in the original)
  Unit* f = FindUnitAny(*S(L), u->focusId);
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
// NotifyUpgrade(from, to) 0x6cce70 (FAF's StructureUnit.OnStopBuild, the upgrade finished, before from:Destroy()):
// the upgrade takes over the old unit's orders, rally queue, platoon place, repeat switch, health fraction, guard
// links and leash point, in this order.
int l_NotifyUpgrade(lua_State* L) {
  Sim& sim = *S(L);
  Unit* from = ToObject<Unit>(L, 1);
  Unit* to = ToObject<Unit>(L, 2);
  if (!from || !to) return 0;
  // 0x6ccf94..0x6cd05d: every command of the old unit's queue (a copy of it, 0x5db610) is added to the new unit's
  // (CUnitCommandQueue::AddCommandToQueue 0x6edd80: the same command object, appended, queue event 0), except an
  // Upgrade (cmd+0x98 == 0x1b) whose blueprint (cmd+0x60) is the new unit's own (the order just carried out). Seen:
  // Economy 3079, four T1 extractors ordered Upgrade ueb1202 then Upgrade ueb1302 at 2740; the first T2 to finish
  // (0x3b) starts its T3 upgrade in the beat the old one goes; ours' T2 stood idle.
  // NOT MODELLED: AddCommandToQueue's patrol rule (a Patrol / FormPatrol added to a queue of more than one command
  // whose head is a patrol goes before the queued command with the smallest cmd+0x13c, when that is not the head).
  {
    const std::vector<std::shared_ptr<UnitCommand>> mine(from->commands.begin(), from->commands.end());
    for (const auto& c : mine) {
      if (!c) continue;
      if (c->type == CommandType::Upgrade && sim.blueprints().Find(c->blueprintId) == to->blueprint) continue;
      if (!c->units.insert(to).second) continue;
      to->commands.push_back(c);
      AirSpeedThroughEvent(sim, to);  // queue event 0 (0x6edd21)
    }
  }
  // 0x6cd060..0x6cd0c5: both have a builder (unit+0x554): the new one's rally queue is cleared (vf+0x38) and the old
  // one's commands are appended to it (vf+0x28 with -1)
  if (from->bpData && from->bpData->hasBuilder && to->bpData && to->bpData->hasBuilder) {
    ClearFactoryCommandQueue(to);
    const std::vector<std::shared_ptr<UnitCommand>> rally(from->factoryCommands.begin(), from->factoryCommands.end());
    for (const auto& c : rally)
      if (c) AddFactoryCommand(to, c);
  }
  PlatoonsNotifyUpgrade(from, to);  // 0x6cd0c7: the upgrade takes the old unit's place in its platoon's squad
  // 0x6cd11e..0x6cd156: the repeat switch (unit+0x2a2) is copied; the new unit's script hears of a change
  if (from->repeatQueue != to->repeatQueue && to->HasLuaObject())
    sim.CallMethod(L, to, from->repeatQueue ? "OnStartRepeatQueue" : "OnStopRepeatQueue", 0);
  to->repeatQueue = from->repeatQueue;
  // 0x6cd163..0x6cd1a3: the new unit's health = its maximum x the old unit's health fraction (Entity::SetHealth
  // 0x679940 when that differs)
  if (from->maxHealth > 0.0f) {
    const float h = to->maxHealth * (from->health / from->maxHealth);
    if (h != to->health) EntityAdjustHealth(L, to, nullptr, h - to->health);
  }
  SetGuardedUnit(sim, to, FindUnit(sim, from->guardedId));  // 0x6cd1a8 (unit+0x4e0)
  to->leashPos = from->leashPos;                             // 0x6cd1c0 (unit+0x4e8..+0x4f0)
  for (Unit* g : Guards(sim, from)) SetGuardedUnit(sim, g, to);  // 0x6cd22e (the guards, unit+0x500)
  return 0;
}
int l_SetBusy(lua_State* L) {
  Unit* u = U(L);
  u->busy = lua_toboolean(L, 2) != 0;
  if (u->busy) u->unitStates.Insert(US("Busy"));
  else u->unitStates.Erase(US("Busy"));
  return 0;
}
// Unit:SetUnSelectable(flag) 0x6c6a50: sets or clears unit state UnSelectable (bit 33), nothing else
int l_SetUnSelectable(lua_State* L) {
  Unit* u = U(L);
  if (lua_toboolean(L, 2)) u->unitStates.Insert(US("UnSelectable"));
  else u->unitStates.Erase(US("UnSelectable"));
  return 0;
}
int l_SetBlockCommandQueue(lua_State* L) {
  Unit* u = U(L);
  u->blockCommandQueue = lua_toboolean(L, 2) != 0;
  if (u->blockCommandQueue) u->unitStates.Insert(US("BlockCommandQueue"));
  else u->unitStates.Erase(US("BlockCommandQueue"));
  return 0;
}

// ---- attachment: a factory holds what it builds at its build bone ----
int BoneIndex(lua_State* L, Entity* e, int arg) {
  if (lua_isnumber(L, arg)) return static_cast<int>(lua_tonumber(L, arg));
  if (lua_isstring(L, arg) && e->skeleton) return e->skeleton->Find(lua_tostring(L, arg));
  return -1;
}
// A plain entity (no unit) attached to another entity's bone: it takes the bone's transform now and whenever
// something attached to it asks (Sim::Tick). FAF hangs a mobile factory's external factory on such an anchor.
// It does not jump there: until the entity stage at the end of the tick its transform is zero (seen in the
// original: the units FAF hangs on a fresh anchor entity sit at (0, 0, 0) for their first tick, and from then on
// one tick behind the bone).
bool AttachPlainEntity(Sim& sim, Entity* e, Entity* parent, int bone) {
  if (!e || e->kind == Entity::Kind::Unit) return false;
  e->attachParent = EntityRef(parent);
  e->attachBone = bone;
  e->attachOffset = {};
  e->position = {};
  e->orientation = Quat{};
  sim.attachedEntities.push_back(EntityRef(e));
  return true;
}
// Entity:SetParentOffset(vector) 0x68c330: an attached entity's place relative to the bone (a shield bubble hangs
// ShieldVerticalOffset below its unit)
int l_SetParentOffset(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  // (the test is the entity's attach parent, +0x18c: a unit hung on a bone by AttachBoneTo / AttachTo has one too,
  // its parentId here; its offset goes into AttachedTransform, sim/transport.cpp)
  const bool unitAttached = e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->parentId;
  if (!e->attachParent && !unitAttached) return luaL_error(L, "Entity:SetParentOffset: entity is not attached to anything");
  luaL_checktype(L, 2, LUA_TTABLE);
  float v[3];
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, 2, i + 1);
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  e->attachOffset = {v[0], v[1], v[2]};
  return 0;
}
// Unit state Attached (14) belongs to the unit's motion: a unit without one (a structure, a dummy unit) never
// shows it (the anchor and external factory units of a Fatboy are attached and not Attached)
void SetAttachedState(Unit* u) {
  if (u->motion.bp && u->motion.bp->mobile()) u->unitStates.Insert(US("Attached"));
}
// Unit::AttachTo 0x6ab410 (the attach of AttachBoneTo / AttachTo from Lua): after Entity::AttachTo, a mobile unit's
// motion hears of it (CUnitMotion::NotifyAttached 0x6b94a0, 0x6ab43a: motion state Attached, horizontal event
// Stopped, vertical event Bottom, each with its script callback when it changes), and only then does the unit take
// state Attached (0x6ab445). For an aircraft that is AirNotifyAttached; so one its factory lets go is at the Bottom
// and climbs away MovingUp (CalcAirMotion: MovingUp while the vertical event is Bottom), its event Up the beat after.
// Seen: Skirmish 1349-1350, Chloe's Aeon transport 0x600018: MovingUp and vertical event 1, then 2 in the original;
// ours kept the event Top from the unit's creation (0, no state).
void NotifyAttached(Sim& sim, Unit* u) {
  if (u->motion.air) AirNotifyAttached(sim, u);
}
// entity:AttachBoneTo(selfBone, parent, parentBone)
int l_AttachBoneTo(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  Entity* parent = ToObject<Entity>(L, 3);
  if (!u && parent && AttachPlainEntity(*S(L), ToObject<Entity>(L, 1), parent, BoneIndex(L, parent, 4))) return 0;
  if (!u || !parent) return 0;
  if (u->parentId != EntityRef(parent)) u->attachSeq = S(L)->NextAttachSeq();  // (the views' child order)
  u->parentId = EntityRef(parent);
  u->attachOffset = {};  // (a new attach info: no relative offset until SetParentOffset)
  u->ownBone = BoneIndex(L, u, 2);
  u->parentBone = BoneIndex(L, parent, 4);
  NotifyAttached(*S(L), u);
  SetAttachedState(u);
  if (u->motion.hasGoal) MotionStop(u);
  // Entity::AttachTo 0x679550 only stores the link (ApplyAttachInfo 0x679680): the unit's transform follows its bone in
  // the entity stage as a pending one, and its collision primitive stays at the transform the beat began with (for a
  // unit made in this beat: where its constructor put it) until AdvanceCoords (step 13): the shape is held at the old
  // place for the rest of the beat. Seen: MP1 10601, army 6's factory makes the
  // engineer 0x60002d and attaches it in the command stage; army 7's 0x700007's shell lands beside it in the entity
  // stage (DamageArea r 1.2 at (983.02, 19.22, 528.29)): the original's engineer takes nothing (hp 1.0), ours, its box
  // already on the bone at (981.50, 18.80, 528.90), took 625 and died.
  S(L)->HoldShape(u);
  // The unit is not moved here at all (Entity::AttachTo 0x679550 stores the link only): its next turn in the entity
  // stage puts it on the bone (Entity::TaskTick 0x679c40, CalculateAttachedTransform 0x67a020), behind the parent's when
  // the parent has not had its turn. A unit that has had its turn in this beat so ends it where that turn put it, and
  // one let go again before its turn (DetachFrom) never stands on the bone. Seen: MP1 13120, army 6's crashing scout
  // 0x6000ee hung on FAF's ShieldCollider in the collider's OnImpact (the projectile pass, after the plane's step):
  // the original's plane ends the beat where its own crash step put it (854.3552, 23.7286, 739.1945) and is on the
  // collider from 13121. MP1 13492, army 2's naval factory 0x20004e makes the engineer 0x20008a and hangs it on its
  // build bone (FactoryUnit.lua:448, AttachBoneTo(-2, ...)), and is killed in the same beat; its OnKilled ->
  // KillUnitBeingBuilt lets the engineer go (DetachFrom(true)) before its turn: the original's engineer stands where it
  // was made (350.0, 17.5, 278.5) with the factory's quaternion; ours stood on the bone, 3.1 away and turned round.
  S(L)->MarkUnitsMoved();
  return 0;
}
int l_AttachTo(lua_State* L) {  // entity:AttachTo(parent, bone)
  Unit* u = ToObject<Unit>(L, 1);
  Entity* parent = ToObject<Entity>(L, 2);
  if (!u && parent && AttachPlainEntity(*S(L), ToObject<Entity>(L, 1), parent, BoneIndex(L, parent, 3))) return 0;
  if (!u || !parent) return 0;
  if (u->parentId != EntityRef(parent)) u->attachSeq = S(L)->NextAttachSeq();  // (the views' child order)
  u->parentId = EntityRef(parent);
  u->attachOffset = {};  // (a new attach info: no relative offset until SetParentOffset)
  // Entity:AttachTo 0x68be10 is AttachBoneTo with the entity's own bone 0 (0x68bf14: push 0), not -1: the unit is
  // placed so that ITS bone 0 lies on the parent's bone. Seen: an aircraft a carrier builds while it sails
  // (AircraftCarrier's BuildingState: unitBuilding:AttachTo(self, bone)) sits 0.47 below and 0.06 ahead of the
  // carrier's attach bone, which is where its model's root bone is.
  u->ownBone = 0;
  u->parentBone = BoneIndex(L, parent, 3);
  NotifyAttached(*S(L), u);
  SetAttachedState(u);
  if (u->motion.hasGoal) MotionStop(u);
  S(L)->HoldShape(u);  // (see l_AttachBoneTo: the link only, the unit's turn places it)
  S(L)->MarkUnitsMoved();
  return 0;
}
void Detach(Sim& sim, Unit* u) {
  if (!u->parentId) return;
  // CUnitMotion::NotifyDetached 0x6b9570 (EAX = the parent): the motion's target becomes the unit's position less
  // the PARENT's local Z axis (from its quaternion +0x9c..+0xa8: Z = (2(wy + zx), 2(zy - wx), 1 - 2(yy + xx)), in
  // that order of operations), no direction, layer 0 (CUnitMotion::SetTarget 0x6b85e0 at 0x6b9654). For an aircraft
  // that is its air target: the one beat until its first order's goal comes it climbs toward a point one unit in
  // front of the factory's back (MovingUp). Seen: Skirmish 1349, Chloe's Aeon transport 0x600018 let go by its air
  // factory (FactoryUnit.lua DetachFrom(true)): the original's target (909.5339, 27.4956, 80.5623) and MovingUp,
  // ours kept the cell it was made on, (909.5, 26.7148, 81.5); its Move's goal came at 1350 in both.
  if (const Entity* P = sim.FindEntity(u->parentId)) {
    const Quat& q = P->orientation;
    const float zx = (q.w * q.y + q.z * q.x) * 2.0f;
    const float zy = (q.z * q.y - q.w * q.x) * 2.0f;
    const float zz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
    const Vec3 t{u->position.x - zx, u->position.y - zy, u->position.z - zz};
    if (u->motion.air) AirSetTargetNow(sim, u, t, 0);
    else u->motion.motionTarget = t;  // (the same SetTarget for a land, hover or naval unit's motion)
  }
  u->parentId = 0;
  u->unitStates.Erase(US("Attached"));
  // NotifyDetached's motion+0x90 asks for a snap only where the motion step reads it: CalcMoveLand (0x6c31b7) and
  // CalcMoveHover. CalcMoveWater 0x6c3480 (any other layer, no hover) sets the unit on the water only when it moved,
  // dives, or still pitches (0x6c34b0 / 0x6c3523): a submarine let go by its naval factory stays at the build bone's
  // height until it moves (ProcessSurfaceCollisionFromLastMove, which reads +0x90 too, skips the Sub layer).
  // Seen: Skirmish 1986, army 5's submarine 0x500022 finished at y 17.508: the original kept that y to 1988 and was
  // at its depth (15.0) when it moved at 1989; ours set it at its depth at once (our creation-time snap request,
  // still pending while the unit was attached, is dropped here too: CalcMoveWater has no such request).
  const MotionBlueprint* mb = u->motion.bp;
  const bool waterCalc = mb && !mb->canFly && mb->motionType != kMotionHover && u->layer != "Land" && u->layer != "Seabed";
  u->motion.needSnap = !waterCalc;
  if (waterCalc) u->motion.surfaceNext = true;
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
  // (the skip flag, arg 2: without it a unit that cannot fly falls, DetachFromFalls in sim/transport.cpp)
  if (Unit* u = ToObject<Unit>(L, 1)) {
    if (lua_toboolean(L, 2) || !DetachFromFalls(*S(L), u)) Detach(*S(L), u);
  }
  else if (Entity* e = ToObject<Entity>(L, 1)) e->attachParent = 0;
  return 0;
}
// Unit:SetDoNotTarget(flag) 0x6c6bc0: sets or clears unit state DoNotTarget (bit 34), nothing else
int l_SetDoNotTarget(lua_State* L) {
  Unit* u = U(L);
  if (lua_toboolean(L, 2)) u->unitStates.Insert(US("DoNotTarget"));
  else u->unitStates.Erase(US("DoNotTarget"));
  return 0;
}
// Unit:HideBone(bone, affectChildren) / Unit:ShowBone(bone, affectChildren) 0x6d14f0: the pose bone's "visible" flag
// (bone+0x48), set for the bone alone or, with affectChildren, for it and everything below it (0x54be30). The
// simulation does not go by it; a host that draws the unit does (HiddenBones in sim/sim_view.h).
template <bool Hide>
int l_ShowHideBone(lua_State* L) {
  Unit* u = U(L);
  const int bone = BoneIndex(L, u, 2);
  if (!u->skeleton || bone < 0 || bone >= u->skeleton->Count()) return 0;
  const bool children = lua_toboolean(L, 3) != 0;
  const auto& bones = u->skeleton->bones();
  if (u->hiddenBones.size() != bones.size()) u->hiddenBones.assign(bones.size(), 0);
  u->hiddenBones[static_cast<size_t>(bone)] = Hide ? 1 : 0;
  if (children) {  // (.scm lists a parent before its children)
    std::vector<uint8_t> below(bones.size(), 0);
    below[static_cast<size_t>(bone)] = 1;
    for (size_t i = static_cast<size_t>(bone) + 1; i < bones.size(); ++i) {
      const int par = bones[i].parent;
      if (par >= 0 && static_cast<size_t>(par) < i && below[static_cast<size_t>(par)]) {
        below[i] = 1;
        u->hiddenBones[i] = Hide ? 1 : 0;
      }
    }
  }
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
std::vector<Unit*> UnitList(lua_State* L, int idx) { return ScriptUnitList(L, idx); }  // func_GetUnitList 0x6eee40
// IssueFactoryRallyPoint(factories, target) 0x6f22c0: a Move (entity or position) appended to the rally queues.
int l_IssueFactoryRallyPoint(lua_State* L) {
  Sim& sim = *S(L);
  std::vector<Unit*> units;
  for (Unit* u : UnitList(L, 1))
    if (LiveCommandCaps(L, u) & 0x1u) units.push_back(u);  // RULEUCC_Move
  CheckScriptTarget(L, 2);  // SCR_GetTarget 0x6eef60, after the unit list (0x6f23d0)
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
  CheckScriptTarget(L, 2);  // SCR_GetTarget 0x6eef60, after the unit list (0x6f2ae6)
  Vec3 p;
  if (!PosArg(L, 2, &p)) return 0;
  Sim* sim = S(L);
  auto c = std::make_shared<UnitCommand>();
  c->type = CommandType::Move;
  c->pos = p;
  c->hasPos = true;
  c->factoryIssued = true;  // IssueMoveOffFactory 0x6f29d0 = IssueMove (append) + cmd+0x142
  for (Unit* u : units) {
    if (c->units.count(u)) continue;
    if (c->units.empty()) {  // (made for the first unit that takes it, as IssueToUnits 0x6f12c0)
      AssignEngineCommandId(*sim, c);
    }
    c->units.insert(u);
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
// Unit:CalculateWorldPositionFromRelative(vector) 0x6c74f0: two arguments; the vector (0x4d0080) turned by the unit's
// orientation (vt+0x18, MultQuadVec 0x452d40) plus its position (vt+0x14), both committed (where the beat began with
// it until AdvanceCoords), as a new Vector. Seen: MP1 15836, army 6's Tempest 0x600049 finishes the cruiser 0x6000d2;
// UAS0401's BuildingState.OnStopBuild sends it off with IssueToUnitMoveOffFactory(unit, CalculateWorldPositionFromRelative(
// {0, 0, -12})): the original's cruiser holds the roll-off Move and the rally (Move x2); ours, with the call a stub that
// gave nil, only the rally.
int l_CalculateWorldPositionFromRelative(lua_State* L) {
  if (lua_gettop(L) != 2)
    luaL_error(L, "%s\n  expected %d args, but got %d", "Unit:CalculateWorldPositionFromRelative(vector)", 2, lua_gettop(L));
  Unit* u = U(L);
  Vec3 v;
  PosArg(L, 2, &v);
  const Sim& sim = *S(L);
  const bool pend = sim.coordsPending && u->lastPosTick == sim.tick();
  const Vec3 p = pend ? u->lastPosition : u->position;
  const Quat q = pend ? u->lastOrientation : u->orientation;
  const Vec3 r = vm::RotateM(q, v);
  PushVector(L, p.x + r.x, p.y + r.y, p.z + r.z);
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
  SetMethod(L, "Unit", "SetUnSelectable", l_SetUnSelectable);
  SetMethod(L, "Unit", "SetDoNotTarget", l_SetDoNotTarget);
  SetMethod(L, "Unit", "HideBone", l_ShowHideBone<true>);
  SetMethod(L, "Unit", "ShowBone", l_ShowHideBone<false>);
  SetMethod(L, "Unit", "SetBlockCommandQueue", l_SetBlockCommandQueue);
  SetMethod(L, "Entity", "AttachBoneTo", l_AttachBoneTo);
  SetMethod(L, "Entity", "AttachTo", l_AttachTo);
  SetMethod(L, "Entity", "DetachFrom", l_DetachFrom);
  SetMethod(L, "Entity", "SetParentOffset", l_SetParentOffset);
  SetMethod(L, "Entity", "DetachAll", l_DetachAll);
  SetMethod(L, "Entity", "GetParent", l_GetParent);
  SetGlobal(L, "IssueFactoryRallyPoint", l_IssueFactoryRallyPoint);
  SetGlobal(L, "IssueClearFactoryCommands", l_IssueClearFactoryCommands);
  SetGlobal(L, "IssueMoveOffFactory", l_IssueMoveOffFactory);
  SetMethod(L, "Unit", "GetRallyPoint", l_GetRallyPoint);
  SetMethod(L, "Unit", "CalculateWorldPositionFromRelative", l_CalculateWorldPositionFromRelative);
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
  float ticks = 1, progress = 0;  // silo+0x44 (beats a missile takes), +0x48 (beats' worth done; a float in FAF)
  float bank[2] = {0, 0};         // silo+0x3c / +0x40: resources helpers brought that are not spent yet
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
      // SiloUpdateProjectileBlueprint 0x5cee40: every counted weapon with a projectile takes the slot of its
      // kind in turn, so the last one holds it
      int kind = FieldBool(L, w, "NukeWeapon") ? 1 : 0;
      if (u->weapons[i]->projBp) {
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
  // CAiSiloBuildImpl::SiloAddBuild 0x5cefc0: the kind needs its weapon, and the weapon must be ENABLED (w+0xac):
  // a commander whose missile launcher waits for its upgrade builds nothing, auto mode or not
  SiloState* s = GetSilo(sim, u);
  if (s->weapon[kind] < 0) return false;
  if (!u->weapons[static_cast<size_t>(s->weapon[kind])]->enabled) return false;
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

// CAiSiloBuildImpl vt+0x28 as FAF patched it (0x5cf030 -> 0x12916f7): a helper hands the silo resources; they
// are banked and spent at once as far as they cover whole shares of the per-beat cost
void SiloAssist(Sim& sim, Unit* u, const float amount[2]) {
  (void)sim;
  SiloState* s = u->silo;
  if (!s || s->stage == 0) return;
  s->bank[0] += amount[0];
  s->bank[1] += amount[1];
  const float a = s->bank[0] / s->perTick[0], b = s->bank[1] / s->perTick[1];
  const float frac = a < b ? a : b;  // minss
  const float left = s->ticks - s->progress;
  const float use = left > frac ? frac : left;
  const float e = s->perTick[0] * use, m = s->perTick[1] * use;
  s->progress += use;
  u->consumedTick[0] += e;
  u->consumedTick[1] += m;
  u->workProgress = s->progress / s->ticks;
  s->bank[0] -= e;
  s->bank[1] -= m;
  if (s->progress >= s->ticks) s->stage = 3;
}

// SiloIsBusy 0x5cef00: the kind is the one at the head of the build list. SiloIsFull 0x5cef20: what is stored
// plus what is listed has reached the weapon's MaxProjectileStorage.
bool SiloIsBusy(Sim& sim, Unit* u, int kind) {
  SiloState* s = GetSilo(sim, u);
  return !s->queue.empty() && s->queue.front() == kind;
}
bool SiloIsFull(Sim& sim, Unit* u, int kind) {
  SiloState* s = GetSilo(sim, u);
  int pending = 0;
  for (int k : s->queue)
    if (k == kind) ++pending;
  return u->siloAmmo[kind] + pending >= s->maxStore[kind];
}

// The silo builder's "add one" (CAiSiloBuildImpl vt+0x20; the interface's build-missile button sends it as the
// info pair SiloBuildTactical / SiloBuildNuke = "add"): kind 0 tactical, 1 nuke. False if the unit has no such
// silo or its store is full.
bool SiloQueueAmmo(Sim& sim, Unit* u, int kind) { return SiloAddBuild(sim, u, kind); }

void SiloNumbers(Sim& sim, const Unit* u, int building[2], int room[2]) {
  building[0] = building[1] = room[0] = room[1] = 0;
  if (const SiloState* s = u->silo) {
    for (int kind : s->queue)
      if (kind == 0 || kind == 1) ++building[kind];
    room[0] = s->maxStore[0];
    room[1] = s->maxStore[1];
    return;
  }
  // (no missile was asked for yet: the room is what the counted weapons' blueprints say, as GetSilo reads them;
  // read as a view reads, making and checking nothing in the state: sim/sim_view.cpp UnitStateOf is the only caller)
  lua_State* L = sim.L();
  const int top = lua_gettop(L);
  auto field = [&](int t, const char* k) {
    lua_gpg_rawgetstr(L, t, k);
    return lua_gettop(L);
  };
  for (const UnitWeapon* w : u->weapons) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->bpRef);
    const int t = lua_gettop(L);
    if (lua_istable(L, t) && lua_toboolean(L, field(t, "CountedProjectile")) && w->projBp) {
      const bool nuke = lua_toboolean(L, field(t, "NukeWeapon")) != 0;
      const int n = field(t, "MaxProjectileStorage");
      room[nuke ? 1 : 0] = lua_isnumber(L, n) ? static_cast<int>(static_cast<float>(lua_tonumber(L, n))) : 0;
    }
    lua_settop(L, top);
  }
}

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
      s->ticks = ticks;
      s->progress = 0;
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
      u->unitStates.Insert(US("SiloBuildingAmmo"));
      s->stage = 2;
      CallWeaponMethod(sim, L, u, "OnSiloBuildStart", wi);
      return;
    }
    case 2: {
      // FAF's patch of this state (0x5cf42a -> 0x129163f): what was granted this beat is spent whatever its
      // size, and the missile advances by the smaller of the two granted shares (minss: the mass share when
      // either is not a number); the full amounts stay requested. (The unpatched game waited for a full grant.)
      if (s->request) {
        const float gE = s->request->granted[0], gM = s->request->granted[1];
        const float a = gE / s->perTick[0], b = gM / s->perTick[1];
        const float frac = a < b ? a : b;
        s->request->granted[0] = s->request->granted[1] = 0;
        u->consumptionShown[0] = s->perTick[0];
        u->consumptionShown[1] = s->perTick[1];
        u->consumedTick[0] += gE;
        u->consumedTick[1] += gM;
        s->progress += frac;
      } else {
        s->progress += 1.0f;
      }
      u->workProgress = s->progress / s->ticks;
      if (s->progress >= s->ticks) s->stage = 3;
      return;
    }
    case 3: {
      if (s->request) {
        s->request->live = false;
        s->request.reset();
      }
      u->consumptionShown[0] = u->consumptionShown[1] = 0;
      u->unitStates.Erase(US("SiloBuildingAmmo"));
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
      u->unitStates.Erase(US("SiloBuildingAmmo"));
      u->silo->stage = 0;
    }
  }
  return 0;
}
int l_SetAutoMode(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  bool on = lua_toboolean(L, 2) != 0;
  u->autoMode = on;  // Unit::SetAutoMode 0x6a73a0: the script is told whether or not it changed
  S(L)->CallMethod(L, u, on ? "OnAutoModeOn" : "OnAutoModeOff", 0);
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

void UnitSkirtRect(const Unit* u, float r[4]) {
  const FRect s = SkirtRect(*u->blueprint, *u->bpData, u->position.x, u->position.z);
  r[0] = s.x0;
  r[1] = s.z0;
  r[2] = s.x1;
  r[3] = s.z1;
}
bool RectFreeOfUnits(Sim& sim, const int r[4]) { return LocationFreeOfUnits(sim, sim.L(), nullptr, r, 0); }
void BlueprintSkirtRect(Sim& sim, const BlueprintInfo& bp, float x, float z, float r[4]) {
  const FRect s = SkirtRect(bp, GetUnitBpData(sim.L(), bp), x, z);
  r[0] = s.x0;
  r[1] = s.z0;
  r[2] = s.x1;
  r[3] = s.z1;
}

}  // namespace moho
