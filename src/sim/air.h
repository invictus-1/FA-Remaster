// Aircraft: the original's flight model (CUnitMotion::CalcMoveAir and its helpers), the rigid
// body it integrates (SPhysBody), the air navigator's target/arrival rules (CAiNavigatorAir) and
// the attack-run state machine (ComputeAirCombatTactics).
//
// What the original does (FA exe, read 2026-10-09; specs engine-ref/specs/air_motion.md,
// air_control.md, air_nav.md, air_extra.md):
// - Every aircraft has a rigid body: mass = AverageDensity * SizeX*SizeY*SizeZ, the blueprint's
//   inertia tensor, gravity (0, -4.9, 0). Each tick a controller computes a force and a torque and
//   the body is integrated (trapezoidal, dt = 0.1); the unit's transform is the body's.
// - The force is a P-D controller on velocity toward a "desired velocity": horizontally toward the
//   motion target at up to MaxAirspeed (KMove, damping), vertically toward the smoothed terrain
//   height ahead plus the flying elevation (KLift, KLiftDamping).
// - The torque is a P-D controller on orientation toward a desired orientation: winged aircraft
//   yaw toward the target at TurnSpeed, pitch with the climb and bank into the turn (BankFactor);
//   hovering aircraft tilt with their acceleration; gunships circle their target.
// - The navigator flies straight at the goal point (the goal cell's centre). A move ends when the
//   aircraft is within 0.25 of the point, or within MaxAirspeed of it when another move follows.
// - An idle aircraft lands after AutoLandTime (it finds a free spot under it), and takes off on
//   the next order. A dead one falls (CalcMoveBallistic) and calls OnImpact on the ground.
#pragma once
#include <cstdint>
#include <memory>

#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;
class TerrainMap;
struct BlueprintInfo;

// RUnitBlueprintAir and the other blueprint values the flight model reads (cached per blueprint).
struct AirBp {
  bool canFly = false, winged = false, flyInWater = false;
  float autoLandTime = 0, maxAirspeed = 0, minAirspeed = 0, turnSpeed = 1, combatTurnSpeed = 1;
  float startTurnDistance = 0, tightTurnMultiplier = 1, sustainedTurnThreshold = 10, liftFactor = 5;
  float bankFactor = 0.5f;
  bool bankForward = false;
  float engageDistance = 0, breakOffTrigger = 0, breakOffDistance = 0;
  bool breakOffIfNearNewTarget = false;
  float kMove = 1, kMoveDamping = 1, kLift = 1, kLiftDamping = 1, kTurn = 3, kTurnDamping = 3, kRoll = 3,
        kRollDamping = 3;
  float circlingTurnMult = 3, circlingRadiusChangeMinRatio = 0.6f, circlingRadiusChangeMaxRatio = 0.9f,
        circlingRadiusVsAirMult = 1, circlingElevationChangeRatio = 0.25f, circlingFlightChangeFrequency = 2;
  bool circlingDirChange = true, hoverOverAttack = false;
  float randomBreakOffDistanceMult = 1.5f, randomMinChangeCombatStateTime = 3, randomMaxChangeCombatStateTime = 6;
  float transportHoverHeight = 0, predictAheadForBombDrop = 0;
  // Physics / entity blueprint
  float elevation = 0, attackElevation = 0, maxSpeed = 0;
  float sizeX = 1, sizeY = 1, sizeZ = 1, averageDensity = 0.49f;
  float inertia[3] = {1, 1, 1};
  float collisionOffset[3] = {0, 0, 0};
  int footprintX = 1, footprintZ = 1;
  // the blueprint's own Footprint values PrepareMove keeps for a flyer (air_extra.md 3.1-3.2)
  float fpMaxSlope = 0, fpMinWaterDepth = 0;
  uint8_t fpFlags = 0;
  bool transportation = false, targetChaser = false, experimental = false;
  bool airCategory = false;  // in category AIR (the unit's u+0x68b, Unit ctor 0x6a6aae)
};

// Quaternion in the original's memory order (w, x, y, z).
struct QuatW {
  float w = 1, x = 0, y = 0, z = 0;
};

// SPhysBody (0x54 bytes in the original).
struct PhysBody {
  float mass = 1;
  float invI[3] = {1, 1, 1};  // 1 / (mass * InertiaTensor)
  Vec3 off;                   // centre of mass in the entity frame
  Vec3 com;                   // centre of mass, world
  QuatW q;
  Vec3 v;                     // world units per second
  Vec3 L;                     // angular momentum, world frame
};

