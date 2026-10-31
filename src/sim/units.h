// Units, weapons, props and platoons: the engine objects that scripts create and drive.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
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
  int bpIndex = 0;        // 1-based, its entry in bp.Weapon (DummyWeapon entries counted; for a host's lists only)
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
  // The same two sets as entries of Sim::categorySets (-1: the set is empty), for UnitView's WeaponView (ui request
  // 37). Set with them in InitUnitWeapon; nothing changes them later (no script call writes weapon+0xf8 / +0x120).
  int disallowSet = -1, onlyAllowSet = -1;
  int layerCaps = 0;
  float firingRandomness = 0;
  std::vector<std::vector<uint64_t>> priorities;
  struct BlackEntry { uint32_t id; int counter; };  // (weapon +0x160: a WeakPtr to the entity and a count, 12 bytes)
  std::vector<BlackEntry> blacklist;
  int missCount = 0, shots = 0;
  int fireClock = 0;
  // the fire task's thread is off the attacker's run list (CFireWeaponTask::Execute returned -2: disabled or
  // manual fire, clock run down) until SetTarget or SetEnabled(true) links it back in, at the end
  bool fireSuspended = false;
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
  float sacrificeMult[2] = {0, 0};  // Economy.SacrificeEnergyMult (bp+0x560), SacrificeMassMult (bp+0x55c)
  float regenRate = 0, maxHealth = 1;
  bool initialAutoMode = false;  // AI.InitialAutoMode (bp+0x48d)
  float sizeX = 1, sizeY = 1, sizeZ = 1, collisionOffsetY = 0;
  float collisionOffsetX = 0, collisionOffsetZ = 0;
  std::vector<uint64_t> buildable;  // Economy.BuildableCategory (union of the expressions)
  std::string buildRestriction;     // Physics.BuildRestriction
  int buildOnLayerCaps = 1;         // Physics.BuildOnLayerCaps (layer bits)
  float skirtOffsetX = 0, skirtOffsetZ = 0, skirtSizeX = 0, skirtSizeZ = 0;
  std::vector<float> occupyRects;   // Physics.OccupyRects (bp+0x310): centre x, z offsets, half sizes x, z per rect
  bool structure = false;           // no motion type (the build grid applies)
  float maxGroundVariation = 1;
  bool flattenSkirt = false;
  float capCost = 1;
  float initialRallyX = 0, initialRallyZ = 0;
  bool hasBuilder = false;          // it can build (non-empty BuildableCategory)
};
const UnitBpData& GetUnitBpData(lua_State* L, const BlueprintInfo& bp);
// While one lives, a host's view is reading (sim/sim_view.cpp BuildSiteOf): GetUnitBpData reads a blueprint the sim
// has not read yet into a cache of the views' own, through reads that make and check nothing in the state, so the
// sim's own first read of it (its collector checks, a key string it makes) stays where it would be without the
// view. Only for the views, on the thread they run on.
struct ViewReadScope {
  ViewReadScope();
  ~ViewReadScope();
  ViewReadScope(const ViewReadScope&) = delete;
  ViewReadScope& operator=(const ViewReadScope&) = delete;
};
// An EUnitState name (reflection 0x55bba0); Lua calls with any other name raise "Invalid enum value".
bool IsUnitStateName(const char* name);

