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
//   hits the nearest unit, shell or shield that accepts OnCollisionCheckWeapon(weapon), or the terrain
//   or water; the beam's OnImpact(type, entity) runs on every check (also 'Air' for a miss).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/transport.h"
#include "sim/air.h"
#include "sim/ai_platoon.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/landnav.h"
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
bool TerrainSegmentHitCDE(const TerrainMap* map, Vec3 C, Vec3 D, float ext, float* dist, Vec3* hit);
bool WaterPlaneSegmentHitCDE(Vec3 C, Vec3 D, float ext, float water, float* dist, Vec3* hit);
bool SegmentStartUnderWaterCDE(Vec3 C, Vec3 D, float ext, float water, float* dist, Vec3* hit);
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
  float radius = 0;  // (for a host's hit list only: DamageArea's radius, DamageRing's outer one)
};

int KindOf(const Entity* e) {  // HitEvent::targetKind
  switch (e->kind) {
    case Entity::Kind::Unit: return 0;
    case Entity::Kind::Shield: return 1;
    case Entity::Kind::Prop: return 2;
    case Entity::Kind::Projectile: return 3;
    default: return 4;
  }
}
void Put3(float out[3], Vec3 v) {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
}
Vec3 UnitOrZero(Vec3 v) {
  const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l > 0 && std::isfinite(l) ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

// One damage for a host's hit list (render REQUESTS 36). It reads only: the shell or beam the damage is credited to
// is the one whose OnImpact runs now (Sim::impactShot) when the damage call names it or its launcher as the
// instigator (FAF's Projectile.OnImpact passes the launcher, or the shell when the launcher is gone; its
// CollisionBeam.OnImpact the beam's unit), else a shell named as the instigator itself. A death weapon fired from
// a unit killed by that damage names the dying unit, so it is not credited to the shell.
void RecordHit(Sim& sim, const DamageSpec& d, const Entity* target, float damage, Vec3 vector) {
  HitEvent h;
  h.target = target->id;
  h.targetKind = KindOf(target);
  if (target->kind == Entity::Kind::Shield) {
    h.shield = true;
    h.shieldId = target->id;
    // the bubble is attached to its owner (FAF's Shield.OnCreate: AttachBoneTo(-1, spec.Owner, -1))
    if (const Entity* owner = target->attachParent ? sim.FindEntity(target->attachParent) : nullptr) h.target = owner->id;
  }
  const Entity* I = d.instigator;
  if (I) {
    h.instigator = I->id;
    h.army = I->army ? I->army->index : 0;
  }
  const Entity* by = nullptr;
  if (const Entity* shot = sim.impactShot) {
    const Entity* shooter = shot->kind == Entity::Kind::Projectile ? static_cast<const Projectile*>(shot)->launcher
                            : static_cast<const CollisionBeam*>(shot)->weapon
                                ? static_cast<const CollisionBeam*>(shot)->weapon->unit
                                : nullptr;
    if (I && (I == shot || I == shooter)) by = shot;
  }
  if (!by && I && I->kind == Entity::Kind::Projectile) by = I;
  Vec3 dir = UnitOrZero(vector);
  if (by && by->kind == Entity::Kind::Projectile) {
    const Projectile* p = static_cast<const Projectile*>(by);
    h.weapon = WeaponBlueprintId(p->weapon);
    if (p->blueprint) h.projectile = p->blueprint->id;
    const Vec3 v = UnitOrZero(p->velocity);
    if (v.x != 0 || v.y != 0 || v.z != 0) dir = v;
  } else if (by && by->kind == Entity::Kind::Beam) {
    h.weapon = WeaponBlueprintId(static_cast<const CollisionBeam*>(by)->weapon);
    h.beam = true;
    dir = Forward(by->orientation);
  }
  h.damageType = d.type;
  h.amount = damage;
  Put3(h.position, d.origin);
  h.radius = d.radius;
  Put3(h.direction, dir);
  sim.hitEvents.push_back(std::move(h));
}

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
  // the army statistics, and a unit's damage also for its blueprint (0x7374af, 0x737597)
  if (d.instigator && d.instigator->army) {
    AddStat(d.instigator->army, "DamageStats_TotalDamageDealt", damage);
    if (d.instigator->kind == Entity::Kind::Unit)
      d.instigator->army->AddBpStat("Units_TotalDamageDealt", d.instigator->blueprint, damage);
  }
  if (d.instigator && target->army) {
    AddStat(target->army, "DamageStats_TotalDamageReceived", damage);
    if (target->kind == Entity::Kind::Unit) target->army->AddBpStat("Units_TotalDamageReceive", target->blueprint, damage);
  }
  if (d.instigator)  // the platoons' lifetime stats (+0xec dealt by a unit, +0xf0 taken by a unit; damage.md 4)
    PlatoonStatDamage(d.instigator->army && d.instigator->kind == Entity::Kind::Unit ? static_cast<Unit*>(d.instigator)
                                                                                      : nullptr,
                      target->army && target->kind == Entity::Kind::Unit ? static_cast<Unit*>(target) : nullptr, damage);
  {
    // MOHO64_DEBUG_DAMAGE=<entity id>: every damage that entity is dealt (who, how much, the script's call chain)
    static const long dbgDmg = getenv("MOHO64_DEBUG_DAMAGE") ? atol(getenv("MOHO64_DEBUG_DAMAGE")) : -1;
    if (dbgDmg >= 0 && static_cast<long>(target->id) == dbgDmg) {
      std::string where;
      lua_Debug ar;
      for (int lvl = 0; lvl < 6 && lua_getstack(L, lvl, &ar); ++lvl) {
        lua_getinfo(L, "Sl", &ar);
        if (ar.currentline > 0) where += std::string(where.empty() ? "" : " < ") + ar.short_src + ":" + std::to_string(ar.currentline);
      }
      const Entity* I = d.instigator;
      Logf(LogLevel::Info, "damage %u to %u: %.3f (%s) by %s %d from %.2f %.2f %.2f; %s", sim.tick(), target->id, damage,
           d.type.c_str(),
           !I ? "nobody" : I->kind == Entity::Kind::Projectile ? "shell" : I->kind == Entity::Kind::Unit ? "unit" : "entity",
           I ? static_cast<int>(I->id) : -1, d.origin.x, d.origin.y, d.origin.z, where.c_str());
    }
  }
  if (damage > 0 && target->HasLuaObject()) {
    if (sim.collectHits) RecordHit(sim, d, target, damage, vector);  // (a host's list, before what OnDamage does)
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
  auto test = [&](Entity* e) {
    WorldShape ws;
    if (!GetWorldShape(e, &ws)) return;  // destroy-queued entities stay candidates until teardown (damage.md 1.6)
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
  // COGrid::ForAllEntitiesIterator 0x721fb0 / the ring's 0x722560: the entity-grid cells covering the sphere's
  // box (AABoxToRect 0x4fcbe0: floor / ceil), in grid order; destroy-queued entities are still there until
  // teardown. Both gather with the mask 0xf00 (0x737727 / 0x722650: GatherUnmarkedEntities 0x722df0): units, props,
  // shells and the rest of the third list (shields, script entities), each where its cells are first met, after the
  // cell's units and props. So a shield takes its damage (and its OnDamage its Random) in the cell order, not after
  // everything else. Seen: MP2 11690, army 0's ACU death blast (acudeathweapon.lua:46) meets army 1's shield
  // 0x40100005 (at 240.9, 409.1); the original's ApplyDamage Random (shield.lua:614) comes before the T2
  // 0x1001ce (uea0203)'s death effects and debris. Ours took the shields, and the shells by their distance alone,
  // after the grid's units and props.
  std::vector<Entity*> cand;
  sim.entityGrid().GatherBox(c.x - r, c.z - r, c.x + r, c.z + r, 15, &cand);
  for (Entity* e : cand) test(e);
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
  d.radius = r;
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
  d.radius = r1;
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
    // 0x6a811c: a commander that may not be killed dies anyway outside the playable rect (edges included)
    const int* r = sim.playableRect;
    if (!sim.map() || (!(static_cast<float>(r[0]) > u->position.x) && !(static_cast<float>(r[1]) > u->position.z) &&
                       !(u->position.x > static_cast<float>(r[2])) && !(u->position.z > static_cast<float>(r[3]))))
      return;
  }
  u->unitStates.Insert(US("NoCost"));
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
  sim.RequestRefreshUI(u);  // (Entity::Kill 0x679a80 sets entity+0x9a for every kill, 0x679ad8)
  // (Unit::Kill frees no o-grid reservation: 0x6a83b9 is ReserveOgridRect 0x6ab6f0 on each overlapping neighbour;
  // the unit's own goes in Unit::OnDestroy 0x6a8911, for a unit that occupies ground)
  TransportOnKillEnd(sim, L, u);  // steps 8 and 11: attach callbacks, a transport's cargo
  u->killCleanup = true;
  if (!u->beingBuilt) AdjacencyLost(sim, L, u);
  const bool hadCommands = !u->commands.empty();
  // 0x6a841a: CUnitCommandQueue::ClearCommandQueue 0x6ee2d0, the same teardown IssueClearCommands makes (cfunc
  // 0x6f1950), without its attacker stop: a move under way ends in its CUnitMoveTask's dtor, whose AbortMove goes
  // through MakeIdle to ResetPathState 0x5ad9c0, so PathFinding and ProblemGettingToGoal go too. Seen: MP1 7967, army
  // 6's engineer 0x600015, killed on a Move whose path ended short of its goal: the original's states lose
  // ProblemGettingToGoal in the kill's beat; ours kept it (the queue forgotten without the move's abort).
  IssueClearCommandsTo(sim, u, false);
  // the emptied queue's event (Unit::UpdateSpeedThroughStatus 0x6ac940) reaches a killed aircraft too: with nothing
  // queued its speed-through flag goes (seen: five patrolling aircraft killed in one beat, each 1 -> 0)
  if (u->motion.air) {
    u->motion.airSpeedThrough = false;
    u->motion.air->fullSpeed = false;
  } else if (hadCommands && u->motion.nav) {
    // and a ground unit's: IAiCommandDispatchImpl::OnEvent 0x599030 (event 0) -> UpdateSpeedThroughStatus, whose
    // empty queue gives navigator->SetSpeedThroughGoal(false) (0x6acaf1, vt+0x18) -> CAiSteeringImpl 0x5d2390
    // (seen: Skirmish 2764, two engineers 0xd / 0x15 killed on a move with another queued, each 1 -> 0)
    LandNavSetSpeedThrough(u, false);
  }
  u->unitStates.Erase(US("Moving"));  // (the move task goes with the queue: a unit killed on its way is not "Moving")
  u->unitStates.Erase(US("Patrolling"));  // (nor "Patrolling": the patrol task goes the same way)
  u->unitStates.Erase(US("Guarding"));    // (nor guarding)
  u->unitStates.Erase(US("GuardBusy"));
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
  // (Unit::Kill 0x6a84b6: Units_Killed and Enemies_Killed also count the victim's blueprint, 0x6a84cf / 0x6a8667)
  if (u->army) u->army->AddBpStat("Units_Killed", u->blueprint, 1.0f);
  AddStat(u->army, "Units_Killed", 1);
  PlatoonStatKill(u, instigator && instigator->kind == Entity::Kind::Unit && instigator->army &&
                         IsEnemy(u->army, instigator->army) ? static_cast<Unit*>(instigator) : nullptr);
  AddStat(u->army, "Units_MassValue_Lost", mass);
  AddStat(u->army, "Units_EnergyValue_Lost", energy);
  if (instigator && instigator->army && IsEnemy(u->army, instigator->army)) {
    AddStat(instigator->army, "Enemies_Killed", 1);
    instigator->army->AddBpStat("Enemies_Killed", u->blueprint, 1.0f);
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
  sim.RequestRefreshUI(e);  // (Entity::Kill sets entity+0x9a too, 0x679ad8)
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

Vec3 BeamMuzzle(Sim& sim, CollisionBeam* b, Quat* rot) {
  Vec3 p;
  Quat q;
  Unit* u = b->weapon ? b->weapon->unit : nullptr;
  if (!u) {
    *rot = b->orientation;
    return b->position;
  }
  // (The muzzle bone by the unit's pose of THIS beat: the unit's manipulators have had their turn before the
  // beam's. Tried twice and wrong, 2026-10-13: the pose of the beat before with the unit's place of the beat
  // before (Experimental stops matching at 452, Naval at 148), and the pose of the beat before with the unit's
  // place of this beat (Experimental 423, Naval 137).)
  (void)sim;
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
  Vec3 P = BeamMuzzle(sim, b, &q);
  sim.HoldShape(b);  // (its committed transform stays what the beat began with: BoneWorld)
  b->position = P;
  b->orientation = NormalizeQuatExe(q);  // (the beam's own transform: see BeamTick)
  Vec3 dir = Forward(q);
  // the horizontal reach: the distance from P to the rounded end point P + dir * L (0x6d6cc2..0x6d6d07)
  const float ex = P.x - (dir.x * Lr + P.x), ez = P.z - (P.z + dir.z * Lr);
  float h = std::sqrt(ex * ex + ez * ez);
  float s = h > 0.001f ? std::min(Lr / h, 5.0f) : 1.0f;
  const float len = s * Lr;
  Vec3 E = Add(P, Mul(dir, len));
  // Every entity with a collision shape that the segment crosses (0x7229c0 asks the collision grid, whatever
  // the entity is), then the choice among them (0x6d6a00): the script's OnCollisionCheckWeapon first (0x6dd5d0,
  // asked of the firing unit too), then allied aircraft, then the nearest, and last the firing unit is left out.
  Entity* best = nullptr;
  float bestD = std::numeric_limits<float>::infinity();
  Vec3 bestHit;
  auto consider = [&](Entity* e) {
    if (e->destroyQueued) return;
    WorldShape ws;
    Vec3 hit;
    float d;
    if (!GetWorldShape(e, &ws) || !SegmentHit(ws, P, E, &hit, &d)) return;
    PushObject(L, w);
    if (CallMethodBool(sim, L, e, "OnCollisionCheckWeapon", 1) != 1) return;
    if (w->bp->ignoresAlly && e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->layer == "Air" &&
        !IsEnemy(owner->army, e->army))
      return;
    if (!(d < bestD) || e == owner) return;
    best = e;
    bestD = d;
    bestHit = hit;
  };
  // Shells too: a missile has a sphere of its own and a beam meets it like anything else (the Cybran zapper: its
  // beam is drawn up to five times the range long, and the missile it aims at is struck where the two cross).
  if (sim.entityGrid().ready()) {
    // (0x6d6e56: the grid's line query, the cells from the muzzle outward; the order the scripts are asked in)
    std::vector<Entity*> cands;
    sim.entityGrid().GatherLine(P.x, P.z, E.x, E.z, &cands);
    for (Entity* e : cands)
      if (e->kind != Entity::Kind::Beam) consider(e);
  } else {
    float x0 = std::min(P.x, E.x) - 10, x1 = std::max(P.x, E.x) + 10, z0 = std::min(P.z, E.z) - 10,
          z1 = std::max(P.z, E.z) + 10;
    std::vector<Unit*> units;
    sim.ForUnitsInRect(x0, z0, x1, z1, [&](Unit* u) { units.push_back(u); });
    std::sort(units.begin(), units.end(), [](const Unit* a, const Unit* c) { return a->id < c->id; });
    for (Unit* u : units) consider(u);
    for (Projectile* o : std::vector<Projectile*>(sim.projectiles))
      if (o->shape.type != ShapeType::None) consider(o);
    for (ShieldEntity* sh : std::vector<ShieldEntity*>(sim.shields)) consider(sh);
    for (auto it = sim.scriptEntities.rbegin(); it != sim.scriptEntities.rend(); ++it)
      if ((*it)->kind == Entity::Kind::Entity && (*it)->shape.type != ShapeType::None) consider(*it);
  }
  float D = Lr;
  Vec3 impactPos = E;
  int type = 3;  // Air
  Entity* target = nullptr;
  if (best) {
    target = best;
    impactPos = bestHit;
    // (0x6d6ea0..0x6d6fb5: the length is measured to the hit point on a shield (an id 0x4xxxxxxx, 0x6d6ec8), to
    // a unit's bone -1, half its SizeY above its place (Entity vt+0x10 IsUnit, then vt+0x48 GetBoneWorldTransform
    // with -1, 0x6d6f29), and to anything else's place (+0xac); ((x*x + y*y) + z*z), 0x452fc0. Ours measured a unit
    // to its origin: the beam's end, bone 1, lay 0.005 to 0.07 further in than the original's (Naval's beam lines).)
    if (best->kind == Entity::Kind::Shield) D = Len(Sub(P, bestHit));
    else if (best->kind == Entity::Kind::Unit) D = Len(Sub(P, UnitBoneMinus1(*static_cast<Unit*>(best), best->position)));
    else  // (its COMMITTED place, +0xac..+0xb4: 0x6d6f75..0x6d6f85; a shell that stepped earlier in this beat too)
      D = Len(Sub(P, best->shapeHeld ? best->shapePos : best->position));
    type = ImpactTypeOf(sim, best, impactPos);
  }
  // The terrain and the water: STIMap::SurfaceIntersection 0x577bc0 along the beam's own segment (0x6d6d47..
  // 0x6d6e0f: centre P + F (len / 2), direction F, the hull's forward column as it is, not normalised, half length
  // len / 2; not 0x4fe130's segment of P -> E), from its start C - F len / 2 over len (0x6d6fbb..0x6d7095). As the
  // ballistic fall asks it (air.cpp CalcMoveBallistic): on a map with water, a start under the water is the hit
  // at distance 0 (0x577bdf..0x577c47); else the terrain's point (STIMap::Intersection 0x476f30) when there is no
  // water or it is not under the water (0x577c7d..0x577c92: type 1), else the water plane by
  // CColHitResult::PlaneIntersection 0x577540 (type 3). The beam takes it when its length so far is longer
  // (0x6d70a7..0x6d70b7): the entity met is let go (0x6d70bc..0x6d70dc), type 3 is Water and type 1 Terrain
  // (0x6d70e9..0x6d711b); a start under the water leaves the type as it was, and with none its point's type, by
  // ENT_GetImpactType 0x67b240 without an entity: Underwater below the water, else Air. Ours took the terrain's
  // point also under the water, along 0x4fe130's segment, and crossed the water by its own formula, t x |E - P|:
  // Naval's and Economy's beam lines on water and terrain ended a last bit off the original's.
  {
    const TerrainMap* m = sim.map();
    const bool water = m && m->hasWater;
    const float wl = WaterLevel(sim);
    const Vec3 SC{(dir.x * len) * 0.5f + P.x, P.y + (dir.y * len) * 0.5f, P.z + (dir.z * len) * 0.5f};
    const float sext = len * 0.5f;
    float sdist = 0.0f;
    Vec3 sh{};
    int stype = -1;  // (none; 0 the start under the water, 1 the terrain, 3 the water plane)
    if (water && SegmentStartUnderWaterCDE(SC, dir, sext, wl, &sdist, &sh)) {
      stype = 0;
    } else {
      const bool th = TerrainSegmentHitCDE(m, SC, dir, sext, &sdist, &sh);
      if (th && (!water || sh.y >= wl)) stype = 1;
      else if (water && WaterPlaneSegmentHitCDE(SC, dir, sext, wl, &sdist, &sh)) stype = 3;
    }
    if (stype >= 0 && D > sdist) {
      D = sdist;
      const bool hadEntity = target != nullptr;
      target = nullptr;
      impactPos = sh;
      if (stype == 3) type = 2;
      else if (stype == 1) type = 1;
      else if (!hadEntity) type = wl > sh.y ? 4 : 3;
    }
  }
  b->length = D;
  {
    // MOHO64_DEBUG_BEAM=<a>-<b>: every beam check in ticks a..b (whose, from where, which way, what it meets)
    static const char* dbg = getenv("MOHO64_DEBUG_BEAM");
    static const uint32_t dbgA = dbg ? static_cast<uint32_t>(atol(dbg)) : 1u;
    static const uint32_t dbgB = dbg && strchr(dbg, '-') ? static_cast<uint32_t>(atol(strchr(dbg, '-') + 1)) : 0u;
    if (sim.tick() >= dbgA && sim.tick() <= dbgB)
      Logf(LogLevel::Info, "beam %u unit %u %s from %.4f %.4f %.4f dir %.5f %.5f %.5f meets %s %d%s at %.3f", sim.tick(),
           owner->id, w->bp->label.c_str(), P.x, P.y, P.z, dir.x, dir.y, dir.z, ImpactTypeName(type),
           target ? static_cast<int>(target->id) : -1, target && target->dead ? " (dead)" : "", D);
  }
  lua_pushstring(L, ImpactTypeName(type));
  Entity* const met = target && target->HasLuaObject() ? target : nullptr;
  PushObject(L, met);
  // a host's lists (render REQUESTS 36; reads only): the impact, and the beam as what its OnImpact's damage is from
  if (sim.collectImpacts) RecordImpact(sim, 1, b, ImpactTypeName(type), met, impactPos, dir);
  const bool credit = sim.collectHits;
  const Entity* const shotBefore = sim.impactShot;
  if (credit) sim.impactShot = b;
  sim.CallMethod(L, b, "OnImpact", 2);
  if (credit) sim.impactShot = shotBefore;
  // 0x673337: the weapon hears whether the beam met its target (ReTargetOnMiss)
  if (!owner->destroyQueued && !owner->combatGone &&
      std::find(owner->weapons.begin(), owner->weapons.end(), w) != owner->weapons.end())
    WeaponImpactEvent(sim, w, ShotEvent(sim, type, w->target, target, false));
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
// MOHO64_DEBUG_BEAM=<a>-<b> also logs the scripts' Enable / Disable calls in those ticks
static void BeamSwitchLog(lua_State* L, const CollisionBeam* b, const char* what) {
  static const char* dbg = getenv("MOHO64_DEBUG_BEAM");
  if (!dbg) return;
  static const uint32_t dbgA = static_cast<uint32_t>(atol(dbg));
  static const uint32_t dbgB = strchr(dbg, '-') ? static_cast<uint32_t>(atol(strchr(dbg, '-') + 1)) : 0u;
  Sim& sim = *S(L);
  if (sim.tick() < dbgA || sim.tick() > dbgB) return;
  Logf(LogLevel::Info, "beam %u unit %u %s %s (was %s, counter %d, interval %d)", sim.tick(),
       b->weapon && b->weapon->unit ? b->weapon->unit->id : 0u, b->weapon && b->weapon->bp ? b->weapon->bp->label.c_str() : "?",
       what, b->enabled ? "on" : "off", b->counter, b->interval);
}
int l_beam_Enable(lua_State* L) {
  CollisionBeam* b = B(L);
  BeamSwitchLog(L, b, "Enable");
  S(L)->CallMethod(L, b, "OnEnable", 0);
  b->enabled = true;
  b->counter = b->interval;
  return 0;
}
int l_beam_Disable(lua_State* L) {
  CollisionBeam* b = B(L);
  BeamSwitchLog(L, b, "Disable");
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

// CollisionBeamEntity::MotionTick 0x6735c0 for one beam
void BeamTick(Sim& sim, CollisionBeam* b) {
  lua_State* L = sim.L();
  if (b->destroyQueued || !b->HasLuaObject()) return;
  UnitWeapon* w = b->weapon;
  if (!w || !w->HasLuaObject() || !w->unit || w->unit->destroyQueued) return;
  Quat q;
  const Vec3 muzzle = BeamMuzzle(sim, b, &q);
  sim.HoldShape(b);
  b->position = muzzle;
  // The beam rides its weapon's muzzle as an attached entity: its transform is Entity::CalculateAttachedTransform
  // 0x67a020's, which ends with the quaternion normalise 0x4edaa0, while the collision check aims along the unit's
  // own bone transform (CreateCollisionBeamHelper 0x6d6c1f, vt+0x48). So bone 1, the end its script reads
  // (CollisionBeamEntity::GetBoneWorldTransform 0x6730b0: the committed position + the forward column x the length
  // +0x280), turns with the normalised one. Ours kept the bone's quaternion as it came: Naval's beam lines ended 1 to
  // 5 units in the last place off the original's.
  b->orientation = NormalizeQuatExe(q);
  if (!b->enabled) return;
  int old = b->counter++;
  if (old >= b->interval) {
    BeamCheck(sim, L, b);
    b->counter = 0;
  }
}

void BeamsTick(Sim& sim) {
  std::vector<CollisionBeam*> beams = sim.beams;
  for (CollisionBeam* b : beams) BeamTick(sim, b);
}

// ---- hits, impacts and health for a host (render REQUESTS 36) ------------------------------------------------
// Kept only while the host asks (Sim::collectHits / collectImpacts / collectHealth). Recording reads the
// simulation's own values and appends to a list: no script call, no Lua value, no random draw. (Engine objects
// stay allocated until the Sim ends, Sim::owned_, so a shell's weapon and launcher pointers are safe to read.)

std::string WeaponBlueprintId(const UnitWeapon* w) {
  // FAF's lua/system/Blueprints.lua (ModBlueprints): w.BlueprintId = bp.BlueprintId .. "-" .. i .. "-" .. label,
  // label = w.Label or "Unlabelled", i its index in bp.Weapon
  if (!w || !w->unit || !w->unit->blueprint) return {};
  return w->unit->blueprint->id + "-" + std::to_string(w->bpIndex) + "-" + (w->label.empty() ? "Unlabelled" : w->label);
}

void RecordImpact(Sim& sim, int kind, const Entity* source, const char* type, const Entity* met, Vec3 at, Vec3 velocity) {
  ImpactEvent ev;
  ev.kind = static_cast<ImpactEvent::Kind>(kind);
  ev.type = type;
  ev.source = source->id;
  ev.army = source->army ? source->army->index : 0;
  if (kind == ImpactEvent::Projectile) {
    const Projectile* p = static_cast<const Projectile*>(source);
    if (p->blueprint) ev.projectile = p->blueprint->id;
    ev.weapon = WeaponBlueprintId(p->weapon);
  } else if (kind == ImpactEvent::Beam) {
    ev.weapon = WeaponBlueprintId(static_cast<const CollisionBeam*>(source)->weapon);
  } else if (source->blueprint) {
    ev.projectile = source->blueprint->id;
  }
  if (met) ev.entity = met->id;
  Put3(ev.position, at);
  Put3(ev.direction, UnitOrZero(velocity));
  if (kind != ImpactEvent::Beam)
    ev.speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
  sim.impactEvents.push_back(std::move(ev));
}

void RecordHealth(Sim& sim, const Entity* e, float before, float beforeMax) {
  HealthEvent ev;
  ev.entity = e->id;
  ev.kind = KindOf(e);
  ev.fraction = e->maxHealth > 0 ? e->health / e->maxHealth : 0.0f;
  ev.before = beforeMax > 0 ? before / beforeMax : 0.0f;
  if (ev.fraction == ev.before) return;  // (the fraction did not change: a new maximum while the health is 0)
  sim.healthEvents.push_back(ev);
}

std::vector<HitEvent> Sim::TakeHits() const {
  std::vector<HitEvent> out;
  out.swap(hitEvents);
  return out;
}
std::vector<ImpactEvent> Sim::TakeImpacts() const {
  std::vector<ImpactEvent> out;
  out.swap(impactEvents);
  return out;
}
std::vector<HealthEvent> Sim::TakeHealth() const {
  std::vector<HealthEvent> out;
  out.swap(healthEvents);
  return out;
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
