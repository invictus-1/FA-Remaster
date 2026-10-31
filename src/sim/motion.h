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
  float rollStability = 0, rollDamping = 0;  // Physics +0x40, +0x44 (registered 0x520be3 / 0x520bfb): CalcRollHack
  float turnRadius = 0, turnRate = 0, turnFacingRate = 0;
  float elevation = 0;
  float diveSurfaceSpeed = 1;         // Physics.DiveSurfaceSpeed (P+0x24)
  float layerChangeOffset = -0.1f;    // Physics.LayerChangeOffsetHeight (P+0x74)
  float layerTransitionDuration = 0;  // Physics.LayerTransitionDuration (P+0x78), seconds: the layer blend's length
  bool rotateOnSpot = false, standUpright = false, sinkLower = false, rotateBodyWhileMoving = false;
  float rotateOnSpotThreshold = 0.5f;
  float backUpDistance = 0;
  float sizeX = 1, sizeY = 1, sizeZ = 1;
  float averageDensity = 1;  // AverageDensity (mass = density x SizeX x SizeY x SizeZ)
  bool canFly = false;       // Air.CanFly
  mutable int8_t naval = -1, airCat = -1;  // in category NAVAL / AIR (u+0x68a / u+0x68b; -1: not looked up)
  std::vector<float> raisedPlatforms;  // Physics.RaisedPlatforms: quads of (x, z, h) x 4 from the position
  NamedFootprint footprint;
  NamedFootprint altFootprint;  // the named AltFootprint (bp+0x300): a FAVORSWATER unit's water-only paths
  // the footprints' passability grids (cached; grids live as long as the sim's Navigation)
  mutable const class PathGrid* grid = nullptr;
  mutable const void* gridOwner = nullptr;
  mutable const class PathGrid* altGrid = nullptr;
  mutable const void* altGridOwner = nullptr;
  bool mobile() const { return motionType != kMotionNone; }
};
// Read once per blueprint (the unit's blueprint table must be the reflected sim table).
const MotionBlueprint& GetMotionBlueprint(lua_State* L, const BlueprintInfo& bp, const SimBlueprints& bps);

// Per-unit motion state.
struct UnitMotion {
  const MotionBlueprint* bp = nullptr;
  Vec3 vel;          // steering velocity (world units per tick)
  Vec3 lastMove;     // the displacement of its last motion tick
  Vec3 velCommitted; // lastMove as of the last AdvanceCoords (step 13): what GetVelocity returns in the air
  float fx = 0, fz = 1;  // steering facing (unit vector in xz): the direction it drives
  float bx = 0, bz = 1;  // body facing (= fx, fz unless RotateBodyWhileMoving with a TurnFacingRate)
  bool needSnap = true;  // place on the ground at the next tick (after creation / warp)
  // A change between Water and Land of a unit with Physics.LayerTransitionDuration (the Cybran destroyer that
  // walks ashore): m+0x11c the blend's beat counter (0: none), m+0x120 the transform it began with, m+0x13c the
  // one it ends on. While it runs the unit is moved along the blend and takes no spline point (motion.cpp).
  Quat turnQ;              // the hull's quaternion after this beat's turn on the spot (ArrivalTurn)
  bool heldLatch = false;  // steering+0xa0: the spline was dropped because the unit is immobile or stunned
  float layerBlend = 0;
  Vec3 blendFromPos, blendToPos;
  Quat blendFromOri, blendToOri;
  bool snapTerrain = false;  // (SnapUnit for the blend's end: the ground itself, also for a floating type)
  Vec3 prevPos;          // m+0x2c: where the last move ended (a spline point: its terrain y); vel = pos - prevPos
  bool prevPosSet = false;
  Vec3 accel;            // m+0x44: this tick's change of vel (spline-point moves; 0 when blocked)
  // CalcMoveHover 0x6c2bc0: the hover's lean (m+0xc0) and wobble (target m+0xf0, velocity m+0xe4, offset
  // m+0xd8); lean and offset are added to the ground normal before the orientation is built
  Vec3 hoverLean, wobbleTarget, wobbleVel, wobble;
  float speedMult = 1, accMult = 1, turnMult = 1;
  // the attack run's break-off multipliers (attributes +0x40 / +0x44; Unit:SetBreakOffTriggerMult / DistanceMult):
  // a bomber's script raises them while it drops, so that it turns away after the run
  float breakOffTriggerMult = 1, breakOffDistanceMult = 1;
  // fuel (fuel.md): m+0x0c use time, Physics.FuelRechargeRate, the platform refuel/repair state
  float fuelUseTime = 0, fuelRecharge = 0;
  bool refuelFlag = false;
  std::shared_ptr<struct EconRequest> repairRequest;
  float repairReq[2] = {0, 0};  // energy, mass
  float maxSpeedU594 = 0;  // u+0x594 as UpdateInfoCache last wrote it (0 until the unit's first MotionTick)
  float speedCap = 0;  // > 0: the formation-capped top speed u+0x594 (world units/s, speedMult included)

