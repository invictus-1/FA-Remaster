// Units, weapons, props and platoons: the engine objects that scripts create and drive.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sim/entity.h"
#include "sim/motion.h"

namespace moho {

class Unit;
class Platoon;
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

class Unit : public Entity {
 public:
  Unit() { typeBits |= kTypeUnit; }
  std::string layer = "None";
  std::vector<UnitWeapon*> weapons;
  std::set<std::string> unitStates;
  float fractionComplete = 1;
  float capCost = 1;  // General.CapCost: what the unit counts against its army's unit cap
  std::string armorType;  // Defense.ArmorType (multipliers: Sim armour types; used by damage, M4)
  Platoon* platoon = nullptr;
  UnitMotion motion;
  std::deque<std::shared_ptr<UnitCommand>> commands;  // the command queue (sim/commands.cpp)
  NavigatorObject* navigator = nullptr;               // Lua's GetNavigator() object (owned by the sim)
  bool immobile = false;
  int headState = 0;  // progress of the head command (sim/commands.cpp)
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

// Helpers to keep a script value per object (for engine state that only scripts read back).
void SetObjectValue(lua_State* L, ScriptObject* obj, const char* key, int valueIdx);
void PushObjectValue(lua_State* L, ScriptObject* obj, const char* key);  // nil if unset

void RegisterEntityBindings(lua_State* L);
void RegisterEffectBindings(lua_State* L);
void ReleaseEffectObjects();

}  // namespace moho
