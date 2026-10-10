// Combat: unit weapons (target acquisition, aim controllers, the fire clock), projectiles,
// collision beams, damage, killing, and the intel that decides what armies can target.
//
// The engine<->script contract (FA exe read 2026-10-08; specs in FINDINGS "Rebuild M4c"):
// - Weapons pick targets themselves (CAcquireTargetTask, every TargetCheckInterval) from the
//   enemies their army has a blip on, ranked by the script-set TargetPriorities; they call
//   weapon:OnGotTarget() / OnLostTarget() when they get a target from none / lose it.
// - Every tick a weapon whose fire clock ran out and that can fire (target in range, aim
//   controller on target, not busy...) gets weapon:OnFire() and its clock restarts at
//   round(10 / RateOfFire). Everything else about firing is script: the Lua weapon creates the
//   projectiles (weapon:CreateProjectile(muzzle)) and plays the effects.
// - Projectiles fly (gravity, thrust, homing, zig-zag), sweep their path against terrain, water
//   and the collision primitives of units, shields and projectiles (asking the hit entity's
//   OnCollisionCheck), and call OnImpact(type, entity) the tick after a hit. Scripts deal the
//   damage (Damage / DamageArea / DamageRing -> target:OnDamage) and kill (Unit:Kill ->
//   OnKilled); the dying unit is destroyed by its script later.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/lua.hpp"
#include "sim/entity.h"
#include "sim/script_object.h"

namespace moho {

class Sim;
class Unit;
class UnitWeapon;
class Projectile;
struct BlueprintInfo;
struct BuildTask;
struct UnitCommand;
// Unit::PickTargetPoint 0x6aaf50 via CAiTarget::UpdateTarget 0x5d55b0 for an entity target (a unit, or a recon
// blip's source unit): IntRange(#AI.TargetBones) -> index, or -1 without a draw (structure_draws.md 1).
int TargetPointDraw(Sim& sim, Entity* e);

// Weapon blueprint values the engine reads (RUnitBlueprintWeapon), cached per blueprint table.
struct WeaponBp {
  std::string label;
  int rangeCategory = 0;  // UWRC_*: 0 Undefined, 1 DirectFire, 2 IndirectFire, 3 AntiAir, 4 AntiNavy, 5 Countermeasure
  bool prefersPrimaryWeaponTarget = false, stopOnPrimaryWeaponBusy = false, slavedToBody = false;
  float slavedToBodyArcRange = 1;
  bool autoInitiateAttackCommand = false;
  float targetCheckInterval = 3;
  bool alwaysRecheckTarget = true;
  float minRadius = 0, maxRadius = 0, maximumBeamLength = 0, maxHeightDiff = 0, trackingRadius = 1;
  float headingArcCenter = 0, headingArcRange = 180, firingTolerance = 0.01f, firingRandomness = 0;
  float muzzleVelocity = 0, muzzleVelocityRandom = 0, muzzleVelocityReduceDistance = 0;
  bool leadTarget = true;
  float projectileLifetime = 0, projectileLifetimeUsesMultiplier = 0;
  float damage = 0, damageRadius = 0;
  std::string damageType = "Normal";
  float rateOfFire = 1;
  std::string projectileId;
  int ballisticArc = 0;  // 0 None, 1 LowArc, 2 HighArc
  std::string targetRestrictOnlyAllow, targetRestrictDisallow;
  bool manualFire = false, nukeWeapon = false, overChargeWeapon = false, needPrep = false, countedProjectile = false;
  bool ignoresAlly = true;
  int targetType = 0;  // 0 Unit, 1 Projectile, 2 Prop
  int attackGroundTries = 3;
  bool aimsStraightOnDisable = false, turreted = false, yawOnlyOnTarget = false;
  bool aboveWaterFireOnly = false, belowWaterFireOnly = false, aboveWaterTargetsOnly = false,
       belowWaterTargetsOnly = false;
  bool reTargetOnMiss = false, needToComputeBombDrop = false;
  float bombDropThreshold = 1.5f;
  bool useFiringSolutionInsteadOfAimBone = false, ignoreIfDisabled = false, cannotAttackGround = false;
};
const WeaponBp& GetWeaponBp(lua_State* L, int bpRef);

// A weapon's (or projectile's) target (the original's CAiTarget).
struct AiTarget {
  int type = 0;  // 0 none, 1 entity, 2 ground
  uint32_t entityId = 0;
  Vec3 pos;      // ground target
  int aimBone = -1;
  bool mobile = false;
  bool operator==(const AiTarget& o) const {
    return type == o.type && entityId == o.entityId && (type != 2 || (pos.x == o.pos.x && pos.y == o.pos.y && pos.z == o.pos.z));
  }
};

// CreateAimController(weapon, label, yawBone, pitchBone, muzzleBone) (CAimManipulator).
class AimController : public ScriptObject {
 public:
  UnitWeapon* weapon = nullptr;
  Unit* unit = nullptr;
  std::string label;
  int yawBone = -1, pitchBone = -1, muzzleBone = -1;
  bool enabled = true;
  float heading = 0, pitch = 0;           // current, radians, bone-relative
  float hCenter = 0, hHalf = 3.14159265f, hSlew = 0.0628318f;
  float pCenter = 15, pHalf = 30, pSlew = 0.0610865f;  // (sic: degrees until SetFiringArc)
  bool onTarget = false, tracking = false, yawVertical = false;
  int resetPoseTicks = 0, resetCountdown = 0;
  float headingOffset = 0;
  bool alive = true;
  bool usesGravity = false, tracksTarget = false;
  float projMaxSpeed = 0;
  int EventState() const override { return onTarget ? 1 : 0; }
};

// CRotateManipulator (CreateRotator): turns one bone about a local axis toward a goal or at a speed;
// applied to the pose every tick before the aim controllers.
class RotateManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int bone = -1, axis = 1;  // 0 x, 1 y, 2 z
  float cur = 0, goal = 0, speed = 0, targetSpeed = 0, accel = 0;  // degrees, degrees per second
  bool hasGoal = false, enabled = true, alive = true;
};
void RotatorsTick(Unit* u);

