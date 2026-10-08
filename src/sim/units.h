// Units, weapons, props and platoons: the engine objects that scripts create and drive.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/entity.h"

namespace moho {

class Unit;
class Platoon;

class UnitWeapon : public ScriptObject {
 public:
  Unit* unit = nullptr;
  int index = 0;          // 1-based, as GetWeapon(i)
  std::string label;
  int bpRef = LUA_NOREF;  // the weapon's blueprint table (bp.Weapon[i])
  bool enabled = true;
};

class Unit : public Entity {
 public:
  std::string layer = "None";
  std::vector<UnitWeapon*> weapons;
  std::set<std::string> unitStates;
  float fractionComplete = 1;
  std::string armorType;  // Defense.ArmorType (multipliers: Sim armour types; used by damage, M4)
  Platoon* platoon = nullptr;
};

class Prop : public Entity {};

class ShieldEntity : public Entity {};

class Projectile : public Entity {
 public:
  Entity* launcher = nullptr;
  Vec3 velocity;
  std::string layer = "None";
};

class Platoon : public ScriptObject {
 public:
  Army* army = nullptr;
  std::string name;  // unique name ("ArmyPool")
  std::string plan;
  std::vector<Unit*> units;
};

// Helpers to keep a script value per object (for engine state that only scripts read back).
void SetObjectValue(lua_State* L, ScriptObject* obj, const char* key, int valueIdx);
void PushObjectValue(lua_State* L, ScriptObject* obj, const char* key);  // nil if unset

void RegisterEntityBindings(lua_State* L);
void RegisterEffectBindings(lua_State* L);
void ReleaseEffectObjects();

}  // namespace moho