// The unit's states (unit+0x4a0's EUnitState bits in the original). The string set is the truth and keeps its order
// (the fingerprint walks it); beside it a bit per EUnitState name (the reflection's order, as the recorder's
// kUnitStates) is set and cleared with it, so a test of a known state is one bit, not a walk of a tree of strings
// (B1 at cores: the string tests were 27 s of the sim thread's 729 s, 11.6 s of it in the path searches' unit test).
// A name outside the list (none is inserted today) is asked of the set itself.
constexpr int UnitStateIndex(std::string_view s) {
  switch (s.size()) {
    case 4: if (s == "Busy") return 13; break;
    case 6: if (s == "Moving") return 2; if (s == "Diving") return 17; if (s == "NoCost") return 41; break;
    case 8: if (s == "Immobile") return 1; if (s == "Guarding") return 4; if (s == "Building") return 5; if (s == "MovingUp") return 11; if (s == "Attached") return 14; if (s == "Ferrying") return 20; break;
    case 9: if (s == "Attacking") return 3; if (s == "Upgrading") return 6; if (s == "Repairing") return 16; if (s == "Surfacing") return 18; if (s == "Capturing") return 26; if (s == "Refueling") return 30; if (s == "GuardBusy") return 31; if (s == "Enhancing") return 38; if (s == "NoReclaim") return 40; break;
    case 10: if (s == "MovingDown") return 10; if (s == "Patrolling") return 12; if (s == "Reclaiming") return 28; if (s == "BeingBuilt") return 39; break;
    case 11: if (s == "Teleporting") return 19; if (s == "PathFinding") return 23; if (s == "DoNotTarget") return 34; break;
    case 12: if (s == "WaitForFerry") return 21; if (s == "AssistMoving") return 22; if (s == "UnSelectable") return 33; break;
    case 13: if (s == "BeingCaptured") return 27; if (s == "BeingUpgraded") return 37; break;
    case 14: if (s == "BeingReclaimed") return 15; if (s == "HoldingPattern") return 44; break;
    case 15: if (s == "MakingAttackRun") return 43; break;
    case 16: if (s == "TransportLoading") return 8; if (s == "SiloBuildingAmmo") return 45; break;
    case 17: if (s == "ForceSpeedThrough") return 32; if (s == "LandingOnPlatform") return 35; if (s == "BlockCommandQueue") return 42; break;
    case 18: if (s == "TransportUnloading") return 9; if (s == "AssistingCommander") return 29; break;
    case 19: if (s == "WaitingForTransport") return 7; if (s == "NeedToTerminateTask") return 25; break;
    case 20: if (s == "ProblemGettingToGoal") return 24; break;
    case 21: if (s == "CannotFindPlaceToLand") return 36; break;
    default: break;
  }
  return 0;
}
// Each index's name (UnitStateIndex backwards; nullptr: no state has it).
inline constexpr const char* kUnitStateNameOf[] = {nullptr, "Immobile", "Moving", "Attacking", "Guarding", "Building", "Upgrading", "WaitingForTransport", "TransportLoading", "TransportUnloading", "MovingDown", "MovingUp", "Patrolling", "Busy", "Attached", "BeingReclaimed", "Repairing", "Diving", "Surfacing", "Teleporting", "Ferrying", "WaitForFerry", "AssistMoving", "PathFinding", "ProblemGettingToGoal", "NeedToTerminateTask", "Capturing", "BeingCaptured", "Reclaiming", "AssistingCommander", "Refueling", "GuardBusy", "ForceSpeedThrough", "UnSelectable", "DoNotTarget", "LandingOnPlatform", "CannotFindPlaceToLand", "BeingUpgraded", "Enhancing", "BeingBuilt", "NoReclaim", "NoCost", "BlockCommandQueue", "MakingAttackRun", "HoldingPattern", "SiloBuildingAmmo"};
// A state the source names, as its index, at compile time: US("Moving"). A name that is no EUnitState does not compile.
consteval int US(std::string_view s) {
  const int i = UnitStateIndex(s);
  if (i == 0) throw "not an EUnitState name";
  return i;
}
class UnitStateSet {
 public:
  using Names = std::set<std::string, std::less<>>;
  size_t count(std::string_view s) const {
    const int i = UnitStateIndex(s);
    return i ? (bits_ >> i) & 1u : names_.count(s);
  }
  std::pair<Names::iterator, bool> insert(std::string_view s) {
    if (const int i = UnitStateIndex(s)) bits_ |= uint64_t{1} << i;
    return names_.emplace(s);
  }
  size_t erase(std::string_view s) {
    if (const int i = UnitStateIndex(s)) bits_ &= ~(uint64_t{1} << i);
    auto it = names_.find(s);
    if (it == names_.end()) return 0;
    names_.erase(it);
    return 1;
  }
  // By a known state's index (US("Moving")): the bit alone. For a known name the bit and the set's entry are always set
  // and cleared together (insert / erase above, Insert / Erase here), so the set is touched only when the bit changes:
  // the same set, in the same order, as insert / erase by the name give (speed only, 2026-10-30: the string tests
  // were 2.1% of B1's beat time and 3.8% of B3-16k's).
  bool Has(int i) const { return (bits_ >> i) & 1u; }
  void Insert(int i) {
    if (Has(i)) return;
    bits_ |= uint64_t{1} << i;
    names_.emplace(kUnitStateNameOf[i]);
  }
  size_t Erase(int i) {
    if (!Has(i)) return 0;
    bits_ &= ~(uint64_t{1} << i);
    names_.erase(names_.find(std::string_view(kUnitStateNameOf[i])));
    return 1;
  }
  // MOHO64_VERIFY_STATES (sim.cpp, each beat's end): the bits are what the set holds, for every known name.
  bool BitsAgree() const {
    uint64_t b = 0;
    for (const std::string& n : names_)
      if (const int i = UnitStateIndex(n)) b |= uint64_t{1} << i;
    return b == bits_;
  }
  size_t size() const { return names_.size(); }
  const uint64_t* bitsAddress() const { return &bits_; }  // (a read noted by a speculation: core/speculate.h)
  Names::const_iterator begin() const { return names_.begin(); }
  Names::const_iterator end() const { return names_.end(); }

