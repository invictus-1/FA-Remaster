// Damage, killing and collision beams (see combat.h).
//
// What the original does (FA exe, read 2026-10-08):
// - Damage(instigator, origin, target, amount, type) hurts one entity (DoDamagePoint 0x737140):
//   no damage to the instigator itself (a projectile counts as its launcher), units multiply by
//   their armour for the damage type, the army statistics count it, and target:OnDamage(
//   instigator, amount, target.position - origin, type) runs when the amount is above 0.
//   Shields are not consulted.
// - DamageArea / DamageRing (0x737680 / 0x737b30) hit every entity whose collision primitive
//   meets the sphere (ring: the outer but not the inner one), skipping allies of the instigator
//   unless damageFriendly and NOSPLASHDAMAGE entities. Shields that overlap the blast (and do not
//   contain its origin) are asked OnGetDamageAbsorption first; what they absorb is taken off the
//   damage of everything inside them and (area only) dealt to the shield afterwards.
// - Unit:Kill (Unit::Kill 0x6a8090) asks CheckCanBeKilled, marks the unit dead, drops adjacency
//   and commands, calls OnKilled(instigator, type, overkill) and counts the statistics; the
//   unit's script destroys it later. Weapons and the rest go at the next beat (KillCleanup).
// - Collision beams (CollisionBeamEntity, MotionTick 0x6735c0): while enabled, every interval+1
//   ticks a ray from the weapon unit's muzzle bone, MaximumBeamLength or the weapon's range long,
//   hits the nearest unit or shield that accepts OnCollisionCheckWeapon(weapon), or the terrain
//   or water; the beam's OnImpact(type, entity) runs on every check (also 'Air' for a miss).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/transport.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace combat {
int Relation(const Army* a, const Army* b);
bool IsEnemy(const Army* a, const Army* b);
bool IsAlly(const Army* a, const Army* b);
float WaterLevel(const Sim& sim);
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs);
bool CallMethodNumber(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs, float* out);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool HasTarget(Sim& sim, const AiTarget& t);
Entity* TargetEntity(Sim& sim, const AiTarget& t);
const char* ImpactTypeName(int t);
int ImpactTypeOf(Sim& sim, const Entity* e, Vec3 at);
}  // namespace combat
using namespace combat;

