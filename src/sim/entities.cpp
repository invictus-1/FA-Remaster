// Entity, unit, weapon, prop and platoon creation and their script bindings.
//
// Unit creation follows the original Unit constructor (FA exe 0x6a53f0, read 2026-10-08):
//   script object (blueprint's script class) -> OnPreCreate -> layer set (OnLayerChange) ->
//   weapons (each weapon's class from the unit class's Weapons table; weapon:OnCreate) ->
//   OnCreate -> OnStopBeingBuilt(builder or nil, layer) when complete, else OnStartBeingBuilt.
// A blueprint's script class: bp.ScriptModule/ScriptClass when given, else
// "<blueprint dir>/<name>_script.lua" with class TypeClass, else the kind's default
// (/lua/sim/unit.lua Unit, /lua/sim/prop.lua Prop, /lua/sim/projectile.lua Projectile).
#include <cmath>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

const char* kValuesKey = "moho64.values";

Sim* S(lua_State* L) { return Sim::From(L); }

std::string IdString(uint32_t id) { return std::to_string(id); }

// ---- small Lua helpers ---------------------------------------------------------------------

float FieldNumber(lua_State* L, int t, const char* k, float def) {
  if (!lua_istable(L, t)) return def;
  lua_pushstring(L, k);
  lua_gettable(L, t < 0 && t > LUA_REGISTRYINDEX ? t - 1 : t);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}

// Push bp[a][b] (nil when missing).
void PushPath(lua_State* L, int t, const char* a, const char* b = nullptr) {
  lua_pushvalue(L, t);
  for (const char* k : {a, b}) {
    if (!k) break;
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      return;
    }
    lua_pushstring(L, k);
    lua_gettable(L, -2);
    lua_remove(L, -2);
  }
}

void PushVec(lua_State* L, const Vec3& v) {
  lua_getglobal(L, "Vector");
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  lua_call(L, 3, 1);
}

Vec3 CheckVec(lua_State* L, int idx) {
  luaL_checktype(L, idx, LUA_TTABLE);
  Vec3 v;
  lua_rawgeti(L, idx, 1);
  v.x = static_cast<float>(lua_tonumber(L, -1));
  lua_rawgeti(L, idx, 2);
  v.y = static_cast<float>(lua_tonumber(L, -1));
  lua_rawgeti(L, idx, 3);
  v.z = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 3);
  return v;
}

Quat FromEuler(float pitch, float yaw, float roll) {  // radians; yaw about Y
  float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
  float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
  float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
  Quat q;
  q.w = cy * cp * cr + sy * sp * sr;
  q.x = cy * sp * cr + sy * cp * sr;
  q.y = sy * cp * cr - cy * sp * sr;
  q.z = cy * cp * sr - sy * sp * cr;
  return q;
}

float Heading(const Quat& q) { return std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x)); }

const BlueprintInfo* CheckBlueprint(lua_State* L, int idx) {
  const char* id = luaL_checkstring(L, idx);
  const BlueprintInfo* bp = S(L)->blueprints().Find(id);
  if (!bp) luaL_error(L, "Invalid blueprint id '%s'", id);
  return bp;
}

Army* CheckArmy(lua_State* L, int idx) {
  Army* a = S(L)->GetArmy(L, idx);
  if (!a) luaL_error(L, "Invalid army %s", lua_tostring(L, idx) ? lua_tostring(L, idx) : "?");
  return a;
}

// ---- globals -------------------------------------------------------------------------------

int PushNew(lua_State* L, ScriptObject* o) {
  PushObject(L, o);
  return 1;
}

// CreateUnit(bp, army, x, y, z, qx, qy, qz, qw, [layer])
int l_CreateUnit(lua_State* L) {
  const BlueprintInfo* bp = CheckBlueprint(L, 1);
  Army* a = CheckArmy(L, 2);
  Vec3 p{static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
         static_cast<float>(luaL_checknumber(L, 5))};
  Quat q{static_cast<float>(luaL_optnumber(L, 6, 0)), static_cast<float>(luaL_optnumber(L, 7, 0)),
         static_cast<float>(luaL_optnumber(L, 8, 0)), static_cast<float>(luaL_optnumber(L, 9, 1))};
  return PushNew(L, S(L)->CreateUnit(L, *bp, a, p, q, true));
}

// CreateUnitHPR(bp, army, x, y, z, pitch, yaw, roll)
int l_CreateUnitHPR(lua_State* L) {
  const BlueprintInfo* bp = CheckBlueprint(L, 1);
  Army* a = CheckArmy(L, 2);
  Vec3 p{static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
         static_cast<float>(luaL_checknumber(L, 5))};
  Quat q = FromEuler(static_cast<float>(luaL_optnumber(L, 6, 0)), static_cast<float>(luaL_optnumber(L, 7, 0)),
                     static_cast<float>(luaL_optnumber(L, 8, 0)));
  return PushNew(L, S(L)->CreateUnit(L, *bp, a, p, q, true));
}