 private:
  Names names_;
  uint64_t bits_ = 0;
};

class Unit : public Entity {
 public:
  Unit() { typeBits |= kTypeUnit; }
  const UnitBpData* bpData = nullptr;
  std::string layer = "None";
  std::vector<UnitWeapon*> weapons;
  // The attacker's task stage (attacker+0x44): each weapon's fire task (false) and acquire task (true), in the
  // order they run. A fire thread that was suspended goes to the end when it is woken (WakeFireThread).
  std::vector<std::pair<UnitWeapon*, bool>> weaponThreads;
  int weaponThreadPos = -1;  // the thread running now (-1: the stage is not running)
  UnitStateSet unitStates;
  float capCost = 1;  // General.CapCost: what the unit counts against its army's unit cap
  std::string armorType;  // Defense.ArmorType (multipliers: Sim armour types; used by damage, M4)
  std::map<std::string, float> armorOverride;  // Unit:AlterArmor(type, mult)
  Platoon* platoon = nullptr;
  UnitMotion motion;
  std::deque<std::shared_ptr<UnitCommand>> commands;  // the command queue (sim/commands.cpp)
  bool navIgnoreFormation = false;  // CAiNavigatorLand::IgnoreFormation (nav+0x60) set by the attack task
  Vec3 leashPos;                    // u+0x4e8: where an engagement started (patrol) / the guard reference; zero: none
  NavigatorObject* navigator = nullptr;               // Lua's GetNavigator() object (owned by the sim)
  bool navigatorGone = false;  // unit+0x54c deleted (KillCleanup 0x6a8804 / ~Unit 0x6a6cdb): GetNavigator() is nil
  bool immobile = false;
  // entity+0x11c / +0x11d (unit+0x124 / +0x125): the unit stands and paths with its AltFootprint (UnitFootprint,
  // sim/motion.h). +0x124 is the navigator's: UpdateWaterFavorAltFootprintMode 0x5adc70 sets it for a FAVORSWATER
  // unit whose whole move is on water, ResetPathState 0x5ad9c0 (0x5ada33) clears it. +0x125 is the script's: FAF's
  // Unit:ForceAltFootprint(bool) 0x128dd70 (the Salem's weapon toggle, urs0201_script.lua:176 / 186).
  bool altFootprintNav = false, altFootprintForced = false;
  // unit+0x57c: the terrain type code under the unit, as Unit::UpdateTerrainType 0x6a9b50 last read it (the
  // constructor starts it at 0, 0x6a5651; UpdateTerrainType in sim/motion.h)
  uint8_t terrainType = 0;
  int headState = 0;  // progress of the head command (sim/commands.cpp)
  int headType = 0;   // the head command's type when it was dispatched (CommandType; sim/commands.cpp HeadTaskType)
  // The command dispatcher's result word (IAiCommandDispatchImpl +0x2c): 0 when it dispatches (0x598fc4) and when a
  // task is made with it as parent (CCommandTask ctor 0x5f095f); a child's end writes its result there through its
  // +0x28 (a patrol's repair: 1 done, else 2, ~CUnitRepairTask 0x5f8f40; its reclaim: 1 when the target is gone,
  // else 2, 0x6200b3..0x6200ce). A patrol leg that ends while it holds 2 starts the next leg in the same beat.
  int dispatchResult = 0;

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
  int capturers = 0;               // u+0x690: capture tasks at work on this unit
  // u+0x560: the unit occupies ground (Unit::Unit 0x6a5f71 after ExecuteOccupyGround; cleared by ReleaseOccupyGround
  // 0x6a7b81 and by a finished upgrade's dtor 0x5f8581 on the old unit). Unit::OnDestroy frees the o-grid reservation
  // only while it is set (0x6a8905).
  bool occupiesGround = false;
  bool isPod = false;              // u+0x688: in category POD or STATIONASSISTPOD (pod_assist.md)
  bool isEngineer = false;         // u+0x689: in category ENGINEER (ctor 0x6a6a0a): never in a guard formation
  // set while the queue is cleared for a new order under which CUnitMoveTask's destructor (0x618700, the table
  // 0x618998) does NOT abort the navigator: a task whose move child is torn down then leaves the move running
  bool clearKeepsMove = false;
  // the head Move task reserved its spot in the o-grid (CUnitMoveTask +0x92: a factory's rally Move put through
  // PrepareMove, ctor 0x618622); its dtor frees it (0x618891)
  bool moveTaskReserved = false;
  // a task's walking child (a CUnitMoveTask) took the factory rally override in its ctor (0x618a00 -> PrepareMove
  // 0x618521): +0x95, so its command listener 0x618c40 puts a new target through PrepareMove too (MoveTaskWalk)
  bool walkPrepared = false;
  // when its last task walk (a CUnitMoveTask: its listener linked into the head command's list) was made (Sim::walkSeqNext)
  uint64_t walkSeq = 0;
  bool isFactoryBuilder = false;
  uint32_t creatorId = 0;              // unit+0x4b8: the factory, engineer or upgrading unit that built it       // builder.IsFactory (a builder in category FACTORY)       // unit+0x4f8: the units guarding this one, ascending id
  std::vector<uint64_t> buildAllowed;  // per-unit build restrictions removed from "all"
  bool busy = false, blockCommandQueue = false, paused = false;
  int siloAmmo[2] = {0, 0};  // tactical, nuke missiles in store
  struct SiloState* silo = nullptr;  // missile production (sim/build.cpp)
  bool autoMode = false;
  // set by the info pairs SetRepeatQueue / SetAutoSurfaceMode. repeatQueue (unit+0x2a2) is acted on by the command
  // dispatcher (a factory's finished order goes back to the end of its queue); autoSurfaceMode is not acted on yet
  bool repeatQueue = false, autoSurfaceMode = false;
  Vec3 rallyPoint;
  bool hasRallyPoint = false;
  struct BuildTask* task = nullptr;   // the build-like command it is carrying out
  // Factories: commands for the units they build (rally point, patrols): Issue* of move-like
  // commands to a factory land here; a finished unit takes them over.
  std::deque<std::shared_ptr<UnitCommand>> factoryCommands;
  uint32_t parentId = 0;           // attached to (AttachBoneTo / AttachTo)
  uint64_t attachSeq = 0;          // when it took that parent (Sim::NextAttachSeq); only the views read it
  uint32_t scriptBits = 0;         // u+0x498: the toggle buttons' state (sim/script_bits.cpp)
  int64_t toggleCaps = -1;         // u+0x48c: live toggle caps (-1 = not yet read from the blueprint)
  int64_t commandCaps = -1;        // u+0x488: live command caps (-1 = still the blueprint's General.CommandCaps)
  bool attachFull = false;         // carried by a transport: full attach transform, parent's layer
  std::shared_ptr<struct TransportObj> transport;  // transports (sim/transport.cpp)
  uint32_t transportedBy = 0;      // the transport carrying it (entity ref)
  uint64_t cmdSeq = 0;  // command-thread order (Sim::CommandOrder)
  uint64_t entSeq = 0;  // creation order (the army stages go by it)
  // The entity task thread's place in stage sim+0x930 (Sim::MotionOrder): creation order, until the unit is
  // attached to a parent whose turn comes later in the pass. Then Entity::TaskTick answers -4 and the thread is
  // linked at the run list's new end, for good (orderHandle: that place among the creation handles;
  // orderedParent: the parent this has been settled for).
  uint64_t motSeq = 0;
  uint32_t orderHandle = 0, orderedParent = 0;
  Vec3 attachCp;  // the own bone's local transform the attach of beat attachCTick took (AttachedUnitsTick)
  Quat attachCq;
  uint32_t attachCTick = 0;
  // Its place in the sim's dirty-entity list sim+0xa5c (node Entity+0x60; Sim::DirtyOrder): linked when it is made
  // (Entity ctor 0x677c2d), moved to the list's newest end by every SetHealth (0x679a3c..0x679a6b, no test). The
  // transform's own link (SetPendingTransform 0x678ed9) only links an entity that is not on the list.
  uint64_t dirtySeq = 0;
  uint32_t detachedAt = 0xffffffffu;  // the beat a transport let it go (its order queue runs from the next one)
  float fuelRatio = -1;            // u+0x294 (fuel.md): -1 none; 1 at spawn for FuelUseTime > 0
  uint32_t ferryUnit = 0;          // GetFerryUnit (+0x4c8): the ferry assigned to pick it up
  int ogridRect[4] = {0, 0, 0, 0}; // a ground unit's reserved o-grid rect (ReserveOgridRect)
  int parentBone = -1, ownBone = -1;
  float shieldRatio = 0;
  // AimMuzzlesAhead's walk of this beat (MOHO64_PAR_AIM): its beat and what it walked from
  uint32_t aimAheadTick = 0xffffffffu, aimAheadPoseVersion = 0;
  Vec3 aimAheadP0;
  Quat aimAheadR0;
  const Skeleton* aimAheadSkeleton = nullptr;
  float aimAheadScale = 0;  // meshScale x scale[0]
  AimController* const* aimAheadList = nullptr;
  size_t aimAheadCount = 0;
  uint32_t aimPoseTick = 0xffffffffu;  // the beat AimReadLastPose last ran in  // as the shield's script last gave Unit:SetShieldRatio (for the views; no simulation use)
  // Combat (sim/combat.cpp)
  const struct CombatBpData* combat = nullptr;
  AiTarget desiredTarget;          // set by attack commands (the attacker's desired target)
  int attackState = 0;             // the reporting weapon's state code (see TickAttack)
  bool attackStateSignal = false;  // changed since the attack task last looked
  int fireState = 0;               // 0 ReturnFire, 1 HoldFire, 2 HoldGround
  // u+0x29c: the beats the stun still lasts (Unit:SetStunned 0x6c68e0 writes (int)(seconds * 10); Unit::MotionTick
  // 0x6a906e counts it down, before the motion). Stunned while it is not 0.
  int stunned = 0;
  uint8_t recon[16] = {};          // per army index: Radar 1, Sonar 2, Omni 4, LOSNow 8, LOSEver 0x10
  bool killCleanup = false;        // killed: weapons and commands go at the next beat
  bool combatGone = false;         // ... and they went
  std::vector<AimController*> aimControllers;
  // The world transform of the unit's pose (CAniPose, set by the Unit constructor and then once per beat by
  // UpdateManipulators 0x63aa80 after the unit's motion): its bones hang on this, not on a transform a script
  // set since. prevPoseBase*: last beat's pose (actor+8), which aim controllers and builder arms measure from.
  Vec3 poseBasePos, prevPoseBasePos;
  Quat poseBaseOri, prevPoseBaseOri;
  bool poseSet = false;
  std::vector<class SlideManipulator*> sliders;
  std::vector<class SlaveManipulator*> slavers;  // CreateSlaver
  std::vector<class CollisionManipulator*> colliders;  // CreateCollisionDetector
  std::vector<class FootPlantManipulator*> footPlants;  // CreateFootPlantController
  std::vector<class ThrustManipulator*> thrusters;      // CreateThrustController
  std::vector<ScriptObject*> animators;  // its CAnimationManipulators, in creation order (sim/anim.cpp)
  std::vector<class RotateManipulator*> rotators;
  std::vector<class BuilderArm*> builderArms;
  // formation (Unit::UpdateInfoCache, formations.md 2.5)
  struct Formation* form = nullptr;  // u+0x580
  // keeps u+0x580's formation alive until UpdateInfoCache writes the field again: the formation of a command the unit
  // let go of lives on with the command object (deleted at a beat's end), and u+0x580 still points at it
  std::shared_ptr<struct Formation> formHold;
  int formPathDelay = 1;             // u+0x58c
  uint32_t formLeader = 0;           // u+0x584 (entity handle)
  Vec3 formSlot;                     // u+0x59c
  float formRank = 0;                // u+0x598
  bool formAllAtGoal = false;        // u+0x590
  std::shared_ptr<struct Formation> guardForm;  // G+0x520: the guard formation of its guards
  Vec3 armAim;                     // CAiBuilderImpl +0xc: the build arm's aim target (zero: none)
  bool armReady = true;            // CAiBuilderImpl +9: the arm is on target (no arm: always)
  std::vector<Unit*> blipCache;    // enemies its army has a blip on, within its weapons' reach
  std::vector<uint8_t> blipCacheBlip;  // per entry: 1 = the entry is that unit's recon BLIP (a jammer's), not the unit
  std::vector<uint32_t> blipCacheFake; // per entry: the jammer's FAKE blip the entry is (its entity handle), else 0
  uint32_t blipCacheTick = 0;
  bool blipCacheValid = false;
  // MOHO64_PAR_NAV: the navigator's turn played ahead is item navSpecItem of round navSpecRound (sim/landnav.cpp)
  uint32_t navSpecItem = 0, navSpecRound = 0xffffffffu;
  Vec3 lastPosition;               // position at the start of the tick (blacklist reset)
  Quat lastOrientation;            // orientation at the start of the tick (with lastPosition)
  Quat prevLastOrientation;        // ... and at the start of the tick before (the entity's previous transform)
  Vec3 prevLastPosition;
  uint32_t lastPosTick = 0;        // the tick lastPosition / lastOrientation were taken (motion loop)
  int createLayer = 0;             // layer bits it was made in (what its navigator's +0x54 starts from)
  class ReconBlip* blip = nullptr;  // the blip armies hold of it (sim/intel.cpp)
  std::vector<class ReconBlip*> fakeBlips;  // a jammer's fake blips (shared by the armies that hold them)
  bool everMobileChecked = false;
  uint32_t engageId = 0;           // aggressive move / patrol: the enemy it stopped for
  int engageCheck = 0;
};

