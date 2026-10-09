// Projectiles: creation, launch, flight, collision and impact (see combat.h).
//
// What the original does (FA exe, read 2026-10-08):
// - PROJ_Create / Projectile::Projectile 0x69afe0 draws the random parts of the blueprint's
//   Physics (turn rate, max speed, acceleration, lifetime, initial speed, spin), takes the
//   launcher (a projectile's launcher for child projectiles), runs OnPreCreate, sets the layer
//   (Air, or Water below the surface) with OnLayerChange(new, 'None') and runs OnCreate(inWater).
//   A homing projectile without a target destroys itself instead.
// - UnitWeapon:CreateProjectile(muzzle) 0x6d64e0 launches along the muzzle bone as the aim
//   controllers turned it (or the firing solution / straight down), jittered by
//   FiringRandomness (normal, sigma in degrees), at MuzzleVelocity (+ random, reduced at short
//   range) and with the weapon's damage and target.
// - Every tick (MotionTick 0x69bdd0) a projectile accelerates (gravity unless homing; thrust along
//   its nose), turns (VelocityAlign at TurnRate; homing with lead and zig-zag, UpdateTracking
//   0x69c8f0), moves (trapezoid), then sweeps the path (CheckCollision 0x69d1d0): terrain, the
//   water surface (OnEnterWater / OnExitWater), the collision primitives of units, shields and
//   projectiles that accept OnCollisionCheck(projectile), and the detonation heights. A hit
//   puts it at the hit point; OnImpact(type, entity) runs at the start of the next tick
//   (Impact 0x69dec0), which never destroys it (the script does). Running out of lifetime is an
//   'Air' (or 'Underwater') impact.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace combat {
extern uint64_t g_impacts, g_projectiles;
bool IsEnemy(const Army* a, const Army* b);
float WaterLevel(const Sim& sim);
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool HasTarget(Sim& sim, const AiTarget& t);
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
Entity* TargetEntity(Sim& sim, const AiTarget& t);

const char* ImpactTypeName(int t) {
  static const char* const names[] = {"Unknown", "Terrain", "Water", "Air", "Underwater", "Projectile",
                                      "ProjectileUnderwater", "Prop", "Shield", "Unit", "UnitAir", "UnitUnderwater"};
  return t >= 1 && t <= 11 ? names[t] : "Unknown";
}
// ENT_GetImpactType 0x67b240
int ImpactTypeOf(Sim& sim, const Entity* e, Vec3 at) {
  if (at.y < WaterLevel(sim)) {
    if (e->kind == Entity::Kind::Unit) return 11;
    if (e->kind == Entity::Kind::Projectile) return 6;
    if (e->kind == Entity::Kind::Shield) return 8;
    return 4;
  }
  if (e->kind == Entity::Kind::Unit) return static_cast<const Unit*>(e)->layer == "Air" ? 10 : 9;
  if (e->kind == Entity::Kind::Projectile) return 5;
  if (e->kind == Entity::Kind::Prop) return 7;
  if (e->kind == Entity::Kind::Shield) return 8;
  return 3;
}
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

// The sim RNG: one draw in [0, 1); uniform(base, range) = base + U(-range, range).
float U(Sim& sim) { return sim.Random(); }
float Uniform(Sim& sim, float base, float range) { return base - range + U(sim) * (2 * range); }
// Gaussian (polar Box-Muller, second value cached; shared by weapons and projectiles).
float Gauss(Sim& sim) {
  static bool have = false;
  static float cached = 0;
  if (have) {
    have = false;
    return cached;
  }
  float x, y, s;
  do {
    x = 2 * U(sim) - 1;
    y = 2 * U(sim) - 1;
    s = x * x + y * y;
  } while (s >= 1 || s == 0);
  float m = std::sqrt(-2 * std::log(s) / s);
  cached = y * m;
  have = true;
  return x * m;
}

