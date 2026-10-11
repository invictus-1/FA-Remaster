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
#include <memory>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;
class TerrainMap;
struct AirMotion;
struct LandNav;

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
  float bankingSlope = 0, wobbleFactor = 0, wobbleSpeed = 0;  // Physics +0x3c, +0x48, +0x4c (hover)
  float turnRadius = 0, turnRate = 0, turnFacingRate = 0;
  float elevation = 0;
  bool rotateOnSpot = false, standUpright = false, sinkLower = false, rotateBodyWhileMoving = false;
  float rotateOnSpotThreshold = 0.5f;
  float backUpDistance = 0;
  float sizeX = 1, sizeY = 1, sizeZ = 1;
  float averageDensity = 1;  // AverageDensity (mass = density x SizeX x SizeY x SizeZ)
  bool canFly = false;       // Air.CanFly
  mutable int8_t naval = -1, airCat = -1;  // in category NAVAL / AIR (u+0x68a / u+0x68b; -1: not looked up)
  std::vector<float> raisedPlatforms;  // Physics.RaisedPlatforms: quads of (x, z, h) x 4 from the position
  NamedFootprint footprint;
  // the footprint's passability grid (cached; grids live as long as the sim's Navigation)
  mutable const class PathGrid* grid = nullptr;
  mutable const void* gridOwner = nullptr;
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
  Vec3 prevPos;          // m+0x2c: where the last move ended (a spline point: its terrain y); vel = pos - prevPos
  bool prevPosSet = false;
  Vec3 accel;            // m+0x44: this tick's change of vel (spline-point moves; 0 when blocked)
  // CalcMoveHover 0x6c2bc0: the hover's lean (m+0xc0) and wobble (target m+0xf0, velocity m+0xe4, offset
  // m+0xd8); lean and offset are added to the ground normal before the orientation is built
  Vec3 hoverLean, wobbleTarget, wobbleVel, wobble;
  float speedMult = 1, accMult = 1, turnMult = 1;
  // fuel (fuel.md): m+0x0c use time, Physics.FuelRechargeRate, the platform refuel/repair state
  float fuelUseTime = 0, fuelRecharge = 0;
  bool refuelFlag = false;
  std::shared_ptr<struct EconRequest> repairRequest;
  float repairReq[2] = {0, 0};  // energy, mass
  float maxSpeedU594 = 0;  // u+0x594 as UpdateInfoCache last wrote it (recorder)
  float speedCap = 0;  // > 0: the formation-capped top speed u+0x594 (world units/s, speedMult included)

  // Steering: the waypoints of the current move (the last one is the goal).
  std::vector<Vec3> path;
  size_t pathIndex = 0;
  bool hasGoal = false;
  bool passThrough = false;  // queued move follows: drive through the goal
  bool airSpeedThrough = false;  // aircraft: navigator.SetSpeedThroughGoal (motion+0x8c), set only on events
  int goalCellX = 0, goalCellZ = 0;
  uint32_t driveTick = 0;  // first tick it drives (path search latency; it coasts until then)
  // Spline state (CAiPathSpline::Generate): 3 stopping, 4 slowing down to reconsider, 5 reversing
  // while turning toward the target, 6 slowing down from reversing, 7 driving, 8 done.
  int state = 7;
  bool reverse = false;   // backing up to a close target behind it
  bool newSegment = true; // choose the state from scratch at the next step
  // CAiPathSpline: the steering generates its points in batches (sim/steering.cpp); each tick the
  // steering hands the motion the next point and the motion moves the unit onto it.
  struct SplinePoint {
    Vec3 pos;              // xz (y: the unit's height when generated)
    Vec3 vel;              // per tick
    float fx, fz, bx, bz;  // steering and body facing after the step
  };
  std::vector<SplinePoint> spline;
  size_t splineIdx = 0;    // the next point to hand out (spline+0x240)
  bool hasSpline = false;
  int splineMode = 0;      // Generate/Update mode of the batch (spline+0x248): 0 stop, 1 through, 2 side-step, 4 brake
  int savedState = 0;      // spline+0x270: the state a continuation batch starts in (0: 7)
  SplinePoint genLast{};   // the generator after the last point (continuation start)
  bool genReverse = false;
  Vec3 steerTarget;        // st+0x90: the current waypoint target
  bool pointNow = false;   // a point was handed this tick (motion+4)
  uint32_t handOutTick = 0xffffffffu;  // a point was handed outside the steering stage (SetWaypoints(0,0))
  SplinePoint point{};
  // SCollisionInfo (st+0x64): the unit it expects to meet, where, and when; type 0 none, 1 predicted,
  // 2 side-step, 4 brake and wait, 5 brake for a side-stepper
  int colType = 0;
  uint32_t colUnit = 0;
  Vec3 colPos;
  uint32_t colTick = 0xffffffffu;
  Vec3 sideStep;           // st+0x84
  bool surfaceNext = false;  // m+0x90: process contacts next tick (set by AddImpulse)
  bool arrived = false;   // set when the goal cell was reached (consumed by the move command)
  bool failed = false;    // no path
  bool ballistic = false;  // dropped from a transport: falls (sim/transport.cpp)
  std::shared_ptr<AirMotion> air;  // aircraft: the flight model's state (sim/air.cpp)
  // Land navigator (sim/landnav.cpp): it decides the waypoint and when a move ends.
  std::shared_ptr<LandNav> nav;
  bool navDriven = false;    // the current move belongs to the navigator
  bool hasWaypoint = false;  // the steering has a waypoint (else it coasts)
  // CalcMoveCommon's hard rule: a step from a cell the footprint fits into one it does not is
  // undone and the unit is pushed back a little; while pushed it coasts.
  bool pushed = false, wasMoving = false;
};