// CreateInitialArmyUnit(army, bp): at the army's start position, on the surface.
int l_CreateInitialArmyUnit(lua_State* L) {
  Army* a = CheckArmy(L, 1);
  const BlueprintInfo* bp = CheckBlueprint(L, 2);
  const TerrainMap* m = S(L)->map();
  Vec3 p{a->startX, m ? m->SurfaceHeight(a->startX, a->startZ) : 0, a->startZ};
  return PushNew(L, S(L)->CreateUnit(L, *bp, a, p, Quat{}, true));
}

// CreatePropHPR(bp, x, y, z, heading, pitch, roll)
int l_CreatePropHPR(lua_State* L) {
  const BlueprintInfo* bp = CheckBlueprint(L, 1);
  Vec3 p{static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
         static_cast<float>(luaL_checknumber(L, 4))};
  Quat q = FromEuler(static_cast<float>(luaL_optnumber(L, 6, 0)), static_cast<float>(luaL_optnumber(L, 5, 0)),
                     static_cast<float>(luaL_optnumber(L, 7, 0)));
  return PushNew(L, S(L)->CreateProp(L, *bp, p, q, Vec3{1, 1, 1}));
}

// CreateProp(location, bp)
int l_CreateProp(lua_State* L) {
  Vec3 p = CheckVec(L, 1);
  const BlueprintInfo* bp = CheckBlueprint(L, 2);
  return PushNew(L, S(L)->CreateProp(L, *bp, p, Quat{}, Vec3{1, 1, 1}));
}

int l_SetArmyStart(lua_State* L) {
  Army* a = CheckArmy(L, 1);
  a->startX = static_cast<float>(luaL_checknumber(L, 2));
  a->startZ = static_cast<float>(luaL_checknumber(L, 3));
  a->hasStart = true;
  return 0;
}

int l_GenerateArmyStart(lua_State* L) {
  Army* a = CheckArmy(L, 1);
  const TerrainMap* m = S(L)->map();
  if (m) {
    a->startX = m->width() * 0.5f;
    a->startZ = m->height() * 0.5f;
  }
  a->hasStart = true;
  return 0;
}

Entity* EntityFromId(lua_State* L, int idx) {
  uint32_t id = static_cast<uint32_t>(std::strtoul(luaL_checkstring(L, idx), nullptr, 10));
  return S(L)->FindEntity(id);
}

int l_GetEntityById(lua_State* L) { return PushNew(L, EntityFromId(L, 1)); }

int l_GetUnitById(lua_State* L) {
  Entity* e = EntityFromId(L, 1);
  return PushNew(L, dynamic_cast<Unit*>(e));
}

int l_GetUnitBlueprintByName(lua_State* L) {
  const BlueprintInfo* bp = S(L)->blueprints().Find(luaL_checkstring(L, 1));
  if (!bp || bp->kind != BpKind::Unit) {
    lua_pushnil(L);
    return 1;
  }
  S(L)->blueprints().PushTable(L, *bp);
  return 1;
}

template <class T>
int l_IsKind(lua_State* L) {
  lua_pushboolean(L, ToObject<T>(L, 1) != nullptr);
  return 1;
}

// ---- Entity --------------------------------------------------------------------------------

Entity* E(lua_State* L) { return CheckObject<Entity>(L, 1); }
Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