struct AirMotion {
  const AirBp* bp = nullptr;
  PhysBody body;
  Vec3 target;          // m+0x14: where it flies (or the landing spot)
  Vec3 facing;          // m+0x20: desired arrival heading (zero: none)
  float height = 0;     // m+0x50: height above the surface
  float curTerrain = 0; // m+0x54: smoothed terrain height ahead
  float elevOffset = 0; // m+0x58: desired height above curTerrain
  float landHeight;     // m+0x64: explicit landing height (+inf: none)
  int landLayer = 0;    // m+0x74: layer bits (0 none, 1 Land, 8 Water, 0x10 Air)
  int motionState = 0;  // 0 None, 1 Attached, 2 Ballistic, 3 Crashed
  int horzEvent = 3;    // 0 Cruise, 1 TopSpeed, 2 Stopping, 3 Stopped
  int vertEvent = 0;    // 0 Top, 1 Bottom, 2 Up, 3 Down, 4 Hover
  int carrierEvent = 0;
  bool fullSpeed = false;  // m+0x8c speed through the goal
  bool stopped = false;
  bool circleDir = false;  // m+0x8f
  float circleElev = 0;    // m+0x94
  float circleRatio = 1;   // m+0x98
  float randElev = 0;      // m+0x9c (0 in practice, see the spec)
  int combatState = 0;     // m+0xa0
  uint32_t combatTimer = 0;  // m+0xa4
  int combatCounter = 0;     // m+0xa8
  uint32_t idleTick = 0;     // m+0xac
  Vec3 prevVel;              // m+0xb4
  // m+0xfc..+0x104: ComputeAirControl's last force (0x6bedf4..0x6bee10: gravity's share and the mass in it), which the
  // thrust controllers read (CThrustManipulator::MoveManipulator 0x64a879).
  // m+0x108..+0x110: ONE field with two writers, as the exe's: ComputeAirControl's body torque T before the division by
  // the inertia (0x6bee1e..0x6bee3c), and the dead branch's random spin torque, world (CalcMoveAir 0x6bf39c..0x6bf3b8).
  // Its readers: CalcMoveBallistic's spin, whenever it is not zero (0x6c03be..0x6c03db, the memcmp 0x4f0a50 against
  // 0xf3d21c), and the thrust controllers' yaw torque (+0x10c, 0x64a8c2).
  Vec3 ctrlForce, torque108;
  bool spinFromDeath = false;  // (MOHO64_DEBUG_BALLISTIC only: the dead branch wrote torque108 last)
  int landRect[4] = {0, 0, 0, 0};  // reserved landing cells (x0, z0, x1, z1)
  float elevationAttr = 0;   // attributes elevation (SetElevation)
  float lastScale = 1;       // 1/scale of the last MoveTo (GetVelocity)
  // navigator (CAiNavigatorAir)
  bool steering = false;     // status 2
  uint32_t destEntity = 0;   // SetDestUnit's entity: the target follows it every beat until the next SetGoal
  bool inFormation = false;  // navigator +0x88: follows its formation slot (air_nav 4.3)
  bool pending = false;      // a SetGoal waiting for its tick
  uint32_t pendingTick = 0;
  Vec3 pendingGoal;
  int pendingLayer = 0;   // the goal's layer (0: Air; 1: a landing move)
  Vec3 pendingFacing;     // SetFacing while a goal is pending
  Vec3 goal;              // navigator +0x7c
  Vec3 curTarget;         // navigator +0x70: what SetGoal or the formation update last handed to the motion (0 at first)
};

const AirBp& GetAirBp(lua_State* L, const BlueprintInfo& bp);