// Projectile blueprint values, cached.
struct ProjBp {
  bool collideSurface = true, collideEntity = true, trackTarget = false, velocityAlign = true, stayUpright = false,
       leadTarget = true, stayUnderwater = false, useGravity = true, destroyOnWater = false,
       realisticOrdinance = false, straightDown = false;
  float detonateAbove = 0, detonateBelow = 0;
  float turnRate = 0, turnRateRange = 0, lifetime = 15, lifetimeRange = 0, initialSpeed = 1, initialSpeedRange = 0;
  float maxSpeed = 0, maxSpeedRange = 0, accel = 0, accelRange = 0;
  float pos[3] = {0, 0, 0}, posRange[3] = {0, 0, 0}, dir[3] = {0, 1, 0}, dirRange[3] = {1.5f, 0, 1.5f};
  float rotVel = 0, rotVelRange = 0, maxZigZag = 0, zigZagFrequency = 0;
  int minBounce = 0, maxBounce = 0;
  float bounceVelDamp = 0.5f;
};
const ProjBp& GetProjBp(Sim& sim, lua_State* L, const BlueprintInfo& bp) {
  static std::map<const BlueprintInfo*, ProjBp> cache;
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  ProjBp d;
  int top = lua_gettop(L);
  sim.blueprints().PushTable(L, bp);
  int ph = lu::Sub(L, lua_gettop(L), "Physics");
  d.collideSurface = lu::Bool(L, ph, "CollideSurface", true);
  d.collideEntity = lu::Bool(L, ph, "CollideEntity", true);
  d.trackTarget = lu::Bool(L, ph, "TrackTarget");
  d.velocityAlign = lu::Bool(L, ph, "VelocityAlign", true);
  d.stayUpright = lu::Bool(L, ph, "StayUpright");
  d.leadTarget = lu::Bool(L, ph, "LeadTarget", true);
  d.stayUnderwater = lu::Bool(L, ph, "StayUnderwater");
  d.useGravity = lu::Bool(L, ph, "UseGravity", true);
  d.destroyOnWater = lu::Bool(L, ph, "DestroyOnWater");
  d.realisticOrdinance = lu::Bool(L, ph, "RealisticOrdinance");
  d.straightDown = lu::Bool(L, ph, "StraightDownOrdinance");
  d.detonateAbove = lu::Num(L, ph, "DetonateAboveHeight", 0);
  d.detonateBelow = lu::Num(L, ph, "DetonateBelowHeight", 0);
  d.turnRate = lu::Num(L, ph, "TurnRate", 0);
  d.turnRateRange = lu::Num(L, ph, "TurnRateRange", 0);
  d.lifetime = lu::Num(L, ph, "Lifetime", 15);
  d.lifetimeRange = lu::Num(L, ph, "LifetimeRange", 0);
  d.initialSpeed = lu::Num(L, ph, "InitialSpeed", 1);
  d.initialSpeedRange = lu::Num(L, ph, "InitialSpeedRange", 0);
  d.maxSpeed = lu::Num(L, ph, "MaxSpeed", 0);
  d.maxSpeedRange = lu::Num(L, ph, "MaxSpeedRange", 0);
  d.accel = lu::Num(L, ph, "Acceleration", 0);
  d.accelRange = lu::Num(L, ph, "AccelerationRange", 0);
  const char* ax[3] = {"X", "Y", "Z"};
  for (int i = 0; i < 3; ++i) {
    d.pos[i] = lu::Num(L, ph, (std::string("Position") + ax[i]).c_str(), 0);
    d.posRange[i] = lu::Num(L, ph, (std::string("Position") + ax[i] + "Range").c_str(), 0);
    d.dir[i] = lu::Num(L, ph, (std::string("Direction") + ax[i]).c_str(), i == 1 ? 1.0f : 0.0f);
    d.dirRange[i] = lu::Num(L, ph, (std::string("Direction") + ax[i] + "Range").c_str(), i == 1 ? 0.0f : 1.5f);
  }
  d.rotVel = lu::Num(L, ph, "RotationalVelocity", 0);
  d.rotVelRange = lu::Num(L, ph, "RotationalVelocityRange", 0);
  d.maxZigZag = lu::Num(L, ph, "MaxZigZag", 0);
  d.zigZagFrequency = lu::Num(L, ph, "ZigZagFrequency", 0);
  d.minBounce = static_cast<int>(lu::Num(L, ph, "MinBounceCount", 0));
  d.maxBounce = static_cast<int>(lu::Num(L, ph, "MaxBounceCount", 0));
  d.bounceVelDamp = lu::Num(L, ph, "BounceVelDamp", 0.5f);
  lua_settop(L, top);
  return cache.emplace(&bp, d).first->second;
}

}  // namespace

// ---- creation -------------------------------------------------------------------------------------

Projectile* Sim::CreateProjectile(lua_State* L, const BlueprintInfo& bp, Army* army, Entity* launcher, Vec3 pos,
                                  Quat q, float damage, float damageRadius, const std::string& damageType,
                                  const AiTarget& target, bool ignoresAlly) {
  lua_checkstack(L, 20);
  int top = lua_gettop(L);
  const ProjBp& b = GetProjBp(*this, L, bp);
  auto owned = std::make_unique<Projectile>();
  Projectile* p = owned.get();
  p->kind = Entity::Kind::Projectile;
  p->blueprint = &bp;
  p->army = army;
  p->id = ReserveId(army, 0x1);
  AttachSkeleton(L, p);
  RevertCollisionShape(L, p);
  p->position = p->prevPos = pos;
  p->orientation = q;
  // spin
  Vec3 sd = Norm({Gauss(*this), Gauss(*this), Gauss(*this)});
  p->angVel = Mul(sd, Uniform(*this, b.rotVel, b.rotVelRange) * kPi / 180);
  p->turnRate = Uniform(*this, b.turnRate, b.turnRateRange);
  p->maxSpeed = Uniform(*this, b.maxSpeed, b.maxSpeedRange);
  p->accel = Uniform(*this, b.accel, b.accelRange);
  p->ballisticAccel = b.useGravity ? Vec3{0, -4.9f, 0} : Vec3{};
  p->collideSurface = b.collideSurface;
  p->collideEntity = b.collideEntity;
  p->trackTarget = b.trackTarget;
  p->velocityAlign = b.velocityAlign;
  p->stayUpright = b.stayUpright;
  p->leadTarget = b.leadTarget;
  p->stayUnderwater = b.stayUnderwater;
  p->destroyOnWater = b.destroyOnWater;
  p->bounceDamp = b.bounceVelDamp;
  p->damage = damage;
  p->damageRadius = damageRadius;
  p->damageType = damageType;
  p->target = target;
  p->ignoresAlly = ignoresAlly;
  p->expireTick = tick_ + static_cast<uint32_t>(std::max(0.0f, std::nearbyint(Uniform(*this, b.lifetime, b.lifetimeRange) * 10)));
  p->launcher = launcher && launcher->kind == Entity::Kind::Projectile ? static_cast<Projectile*>(launcher)->launcher
                                                                        : launcher;
  // the script object
  PushScriptClass(L, bp, "/lua/sim/projectile.lua", "Projectile");
  int cls = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushvalue(L, cls);
  if (lua_pcall(L, 0, 1, cls + 1) != 0 || !lua_istable(L, -1)) {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "projectile script class did not create an object");
    lua_settop(L, top);
    return nullptr;
  }
  BindObject(L, -1, p);
  owned_.push_back(std::move(owned));
  entities_[p->id] = p;
  projectiles.push_back(p);
  ++g_projectiles;
  lua_settop(L, top);
  CallMethod(L, p, "OnPreCreate", 0);
  if (b.maxBounce > b.minBounce) {
    uint32_t r = static_cast<uint32_t>(U(*this) * 4294967296.0);
    p->bounceLimit = b.minBounce + static_cast<int>((static_cast<uint64_t>(r) * static_cast<uint64_t>(b.maxBounce - b.minBounce)) >> 32);
  } else {
    p->bounceLimit = b.minBounce;
  }
  // initial velocity
  if (b.realisticOrdinance && p->launcher) {
    Vec3 v = Mul(EntityVelocity(p->launcher), 10);
    if (HasTarget(*this, target) && p->launcher->kind == Entity::Kind::Unit) {
      Vec3 T = TargetPos(*this, target, false);
      Vec3 dd{T.x - p->launcher->position.x, 0, T.z - p->launcher->position.z};
      v = Mul(Norm(dd), Len(v));
    }
    if (b.rotVelRange > 0) {
      v.x += -b.rotVelRange + U(*this) * 2 * b.rotVelRange;
      v.z += -b.rotVelRange + U(*this) * 2 * b.rotVelRange;
    }
    p->velocity = v;
  } else {
    p->velocity = Mul(Forward(q), Uniform(*this, b.initialSpeed, b.initialSpeedRange));
  }
  if (p->trackTarget && !HasTarget(*this, target)) {
    QueueDestroy(p);
    return p;
  }
  if (Entity* te = TargetEntity(*this, target)) {
    std::string tl = te->kind == Entity::Kind::Unit ? static_cast<Unit*>(te)->layer : "";
    if (tl != "Air" && tl != "Sub") p->homeLastPos = true;
  }
  p->lastTargetPos = TargetPos(*this, target, false);
  float w = WaterLevel(*this);
  bool inWater = pos.y < w;
  p->underwater = inWater;
  p->layer = inWater ? "Water" : "Air";
  lua_pushstring(L, p->layer.c_str());
  lua_pushstring(L, "None");
  CallMethod(L, p, "OnLayerChange", 2);
  if (inWater && p->destroyOnWater) {
    QueueDestroy(p);
    return p;
  }
  lua_pushboolean(L, inWater);
  CallMethod(L, p, "OnCreate", 1);
  return p;
}