int l_GetEntityId(lua_State* L) {
  lua_pushstring(L, IdString(E(L)->id).c_str());
  return 1;
}
int l_GetArmy(lua_State* L) {
  Entity* e = E(L);
  lua_pushnumber(L, e->army ? e->army->index : -1);
  return 1;
}
int l_GetBlueprint(lua_State* L) {
  Entity* e = E(L);
  if (!e->blueprint) {
    lua_pushnil(L);
    return 1;
  }
  S(L)->blueprints().PushTable(L, *e->blueprint);
  return 1;
}
int l_GetAIBrain(lua_State* L) {
  Entity* e = E(L);
  return PushNew(L, e->army ? e->army->brain : nullptr);
}
int l_GetPosition(lua_State* L) {
  PushVec(L, E(L)->position);
  return 1;
}
int l_GetPositionXYZ(lua_State* L) {
  Entity* e = E(L);
  lua_pushnumber(L, e->position.x);
  lua_pushnumber(L, e->position.y);
  lua_pushnumber(L, e->position.z);
  return 3;
}
int l_GetOrientation(lua_State* L) {
  const Quat& q = E(L)->orientation;
  lua_newtable(L);
  float v[4] = {q.x, q.y, q.z, q.w};
  for (int i = 0; i < 4; ++i) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
int l_GetHeading(lua_State* L) {
  lua_pushnumber(L, Heading(E(L)->orientation));
  return 1;
}
int l_SetPosition(lua_State* L) {
  E(L)->position = CheckVec(L, 2);
  return 0;
}
int l_SetOrientation(lua_State* L) {
  Entity* e = E(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  float v[4];
  for (int i = 0; i < 4; ++i) {
    lua_rawgeti(L, 2, i + 1);
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  e->orientation = {v[0], v[1], v[2], v[3]};
  return 0;
}
int l_GetHealth(lua_State* L) {
  lua_pushnumber(L, E(L)->health);
  return 1;
}
int l_GetMaxHealth(lua_State* L) {
  lua_pushnumber(L, E(L)->maxHealth);
  return 1;
}
int l_SetHealth(lua_State* L) {  // SetHealth(instigator, health)
  Entity* e = E(L);
  e->health = std::min(e->maxHealth, static_cast<float>(luaL_checknumber(L, 3)));
  return 0;
}
int l_SetMaxHealth(lua_State* L) {
  Entity* e = E(L);
  e->maxHealth = static_cast<float>(luaL_checknumber(L, 2));
  if (e->health > e->maxHealth) e->health = e->maxHealth;
  return 0;
}
int l_AdjustHealth(lua_State* L) {  // AdjustHealth(instigator, delta)
  Entity* e = E(L);
  e->health = std::clamp(e->health + static_cast<float>(luaL_checknumber(L, 3)), 0.0f, e->maxHealth);
  return 0;
}
int l_GetFractionComplete(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  E(L);
  lua_pushnumber(L, u ? u->fractionComplete : 1);
  return 1;
}
int l_BeenDestroyed(lua_State* L) {
  lua_pushboolean(L, GetObject(L, 1) == nullptr);
  return 1;
}
int l_GetScale(lua_State* L) {
  Entity* e = E(L);
  for (float s : e->scale) lua_pushnumber(L, s);
  return 3;
}
int l_SetScale(lua_State* L) {
  Entity* e = E(L);
  float s = static_cast<float>(luaL_checknumber(L, 2));
  e->scale[0] = s;
  e->scale[1] = static_cast<float>(luaL_optnumber(L, 3, s));
  e->scale[2] = static_cast<float>(luaL_optnumber(L, 4, s));
  return 0;
}
int l_GetBoneCount(lua_State* L) {
  E(L);
  lua_pushnumber(L, 1);  // TODO(M3): skeleton from the mesh; bone 0 is the root
  return 1;
}
int l_IsValidBone(lua_State* L) {
  E(L);
  // TODO(M3): check against the mesh skeleton. Until meshes load, accept the root and names.
  lua_pushboolean(L, lua_isnumber(L, 2) ? lua_tonumber(L, 2) <= 0 : lua_isstring(L, 2));
  return 1;
}
int l_GetBoneName(lua_State* L) {
  E(L);
  lua_pushstring(L, "root");
  return 1;
}

// ---- Unit ----------------------------------------------------------------------------------

int l_GetUnitId(lua_State* L) {
  Unit* u = U(L);
  lua_pushstring(L, u->blueprint ? u->blueprint->id.c_str() : "");
  return 1;
}
int l_GetCurrentLayer(lua_State* L) {
  lua_pushstring(L, U(L)->layer.c_str());
  return 1;
}
int l_GetWeaponCount(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(U(L)->weapons.size()));
  return 1;
}
int l_GetWeapon(lua_State* L) {
  Unit* u = U(L);
  int i = static_cast<int>(luaL_checknumber(L, 2));
  if (i < 1 || i > static_cast<int>(u->weapons.size())) return luaL_error(L, "GetWeapon: index %d out of range", i);
  return PushNew(L, u->weapons[i - 1]);
}
int l_IsUnitState(lua_State* L) {
  lua_pushboolean(L, U(L)->unitStates.count(luaL_checkstring(L, 2)) > 0);
  return 1;
}
int l_SetUnitState(lua_State* L) {
  Unit* u = U(L);
  std::string s = luaL_checkstring(L, 2);
  if (lua_toboolean(L, 3)) u->unitStates.insert(s);
  else u->unitStates.erase(s);
  return 0;
}
int l_IsBeingBuilt(lua_State* L) {
  lua_pushboolean(L, U(L)->fractionComplete < 1);
  return 1;
}

// Engine values that scripts set and read back: Set<X>(v) / Get<X>() pairs, kept per object.
int l_AttrSet(lua_State* L) {
  ScriptObject* o = CheckAnyObject(L, 1);
  SetObjectValue(L, o, lua_tostring(L, lua_upvalueindex(1)), 2);
  return 0;
}
int l_AttrGet(lua_State* L) {
  ScriptObject* o = CheckAnyObject(L, 1);
  PushObjectValue(L, o, lua_tostring(L, lua_upvalueindex(1)));
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
  }
  return 1;
}

struct Attr {
  const char* cls;
  const char* setter;
  const char* getter;  // may be null
  int defType;         // 0 nil, 1 number, 2 boolean
  float def;
};
const Attr kAttrs[] = {
    {"Unit", "SetConsumptionPerSecondEnergy", "GetConsumptionPerSecondEnergy", 1, 0},
    {"Unit", "SetConsumptionPerSecondMass", "GetConsumptionPerSecondMass", 1, 0},
    {"Unit", "SetProductionPerSecondEnergy", "GetProductionPerSecondEnergy", 1, 0},
    {"Unit", "SetProductionPerSecondMass", "GetProductionPerSecondMass", 1, 0},
    {"Unit", "SetBuildRate", "GetBuildRate", 1, 0},
    {"Unit", "SetFireState", "GetFireState", 1, 0},
    {"Unit", "SetShieldRatio", "GetShieldRatio", 1, 0},
    {"Unit", "SetFuelRatio", "GetFuelRatio", 1, -1},
    {"Unit", "SetFuelUseTime", "GetFuelUseTime", 1, 0},
    {"Unit", "SetWorkProgress", "GetWorkProgress", 1, 0},
    {"Unit", "SetPaused", "IsPaused", 2, 0},
    {"Unit", "SetOverchargePaused", "IsOverchargePaused", 2, 0},
    {"Unit", "SetStunned", "IsStunned", 2, 0},
    {"Unit", "SetIsValidTarget", "IsValidTarget", 2, 1},
    {"Unit", "SetCapturable", "IsCapturable", 2, 1},
    {"Unit", "SetProductionActive", nullptr, 0, 0},
    {"Unit", "SetConsumptionActive", nullptr, 0, 0},
    {"Unit", "SetAutoMode", nullptr, 0, 0},
    {"Unit", "SetCreator", nullptr, 0, 0},
    {"Unit", "SetCustomName", nullptr, 0, 0},
    {"Unit", "SetRegenRate", nullptr, 0, 0},
    {"Unit", "SetElevation", nullptr, 0, 0},
    {"Unit", "SetSpeedMult", nullptr, 0, 0},
    {"Unit", "SetAccMult", nullptr, 0, 0},
    {"Unit", "SetTurnMult", nullptr, 0, 0},
    {"Unit", "SetBreakOffTriggerMult", nullptr, 0, 0},
    {"Unit", "SetBreakOffDistanceMult", nullptr, 0, 0},
    {"Unit", "SetDoNotTarget", nullptr, 0, 0},
    {"Unit", "SetReclaimable", nullptr, 0, 0},
    {"Unit", "SetUnSelectable", nullptr, 0, 0},
    {"Unit", "SetBusy", nullptr, 0, 0},
    {"Unit", "SetBlockCommandQueue", nullptr, 0, 0},
    {"Unit", "SetImmobile", nullptr, 0, 0},
    {"Unit", "SetStrategicUnderlay", nullptr, 0, 0},
    {"UnitWeapon", "SetEnabled", nullptr, 0, 0},
    {"UnitWeapon", "SetFireControl", "IsFireControl", 2, 0},
    {"UnitWeapon", "SetFiringRandomness", "GetFiringRandomness", 1, 0},
};

// Scripts' per-unit statistics: GetStat(name, default) -> { Value = ... }; SetStat(name, value)
int l_GetStat(lua_State* L) {
  ScriptObject* o = CheckAnyObject(L, 1);
  std::string key = std::string("stat:") + luaL_checkstring(L, 2);
  PushObjectValue(L, o, key.c_str());
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushvalue(L, 3);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_pushnumber(L, 0);
    }
  }
  lua_newtable(L);
  lua_pushstring(L, "Value");
  lua_pushvalue(L, -3);
  lua_rawset(L, -3);
  return 1;
}
int l_SetStat(lua_State* L) {
  ScriptObject* o = CheckAnyObject(L, 1);
  std::string key = std::string("stat:") + luaL_checkstring(L, 2);
  SetObjectValue(L, o, key.c_str(), 3);
  lua_pushboolean(L, 1);
  return 1;
}

