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
#include "core/speculate.h"
#include <cstdint>
#include <cstring>
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
int TargetPointBone(Sim& sim, Entity* e);  // the same draw, as the bone of the target it names (-1 none)
// A shot's verdict for the weapon that fired it (ReTargetOnMiss): ShotEvent says which (0 hit the target, 1
// missed, 2 no verdict) from the impact type, the shot's target and what it met; WeaponImpactEvent is the
// weapon's acquire task hearing it.
struct AiTarget;
int ShotEvent(Sim& sim, int impactType, const AiTarget& target, const Entity* hit, bool shell);
void WeaponImpactEvent(Sim& sim, class UnitWeapon* w, int event);
// The direction a standing unit turns its body to for a weapon slaved to the body (false: none).
bool SlavedFaceVector(Sim& sim, Unit* u, Vec3* dir);

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

// AiTarget::aimBone of a target point whose bone the target's model does not have (-1: no target point at all)
enum : int { kAimBoneMissing = -2 };

// A weapon's (or projectile's) target (the original's CAiTarget).
struct AiTarget {
  int type = 0;  // 0 none, 1 entity, 2 ground
  uint32_t entityId = 0;
  Vec3 pos;      // ground target
  int aimBone = -1;
  bool mobile = false;
  // the target is held as the army's recon blip of the unit (an order's enemy target), not as the unit (what a
  // weapon found itself): scripts see the difference (GetCurrentTarget returns the blip)
  bool viaBlip = false;
  uint32_t blipId = 0;  // that blip (set when a weapon takes the target): it outlives the unit by a few beats
  // the blip was taken when its unit was already dead (a jammer's blip out of a list of enemies built while the
  // unit lived): a blip is not dead while it exists (HasTarget 0x5e2a5a), so the target lasts as long as the blip
  bool pastDeath = false;
  // the target is one of a jammer's FAKE blips (its ReconBlip entity; entityId stays the jamming unit, viaBlip is
  // set): it stands where the fake stands and lasts as long as the fake does
  uint32_t fakeId = 0;
  // (a unit and the blip of that unit are two entities in the original: going from one to the other is a new target)
  // (the original's CAiTarget compare 0x6d31d0, UnitWeapon::SetTarget 0x6d5f47: the type, the entity, and for a point
  // the point by memcmp 0x4f0a50 at 0x6d31f7..0x6d320a, so a -0.0 part is another point)
  bool operator==(const AiTarget& o) const {
    return type == o.type && entityId == o.entityId && (type != 1 || (viaBlip == o.viaBlip && fakeId == o.fakeId)) &&
           (type != 2 || std::memcmp(&pos, &o.pos, sizeof(Vec3)) == 0);
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
  // the muzzle bone in last beat's pose (actor+8: last beat's unit transform and bone rotations), which is what
  // Aim 0x6317b0 measures the target from
  Vec3 lastMuzzlePos;
  Quat lastMuzzleRot;
  // the same, walked ahead of the unit's turn (MOHO64_PAR_AIM, AimMuzzlesAhead): in beat aheadTick, for muzzleBone
  // aheadBone
  Vec3 aheadMuzzlePos;
  Quat aheadMuzzleRot;
  int aheadBone = -1;
  uint32_t aheadTick = 0xffffffffu;
  int precedence = 0;
  uint32_t seq = 0;  // Sim::NextManipSeq
  int EventState() const override { return onTarget ? 1 : 0; }
};

// CSlideManipulator (CreateSlider 0x6476e0): moves one bone along a vector in the bone's own frame toward a goal
// (weapon recoil, hatches, sinking wrecks). An event: scripts WaitFor it, signalled while it is at its goal.
class SlideManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int bone = -1;
  Vec3 cur, goal;                       // +0x80, +0x8c (pose units: x mesh scale unless world units)
  float speed = std::numeric_limits<float>::infinity();  // +0x98, per tick; <= 0: there at once
  float vel = 0, accel = 0, decel = 0;  // +0x9c, +0xa0, +0xa4
  bool world = false;                   // +0xa8 SetWorldUnits
  bool signalled = true, enabled = true, alive = true;
  int precedence = 0;
  uint32_t seq = 0;
  int EventState() const override { return signalled ? 1 : 0; }
};

// CSlaveManipulator (CreateSlaver(unit, destBone, srcBone) 0x646330, MoveManipulator 0x646140): the destination
// bone is turned by the source bone's rotation relative to its parent AS IT WAS IN LAST BEAT'S POSE (the actor's
// pose +8), so what it follows it follows one beat behind. FAF's weapons slave the racks of a turret
// (RackSlavedToTurret) to its pitch bone: a destroyer's second barrel rises a beat after the first, and its
// shells leave at that pitch.
class SlaveManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int destBone = -1, srcBone = -1;  // +0x60's bone, +0x80
  float maxRate = -1;               // +0x94 (SetMaxRate(degrees a second) x 0.00174532924); below 0: at once
  Quat cur;                         // +0x84: the rotation given to the destination bone
  Quat srcLocal;                    // the source's local rotation in the pose that is being replaced
  bool enabled = true, alive = true;
  int precedence = 0;
  uint32_t seq = 0;
};

