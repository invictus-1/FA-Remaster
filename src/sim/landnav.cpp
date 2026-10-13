// The land navigator (see landnav.h). Function names and addresses refer to the FA exe;
// engine-ref/specs/pathfinding.md and land_motion_blocking.md give the details.
#include "sim/landnav.h"
#include "sim/collision.h"
#include "sim/steering.h"
#include "sim/formation.h"
#include "sim/air.h"
#include "sim/commands.h"

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
const NamedFootprint& Fp(const Unit* u) { return u->motion.bp->footprint; }
int SX(const Unit* u) { return std::max<int>(1, Fp(u).sizeX); }
int SZ(const Unit* u) { return std::max<int>(1, Fp(u).sizeZ); }

PathCell CellOf(const Unit* u, float x, float z) {
  return {static_cast<int>(std::nearbyint(x - SX(u) * 0.5f)), static_cast<int>(std::nearbyint(z - SZ(u) * 0.5f))};
}

const PathGrid* Grid(Sim& sim, const Unit* u) { return FootprintGrid(sim, *u->motion.bp); }

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
    // (the gather asks the entity grid first: a unit whose box only touches the square on a multiple of 4 is in
    // other cells and is not found at all)
    if (!GridCellsMeet(Vec3{x0, -1000.0f, z0}, Vec3{x1, 1000.0f, z1}, mn, mx)) return;
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
                       if (!ShapeOverlapsOBox(s, centre, axes, half)) return;
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

void Pop(LandNav& n, int k) {
  int m = std::min<int>(k, static_cast<int>(n.path.size()));
  if (m > 0) n.path.erase(n.path.begin(), n.path.begin() + m);
  if (n.path.empty()) n.wait = 0;
  n.spliced -= k;
  if (n.spliced < -1) n.spliced = -1;
}

bool InGoal(const LandNav& n, const PathCell& c) {
  return n.goal[0] <= c.x && c.x < n.goal[2] && n.goal[1] <= c.z && c.z < n.goal[3];
}

