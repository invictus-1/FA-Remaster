// The land navigator (see landnav.h). Function names and addresses refer to the FA exe;
// engine-ref/specs/pathfinding.md and land_motion_blocking.md give the details.
#include "sim/landnav.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/steering.h"
#include "sim/formation.h"
#include "sim/air.h"
#include "sim/commands.h"
#include "sim/combat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>

#include "core/log.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
// TryAdvance's distance limit: the exe reads FAF's constant 0x1292b54 (50.0 in the file), but the
// oracle probe shows targets 86 cells ahead being taken (2026-10-09). MOHO64_NAV_LOOK overrides.
float LookAhead() {
  static const float v = getenv("MOHO64_NAV_LOOK") ? static_cast<float>(atof(getenv("MOHO64_NAV_LOOK"))) : 1e9f;
  return v;
}

bool Dbg() {
  static const bool on = getenv("MOHO64_DEBUG_NAV") != nullptr;
  return on;
}

std::map<const Sim*, std::unique_ptr<HPathTables>>& TablesMap() {
  static std::map<const Sim*, std::unique_ptr<HPathTables>> m;
  return m;
}

const MotionBlueprint& Bp(const Unit* u) { return *u->motion.bp; }
// Entity::GetFootprint 0x678880: the AltFootprint while unit+0x124 or +0x125 is set (UnitFootprint, sim/motion.h)
const NamedFootprint& Fp(const Unit* u) { return UnitFootprint(u); }
int SX(const Unit* u) { return std::max<int>(1, Fp(u).sizeX); }
int SZ(const Unit* u) { return std::max<int>(1, Fp(u).sizeZ); }

PathCell CellOf(const Unit* u, float x, float z) {
  return {static_cast<int>(std::nearbyint(x - SX(u) * 0.5f)), static_cast<int>(std::nearbyint(z - SZ(u) * 0.5f))};
}

const PathGrid* Grid(Sim& sim, const Unit* u) { return UnitFootprintGrid(sim, u); }

// FootprintFits(cell, MobileCheck caps): terrain and structures
bool Fits(Sim& sim, const Unit* u, const PathCell& c) {
  const PathGrid* g = Grid(sim, u);
  return !g || g->Passable(c.x, c.z);
}

float Dist(const PathCell& a, const PathCell& b) {
  float dx = static_cast<float>(a.x - b.x), dz = static_cast<float>(a.z - b.z);
  return std::sqrt(dx * dx + dz * dz);
}
int Manhattan(const PathCell& a, const PathCell& b) { return std::abs(a.x - b.x) + std::abs(a.z - b.z); }

LandNav* Nav(const Unit* u) { return u->motion.nav.get(); }

// One trace of the corridor test (0x720b50): a 4-connected walk over the cells the segment
// a+off -> b+off crosses (mirrored so it runs toward +x, +z); every cell must fit.
bool Trace(Sim& sim, const Unit* u, const PathCell& a, const PathCell& b, float ox, float oz) {
  float ax = a.x + ox, az = a.z + oz, bx = b.x + ox, bz = b.z + oz;
  float sx, ex, sz, ez;
  int mx, mz;
  if (bx >= ax) { sx = ax; ex = bx; mx = 0; } else { sx = -ax; ex = -bx; mx = -1; }
  if (bz >= az) { sz = az; ez = bz; mz = 0; } else { sz = -az; ez = -bz; mz = -1; }
  float dx = ex - sx, dz = ez - sz;
  int cx = static_cast<int>(std::floor(sx)), cz = static_cast<int>(std::floor(sz));
  for (;;) {
    PathCell c{cx ^ mx, cz ^ mz};
    if (!Fits(sim, u, c)) return false;
    int nx = cx + 1, nz = cz + 1;
    float tx = (static_cast<float>(nx) - ex) * dz, tz = (static_cast<float>(nz) - ez) * dx;
    if (tz > tx) cx = nx;
    else cz = nz;
    if (static_cast<float>(cx) > ex || static_cast<float>(cz) > ez) return true;
  }
}

// 0x720c90: one trace along a row/column (cell centres), else two (offsets by direction)
bool Corridor(Sim& sim, const Unit* u, const PathCell& a, const PathCell& b) {
  if (a.x == b.x || a.z == b.z) return Trace(sim, u, a, b, 0.5f, 0.5f);
  bool same = (b.x > a.x && b.z > a.z) || (b.x < a.x && b.z < a.z);
  if (same) return Trace(sim, u, a, b, 0.1f, 0.9f) && Trace(sim, u, a, b, 0.9f, 0.1f);
  return Trace(sim, u, a, b, 0.1f, 0.1f) && Trace(sim, u, a, b, 0.9f, 0.9f);
}

// Reach 0x5af4e0
bool Reach(Sim& sim, const Unit* u, const PathCell& a, const PathCell& b) {
  if (Manhattan(a, b) <= 1) return Fits(sim, u, b);
  return Corridor(sim, u, a, b);
}

// The unit filter of the occupancy tests (0x62eea0): does `e` count as an obstacle for `self`?
// flags 1: units that moved this tick never block; flags 2 (attacking): everyone blocks.
bool BlocksFor(const Unit* self, const Unit* e, int flags) { return !UnitIgnores(self, e, flags); }

}  // namespace

// COGrid::UnitIsBlocked 0x721340: the square [x, x+S] x [z, z+S] (S = the larger footprint byte), y in
// [-1000, 1000]. Candidates are the units whose collision AABB touches the square (GatherUnmarkedUnitsInBox
// 0x721c00, inclusive), not the blueprint size around the position: a turned 0.6 box reaches further.
bool PathUnitBlocked(Unit* u, int x, int z, int flags) {
  if (!u) return false;
  Sim& sim = *Sim::From(u->luaState());
  int S = std::max(SX(u), SZ(u));
  float x0 = static_cast<float>(x), z0 = static_cast<float>(z), x1 = x0 + S, z1 = z0 + S;
  bool blocked = false;
  const Unit* by = nullptr;
  Vec3 byMn{}, byMx{};
  sim.ForUnitsInRect(x0 - 8, z0 - 8, x1 + 8, z1 + 8, [&](Unit* e) {
    if (blocked || e == u) return;
    WorldShape s;
    if (!GetWorldShape(e, &s)) return;
    Vec3 mn, mx;
    ShapeBounds(s, &mn, &mx);
    if (x0 > mx.x || mn.x > x1 || -1000.0f > mx.y || mn.y > 1000.0f || z0 > mx.z || mn.z > z1) return;
    // (the gather asks the entity grid first: a unit is found only in the cells the grid has it in, its span as of
    // the last AdvanceCoords, skirt-wide (Unit::UpdateCollision; GridFindsInBox, as the formation's spot test), not
    // its collision box's own cells. Seen (probe mp1_5397): MP1 beat 5397, army 0's engineer 0x1b on a Repair walk
    // tries (827,225) from (819,228); 0x4c, repairing at (828.46, 224.79), has a box from x 828.0 exactly but is
    // registered from cell 206 (x 827.9 skirt-wide): the original finds it touching the square, tries (826,225) next
    // and takes it; ours left 0x4c out (the box alone starts in cell 207) and took (827,225), 2.5 degrees aside.)
    if (!GridFindsInBox(sim, e, x0, z0, x1, z1)) return;
    by = e;
    byMn = mn;
    byMx = mx;
    // 0x721488: a Land-layer unit counts for an IgnoreStructures footprint only when its own has the flag;
    // Air and Sub layer units never count
    if (e->layer == "Land") {
      if ((Fp(u).flags & 1) && !(e->motion.bp && (Fp(e).flags & 1))) return;
    } else if (e->layer == "Air" || e->layer == "Sub") {
      return;
    }
    if (SameFormationLayer(u, e) || !BlocksFor(u, e, flags)) return;
    // 0x721540: a unit whose footprint is at most 1 blocks outright; a bigger one only when its collision
    // primitive meets the box (square centre, half extents = the path-finder's footprint / 2, 1000) (0x67a9d0)
    if (std::max(Fp(e).sizeX, Fp(e).sizeZ) <= 1) {
      blocked = true;
      return;
    }
    float cx = (x1 + x0) * 0.5f, cz = (z1 + z0) * 0.5f;
    float hx = static_cast<float>(Fp(u).sizeX) * 0.5f, hz = static_cast<float>(Fp(u).sizeZ) * 0.5f;
    if (ShapeOverlapsAABox(s, Vec3{cx - hx, -1000.0f, cz - hz}, Vec3{cx + hx, 1000.0f, cz + hz})) blocked = true;
  });
  static const long dbgClear = getenv("MOHO64_DEBUG_CLEAR") ? atol(getenv("MOHO64_DEBUG_CLEAR")) : -1;
  if (blocked && dbgClear == static_cast<long>(u->id))
    Logf(LogLevel::Info, "clear %u unit %u cell (%d,%d) flags %d: blocked by unit %u at %.4f %.4f, box x %.5f..%.5f z %.5f..%.5f",
         sim.tick(), u->id, x, z, flags, by ? by->id : 0u, by ? by->position.x : 0.0f, by ? by->position.z : 0.0f,
         byMn.x, byMx.x, byMn.z, byMx.z);
  return blocked;
}