class Prop : public Entity {
 public:
  Prop() { typeBits |= kTypeProp; }
  // For PropEvents (sim/sim_view.h): its place in the map file's prop list (-1: made after the start), and what
  // the host was last told of it.
  int mapIndex = -1;
  bool viewSent = false;
  Vec3 viewPos;
  Quat viewOri;
  float viewScale[4] = {1, 1, 1, 1};  // scale x, y, z and the draw scale
  std::string viewMesh;
  // In the occupancy grid (prop+0x278, Prop::Prop 0x6f9f48): its blueprint gives reclaim. Footprint size and
  // OccupancyCaps of its blueprint (bp+0xd8 / +0xd9 / +0xda).
  bool occupies = false;
  uint8_t occSizeX = 0, occSizeZ = 0, occCaps = 0;
  // Its blueprint's Economy.ReclaimMassMax / ReclaimEnergyMax (bp+0x1a4 / +0x1a8), as Prop::Prop reads them.
  float bpReclaimMass = 0, bpReclaimEnergy = 0;
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
  // A jammer's fake blip (blip+0x294): its place relative to the source (+0x27c, drawn in the ctor) and, per army
  // index, that army's recon flags with kReconInUse. (A real blip's flags are the unit's Unit::recon.)
  bool fake = false;
  Vec3 offset;
  uint8_t rflags[16] = {};
  // Its committed previous place (entity +0xc8): ReconBlip::Refresh 0x5bf810 gives the blip the unit's committed
  // transform as its current one and the unit's pending one as its pending one, so after AdvanceCoords the previous
  // place is where the unit stood when the beat began. The blip's GetVelocity (vtable slot 15 = Entity::GetVelocity
  // 0x678dc0) is position - prevPos: the unit's move of the beat before.
  Vec3 prevPos;
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
  // The size its mesh is drawn at, per axis (the original's entity scale, +0x84): UniformScale +- MeshScaleRange
  // at launch, + scaleVel x 0.1 every beat (MotionTick 0x69be40), replaced outright by SetScale / SetDrawScale.
  // For a host that draws (ProjectileView::size); nothing in the simulation goes by it.
  Vec3 drawSize{1, 1, 1};
  Vec3 prevPos;
  Vec3 prevPosBefore;         // prevPos before its last step (the committed position before the one its step began from)
  uint32_t prevPosTick = 0;   // the beat that step set prevPos in
  Vec3 heldVel;  // its velocity as the beat began (taken before its own step: what another shell leads it by)
  uint32_t steppedTick = 0;  // the beat of its last step (ProjectileStep: once per beat, between the units)
  uint32_t queuedInStepTick = 0;  // the beat its own step queued its Destroy (an impact)
  bool warpedInStep = false;      // a script warped it inside its own step's sweep (l_Warp; MotionTick keeps that place)
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

// class Platoon: sim/ai_platoon.h

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
// A unit's bone -1, Unit::GetBoneWorldTransform 0x6aa5c0 with bone -1 (0x6aa637..0x6aa705): half the blueprint's
// SizeY (bp+0xb0 x 0.5, 0x6aa67c) straight above `at` (the unit's place, or the place of what it rides), in the
// world's axes: the offset is the outer transform of the composition 0x549c20, so the unit's turn does not move it.
// What hangs on a unit's bone -1 (a shield, a drone's guard dummy, a carried unit) and a collision beam's length
// to a unit (CreateCollisionBeamHelper 0x6d6f30) are measured from it.
inline Vec3 UnitBoneMinus1(const Unit& u, const Vec3& at) {
  if (!u.motion.bp) return at;
  return Vec3{at.x + 0.0f, at.y + u.motion.bp->sizeY * 0.5f, at.z + 0.0f};
}
// A unit under construction was finished (after OnStopBeingBuilt): adjacency (sim/build.cpp).
void UnitFinishedBuilding(Sim& sim, lua_State* L, Unit* u);

// Helpers to keep a script value per object (for engine state that only scripts read back).
void SetObjectValue(lua_State* L, ScriptObject* obj, const char* key, int valueIdx);
void PushObjectValue(lua_State* L, ScriptObject* obj, const char* key);  // nil if unset
void PeekObjectValue(lua_State* L, ScriptObject* obj, const char* key);  // the same for a host's view: makes nothing
// The object is deleted: its values go with it (the original keeps them in the object itself). Makes nothing.
void DropObjectValues(lua_State* L, ScriptObject* obj);

void RegisterEntityBindings(lua_State* L);
// The entity's texture scroller's step (entity.h TextureScroller; CTextureScroller::Tick 0x777730), first thing in
// its own turn: cur / prev are its committed transform and the one before it (entity+0x9c / +0xb8).
void TextureScrollerTick(Entity* e, const Vec3& curPos, const Quat& curOri, const Vec3& prevPos, const Quat& prevOri);
void RegisterEffectBindings(lua_State* L);
void ReleaseEffectObjects(Sim& sim);
// The effect manager's tick (AdvanceBeat step 11) and the end of the beat (steps 16-17 and the manipulators of purged
// units): what ends the objects the effect and manipulator factories made (sim/effects.cpp).
void EffectsTick(Sim& sim);
void EffectsEndOfBeat(Sim& sim);
// For MOHO64_STATS: the factories' objects not yet deleted, the effect manager's active effects, the decals queued to
// expire.
void EffectObjectCounts(const Sim& sim, size_t* objects, size_t* effects, size_t* decals);
// A script played a sound once on e (the sound object at stack index arg): listed for the host while
// Sim::collectSounds is set (sim/effects.cpp).
void RecordPlaySound(lua_State* L, const Entity* e, int arg);

}  // namespace moho