// CBuilderArmManipulator (CreateBuilderArmController, builder_arm.md): turns the yaw/pitch bones toward
// the builder's aim target and reports the builder "on target" (Unit::armReady).
class BuilderArm : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int yawBone = -1, pitchBone = -1, aimBone = -1;
  float heading = 0, pitch = 0;                                  // +0x88 / +0x8c (rad)
  float hCenter = 0, hHalf = 3.14159265f, hSlew = 0.0628318563f;  // +0x98..+0xa0
  float pCenter = 15.0f, pHalf = 30.0f, pSlew = 0.0610865243f;    // +0xa4..+0xac
  bool tracking = false, onTarget = false, enabled = true, alive = true;
  int precedence = 0;
  int EventState() const override { return onTarget ? 1 : 0; }
};
// MoveManipulator 0x636590 of each arm: runs in Unit::MotionTick after the unit's own motion (UpdateManipulators 0x63aa80).
void BuilderArmsTick(Sim& sim, Unit* u, const Vec3& priorPos, const Quat& priorOri);  // after the unit's motion
// CAiBuilderImpl::SetAimTarget 0x59f600: a non-zero target calls the unit's OnPrepareArmToBuild.
void SetArmAimTarget(Sim& sim, Unit* u, Vec3 p);

// Per-tick velocity of an entity (the last tick's displacement).
Vec3 EntityVelocity(const Entity* e);
// World transform of a bone in the current pose (rest pose plus this tick's aim rotations);
// bone -1: the entity itself.
void BoneWorld(const Entity* e, int bone, Vec3* pos, Quat* rot);
// Whether the blueprint is in a named category (BENIGN, COMMAND, ...).
bool BpInCategory(Sim& sim, const BlueprintInfo* bp, const char* name);

// Called when a unit's weapon was created (after its OnCreate).
void InitUnitWeapon(lua_State* L, UnitWeapon* w);
// Per tick (see Sim::Tick for the order).
void KillCleanupTick(Sim& sim);
void UnitAimTick(Sim& sim, Unit* u);       // after the unit's motion
void ProjectilesTick(Sim& sim);
void BeamsTick(Sim& sim);
void WeaponsTick(Sim& sim);
// A unit leaves the world.
void ReleaseUnitCombat(Sim& sim, Unit* u);

// Attack commands (CUnitAttackTargetTask), run as the unit's command task.
BuildTask* StartAttackTask(Sim& sim, Unit* u, const UnitCommand& c);
float GuardScanRadiusOf(Unit* u);
Unit* GuardBestEnemy(Sim& sim, Unit* u);
// The patrol's search box (an OBB in xz: centre, unit direction along the leg, half extents across / along).
struct PatrolBox {
  float cx = 0, cz = 0, dx = 0, dz = 1, side = 0, along = 0;
};
// CUnitPatrolTask::FindTarget 0x61b710: the primary weapon's best enemy among the cached blips that touch the
// box, within GuardScanRadius.
Unit* PatrolFindTarget(Sim& sim, Unit* u, const PatrolBox& box);
BuildTask* MakeAttackTaskOn(Sim& sim, Unit* u, Entity* e);
int TickAttack(Sim& sim, lua_State* L, Unit* u, BuildTask& t);
void EndAttack(Sim& sim, Unit* u, BuildTask& t);
// Aggressive moves and patrols look for enemies on the way (CUnitPatrolTask): an engaged target
// (0: none) the move stops for.
// The command it was engaging for ended.
void ClearEngagement(Sim& sim, Unit* u);

// UnitWeapon::CanAttackTarget 0x6d5720.
bool WeaponCanAttackTarget(Sim& sim, UnitWeapon* w, const AiTarget& t);
// Unit::Kill (0x6a8090).
void KillUnit(Sim& sim, lua_State* L, Unit* u, Entity* instigator, const std::string& type, float ratio);

void RegisterCombatBindings(lua_State* L);
// Animation manipulators (sim/anim.cpp): timing only.
void AnimTick(Sim& sim);
void ReleaseAnimators();
void RegisterAnimBindings(lua_State* L);

}  // namespace moho
