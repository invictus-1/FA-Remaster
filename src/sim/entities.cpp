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

#include "core/dmath.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "sim/transport.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/intel.h"
#include "sim/sim.h"
#include "sim/air.h"
#include "sim/skeleton.h"
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

void PushVec(lua_State* L, const Vec3& v) { PushVector(L, v.x, v.y, v.z); }

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
  float cy = dmath::Cos(yaw * 0.5f), sy = dmath::Sin(yaw * 0.5f);
  float cp = dmath::Cos(pitch * 0.5f), sp = dmath::Sin(pitch * 0.5f);
  float cr = dmath::Cos(roll * 0.5f), sr = dmath::Sin(roll * 0.5f);
  Quat q;
  q.w = cy * cp * cr + sy * sp * sr;
  q.x = cy * sp * cr + sy * cp * sr;
  q.y = sy * cp * cr - cy * sp * sr;
  q.z = cy * cp * sr - sy * sp * cr;
  return q;
}

float Heading(const Quat& q) { return dmath::Atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x)); }

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

// FlattenMapRect(x, z, sizeX, sizeZ, elevation) 0x74b120: the grid vertices [x, x+sx] x [z, z+sz] take
// the elevation (truncated to the height scale); structures standing there (Land / Seabed, no motion)
// are warped to the new ground, moving units re-snap.
int l_FlattenMapRect(lua_State* L) {
  Sim& sim = *S(L);
  TerrainMap* m = sim.mutableMap();
  if (!m) return 0;
  int x = static_cast<int>(luaL_checknumber(L, 1)), z = static_cast<int>(luaL_checknumber(L, 2));
  int sx = static_cast<int>(luaL_checknumber(L, 3)), sz = static_cast<int>(luaL_checknumber(L, 4));
  float h = static_cast<float>(luaL_checknumber(L, 5));
  for (int j = z; j <= std::min(z + sz, m->height()); ++j)
    for (int i = x; i <= std::min(x + sx, m->width()); ++i) m->SetHeightAt(i, j, h);
  for (Unit* u : sim.units()) {
    if (u->dead || u->destroyQueued || (u->layer != "Land" && u->layer != "Seabed")) continue;
    if (u->position.x < x - 1 || u->position.x > x + sx + 1 || u->position.z < z - 1 || u->position.z > z + sz + 1) continue;
    if (!u->motion.bp || u->motion.bp->motionType == kMotionNone) {
      u->position.y = m->TerrainHeight(u->position.x, u->position.z);
      u->lastPosition = u->position;
    } else {
      u->motion.needSnap = true;
    }
  }
  sim.MarkUnitsMoved();
  return 0;
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
  return S(L)->FindEntityById(id);
}

int l_GetEntityById(lua_State* L) { return PushNew(L, EntityFromId(L, 1)); }