// QueueSearch 0x5aa310
void QueueSearch(Sim& sim, Unit* u, LandNav& n) {
  PathTraveler& t = n.pf;
  t.unit = u;
  t.fp = Fp(u);
  t.S = std::max(SX(u), SZ(u));
  t.armyIndex = u->army ? u->army->index : 0;
  PathCell c = CellOf(u, u->position.x, u->position.z);
  t.startStandable = Fits(sim, u, c);
  const TerrainMap* map = sim.map();
  int w = map ? map->width() : 0, h = map ? map->height() : 0;
  float m = static_cast<float>(t.S);
  t.insidePlayable = u->position.x - m >= 0 && u->position.z - m >= 0 && u->position.x + m <= w &&
                     u->position.z + m <= h;
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

// RequestPath 0x5adfe0
void RequestPath(Sim& sim, Unit* u, LandNav& n, int mode) {
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
  n.path.clear();
  n.state = 0;
  n.wait = 0;
  // 0x5ada19: the unit's PathFinding and ProblemGettingToGoal states go with the path (a search cut short by a
  // new goal does not leave PathFinding standing)
  u->unitStates.erase("PathFinding");
  u->unitStates.erase("ProblemGettingToGoal");
  // (0x5ad9c0 leaves the splice count +0x5c alone: only ConsumePathPrefix 0x5ad830 and the splice itself write
  // it, so a move dropped with spliced cells ahead hands the count to the next goal's first waypoints)
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
    if (n.path.empty()) {
      n.state = 1;
      n.problem = true;
      return;
    }
    const PathCell& last = n.path.back();
    if (!InGoal(n, last)) {
      n.problem = true;
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
  n.targetChanged = true;
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
      n.targetChanged = true;
    }
    n.waiting = true;
    return 0;
  }
  n.waiting = false;
  const Vec3 p = u->formSlot;
  PathCell s = CellOf(u, p.x, p.z);
  if (u->id % 13 != sim.tick() % 13 && !IsZeroVec(n.lastFollow)) return 1;  // between phases: keep following
  if (n.hasTarget && n.target == s) {
    n.lastFollow = p;
    // the target position is the slot's (+0x84): a new position in the same cell is a new steering waypoint
    if (n.following && n.followCell == s && (n.followPos.x != p.x || n.followPos.z != p.z)) {
      n.followPos = p;
      n.targetChanged = true;
    }
    return 1;
  }
  float dd = Dist(n.cur, s);
  float step = std::min(dd, 10.0f);
  PathCell probe = n.cur;
  if (dd > 0) {
    probe = {n.cur.x + static_cast<int>((s.x - n.cur.x) * step / dd), n.cur.z + static_cast<int>((s.z - n.cur.z) * step / dd)};
  }
  const TerrainMap* map = sim.map();
  bool within = !map || (p.x - 1.0f >= 0 && p.z - 1.0f >= 0 && p.x + 1.0f <= map->width() && p.z + 1.0f <= map->height());
  if (Reach(sim, u, n.cur, probe) && UnitClearTo(sim, u, n, probe) && within) {
    n.advanceDist = 0.5f * dd;
    n.target = s;
    n.hasTarget = true;
    n.targetChanged = true;
    n.following = true;
    n.lastFollow = p;
    n.followPos = p;
    n.followCell = s;
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
  n.cur = CellOf(u, u->position.x, u->position.z);
  if (n.thinkDelay > 0) {
    if (--n.thinkDelay == 0) RequestPath(sim, u, n, n.requestMode);
    if (!n.hasTarget || n.target != n.cur) n.targetChanged = true;  // (0x5ae37f) the target is the own cell
    n.target = n.cur;
    n.hasTarget = true;
    return;
  }
  if (n.wait > 0) {
    if (--n.wait == 0) RequestContinuation(sim, u, n, 2);
    return;
  }
  while (n.path.size() > 1 && Dist(n.path[0], n.cur) >= Dist(n.path[1], n.cur) && UnitClearTo(sim, u, n, n.path[1]))
    Pop(n, 1);
  if (n.state != 5 && n.state != 6) return;
  if (!n.path.empty() && n.cur == n.path.back()) {
    n.state = 0;
    return;
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
  if (n.stuck > 30) {
    n.state = 0;
    return;
  }
  if (n.poke) {
    n.poke = false;
    RequestContinuation(sim, u, n, 3);
    return;
  }
  if (!n.path.empty() && n.path.back() == n.target && !u->unitStates.count("WaitingForTransport") &&
      PathUnitBlocked(u, n.target.x, n.target.z, 1)) {
    n.wait = 10;
    return;
  }
  RequestContinuation(sim, u, n, 2);
}

// Execute 0x5a4280: the steering gets the new waypoint; a finished path ends the move
void Execute(Sim& sim, Unit* u, LandNav& n) {
  n.targetChanged = false;
  UpdateCurrentPosition(sim, u, n);
  n.prevPos = u->position;
  // 0x5a4342: while the goal still waits for its search (path state 2: a formation's path delay, the thinking
  // beat) the steering is told nothing; the navigator stays "thinking" and hands the target over afterwards
  if (n.targetChanged && n.active && n.state == 2) n.targetPending = true;
  if ((n.targetChanged || n.targetPending) && n.active && n.state != 2) {
    n.targetPending = false;
    bool inGoal = InGoal(n, n.target);
    bool f9e = n.thinking ? false : n.spliced < 0;
    bool through = inGoal ? n.speedThroughGoal : f9e;
    if (Dbg())
      Logf(LogLevel::Info, "nav: tick %u unit %u through: in goal %d (through goal %d) thinking %d spliced %d", sim.tick(), u->id,
           inGoal ? 1 : 0, n.speedThroughGoal ? 1 : 0, n.thinking ? 1 : 0, n.spliced);
    n.thinking = false;
    // following a formation slot: the target position is the slot's own (not the cell centre)
    Vec3 wp = (n.following && n.target == n.followCell) ? Vec3{n.followPos.x, WorldOf(sim, u, n.target).y, n.followPos.z} : WorldOf(sim, u, n.target);
    // the steering gets it only when it differs from its last waypoint (GetWaypoint 0x5d2170): the same
    // point re-set by TryAdvance makes no new batch
    const UnitMotion& m = u->motion;
    bool same = m.hasWaypoint && !m.path.empty() && m.path.back().x == wp.x && m.path.back().y == wp.y && m.path.back().z == wp.z;
    if (!same) {
      MotionSetWaypoint(sim, u, wp, through);
      u->motion.wpInGoal = inGoal;
    }
    if (Dbg())
      Logf(LogLevel::Info, "nav: tick %u unit %u target (%d,%d) through %d cur (%d,%d) path %zu", sim.tick(), u->id,
           n.target.x, n.target.z, through ? 1 : 0, n.cur.x, n.cur.z, n.path.size());
  }
  if (n.state < 2) {
    bool ok = n.state == 0;
    if (Dbg()) Logf(LogLevel::Info, "nav: tick %u unit %u %s", sim.tick(), u->id, ok ? "succeeded" : "failed");
    n.active = false;
    n.hasTarget = false;  // (a navigator that went idle starts its next goal with a target change)
    Tables(sim).Cancel(&n.pf);
    u->unitStates.erase("PathFinding");
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
  if (u->blueprint && u->blueprint->hasFootprint && (u->blueprint->footprint.caps & 0x10)) return true;
  return Fits(sim, u, CellOf(u, x, z));
}

// The rings of the free-spot search 0x62b200 around a cell that does not fit: square rings stepping by the
// footprint size, up to 900 candidates. true: *gc is the first cell where the unit fits; false: *gc unchanged.
static bool FreeSpotRings(Sim& sim, const Unit* u, PathCell* gc) {
  int step = std::max(SX(u), SZ(u));
  int tested = 0;
  bool found = false;
  const TerrainMap* map = sim.map();
  float W = map ? static_cast<float>(map->width()) : 1e9f, H = map ? static_cast<float>(map->height()) : 1e9f;
  Vec3 base = WorldOf(sim, u, *gc);
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

// FindFreeSpot 0x62b200 for a point: where the unit's footprint fits, at the point or on the rings around it.
bool LandFreeSpot(Sim& sim, const Unit* u, Vec3* pos) {
  PathCell c = CellOf(u, pos->x, pos->z);
  if (!Fits(sim, u, c) && !FreeSpotRings(sim, u, &c)) return false;
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
  u->motion.faceDir = {};  // (CUnitMotion::SetTarget 0x6b8872 takes the goal's facing: none)
  if (!u->motion.nav) u->motion.nav = std::make_shared<LandNav>();
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
  if (single && !Fits(sim, u, gc)) FreeSpotRings(sim, u, &gc);
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
  n.targetPending = false;
  n.problem = false;
  n.retries = n.replans = 0;
  n.hasDeadEnd = false;
  n.stuck = 0;
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
    LandNav* n = Nav(u);
    if (!n || !n->active || u->destroyQueued || u->dead) continue;
    if (n->startedThisTick) {  // (navigators run at the beat start, before the command stage)
      n->startedThisTick = false;
      n->prevPos = u->position;
    }
    Execute(sim, u, *n);
    if (n->problem) u->unitStates.insert("ProblemGettingToGoal");
  }
  HPathTables& T = Tables(sim);
  double t1 = prof ? NowS() : 0;
  T.WorkAll(0);
  double t2 = prof ? NowS() : 0;
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    LandNav* n = Nav(u);
    if (!n || !n->active || !n->pf.delivered) continue;
    OnPath(sim, u, *n);
    if (n->state < 2) {
      n->active = false;
      n->hasTarget = false;
      u->unitStates.erase("PathFinding");
      MotionNavDone(u, n->state == 0);
    }
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

bool LandNavTarget(const Unit* u, Vec3* out) {
  const LandNav* n = u->motion.nav.get();
  if (!n || !n->hasTarget) return false;  // (the last target stays readable after the move ended)
  *out = {n->target.x + SX(u) * 0.5f, 0, n->target.z + SZ(u) * 0.5f};
  if (n->following && n->target == n->followCell) *out = {n->followPos.x, 0, n->followPos.z};
  return true;
}
bool LandNavGoal(const Unit* u, Vec3* out) {
  const LandNav* n = u->motion.nav.get();
  if (!n || !n->active) return false;
  *out = {n->goal[0] + SX(u) * 0.5f, 0, n->goal[1] + SZ(u) * 0.5f};
  return true;
}
int LandNavStatus(const Unit* u) {
  const LandNav* n = u->motion.nav.get();
  if (!n || !n->active) return 0;
  return n->thinking ? 1 : 2;
}

void LandNavDirty(Sim& sim, int x0, int z0, int x1, int z1) { Tables(sim).DirtyRect(x0, z0, x1, z1); }

}  // namespace moho