namespace {

// 0x7216d0: units in the box along the line from (ax, az) to (bx, bz); air/sub units and formation mates are
// skipped before the unit filter
bool UnitInLine(Sim& sim, Unit* u, float ax, float az, float bx, float bz, int flags) {
  // LineBlocked 0x7216d0 (read 2026-10-11): an oriented box around the line (centre = the midpoint, forward = the
  // line's direction in the plane, half length = half its plane length, half width = the unit's SizeX x 0.5555556,
  // half height 1000), and every unit whose COLLISION PRIMITIVE meets that box (CollectEntitiesInBox 0x721dc0:
  // AABB reject, then the primitive's own box test) goes through the filters. (Ours used to test the units'
  // centres with a radius: a turned or long hull was judged wrong by a few tenths.)
  float dx = bx - ax, dz = bz - az, len = std::sqrt(dz * dz + dx * dx);
  if (len <= 0) return false;
  const float fx = dx / len, fz = dz / len, hw = Bp(u).sizeX * 0.5555556f;
  const Vec3 centre{(ax + bx) * 0.5f, u->position.y, (az + bz) * 0.5f};
  const Vec3 axes[3] = {{fz, 0.0f, -fx}, {0.0f, 1.0f, 0.0f}, {fx, 0.0f, fz}};
  const float half[3] = {hw, 1000.0f, len * 0.5f};
  bool blocked = false;
  const float reach = hw + 16.0f;  // (candidates come by their centre: far enough for the biggest hulls)
  // the line box's own bounds, for the entity grid (0x721de9: 0x4fe2d0, then 0x4fcbe0)
  const Vec3 ext{std::fabs(axes[0].x) * half[0] + std::fabs(axes[2].x) * half[2], 1000.0f,
                 std::fabs(axes[0].z) * half[0] + std::fabs(axes[2].z) * half[2]};
  const Vec3 boxMn{centre.x - ext.x, centre.y - ext.y, centre.z - ext.z};
  const Vec3 boxMx{centre.x + ext.x, centre.y + ext.y, centre.z + ext.z};
  sim.ForUnitsInRect(std::min(ax, bx) - reach, std::min(az, bz) - reach, std::max(ax, bx) + reach,
                     std::max(az, bz) + reach, [&](Unit* e) {
                       if (blocked || e == u) return;
                       WorldShape s;
                       if (!GetWorldShape(e, &s)) return;
                       Vec3 mn, mx;
                       ShapeBounds(s, &mn, &mx);
                       if (!GridCellsMeet(boxMn, boxMx, mn, mx)) return;
                       // (CollectEntitiesInBox's primitive test: a box goes through BoxBox 0x474830, whose interval
                       // arithmetic is not the plain separation test's to the last bit. Seen: Skirmish 3095, army 4's
                       // 0x40001b checking its line to (610, 192) past 0x40002e, 4.7e-5 apart by the plain test: the
                       // original finds it in the way and takes (610, 193); ours took (610, 192), 2 degrees aside.)
                       if (s.type == ShapeType::Box ? !BoxPrimitiveMeetsBox(s, centre, axes, half)
                                                    : !ShapeOverlapsOBox(s, centre, axes, half))
                         return;
                       if (e->layer == "Air" || e->layer == "Sub" || SameFormationLayer(u, e) ||
                           !BlocksFor(u, e, flags))
                         return;
                       blocked = true;
                       static const long dbgClear = getenv("MOHO64_DEBUG_CLEAR") ? atol(getenv("MOHO64_DEBUG_CLEAR")) : -1;
                       if (dbgClear == static_cast<long>(u->id))
                         Logf(LogLevel::Info, "clear %u unit %u line (%.4f,%.4f)-(%.4f,%.4f) half width %.4f: unit %u at %.4f %.4f in the way",
                              sim.tick(), u->id, ax, az, bx, bz, hw, e->id, e->position.x, e->position.z);
                     });
  return blocked;
}
bool UnitInWay(Sim& sim, Unit* u, const PathCell& a, const PathCell& b, int flags) {
  if (a == b) return PathUnitBlocked(u, a.x, a.z, flags);
  return UnitInLine(sim, u, a.x + SX(u) * 0.5f, a.z + SZ(u) * 0.5f, b.x + SX(u) * 0.5f, b.z + SZ(u) * 0.5f, flags);
}
// UnitClear 0x5af670 (cur -> p) and UnitClearTo 0x5af5b0 (the line from the unit's own position to p's centre,
// then p itself)
bool UnitClear(Sim& sim, Unit* u, const LandNav& n, const PathCell& p) {
  return !UnitInWay(sim, u, n.cur, p, n.attackVariant ? 2 : 1);
}
bool UnitClearTo(Sim& sim, Unit* u, const LandNav& n, const PathCell& p) {
  int f = n.attackVariant ? 2 : 1;
  if (UnitInLine(sim, u, u->position.x, u->position.z, p.x + SX(u) * 0.5f, p.z + SZ(u) * 0.5f, f)) return false;
  return !PathUnitBlocked(u, p.x, p.z, f);
}

HPathTables& Tables(Sim& sim) {
  auto& m = TablesMap()[&sim];
  if (!m) {
    const TerrainMap* map = sim.map();
    m = std::make_unique<HPathTables>(&sim.navigation(), map ? map->width() : 0, map ? map->height() : 0);
    m->playRect = sim.playableRect;
    m->CreateMaps(sim.blueprints().Footprints());
  }
  return *m;
}

Vec3 WorldOf(Sim& sim, const Unit* u, const PathCell& c) {
  float x = c.x + SX(u) * 0.5f, z = c.z + SZ(u) * 0.5f;
  const TerrainMap* map = sim.map();
  float y = map ? map->TerrainHeight(x, z) : 0;
  if (map && map->hasWater && y < map->waterElevation && !(Fp(u).caps & kLayerSeabed)) y = map->waterElevation;
  return {x, y, z};
}

// ConsumePathPrefix 0x5ad830 (read): m = min(k, the path's length) points go, and the splice count drops by m, not
// by k (0x5ad848..0x5ad88d: edi = min(esi, size)), clamped to -1. (No gate replay changes with it.)
void Pop(LandNav& n, int k) {
  int m = std::max(0, std::min<int>(k, static_cast<int>(n.path.size())));
  if (m > 0) n.path.erase(n.path.begin(), n.path.begin() + m);
  if (n.path.empty()) n.wait = 0;
  n.spliced -= m;
  if (n.spliced < -1) n.spliced = -1;
}

bool InGoal(const LandNav& n, const PathCell& c) {
  return n.goal[0] <= c.x && c.x < n.goal[2] && n.goal[1] <= c.z && c.z < n.goal[3];
}

// QueueSearch 0x5aa310
void QueueSearch(Sim& sim, Unit* u, LandNav& n) {
  PathTraveler& t = n.pf;
  t.unit = u;
  // BeginQuery takes the cluster map of slot 0 0x5a9d30: unit+0x124 ? the AltFootprint (+0x20 = bp+0x300) :
  // the Footprint (+0x1c = bp+0x2fc), both named ones; CanTraverseCell tests level 0 with Entity::GetFootprint
  // (0x5aa73e / 0x5aa7ca), which also follows +0x125
  t.fp = PathFinderFootprint(u);
  t.fitGrid[0] = FootprintGrid(sim, Bp(u));
  t.fitGrid[1] = AltFootprintGrid(sim, Bp(u));
  t.S = std::max(SX(u), SZ(u));
  t.armyIndex = u->army ? u->army->index : 0;
  PathCell c = CellOf(u, u->position.x, u->position.z);
  t.startStandable = Fits(sim, u, c);
  const TerrainMap* map = sim.map();
  int w = map ? map->width() : 0, h = map ? map->height() : 0;
  // 0x5aa348: +0x74 the army's whole-map flag (vt+0xb8); +0x75 the unit's position with a margin of the footprint
  // size inside the playable rect (the search keeps a unit that starts outside it free to leave)
  t.useWholeMap = Sim::UseWholeMap(u->army);
  float m = static_cast<float>(t.S);
  t.insidePlayable = map && sim.IsWithin(u->position.x, u->position.z, m, false);
  if (t.mode != 0) {
    t.boxes.push_back({std::max(0, c.x - 8), std::max(0, c.z - 8), std::min(w - 2, c.x + 8), std::min(h - 2, c.z + 8)});
    while (t.boxes.size() > 3) t.boxes.pop_front();
  } else {
    t.boxes.clear();
  }
  Tables(sim).Queue(&t);
}

// CAiPathFinder::SetGoal 0x5aa120: is some cell on the goal ring free?
void SetFinderGoal(Sim& sim, Unit* u, LandNav& n, const int* g) {
  PathTraveler& t = n.pf;
  std::copy(g, g + 4, t.goal);
  std::fill(t.inner, t.inner + 4, 0);
  t.goalReachable = false;
  for (int x = g[0]; x <= g[2] && !t.goalReachable; ++x)
    for (int z = g[1]; z <= g[3]; ++z)
      if ((x == g[0] || x == g[2] || z == g[1] || z == g[3]) && Fits(sim, u, {x, z})) {
        t.goalReachable = true;
        break;
      }
}

// UpdateWaterFavorAltFootprintMode 0x5adc70 (the first step of RequestPath; RequestContinuationPath keeps the
// flag): only for a unit in category FAVORSWATER (the Cybran destroyer that walks ashore). unit+0x124 := 0; it
// becomes 1 (paths with the AltFootprint, water only) unless some point of the move is on land, 0x5adc20: the
// terrain elevation (0x44fb90) above the water elevation (map+0x1538; -10000 without water).
//   head command: for each FAVORSWATER unit of the head command's unit list (not dead, 0x57d3f0 / vt+0x28 /
//   vt+0x2c): its position; its place in the command (CUnitCommand::GetPosition 0x6e8a30: the formation cell, else
//   the target, as the footprint-origin cell fistp(p - size / 2) and back to its centre 0x50b1a0); the same for its
//   own head command when that is another one.
//   no command: the unit's position, then the goal's corner cell (nav+0x30 / +0x34) at its centre (0x50b0f0).
// Seen (Naval): the Salem 0x44's continuation at 495 searches the Water4x4 map, whose clusters the ships' searches
// had already built, and is capped in beat 506 as the original's (target at wn 507); on WaterLand4x4 every cluster
// it touched was still to make (about 2,950 x 10 budget) and the answer came a beat later.
void UpdateWaterFavor(Sim& sim, Unit* u, LandNav& n) {
  if (!BpInCategory(sim, u->blueprint, "FAVORSWATER")) return;
  u->altFootprintNav = false;
  const TerrainMap* map = sim.map();
  if (!map) return;
  auto onLand = [&](float x, float z) {
    return map->TerrainHeight(x, z) > (map->hasWater ? map->waterElevation : -10000.0f);
  };
  auto cellCentreOnLand = [&](const Unit* v, const Vec3& p) {
    const float sx = static_cast<float>(SX(v)), sz = static_cast<float>(SZ(v));
    return onLand(std::nearbyint(p.x - sx * 0.5f) + sx * 0.5f, std::nearbyint(p.z - sz * 0.5f) + sz * 0.5f);
  };
  auto commandPos = [&](UnitCommand& c, Unit* v) {
    Vec3 g;
    if (c.form && FormationGoal(sim, c, v, &g)) return g;
    if (c.targetId)
      if (Entity* e = sim.FindEntity(c.targetId)) return e->position;
    return c.pos;
  };
  if (!u->commands.empty() && u->commands.front()) {
    UnitCommand& c = *u->commands.front();
    for (Unit* v : c.units) {
      if (!v || v->dead || v->destroyQueued || !BpInCategory(sim, v->blueprint, "FAVORSWATER")) continue;
      if (onLand(v->position.x, v->position.z)) return;
      if (cellCentreOnLand(v, commandPos(c, v))) return;
      if (!v->commands.empty() && v->commands.front() && v->commands.front().get() != &c &&
          cellCentreOnLand(v, commandPos(*v->commands.front(), v)))
        return;
    }
    u->altFootprintNav = true;
    return;
  }
  if (onLand(u->position.x, u->position.z)) return;
  if (onLand(n.goal[0] + SX(u) * 0.5f, n.goal[1] + SZ(u) * 0.5f)) return;
  u->altFootprintNav = true;
}

// RequestPath 0x5adfe0
void RequestPath(Sim& sim, Unit* u, LandNav& n, int mode) {
  UpdateWaterFavor(sim, u, n);
  if (Dbg() && BpInCategory(sim, u->blueprint, "FAVORSWATER"))
    Logf(LogLevel::Info, "nav: tick %u unit %u water favor: alt footprint %d (forced %d)", sim.tick(), u->id,
         u->altFootprintNav ? 1 : 0, u->altFootprintForced ? 1 : 0);
  n.pf.anchor = n.cur;
  SetFinderGoal(sim, u, n, n.goal);
  n.pf.mode = mode;
  n.lastFollow = {};
  n.wait = 0;
  n.poke = false;
  if (n.waiting) {
    // a formation follower waiting for its leader does not search: its goal corner joins the path and it
    // follows its slot (0x5ae04c, state 6)
    n.path.push_back(PathCell{n.goal[0], n.goal[1]});
    n.state = 6;
  } else {
    u->unitStates.insert("PathFinding");
    QueueSearch(sim, u, n);
    n.state = 3;
  }
  n.adjacent = false;
  n.fits = Fits(sim, u, n.cur) && UnitClearTo(sim, u, n, n.cur);
}

void ResetPathState(Sim& sim, Unit* u, LandNav& n) {
  Tables(sim).Cancel(&n.pf);
  // (0x5ad9c0 leaves the path vector, +0x14..+0x20, as it is: the cells of the last path stay until a search result
  // replaces them, and RequestPath's waiting-follower branch 0x5ae06e..0x5ae08b appends its goal corner behind them
  // (0x5afec0 is push_back). Seen in the oracle (probe mp2_2056): MP2 army 0's Mantis 0x1a, in a guard formation,
  // still holds the cell (337, 403) of its last move after SetGoal at 2053; RequestPath at 2054 makes it
  // [(337, 403), (333, 406)] (state 6). With two cells the formation block does not re-path; TryAdvance asks for a
  // continuation, whose 5 cells are spliced in (+0x5c = 4), so the first real waypoint at 2056 has speed-through
  // 0 (splice count >= 0). Ours had emptied the path, the follower's one-cell path made it re-path in full (no
  // splice) and the waypoint had 1.)
  n.state = 0;
  n.wait = 0;
  // 0x5ad9c0: the target is the cell the unit stood in at its last Execute (+0x28 := +0x24)
  n.target = n.cur;
  n.hasTarget = true;
  // 0x5ada19: the unit's PathFinding and ProblemGettingToGoal states go with the path (a search cut short by a
  // new goal does not leave PathFinding standing)
  u->unitStates.erase("PathFinding");
  u->unitStates.erase("ProblemGettingToGoal");
  // 0x5ada33: unit+0x124 := 0 (the navigator's AltFootprint flag; RequestPath sets it again for a FAVORSWATER unit)
  u->altFootprintNav = false;
  // (0x5ad9c0 leaves the splice count +0x5c alone: only ConsumePathPrefix 0x5ad830 and the splice itself write
  // it, so a move dropped with spliced cells ahead hands the count to the next goal's first waypoints)
  // 0x5ada4a..0x5ada72, 0x5ada8f: the followed slot point +0x84 := (0, 0, 0) and following +0x92 := 0. (0x5ad9c0
  // also zeroes +0x90, +0x91, +0x78 and unlinks the leader +0x7c; ours leaves those to ConfigureGoal's next goal.)
  n.following = false;
  n.lastFollow = {};
}

// RequestContinuationPath 0x5aec70
void RequestContinuation(Sim& sim, Unit* u, LandNav& n, int mode) {
  if (Dbg())
    Logf(LogLevel::Info, "nav: tick %u unit %u continuation mode %d cur (%d,%d) path %zu front (%d,%d)", sim.tick(), u->id,
         mode, n.cur.x, n.cur.z, n.path.size(), n.path.empty() ? -1 : n.path[0].x, n.path.empty() ? -1 : n.path[0].z);
  if (!n.path.empty() && n.path[0] == n.cur) {
    Pop(n, 1);
    if (n.path.empty()) {
      ResetPathState(sim, u, n);
      return;
    }
  }
  if (n.path.empty()) {
    ResetPathState(sim, u, n);
    return;
  }
  // 0x5aecb1: Reach(path[0], path[0]) / UnitClear(path[0], path[0]): the point itself, not the way to it
  while (n.path.size() > 1 && (!Fits(sim, u, n.path[0]) ||
                               UnitInWay(sim, u, n.path[0], n.path[0], n.attackVariant ? 2 : 1) ||
                               Manhattan(n.path[0], n.cur) < 2))
    Pop(n, 1);
  n.fits = Fits(sim, u, n.cur) && UnitClearTo(sim, u, n, n.cur);
  if (u->unitStates.count("Attacking")) mode = 3;
  if (mode == 3) n.attackVariant = true;
  n.pf.mode = mode;
  n.pf.anchor = n.cur;
  int g[4] = {n.path[0].x, n.path[0].z, n.path[0].x + 1, n.path[0].z + 1};
  SetFinderGoal(sim, u, n, g);
  QueueSearch(sim, u, n);
  n.state = 4;
}

void RetryRule(Sim& sim, Unit* u, LandNav& n) {
  if (n.retries < 3) {
    ++n.retries;
    n.wait = 10;
    n.state = 5;
    return;
  }
  if (++n.replans >= 3) {
    n.state = 1;
    return;
  }
  n.retries = 0;
  RequestPath(sim, u, n, 1);
}

// OnEvent 0x5aeeb0: a search finished
void OnPath(Sim& sim, Unit* u, LandNav& n) {
  n.pf.delivered = false;
  std::vector<PathCell> got = std::move(n.pf.result);
  n.pf.result.clear();
  if (Dbg()) {
    std::string s;
    for (const auto& c : got) s += " (" + std::to_string(c.x) + "," + std::to_string(c.z) + ")";
    Logf(LogLevel::Info, "nav: tick %u unit %u path state %d accepted %d:%s", sim.tick(), u->id, n.state,
         n.pf.accepted ? 1 : 0, s.c_str());
  }
  if (n.state == 3) {
    u->unitStates.erase("PathFinding");
    n.path = std::move(got);
    // The unit state ProblemGettingToGoal (bit 24) is set here, in the beat the search is delivered (0x5af211 for
    // an empty path, 0x5af2bf for a path that ends outside the goal), not by the next beat's Execute; an empty path
    // of a Patrolling unit (IsUnitState(12), 0x5af224) also sets NeedToTerminateTask (bit 25, 0x5af238). (Seen:
    // LAND 1650, a Spearhead (xel0306) on a patrol whose search toward (310.5, 753.5) ended short at 1650: the
    // original's unit record has ProblemGettingToGoal in that beat, ours one beat later.)
    if (n.path.empty()) {
      n.state = 1;
      n.problem = true;
      u->unitStates.insert("ProblemGettingToGoal");
      if (u->unitStates.count("Patrolling")) u->unitStates.insert("NeedToTerminateTask");
      return;
    }
    const PathCell& last = n.path.back();
    if (!InGoal(n, last)) {
      n.problem = true;
      u->unitStates.insert("ProblemGettingToGoal");
      // the goal centre is appended when the footprint fits there (OnEvent 0x5aeeb0, pathfinding.md 4.4)
      PathCell c{(n.goal[0] + n.goal[2]) / 2, (n.goal[1] + n.goal[3]) / 2};
      if (Fits(sim, u, c)) n.path.push_back(c);
    }
    n.state = 5;
    n.wait = 0;
    n.advanceDist = kInf;
    n.pf.mode = 0;
    return;
  }
  if (n.state != 4) return;
  int k = static_cast<int>(got.size());
  if (got.empty()) {
    RetryRule(sim, u, n);
    return;
  }
  if (!n.path.empty() && got.back() == n.path[0]) {
    n.retries = 0;
    n.hasDeadEnd = false;
    n.path.insert(n.path.begin(), got.begin(), got.end() - 1);
    n.spliced = n.spliced < 0 ? k - 1 : n.spliced + k - 1;
  } else {
    if (!n.path.empty() && k < 3) {
      // OnEvent 0x5af0e0: Reach(path[0], path[0]) = the footprint fits at path[0] (manhattan 0)
      if (!Fits(sim, u, n.path[0]) || UnitInWay(sim, u, n.path[0], n.path[0], n.attackVariant ? 2 : 1)) {
        if (n.retries < 3) RetryRule(sim, u, n);
        else n.state = 1;
        return;
      }
      if (n.hasDeadEnd && n.deadEnd == n.path[0]) {
        n.state = 1;
        n.retries = 0;
        return;
      }
      n.deadEnd = n.path[0];
      n.hasDeadEnd = true;
    }
    n.path.insert(n.path.begin(), got.begin(), got.end());
    n.spliced = n.spliced < 0 ? k : n.spliced + k;
  }
  n.state = 5;
  n.wait = 0;
  n.advanceDist = kInf;
  n.pf.mode = 0;
}

// StraightRun 0x5af360
int StraightRun(Sim& sim, Unit* u, LandNav& n) {
  if (n.path.empty()) return 0;
  if (n.path[0] == n.cur && n.path.size() > 1) Pop(n, 1);
  int dx = n.path[0].x - n.cur.x, dz = n.path[0].z - n.cur.z;
  if (std::abs(dx) >= 2 || std::abs(dz) >= 2) return 0;
  if (!Reach(sim, u, n.cur, n.path[0]) || !UnitClearTo(sim, u, n, n.path[0])) return 0;
  int k = 0;
  for (;;) {
    if (k + 1 == static_cast<int>(n.path.size())) return k;
    const PathCell& a = n.path[k];
    const PathCell& b = n.path[k + 1];
    if (b.x - a.x != dx || b.z - a.z != dz) return k;  // 0x5b0880: the step changes
    if (!Reach(sim, u, n.cur, b) || !UnitClearTo(sim, u, n, b)) return k;
    ++k;
  }
}

void SetTargetPoint(LandNav& n, int i) {
  Pop(n, i);
  if (n.path.empty()) return;
  n.target = n.path[0];
  n.hasTarget = true;
  n.state = 5;
  n.wait = 0;
  n.adjacent = Manhattan(n.cur, n.target) <= 1;
}

// TryAdvanceTargetPoint 0x5af7e0
bool TryAdvance(Sim& sim, Unit* u, LandNav& n) {
  int lo = 0;
  if (n.fits) lo = std::max(0, StraightRun(sim, u, n));
  int size = static_cast<int>(n.path.size());
  int hi;
  if (n.fits) hi = std::min(size - 1, std::max(10, lo));
  else hi = std::min(size - 1, 1);
  for (int i = hi; i >= lo; --i) {
    if (!(Dist(n.path[i], n.cur) < LookAhead() || i == lo)) continue;
    bool ok1 = Reach(sim, u, n.cur, n.path[i]);
    // (0x5af946: for a neighbouring cell UnitClear 0x5af670 gets the cell twice, which is the test of the cell
    // itself, not of the way from the current cell to it; further cells get the line from the unit's position)
    bool ok2 = Manhattan(n.path[i], n.cur) <= 1 ? !UnitInWay(sim, u, n.path[i], n.path[i], n.attackVariant ? 2 : 1)
                                                : UnitClearTo(sim, u, n, n.path[i]);
    if (Dbg())
      Logf(LogLevel::Info, "nav: tick %u unit %u advance? i %d (%d,%d) from (%d,%d) reach %d clear %d fits %d lo %d hi %d",
           sim.tick(), u->id, i, n.path[i].x, n.path[i].z, n.cur.x, n.cur.z, ok1 ? 1 : 0, ok2 ? 1 : 0, n.fits ? 1 : 0, lo, hi);
    bool set = false;
    if (ok1 && ok2) {
      n.fits = true;
      set = true;
    } else if (!n.fits && !n.poke) {
      set = true;
    }
    if (!set) continue;
    if (i == 0 && lo < hi && n.path.size() > 1 && UnitInWay(sim, u, n.path[0], n.path[1], n.attackVariant ? 2 : 1) &&
        Dist(n.cur, n.path[0]) < 10.0f) {
      Pop(n, 1);
      return false;
    }
    if (n.stuck > 30 && i == 0) return false;
    SetTargetPoint(n, i);
    n.attackVariant = false;
    n.following = false;
    n.lastFollow = {};
    return true;
  }
  if (n.attackVariant && hi > 0) {
    SetTargetPoint(n, 0);
    n.attackVariant = false;
    return true;
  }
  if (lo > 0) Pop(n, lo - 1);
  return false;
}

bool IsZeroVec(const Vec3& v) { return v.x == 0 && v.y == 0 && v.z == 0; }
// GetTargetPos 0x5ad8b0: the followed slot point +0x84 is the target position while +0x92 is set and the point is not
// (0, 0, 0) (0x4f0a50 against the zero vector 0xf3d21c)
bool FollowsSlotPoint(const LandNav& n) { return n.following && !IsZeroVec(n.lastFollow); }

// The formation block of UpdateCurrentPosition (formations.md 9.2). 1: return now, 0: go on to the tail.
int FollowSlot(Sim& sim, Unit* u, LandNav& n) {
  Formation* F = GetFormation(sim, u);
  if (!F) {
    n.inFormation = false;
    n.cachedLeader = 0;
    n.following = false;
    n.lastFollow = {};
    return 0;
  }
  const uint32_t self = EntityRef(u);
  uint32_t L = u->formLeader;
  bool changed = L != n.cachedLeader;
  n.cachedLeader = L;
  if (!L || L == self) {
    if (changed && n.cachedLeader == self) {  // just became the leader: re-path to its own goal
      n.path.clear();
      n.wait = 0;
      n.lastFollow = {};
      n.following = false;
      n.waiting = false;
      RequestPath(sim, u, n, 0);
      return 1;
    }
    n.lastFollow = {};
    n.following = false;
    n.cachedLeader = 0;
    n.inFormation = false;
    return 0;
  }
  Entity* le = sim.FindEntity(L);
  Unit* lu = le && le->kind == Entity::Kind::Unit ? static_cast<Unit*>(le) : nullptr;
  if (lu && LandNavStatus(lu) == 1) {  // the leader is still computing its path: hold
    if (!n.hasTarget || n.target != n.cur) {
      n.target = n.cur;
      n.hasTarget = true;
    }
    n.waiting = true;
    return 0;
  }
  n.waiting = false;
  const Vec3 p = u->formSlot;
  PathCell s = CellOf(u, p.x, p.z);
  if (u->id % 13 != sim.tick() % 13 && !IsZeroVec(n.lastFollow)) return 1;  // between phases: keep following
  if (n.hasTarget && n.target == s) {
    n.lastFollow = p;  // (the target position while following: a new position in the same cell is a new waypoint)
    return 1;
  }
  float dd = Dist(n.cur, s);
  float step = std::min(dd, 10.0f);
  PathCell probe = n.cur;
  if (dd > 0) {
    probe = {n.cur.x + static_cast<int>((s.x - n.cur.x) * step / dd), n.cur.z + static_cast<int>((s.z - n.cur.z) * step / dd)};
  }
  // 0x5ae8a5: the followed point with a margin of 1 inside the playable rect (the whole map: the army's flag)
  bool within = !sim.map() || sim.IsWithin(p.x, p.z, 1.0f, Sim::UseWholeMap(u->army));
  if (Reach(sim, u, n.cur, probe) && UnitClearTo(sim, u, n, probe) && within) {
    n.advanceDist = 0.5f * dd;
    n.target = s;
    n.hasTarget = true;
    n.following = true;
    n.lastFollow = p;
    n.state = 6;
    n.adjacent = Manhattan(n.cur, n.target) <= 1;
    if (n.path.size() > 1) n.path = {PathCell{n.goal[0], n.goal[1]}};
    return 1;
  }
  n.lastFail = sim.tick();
  if (n.following || n.path.size() == 1) {
    n.lastFollow = {};
    n.following = false;
    n.path.clear();
    n.wait = 0;
    RequestPath(sim, u, n, 0);
    return 1;
  }
  return 0;
}

// UpdateCurrentPosition 0x5ae2d0
void UpdateCurrentPosition(Sim& sim, Unit* u, LandNav& n) {
  {  // SetCurrentPosition 0x5ae210: the size of the path-finder's footprint (+0x124 only)
    const NamedFootprint& f = PathFinderFootprint(u);
    n.cur = {static_cast<int>(std::nearbyint(u->position.x - std::max<int>(1, f.sizeX) * 0.5f)),
             static_cast<int>(std::nearbyint(u->position.z - std::max<int>(1, f.sizeZ) * 0.5f))};
  }
  if (n.thinkDelay > 0) {
    if (--n.thinkDelay == 0) RequestPath(sim, u, n, n.requestMode);
    n.target = n.cur;  // (0x5ae37f) the target is the own cell
    n.hasTarget = true;
    return;
  }
  if (n.wait > 0) {
    if (--n.wait == 0) RequestContinuation(sim, u, n, 2);
    return;
  }
  while (n.path.size() > 1 && Dist(n.path[0], n.cur) >= Dist(n.path[1], n.cur) && UnitClearTo(sim, u, n, n.path[1]))
    Pop(n, 1);
  if (n.state != 5 && n.state != 6) {  // (0x5ae477) no path is being followed: the target is the own cell
    n.target = n.cur;
    n.hasTarget = true;
    return;
  }
  if (!n.path.empty() && n.cur == n.path.back()) {
    n.state = 0;
    n.target = n.cur;  // (0x5ae498)
    return;
  }
  // 0x5ae4a6..0x5ae4e4: a SurfacingSub (Physics.MotionType 5, bp+0x290) whose layer (unit+0x120) is not the one
  // its navigator holds (+0x54: the layer the unit was made in, until this changes it) takes the new layer and
  // asks for a continuation path (mode 2),
  // and does nothing else in this beat. (Seen: Skirmish 2978, army 4's sub 0x400028 reaches the surface on its
  // aggressive move; the original's target goes from (225, 525) to (310, 461) at 2980, ours kept the far one.)
  if (Bp(u).motionType == kMotionSurfacingSub) {
    const std::string& l = u->layer;
    const int layer = l == "Land" ? 1 : l == "Seabed" ? 2 : l == "Sub" ? 4 : l == "Water" ? 8 : l == "Air" ? 0x10
                    : l == "Orbit" ? 0x20 : 0;
    if (n.pathLayer != layer) {
      n.pathLayer = layer;
      RequestContinuation(sim, u, n, 2);
      return;
    }
  }
  float d = n.hasTarget ? Dist(n.cur, n.target) : kInf;
  bool force = n.poke;  // the steering's push report forces a TryAdvance (Func1 0x5a3e80)
  const MotionBlueprint& b = Bp(u);
  const uint32_t t13 = sim.tick() % 13, t7 = sim.tick() % 7;
  bool ok = !n.poke;
  if (n.hasTarget && (n.inFormation ? u->id % 13 == t13 : u->id % 7 == t7) && n.cur != n.target && !n.waiting &&
      !n.adjacent && !n.poke && Fits(sim, u, n.cur)) {
    float look = std::max(4.0f * std::max(SX(u), SZ(u)), 4.0f * b.maxSpeed);
    PathCell t = n.target;
    float dd = Dist(n.cur, t);
    if (dd > look) {
      // 0x5ae170: the point `look` along the way, each part cut toward zero (cvttss2si at 0x5ae1f3), not rounded
      const float dx = static_cast<float>(t.x - n.cur.x), dz = static_cast<float>(t.z - n.cur.z);
      const float k = look / std::sqrt(dz * dz + dx * dx);
      t = {n.cur.x + static_cast<int>(dx * k), n.cur.z + static_cast<int>(dz * k)};
    }
    const bool reach = Reach(sim, u, n.cur, t), clear = reach && UnitClear(sim, u, n, t);
    if (Dbg())
      Logf(LogLevel::Info, "nav: tick %u unit %u look ahead %.3f from (%d,%d) to (%d,%d) of target (%d,%d): reach %d clear %d",
           sim.tick(), u->id, look, n.cur.x, n.cur.z, t.x, t.z, n.target.x, n.target.z, reach ? 1 : 0, clear ? 1 : 0);
    if (!clear) {
      force = true;
      ok = false;
    } else if (b.turnRadius > b.turnRate && sim.tick() > n.lastAdvance + 10) {
      force = true;
    }
  }
  // formation block (0x5ae63a)
  static const long dbgF = getenv("MOHO64_DEBUG_FOLLOW") ? atol(getenv("MOHO64_DEBUG_FOLLOW")) : -1;
  if (dbgF == static_cast<long>(u->id))
    Logf(LogLevel::Info, "follow %u inForm %d ok %d lastFail %u waiting %d following %d state %d target (%d,%d) poke %d",
         sim.tick(), n.inFormation ? 1 : 0, ok ? 1 : 0, n.lastFail, n.waiting ? 1 : 0, n.following ? 1 : 0, n.state,
         n.target.x, n.target.z, n.poke ? 1 : 0);
  if (n.inFormation && ok && sim.tick() > n.lastFail + 100) {
    switch (FollowSlot(sim, u, n)) {
      case 1: return;      // following / re-pathed
      default: break;      // TAIL
    }
  }
  if (n.waiting) return;   // waiting for the leader: no stuck count, no advance
  bool still = u->position.x == n.prevPos.x && u->position.y == n.prevPos.y && u->position.z == n.prevPos.z;
  n.stuck = (still && !u->immobile) ? n.stuck + 1 : 0;
  if (n.advanceDist < d && !force && n.stuck < 31) return;
  n.lastAdvance = sim.tick();
  if (TryAdvance(sim, u, n)) {
    n.advanceDist = 0.5f * Dist(n.cur, n.target);
    n.poke = false;  // 0x5aebaa: a target taken clears the steering's push report (else it forces one every beat)
    return;
  }
  if (n.stuck > 30) {  // (0x5aebb9: given up; the target is the own cell, which Execute then hands over)
    n.state = 0;
    n.target = n.cur;
    n.hasTarget = true;
    n.wait = 0;
    return;
  }
  if (n.poke) {
    n.poke = false;
    RequestContinuation(sim, u, n, 3);
    return;
  }
  // 0x5aec1a: the target cell goes to UnitBlocked 0x62aba0 as the point (x, 0, z) of its corner, not its centre, and
  // 0x62aba0 tests the cell of a unit standing at that point: fistp(x - SizeX/2), fistp(z - SizeZ/2), rounded half
  // to even. So an odd footprint tests the cell itself or the one before it (x - 0.5 rounds to the even one), an
  // even one the cell diagonally before it. Seen: LAND 1306, Titan 4 one diagonal step from its goal cell (285, 683),
  // whose corner a standing AA unit's box touches by 0.05: the original tests (284, 682), finds it free and asks for
  // a continuation path at once (its target becomes (287, 684) at 1307 and the move ends at 1310); ours tested
  // (285, 683) and waited ten beats.
  if (!n.path.empty() && n.path.back() == n.target && !u->unitStates.count("WaitingForTransport")) {
    const PathCell c = CellOf(u, static_cast<float>(n.target.x), static_cast<float>(n.target.z));
    if (PathUnitBlocked(u, c.x, c.z, 1)) {
      n.wait = 10;
      return;
    }
  }
  RequestContinuation(sim, u, n, 2);
}

// Execute 0x5a4280: the steering gets the new waypoint; a finished path ends the move
void Execute(Sim& sim, Unit* u, LandNav& n) {
  UpdateCurrentPosition(sim, u, n);
  n.prevPos = u->position;
  // 0x5a4342: while the goal still waits for its search (path state 2: a formation's path delay, the thinking
  // beat) the steering is told nothing. Else (0x5a434c) what decides is whether the navigator's target point
  // differs from the steering's last waypoint (memcmp 0x4f0a50 of the two; GetWaypoint 0x5d2170), not whether
  // the target changed this beat: then the steering gets it and the status becomes 2 (ours: thinking off). The
  // target is the unit's own cell whenever no path is being followed, so a unit whose search runs brakes on the
  // cell it is in, cell after cell while it still rolls; and a formation member that sat out its path delay on
  // its own cell hands that cell over in the beat its search starts, so that the path's first waypoint is then
  // a drive-through one.
  if (n.active && n.state != 2 && n.hasTarget) {
    // following a formation slot: the target position is the slot's own (not the cell centre). GetTargetPos 0x5ad8b0
    // reads only +0x92 and +0x84, whatever the target cell is: in the beat a follower arrives (cur == path.back(),
    // 0x5ae498 sets the target to its own cell) the slot point still stands, equals the steering's last waypoint and
    // nothing new goes over; MakeIdle then stops it, and the motion's drive-through byte keeps that waypoint's value.
    // Seen (probe mp2_2201): MP2 army 0's Mantis 0x1a arrives at 2201 with no SetWaypoints(1) and speed-through 1
    // until its next goal's first waypoint at 2204; ours handed over its own cell, in the goal, with 0.
    // The point is the whole stored slot point, its height included (0x5ad923..0x5ad932 copy +0x84, +0x88, +0x8c),
    // not the target cell's ground: a follower that holds for its leader (target := its own cell, 0x5ae75a) keeps
    // the same waypoint. Seen (probe mp3_follow): MP3 3142, army 3's guards 0x30002f / 0x300030 hold while their
    // commander's path is searched (its status 1 at their turns, +0x91 = 1 at 3143) and the steering gets nothing;
    // ours took the height of the own cell, so the point differed and went over as a new waypoint, drive-through 1.
    const Vec3 wp = FollowsSlotPoint(n) ? n.lastFollow : WorldOf(sim, u, n.target);
    const UnitMotion& m = u->motion;
    const bool same = m.steerWpSet && m.steerWp.x == wp.x && m.steerWp.y == wp.y && m.steerWp.z == wp.z;
    if (!same) {
      bool inGoal = InGoal(n, n.target);
      bool f9e = n.thinking ? false : n.spliced < 0;
      bool through = inGoal ? n.speedThroughGoal : f9e;
      if (Dbg())
        Logf(LogLevel::Info, "nav: tick %u unit %u through: in goal %d (through goal %d) thinking %d spliced %d", sim.tick(), u->id,
             inGoal ? 1 : 0, n.speedThroughGoal ? 1 : 0, n.thinking ? 1 : 0, n.spliced);
      n.thinking = false;
      MotionSetWaypoint(sim, u, wp, through);
      u->motion.wpInGoal = inGoal;
      if (Dbg())
        Logf(LogLevel::Info, "nav: tick %u unit %u target (%d,%d) through %d cur (%d,%d) path %zu", sim.tick(), u->id,
             n.target.x, n.target.z, through ? 1 : 0, n.cur.x, n.cur.z, n.path.size());
    }
  }
  if (n.state < 2) {
    bool ok = n.state == 0;
    if (Dbg()) Logf(LogLevel::Info, "nav: tick %u unit %u %s", sim.tick(), u->id, ok ? "succeeded" : "failed");
    n.active = false;
    n.hasTarget = false;  // (a navigator that went idle starts its next goal with a target change)
    Tables(sim).Cancel(&n.pf);
    u->unitStates.erase("PathFinding");
    // (0x5a43fb: MakeIdle, vt+0x4c = 0x5a3e00, runs ResetPathState 0x5ad9c0, which clears unit+0x124: a FAVORSWATER
    // unit stands on its Footprint again once its move is over)
    u->altFootprintNav = false;
    // ... and the same ResetPathState takes ProblemGettingToGoal off with the path (0x5ada19). Seen (MP3 1258): army
    // 0's engineer 5, whose reclaim walk failed (the goal could not be reached), loses the state in the beat its walk
    // ends in the original; ours kept it until the reclaim was over.
    u->unitStates.erase("ProblemGettingToGoal");
    n.problem = false;
    n.following = false;  // (and +0x84, +0x92: MakeIdle's ResetPathState)
    n.lastFollow = {};
    MotionNavDone(u, ok);
  }
}

}  // namespace