// ---- flight ---------------------------------------------------------------------------------------

namespace {

void SetLayer(Sim& sim, Projectile* p, const char* layer) {
  std::string old = p->layer;
  p->layer = layer;
  lua_State* L = sim.L();
  lua_pushstring(L, layer);
  lua_pushstring(L, old.c_str());
  sim.CallMethod(L, p, "OnLayerChange", 2);
}

// UpdateTracking 0x69c8f0
void UpdateTracking(Sim& sim, Projectile* p, Quat& q, Vec3& v) {
  Vec3 pos = p->position;
  if (!HasTarget(sim, p->target)) {
    sim.CallMethod(sim.L(), p, "OnLostTarget", 0);
    p->trackTarget = false;
    if (!p->homeLastPos) return;
  } else {
    Vec3 t = TargetPos(sim, p->target, false);
    if (!IsNaN(t)) p->lastTargetPos = t;
  }
  Vec3 aim = p->lastTargetPos;
  if (IsNaN(aim)) return;
  Entity* te = TargetEntity(sim, p->target);
  if (p->leadTarget && p->maxSpeed > 0 && te && HasTarget(sim, p->target)) {
    Vec3 tv = EntityVelocity(te);
    float k = 1 / (p->maxSpeed * 0.1f);
    float t1 = Len(Sub(pos, aim)) * k;
    Vec3 P1 = Add(aim, Mul(tv, t1));
    float t2 = Len(Sub(pos, P1)) * k;
    aim = Add(aim, Mul(tv, t2));
  }
  float w = WaterLevel(sim);
  if (p->stayUnderwater) aim.y = std::min(aim.y, w - 0.25f);
  Vec3 d = Sub(aim, pos);
  if (p->stayUnderwater && p->underwater && Dot(Forward(q), d) < 0) d.y = 0;
  const ProjBp& b = GetProjBp(sim, sim.L(), *p->blueprint);
  float Z = p->ovMaxZigZag >= 0 ? p->ovMaxZigZag : b.maxZigZag;
  float F = p->ovZigZagFreq >= 0 ? p->ovZigZagFreq : b.zigZagFrequency;
  if (Z > 0 && F > 0) {
    if (sim.tick() >= p->nextZigZag) {
      float ox = -Z + U(sim) * 2 * Z, oy = -Z + U(sim) * 2 * Z, oz = -Z + U(sim) * 2 * Z;
      p->zigOffset = {ox, oy, oz};
      p->nextZigZag = sim.tick() + static_cast<uint32_t>(std::floor(F * 10));
    }
    float s = std::min(Len(Sub(aim, pos)) / Z, 1.0f);
    Vec3 n = Norm(d);
    float y0 = pos.y + n.y * p->maxSpeed;
    Vec3 a{pos.x + n.x * p->maxSpeed + p->zigOffset.x * s, y0 + p->zigOffset.y * s,
           pos.z + n.z * p->maxSpeed + p->zigOffset.z * s};
    const TerrainMap* m = sim.map();
    float g = std::min(y0, (m ? m->TerrainHeight(a.x, a.z) : 0) + 0.5f);
    float ww = std::min(y0, w + 0.5f);
    a.y = std::max(a.y, g);
    if (!p->stayUnderwater) a.y = std::max(a.y, ww);
    d = Sub(a, pos);
  }
  float maxAng = p->turnRate * 0.1f * kPi / 180;
  q = RotateToward(q, d, maxAng);
  if (p->stayUnderwater && p->underwater) {
    Vec3 f = Forward(q);
    if (f.y > 0) {
      float t = (w - pos.y) / f.y;
      if (t < 1) {
        f.y *= t;
        if (Len2(f) >= 1e-6f) q = Orient(f);
      }
    }
  }
  if (p->velocityAlign) {
    Vec3 f = Forward(q);
    v = Mul(f, Dot(f, v));
  }
}

// CheckCollision 0x69d1d0 on the segment p0 -> p1.
void CheckCollision(Sim& sim, Projectile* p, Vec3 p0, Vec3 p1) {
  lua_State* L = sim.L();
  Vec3 seg = Sub(p1, p0);
  float len = Len(seg);
  const TerrainMap* m = sim.map();
  float w = WaterLevel(sim);
  auto record = [&](float dist, Vec3 at, int type, uint32_t entity) {
    p->hitFraction = len > 0 ? dist / len : 0;
    p->hitPos = at;
    p->impactType = type;
    p->hitEntity = entity;
  };
  if (p->collideSurface) {
    int crossing = 0;  // 1 entering, -1 leaving the water
    float waterDist = std::numeric_limits<float>::infinity();
    Vec3 waterPt;
    if (m && m->hasWater) {
      if (!p->underwater && p1.y < w) crossing = 1;
      else if (p->underwater && p1.y > w) crossing = -1;
      if (crossing) {
        float dy = p0.y - p1.y;
        if (std::fabs(dy) > 1e-9f) {
          float t = (p0.y - w) / dy;
          if (t >= 0 && t <= 1) {
            waterDist = t * len;
            waterPt = Add(p0, Mul(seg, t));
          }
        }
        if (p->destroyOnWater) {
          if (waterDist < std::numeric_limits<float>::infinity()) record(waterDist, waterPt, 2, 0);
          else record(0, p0, 2, 0);
        }
      }
    }
    float td;
    Vec3 th;
    bool terrain = TerrainSegmentHit(m, p0, p1, &td, &th);
    if (terrain && (p->hitFraction < 0 || td < p->hitFraction * len)) record(td, th, 1, 0);
    if (crossing && !(terrain && td <= waterDist)) {
      if (crossing == 1) {
        SetLayer(sim, p, "Water");
        p->underwater = true;
        sim.CallMethod(L, p, "OnEnterWater", 0);
      } else {
        SetLayer(sim, p, "Air");
        p->underwater = false;
        sim.CallMethod(L, p, "OnExitWater", 0);
      }
    }
    if (p->destroyQueued) return;
  }
  if (p->collideEntity) {
    Entity* target = TargetEntity(sim, p->target);
    if (target && target->kind == Entity::Kind::Projectile && target->shape.type == ShapeType::None) {
      // a shapeless projectile target: close enough to the segment
      Vec3 tp = target->position;
      float t = len > 0 ? std::clamp(Dot(Sub(tp, p0), seg) / (len * len), 0.0f, 1.0f) : 0;
      Vec3 C = Add(p0, Mul(seg, t));
      float d2 = Len2(Sub(tp, C));
      if (d2 <= Len2(EntityVelocity(target)) + 0.5f) {
        PushObject(L, p);
        if (CallMethodBool(sim, L, target, "OnCollisionCheck", 1) == 1) {
          p->hitFraction = t;
          p->hitPos = p1;
          p->hitEntity = EntityRef(target);
          p->impactType = 5;
          return;
        }
      }
    }
    Unit* launcherUnit = p->launcher && p->launcher->kind == Entity::Kind::Unit ? static_cast<Unit*>(p->launcher) : nullptr;
    if (len >= 0.01f) {
      Vec3 a = Sub(p0, Mul(seg, 0.01f)), b2 = Add(p1, Mul(seg, 0.01f));
      std::vector<Entity*> cands;
      float x0 = std::min(a.x, b2.x) - 10, x1 = std::max(a.x, b2.x) + 10, z0 = std::min(a.z, b2.z) - 10,
            z1 = std::max(a.z, b2.z) + 10;
      std::vector<Unit*> units;
      sim.ForUnitsInRect(x0, z0, x1, z1, [&](Unit* u) { units.push_back(u); });
      std::sort(units.begin(), units.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
      for (Unit* u : units) cands.push_back(u);
      for (Projectile* o : sim.projectiles)
        if (o != p && o->shape.type != ShapeType::None && o->position.x >= x0 && o->position.x <= x1 &&
            o->position.z >= z0 && o->position.z <= z1)
          cands.push_back(o);
      for (ShieldEntity* sh : sim.shields) cands.push_back(sh);
      for (Entity* e : cands) {
        if (e->destroyQueued) continue;
        WorldShape ws;
        Vec3 hit;
        float d;
        if (!GetWorldShape(e, &ws) || !SegmentHit(ws, a, b2, &hit, &d)) continue;
        d = std::max(0.0f, d - 0.01f * len);
        if (e != target) {
          if (e == p || (launcherUnit && e == launcherUnit) || e == p->launcher) continue;
          if (p->ignoresAlly && p->army && e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->layer == "Air" &&
              !IsEnemy(p->army, e->army))
            continue;
        }
        if (p->hitFraction < 0 || d < p->hitFraction * len) {
          PushObject(L, p);
          if (CallMethodBool(sim, L, e, "OnCollisionCheck", 1) == 1) {
            record(d, hit, ImpactTypeOf(sim, e, p0), EntityRef(e));
          }
          if (p->destroyQueued) return;
        }
      }
    } else {
      std::vector<Entity*> cands;
      sim.ForUnitsInRect(p0.x - 1, p0.z - 1, p0.x + 1, p0.z + 1, [&](Unit* u) { cands.push_back(u); });
      std::sort(cands.begin(), cands.end(), [](const Entity* x, const Entity* y) { return x->id < y->id; });
      for (Projectile* o : sim.projectiles)
        if (o != p) cands.push_back(o);
      for (ShieldEntity* sh : sim.shields) cands.push_back(sh);
      for (Entity* e : cands) {
        if (e->destroyQueued || e == p || (p->launcher && e == p->launcher)) continue;
        WorldShape ws;
        if (!GetWorldShape(e, &ws) || !SphereOverlap(ws, p0, 1.0f)) continue;
        if (p->hitFraction >= 0) break;
        PushObject(L, p);
        if (CallMethodBool(sim, L, e, "OnCollisionCheck", 1) == 1) {
          p->hitFraction = 0;
          p->hitPos = p0;
          p->hitEntity = EntityRef(e);
          p->impactType = ImpactTypeOf(sim, e, p0);
          return;
        }
      }
    }
  }
  // detonation heights
  const ProjBp& b = GetProjBp(sim, L, *p->blueprint);
  float terrainY = m ? m->TerrainHeight(p1.x, p1.z) : 0;
  float ground = std::max(terrainY, w);
  float h = p1.y - ground, dy = p1.y - p0.y;
  float above = p->ovDetAbove >= 0 ? p->ovDetAbove : b.detonateAbove;
  float below = p->ovDetBelow >= 0 ? p->ovDetBelow : b.detonateBelow;
  float f;
  if (below > 0 && h < below && dy < 0) f = (below + terrainY - p0.y) / dy;
  else if (above > 0 && h > above && dy > 0) f = (above + terrainY - p0.y) / dy;
  else return;
  f = std::max(std::min(f, 1.0f), 0.1f);
  p->hitFraction = f;
  p->hitPos = Add(p0, Mul(seg, f));
  p->impactType = 3;
}

// Impact 0x69dec0
void Impact(Sim& sim, Projectile* p) {
  lua_State* L = sim.L();
  Entity* e = p->hitEntity ? sim.FindEntity(p->hitEntity) : nullptr;
  int type = p->impactType;
  p->hitPos = {};
  p->hitEntity = 0;
  p->impactType = 0;
  p->hitFraction = -1;
  ++g_impacts;
  lua_pushstring(L, ImpactTypeName(type));
  PushObject(L, e && !e->destroyQueued && e->HasLuaObject() ? e : nullptr);
  sim.CallMethod(L, p, "OnImpact", 2);
}

// MotionTick 0x69bdd0
void MotionTick(Sim& sim, Projectile* p) {
  if (p->hitFraction >= 0) {
    p->prevPos = p->position;
    Impact(sim, p);
    return;
  }
  if (p->bouncePending) {
    p->velocity = p->bouncedVel;
    p->bouncePending = false;
  }
  Vec3 v = p->velocity, v0 = v;
  Quat q = p->orientation;
  Quat qStart = q;
  float maxAng = p->turnRate * 0.1f * kPi / 180;
  if (!p->trackTarget) {
    v = Add(v, Mul(p->ballisticAccel, 0.1f));
    v = Add(v, Mul(Forward(q), p->accel * 0.1f));
    if (p->velocityAlign && maxAng > 0) q = RotateToward(q, v, maxAng >= kPi ? 10.0f : maxAng);
  } else {
    UpdateTracking(sim, p, q, v);
    if (p->destroyQueued) return;
    v = Add(v, Mul(Forward(q), p->accel * 0.1f));
  }
  if (p->maxSpeed != 0) {
    float l = Len(v);
    if (l > p->maxSpeed) v = Mul(v, p->maxSpeed / l);
  }
  if (p->stayUpright) q = Orient(Forward(q));
  Vec3 p0 = p->position;
  Vec3 p1 = Add(p0, Mul(Add(v, v0), 0.05f));
  float av = Len(p->angVel);
  if (av != 0) {
    if (p->velocityAlign || p->trackTarget) p->angVel = {0, 0, av};
    else if (p->stayUpright) p->angVel = {0, av, 0};
    q = QNorm(QMul(q, FromRotVec(Mul(p->angVel, 0.1f))));
  }
  float w = WaterLevel(sim);
  if (p->stayUnderwater && p->underwater) p1.y = std::min(p1.y, w - 0.01f);
  p->velocity = v;
  CheckCollision(sim, p, p0, p1);
  if (p->destroyQueued) return;
  if (sim.tick() >= p->expireTick && p->hitFraction < 0) {
    p->hitFraction = 1;
    p->hitPos = p1;
    p->impactType = p->underwater ? 4 : 3;
  }
  p->prevPos = p0;
  p->velScale = 1;
  if (p->hitFraction >= 0) {
    float t = p->hitFraction;
    if (p->impactType == 1 && p->bounces++ < p->bounceLimit) {
      const TerrainMap* m = sim.map();
      Vec3 hp = p->hitPos;
      Vec3 n{0, 1, 0};
      if (m) {
        float hx = m->TerrainHeight(hp.x + 0.5f, hp.z) - m->TerrainHeight(hp.x - 0.5f, hp.z);
        float hz = m->TerrainHeight(hp.x, hp.z + 0.5f) - m->TerrainHeight(hp.x, hp.z - 0.5f);
        n = Norm({-hx, 1, -hz});
      }
      Vec3 vd = Mul(v, p->bounceDamp);
      p->bouncedVel = Sub(vd, Mul(n, 2 * Dot(n, vd)));
      p->hitPos = Add(p->hitPos, Mul(Norm(Mul(v, -1)), 0.05f));
      p->hitFraction = -1;
      p->bouncePending = true;
    }
    p->velScale = t * 0.95f > 0.001f ? 1 / (t * 0.95f) : 1000;
    float f = p->hitFraction >= 0 ? std::clamp(p->hitFraction, 0.0f, 1.0f) : 0.0f;
    p->orientation = Nlerp(qStart, q, f);
    p->position = p->hitPos;
    if (p->bouncePending) p->hitPos = {};
  } else {
    p->orientation = q;
    p->position = p1;
  }
  if (p->position.x != p0.x || p->position.y != p0.y || p->position.z != p0.z) p->lastMoveTick = sim.tick();
}

}  // namespace

void ProjectilesTick(Sim& sim) {
  std::vector<Projectile*> list = sim.projectiles;  // (impacts create and destroy projectiles)
  for (Projectile* p : list) {
    if (p->destroyQueued || !p->HasLuaObject()) continue;
    MotionTick(sim, p);
  }
}

// ---- launch bindings ------------------------------------------------------------------------------

namespace {

const BlueprintInfo* CheckProjBp(lua_State* L, int idx) {
  const BlueprintInfo* bp = nullptr;
  if (lua_isstring(L, idx)) bp = S(L)->blueprints().Find(lua_tostring(L, idx));
  else if (lua_istable(L, idx)) {
    lua_pushstring(L, "BlueprintId");
    lua_gettable(L, idx);
    if (lua_isstring(L, -1)) bp = S(L)->blueprints().Find(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  return bp;
}

int PushProj(lua_State* L, Projectile* p) {
  if (p && p->HasLuaObject()) PushObject(L, p);
  else lua_pushnil(L);
  return 1;
}

int ResolveBoneOf(lua_State* L, Entity* e, int idx) {
  if (lua_isnoneornil(L, idx)) return -1;
  if (lua_type(L, idx) == LUA_TNUMBER) {
    int b = static_cast<int>(lua_tonumber(L, idx));
    return b;
  }
  const char* n = lua_tostring(L, idx);
  return e->skeleton && n ? e->skeleton->Find(n) : -1;
}

// Entity:CreateProjectile(bp, ox, oy, oz, dx, dy, dz) 0x68a110
int l_CreateProjectile(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Sim& sim = *S(L);
  const BlueprintInfo* bp = CheckProjBp(L, 2);
  if (!bp) return luaL_error(L, "CreateProjectile: Invalid blueprint %s", lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
  const ProjBp& b = GetProjBp(sim, L, *bp);
  Vec3 o, d;
  if (lua_isnoneornil(L, 3)) {
    o = {Uniform(sim, b.pos[0], b.posRange[0]), Uniform(sim, b.pos[1], b.posRange[1]), Uniform(sim, b.pos[2], b.posRange[2])};
  } else {
    o = {static_cast<float>(luaL_optnumber(L, 3, 0)), static_cast<float>(luaL_optnumber(L, 4, 0)),
         static_cast<float>(luaL_optnumber(L, 5, 0))};
  }
  if (lua_isnoneornil(L, 6)) {
    d = {Uniform(sim, b.dir[0], b.dirRange[0]), Uniform(sim, b.dir[1], b.dirRange[1]), Uniform(sim, b.dir[2], b.dirRange[2])};
  } else {
    d = {static_cast<float>(luaL_optnumber(L, 6, 0)), static_cast<float>(luaL_optnumber(L, 7, 0)),
         static_cast<float>(luaL_optnumber(L, 8, 0))};
  }
  Vec3 pos = Add(e->position, o);
  return PushProj(L, sim.CreateProjectile(L, *bp, e->army, e, pos, Orient(d), 0, 0, "Normal", AiTarget{}, true));
}

// Entity:CreateProjectileAtBone(bp, bone) 0x68a6f0
int l_CreateProjectileAtBone(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Sim& sim = *S(L);
  const BlueprintInfo* bp = CheckProjBp(L, 2);
  if (!bp) return luaL_error(L, "CreateProjectileAtBone: Invalid blueprint %s", lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
  const ProjBp& b = GetProjBp(sim, L, *bp);
  int bone = ResolveBoneOf(L, e, 3);
  Vec3 pos;
  Quat q;
  BoneWorld(e, bone, &pos, &q);
  Vec3 d{Uniform(sim, b.dir[0], b.dirRange[0]), Uniform(sim, b.dir[1], b.dirRange[1]), Uniform(sim, b.dir[2], b.dirRange[2])};
  if (d.x == 0 && d.y == 0 && d.z == 0) d = Forward(q);
  float speed = Uniform(sim, b.initialSpeed, b.initialSpeedRange);
  Projectile* p = sim.CreateProjectile(L, *bp, e->army, e, pos, q, 0, 0, "Normal", AiTarget{}, true);
  if (p && !p->destroyQueued) p->velocity = Mul(d, speed);
  return PushProj(L, p);
}

// UnitWeapon:CreateProjectile(muzzle) 0x6d64e0
int l_weapon_CreateProjectile(lua_State* L) {
  UnitWeapon* w = CheckObject<UnitWeapon>(L, 1);
  Sim& sim = *S(L);
  if (!w->bp) InitUnitWeapon(L, w);
  Unit* u = w->unit;
  if (!w->projBp) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Logf(LogLevel::Debug, "moho64: %s:%s:CreateProjectile: no projectile blueprint (instahit not implemented)",
           u->blueprint ? u->blueprint->id.c_str() : "?", w->label.c_str());
    }
    lua_pushnil(L);
    return 1;
  }
  int bone = ResolveBoneOf(L, u, 2);
  Vec3 pos;
  Quat q;
  BoneWorld(u, bone, &pos, &q);
  const ProjBp& pb = GetProjBp(sim, L, *w->projBp);
  if (pb.straightDown) q = Orient({0, -1, 0});
  else if (w->bp->useFiringSolutionInsteadOfAimBone && !IsNaN(w->solution)) q = Orient(Norm(w->solution));
  float R = w->firingRandomness;
  if (R > 0) {
    float g1 = Gauss(sim), g2 = Gauss(sim);
    float pitch = g1 * R * kPi / 180, yaw = g2 * R * kPi / 180;
    Quat dq = QMul(AxisAngle({0, 1, 0}, yaw), AxisAngle({1, 0, 0}, pitch));
    q = QNorm(QMul(q, dq));
  }
  float damage = w->ovDamage >= 0 ? w->ovDamage : w->bp->damage;
  float radius = w->ovDamageRadius >= 0 ? w->ovDamageRadius : w->bp->damageRadius;
  const std::string& type = w->ovDamageType.empty() ? w->bp->damageType : w->ovDamageType;
  Projectile* p = sim.CreateProjectile(L, *w->projBp, u->army, u, pos, q, damage, radius, type, w->target,
                                       w->bp->ignoresAlly);
  if (!p || p->destroyQueued) return PushProj(L, p);
  if (w->bp->muzzleVelocity != 0) {
    Vec3 tp = TargetPos(sim, w->target, false);
    float dist = IsNaN(tp) ? 0 : Len(Sub(pos, tp));
    float mv = w->bp->muzzleVelocity + (w->bp->muzzleVelocityRandom != 0 ? Gauss(sim) * w->bp->muzzleVelocityRandom : 0);
    if (!IsNaN(tp) && dist < w->bp->muzzleVelocityReduceDistance)
      mv *= std::sqrt(dist / w->bp->muzzleVelocityReduceDistance);
    p->velocity = Mul(Norm(p->velocity), mv);
  }
  if (w->bp->projectileLifetime > 0)
    p->expireTick = sim.tick() + static_cast<uint32_t>(std::nearbyint(w->bp->projectileLifetime * 10));
  if (w->bp->projectileLifetimeUsesMultiplier > 0 && w->bp->muzzleVelocity != 0) {
    float maxR = w->ovMaxRadius >= 0 ? w->ovMaxRadius : w->bp->maxRadius;
    p->expireTick = sim.tick() + static_cast<uint32_t>(std::nearbyint(
                                     maxR / w->bp->muzzleVelocity * w->bp->projectileLifetimeUsesMultiplier * 10));
  }
  p->weapon = w;
  return PushProj(L, p);
}

// ---- Projectile methods ---------------------------------------------------------------------------

Projectile* P(lua_State* L) { return CheckObject<Projectile>(L, 1); }
int Self(lua_State* L) {
  lua_settop(L, 1);
  return 1;
}

int l_GetLauncher(lua_State* L) {
  Projectile* p = P(L);
  if (p->launcher && !p->launcher->destroyQueued && p->launcher->HasLuaObject()) PushObject(L, p->launcher);
  else lua_pushnil(L);
  return 1;
}
int l_CreateChildProjectile(lua_State* L) {
  Projectile* p = P(L);
  Sim& sim = *S(L);
  const BlueprintInfo* bp = CheckProjBp(L, 2);
  if (!bp) {
    Logf(LogLevel::Warning, "Blueprint for projectile %s not found!, returning a nil object",
         lua_tostring(L, 2) ? lua_tostring(L, 2) : "?");
    lua_pushnil(L);
    return 1;
  }
  return PushProj(L, sim.CreateProjectile(L, *bp, p->army, p, p->position, p->orientation, p->damage,
                                          p->damageRadius, p->damageType, p->target, true));
}
int l_GetVelocity(lua_State* L) {
  Vec3 v = EntityVelocity(P(L));
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}
int l_GetCurrentSpeed(lua_State* L) {
  lua_pushnumber(L, Len(EntityVelocity(P(L))));
  return 1;
}
int l_GetCurrentTargetPosition(lua_State* L) {
  Projectile* p = P(L);
  Vec3 t = p->target.type == 0 ? Vec3{} : TargetPos(*S(L), p->target, false);
  if (IsNaN(t)) t = p->lastTargetPos;
  if (IsNaN(t)) t = {};
  PushVec(L, t);
  return 1;
}
int l_GetCurrentTargetPositionXYZ(lua_State* L) {
  Projectile* p = P(L);
  Vec3 t = p->target.type == 0 ? Vec3{} : TargetPos(*S(L), p->target, false);
  if (IsNaN(t)) t = p->lastTargetPos;
  if (IsNaN(t)) t = {};
  lua_pushnumber(L, t.x);
  lua_pushnumber(L, t.y);
  lua_pushnumber(L, t.z);
  return 3;
}
int l_GetTrackingTarget(lua_State* L) {
  Projectile* p = P(L);
  Entity* e = p->trackTarget && HasTarget(*S(L), p->target) ? TargetEntity(*S(L), p->target) : nullptr;
  if (e && e->HasLuaObject()) PushObject(L, e);
  else lua_pushnil(L);
  return 1;
}
int l_SetAcceleration(lua_State* L) {
  P(L)->accel = static_cast<float>(luaL_checknumber(L, 2));
  return Self(L);
}
int l_SetBallisticAcceleration(lua_State* L) {
  Projectile* p = P(L);
  int n = lua_gettop(L);
  if (n == 1) p->ballisticAccel = {0, -4.9f, 0};
  else if (n == 2) p->ballisticAccel = {0, static_cast<float>(luaL_checknumber(L, 2)), 0};
  else if (n == 4)
    p->ballisticAccel = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                         static_cast<float>(luaL_checknumber(L, 4))};
  else
    return luaL_error(L, "Wrong number of arguments to Projectile:SetAccelerationVector(), expected 1, 2, or 4 but got %d", n);
  return Self(L);
}
int l_SetCollideEntity(lua_State* L) {
  P(L)->collideEntity = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_SetCollideSurface(lua_State* L) {
  P(L)->collideSurface = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_SetCollision(lua_State* L) {
  Projectile* p = P(L);
  p->collideSurface = p->collideEntity = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_SetDamage(lua_State* L) {  // (sic: the radius argument also writes the amount)
  Projectile* p = P(L);
  if (!lua_isnoneornil(L, 2)) p->damage = static_cast<float>(luaL_checknumber(L, 2));
  if (!lua_isnoneornil(L, 3)) p->damage = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
int l_SetDestroyOnWater(lua_State* L) {
  P(L)->destroyOnWater = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetLifetime(lua_State* L) {
  Projectile* p = P(L);
  p->expireTick = S(L)->tick() + static_cast<uint32_t>(std::max(0.0, static_cast<double>(std::nearbyint(luaL_checknumber(L, 2) * 10))));
  return Self(L);
}
int l_SetLocalAngularVelocity(lua_State* L) {
  P(L)->angVel = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                  static_cast<float>(luaL_checknumber(L, 4))};
  return Self(L);
}
int l_SetMaxSpeed(lua_State* L) {
  P(L)->maxSpeed = static_cast<float>(luaL_checknumber(L, 2));
  return Self(L);
}
int l_SetNewTarget(lua_State* L) {
  Projectile* p = P(L);
  Entity* e = CheckObject<Entity>(L, 2);
  p->target = AiTarget{};
  p->target.type = 1;
  p->target.entityId = EntityRef(e);
  return 0;
}
int l_SetNewTargetGround(lua_State* L) {
  Projectile* p = P(L);
  p->target = AiTarget{};
  p->target.type = 2;
  p->target.pos = ReadVec(L, 2);
  return 0;
}
int l_SetNewTargetGroundXYZ(lua_State* L) {
  Projectile* p = P(L);
  p->target = AiTarget{};
  p->target.type = 2;
  p->target.pos = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                   static_cast<float>(luaL_checknumber(L, 4))};
  return 0;
}
int l_SetScaleVelocity(lua_State* L) {
  Projectile* p = P(L);
  if (lua_gettop(L) >= 4)
    p->scaleVel = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                   static_cast<float>(luaL_checknumber(L, 4))};
  else {
    float s = static_cast<float>(luaL_checknumber(L, 2));
    p->scaleVel = {s, s, s};
  }
  return Self(L);
}
int l_SetStayUpright(lua_State* L) {
  P(L)->stayUpright = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetTurnRate(lua_State* L) {
  P(L)->turnRate = static_cast<float>(luaL_checknumber(L, 2));
  return Self(L);
}
int l_SetVelocity(lua_State* L) {
  Projectile* p = P(L);
  int n = lua_gettop(L);
  if (n == 2) {
    p->velocity = Mul(Norm(p->velocity), static_cast<float>(luaL_checknumber(L, 2)));
  } else if (n == 4) {
    p->velocity = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
                   static_cast<float>(luaL_checknumber(L, 4))};
  } else {
    return luaL_error(L, "Wrong number of arguments to Projectile:SetVelocity(x,y,z), expected 2 or 4 but got %d", n);
  }
  return Self(L);
}
int l_SetVelocityAlign(lua_State* L) {
  P(L)->velocityAlign = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_SetVelocityRandomUpVector(lua_State* L) {
  Projectile* p = P(L);
  Sim& sim = *S(L);
  Vec3 v{U(sim), 0.05f + 0.95f * U(sim), U(sim)};
  const ProjBp& b = GetProjBp(sim, L, *p->blueprint);
  p->velocity = b.maxSpeed > 0 ? Mul(Norm(v), b.maxSpeed) : Vec3{};
  return 0;
}
int l_StayUnderwater(lua_State* L) {
  P(L)->stayUnderwater = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_TrackTarget(lua_State* L) {
  P(L)->trackTarget = lua_toboolean(L, 2) != 0;
  return Self(L);
}
int l_ChangeDetonateAboveHeight(lua_State* L) {
  P(L)->ovDetAbove = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_ChangeDetonateBelowHeight(lua_State* L) {
  P(L)->ovDetBelow = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_ChangeMaxZigZag(lua_State* L) {
  P(L)->ovMaxZigZag = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_ChangeZigZagFrequency(lua_State* L) {
  P(L)->ovZigZagFreq = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_GetMaxZigZag(lua_State* L) {
  lua_pushnumber(L, P(L)->ovMaxZigZag);
  return 1;
}
int l_GetZigZagFrequency(lua_State* L) {
  lua_pushnumber(L, P(L)->ovZigZagFreq);
  return 1;
}

}  // namespace

void RegisterProjectileBindings(lua_State* L) {
  SetMethod(L, "Entity", "CreateProjectile", l_CreateProjectile);
  SetMethod(L, "Entity", "CreateProjectileAtBone", l_CreateProjectileAtBone);
  SetMethod(L, "UnitWeapon", "CreateProjectile", l_weapon_CreateProjectile);
  struct M {
    const char* n;
    lua_CFunction f;
  };
  const M ms[] = {
      {"GetLauncher", l_GetLauncher},
      {"CreateChildProjectile", l_CreateChildProjectile},
      {"GetVelocity", l_GetVelocity},
      {"GetCurrentSpeed", l_GetCurrentSpeed},
      {"GetCurrentTargetPosition", l_GetCurrentTargetPosition},
      {"GetCurrentTargetPositionXYZ", l_GetCurrentTargetPositionXYZ},
      {"GetTrackingTarget", l_GetTrackingTarget},
      {"SetAcceleration", l_SetAcceleration},
      {"SetBallisticAcceleration", l_SetBallisticAcceleration},
      {"SetCollideEntity", l_SetCollideEntity},
      {"SetCollideSurface", l_SetCollideSurface},
      {"SetCollision", l_SetCollision},
      {"SetDamage", l_SetDamage},
      {"SetDestroyOnWater", l_SetDestroyOnWater},
      {"SetLifetime", l_SetLifetime},
      {"SetLocalAngularVelocity", l_SetLocalAngularVelocity},
      {"SetMaxSpeed", l_SetMaxSpeed},
      {"SetNewTarget", l_SetNewTarget},
      {"SetNewTargetGround", l_SetNewTargetGround},
      {"SetNewTargetGroundXYZ", l_SetNewTargetGroundXYZ},
      {"SetScaleVelocity", l_SetScaleVelocity},
      {"SetStayUpright", l_SetStayUpright},
      {"SetTurnRate", l_SetTurnRate},
      {"SetVelocity", l_SetVelocity},
      {"SetVelocityAlign", l_SetVelocityAlign},
      {"SetVelocityRandomUpVector", l_SetVelocityRandomUpVector},
      {"StayUnderwater", l_StayUnderwater},
      {"TrackTarget", l_TrackTarget},
      {"ChangeDetonateAboveHeight", l_ChangeDetonateAboveHeight},
      {"ChangeDetonateBelowHeight", l_ChangeDetonateBelowHeight},
      {"ChangeMaxZigZag", l_ChangeMaxZigZag},
      {"ChangeZigZagFrequency", l_ChangeZigZagFrequency},
      {"GetMaxZigZag", l_GetMaxZigZag},
      {"GetZigZagFrequency", l_GetZigZagFrequency},
  };
  for (const M& m : ms) SetMethod(L, "Projectile", m.n, m.f);
  SetGlobal(L, "IsProjectile", [](lua_State* L) -> int {
    ScriptObject* o = GetObject(L, 1);
    lua_pushboolean(L, o && (o->typeBits & kTypeProjectile));
    return 1;
  });
}

}  // namespace moho