// The passability grid of a motion blueprint's footprint (cached).
const class PathGrid* FootprintGrid(Sim& sim, const MotionBlueprint& b);
// Goal cell of a footprint at a world position (round half to even, as the original).
void GoalCell(const MotionBlueprint& b, float x, float z, int* cx, int* cz);

// One sim tick of a unit's motion (after its commands ran).
void MotionTick(Sim& sim, Unit* u);
// Start moving toward `goal` along `path` (waypoints after the start; last = goal).
// It starts driving at `driveTick` (until then it coasts).
void MotionSetGoal(Sim& sim, Unit* u, const std::vector<Vec3>& path, bool passThrough, uint32_t driveTick);
// Abort the move: the unit coasts to a stop.
void MotionStop(Unit* u);
// Unit::GetVelocity 0x6a9c90 (entity vtbl+0x3c): a unit with a land motion not on the Air layer reports m+0x38,
// otherwise the last tick's displacement
const Vec3& UnitVelocity(const Unit* u);
// CAiSteeringImpl::SetWaypoints(nullptr, 0) 0x5d29c0: no waypoint; a unit still on a spline gets a brake spline
// and its first point at once (engine-ref move_handoff.md).
void SteeringClearWaypoints(Sim& sim, Unit* u);
// Navigator interface (sim/landnav.cpp): a move starts (no waypoint yet: it coasts), the steering
// gets a waypoint (through: drive through it, else brake to it), the move ended.
void MotionNavBegin(Unit* u);
void MotionSetWaypoint(Sim& sim, Unit* u, const Vec3& p, bool through);
void MotionNavDone(Unit* u, bool succeeded);
// The steering stage (CAiSteeringImpl::OnTick, before motion; sim/steering.cpp): new waypoints,
// spline batches, push ends, predicted collisions and their reactions (engine-ref unit_collision.md).
void SteeringTickAll(Sim& sim);
// Place a unit on the ground/water at its current position (SnapToGround / SnapToWater).
void SnapUnit(const Sim& sim, Unit* u);

// Quaternion helpers shared with the probe-facing bindings.
Quat YawQuat(float fx, float fz);

}  // namespace moho