// CCollisionManipulator (CreateCollisionDetector 0x638020, ManipulatorUpdate 0x637c90): watches bones of a unit
// and tells the unit's script when one goes under the surface and when it comes back up (EnableTerrainCheck:
// OnAnimTerrainCollision / OnNotAnimTerrainCollision(bone name, x, y, z)), or, without the terrain check, when
// one comes within 0.1 of the unit's own base plane (OnAnimCollision(bone name, the point in the unit's frame)).
// A crashing CZAR's death script waits for its rim to touch the ground this way.
class CollisionManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  bool enabled = false, terrain = false, alive = true;
  struct Watch {
    int bone = -1;
    bool under = false, atBase = false;
  };
  std::vector<Watch> watch;
};
void CollidersTick(Sim& sim, Unit* u);

// CFootPlantManipulator (CreateFootPlantController(unit, foot, knee, hip, [straightLegs], [maxFootFall]) 0x639d00,
// constructor 0x639380, MoveManipulator 0x6395f0): plants a leg's foot on the ground. FAF's
// CreateFootFallManipulators makes one per Footfall bone set of a walker (every commander and support commander,
// the walking bots), precedence 10. Each beat, in its place among the unit's manipulators, it lifts a foot that
// the pose put below the ground (or the raised platform the unit stands on) to it: half by turning the hip about
// its X axis (the knee turned back by as much), half by turning the knee. Ours had no such manipulator (a stub):
// a standing commander's feet stayed at their rest pose, 3 and 6 mm from the original's (MP1, the Cybran
// commander's target bones Left_Foot_B01 / Right_Foot_B01).
class FootPlantManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int footBone = -1, kneeBone = -1, hipBone = -1;  // +0x88, +0x8c, +0x90 (its watched bones in that order, +0x60)
  bool straightLegs = true;                         // +0x94
  float maxFootFall = 0;                            // +0x98
  float halfLeg = 0;                                // +0x9c: |foot y - hip y| / 2 in the pose it was made in
  bool enabled = true, alive = true;                // (+0x4c)
  int precedence = 0;                               // (+0x58)
  uint32_t seq = 0;
};

// CThrustManipulator (CreateThrustController(unit, label, thrustBone) 0x64abc0, constructor 0x64a4a0,
// SetThrustingParam 0x64ad90, MoveManipulator 0x64a800): tilts an aircraft's engine bone with the flight's control
// force. FAF's UEF transports and gunships (UEA0107, UEA0104, UEA0203, UEA0305, XEA0306) make one per engine bone in
// OnStopBeingBuilt, precedence 0. Each beat, in its place among the unit's manipulators: the last control force (the
// motion's m+0xfc) plus the turn's share (the bone's offset from the unit turned about the up axis by the yaw torque,
// m+0x10c, less the offset, times turnMult), in the bone's frame, capped per axis, normalised; the bone turns toward the
// rotation that takes its forward axis there by at most turnSpeed. Ours had no such manipulator (a stub): the
// engines stood at their rest pose.
class ThrustManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int bone = -1;                                 // +0xa4 (its watched bone, +0x60)
  Vec3 capMin{-100.0f, -100.0f, -100.0f};        // +0xac..+0xb4 (the ctor's -100, [0xe4f9b8])
  Vec3 capMax{100.0f, 100.0f, 100.0f};           // +0xb8..+0xc0 (100, [0xe4f744])
  float turnMult = 1.0f, turnSpeed = 0.3f;       // +0xc4, +0xc8 (1, [0xdfec20]; 0.3, [0xe4f9b4])
  Vec3 fwd;                                      // +0xcc: the bone's local forward axis in the pose it was made in
  Quat cur;                                      // +0xd8: the turn applied last beat
  bool enabled = true, alive = true;             // (+0x4c)
  int precedence = 0;                            // (+0x58)
  uint32_t seq = 0;
};