bool LandCellFits(Sim& sim, const Unit* u, int x, int z) { return Fits(sim, u, PathCell{x, z}); }

bool LandNavFollowingSlot(const Unit* u) {
  const LandNav* n = u->motion.nav.get();
  return n && n->active && n->state == 6;
}

bool UnitFitsAt(Sim& sim, const Unit* u, float x, float z) {
  const TerrainMap* map = sim.map();
  if (map && (x < 0 || z < 0 || x > map->width() || z > map->height())) return false;
  // 0x720aa0 -> OCCUPY_FootprintFits(caps = -1): the occupancy layers only clear the land (1, 2) and water (8)
  // bits, so a footprint that may be in the Air (16) always fits (a build drone over a factory)
  if (u->blueprint && u->blueprint->hasFootprint && (Fp(u).caps & 0x10)) return true;
  return Fits(sim, u, CellOf(u, x, z));
}

// The rings of the free-spot search 0x62b200 around a cell that does not fit: square rings stepping by the
// footprint size, up to 900 candidates. true: *gc is the first cell where the unit fits; false: *gc unchanged.
static bool FreeSpotRings(Sim& sim, const Unit* u, PathCell* gc, bool wholeMap) {
  int step = std::max(SX(u), SZ(u));
  int tested = 0;
  bool found = false;
  const TerrainMap* map = sim.map();
  float W = map ? static_cast<float>(map->width()) : 1e9f, H = map ? static_cast<float>(map->height()) : 1e9f;
  Vec3 base = WorldOf(sim, u, *gc);
  // FindFreeSpot 0x62b200 tests the point it was given (not each candidate) with STIMap::IsWithin, margin = the
  // size, CAiNavigatorLand::SetGoal's flag the army's (0x5a4012): outside, no candidate is taken (0x62b4f7)
  if (map && !sim.IsWithin(base.x, base.z, static_cast<float>(step), wholeMap)) return false;
  for (int r = 1; !found; ++r) {
    for (int i = -r; i <= r && !found; ++i) {
      int jStep = (i == -r || i == r) ? 1 : 2 * r;
      for (int j = -r; j <= r; j += jStep) {
        ++tested;
        float x = base.x + i * step, z = base.z + j * step;
        float m = static_cast<float>(step);
        if (!(x - m >= 0 && z - m >= 0 && x + m < W && z + m < H)) continue;
        PathCell c = CellOf(u, x, z);
        if (Fits(sim, u, c)) {
          *gc = c;
          found = true;
          break;
        }
      }
    }
    if (tested > 899) break;
  }
  return found;
}

