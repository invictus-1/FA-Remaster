// Units, weapons, props and platoons: the engine objects that scripts create and drive.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
};

// Blueprint values the engine reads for units, cached per blueprint (sim/economy.cpp).
struct UnitBpData {
  float buildCostEnergy = 0, buildCostMass = 0, buildTime = 0, buildRate = 0;
  float storage[2] = {0, 0};  // StorageEnergy, StorageMass
  bool naturalProducer = false, needToFaceTargetToBuild = false;
  float maxBuildDistance = 0;
  float regenRate = 0, maxHealth = 1;
  float sizeX = 1, sizeY = 1, sizeZ = 1;
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
  uint32_t guardedId = 0;
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
  int parentBone = -1, ownBone = -1;
};

class Prop : public Entity {
 public:
  Prop() { typeBits |= kTypeProp; }
};

class ShieldEntity : public Entity {
 public:
  ShieldEntity() { typeBits |= kTypeShield; }
};

class Projectile : public Entity {
 public:
  Projectile() { typeBits |= kTypeProjectile; }
  Entity* launcher = nullptr;
  Vec3 velocity;
  std::string layer = "None";
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