// CRotateManipulator (CreateRotator 0x643d80, MoveManipulator 0x643860): turns one bone about a local axis
// toward a goal, at a speed, or spinning down; applied to the pose every tick at its place among the unit's
// manipulators (precedence 0 unless a script sets one: the UEF cruiser's spinning Phalanx barrel has 10, the
// same as the gun's aim controller, and comes after it because it was made later). An event: scripts WaitFor
// it (signalled at the goal, or when the speed reached the target speed).
class RotateManipulator : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int bone = -1;
  Vec3 axis{0, 1, 0};                // +0x84 ('x' 'y' 'z', or '-x' '-y' '-z')
  float cur = 0, goal = 0;           // +0x90, +0x94: radians
  float speed = 0, targetSpeed = 0;  // +0x98, +0x9c: radians per tick
  float accel = 0;                   // +0xa0: radians per tick per tick
  int followBone = -1;               // +0xa4: the goal is that bone's heading
  bool hasGoal = false, spinDown = false;  // +0x80, +0x81
  bool signalled = true, enabled = true, alive = true;
  int precedence = 0;
  uint32_t seq = 0;
  int EventState() const override { return signalled ? 1 : 0; }
};

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
  // the tracking callback this beat's step owes (1 OnStartBuilderTracking, 2 OnStopBuilderTracking): it is made at
  // the arm's place among the unit's manipulators (UnitAimTickManipulators), after the lower-precedence ones moved
  int trackCall = 0;
  // this beat's step, asked for by BuilderArmsTick (last beat's pose: the aim direction, or a zero aim target) and
  // made in the arm's slot among the unit's manipulators (this beat's pose)
  bool stepPending = false, stepZero = false;
  Vec3 stepDir;
  int precedence = 0;
  uint32_t seq = 0;
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
// BpInCategory's answer for every blueprint index at once (the same cached vector; the reference stays good for
// the sim's life): BpInCategory(bp) == bp && bp->entityIndex >= 0 && index < size && mask[index].
const std::vector<bool>& BpCategoryMask(Sim& sim, const char* name);

// Called when a unit's weapon was created (after its OnCreate).
void InitUnitWeapon(lua_State* L, UnitWeapon* w);
// Per tick (see Sim::Tick for the order).
void KillCleanupTick(Sim& sim);
// The unit's pose takes the unit's transform (and last beat's pose is kept): the start of UpdateManipulators
// 0x63aa80, once per beat after the unit's motion, before its builder arms and UnitAimTick
void PoseAdvance(Unit* u);
void UnitAimTick(Sim& sim, Unit* u);       // after the unit's motion
// The aim controllers' muzzles in last beat's pose, once a beat: before the builder arms' step, else UnitAimTick's.
void AimReadLastPose(Sim& sim, Unit* u);
// MOHO64_PAR_AIM (multicore step 3; the modes of core/speculate.h): before the entity pass, every unit's muzzles of
// this beat walked ahead on the worker pool from last beat's pose, and kept on the unit and its controllers with what
// they were made from (Unit::aimAhead*, AimController::ahead*); at the unit's turn AimReadLastPose takes them when the
// base it walks from, the pose's version, the skeleton, the scale and the controllers are the same. (The result lives
// on the objects, not in a ReadLog: the walk is a fraction of a microsecond, and a record kept apart costs its turn
// more in cold memory than the walk.) order = the pass's units (Sim::Tick's motionOrder).
struct AimAhead {
  AimAhead();
  SpecMode mode;
  SpecCounts counts;
  bool active() const { return mode != SpecMode::kOff; }
};
void AimMuzzlesAhead(Sim& sim, const std::vector<Unit*>& order);
void ProjectilesTick(Sim& sim);
// One projectile's turn in the entity stage (Projectile::MotionTick 0x69bdd0), once per beat.
void ProjectileStep(Sim& sim, Projectile* p);
void BeamsTick(Sim& sim);
void BeamTick(Sim& sim, class CollisionBeam* b);  // one collision beam's beat
void WeaponsStageDebug(Sim& sim);                                     // (MOHO64_DEBUG_COMBAT's lines)
void WeaponsTickUnits(Sim& sim, const std::vector<Unit*>& units);  // one army's attacker stage
// A unit leaves the world.
void ReleaseUnitCombat(Sim& sim, Unit* u);
// The unit is deleted (EntityDb::Purge, after the destroy queue): ~Unit deletes its CAniActor (0x6a714c ->
// ~CAniActor 0x63a930), and with it every manipulator on the actor; each one's Lua table loses its object then.
void EndUnitManipulators(Unit* u);

