// The land navigator (the original's CAiNavigatorLand + CAiPathNavigator, FA exe, read
// 2026-10-09; engine-ref/specs/pathfinding.md section 4-7, land_motion_blocking.md):
// - SetGoal: the goal cell (moved by the free-spot spiral when the footprint does not fit there),
//   one tick of "thinking", then a search in the army's path queue.
// - The delivered path is a list of cells (with jumps between cluster boundary cells). Each tick
//   the navigator pops points it has passed and, when the unit has come half way to its current
//   target (or a periodic re-check says the way is blocked), advances the target: the farthest of
//   the next 10 path points within 50 cells that a corridor test reaches (TryAdvanceTargetPoint).
//   When none is reachable it searches again from where it is to the next point (a
//   "continuation") and splices the result in; repeated failures re-plan or fail the move.
// - The steering drives to the target; it brakes at the first waypoint after a goal, at spliced
//   continuation points and at the goal (unless a queued move follows), and passes through others.
// - A move succeeds when the unit stands in the last path cell, or when it has not moved for 31
//   ticks; it fails after the retry rules give up.
// - A unit standing where its footprint does not fit gets a terrain-blind path near its start
//   and follows it unchecked until it reaches a cell where it fits.
#pragma once
#include <memory>
#include <vector>

#include "sim/entity.h"
#include "sim/hpath.h"

namespace moho {

class Sim;
class Unit;

// MOHO64_PAR_NAV (sim/landnav.cpp): a unit test of a navigator's turn played ahead: kind 0 UnitInLine (a = ax, az,
// bx, bz), 1 PathUnitBlocked (a = x, z), its flags and its answer
struct NavQuery {
  uint8_t kind = 0, flags = 0;
  float a[4] = {0, 0, 0, 0};
  bool result = false;
};
struct NavAhead {
  std::vector<NavQuery> q;
};

struct LandNav {
  PathTraveler pf;
  bool active = false;   // a goal is being worked on
  int state = 0;         // pathnav: 0 done, 1 failed, 2 thinking, 3 first path, 4 continuation, 5 following
  int goal[4] = {0, 0, 0, 0};
  std::vector<PathCell> path;
  PathCell cur, target;
  bool hasTarget = false;
  float advanceDist = 0;  // +0x6c
  int thinkDelay = 0, wait = 0, stuck = 0, retries = 0, replans = 0;
  int spliced = -1;       // +0x5c
  bool fits = false;      // +0x93: the unit's cell fits and is clear
  bool poke = false;      // +0x94: the steering reported a push
  bool attackVariant = false;  // +0x95
  bool adjacent = false;  // +0x96
  bool hasDeadEnd = false;
  PathCell deadEnd;       // +0x2c
  uint32_t lastAdvance = 0;
  int requestMode = 0;    // +0x98
  int pathLayer = 0;      // +0x54: the unit's layer as its navigator last took it (ctor 0x5ad513, 0x5ae4d3)
  bool speedThroughGoal = false;
  bool thinking = true;   // navigator status Thinking: the first waypoint brakes
  bool problem = false;   // ProblemGettingToGoal
  bool startedThisTick = false;
  Vec3 prevPos;
  // formation following (formations.md 9): +0x90 inFormation, +0x91 waiting for the leader,
  // +0x92 following the slot, +0x7c cached leader, +0x84 last followed slot, +0x78 last failed follow
  bool inFormation = false, waiting = false, following = false;
  uint32_t cachedLeader = 0;
  Vec3 lastFollow;  // +0x84: while +0x92 is set and this is not zero, it is the target position (GetTargetPos 0x5ad8b0)
  uint32_t lastFail = 0;
  // (unit+0x124, the AltFootprint flag of UpdateWaterFavorAltFootprintMode 0x5adc70, is the unit's own:
  // Unit::altFootprintNav)
};

// Start a move to world position `goal` (land units). speedThrough: drive through the goal.
void LandNavSetGoal(Sim& sim, Unit* u, const Vec3& goal, bool speedThrough);
// FindFreeSpot 0x62b200: the nearest place to *pos where the unit's footprint fits (false: none found).
bool LandFreeSpot(Sim& sim, const Unit* u, Vec3* pos);
// The path tables' background cluster build (beat start, before the armies).
void LandNavBackground(Sim& sim);
// The goal as a rect of cells [x0, x1) x [z0, z1) (a 1x1 goal moves to a free spot nearby).
void LandNavSetGoalRect(Sim& sim, Unit* u, int x0, int z0, int x1, int z1, bool speedThrough);
// The unit's move is aborted: the navigator forgets its goal (no event).
void LandNavStop(Sim& sim, Unit* u);
void LandNavSetSpeedThrough(Unit* u, bool on);
// Per tick, the navigators' stages. CArmyImpl::OnTick 0x6ffd70 runs each army's brain stages in turn, army by army
// (0x6ffe83..0x6ffe9f: navigators brain+0xa8, steering +0xac, attackers +0xb0), so an army's navigators come after
// the armies before it have steered and fired: LandNavStageBegin once, LandNavTickUnits with each army's units in
// its turn (Sim::Tick), then LandNavStageEnd (the path queues' work and what it delivered).
void LandNavStageBegin(Sim& sim);
void LandNavTickUnits(Sim& sim, const std::vector<Unit*>& units);
void LandNavStageEnd(Sim& sim);
// The steering's push ended (Func1 0x5a3e80).
void LandNavPoke(Unit* u);
bool LandNavActive(const Unit* u);
// Lua navigator queries: the current target / goal (world), status 0 idle 1 thinking 2 steering.
bool LandNavTarget(const Unit* u, Vec3* out);
bool LandNavGoal(const Unit* u, Vec3* out);
int LandNavStatus(const Unit* u);
// Structures changed occupancy in cells [x0, x1) x [z0, z1).
void LandNavDirty(Sim& sim, int x0, int z0, int x1, int z1);
// Before the occupancy (cells [x0, x1) x [z0, z1)) or the heights (vertices [vx0, vx1] x [vz0, vz1]) change: the
// cluster data the background paid for is made from the grids as they still are (HPathTables::BeforeOccupancyChange).
void LandNavBeforeOccupancy(Sim& sim, int x0, int z0, int x1, int z1);
void LandNavBeforeTerrain(Sim& sim, int vx0, int vz0, int vx1, int vz1);
// The heightfield vertices [vx0, vx1] x [vz0, vz1] changed (FlattenMapRect): the path tables drop the cluster data
// they made ahead there (HPathTables::TerrainChanged).
void LandNavTerrainChanged(Sim& sim, int vx0, int vz0, int vx1, int vz1);
// Can the unit's footprint stand at world (x, z)? (Unit::WontFitAt 0x62aa90, negated)
bool UnitFitsAt(Sim& sim, const Unit* u, float x, float z);
// FootprintFits at a cell (terrain and structures).
bool LandCellFits(Sim& sim, const Unit* u, int x, int z);
// The path navigator follows its formation slot (state 6): the land navigator's FollowingLeader.
bool LandNavFollowingSlot(const Unit* u);
// Drops the sim's path tables (Sim::~Sim: they point into this sim's navigation grids) and prints its profile
// (MOHO64_PROFILE).
void LandNavForget(Sim& sim);

}  // namespace moho