// FindFreeSpot 0x62b200 for a point: where the unit's footprint fits, at the point or on the rings around it (the
// teleport's: the whole map, 0x60ad60 pushes 1).
bool LandFreeSpot(Sim& sim, const Unit* u, Vec3* pos) {
  PathCell c = CellOf(u, pos->x, pos->z);
  if (!Fits(sim, u, c) && !FreeSpotRings(sim, u, &c, true)) return false;
  *pos = WorldOf(sim, u, c);
  return true;
}

// CAiNavigatorLand::SetGoal 0x5a3ed0 (1x1 goal) with the free-spot spiral 0x62b200
void LandNavSetGoal(Sim& sim, Unit* u, const Vec3& goalPos, bool speedThrough) {
  PathCell gc = CellOf(u, goalPos.x, goalPos.z);
  LandNavSetGoalRect(sim, u, gc.x, gc.z, gc.x + 1, gc.z + 1, speedThrough);
}

// SetGoal 0x5a3ed0 with a goal rect [x0, x1) x [z0, z1) of cells: a 1x1 goal is moved to a free spot nearby,
// larger rects are taken as they are (pathfinding.md 4.1)
void LandNavSetGoalRect(Sim& sim, Unit* u, int x0, int z0, int x1, int z1, bool speedThrough) {
  // 0x5a3f3d..0x5a3f46: a goal rect whose corner is cell (0, 0) is no goal: SetGoal returns at once (0x5a416a) and the
  // navigator keeps what it had. Seen (probe mp2_8416b): MP2 8416, army 1's Striker 0x10000d starts a queued Attack
  // whose target is gone; its task's lastPos is (0, 0, 0), its Update asks for the cell (0, 0) every beat, and the
  // original's unit stands Attacking for 150 beats; ours searched a path to the map's corner and drove off.
  if (x0 == 0 && z0 == 0) return;
  // (No facing change here: the motion's facing (+0x20, SetFacing 0x6b8590) is replaced only when the steering hands
  // the motion a target, CUnitMotion::SetTarget 0x6b85e0 by way of 0x6b88f0 with no facing (for a unit that cannot
  // fly, 0x6b878d -> 0x6b8872: zero): DriveToNextWaypoint 0x5d310e and two collision cases (SteeringTick,
  // ProcessSplineMovement).)
  if (!u->motion.nav) {
    u->motion.nav = std::make_shared<LandNav>();
    u->motion.nav->pathLayer = u->createLayer;  // (the navigator ctor 0x5ad513: +0x54 = unit+0x120 then)
  }
  LandNav& n = *u->motion.nav;
  PathCell gc{x0, z0};
  const bool single = x1 == x0 + 1 && z1 == z0 + 1;
  // 0x5a3edc: while the navigator's status is 1 (a goal given, no target handed to the steering yet: also while
  // its search runs and in the beat the path arrived) the same goal again changes nothing. A formation gives
  // every member its goal each beat while it forms up, so a member whose slot did not move keeps its search.
  const bool same = n.active && n.thinking;
  if (same && n.goal[0] == x0 && n.goal[1] == z0 && n.goal[2] == x1 && n.goal[3] == z1) {
    n.speedThroughGoal = speedThrough;
    return;
  }
  // the free-spot spiral: the cell itself, then square rings stepping by the footprint size
  if (single && !Fits(sim, u, gc)) FreeSpotRings(sim, u, &gc, Sim::UseWholeMap(u->army));
  if (single && same && n.goal[0] == gc.x && n.goal[1] == gc.z) {  // (0x5a4056: the spot found is the one it has)
    n.speedThroughGoal = speedThrough;
    return;
  }
  ResetPathState(sim, u, n);
  n.active = true;
  n.goal[0] = gc.x;
  n.goal[1] = gc.z;
  n.goal[2] = single ? gc.x + 1 : x1;
  n.goal[3] = single ? gc.z + 1 : z1;
  n.speedThroughGoal = speedThrough;
  n.thinking = true;
  n.problem = false;
  n.retries = n.replans = 0;
  n.hasDeadEnd = false;
  // (n.stuck goes on: the path navigator's still-beat count +0x74 is written only by its ctor (0x5ad646) and by
  // UpdateCurrentPosition (0x5aeb1a / 0x5aeb20); ResetPathState 0x5ad9c0 clears +0x60, +0x64, +0x68 and +0x78
  // (0x5ada7d..0x5ada86) but not it. So a unit that gave up a walk by the stuck rule and gets a new goal without having
  // moved gives that one up too at its first TryAdvance that finds nothing. Seen: MP3 6184, army 1's commander
  // 0x100000 on patrol, boxed in by its own idle tanks: its reclaim walk ends by the stuck rule at 6176 (still since
  // 6143); the next reclaim walk (6182) ends at 6184 in the original and the reclaim starts from where it stands;
  // ours had counted from 0 again and kept asking for continuations.)
  n.poke = false;
  n.attackVariant = false;
  // (the target point survives a new goal: seen in the original's navigator lines, a unit re-ordered while its
  // target is already its own cell reports no target change the next beat, so the first waypoint of the new path
  // still counts as the goal's first ("thinking") and is not a drive-through one)
  n.requestMode = 0;
  n.state = 2;
  // ConfigureGoal 0x5ad6e0: the formation flags
  n.lastFail = 0;
  n.cachedLeader = 0;
  n.waiting = false;
  n.following = false;
  n.lastFollow = {};
  n.inFormation = false;
  if (!(u->navIgnoreFormation || (u->navigator && u->navigator->ignoreFormation))) {
    Formation* F = GetFormation(sim, u);
    n.inFormation = F && FormationIsForm(*F);
    if (n.inFormation) {
      n.cachedLeader = u->formLeader;
      if (u->formLeader && u->formLeader != EntityRef(u)) n.waiting = true;
    }
  }
  // BeginThinking 0x5adba0: a formation member waits its path delay
  n.thinkDelay = u->unitStates.count("TransportLoading") ? 1 : FormationPathDelay(sim, u);
  n.startedThisTick = true;
  n.prevPos = u->position;
  u->unitStates.erase("ProblemGettingToGoal");
  MotionNavBegin(u);
  if (Dbg())
    Logf(LogLevel::Info, "nav: tick %u unit %u goal (%d,%d) from (%.3f,%.3f) asked (%d,%d)..(%d,%d)", sim.tick(), u->id,
         gc.x, gc.z, u->position.x, u->position.z, x0, z0, x1, z1);
}

