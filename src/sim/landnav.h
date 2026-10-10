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
  bool speedThroughGoal = false;
  bool thinking = true;   // navigator status Thinking: the first waypoint brakes
  bool problem = false;   // ProblemGettingToGoal
  bool startedThisTick = false;
  Vec3 prevPos;
  bool targetChanged = false;
  // formation following (formations.md 9): +0x90 inFormation, +0x91 waiting for the leader,
  // +0x92 following the slot, +0x7c cached leader, +0x84 last followed slot, +0x78 last failed follow
  bool inFormation = false, waiting = false, following = false;
  uint32_t cachedLeader = 0;
  Vec3 lastFollow;
  PathCell followCell;
  Vec3 followPos;  // following: the slot position the target was taken from (the steering drives to it)
  uint32_t lastFail = 0;
};

// Start a move to world position `goal` (land units). speedThrough: drive through the goal.
void LandNavSetGoal(Sim& sim, Unit* u, const Vec3& goal, bool speedThrough);
// The unit's move is aborted: the navigator forgets its goal (no event).
void LandNavStop(Sim& sim, Unit* u);
void LandNavSetSpeedThrough(Unit* u, bool on);
// Per tick, after the units moved: every navigator's Execute, then the armies' path queues.
void LandNavTickAll(Sim& sim);
// The steering's push ended (Func1 0x5a3e80).
void LandNavPoke(Unit* u);
bool LandNavActive(const Unit* u);
// Lua navigator queries: the current target / goal (world), status 0 idle 1 thinking 2 steering.
bool LandNavTarget(const Unit* u, Vec3* out);
bool LandNavGoal(const Unit* u, Vec3* out);
int LandNavStatus(const Unit* u);
// Structures changed occupancy in cells [x0, x1) x [z0, z1).
void LandNavDirty(Sim& sim, int x0, int z0, int x1, int z1);
// Can the unit's footprint stand at world (x, z)? (Unit::WontFitAt 0x62aa90, negated)
bool UnitFitsAt(Sim& sim, const Unit* u, float x, float z);
// FootprintFits at a cell (terrain and structures).
bool LandCellFits(Sim& sim, const Unit* u, int x, int z);
// The path navigator follows its formation slot (state 6): the land navigator's FollowingLeader.
bool LandNavFollowingSlot(const Unit* u);

}  // namespace moho