// ---- UnitWeapon -----------------------------------------------------------------------------

int l_weapon_GetBlueprint(lua_State* L) {
  UnitWeapon* w = CheckObject<UnitWeapon>(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, w->bpRef);
  return 1;
}

// ---- Platoon -------------------------------------------------------------------------------

Platoon* P(lua_State* L) { return CheckObject<Platoon>(L, 1); }

int l_platoon_GetBrain(lua_State* L) { return PushNew(L, P(L)->army->brain); }
int l_platoon_GetPlatoonUnits(lua_State* L) {
  Platoon* p = P(L);
  lua_newtable(L);
  int i = 0;
  for (Unit* u : p->units) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}
int l_platoon_UniquelyNamePlatoon(lua_State* L) {
  P(L)->name = luaL_checkstring(L, 2);
  return 0;
}
int l_platoon_GetPlatoonUniqueName(lua_State* L) {
  lua_pushstring(L, P(L)->name.c_str());
  return 1;
}
int l_platoon_GetAIPlan(lua_State* L) {
  lua_pushstring(L, P(L)->plan.c_str());
  return 1;
}

// ---- Brain (unit and platoon queries) -------------------------------------------------------

AiBrain* B(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

int l_brain_GetArmyStartPos(lua_State* L) {
  Army* a = B(L)->army;
  lua_pushnumber(L, a->startX);
  lua_pushnumber(L, a->startZ);
  return 2;
}
int l_brain_MakePlatoon(lua_State* L) {  // MakePlatoon(name, plan)
  AiBrain* b = B(L);
  std::string name = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  std::string plan = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
  return PushNew(L, S(L)->CreatePlatoon(L, b->army, name, plan));
}
int l_brain_GetPlatoonUniquelyNamed(lua_State* L) {
  AiBrain* b = B(L);
  std::string name = luaL_checkstring(L, 2);
  if (name == "ArmyPool") return PushNew(L, b->army->pool);
  lua_pushnil(L);
  return 1;
}
// GetListOfUnits(category, needToBeIdle, requireBuilt)
int l_brain_GetListOfUnits(lua_State* L) {
  AiBrain* b = B(L);
  const uint64_t* cat = ToCategory(L, 2);
  lua_newtable(L);
  int n = 0;
  for (auto& [id, e] : S(L)->entities()) {
    Unit* u = dynamic_cast<Unit*>(e);
    if (!u || u->army != b->army || u->dead) continue;
    if (cat && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(cat, u->blueprint->entityIndex)))
      continue;
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
int l_brain_GetCurrentUnits(lua_State* L) {
  l_brain_GetListOfUnits(L);
  lua_pushnumber(L, luaL_getn(L, -1));
  return 1;
}

}  // namespace

// ---- per-object script values ---------------------------------------------------------------

void SetObjectValue(lua_State* L, ScriptObject* obj, const char* key, int valueIdx) {
  if (valueIdx < 0) valueIdx = lua_gettop(L) + valueIdx + 1;
  lua_pushstring(L, kValuesKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kValuesKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
  }
  lua_pushlightuserdata(L, obj);
  lua_rawget(L, -2);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushlightuserdata(L, obj);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
  }
  lua_pushstring(L, key);
  lua_pushvalue(L, valueIdx);
  lua_rawset(L, -3);
  lua_pop(L, 2);
}