int l_GetUnitById(lua_State* L) {
  Entity* e = EntityFromId(L, 1);
  return PushNew(L, e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr);
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

// IsEntity (exe 0x75e560): a live entity -> true, anything else false.
int l_IsEntity(lua_State* L) {
  lua_pushboolean(L, ToObject<Entity>(L, 1) != nullptr);
  return 1;
}
// IsUnit / IsProp / IsProjectile (exe 0x75e6a0, 0x75e800, 0x75eac0): the argument must be a live
// game object (else "Game object has been destroyed" / "Expected a game object"); returns the
// object itself when it is of that kind, nil otherwise.
template <class T>
int l_IsKind(lua_State* L) {
  CheckAnyObject(L, 1);
  if (ToObject<T>(L, 1)) lua_pushvalue(L, 1);
  else lua_pushnil(L);
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
int l_GetBlueprint(lua_State* L) {  // exe 0x68afb0: nil for a destroyed entity or a non-entity
  Entity* e = ToObject<Entity>(L, 1);
  if (!e || !e->blueprint) {
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
Vec3 BonePosition(const Entity* e, int bone);
int ResolveBone(lua_State* L, Entity* e, int arg);
int l_GetPosition(lua_State* L) {  // (bone?) the entity's or a bone's world position
  // exe 0x68fc90: a destroyed entity (or a non-entity) gives (0, 0, 0), no error (probe v9).
  Entity* e = ToObject<Entity>(L, 1);
  if (!e) {
    PushVec(L, Vec3{0, 0, 0});
    return 1;
  }
  PushVec(L, lua_isnoneornil(L, 2) ? e->position : BonePosition(e, ResolveBone(L, e, 2)));
  return 1;
}
int l_GetPositionXYZ(lua_State* L) {
  Entity* e = E(L);
  Vec3 p = lua_isnoneornil(L, 2) ? e->position : BonePosition(e, ResolveBone(L, e, 2));
  lua_pushnumber(L, p.x);
  lua_pushnumber(L, p.y);
  lua_pushnumber(L, p.z);
  return 3;
}
int l_GetOrientation(lua_State* L) {
  const Quat& q = E(L)->orientation;
  PushVector(L, 0, 0, 0);  // quaternion = {x,y,z,w} with the vector metatable
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
int l_SetPosition(lua_State* L) {  // SetPosition(vector, immediate): a warp
  Entity* e = E(L);
  e->position = CheckVec(L, 2);
  if (e->kind == Entity::Kind::Unit) {
    static_cast<Unit*>(e)->motion.needSnap = true;
    S(L)->MarkUnitsMoved();
  }
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
int l_SetHealth(lua_State* L) {  // SetHealth(instigator, health) = AdjustHealth by the difference
  Entity* e = E(L);
  EntityAdjustHealth(L, e, ToObject<Entity>(L, 2), static_cast<float>(luaL_checknumber(L, 3)) - e->health);
  return 0;
}
int l_SetMaxHealth(lua_State* L) {  // only the maximum (the original does not clamp the health)
  E(L)->maxHealth = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_AdjustHealth(lua_State* L) {  // AdjustHealth(instigator, delta)
  Entity* e = E(L);
  EntityAdjustHealth(L, e, ToObject<Entity>(L, 2), static_cast<float>(luaL_checknumber(L, 3)));
  return 0;
}
int l_GetFractionComplete(lua_State* L) {
  lua_pushnumber(L, E(L)->fractionComplete);
  return 1;
}
int l_BeenDestroyed(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  Entity* e = dynamic_cast<Entity*>(o);
  lua_pushboolean(L, o == nullptr || (e && e->destroyQueued));
  return 1;
}
int l_Destroy(lua_State* L) {
  if (Entity* e = ToObject<Entity>(L, 1)) S(L)->QueueDestroy(e);
  static const bool dbg = getenv("MOHO64_DEBUG_DESTROY") != nullptr;
  if (dbg) {
    Entity* e = ToObject<Entity>(L, 1);
    if (e && e->kind == Entity::Kind::Unit && !e->dead) {  // who destroys a living unit
      std::string tb;
      lua_pushstring(L, "debug");
      lua_gettable(L, LUA_GLOBALSINDEX);
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "traceback");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_isstring(L, -1)) tb = lua_tostring(L, -1);
        lua_settop(L, lua_gettop(L) - 1);
      }
      lua_pop(L, 1);
      Logf(LogLevel::Debug, "moho64: Destroy living %s (army %d, tick %u, complete %.2f)\n%s",
           e->blueprint ? e->blueprint->id.c_str() : "?", e->army ? e->army->index : 0, S(L)->tick(),
           e->fractionComplete, tb.c_str());
    }
  }
  return 0;
}
// GetReclaimablesInRect(rect) -> props (and wrecks) inside; TODO(M3): spatial index
int l_GetReclaimablesInRect(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  float r[4];
  const char* k[4] = {"x0", "y0", "x1", "y1"};
  for (int i = 0; i < 4; ++i) {
    lua_pushstring(L, k[i]);
    lua_gettable(L, 1);
    r[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  lua_newtable(L);
  int n = 0;
  for (auto& [id, e] : S(L)->entities()) {
    if (e->kind != Entity::Kind::Prop || e->destroyQueued) continue;
    if (e->position.x < r[0] || e->position.x > r[2] || e->position.z < r[1] || e->position.z > r[3]) continue;
    PushObject(L, e);
    lua_rawseti(L, -2, ++n);
  }
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
// ---- bones ------------------------------------------------------------------------------------
// Bone 0 is the root; an entity without a mesh has just that one. -1 means the entity itself.

int BoneCount(const Entity* e) {
  if (e->kind == Entity::Kind::Beam) return 3;  // collision beams: origin, end, end
  return e->skeleton ? e->skeleton->Count() : 1;
}

// CAniActor::ResolveBoneIndex: a bone argument is an index (-2 .. count-1), a name, or nil (-1);
// anything else is a script error.
int ResolveBone(lua_State* L, Entity* e, int arg) {
  int ty = lua_type(L, arg);
  if (ty == LUA_TNUMBER) {
    int i = static_cast<int>(lua_tonumber(L, arg));
    if (i < -2 || i >= BoneCount(e)) luaL_error(L, "Arg %d: invalid bone index (%d)", arg, i);
    return i;
  }
  if (ty == LUA_TSTRING) {
    const char* name = lua_tostring(L, arg);
    int i = e->skeleton ? e->skeleton->Find(name) : -1;
    if (i < 0) luaL_error(L, "Arg %d: unit has no bone \"%s\".", arg, name);
    return i;
  }
  if (ty == LUA_TNIL || ty == LUA_TNONE) return -1;
  luaL_error(L, "Arg %d: invalid bone identifier; must be string, integer, or nil", arg);
  return -1;
}

// World transform of a bone at rest (TODO(M3): animation poses).
Vec3 BonePosition(const Entity* e, int bone) {
  Vec3 p;
  Quat q;
  BoneWorld(e, bone, &p, &q);
  return p;
}
Quat BoneOrientation(const Entity* e, int bone) {
  Vec3 p;
  Quat q;
  BoneWorld(e, bone, &p, &q);
  return q;
}

int l_GetBoneCount(lua_State* L) {
  lua_pushnumber(L, BoneCount(E(L)));
  return 1;
}
int l_IsValidBone(lua_State* L) {  // index 0 .. count-1 or a bone name (probe: -1 and unknown names false)
  Entity* e = E(L);
  bool ok = false;
  if (lua_type(L, 2) == LUA_TNUMBER) {
    double v = lua_tonumber(L, 2);
    ok = v >= 0 && v < BoneCount(e);
  } else if (lua_type(L, 2) == LUA_TSTRING) {
    ok = e->skeleton && e->skeleton->Find(lua_tostring(L, 2)) >= 0;
  }
  lua_pushboolean(L, ok);
  return 1;
}
int l_GetBoneName(lua_State* L) {
  Entity* e = E(L);
  int i = static_cast<int>(luaL_checknumber(L, 2));
  if (i < 0 || i >= BoneCount(e)) {
    lua_pushnil(L);
  } else if (e->skeleton) {
    lua_pushstring(L, e->skeleton->bones()[i].name.c_str());
  } else {
    lua_pushstring(L, "root");
  }
  return 1;
}
int l_GetBoneDirection(lua_State* L) {  // the bone's forward (+z) axis in the world
  Entity* e = E(L);
  Vec3 d = QuatRotate(BoneOrientation(e, ResolveBone(L, e, 2)), Vec3{0, 0, 1});
  lua_pushnumber(L, d.x);
  lua_pushnumber(L, d.y);
  lua_pushnumber(L, d.z);
  return 3;
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
  lua_pushboolean(L, U(L)->beingBuilt);
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

int l_c_CreateEntity(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  S(L)->AdoptScriptEntity(L, 1, 2, false);
  return 0;
}
int l_c_CreateShield(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  S(L)->AdoptScriptEntity(L, 1, 2, true);
  return 0;
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
  if (!b->army) return 1;
  for (Unit* u : b->army->units) {
    if (u->dead || u->destroyQueued) continue;
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

Vec3 EntityBonePosition(const Entity* e, int bone) { return BonePosition(e, bone); }

// ---- health ----------------------------------------------------------------------------------

void EntityAdjustHealth(lua_State* L, Entity* e, Entity* /*instigator*/, float amount) {
  if (amount == 0.0f) return;
  float h = e->health + amount;
  if (e->maxHealth <= h) h = e->maxHealth;
  if (h < 0.0f) h = 0.0f;
  if (h == e->health) return;
  // Entity::SetHealth: OnHealthChanged(new, old) with both rounded down to quarters of the maximum
  float inv = 1.0f / e->maxHealth;
  float qNew = std::floor(inv * h * 4.0f) * 0.25f;
  float qOld = std::floor(inv * e->health * 4.0f) * 0.25f;
  e->health = h;
  if (qNew != qOld && e->HasLuaObject()) {
    lua_pushnumber(L, qNew);
    lua_pushnumber(L, qOld);
    S(L)->CallMethod(L, e, "OnHealthChanged", 2);
  }
}

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
  SetGlobal(L, "FlattenMapRect", l_FlattenMapRect);
  SetGlobal(L, "GetUnitById", l_GetUnitById);
  SetGlobal(L, "GetUnitBlueprintByName", l_GetUnitBlueprintByName);
  SetGlobal(L, "IsEntity", l_IsEntity);
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
  SetMethod(L, "Entity", "Destroy", l_Destroy);
  SetGlobal(L, "GetReclaimablesInRect", l_GetReclaimablesInRect);
  SetMethod(L, "Entity", "GetScale", l_GetScale);
  SetMethod(L, "Entity", "SetScale", l_SetScale);
  SetMethod(L, "Entity", "GetBoneCount", l_GetBoneCount);
  SetMethod(L, "Entity", "IsValidBone", l_IsValidBone);
  SetMethod(L, "Entity", "GetBoneName", l_GetBoneName);
  SetMethod(L, "Entity", "GetBoneDirection", l_GetBoneDirection);

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
  SetGlobal(L, "IsProjectile", l_IsKind<Projectile>);
  SetGlobal(L, "_c_CreateEntity", l_c_CreateEntity);
  SetGlobal(L, "_c_CreateShield", l_c_CreateShield);
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

void Sim::AttachSkeleton(lua_State* L, Entity* e) {
  int top = lua_gettop(L);
  bps_.PushTable(L, *e->blueprint);
  lua_pushstring(L, "Display");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    int display = lua_gettop(L);
    lua_pushstring(L, "UniformScale");
    lua_gettable(L, display);
    if (lua_isnumber(L, -1)) e->meshScale = static_cast<float>(lua_tonumber(L, -1));
    lua_pushstring(L, "MeshBlueprint");
    lua_gettable(L, display);
    const BlueprintInfo* mesh = lua_isstring(L, -1) ? bps_.Find(lua_tostring(L, -1)) : nullptr;
    if (mesh) {
      bps_.PushTable(L, *mesh);
      lua_pushstring(L, "LODs");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        if (lua_istable(L, -1)) {
          lua_pushstring(L, "MeshName");
          lua_gettable(L, -2);
          if (lua_isstring(L, -1) && lua_strlen(L, -1)) e->skeleton = skeletons_->Get(lua_tostring(L, -1));
        }
      }
    }
  }
  lua_settop(L, top);
}

// Unit::InitializeArmor: the unit's Defense.ArmorType names a row of /lua/armordefinition.lua
// ({ 'Name', 'DamageType multiplier', ... }); the module is imported on first use.
void Sim::InitializeArmor(lua_State* L, Unit* u) {
  int top = lua_gettop(L);
  if (!armorLoaded_) {
    armorLoaded_ = true;
    if (!PushImport(L, "/lua/armordefinition.lua")) {
      Logf(LogLevel::Warning, "can't load the armordefinition module -- no armor for you.");
    } else {
      lua_pushstring(L, "armordefinition");
      lua_gettable(L, -2);
      if (!lua_istable(L, -1)) {
        Logf(LogLevel::Warning, "The armor module didn't define any armors.  Hmm Odd?");
      } else {
        int defs = lua_gettop(L);
        for (int i = 1;; ++i) {
          lua_rawgeti(L, defs, i);
          if (!lua_istable(L, -1)) break;
          lua_rawgeti(L, -1, 1);
          std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
          lua_pop(L, 1);
          auto& mults = armorTypes_[name];
          for (int k = 2;; ++k) {
            lua_rawgeti(L, -1, k);
            if (!lua_isstring(L, -1)) {
              lua_pop(L, 1);
              break;
            }
            std::string entry = lua_tostring(L, -1);  // "DamageType 0.5"
            size_t sp = entry.find(' ');
            if (sp != std::string::npos)
              mults[entry.substr(0, sp)] = static_cast<float>(std::atof(entry.c_str() + sp + 1));
            lua_pop(L, 1);
          }
          lua_pop(L, 1);
        }
      }
    }
    lua_settop(L, top);
  }
  bps_.PushTable(L, *u->blueprint);
  lua_pushstring(L, "Defense");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "ArmorType");
    lua_gettable(L, -2);
    if (lua_isstring(L, -1)) u->armorType = lua_tostring(L, -1);
  }
  lua_settop(L, top);
}

float Sim::ArmorMult(const Unit* u, const std::string& damageType) const {
  auto o = u->armorOverride.find(damageType);
  if (o != u->armorOverride.end()) return o->second;
  auto t = armorTypes_.find(u->armorType);
  if (t == armorTypes_.end()) return 1.0f;
  auto m = t->second.find(damageType);
  return m == t->second.end() ? 1.0f : m->second;
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
  auto it = entities_.find(id);  // a handle (EntityRef)
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

Unit* Sim::CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete,
                      Unit* builder) {
  lua_checkstack(L, 40);
  int top = lua_gettop(L);
  auto owned = std::make_unique<Unit>();
  Unit* u = owned.get();
  u->kind = Entity::Kind::Unit;
  u->blueprint = &bp;
  u->army = army;
  AttachSkeleton(L, u);
  u->id = ReserveId(army, 0);  // EntityDB::DoReserveId: the lowest free serial of the army's unit pool
  u->position = pos;
  u->orientation = q;
  u->lastPosition = pos;
  RevertCollisionShape(L, u);
  u->fractionComplete = complete ? 1.0f : 0.0f;
  u->beingBuilt = !complete;
  if (!complete) u->unitStates.insert("BeingBuilt");
  u->builderId = EntityRef(builder);
  u->lastMaterializeTick = tick_;
  UnitEconomyInit(L, u);
  u->motion.bp = &GetMotionBlueprint(L, bp, bps_);
  if (u->motion.bp->motionType == kMotionAir && complete && !builder) {
    // IUnit::CalcSpawnElevation: flyers appear at their flying height
    u->position.y = AirSpawnHeight(*this, bp, L, pos.x, pos.z, pos.y);
    u->lastPosition = u->position;
  }
  if (u->motion.bp->motionType == kMotionAir) AirInit(*this, u);
  TransportCreate(*this, u);
  CreateUnitIntel(*this, L, u);
  {
    float h = dmath::Atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x));
    u->motion.fx = dmath::Sin(h);
    u->motion.fz = dmath::Cos(h);
    u->motion.bx = u->motion.fx;
    u->motion.bz = u->motion.fz;
  }
  bps_.PushTable(L, bp);
  int bpIdx = lua_gettop(L);
  u->maxHealth = 1;
  PushPath(L, bpIdx, "Defense", "MaxHealth");
  if (lua_isnumber(L, -1)) u->maxHealth = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  u->health = complete ? u->maxHealth : 1;  // a new construction starts at 1 (FAF's OnStartBuild tests it)
  if (u->motion.bp->motionType != kMotionNone) {  // the motion ctor: fuel (fuel.md 1.1)
    PushPath(L, bpIdx, "Physics", "FuelUseTime");
    u->motion.fuelUseTime = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    PushPath(L, bpIdx, "Physics", "FuelRechargeRate");
    u->motion.fuelRecharge = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    if (u->motion.fuelUseTime > 0) u->fuelRatio = 1.0f;
  }
  PushPath(L, bpIdx, "General", "CapCost");
  if (lua_isnumber(L, -1)) u->capCost = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  if (army) army->unitCost += u->capCost;

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
  RegisterEntity(u);
  AddUnitToLists(u);
  OccupyStructure(*this, u);
  u->isFactoryBuilder = IsFactoryBuilder(*this, u);
  if (builder) u->creatorId = EntityRef(builder);
  if (u->isFactoryBuilder) SetUpInitialRally(*this, u);  // Unit ctor end (0x6a6413)
  if (army && army->pool) army->pool->units.push_back(u);

  CallMethod(L, u, "OnPreCreate", 0);
  u->layer = StartingLayer(L, bpIdx, map_.get(), pos);
  lua_pushstring(L, u->layer.c_str());
  lua_pushstring(L, "None");
  CallMethod(L, u, "OnLayerChange", 2);

  // Weapons: one per bp.Weapon entry; class from unit:GetWeaponClass(label) (FA exe
  // UnitWeapon::CreateInstance), falling back to /lua/sim/Weapon.lua's Weapon.
  PushPath(L, bpIdx, "Weapon");
  int wlist = lua_gettop(L);
  if (lua_istable(L, wlist)) {
    int wclasses = 0;  // unused
    for (int i = 1;; ++i) {
      lua_rawgeti(L, wlist, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      int wbp = lua_gettop(L);
      // Unit ctor (0x6a5fb6): DummyWeapon entries get no UnitWeapon (GetWeaponCount skips them)
      lua_pushstring(L, "DummyWeapon");
      lua_gettable(L, wbp);
      bool dummy = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      if (dummy) {
        lua_pop(L, 1);
        continue;
      }
      std::string label;
      lua_pushstring(L, "Label");
      lua_gettable(L, wbp);
      if (lua_isstring(L, -1)) label = lua_tostring(L, -1);
      lua_pop(L, 1);
      auto wowned = std::make_unique<UnitWeapon>();
      UnitWeapon* w = wowned.get();
      w->unit = u;
      w->index = static_cast<int>(u->weapons.size()) + 1;
      w->label = label;
      lua_pushvalue(L, wbp);
      w->bpRef = luaL_ref(L, LUA_REGISTRYINDEX);
      (void)wclasses;
      lua_pushcfunction(L, ScriptTraceback);
      lua_pushstring(L, "GetWeaponClass");
      lua_gettable(L, obj);
      if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, obj);
        lua_pushstring(L, label.c_str());
        if (lua_pcall(L, 2, 1, -4) != 0) {
          LogScriptError(lua_tostring(L, -1));
          lua_pop(L, 1);
          lua_pushnil(L);
        }
      }
      lua_remove(L, -2);  // handler
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        Logf(LogLevel::Info, "%s:GetWeaponClass(\"%s\") returned nil, falling back to Weapon", bp.id.c_str(),
             label.c_str());
        if (PushImport(L, "/lua/sim/Weapon.lua")) {
          lua_pushstring(L, "Weapon");
          lua_gettable(L, -2);
          lua_remove(L, -2);
        } else {
          lua_pushnil(L);
        }
      }
      int wcls = lua_gettop(L);
      if (!lua_istable(L, wcls)) {
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
      InitUnitWeapon(L, w);
      CallMethod(L, w, "OnCreate", 0);
    }
  }
  lua_settop(L, obj);
  InitializeArmor(L, u);
  CallMethod(L, u, "OnCreate", 0);
  if (complete) UnitFinishedBuilding(*this, L, u);  // adjacency with what it touches
  else if (army) army->stats["Units_BeingBuilt"] += 1;
  PushObject(L, builder);
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
  AttachSkeleton(L, p);
  p->id = ReserveId(nullptr, 2);  // props: one pool, 0x2FFxxxxx
  p->position = pos;
  p->orientation = q;
  p->scale[0] = scale.x;
  p->scale[1] = scale.y;
  p->scale[2] = scale.z;
  RevertCollisionShape(L, p);
  props.push_back(p);
  propGridDirty_ = true;
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
  RegisterEntity(p);
  lua_settop(L, top);
  CallMethod(L, p, "OnCreate", 0);
  return p;
}

Entity* Sim::AdoptScriptEntity(lua_State* L, int t, int spec, bool shield) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  std::unique_ptr<Entity> owned = shield ? std::make_unique<ShieldEntity>() : std::make_unique<Entity>();
  Entity* e = owned.get();
  e->kind = shield ? Entity::Kind::Shield : Entity::Kind::Entity;
  if (lua_istable(L, spec)) {
    lua_pushstring(L, "Owner");
    lua_gettable(L, spec);
    if (Entity* owner = ToObject<Entity>(L, -1)) {
      e->army = owner->army;
      e->position = owner->position;
      e->orientation = owner->orientation;
    }
    lua_pop(L, 1);
  }
  e->id = ReserveId(e->army, shield ? 0x4 : 0x5);  // shields 0x4AAxxxxx, script entities 0x5AAxxxxx
  BindObject(L, t, e);
  owned_.push_back(std::move(owned));
  RegisterEntity(e);
  if (shield) shields.push_back(static_cast<ShieldEntity*>(e));
  return e;
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