// A new aircraft (after its position and orientation are set): spawn height, body, state.
void AirInit(Sim& sim, Unit* u);
// The height a unit spawns at (IUnit::CalcSpawnElevation): surface + elevation for flyers.
float AirSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z, float y);
// IUnit::CalcSpawnElevation for a unit starting on the Air / Orbit layer: the surface (ground or water) + Physics.Elevation.
float AirLayerSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z);
// One tick (CUnitMotion::MotionTick, air path, then the navigator's arrival test).
void AirMotionTick(Sim& sim, Unit* u);
// The air navigator's formation following (UpdateCurrentTargetFromFormation 0x5a4d80), as its Execute runs it in the
// army stage: in the motion pass before the unit's UpdateInfoCache and MotionTick.
void AirFormationFollow(Sim& sim, Unit* u);
// The navigator's SetGoal: fly to the cell of `goal`; takes effect at `tick`. layer 1 (Land) is a
// landing move (NewMoveTask with goal layer Land): the spot goes through PrepareMove and the
// aircraft lands (or hovers at TransportHoverHeight while loading / carrying) there.
void AirSetGoal(Sim& sim, Unit* u, Vec3 goal, uint32_t tick, int layer = 0, bool landingSpot = true);
// ... and the goal given in the command stream, between beats: the navigator's SetGoal sets the motion's target at
// once (SetMotionTarget 0x5a4a90 -> CUnitMotion::SetTarget 0x6b85e0), where ours waits for the motion step.
void AirTakeGoalNow(Sim& sim, Unit* u);
// CUnitMotion+0x64: a fixed landing height (+inf: none), e.g. a staging platform's bone.
// The navigator's SetDestUnit: fly at an entity's exact position (attack runs on a mobile target).
void AirSetDestEntity(Sim& sim, Unit* u, const Entity* e);
void AirSetLandHeight(Unit* u, float h);
void AirNavigatorGone(Unit* u);  // KillCleanup deleted the navigator: it steers no more
// CUnitMotion::SetFacing: the heading wanted on arrival.
void AirSetFacing(Unit* u, Vec3 dir);
// CUnitMotion::SetTarget(p, zero, layer) right away (layer bits: 1 Land, 0x10 Air).
void AirSetTargetNow(Sim& sim, Unit* u, Vec3 p, int layer);
// CUnitMotion::SetTarget 0x6b85e0 as a task calls it on the motion itself: the point as given (no goal cell),
// a facing to arrive with, the layer.
void AirSetTargetFacing(Sim& sim, Unit* u, Vec3 p, Vec3 dir, int layer);
// The terrain samples [x0, x1] x [z0, z1] changed (FlattenMapRect): refresh the look-ahead min/max pyramid.
void AirTerrainChanged(const Sim& sim, const TerrainMap* map, int x0, int z0, int x1, int z1);
// Unit::PrepareMove: a free landing spot near *pos (false: none found), its cell outside `excl` (x0, z0, x1, z1;
// null or empty = none), rings `spacing` cells apart (0 = 2 x the squared footprint).
bool AirPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float* excl = nullptr, int spacing = 0);
// Unit::PrepareMove for a ground unit (its own footprint): a free spot near *pos whose cell does
// not overlap `excl` (x0, z0, x1, z1; empty = none). false: none found.
// Unit::PrepareMove 0x62b780 (air_nav.md 7.1): spacing 0 = 2 x the footprint size; the exclusion test is closed.
bool GroundPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float excl[4], int spacing = 0);
// A ground unit's o-grid reservation (ReserveOgridRect / FreeOgridRect).
void GroundReserveRect(Sim& sim, Unit* u, const int r[4]);
void GroundFreeRect(Sim& sim, Unit* u);
// Unit::CanReserveOgridRect 0x6ab810 (its own reservation does not count).
bool GroundCanReserveRect(Sim& sim, Unit* u, const int r[4]);
// OCCUPY_FootprintFits 0x7209e0 at a footprint-origin cell with OCCUPY_MobileCheck 0x564ab0's caps (no Sub on the
// Water layer): terrain and structures.
bool GroundCellFits(Sim& sim, Unit* u, int cx, int cz);
// AbortMove: fly on to the point one second ahead and stay there.
void AirAbort(Sim& sim, Unit* u);
// CAiNavigatorAir::Execute's arrival test, in the navigators' stage of the beat (sim/landnav.cpp LandNavTickUnits).
bool AirNavArrival(Sim& sim, Unit* u);  // true: Execute re-aimed at its destination entity and returned
// CUnitMotion::Stop 0x6b8460 with no point: the target is where the aircraft is now.
void AirStopHere(Sim& sim, Unit* u);
// Unit::UpdateSpeedThroughStatus 0x6ac940 for aircraft: call at the exe's event points only (air_nav.md 5).
void AirSpeedThroughEvent(Sim& sim, Unit* u);
// CAiNavigatorAir::SetSpeedThroughGoal 0x5a5080: the steering's "last waypoint in the goal" (vt+0x28, 1) and its
// request (vt+0x20, v): the motion's speed-through byte becomes v.
void AirSetSpeedThroughGoal(Unit* u, bool v);
// Unit::PredictAheadBomb: position after t seconds along the current turn.
Vec3 PredictAhead(Sim& sim, Unit* u, float t, bool other = false);
// Warp: re-sync the body and the terrain height.
void AirWarp(Sim& sim, Unit* u);
// CUnitMotion::ProcessFuelLevels 0x6b9940 (fuel.md 3): burn in the air, refuel on the ground or
// docked on a staging platform (which also repairs, paid through an economy request).
void FuelTick(Sim& sim, Unit* u);
// The staging platform u is docked on (Unit::GetStagingPlatform 0x62ee00), or null.
Unit* StagingPlatformOf(Sim& sim, const Unit* u);
// CUnitMotion::NotifyAttached / NotifyDetached for aircraft: motion state, events.
void AirNotifyAttached(Sim& sim, Unit* u);
void AirNotifyDetached(Sim& sim, Unit* u);
void AirParentGone(Sim& sim, Unit* u);
// CUnitMotion+0x88 (EUnitMotionCarrierEvent: 0 none, 1 approach, 2 final landing).
void AirSetCarrierEvent(Unit* u, int e);
// CUnitMotion::SetImmediateVelocity 0x6b9460: the body's velocity and orientation (a carrier launch).
void AirSetImmediateVelocity(Sim& sim, Unit* u, Vec3 vel, Quat q);
void RegisterAirBindings(lua_State* L);
// Drops the sim's landing-spot reservations (Sim::~Sim).
void AirForget(Sim& sim);

}  // namespace moho