void PushObjectValue(lua_State* L, ScriptObject* obj, const char* key) {
  lua_pushstring(L, kValuesKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_isnil(L, -1)) return;
  lua_pushlightuserdata(L, obj);
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (lua_isnil(L, -1)) return;
  lua_pushstring(L, key);
  lua_rawget(L, -2);
  lua_remove(L, -2);
}

void RegisterEntityBindings(lua_State* L) {
  SetGlobal(L, "CreateUnit", l_CreateUnit);
  SetGlobal(L, "CreateUnitHPR", l_CreateUnitHPR);
  SetGlobal(L, "CreateInitialArmyUnit", l_CreateInitialArmyUnit);
  SetGlobal(L, "CreatePropHPR", l_CreatePropHPR);
  SetGlobal(L, "CreateProp", l_CreateProp);
  SetGlobal(L, "SetArmyStart", l_SetArmyStart);
  SetGlobal(L, "GenerateArmyStart", l_GenerateArmyStart);
  SetGlobal(L, "GetEntityById", l_GetEntityById);
  SetGlobal(L, "GetUnitById", l_GetUnitById);
  SetGlobal(L, "GetUnitBlueprintByName", l_GetUnitBlueprintByName);
  SetGlobal(L, "IsEntity", l_IsKind<Entity>);
  SetGlobal(L, "IsUnit", l_IsKind<Unit>);
  SetGlobal(L, "IsProp", l_IsKind<Prop>);

  SetMethod(L, "Entity", "GetEntityId", l_GetEntityId);
  SetMethod(L, "Entity", "GetArmy", l_GetArmy);
  SetMethod(L, "Entity", "GetBlueprint", l_GetBlueprint);
  SetMethod(L, "Entity", "GetAIBrain", l_GetAIBrain);
  SetMethod(L, "Entity", "GetPosition", l_GetPosition);
  SetMethod(L, "Entity", "GetPositionXYZ", l_GetPositionXYZ);
  SetMethod(L, "Entity", "GetOrientation", l_GetOrientation);
  SetMethod(L, "Entity", "GetHeading", l_GetHeading);
  SetMethod(L, "Entity", "SetPosition", l_SetPosition);
  SetMethod(L, "Entity", "SetOrientation", l_SetOrientation);
  SetMethod(L, "Entity", "GetHealth", l_GetHealth);
  SetMethod(L, "Unit", "GetHealth", l_GetHealth);
  SetMethod(L, "Entity", "GetMaxHealth", l_GetMaxHealth);
  SetMethod(L, "Entity", "SetHealth", l_SetHealth);
  SetMethod(L, "Entity", "SetMaxHealth", l_SetMaxHealth);
  SetMethod(L, "Entity", "AdjustHealth", l_AdjustHealth);
  SetMethod(L, "Entity", "GetFractionComplete", l_GetFractionComplete);
  SetMethod(L, "Entity", "BeenDestroyed", l_BeenDestroyed);
  SetMethod(L, "Entity", "GetScale", l_GetScale);
  SetMethod(L, "Entity", "SetScale", l_SetScale);
  SetMethod(L, "Entity", "GetBoneCount", l_GetBoneCount);
  SetMethod(L, "Entity", "IsValidBone", l_IsValidBone);
  SetMethod(L, "Entity", "GetBoneName", l_GetBoneName);

  SetMethod(L, "Unit", "GetUnitId", l_GetUnitId);
  SetMethod(L, "Unit", "GetCurrentLayer", l_GetCurrentLayer);
  SetMethod(L, "Unit", "GetWeaponCount", l_GetWeaponCount);
  SetMethod(L, "Unit", "GetWeapon", l_GetWeapon);
  SetMethod(L, "Unit", "IsUnitState", l_IsUnitState);
  SetMethod(L, "Unit", "SetUnitState", l_SetUnitState);
  SetMethod(L, "Unit", "IsBeingBuilt", l_IsBeingBuilt);
  SetMethod(L, "Unit", "GetStat", l_GetStat);
  SetMethod(L, "Unit", "SetStat", l_SetStat);
  for (const auto& a : kAttrs) {
    PushClassTable(L, a.cls);
    lua_pushstring(L, a.setter);
    lua_pushstring(L, a.setter + 3);
    lua_pushcclosure(L, l_AttrSet, 1);
    lua_rawset(L, -3);
    if (a.getter) {
      lua_pushstring(L, a.getter);
      lua_pushstring(L, a.setter + 3);
      if (a.defType == 1) lua_pushnumber(L, a.def);
      else if (a.defType == 2) lua_pushboolean(L, a.def != 0);
      else lua_pushnil(L);
      lua_pushcclosure(L, l_AttrGet, 2);
      lua_rawset(L, -3);
    }
    lua_pop(L, 1);
  }
  SetMethod(L, "UnitWeapon", "GetBlueprint", l_weapon_GetBlueprint);
  SetMethod(L, "UnitWeapon", "BeenDestroyed", l_BeenDestroyed);

  SetMethod(L, "CPlatoon", "GetBrain", l_platoon_GetBrain);
  SetMethod(L, "CPlatoon", "GetPlatoonUnits", l_platoon_GetPlatoonUnits);
  SetMethod(L, "CPlatoon", "UniquelyNamePlatoon", l_platoon_UniquelyNamePlatoon);
  SetMethod(L, "CPlatoon", "GetPlatoonUniqueName", l_platoon_GetPlatoonUniqueName);
  SetMethod(L, "CPlatoon", "GetAIPlan", l_platoon_GetAIPlan);

  SetMethod(L, "CAiBrain", "GetArmyStartPos", l_brain_GetArmyStartPos);
  SetMethod(L, "CAiBrain", "MakePlatoon", l_brain_MakePlatoon);
  SetMethod(L, "CAiBrain", "GetPlatoonUniquelyNamed", l_brain_GetPlatoonUniquelyNamed);
  SetMethod(L, "CAiBrain", "GetListOfUnits", l_brain_GetListOfUnits);
  SetMethod(L, "CAiBrain", "GetCurrentUnits", l_brain_GetCurrentUnits);
}