void LandNavStop(Sim& sim, Unit* u) {
  LandNav* n = Nav(u);
  if (!n || !n->active) return;
  ResetPathState(sim, u, *n);
  n->active = false;
  n->hasTarget = false;
  u->unitStates.erase("PathFinding");
}

void LandNavSetSpeedThrough(Unit* u, bool on) {
  // CAiNavigatorLand::SetSpeedThroughGoal 0x5a4240 -> CAiSteeringImpl 0x5d2390: the request is stored, and the
  // motion's byte becomes it when the steering's last waypoint lay in the goal (also when the navigator is idle:
  // two patrol points given to standing tanks set it at once); else the byte keeps that waypoint's own flag.
  LandNav* n = Nav(u);
  if (!n) return;
  n->speedThroughGoal = on;
  if (u->motion.wpInGoal) u->motion.passThrough = on;
}

void LandNavPoke(Unit* u) {
  if (LandNav* n = Nav(u)) n->poke = true;
}

bool LandNavActive(const Unit* u) {
  const LandNav* n = u->motion.nav.get();
  return n && n->active;
}

namespace {
struct NavProf {
  double exec = 0, work = 0, deliver = 0, bg = 0;
  uint64_t n = 0;
  ~NavProf() {
    if (getenv("MOHO64_PROFILE"))
      fprintf(stderr, "landnav profile: execute %.2f s, path work %.2f s, deliver %.2f s, background %.2f s (%llu ticks)\n",
              exec, work, deliver, bg, static_cast<unsigned long long>(n));
  }
} g_navProf;
double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

void LandNavTickAll(Sim& sim) {
  static const bool prof = getenv("MOHO64_PROFILE") != nullptr;
  double t0 = prof ? NowS() : 0;
  ++g_navProf.n;
  const std::vector<Unit*> all = sim.ArmyStageOrder();  // (brain+0xa8: creation order inside each army)
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    if (u->motion.air && !u->destroyQueued && !u->dead) {  // (the air navigator, same stage: CAiNavigatorAir::Execute)
      if (!AirNavArrival(sim, u) && !u->parentId) AirFormationFollow(sim, u);
    }
    LandNav* n = Nav(u);
    if (!n || !n->active || u->destroyQueued || u->dead) continue;
    if (n->startedThisTick) {  // (navigators run at the beat start, before the command stage)
      n->startedThisTick = false;
      n->prevPos = u->position;
    }
    SteeringNavStageArmy(u->army ? u->army->index : 0);
    Execute(sim, u, *n);
    SteeringNavStageArmy(0);
    if (n->problem) u->unitStates.insert("ProblemGettingToGoal");
  }
  HPathTables& T = Tables(sim);
  double t1 = prof ? NowS() : 0;
  T.WorkAll(0);
  double t2 = prof ? NowS() : 0;
  // A search that ends the move (state 1, Failed: an empty first path, a continuation blocked after its
  // retries, the same dead end twice) only sets the path state here: OnEvent 0x5aeeb0 writes +0xc = 1 and
  // returns (0x5af004, 0x5af17a, 0x5af1fb; it never calls the broadcaster 0x5a6c50, whose only land caller is
  // Execute 0x5a4409). The move task hears of it from the NEXT beat's Execute (0x5a43d4: state <= 1 -> the
  // steering's stop, MakeIdle, event 0 / 2), so the command thread wakes one beat later (Economy 2006/2007,
  // the oracle's wn lines of unit 0x27: its new goal comes in beat 2007).
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    LandNav* n = Nav(u);
    if (!n || !n->active || !n->pf.delivered) continue;
    OnPath(sim, u, *n);
  }
  if (prof) {
    double t3 = NowS();
    g_navProf.exec += t1 - t0;
    g_navProf.work += t2 - t1;
    g_navProf.deliver += t3 - t2;
  }
}