  // Steering: the waypoints of the current move (the last one is the goal).
  std::vector<Vec3> path;
  size_t pathIndex = 0;
  bool hasGoal = false;
  bool passThrough = false;  // queued move follows: drive through the goal
  // CAiSteeringImpl+0x9f: the navigator's last waypoint lay in the goal. SetSpeedThrough 0x5d2390 makes the
  // drive-through byte (motion+0x8c) `wpInGoal ? the requested value : the waypoint's own flag (+0x9e)`.
  bool wpInGoal = false;
  Vec3 brakeDir;  // the hull's forward axis when the current brake spline was made (its points' direction)
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
    // The point's state word (+0x18 of the exe's 0x1c-byte point; 7 by default, 0x5b1f4d): a brake spline's points
    // (CAiPathSpline::Update 0x5b26c0) carry 1, or 2 when it brakes a backward motion (0x5b299d), the last one 8
    // (0x5b2ec3); a batch's (Generate 0x5b2ff0) the state it was made in (0x5b41e2..0x5b41e6).
    int state = 7;
  };
  std::vector<SplinePoint> spline;
  size_t splineIdx = 0;    // the next point to hand out (spline+0x240)
  bool hasSpline = false;
  int splineMode = 0;      // Generate/Update mode of the batch (spline+0x248): 0 stop, 1 through, 2 side-step, 4 brake
  int savedState = 0;      // spline+0x270: the state a continuation batch starts in (0: 7)
  SplinePoint genLast{};   // the generator after the last point (continuation start)
  bool genReverse = false;
  Vec3 steerTarget;        // st+0x90: the current waypoint target
  // CUnitMotion's target m+0x14 (land, hover, naval): what CUnitMotion::SetTarget 0x6b85e0 last wrote (the steering's
  // waypoint when it takes one up, 0x5d310e, or a collision record's target, 0x5d2f70 / 0x5d2ebf; the place after a
  // Warp, 0x6b9421, or a detach, 0x6b9654) or CUnitMotion::Stop 0x6b8460 (the unit's position, or 100 ahead along its
  // velocity when it drives through, 0x6b84c6..0x6b8564). ProcessCommonMotionState measures to it (0x6c2b18).
  Vec3 motionTarget;
  // st+0x90 holds a waypoint (not the "no target" vector the ctor and SetWaypoints(0, 0) write there, 0x5d2abb):
  // set when DriveToNextWaypoint takes a waypoint up (0x5d30a6). ProcessSplineMovement asks this, not whether
  // the navigator has handed a new waypoint since.
  bool steerValid = false;
  bool pointNow = false;   // a point was handed this tick (motion+4)
  uint32_t handOutTick = 0xffffffffu;  // a point was handed outside the steering stage (SetWaypoints(0,0))
  uint32_t stopCount = 0;  // MotionStop calls (AbortMove), to tell whether a teardown already made one
  // The beat a collision record was written OUTSIDE the steering stage (a stop in the command or script stage):
  // the original's steering stage has already run by then, so the record is first looked at a beat later.
  uint32_t colLateTick = 0xffffffffu;
  SplinePoint point{};
  // SCollisionInfo (st+0x64): the unit it expects to meet, where, and when; type 0 none, 1 predicted,
  // 2 side-step, 4 brake and wait, 5 brake for a side-stepper
  int colType = 0;
  uint32_t colUnit = 0;
  Vec3 colPos;
  uint32_t colTick = 0xffffffffu;
  Vec3 sideStep;           // st+0x84
  bool surfaceNext = false;  // m+0x90: process contacts next tick (set by AddImpulse)
  // This beat's MotionTick ended without CUnitMotion::MoveTo 0x6b92e0 (so without its normalising of the quaternion):
  // state 3 at rest (0x6ba1ca), or held, immobile / stunned, unless a layer blend frame is applied (0x6ba00d..0x6ba065)
  bool noMoveTo = false;
  // m+0x178: what the last contact pass found under the unit's box (EntityRefs; emptied when a pass starts, also one
  // that stops early), and m+0x114: the raised platform the last search picked from it (SnapToGround stands on it)
  std::vector<uint32_t> contacts;
  uint32_t platformRef = 0;
  // An aircraft let go by its transport keeps its layer until its own motion step of that beat (CUnitMotion::
  // MotionTick 0x6ba128 sets Air there): set by DetachUnitFrom, taken up by AirMotionTick
  bool airLayerPending = false;
  // CalcMoveWater 0x6c3480 (ships and submarines): m+0x5c the depth below the water (<= 0: the unit rides at
  // water + depth), m+0x60 this tick's dive / surfacing step, m+0x74 the layer the dive or surfacing ends on,
  // m+0x80 the vertical event of a non-flying unit (0 Top, 1 Bottom, 2 Up, 3 Down), and the unit attribute
  // elevation (u+0x428+0x30; Physics.Elevation until Unit:SetElevation)
  float depth = 0, depthStep = 0, elevationAttr = 0;
  int targetLayer = 0, vertEvent = 0;
  // CalcRollHack 0x6c1350 (SnapToWater 0x6c1cb0's up axis): m+0x68 the dive / surfacing pitch, m+0xcc the roll's
  // velocity (written only there and by AddRecoilImpulse 0x6b89b0, Unit:RecoilImpulse); the roll's offset is m+0xc0,
  // the field the hover's lean uses (hoverLean). committedOri: the orientation as the beat began (the committed
  // transform IUnit::GetTransform 0x6a49c0 returns, u+0xa4), whose forward axis the pitch leans against.
  Vec3 diveTilt, rollVel;
  Quat committedOri;
  // m+0x7c: the horizontal event of a non-flying unit (0 Cruise, 1 TopSpeed, 2 Stopping, 3 Stopped), which
  // scripts hear as OnMotionHorzEventChange (ProcessCommonMotionState 0x6c2a40)
  int horzEvent = 3;
  bool slavedTurning = false;  // m+0x91: turning the body toward a slaved weapon's target
  // m+0x20: the facing a task asked for (CUnitMotion::SetFacing 0x6b8590: the attack task's AttackAngle); a
  // standing unit turns to it when no slaved weapon and no formation say otherwise; a new goal clears it
  Vec3 faceDir;
  bool arrived = false;   // set when the goal cell was reached (consumed by the move command)
  bool failed = false;    // no path
  bool ballistic = false;  // dropped from a transport: falls (sim/transport.cpp)
  bool atRest = false;     // motion+0x78 = 3: a dead unit's fall has ended; it never moves again (MotionTick)
  std::shared_ptr<AirMotion> air;  // aircraft: the flight model's state (sim/air.cpp)
  // Land navigator (sim/landnav.cpp): it decides the waypoint and when a move ends.
  std::shared_ptr<LandNav> nav;
  bool navDriven = false;    // the current move belongs to the navigator
  bool hasWaypoint = false;  // the steering has a waypoint (else it coasts)
  // The steering's last waypoint as the original keeps it (GetWaypoint 0x5d2170: its list's last entry, or zero
  // while the list is empty): SetWaypoints 0x5d29c0 alone writes it, so it stands after the unit arrived there
  // and goes only with SetWaypoints(0, 0) (a stop). The navigator's Execute compares its target with it.
  bool steerWpSet = false;
  Vec3 steerWp;
  // CalcMoveCommon's hard rule: a step from a cell the footprint fits into one it does not is
  // undone and the unit is pushed back a little; while pushed it coasts.
  bool pushed = false, wasMoving = false;
};