// ---- Sim: creation ---------------------------------------------------------------------------

bool Sim::PushImport(lua_State* L, const std::string& module) {
  lua_checkstack(L, 10);
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, module.c_str());
  if (lua_pcall(L, 1, 1, top + 1) != 0) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "import failed");
    lua_settop(L, top);
    return false;
  }
  lua_remove(L, top + 1);
  return true;
}

void Sim::PushScriptClass(lua_State* L, const BlueprintInfo& bp, const char* defModule, const char* defClass) {
  int top = lua_gettop(L);
  bps_.PushTable(L, bp);
  int t = lua_gettop(L);
  auto field = [&](const char* k) -> std::string {
    lua_pushstring(L, k);
    lua_gettable(L, t);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
  };
  std::string module = field("ScriptModule"), cls = field("ScriptClass");
  if (module.empty()) {
    std::string src = field("Source");  // "/units/uel0001/uel0001_unit.bp"
    size_t slash = src.rfind('/');
    std::string dir = slash == std::string::npos ? "" : src.substr(0, slash + 1);
    std::string name = src.substr(slash + 1);
    size_t us = name.rfind('_');
    if (us != std::string::npos) name = name.substr(0, us);
    module = dir + name + "_script.lua";
    if (!vfs_->Exists(module)) module.clear();
  }
  if (cls.empty()) cls = "TypeClass";
  lua_settop(L, top);
  if (!module.empty() && PushImport(L, module)) {
    lua_pushstring(L, cls.c_str());
    lua_gettable(L, -2);
    lua_remove(L, -2);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    Logf(LogLevel::Warning, "Problems loading module '%s'.  Falling back to '%s' in '%s'.", module.c_str(), defClass,
         defModule);
  }
  if (PushImport(L, defModule)) {
    lua_pushstring(L, defClass);
    lua_gettable(L, -2);
    lua_remove(L, -2);
    return;
  }
  lua_pushnil(L);
}

