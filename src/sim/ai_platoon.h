// Platoons: the groups the classic AI brains (and the scenario scripts) order their units in. The original's
// CPlatoon (vtable 0xe31ab4, 0x110 bytes, ctor 0x724cc0) holds its units in squads (CSquad, 0x60 bytes, ctor
// 0x723e70), one per squad class; every army keeps its platoons in a list (CArmyImpl+0x228) and owns one
// "ArmyPool" platoon that every new unit joins. The orders (MoveToLocation, AttackTarget, ...) go to a squad's
// units through UNIT_IssueCommand 0x6f12c0 like the Lua Issue* functions. Read from the FA exe (build 3839)
// 2026-10-15; the addresses are in sim/ai_platoon.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/lua.hpp"
#include "sim/script_object.h"

namespace moho {

class Army;
class Sim;
class Unit;

// ESquadClass (reflection 0x723bd0, prefix SQUADCLASS_): what Lua names a squad by.
enum SquadClass : int {
  kSquadUnassigned = 0, kSquadAttack = 1, kSquadArtillery = 2, kSquadGuard = 3, kSquadSupport = 4, kSquadScout = 5,
  kSquadAll = 6,  // (not an enum value: the default of Stop, Destroy, the orders: "every squad but Unassigned")
};

// CSquad (0x60 bytes, ctor 0x723e70).
struct Squad {
  int cls = kSquadUnassigned;  // +0x30
  std::string formation;       // +0x34: the formation its orders ask for (a script name; '' none)
  std::vector<Unit*> units;    // +0x08: an EntitySet (ascending entity id); a unit leaves it when it is purged
  std::vector<std::vector<uint64_t>> priorities;  // +0x54: SetPrioritizedTargetList's categories, in order
};

class Platoon : public ScriptObject {
 public:
  Platoon() { typeBits |= kTypePlatoon; }
  Army* army = nullptr;           // +0x38
  std::string name;               // +0x70: MakePlatoon's name (a template's [1])
  std::string plan;               // +0x8c: the AI plan; the script's OnCreate(plan) starts it
  std::string uniqueName;         // +0xa8: UniquelyNamePlatoon ('ArmyPool' for the pool)
  std::string formationOverride;  // +0xc4: SetPlatoonFormationOverride; when set, orders go to all units at once
  std::vector<std::unique_ptr<Squad>> squads;  // +0x40, in creation order
  bool disbandOnIdle = false;     // +0xe0
  int kills = 0, losses = 0;      // +0xe4, +0xe8 (GetPlatoonLifetimeStats)
  float damageDealt = 0, damageTaken = 0;  // +0xec, +0xf0
  int unitsRef = LUA_NOREF;       // +0xf4: the table GetPlatoonUnits hands out, kept until the squads change
  bool unitsValid = false;        // +0x108
};

// The army's pool, made by the army's ctor (CArmyImpl ctor 0x6ff179): MakePlatoon('Pool', 'PoolAI'), an Unassigned
// squad, unique name 'ArmyPool'.
Platoon* CreateArmyPool(Sim& sim, lua_State* L, Army* army);
// A new unit joins its army's pool (Unit ctor 0x6a63e6: CArmyImpl::AssignUnitsToPlatoon({u}, 'ArmyPool')).
void AssignNewUnitToPool(Army* army, Unit* u);
// A unit's entity is purged (destroy queue): it leaves every squad it is in (the squads' EntitySets).
void PlatoonsForgetUnit(Sim& sim, Unit* u);
// NotifyUpgrade(from, to) 0x6cd0c7: `to` takes `from`'s place in its platoon and squad.
void PlatoonsNotifyUpgrade(Unit* from, Unit* to);
// CArmyImpl::OnTick 0x6ffdb8 (before the army's economy): CleanUpPlatoons 0x700820.
void CleanUpPlatoons(Sim& sim, Army& army);
// The platoons' lifetime statistics (damage.md 4, 14, 15): damage a unit dealt / took, a unit killed / lost.
void PlatoonStatDamage(Unit* dealer, Unit* receiver, float damage);
void PlatoonStatKill(Unit* victim, Unit* killer);

void RegisterPlatoonBindings(lua_State* L);

}  // namespace moho