// The passability grid of a motion blueprint's footprint, and of its AltFootprint (cached).
const class PathGrid* FootprintGrid(Sim& sim, const MotionBlueprint& b);
const class PathGrid* AltFootprintGrid(Sim& sim, const MotionBlueprint& b);
// Goal cell of a footprint at a world position (round half to even, as the original).
void GoalCell(const MotionBlueprint& b, float x, float z, int* cx, int* cz);
void GoalCell(const NamedFootprint& f, float x, float z, int* cx, int* cz);
// Entity::GetFootprint 0x678880 of a unit: the AltFootprint (bp+0xe8) while unit+0x124 or +0x125 is set, else the
// Footprint (bp+0xd8). The stock exe tests +0x124 (entity+0x11c) alone; FAF's patch (0x6788a5 -> 0x128e9a6) adds
// +0x125 (entity+0x11d). The footprint tests of a moving unit all call it (223 call sites): the path-finder's fit
// tests (SetGoal's ring 0x5aa13a, QueueSearch's start cell 0x5aa3e8 / 0x5aa45f, CanTraverseCell 0x5aa73e /
// 0x5aa7ca), the navigator's (FootprintFits in RequestPath 0x5ae11b, UpdateCurrentPosition 0x5ae591 and
// RequestContinuationPath 0x5aed81; Reach 0x5af549; the corridor 0x720bbb; SetGoal's free spot 0x5a3f7b; the
// target point's place 0x5ad992), Unit::WontFitAt 0x62aa90 (the hard rule, CalcMoveCommon 0x6c2197 / 0x6c2958, and
// the spline's look-ahead, Generate 0x5b4488 / 0x5b460e), IsAtPosition 0x62cc40, PrepareMove 0x62b7c1, the unit
// blocking tests (UnitIsBlocked 0x72136b, PathTransitionBlocked 0x7219ca, COORDS_CanMoveAt 0x720fdb), the layer
// change's offset (UpdateCurrentLayer 0x6c368b), Unit:CanPathTo 0x6cba42, the collision pass (DoCollisionsFor
// 0x59808b..). Only a unit with an AltMotionType has an AltFootprint other than its Footprint (the Salem: named
// WaterLand4x4 / Water4x4, the same size; caps 9 / 8, MinWaterDepth 0 / 1.5).
const NamedFootprint& UnitFootprint(const Unit* u);
const class PathGrid* UnitFootprintGrid(Sim& sim, const Unit* u);
// CAiPathFinder::GetFootprint 0x5a9d30 (slot 0: the cluster map BeginQuery searches) and the navigator's
// SetCurrentPosition 0x5ae210: the named AltFootprint (+0x20 = bp+0x300) while unit+0x124 is set; they do not look
// at +0x125 (a Salem forced onto its AltFootprint by its script searches the WaterLand4x4 map, but every cell of
// level 0 is tested with Water4x4).
const NamedFootprint& PathFinderFootprint(const Unit* u);