bool Sim::CallMethod(lua_State* L, ScriptObject* obj, const char* method, int nargs) {
  lua_checkstack(L, 10);
  int base = lua_gettop(L) - nargs;
  PushObject(L, obj);
  int self = lua_gettop(L);
  lua_pushstring(L, method);
  lua_gettable(L, self);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, base);
    return true;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);   // handler below the args
  // stack: base+1 handler, args..., self, fn  ->  fn, self, args...
  lua_insert(L, base + 2);   // fn
  lua_insert(L, base + 3);   // self
  bool ok = lua_pcall(L, nargs + 1, 0, base + 1) == 0;
  if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, base);
  return ok;
}

Entity* Sim::FindEntity(uint32_t id) const {
  auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : it->second;
}

namespace {
// The layer a unit starts on (Entity::GetStartingLayer): from its motion type and the water.
std::string StartingLayer(lua_State* L, int bpIdx, const TerrainMap* map, const Vec3& p) {
  PushPath(L, bpIdx, "Physics", "MotionType");
  std::string mt = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  bool underWater = map && map->hasWater && map->TerrainHeight(p.x, p.z) < map->waterElevation;
  if (mt == "RULEUMT_Air") return "Air";
  if (mt == "RULEUMT_Water" || mt == "RULEUMT_SurfacingSub") return mt == "RULEUMT_SurfacingSub" ? "Sub" : "Water";
  if (!underWater) return "Land";
  if (mt == "RULEUMT_Amphibious") return "Seabed";
  return "Water";
}
}  // namespace