// Sim::AdvanceBeat step 3 (0x74a078), before the armies: PathTables::UpdateBackground with
// path_BackgroundBudget (1000) when path_BackgroundUpdate is on (default).
void LandNavBackground(Sim& sim) {
  static const bool prof = getenv("MOHO64_PROFILE") != nullptr;
  double t0 = prof ? NowS() : 0;
  Tables(sim).UpdateBackground(1000);
  if (prof) g_navProf.bg += NowS() - t0;
}

// (both answers are COORDS_ToWorldPos 0x50b0f0 of a cell: its footprint centre at the height of the ground there,
// or of the water above it unless the footprint may stand on the seabed: CAiPathNavigator::GetTargetPos 0x5ad9a9,
// CAiNavigatorLand::GetGoalPos 0x5a3dec. Ours answered height 0; the oracle's "wn" lines show the ground's.)
// CAiNavigatorLand::GetCurrentTargetPos 0x5a4260 -> CAiPathNavigator 0x5ad8b0: the path navigator's target cell
// (+0x28 / +0x2a) in the world (COORDS_ToWorldPos 0x50b0f0 with the unit's footprint), or its formation slot while it
// follows one; whatever the status: an idle navigator answers the last target (cell (0, 0) before its first goal).
bool LandNavTarget(const Unit* u, Vec3* out) {
  const LandNav* n = u->motion.nav.get();
  *out = WorldOf(*Sim::From(u->luaState()), u, n ? n->target : PathCell{});
  if (n && n->hasTarget && FollowsSlotPoint(*n)) *out = n->lastFollow;  // (its height too: 0x5ad929)
  return true;
}
bool LandNavGoal(const Unit* u, Vec3* out) {
  const LandNav* n = u->motion.nav.get();
  if (!n || !n->active) return false;
  *out = WorldOf(*Sim::From(u->luaState()), u, PathCell{n->goal[0], n->goal[1]});
  return true;
}
int LandNavStatus(const Unit* u) {
  const LandNav* n = u->motion.nav.get();
  if (!n || !n->active) return 0;
  return n->thinking ? 1 : 2;
}

void LandNavDirty(Sim& sim, int x0, int z0, int x1, int z1) { Tables(sim).DirtyRect(x0, z0, x1, z1); }

void LandNavForget(const Sim& sim) { TablesMap().erase(&sim); }

}  // namespace moho