// One sim tick of a unit's motion (after its commands ran).
void MotionTick(Sim& sim, Unit* u);
// Unit::UpdateTerrainType 0x6a9b50: the terrain type code of the unit's footprint-origin cell at (x, z)
// (fistp(x - SizeX/2), fistp(z - SizeZ/2) of Entity::GetFootprint; outside the map: 1, 'Default') becomes the
// unit's (unit+0x57c, 0 at creation); when it changed, the script hears OnTerrainTypeChange(new, old) with the
// map's TerrainTypes entries for the two codes (nil for a code without one, as code 0). Called by the Unit
// constructor right after the starting layer is set (0x6a5b6e), by Unit::Warp after its layer (0x6aa428), and by
// CUnitMotion::MotionTick right after UpdateCurrentLayer (0x6b9f6f), before the move. Defined in sim/sim.cpp.
void UpdateTerrainType(Sim& sim, Unit* u, float x, float z);
// The CUnitMotion constructor's last step (0x6b8133), once the unit's starting layer is set: a unit created on
// the Sub layer is at its depth at once (OnMotionVertEventChange 'Bottom').
void MotionCreated(Sim& sim, Unit* u);
// CUnitMotion::SetNewTargetLayer 0x6b8920 (a layer bit): Sub -> Water starts surfacing (MovingUp), Water -> Sub
// a dive (MovingDown); CalcMoveWater changes the layer when the depth gets there.
void SetNewTargetLayer(Sim& sim, Unit* u, int layer);
// Start moving toward `goal` along `path` (waypoints after the start; last = goal).
// It starts driving at `driveTick` (until then it coasts).
void MotionSetGoal(Sim& sim, Unit* u, const std::vector<Vec3>& path, bool passThrough, uint32_t driveTick);
// Abort the move: the unit coasts to a stop.
void MotionStop(Unit* u);
// CUnitMotion::Stop 0x6b8460 without a point, as CAiSteeringImpl::Stop 0x5d35e0 calls it (0x5d3675): the motion's
// target (UnitMotion::motionTarget) for a unit that is not an aircraft.
void MotionStopTarget(Unit* u);
// The look rotation 0x50b480: forward along the direction, the world's up (motion.cpp).
Quat LookQuat(float dx, float dy, float dz);
// Unit::GetVelocity 0x6a9c90 (entity vtbl+0x3c): a unit with a land motion not on the Air layer reports m+0x38,
// otherwise the last tick's displacement
const Vec3& UnitVelocity(const Unit* u);
// The "is it moving" test of COORDS_CanMoveAt 0x720f70 (0x721231..0x72124f): GetVelocity against (0, 0, 0) with
// 0x4f0a50, a memcmp of the 12 bytes, so a -0.0 part counts as moving.
bool UnitVelocityBitsSet(const Unit* u);
// CAiSteeringImpl::SetWaypoints(nullptr, 0) 0x5d29c0: no waypoint; a unit still on a spline gets a brake spline
// and its first point at once (engine-ref move_handoff.md).
void SteeringClearWaypoints(Sim& sim, Unit* u);
// CAiSteeringImpl::CheckCollisions 0x5d3740 for one unit (sim/steering.cpp).
void SteeringCheckCollisions(Sim& sim, Unit* u);
// The navigator stage of this army is running (its army index, 1-based; 0: none). The original runs each army's
// navigators and then its steering (brain+0xa8, brain+0xac, beat_order.md): a collision record a navigator's
// steering Stop writes is looked at in this beat by a unit of this army or a later one, in the next by an earlier.
void SteeringNavStageArmy(Sim& sim, int army);
// UnitFootprintGrid's grid is already made for this sim (it would not be made now)
bool UnitFootprintGridReady(Sim& sim, const Unit* u);
// Navigator interface (sim/landnav.cpp): a move starts (no waypoint yet: it coasts), the steering
// gets a waypoint (through: drive through it, else brake to it), the move ended.
void MotionNavBegin(Unit* u);
void MotionSetWaypoint(Sim& sim, Unit* u, const Vec3& p, bool through);
void MotionNavDone(Unit* u, bool succeeded);
// The steering stage (CAiSteeringImpl::OnTick, before motion; sim/steering.cpp): new waypoints,
// spline batches, push ends, predicted collisions and their reactions (engine-ref unit_collision.md).
// One army's steering stage, its units in creation order, in that army's turn (landnav.h: LandNavStageBegin).
void SteeringTickUnits(Sim& sim, const std::vector<Unit*>& units);
// Place a unit on the ground/water at its current position (SnapToGround / SnapToWater).
void SnapUnit(const Sim& sim, Unit* u);
// The raised platform the unit stands on (motion+0x114), and that platform's height at (x, z) above its own
// position (Unit::DistanceToOccupiedRect 0x62af70; 0 for a dead platform or outside its quads).
const Unit* RaisedPlatform(const Sim& sim, const Unit* u);
float PlatformHeight(const Unit* P, float x, float z);

// Quaternion helpers shared with the probe-facing bindings.
Quat YawQuat(float fx, float fz);

}  // namespace moho