Unit* Sim::CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete) {
  lua_checkstack(L, 40);
  int top = lua_gettop(L);
  auto owned = std::make_unique<Unit>();
  Unit* u = owned.get();
  u->kind = Entity::Kind::Unit;
  u->blueprint = &bp;
  u->army = army;
  u->id = (static_cast<uint32_t>(army ? army->index - 1 : 0xff) << 20) | ++(army ? army->serial : propSerial_);
  u->position = pos;
  u->orientation = q;
  u->fractionComplete = complete ? 1.0f : 0.0f;
  bps_.PushTable(L, bp);
  int bpIdx = lua_gettop(L);
  u->maxHealth = 1;
  PushPath(L, bpIdx, "Defense", "MaxHealth");
  if (lua_isnumber(L, -1)) u->maxHealth = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  u->health = complete ? u->maxHealth : 0;

  // Script object: an instance of the blueprint's class (UnitFactory __call).
  PushScriptClass(L, bp, "/lua/sim/unit.lua", "Unit");
  int cls = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushvalue(L, cls);
  if (lua_pcall(L, 0, 1, cls + 1) != 0 || !lua_istable(L, -1)) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "unit script class did not create an object");
    lua_settop(L, top);
    return nullptr;
  }
  BindObject(L, -1, u);
  int obj = lua_gettop(L);
  owned_.push_back(std::move(owned));
  entities_[u->id] = u;
  if (army && army->pool) army->pool->units.push_back(u);

  CallMethod(L, u, "OnPreCreate", 0);
  u->layer = StartingLayer(L, bpIdx, map_.get(), pos);
  lua_pushstring(L, u->layer.c_str());
  lua_pushstring(L, "None");
  CallMethod(L, u, "OnLayerChange", 2);

  // Weapons: one per bp.Weapon entry; class from the unit class's Weapons[label].
  PushPath(L, bpIdx, "Weapon");
  int wlist = lua_gettop(L);
  if (lua_istable(L, wlist)) {
    lua_pushstring(L, "Weapons");
    lua_gettable(L, obj);
    int wclasses = lua_gettop(L);
    for (int i = 1;; ++i) {
      lua_rawgeti(L, wlist, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      int wbp = lua_gettop(L);
      std::string label;
      lua_pushstring(L, "Label");
      lua_gettable(L, wbp);
      if (lua_isstring(L, -1)) label = lua_tostring(L, -1);
      lua_pop(L, 1);
      auto wowned = std::make_unique<UnitWeapon>();
      UnitWeapon* w = wowned.get();
      w->unit = u;
      w->index = i;
      w->label = label;
      lua_pushvalue(L, wbp);
      w->bpRef = luaL_ref(L, LUA_REGISTRYINDEX);
      lua_pushnil(L);
      if (lua_istable(L, wclasses)) {
        lua_pop(L, 1);
        lua_pushstring(L, label.c_str());
        lua_gettable(L, wclasses);
      }
      int wcls = lua_gettop(L);
      if (!lua_istable(L, wcls)) {
        Logf(LogLevel::Warning, "Weapon '%s' of unit %s has no script class.", label.c_str(), bp.id.c_str());
        lua_pop(L, 1);
        PushClassTable(L, "UnitWeapon");
        lua_newtable(L);
        lua_insert(L, -2);
        lua_setmetatable(L, -2);
      } else {
        lua_pushcfunction(L, ScriptTraceback);
        lua_pushvalue(L, wcls);
        lua_pushvalue(L, obj);
        if (lua_pcall(L, 1, 1, wcls + 1) != 0 || !lua_istable(L, -1)) {
          LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "weapon class did not create an object");
          lua_settop(L, wbp - 1);
          continue;
        }
      }
      BindObject(L, -1, w);
      u->weapons.push_back(w);
      owned_.push_back(std::move(wowned));
      lua_settop(L, wbp - 1);
      CallMethod(L, w, "OnCreate", 0);
    }
  }
  lua_settop(L, obj);
  CallMethod(L, u, "OnCreate", 0);
  lua_pushnil(L);  // builder
  lua_pushstring(L, u->layer.c_str());
  CallMethod(L, u, complete ? "OnStopBeingBuilt" : "OnStartBeingBuilt", 2);
  lua_settop(L, top);
  return u;
}

Prop* Sim::CreateProp(lua_State* L, const BlueprintInfo& bp, Vec3 pos, Quat q, Vec3 scale) {
  lua_checkstack(L, 20);
  int top = lua_gettop(L);
  auto owned = std::make_unique<Prop>();
  Prop* p = owned.get();
  p->kind = Entity::Kind::Prop;
  p->blueprint = &bp;
  p->id = (0xffu << 20) + ++propSerial_;
  p->position = pos;
  p->orientation = q;
  p->scale[0] = scale.x;
  p->scale[1] = scale.y;
  p->scale[2] = scale.z;
  bps_.PushTable(L, bp);
  int bpIdx = lua_gettop(L);
  PushPath(L, bpIdx, "Defense", "MaxHealth");
  p->maxHealth = p->health = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 1;
  lua_settop(L, top);
  PushScriptClass(L, bp, "/lua/sim/prop.lua", "Prop");
  int cls = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushvalue(L, cls);
  if (lua_pcall(L, 0, 1, cls + 1) != 0 || !lua_istable(L, -1)) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "prop script class did not create an object");
    lua_settop(L, top);
    return nullptr;
  }
  BindObject(L, -1, p);
  owned_.push_back(std::move(owned));
  entities_[p->id] = p;
  lua_settop(L, top);
  CallMethod(L, p, "OnCreate", 0);
  return p;
}

Platoon* Sim::CreatePlatoon(lua_State* L, Army* army, const std::string& name, const std::string& plan) {
  int top = lua_gettop(L);
  if (!PushImport(L, "/lua/platoon.lua")) return nullptr;
  lua_pushstring(L, "Platoon");
  lua_gettable(L, -2);
  auto owned = std::make_unique<Platoon>();
  Platoon* p = owned.get();
  p->army = army;
  p->name = name;
  p->plan = plan;
  lua_newtable(L);
  lua_insert(L, -2);
  lua_setmetatable(L, -2);
  BindObject(L, -1, p);
  owned_.push_back(std::move(owned));
  lua_settop(L, top);
  return p;
}

}  // namespace moho