namespace {

Sim* S(lua_State* L) { return Sim::From(L); }
bool Alive(const Entity* e) { return e && !e->dead && !e->destroyQueued; }
Vec3 ReadVec(lua_State* L, int idx) {
  Vec3 v;
  if (!lua_istable(L, idx)) return v;
  float f[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, idx, i + 1);
    f[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return {f[0], f[1], f[2]};
}
void PushVec(lua_State* L, Vec3 v) { PushVector(L, v.x, v.y, v.z); }
Entity* OptEntity(lua_State* L, int idx) {
  if (lua_isnoneornil(L, idx)) return nullptr;
  ScriptObject* o = GetObject(L, idx);
  return o && (o->typeBits & kTypeEntity) ? static_cast<Entity*>(o) : nullptr;
}

void AddStat(Army* a, const char* name, float v) {
  if (a) a->stats[name] += v;
}

}  // namespace

// ---- damage ---------------------------------------------------------------------------------------

namespace {

struct DamageSpec {
  Entity* instigator = nullptr;
  Vec3 origin;
  float amount = 0;
  std::string type;
  bool friendly = true, self = false;
};

// DoDamagePoint 0x737140
void DoDamagePoint(Sim& sim, lua_State* L, const DamageSpec& d, Entity* target, float amount, Vec3 vector) {
  if (amount == 0 || !target) return;
  if (!d.self) {
    Entity* I = d.instigator;
    bool check = true;
    if (I && I->kind == Entity::Kind::Projectile) {
      I = static_cast<Projectile*>(I)->launcher;
      if (!I) check = false;
    }
    if (check && I == target) return;
  }
  float damage = amount;
  if (target->kind == Entity::Kind::Unit) damage = amount * sim.ArmorMult(static_cast<Unit*>(target), d.type);
  if (d.instigator && d.instigator->army) AddStat(d.instigator->army, "DamageStats_TotalDamageDealt", damage);
  if (d.instigator && target->army) AddStat(target->army, "DamageStats_TotalDamageReceived", damage);
  if (damage > 0 && target->HasLuaObject()) {
    PushObject(L, d.instigator && d.instigator->HasLuaObject() ? d.instigator : nullptr);
    lua_pushnumber(L, damage);
    PushVec(L, vector);
    lua_pushstring(L, d.type.c_str());
    sim.CallMethod(L, target, "OnDamage", 4);
  }
}

struct Absorb {
  ShieldEntity* sh;
  float absorbed;
};

// SIM_DoDamage's shield pre-pass 0x736eb0
std::vector<Absorb> ShieldPrePass(Sim& sim, lua_State* L, const DamageSpec& d, float r0, float r1) {
  std::vector<Absorb> out;
  std::vector<ShieldEntity*> shields = sim.shields;
  for (ShieldEntity* sh : shields) {
    WorldShape ws;
    if (sh->destroyQueued || !GetWorldShape(sh, &ws)) continue;
    if (!d.friendly && d.instigator && sh->army && IsAlly(sh->army, d.instigator->army)) continue;
    if (ws.type == ShapeType::Sphere && Len(Sub(d.origin, ws.c)) <= ws.r - 0.1f) continue;
    if (!SphereOverlap(ws, d.origin, r0) && !(r1 > 0 && SphereOverlap(ws, d.origin, r1))) continue;
    float absorbed = 0;
    if (d.amount > 0) {
      PushObject(L, d.instigator && d.instigator->HasLuaObject() ? d.instigator : nullptr);
      lua_pushnumber(L, d.amount);
      lua_pushstring(L, d.type.c_str());
      CallMethodNumber(sim, L, sh, "OnGetDamageAbsorption", 3, &absorbed);
    }
    if (absorbed > 0) out.push_back({sh, absorbed});
  }
  return out;
}

float ShieldReduce(const std::vector<Absorb>& S_, Entity* e, float amount) {
  for (const Absorb& a : S_) {
    WorldShape ws;
    if (!GetWorldShape(a.sh, &ws)) continue;
    if (PointInShape(ws, e->position)) amount -= a.absorbed;
  }
  return amount;
}

// Entities whose primitive meets the sphere (ring: and not the inner sphere).
std::vector<Entity*> Gather(Sim& sim, Vec3 c, float r, float rInner, bool ring) {
  std::vector<Entity*> out;
  const float margin = 10;
  auto test = [&](Entity* e) {
    WorldShape ws;
    if (e->destroyQueued || !GetWorldShape(e, &ws)) return;
    if (ring) {
      if (SphereOverlap(ws, c, rInner) || !SphereOverlap(ws, c, r)) return;
    } else if (r > 3.0f) {
      Vec3 mn, mx;
      ShapeBounds(ws, &mn, &mx);
      float h = r * 0.707f;
      bool cube = mn.x <= c.x + h && mx.x >= c.x - h && mn.y <= c.y + h && mx.y >= c.y - h && mn.z <= c.z + h &&
                  mx.z >= c.z - h;
      if (!cube && !SphereOverlap(ws, c, r)) return;
    } else if (!SphereOverlap(ws, c, r)) {
      return;
    }
    out.push_back(e);
  };
  std::vector<Unit*> units;
  sim.ForUnitsInRect(c.x - r - margin, c.z - r - margin, c.x + r + margin, c.z + r + margin,
                     [&](Unit* u) { units.push_back(u); });
  std::sort(units.begin(), units.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* u : units) test(u);
  sim.ForPropsInRect(c.x - r - margin, c.z - r - margin, c.x + r + margin, c.z + r + margin, [&](Prop* p) { test(p); });
  for (Projectile* p : sim.projectiles)
    if (std::fabs(p->position.x - c.x) <= r + margin && std::fabs(p->position.z - c.z) <= r + margin) test(p);
  for (ShieldEntity* sh : sim.shields) test(sh);
  return out;
}

void DoDamageArea(Sim& sim, lua_State* L, const DamageSpec& d, float r0, float r1, bool ring) {
  std::vector<Absorb> shields = ShieldPrePass(sim, L, d, r0, ring ? r1 : 0);
  std::vector<Entity*> hit = ring ? Gather(sim, d.origin, r1, r0, true) : Gather(sim, d.origin, r0, 0, false);
  for (Entity* e : hit) {
    if (!d.friendly && d.instigator && d.instigator->army && e->army && IsAlly(d.instigator->army, e->army)) continue;
    if (e->blueprint && BpInCategory(sim, e->blueprint, "NOSPLASHDAMAGE")) continue;
    float amt = ShieldReduce(shields, e, d.amount);
    if (amt <= 0) continue;
    DoDamagePoint(sim, L, d, e, amt, Sub(e->position, d.origin));
  }
  if (!ring)
    for (const Absorb& a : shields) DoDamagePoint(sim, L, d, a.sh, a.absorbed, Sub(a.sh->position, d.origin));
}

// Damage(instigator, origin, target, amount, damageType)
int l_Damage(lua_State* L) {
  if (lua_gettop(L) != 5) return luaL_error(L, "Damage: expected 5 args, but got %d", lua_gettop(L));
  DamageSpec d;
  d.instigator = OptEntity(L, 1);
  d.origin = ReadVec(L, 2);
  Entity* target = CheckObject<Entity>(L, 3);
  d.amount = static_cast<float>(luaL_checknumber(L, 4));
  d.type = luaL_checkstring(L, 5);
  if (d.amount == 0) return luaL_error(L, "0 damage specified.");
  if (target->dead) return 0;
  DoDamagePoint(*S(L), L, d, target, d.amount, Sub(target->position, d.origin));
  return 0;
}
// DamageArea(instigator, location, radius, amount, damageType, damageFriendly [, damageSelf])
int l_DamageArea(lua_State* L) {
  DamageSpec d;
  d.instigator = OptEntity(L, 1);
  d.origin = ReadVec(L, 2);
  float r = static_cast<float>(luaL_checknumber(L, 3));
  d.amount = static_cast<float>(luaL_checknumber(L, 4));
  d.type = luaL_checkstring(L, 5);
  d.friendly = lua_toboolean(L, 6) != 0;
  d.self = lua_toboolean(L, 7) != 0;
  if (d.amount == 0) return luaL_error(L, "0 damage specified.");
  if (r == 0) return luaL_error(L, "0 radius specified.");
  DoDamageArea(*S(L), L, d, r, 0, false);
  return 0;
}
// DamageRing(instigator, location, minRadius, maxRadius, amount, damageType, damageFriendly [, damageSelf])
int l_DamageRing(lua_State* L) {
  DamageSpec d;
  d.instigator = OptEntity(L, 1);
  d.origin = ReadVec(L, 2);
  float r0 = static_cast<float>(luaL_checknumber(L, 3)), r1 = static_cast<float>(luaL_checknumber(L, 4));
  d.amount = static_cast<float>(luaL_checknumber(L, 5));
  d.type = luaL_checkstring(L, 6);
  d.friendly = lua_toboolean(L, 7) != 0;
  d.self = lua_toboolean(L, 8) != 0;
  if (d.amount == 0) return luaL_error(L, "0 damage specified.");
  if (r0 == 0) return luaL_error(L, "0 min radius specified.");
  if (r1 == 0) return luaL_error(L, "0 max radius specified.");
  if (!(r0 < r1)) return luaL_error(L, "Max radius must be greater than min radius.");
  DoDamageArea(*S(L), L, d, r0, r1, true);
  return 0;
}

}  // namespace

// ---- killing --------------------------------------------------------------------------------------

void KillUnit(Sim& sim, lua_State* L, Unit* u, Entity* instigator, const std::string& type, float ratio) {
  if (u->dead) return;
  PushObject(L, u);
  int ok = CallMethodBool(sim, L, u, "CheckCanBeKilled", 1);
  if (ok != 1) {
    if (!BpInCategory(sim, u->blueprint, "COMMAND")) return;
    const TerrainMap* m = sim.map();
    if (!m || (u->position.x >= 0 && u->position.z >= 0 && u->position.x <= m->width() && u->position.z <= m->height()))
      return;
  }
  u->unitStates.insert("NoCost");
  static const bool dbg = getenv("MOHO64_DEBUG_COMBAT") != nullptr;
  if (dbg)
    Logf(LogLevel::Debug, "moho64: tick %u kill %08x %s (army %d) by %08x %s (army %d) type '%s'", sim.tick(), u->id,
         u->blueprint ? u->blueprint->id.c_str() : "?", u->army ? u->army->index : 0, instigator ? instigator->id : 0,
         instigator && instigator->blueprint ? instigator->blueprint->id.c_str() : "-",
         instigator && instigator->army ? instigator->army->index : 0, type.c_str());
  ratio = TransportOnKillBegin(sim, L, u, ratio);  // step 5: cargo leaves its transport
  if (u->beingBuilt && u->fractionComplete < 0.5f) ratio = 10.0f;
  sim.CallMethod(L, u, "SetDead", 0);
  u->dead = true;
  TransportOnKillEnd(sim, L, u);  // steps 8 and 11: attach callbacks, a transport's cargo
  u->killCleanup = true;
  if (!u->beingBuilt) AdjacencyLost(sim, L, u);
  ForgetUnitCommands(u);
  PushObject(L, instigator && instigator->HasLuaObject() ? instigator : nullptr);
  lua_pushstring(L, type.c_str());
  lua_pushnumber(L, ratio);
  sim.CallMethod(L, u, "OnKilled", 3);
  // statistics
  float mass = u->bpData ? u->bpData->buildCostMass : 0, energy = u->bpData ? u->bpData->buildCostEnergy : 0;
  if (u->beingBuilt) {
    mass *= u->fractionComplete;
    energy *= u->fractionComplete;
  }
  AddStat(u->army, "Units_Killed", 1);
  AddStat(u->army, "Units_MassValue_Lost", mass);
  AddStat(u->army, "Units_EnergyValue_Lost", energy);
  if (instigator && instigator->army && IsEnemy(u->army, instigator->army)) {
    AddStat(instigator->army, "Enemies_Killed", 1);
    AddStat(instigator->army, "Enemies_MassValue_Destroyed", mass);
    AddStat(instigator->army, "Enemies_EnergyValue_Destroyed", energy);
    if (BpInCategory(sim, u->blueprint, "COMMAND")) AddStat(instigator->army, "Enemies_Commanders_Destroyed", 1);
  }
}

namespace {

// Entity:Kill(instigator, type, excessDamageRatio) 0x691d10
int l_Kill(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Sim& sim = *S(L);
  if (e->dead) return 0;
  Entity* inst = OptEntity(L, 2);
  Entity* k = inst && inst->army ? inst : nullptr;
  std::string type = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
  float ratio = lua_isnumber(L, 4) ? static_cast<float>(lua_tonumber(L, 4)) : 0.0f;
  bool selfBuilt = e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->beingBuilt;
  Army* selfArmy = e->army;
  if (e->kind == Entity::Kind::Unit) {
    KillUnit(sim, L, static_cast<Unit*>(e), k, type, ratio);
  } else if (e->kind == Entity::Kind::Prop) {
    PushObject(L, k);
    lua_pushstring(L, type.c_str());
    lua_pushnumber(L, ratio);
    sim.CallMethod(L, e, "OnKilled", 3);
  } else {
    e->dead = true;
  }
  // the instigating unit's KILLS statistic
  if (inst && inst->kind == Entity::Kind::Unit && inst->HasLuaObject() &&
      (e->kind == Entity::Kind::Unit || e->kind == Entity::Kind::Projectile) && !selfBuilt &&
      !(e->blueprint && BpInCategory(sim, e->blueprint, "BENIGN")) && selfArmy != inst->army) {
    PushObjectValue(L, inst, "stat:KILLS");
    float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    lua_pushnumber(L, v + 1);
    SetObjectValue(L, inst, "stat:KILLS", -1);
    lua_pop(L, 1);
  }
  return 0;
}

int l_GetArmorMult(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  lua_pushnumber(L, S(L)->ArmorMult(u, luaL_checkstring(L, 2)));
  return 1;
}
int l_AlterArmor(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  u->armorOverride[luaL_checkstring(L, 2)] = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}

}  // namespace

// ---- collision beams ------------------------------------------------------------------------------

namespace {

CollisionBeam* B(lua_State* L) { return CheckObject<CollisionBeam>(L, 1); }

Vec3 BeamMuzzle(CollisionBeam* b, Quat* rot) {
  Vec3 p;
  Quat q;
  Unit* u = b->weapon ? b->weapon->unit : nullptr;
  if (!u) {
    *rot = b->orientation;
    return b->position;
  }
  BoneWorld(u, b->attachBone, &p, &q);
  *rot = q;
  return p;
}

// CheckCollision 0x6732d0 / CreateCollisionBeamHelper 0x6d6b00
void BeamCheck(Sim& sim, lua_State* L, CollisionBeam* b) {
  UnitWeapon* w = b->weapon;
  if (!w || !w->unit || !w->bp) return;
  Unit* owner = w->unit;
  float Lr = w->bp->maximumBeamLength > 0 ? w->bp->maximumBeamLength
             : (w->ovMaxRadius >= 0 ? w->ovMaxRadius : w->bp->maxRadius);
  Quat q;
  Vec3 P = BeamMuzzle(b, &q);
  b->position = P;
  b->orientation = q;
  Vec3 dir = Forward(q);
  float h = Lr * std::sqrt(dir.x * dir.x + dir.z * dir.z);
  float s = h > 0.001f ? std::min(Lr / h, 5.0f) : 1.0f;
  Vec3 E = Add(P, Mul(dir, s * Lr));
  // entities: units and shields
  Entity* best = nullptr;
  float bestD = std::numeric_limits<float>::infinity();
  Vec3 bestHit;
  auto consider = [&](Entity* e) {
    if (e == owner || e->destroyQueued) return;
    WorldShape ws;
    Vec3 hit;
    float d;
    if (!GetWorldShape(e, &ws) || !SegmentHit(ws, P, E, &hit, &d)) return;
    PushObject(L, w);
    if (CallMethodBool(sim, L, e, "OnCollisionCheckWeapon", 1) != 1) return;
    if (w->bp->ignoresAlly && e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->layer == "Air" &&
        !IsEnemy(owner->army, e->army))
      return;
    if (!(d < bestD)) return;
    best = e;
    bestD = d;
    bestHit = hit;
  };
  float x0 = std::min(P.x, E.x) - 10, x1 = std::max(P.x, E.x) + 10, z0 = std::min(P.z, E.z) - 10,
        z1 = std::max(P.z, E.z) + 10;
  std::vector<Unit*> units;
  sim.ForUnitsInRect(x0, z0, x1, z1, [&](Unit* u) { units.push_back(u); });
  std::sort(units.begin(), units.end(), [](const Unit* a, const Unit* c) { return a->id < c->id; });
  for (Unit* u : units) consider(u);
  for (ShieldEntity* sh : std::vector<ShieldEntity*>(sim.shields)) consider(sh);
  float D = Lr;
  Vec3 impactPos = E;
  int type = 3;  // Air
  Entity* target = nullptr;
  if (best) {
    target = best;
    impactPos = bestHit;
    if (best->kind == Entity::Kind::Shield) D = Len(Sub(P, bestHit));
    else D = Len(Sub(P, best->position));
    type = ImpactTypeOf(sim, best, impactPos);
  }
  // terrain and water
  float td;
  Vec3 th;
  const TerrainMap* m = sim.map();
  float tdist = std::numeric_limits<float>::infinity();
  int ttype = 0;
  if (TerrainSegmentHit(m, P, E, &td, &th)) {
    tdist = td;
    ttype = 1;
  }
  float wl = WaterLevel(sim);
  if (m && m->hasWater && (P.y - wl) * (E.y - wl) < 0) {
    float t = (P.y - wl) / (P.y - E.y);
    float wd = t * Len(Sub(E, P));
    if (wd < tdist) {
      tdist = wd;
      ttype = 2;
      th = Add(P, Mul(Sub(E, P), t));
    }
  }
  if (ttype && tdist < D) {
    D = tdist;
    target = nullptr;
    impactPos = th;
    type = ttype;
  }
  b->length = D;
  lua_pushstring(L, ImpactTypeName(type));
  PushObject(L, target && target->HasLuaObject() ? target : nullptr);
  sim.CallMethod(L, b, "OnImpact", 2);
}

// CollisionBeamEntity.__init(self, spec)
int l_beam_init(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  Sim& sim = *S(L);
  lua_pushstring(L, "Weapon");
  lua_gettable(L, 2);
  UnitWeapon* w = ToObject<UnitWeapon>(L, -1);
  lua_pop(L, 1);
  if (!w) return luaL_error(L, "CollisionBeamEntity: spec.Weapon is not a weapon");
  auto owned = std::make_unique<CollisionBeam>();
  CollisionBeam* b = owned.get();
  b->kind = Entity::Kind::Beam;
  b->weapon = w;
  b->army = w->unit ? w->unit->army : nullptr;
  b->id = sim.ReserveId(b->army, 0x5);
  b->position = w->unit ? w->unit->position : Vec3{};
  BindObject(L, 1, b);
  sim.AddEntity(std::move(owned));
  sim.beams.push_back(b);
  lua_pushvalue(L, 2);
  sim.CallMethod(L, b, "OnCreate", 1);
  b->interval = static_cast<int>(lu::Num(L, 2, "CollisionCheckInterval", 1));
  lua_pushstring(L, "OtherBone");
  lua_gettable(L, 2);
  int bone = -1;
  if (w->unit && w->unit->skeleton) {
    if (lua_type(L, -1) == LUA_TNUMBER) bone = static_cast<int>(lua_tonumber(L, -1));
    else if (lua_isstring(L, -1)) bone = w->unit->skeleton->Find(lua_tostring(L, -1));
  }
  lua_pop(L, 1);
  b->attachParent = EntityRef(w->unit);
  b->attachBone = bone;
  return 0;
}
int l_beam_Enable(lua_State* L) {
  CollisionBeam* b = B(L);
  S(L)->CallMethod(L, b, "OnEnable", 0);
  b->enabled = true;
  b->counter = b->interval;
  return 0;
}
int l_beam_Disable(lua_State* L) {
  CollisionBeam* b = B(L);
  S(L)->CallMethod(L, b, "OnDisable", 0);
  b->enabled = false;
  return 0;
}
int l_beam_IsEnabled(lua_State* L) {
  lua_pushboolean(L, B(L)->enabled);
  return 1;
}
int l_beam_GetLauncher(lua_State* L) {
  CollisionBeam* b = B(L);
  Unit* u = b->weapon ? b->weapon->unit : nullptr;
  if (u && u->HasLuaObject()) PushObject(L, u);
  else lua_pushnil(L);
  return 1;
}
int l_beam_SetBeamFx(lua_State* L) {
  CollisionBeam* b = B(L);
  bool check = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
  if (check && b->weapon && b->weapon->HasLuaObject()) BeamCheck(*S(L), L, b);
  return 0;
}
int l_IsCollisionBeam(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (o && dynamic_cast<CollisionBeam*>(o)) lua_pushvalue(L, 1);
  else lua_pushnil(L);
  return 1;
}

}  // namespace

void BeamsTick(Sim& sim) {
  lua_State* L = sim.L();
  std::vector<CollisionBeam*> beams = sim.beams;
  for (CollisionBeam* b : beams) {
    if (b->destroyQueued || !b->HasLuaObject()) continue;
    UnitWeapon* w = b->weapon;
    if (!w || !w->HasLuaObject() || !w->unit || w->unit->destroyQueued) continue;
    Quat q;
    b->position = BeamMuzzle(b, &q);
    b->orientation = q;
    if (!b->enabled) continue;
    int old = b->counter++;
    if (old >= b->interval) {
      BeamCheck(sim, L, b);
      b->counter = 0;
    }
  }
}

void RegisterDamageBindings(lua_State* L) {
  SetGlobal(L, "Damage", l_Damage);
  SetGlobal(L, "DamageArea", l_DamageArea);
  SetGlobal(L, "DamageRing", l_DamageRing);
  SetMethod(L, "Entity", "Kill", l_Kill);
  SetMethod(L, "Unit", "GetArmorMult", l_GetArmorMult);
  SetMethod(L, "Unit", "AlterArmor", l_AlterArmor);
  SetMethod(L, "CollisionBeamEntity", "__init", l_beam_init);
  SetMethod(L, "CollisionBeamEntity", "Enable", l_beam_Enable);
  SetMethod(L, "CollisionBeamEntity", "Disable", l_beam_Disable);
  SetMethod(L, "CollisionBeamEntity", "IsEnabled", l_beam_IsEnabled);
  SetMethod(L, "CollisionBeamEntity", "GetLauncher", l_beam_GetLauncher);
  SetMethod(L, "CollisionBeamEntity", "SetBeamFx", l_beam_SetBeamFx);
  SetGlobal(L, "IsCollisionBeam", l_IsCollisionBeam);
}

}  // namespace moho