// Attack commands (CUnitAttackTargetTask), run as the unit's command task.
BuildTask* StartAttackTask(Sim& sim, Unit* u, const UnitCommand& c);
float GuardScanRadiusOf(Unit* u);
Unit* GuardBestEnemy(Sim& sim, Unit* u);
// The patrol's search box (an OBB in xz: centre, unit direction along the leg, half extents across / along).
struct PatrolBox {
  float cx = 0, cz = 0, dx = 0, dz = 1, side = 0, along = 0;
  float cy = 0;  // the centre's height (S.y; the box is 200 high)
};
// CUnitPatrolTask::FindTarget 0x61b710: the primary weapon's best enemy among the cached blips that touch the
// box, within GuardScanRadius.
Unit* PatrolFindTarget(Sim& sim, Unit* u, const PatrolBox& box);
BuildTask* MakeAttackTaskOn(Sim& sim, Unit* u, Entity* e);
// The task of a Nuke or Tactical order: CUnitFireAtTask (ctor 0x60b260, TaskTick 0x60b380), the launch of a
// counted missile from a ManualFire weapon at the order's target.
BuildTask* StartFireAtTask(Sim& sim, Unit* u, const UnitCommand& c);
BuildTask* StartOverchargeTask(Sim& sim, Unit* u, const UnitCommand& c);  // an OverCharge order
int TickFireAt(Sim& sim, Unit* u, BuildTask& t);
void EndFireAt(Sim& sim, Unit* u, BuildTask& t);
int TickAttack(Sim& sim, lua_State* L, Unit* u, BuildTask& t);
void EndAttack(Sim& sim, Unit* u, BuildTask& t);
// Aggressive moves and patrols look for enemies on the way (CUnitPatrolTask): an engaged target
// (0: none) the move stops for.
// The command it was engaging for ended.
void ClearEngagement(Sim& sim, Unit* u);
// CAiAttackerImpl::Stop 0x5d56f0: the unit's desired target is cleared (SetDesiredTarget(none)).
void AttackerStop(Sim& sim, Unit* u);
// The unpack test of the task ctors (move 0x618638, form-and-move 0x619401, patrol 0x61b009, guard, attack
// 0x5f2c22) and of the attack task's Update 0x5f303e and back-off 0x5f38b8: a unit standing unpacked (IsUnitState(Immobile))
// whose blueprint has AI.NeedUnpack (bp+0x48c) and that has an attacker gets SetDesiredTarget(none) (vt+0x20).
void AttackerUnpackStop(Sim& sim, Unit* u);

// UnitWeapon::CanAttackTarget 0x6d5720.
bool WeaponCanAttackTarget(Sim& sim, UnitWeapon* w, const AiTarget& t);
// An entity's set of shooters loses the units that are gone (once a beat, in the entity's own turn).
void PurgeShooters(Sim& sim, Entity* e);
// Unit::Kill (0x6a8090).
void KillUnit(Sim& sim, lua_State* L, Unit* u, Entity* instigator, const std::string& type, float ratio);

// The hits, impacts and health changes a host may ask for (Sim::collectHits / collectImpacts / collectHealth,
// HitEvent / ImpactEvent / HealthEvent in sim/sim_view.h; sim/damage.cpp). Each only reads; callers test the flag.
// The weapon's id in FAF's form, "<unit blueprint id>-<index in bp.Weapon>-<Label>" ("" for none).
std::string WeaponBlueprintId(const UnitWeapon* w);
// kind: ImpactEvent::Kind (0 a shell, 1 a collision beam, 2 a dead unit's crash); met: what the script's OnImpact
// gets as its entity; velocity: the shell's or the fall's (units a second), the beam's axis for a beam
void RecordImpact(Sim& sim, int kind, const Entity* source, const char* type, const Entity* met, Vec3 at, Vec3 velocity);
// e's health (or maximum) has just changed from `before` (a health) / beforeMax (its maximum then)
void RecordHealth(Sim& sim, const Entity* e, float before, float beforeMax);

void RegisterCombatBindings(lua_State* L);
// A unit's pose (Entity::poseQ / poseP): back to rest (CAniPose::UpdateBones 0x54b5f0), and one bone's local
// rotation x q (CAniPoseBone::Rotate 0x54bc00)
void PoseReset(Entity* e);
void PoseRotate(Entity* e, int bone, const Quat& q);
// One step of a rotator (MoveManipulator 0x643860) and of a slider (0x647300), from UnitAimTick's ordered pass.
void RotatorStep(Unit* u, RotateManipulator* r);
void SliderStep(Unit* u, SlideManipulator* m);
// Animation manipulators (sim/anim.cpp). A unit's animators advance and pose its bones with its other
// manipulators, each at its place in the unit's order (AnimatorsCollect drops the ones that are gone and lists
// the rest, AnimatorStep runs one; AnimatorsUpdate runs them all, for a unit without a skeleton); AnimTick runs
// the ones of other entities (time only).
struct AnimatorRef {
  ScriptObject* a;
  int precedence;
  uint32_t seq;
};
void AnimatorsCollect(Sim& sim, Unit* u, std::vector<AnimatorRef>* out);
void AnimatorStep(Sim& sim, Unit* u, ScriptObject* a);
void AnimatorsUpdate(Sim& sim, Unit* u);
// CUnitCommand::GetPosition 0x6e8a30's point without a formation (the target point of a unit target)
Vec3 CommandPosGun(Sim& sim, const UnitCommand& c);
void AnimTick(Sim& sim);
void ReleaseAnimators(Sim& sim);
void RegisterAnimBindings(lua_State* L);

}  // namespace moho
