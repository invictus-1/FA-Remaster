// Unit motion: how a mobile unit moves from tick to tick (the original's CUnitMotion,
// CAiSteeringImpl and CAiPathSpline), and the per-blueprint physics it reads.
//
// What the original does (FA exe, read 2026-10-08, checked against the oracle probe's per-tick
// trajectories of test units):
// - Land/hover/naval units follow a spline the steering builds a few ticks ahead; each spline
//   step is: turn the facing toward the waypoint by at most TurnRate/10 degrees, remove the
//   sideways part of the velocity (at most MaxSteerForce/100 per tick), then change the speed
//   toward the target speed by at most MaxAcceleration/100 (speeding up) or MaxBrake/100
//   (slowing down, and from a standstill), capped at MaxSpeed/10 per tick.
// - Near the final goal the unit brakes fully once the distance left is below the braking
//   distance v^2/(2*brake). Queued moves are driven through without braking.
// - A move ends when the unit's footprint cell is the goal cell (goal snapped to the cell grid
//   with round-half-even); the unit then coasts: v = 0.8*v - min(|v|, brake), until |v|^2 <= 1e-6.
// - Every tick a moving land unit is placed on the ground: height = mean of the terrain under the
//   four corners of its size box, tilted to the plane of the corners (StandUpright: stays level
//   and sinks by a quarter of the corners' height range).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;
class TerrainMap;

enum MotionType : int {
  kMotionNone = 0,
  kMotionLand,
  kMotionAir,
  kMotionWater,
  kMotionBiped,
  kMotionSurfacingSub,
  kMotionAmphibious,
  kMotionHover,
  kMotionAmphibiousFloating,
  kMotionSpecial,
};

// The physics a unit's motion reads, cached per blueprint.
struct MotionBlueprint {
  int motionType = kMotionNone;
  float maxSpeed = 0, maxSpeedReverse = 0, maxAccel = 0, maxBrake = 0, maxSteerForce = 0;
  float turnRadius = 0, turnRate = 0, turnFacingRate = 0;
  float elevation = 0;
  bool rotateOnSpot = false, standUpright = false, sinkLower = false, rotateBodyWhileMoving = false;
  float rotateOnSpotThreshold = 0.5f;
  float backUpDistance = 0;
  float sizeX = 1, sizeY = 1, sizeZ = 1;
  NamedFootprint footprint;
  bool mobile() const { return motionType != kMotionNone; }
};
// Read once per blueprint (the unit's blueprint table must be the reflected sim table).
const MotionBlueprint& GetMotionBlueprint(lua_State* L, const BlueprintInfo& bp, const SimBlueprints& bps);

// Per-unit motion state.
struct UnitMotion {
  const MotionBlueprint* bp = nullptr;
  Vec3 vel;          // steering velocity (world units per tick)
  Vec3 lastMove;     // what GetVelocity returns: the last tick's displacement
  float fx = 0, fz = 1;  // steering facing (unit vector in xz): the direction it drives
  float bx = 0, bz = 1;  // body facing (= fx, fz unless RotateBodyWhileMoving with a TurnFacingRate)
  bool needSnap = true;  // place on the ground at the next tick (after creation / warp)
  float speedMult = 1, accMult = 1, turnMult = 1;

  // Steering: the waypoints of the current move (the last one is the goal).
  std::vector<Vec3> path;
  size_t pathIndex = 0;
  bool hasGoal = false;
  bool passThrough = false;  // queued move follows: drive through the goal
  int goalCellX = 0, goalCellZ = 0;
  uint32_t driveTick = 0;  // first tick it drives (path search latency; it coasts until then)
  // Spline state (CAiPathSpline::Generate): 3 stopping, 4 slowing down to reconsider, 5 reversing
  // while turning toward the target, 6 slowing down from reversing, 7 driving, 8 done.
  int state = 7;
  bool reverse = false;   // backing up to a close target behind it
  bool newSegment = true; // choose the state from scratch at the next step
  // Collision avoidance (see CollisionTick): stopping for a unit ahead, at twice the brake.
  bool yielding = false;
  uint32_t yieldTarget = 0;  // the unit it stopped for
  bool arrived = false;   // set when the goal cell was reached (consumed by the move command)
  bool failed = false;    // no path
};

// Goal cell of a footprint at a world position (round half to even, as the original).
void GoalCell(const MotionBlueprint& b, float x, float z, int* cx, int* cz);

// One sim tick of a unit's motion (after its commands ran).
void MotionTick(Sim& sim, Unit* u);
// Start moving toward `goal` along `path` (waypoints after the start; last = goal).
// It starts driving at `driveTick` (until then it coasts).
void MotionSetGoal(Sim& sim, Unit* u, const std::vector<Vec3>& path, bool passThrough, uint32_t driveTick);
// Abort the move: the unit coasts to a stop.
void MotionStop(Unit* u);
// Units see each other coming (before motion): a driving unit that would run into a unit ahead
// of it within the next 20 ticks stops (twice its brake) and drives on once the way is clear;
// a unit that drives into an idle one pushes it aside.
// The original (CAiSteeringImpl::CheckCollisions 0x5d3740, ResolvePossibleCollision 0x596f30,
// CUnitMotion::AddImpulse 0x6b8ac0) compares the two units' spline nodes every 3 ticks with
// boxes (SizeX+SizeZ)/4 wide stretched by the stopping distance. TODO(M3b): read it fully; this
// is an approximation fitted to the probe's 3-bot group.
void CollisionTick(Sim& sim);
// Place a unit on the ground/water at its current position (SnapToGround / SnapToWater).
void SnapUnit(const Sim& sim, Unit* u);

// Quaternion helpers shared with the probe-facing bindings.
Quat YawQuat(float fx, float fz);

}  // namespace moho
