// Units, weapons, props and platoons: the engine objects that scripts create and drive.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sim/combat.h"
#include "sim/economy.h"
#include "sim/entity.h"
#include "sim/motion.h"

namespace moho {

class Unit;
class Platoon;
class Sim;
struct UnitCommand;
class NavigatorObject;

class UnitWeapon : public ScriptObject {
 public:
  UnitWeapon() { typeBits |= kTypeWeapon; }
  Unit* unit = nullptr;
  int index = 0;          // 1-based, as GetWeapon(i)
  std::string label;
  int bpRef = LUA_NOREF;  // the weapon's blueprint table (bp.Weapon[i])
  bool enabled = true;
  // Combat state (sim/combat.cpp; the original's UnitWeapon, ctor 0x6d4310)
  const WeaponBp* bp = nullptr;
  const BlueprintInfo* projBp = nullptr;
  // per-instance overrides (< 0: the blueprint's value; MaxHeightDiff starts at +inf)
  float ovFiringTolerance = -1, ovRateOfFire = -1, ovMinRadius = -1, ovMaxRadius = -1;
  float minR2 = 0, maxR2 = 0;
  float ovMaxHeightDiff = 0;  // set to +inf at init
  std::string ovDamageType;
  float ovDamageRadius = -1, ovDamage = -1;
  int aimBone = -1;
  std::string fireControl = "Default";
  AiTarget target;
  bool onTarget = true;     // aim on-target flag (+0xf0)
  bool canReach = true;     // has a firing solution (+0x174)
  Vec3 solution;            // firing solution direction (+0x178), NaN = none
  bool hasSolution = false;
  std::vector<uint64_t> disallow, onlyAllow;  // category sets (empty: none)
  int layerCaps = 0;
  float firingRandomness = 0;
  std::vector<std::vector<uint64_t>> priorities;
  struct BlackEntry { uint32_t id; Vec3 pos; int counter; };
  std::vector<BlackEntry> blacklist;
  int missCount = 0, shots = 0;
  int fireClock = 0;
  uint32_t nextAcquire = 0;  // tick the acquire task runs next
  int suppress = 0;
  bool firstAcquire = true;
  std::vector<class AimController*> aimControllers;
};

// Blueprint values the engine reads for units, cached per blueprint (sim/economy.cpp).
struct UnitBpData {
  float buildCostEnergy = 0, buildCostMass = 0, buildTime = 0, buildRate = 0;
  float storage[2] = {0, 0};  // StorageEnergy, StorageMass
  bool naturalProducer = false, needToFaceTargetToBuild = false;
  float maxBuildDistance = 0;
  float regenRate = 0, maxHealth = 1;
  float sizeX = 1, sizeY = 1, sizeZ = 1, collisionOffsetY = 0;
  std::vector<uint64_t> buildable;  // Economy.BuildableCategory (union of the expressions)
  std::string buildRestriction;     // Physics.BuildRestriction
  int buildOnLayerCaps = 1;         // Physics.BuildOnLayerCaps (layer bits)
  float skirtOffsetX = 0, skirtOffsetZ = 0, skirtSizeX = 0, skirtSizeZ = 0;
  bool structure = false;           // no motion type (the build grid applies)
  float maxGroundVariation = 1;
  bool flattenSkirt = false;
  float capCost = 1;
  float initialRallyX = 0, initialRallyZ = 0;
  bool hasBuilder = false;          // it can build (non-empty BuildableCategory)
};
const UnitBpData& GetUnitBpData(lua_State* L, const BlueprintInfo& bp);

class Unit : public Entity {
 public:
  Unit() { typeBits |= kTypeUnit; }
  const UnitBpData* bpData = nullptr;
  std::string layer = "None";
  std::vector<UnitWeapon*> weapons;
  std::set<std::string> unitStates;
  float capCost = 1;  // General.CapCost: what the unit counts against its army's unit cap
  std::string armorType;  // Defense.ArmorType (multipliers: Sim armour types; used by damage, M4)
  std::map<std::string, float> armorOverride;  // Unit:AlterArmor(type, mult)
  Platoon* platoon = nullptr;
  UnitMotion motion;
  std::deque<std::shared_ptr<UnitCommand>> commands;  // the command queue (sim/commands.cpp)
  NavigatorObject* navigator = nullptr;               // Lua's GetNavigator() object (owned by the sim)
  bool immobile = false;
  int headState = 0;  // progress of the head command (sim/commands.cpp)

