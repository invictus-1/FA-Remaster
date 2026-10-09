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
  Vec3 spinTorque;           // m+0x108 (dead aircraft)
  int landRect[4] = {0, 0, 0, 0};  // reserved landing cells (x0, z0, x1, z1)
  float elevationAttr = 0;   // attributes elevation (SetElevation)
  float lastScale = 1;       // 1/scale of the last MoveTo (GetVelocity)
  // navigator (CAiNavigatorAir)
  bool steering = false;     // status 2
  bool pending = false;      // a SetGoal waiting for its tick
  uint32_t pendingTick = 0;
  Vec3 pendingGoal;
  int pendingLayer = 0;   // the goal's layer (0: Air; 1: a landing move)
  Vec3 pendingFacing;     // SetFacing while a goal is pending
  Vec3 goal;
};

const AirBp& GetAirBp(lua_State* L, const BlueprintInfo& bp);

// A new aircraft (after its position and orientation are set): spawn height, body, state.
void AirInit(Sim& sim, Unit* u);
// The height a unit spawns at (IUnit::CalcSpawnElevation): surface + elevation for flyers.
float AirSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z, float y);
// One tick (CUnitMotion::MotionTick, air path, then the navigator's arrival test).
void AirMotionTick(Sim& sim, Unit* u);
// The navigator's SetGoal: fly to the cell of `goal`; takes effect at `tick`. layer 1 (Land) is a
// landing move (NewMoveTask with goal layer Land): the spot goes through PrepareMove and the
// aircraft lands (or hovers at TransportHoverHeight while loading / carrying) there.
void AirSetGoal(Sim& sim, Unit* u, Vec3 goal, uint32_t tick, int layer = 0, bool landingSpot = true);
// CUnitMotion+0x64: a fixed landing height (+inf: none), e.g. a staging platform's bone.
void AirSetLandHeight(Unit* u, float h);
// CUnitMotion::SetFacing: the heading wanted on arrival.
void AirSetFacing(Unit* u, Vec3 dir);
// CUnitMotion::SetTarget(p, zero, layer) right away (layer bits: 1 Land, 0x10 Air).
void AirSetTargetNow(Sim& sim, Unit* u, Vec3 p, int layer);
// Unit::PrepareMove: a free landing spot near *pos (false: none found).
bool AirPrepareMove(Sim& sim, Unit* u, Vec3* pos);
// Unit::PrepareMove for a ground unit (its own footprint): a free spot near *pos whose cell does
// not overlap `excl` (x0, z0, x1, z1; empty = none). false: none found.
bool GroundPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float excl[4]);
// A ground unit's o-grid reservation (ReserveOgridRect / FreeOgridRect).
void GroundReserveRect(Sim& sim, Unit* u, const int r[4]);
void GroundFreeRect(Sim& sim, Unit* u);
// AbortMove: fly on to the point one second ahead and stay there.
void AirAbort(Sim& sim, Unit* u);
// Unit::PredictAheadBomb: position after t seconds along the current turn.
Vec3 PredictAhead(Sim& sim, Unit* u, float t);
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
void RegisterAirBindings(lua_State* L);

}  // namespace moho