  // Economy (sim/economy.cpp): per-second rates as scripts set them, the request that claims the
  // consumption from the army, and this beat's figures.
  float consumption[2] = {0, 0}, production[2] = {0, 0};
  bool consumptionActive = false, productionActive = false;
  std::shared_ptr<EconRequest> request;
  float resourceConsumed = 0;  // GetResourceConsumed: fraction of the request granted
  bool storageOn = false;      // its blueprint storage counts toward the army's maximum
  float storageCounted[2] = {0, 0};
  float producedTick[2] = {0, 0}, consumedTick[2] = {0, 0}, consumptionShown[2] = {0, 0};
  float buildRate = 0, regenRate = 0;
  float workProgress = 0;          // GetWorkProgress
  bool beingBuilt = false;         // under construction (fractionComplete < 1 until finished)
  uint32_t lastMaterializeTick = 0;
  uint32_t builderId = 0;          // the unit that started building it
  std::vector<EconomyEvent*> econEvents;
  // Building (sim/build.cpp)
  uint32_t focusId = 0;            // GetFocusUnit: what it builds, repairs, reclaims, ...
  uint32_t guardedId = 0;               // unit+0x4e0 (SetGuardedUnit only)
  std::vector<uint32_t> guarders;
  bool isFactoryBuilder = false;
  uint32_t creatorId = 0;              // unit+0x4b8: the factory, engineer or upgrading unit that built it       // builder.IsFactory (a builder in category FACTORY)       // unit+0x4f8: the units guarding this one, ascending id
  std::vector<uint64_t> buildAllowed;  // per-unit build restrictions removed from "all"
  bool busy = false, blockCommandQueue = false, paused = false;
  int siloAmmo[2] = {0, 0};  // tactical, nuke missiles in store
  struct SiloState* silo = nullptr;  // missile production (sim/build.cpp)
  bool autoMode = false;
  Vec3 rallyPoint;
  bool hasRallyPoint = false;
  struct BuildTask* task = nullptr;   // the build-like command it is carrying out
  // Factories: commands for the units they build (rally point, patrols): Issue* of move-like
  // commands to a factory land here; a finished unit takes them over.
  std::deque<std::shared_ptr<UnitCommand>> factoryCommands;
  uint32_t parentId = 0;           // attached to (AttachBoneTo / AttachTo)
  bool attachFull = false;         // carried by a transport: full attach transform, parent's layer
  std::shared_ptr<struct TransportObj> transport;  // transports (sim/transport.cpp)
  uint32_t transportedBy = 0;      // the transport carrying it (entity ref)
  uint64_t cmdSeq = 0;  // command-thread order (Sim::CommandOrder)
  float fuelRatio = -1;            // u+0x294 (fuel.md): -1 none; 1 at spawn for FuelUseTime > 0
  uint32_t ferryUnit = 0;          // GetFerryUnit (+0x4c8): the ferry assigned to pick it up
  int ogridRect[4] = {0, 0, 0, 0}; // a ground unit's reserved o-grid rect (ReserveOgridRect)
  int parentBone = -1, ownBone = -1;
  // Combat (sim/combat.cpp)
  const struct CombatBpData* combat = nullptr;
  AiTarget desiredTarget;          // set by attack commands (the attacker's desired target)
  int attackState = 0;             // the reporting weapon's state code (see TickAttack)
  bool attackStateSignal = false;  // changed since the attack task last looked
  int fireState = 0;               // 0 ReturnFire, 1 HoldFire, 2 HoldGround
  bool stunned = false;
  uint8_t recon[16] = {};          // per army index: Radar 1, Sonar 2, Omni 4, LOSNow 8, LOSEver 0x10
  bool killCleanup = false;        // killed: weapons and commands go at the next beat
  bool combatGone = false;         // ... and they went
  std::vector<AimController*> aimControllers;
  std::vector<class RotateManipulator*> rotators;
  std::vector<class BuilderArm*> builderArms;
  // formation (Unit::UpdateInfoCache, formations.md 2.5)
  struct Formation* form = nullptr;  // u+0x580
  int formPathDelay = 1;             // u+0x58c
  uint32_t formLeader = 0;           // u+0x584 (entity handle)
  Vec3 formSlot;                     // u+0x59c
  float formRank = 0;                // u+0x598
  bool formAllAtGoal = false;        // u+0x590
  Vec3 armAim;                     // CAiBuilderImpl +0xc: the build arm's aim target (zero: none)
  bool armReady = true;            // CAiBuilderImpl +9: the arm is on target (no arm: always)
  std::vector<Unit*> blipCache;    // enemies its army has a blip on, within its weapons' reach
  uint32_t blipCacheTick = 0;
  bool blipCacheValid = false;
  Vec3 lastPosition;               // position at the start of the tick (blacklist reset)
  class ReconBlip* blip = nullptr;  // the blip armies hold of it (sim/intel.cpp)
  bool everMobileChecked = false;
  uint32_t engageId = 0;           // aggressive move / patrol: the enemy it stopped for
  int engageCheck = 0;
};

class Prop : public Entity {
 public:
  Prop() { typeBits |= kTypeProp; }
};

class ShieldEntity : public Entity {
 public:
  ShieldEntity() { typeBits |= kTypeShield; }
};

// ReconBlip (sim/combat.cpp): what one army knows of a unit.
class ReconBlip : public Entity {
 public:
  Unit* source = nullptr;
  bool mobile = false;
};

// CollisionBeamEntity (sim/combat.cpp): a weapon's beam, attached to its unit's muzzle bone.
class CollisionBeam : public Entity {
 public:
  UnitWeapon* weapon = nullptr;
  int interval = 1;     // CollisionCheckInterval: checks every interval + 1 ticks
  float length = 0;
  bool enabled = true;
  int counter = 0;
};

class Projectile : public Entity {
 public:
  Projectile() { typeBits |= kTypeProjectile; }
  Entity* launcher = nullptr;
  Vec3 velocity;  // units per second
  std::string layer = "None";
  // Motion (sim/projectile.cpp; the original's Projectile, ctor 0x69afe0)
  Vec3 angVel, scaleVel;
  Vec3 prevPos;
  float velScale = 1;
  float hitFraction = -1;
  bool collideSurface = true, collideEntity = true, trackTarget = false, velocityAlign = true, stayUpright = false,
       leadTarget = true, stayUnderwater = false, destroyOnWater = false;
  float turnRate = 0, maxSpeed = 0, accel = 0;
  Vec3 ballisticAccel;
  float damage = 0, damageRadius = 0;
  std::string damageType = "Normal";
  AiTarget target;
  Vec3 lastTargetPos;
  bool homeLastPos = false;
  Vec3 hitPos;
  uint32_t hitEntity = 0;
  uint32_t expireTick = 0;
  bool underwater = false;
  int bounceLimit = 0, bounces = 0;
  bool bouncePending = false;
  Vec3 bouncedVel;
  float bounceDamp = 0.5f;
  uint32_t nextZigZag = 0;
  Vec3 zigOffset;
  int impactType = 3;
  float ovMaxZigZag = -1, ovZigZagFreq = -1, ovDetAbove = -1, ovDetBelow = -1;
  bool ignoresAlly = true;
  UnitWeapon* weapon = nullptr;  // created by (miss tracking)
};

class Platoon : public ScriptObject {
 public:
  Platoon() { typeBits |= kTypePlatoon; }
  Army* army = nullptr;
  std::string name;  // unique name ("ArmyPool")
  std::string plan;
  std::vector<Unit*> units;
};

template <> struct ScriptTypeOf<Unit> { static constexpr uint32_t bit = kTypeUnit; };
template <> struct ScriptTypeOf<UnitWeapon> { static constexpr uint32_t bit = kTypeWeapon; };
template <> struct ScriptTypeOf<Prop> { static constexpr uint32_t bit = kTypeProp; };
template <> struct ScriptTypeOf<ShieldEntity> { static constexpr uint32_t bit = kTypeShield; };
template <> struct ScriptTypeOf<Projectile> { static constexpr uint32_t bit = kTypeProjectile; };
template <> struct ScriptTypeOf<Platoon> { static constexpr uint32_t bit = kTypePlatoon; };

// Health changes (Entity::AdjustHealth 0x679860 / SetHealth 0x679940): clamped to [0, max];
// OnHealthChanged(new, old) runs when the health crosses a quarter of the maximum.
void EntityAdjustHealth(lua_State* L, Entity* e, Entity* instigator, float amount);
// World position of an entity's bone at rest (-1: the entity itself).
Vec3 EntityBonePosition(const Entity* e, int bone);
// A unit under construction was finished (after OnStopBeingBuilt): adjacency (sim/build.cpp).
void UnitFinishedBuilding(Sim& sim, lua_State* L, Unit* u);

// Helpers to keep a script value per object (for engine state that only scripts read back).
void SetObjectValue(lua_State* L, ScriptObject* obj, const char* key, int valueIdx);
void PushObjectValue(lua_State* L, ScriptObject* obj, const char* key);  // nil if unset

void RegisterEntityBindings(lua_State* L);
void RegisterEffectBindings(lua_State* L);
void ReleaseEffectObjects();

}  // namespace moho
