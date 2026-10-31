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
#include "core/dmath.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/air.h"
#include "sim/blueprints.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/entity_grid.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

namespace combat {
bool IsEnemy(const Army* a, const Army* b);
float WaterLevel(const Sim& sim);
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool HasTarget(Sim& sim, const AiTarget& t);
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
Vec3 TargetPosBeatStart(Sim& sim, const AiTarget& t);
Entity* TargetEntity(Sim& sim, const AiTarget& t);
Vec3 BlipPointUnitGone(Sim& sim, const Entity* b);

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

// The sim stream (engine-ref sim_random.md 4.1): U01, the blueprint uniform base + U(-range, range), Gauss.
float U(Sim& sim) { return sim.U01(); }
float Uniform(Sim& sim, float base, float range) { return sim.BpUniform(base, range); }
float Gauss(Sim& sim) { return static_cast<float>(sim.Gauss()); }

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
  float uniformScale = 1, meshScaleRange = 0, meshScaleVel = 0, meshScaleVelRange = 0;  // Display (bp+0x1b4..)
};
const ProjBp& GetProjBp(Sim& sim, lua_State* L, const BlueprintInfo& bp) {
  static const SimCache<std::map<const BlueprintInfo*, ProjBp>> cacheSlot;
  auto& cache = cacheSlot.For(sim);
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
  int di = lu::Sub(L, top + 1, "Display");
  d.uniformScale = lu::Num(L, di, "UniformScale", 1);
  d.meshScaleRange = lu::Num(L, di, "MeshScaleRange", 0);
  d.meshScaleVel = lu::Num(L, di, "MeshScaleVelocity", 0);
  d.meshScaleVelRange = lu::Num(L, di, "MeshScaleVelocityRange", 0);
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
  {  // MOHO64_DEBUG_PROJ: every shell made, with the beat and where
    static const bool dbgMade = getenv("MOHO64_DEBUG_PROJ") != nullptr;
    if (dbgMade)
      Logf(LogLevel::Info, "pdbg %u %u made %s at %.9g %.9g %.9g by %u", tick_, p->id, bp.id.c_str(), pos.x, pos.y, pos.z,
           launcher ? launcher->id : 0u);
  }
  // the script object: Entity::Entity 0x677c90 resolves the script class (0x677360, importing its module) and
  // creates the object first, so a module's import-time Random calls come before the ctor's draws below
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
  lua_settop(L, top);
  AttachSkeleton(L, p);
  RevertCollisionShape(L, p);
  p->position = p->prevPos = pos;
  p->orientation = q;
  // spin
  float sg1 = static_cast<float>(Gauss()), sg2 = static_cast<float>(Gauss()), sg3 = static_cast<float>(Gauss());
  // GetAngularVelocity 0x51c8c0: the axis by Normalize 0x452af0 (0x51c8e5), the rate in degrees times 0.017453292
  // in one multiply (0x51c950), not times pi over 180
  Vec3 sd{sg1, sg2, sg3};
  vm::NormalizeExe(sd);
  p->angVel = Mul(sd, Uniform(*this, b.rotVel, b.rotVelRange) * 0.017453292f);
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
  owned_.push_back(std::move(owned));
  RegisterEntity(p);
  projectiles.push_back(p);
  ++projectilesMade;
  lua_settop(L, top);
  CallMethod(L, p, "OnPreCreate", 0);
  // bounce limit: always one draw (0x69b540), Min + (u * (Max - Min)) >> 32
  p->bounceLimit = b.minBounce + static_cast<int>(IntRange(static_cast<uint32_t>(b.maxBounce - b.minBounce)));
  // initial velocity
  if (b.realisticOrdinance && p->launcher) {
    Vec3 v = Mul(EntityVelocity(p->launcher), 10);
    if (HasTarget(*this, target) && p->launcher->kind == Entity::Kind::Unit) {
      Vec3 T = TargetPos(*this, target, false);
      // a mobile target is led by the launcher's Air.PredictAheadForBombDrop seconds (0x62cd40)
      const Unit* lu0 = static_cast<const Unit*>(p->launcher);
      const float ahead = lu0->motion.air && lu0->motion.air->bp ? lu0->motion.air->bp->predictAheadForBombDrop : 0;
      if (ahead > 0 && target.mobile)
        if (Entity* te = TargetEntity(*this, target); te && te->kind == Entity::Kind::Unit)
          T = PredictAhead(*this, static_cast<Unit*>(te), ahead, true);
      Vec3 dd{T.x - p->launcher->position.x, 0, T.z - p->launcher->position.z};
      // SetLength 0x5b1c90 of dd to |v| (0x69b71c..0x69b76a: one factor |v| / |dd|), then v += (dd - v) per part
      // (0x69b76f..0x69b7c8), not quite dd itself
      const float L = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
      if (const float l2 = (dd.x * dd.x + dd.y * dd.y) + dd.z * dd.z; l2 > 0.0f) {
        const float k = L / std::sqrt(l2);
        dd = {dd.x * k, k * dd.y, k * dd.z};
      }
      v = {v.x + (dd.x - v.x), (dd.y - v.y) + v.y, (dd.z - v.z) + v.z};
    }
    if (b.rotVelRange > 0) {
      v.x += static_cast<float>(FRand(-b.rotVelRange, b.rotVelRange));
      v.z += static_cast<float>(FRand(-b.rotVelRange, b.rotVelRange));
    }
    p->velocity = v;
  } else {
    p->velocity = Mul(Forward(q), Uniform(*this, b.initialSpeed, b.initialSpeedRange));
  }
  // step 6: mesh scale and scale velocity (0x69b952, 0x69ba1d: one draw each)
  {
    float sc = Uniform(*this, b.uniformScale, b.meshScaleRange);
    float sv = Uniform(*this, b.meshScaleVel, b.meshScaleVelRange);
    if (b.meshScaleRange != 0 && b.uniformScale != 0) p->scale[0] = p->scale[1] = p->scale[2] = sc / b.uniformScale;
    if (b.meshScaleVelRange != 0 || b.meshScaleVel != 0) p->scaleVel = {sv, sv, sv};
    p->drawSize = {sc, sc, sc};
  }
  if (p->trackTarget && !HasTarget(*this, target)) {
    QueueDestroy(p);
    return p;
  }
  // The layer test runs only for a shell that tracks from birth (0x69bb52: +0x2aa clear jumps to 0x69bbe9, past it);
  // +0x318 is written nowhere else (the ctor's clear 0x69b359 and this set 0x69bbb1 are its only writers), so a shell
  // whose tracking Lua turns on later never flies on to the last point once its target is gone. Seen: MP1 9446, army
  // 6's torpedo bomber 0x600048 (uaa0204) dropped aantorpedo03 (TrackTarget = false; ATorpedoCluster tracks from
  // water entry) at 0x5000a4, which died at 9446: the original's three torpedoes make no zigzag draws after their
  // OnLostTarget; ours went on homing (9 FRand).
  if (Entity* te = p->trackTarget ? TargetEntity(*this, target) : nullptr) {
    // 0x69bb6e..0x69bbb1: the layer is the target ENTITY's own (+0x118), whatever it is: a shell in the air is in
    // layer Air as an aircraft is (the ctor 0x69bca6), so a missile fired at a missile does not fly on to the last
    // place it saw when its target is gone. Seen (Naval, 616-623): an Aeon cruiser's RedirectMissile shot
    // (aifmissileserpentine03, made at 563 with an enemy missile as its target) was turned onto a flare by the
    // flare's sphere; the flare ended at 622, and at 623 ours went on to its zig-zag (three draws) where the
    // original's UpdateTracking returned after OnLostTarget (0x69c94f), the first random draw apart (105559).
    std::string tl = te->kind == Entity::Kind::Unit         ? static_cast<Unit*>(te)->layer
                     : te->kind == Entity::Kind::Projectile ? static_cast<Projectile*>(te)->layer
                                                            : "";
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

// The recon blip a target was taken through, while it still exists.
const Entity* BlipOfTarget(Sim& sim, const AiTarget& t) {
  if (t.type != 1 || !t.viaBlip || !t.blipId) return nullptr;
  const Entity* b = sim.FindEntity(t.blipId);
  return b && !b->destroyQueued && b->kind == Entity::Kind::Blip ? b : nullptr;
}

// UpdateTracking 0x69c8f0
void UpdateTracking(Sim& sim, Projectile* p, Quat& q, Vec3& v) {
  Vec3 pos = p->position;
  // CAiTarget::HasTarget 0x5e2a10: a unit target is gone when the unit is dead, a recon blip target (a shell of a
  // weapon that was ORDERED onto the unit) only when the blip is, and the blip outlives the dying unit: such a
  // missile keeps homing on the falling wreck (seen: 41 ticks of flight where ours gave up 7 after the death)
  // Once the unit is destroyed the blip stays where it was until the armies let go of it (their recon passes): the
  // missile then steers at the blip's own position (seen: two beats at the wreck's origin, then the target is gone).
  const bool viaBlip = p->target.type == 1 && p->target.viaBlip;
  // (a unit whose Destroy is queued is still there until the end of the beat)
  const Entity* blipUnit = viaBlip && p->target.entityId ? sim.FindEntity(p->target.entityId) : nullptr;
  const Entity* blip = viaBlip && !blipUnit ? BlipOfTarget(sim, p->target) : nullptr;
  // A jammer's FAKE blip is the CAiTarget's entity itself (combat::TargetPos): HasTarget 0x5e2a10 asks the fake, so
  // the target is gone once the armies have let go of it and it was destroyed (the jammer's death drops its fakes
  // one army's recon at a time), however long the dead jammer's wreck stays. Seen: MP1 8288, army 6's torpedo
  // 0x10600027 on a fake of army 5's frigate 0x50007c (dead at 8278, its fakes gone after army 7's recon at 8287):
  // the original's shell has lost its target at 8288 and makes no zigzag draws at 8289; ours went on homing on the
  // fake's last point because the wreck was there.
  const bool has = p->target.type == 1 && p->target.fakeId ? HasTarget(sim, p->target)
                   : viaBlip                                ? (blipUnit != nullptr || blip != nullptr)
                                                            : HasTarget(sim, p->target);
  if (!has) {
    static const bool dbgProj = getenv("MOHO64_DEBUG_PROJ") != nullptr;  // (read once)
    if (dbgProj) Logf(LogLevel::Info, "pdbg %u %u OnLostTarget", sim.tick(), p->id);
    sim.CallMethod(sim.L(), p, "OnLostTarget", 0);
    p->trackTarget = false;
    if (!p->homeLastPos) return;
  } else if (blip) {
    // (0x69c90a..0x69c919: GetTargetPosGun with the centre flag clear, so the blip's GetTargetPoint 0x5bf5f0)
    p->lastTargetPos = BlipPointUnitGone(sim, blip);
  } else {
    // A target WITHOUT an aim bone is taken from the entity's own transform, and an entity that already took its
    // step in this beat still stands, for the original, where the beat began (its transform is committed in step
    // 13). (With an aim bone the point comes from the unit's pose, which did move with the unit's own step: the
    // Experimental replay needs that, the Transports replay the other.)
    Vec3 t = TargetPosBeatStart(sim, p->target);
    // ... once the unit HAS had its turn. The entity stage (task stage sim+0x930) runs one thread per entity in the
    // order they were made, and a unit attached to a parent whose turn comes later is linked at the run list's end
    // for good (Entity::TaskTick 0x679c40): a wreck on FAF's sinker shell has its turn behind every shell made
    // before the sinker, so for those its pose (Unit::GetTargetPoint 0x6ab340 -> 0x6aa5c0) is still last beat's.
    // (Ours moves every unit before the shells.) Seen: a torpedo homing on a sinking sub's wreck through bone 0 aims
    // at where the bone stood a beat before (Subs replay, beats 560-595: the shell's place and turn to the last digit).
    if (p->target.type == 1 && p->target.aimBone != -1 && !p->target.fakeId)
      if (Entity* e = TargetEntity(sim, p->target); e && e->kind == Entity::Kind::Unit) {
        Unit* u = static_cast<Unit*>(e);
        const uint32_t turn = u->orderHandle ? u->orderHandle : u->handle;  // its place in the run list
        if (turn > p->handle && u->poseSet && u->lastPosTick == sim.tick()) {
          const Vec3 curPos = u->poseBasePos;
          const Quat curOri = u->poseBaseOri;
          u->poseBasePos = u->prevPoseBasePos;
          u->poseBaseOri = u->prevPoseBaseOri;
          t = TargetPos(sim, p->target, false);
          u->poseBasePos = curPos;
          u->poseBaseOri = curOri;
        }
      }
    if (!IsNaN(t)) p->lastTargetPos = t;
  }
  Vec3 aim = p->lastTargetPos;
  if (IsNaN(aim)) return;
  Entity* te = TargetEntity(sim, p->target);
  if (p->leadTarget && p->maxSpeed > 0 && te && has) {
    // (a shell that has taken its step already: its velocity of the beat's start goes with its place of the
    // beat's start, both are what is committed)
    Vec3 tv = te->kind == Entity::Kind::Projectile && te->shapeHeld ? static_cast<const Projectile*>(te)->heldVel
                                                                     : EntityVelocity(te);
    // A target held through a recon blip IS the blip for the original's CAiTarget (+0x2f0), and the lead asks it
    // (0x69c9fd, vtable +0x3c): the blip's own GetVelocity (ReconBlip slot 15 = Entity::GetVelocity 0x678dc0), its
    // committed place minus its previous one = the unit's move of the beat before, not the unit's motion velocity
    // (Unit::GetVelocity 0x6a9c90), which a wreck hanging on FAF's sinker keeps at 0. Seen: the torpedo above leads
    // the sinking wreck by that move from its second beat of sinking on.
    if (viaBlip)
      if (const Entity* bl = BlipOfTarget(sim, p->target))
        tv = Sub(bl->position, static_cast<const ReconBlip*>(bl)->prevPos);
    // The same for a blip that IS the target's entity: a shell Lua turned onto a blip (Projectile:SetNewTarget with
    // the weapon's current target, FAF's RetargetThread). Seen (probe mp1_9616): MP1, army 4's torpedo 0x10400004,
    // retargeted at 9586 onto army 5's blip 0x30500112 of the destroyer 0x5000df: the original leads it by the
    // blip's move, ours led by nothing (EntityVelocity has no blip) and from 9608 on stood 1.2 off in x and 1.3 in y;
    // it struck the destroyer's hull at 9615 where the original's struck at 9618.
    if (te->kind == Entity::Kind::Blip)
      tv = Sub(te->position, static_cast<const ReconBlip*>(te)->prevPos);
    float k = 1 / (p->maxSpeed * 0.1f);
    // the squares summed x, z, y (0x69ca38..0x69ca52, 0x69cab9..0x69cad2)
    const Vec3 d1 = Sub(pos, aim);
    float t1 = std::sqrt((d1.x * d1.x + d1.z * d1.z) + d1.y * d1.y) * k;
    Vec3 P1 = Add(aim, Mul(tv, t1));
    const Vec3 d2 = Sub(pos, P1);
    float t2 = std::sqrt((d2.x * d2.x + d2.z * d2.z) + d2.y * d2.y) * k;
    // MOHO64_DEBUG_TRACK=<projectile id>: the lead's inputs each beat
    static const long dbgTrack = getenv("MOHO64_DEBUG_TRACK") ? atol(getenv("MOHO64_DEBUG_TRACK")) : -1;
    if (dbgTrack >= 0 && static_cast<long>(p->id) == dbgTrack)
      Logf(LogLevel::Info,
           "track %u %u pos %.9g %.9g %.9g target %u%s point %.9g %.9g %.9g vel %.9g %.9g %.9g t1 %.9g t2 %.9g", sim.tick(),
           p->id, pos.x, pos.y, pos.z, te->id, te->shapeHeld ? " (moved this beat)" : "", aim.x, aim.y, aim.z, tv.x, tv.y,
           tv.z, t1, t2);
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
      float ox = static_cast<float>(sim.FRand(-Z, Z));
      float oy = static_cast<float>(sim.FRand(-Z, Z));
      float oz = static_cast<float>(sim.FRand(-Z, Z));
      p->zigOffset = {ox, oy, oz};
      static const bool dbgProj = getenv("MOHO64_DEBUG_PROJ") != nullptr;  // (read once)
      if (dbgProj) {
        const Entity* e0 = p->target.entityId ? sim.FindEntity(p->target.entityId) : nullptr;
        const Unit* tu = e0 && e0->kind == Entity::Kind::Unit ? static_cast<const Unit*>(e0) : nullptr;
        Logf(LogLevel::Info, "pdbg %u %u zigzag (launcher %u, target unit %d%s, dead %d destroy queued %d)", sim.tick(),
             p->id, p->launcher ? p->launcher->id : 0u, tu ? static_cast<int>(tu->id) : -1,
             p->target.viaBlip ? " via its blip" : "", tu ? static_cast<int>(tu->dead) : -1,
             tu ? static_cast<int>(tu->destroyQueued) : -1);
      }
      p->nextZigZag = sim.tick() + static_cast<uint32_t>(std::floor(F * 10));
    }
    const Vec3 e = Sub(aim, pos);
    float s = std::min(std::sqrt((e.x * e.x + e.z * e.z) + e.y * e.y) / Z, 1.0f);  // (0x69cd8c..0x69cda6)
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
  q = TurnToward(q, d, p->turnRate * 0.00174532924f);  // 0x69cf67: TurnRate x [0xe4f768]
  if (p->stayUnderwater && p->underwater) {
    Vec3 f = Forward(q);
    if (f.y > 0) {
      float t = (w - pos.y) / f.y;
      if (t < 1) {
        // (0x69d050..0x69d102: the squared length summed z, y, x; below 1e-6 the shell looks along its quaternion's
        // up axis, ((y*x - z*w)*2, 1 - (z*z + x*x)*2, (y*z + w*x)*2); otherwise (NaN too) along f normalised by
        // 0x452af0; both through COORDS_Orient 0x50b480. Ours kept q below 1e-6 and for NaN, and took vm::Orient.)
        f.y *= t;
        const float l2 = (f.z * f.z + f.y * f.y) + f.x * f.x;
        if (9.99999997e-07f > l2) {
          q = LookQuat((q.y * q.x - q.z * q.w) * 2.0f, 1.0f - (q.z * q.z + q.x * q.x) * 2.0f,
                       (q.y * q.z + q.w * q.x) * 2.0f);
        } else {
          vm::NormalizeExe(f);
          q = LookQuat(f.x, f.y, f.z);
        }
      }
    }
  }
  if (p->velocityAlign) {
    // 0x69a2a0: the velocity's part along the new forward, divided by the forward's own squared length (the
    // quaternion is not kept normalised)
    const Vec3 f = Forward(q);
    const float f2 = (f.x * f.x + f.y * f.y) + f.z * f.z;
    if (f2 > 0.0f) {
      const float k = ((v.y * f.y + v.z * f.z) + v.x * f.x) / f2;
      v = {f.x * k, f.y * k, f.z * k};
    } else {
      v = {};
    }
  }
}

// The step's segment as CheckCollision builds it (0x69d247 -> 0x4fe130): centre p0 + h, unit direction h / |h|
// (0 for a zero step), half length |h|, with h = (p1 - p0) x 0.5 and |h|^2 summed y, z, x.
struct StepSegment {
  Vec3 o, d;
  float e;
};
StepSegment StepSegmentExe(Vec3 p0, Vec3 p1) {
  const Vec3 h{(p1.x - p0.x) * 0.5f, (p1.y - p0.y) * 0.5f, (p1.z - p0.z) * 0.5f};
  const float e = std::sqrt((h.y * h.y + h.z * h.z) + h.x * h.x);
  Vec3 d{};
  if (e > 0) {
    const float inv = 1.0f / e;
    d = {inv * h.x, h.y * inv, h.z * inv};
  }
  return {{p0.x + h.x, p0.y + h.y, p0.z + h.z}, d, e};
}

// Wm3 DistVector3Segment3<float>::GetSquared 0xa484f0 as compiled (x87 at 24 bits: every operation rounds to
// float): the point's parameter along the direction from the centre, clamped to [-e, e] (an end when at or past
// it), the closest point (0xa39230 hands it out) and the squared distance to it.
float DistSqPointSegmentExe(const StepSegment& s, Vec3 pt, Vec3* closest) {
  const Vec3 df{pt.x - s.o.x, pt.y - s.o.y, pt.z - s.o.z};
  const float t = (s.d.y * df.y + s.d.x * df.x) + s.d.z * df.z;
  Vec3 c;
  if (t <= -s.e) c = {s.o.x - s.d.x * s.e, s.o.y - s.d.y * s.e, s.o.z - s.d.z * s.e};
  else if (s.e <= t) c = {s.o.x + s.d.x * s.e, s.o.y + s.d.y * s.e, s.o.z + s.d.z * s.e};
  else c = {s.o.x + s.d.x * t, s.o.y + s.d.y * t, s.o.z + s.d.z * t};
  *closest = c;
  const Vec3 r{c.x - pt.x, c.y - pt.y, c.z - pt.z};
  return (r.x * r.x + r.y * r.y) + r.z * r.z;
}

// CheckCollision 0x69d1d0 on the segment p0 -> p1.
void CheckCollision(Sim& sim, Projectile* p, Vec3 p0, Vec3 p1) {
  lua_State* L = sim.L();
  Vec3 seg = Sub(p1, p0);
  // The step's length is twice the half length of its segment (0x69d247 -> 0x4fe130: |h| with h = (p1 - p0) x 0.5,
  // summed y, z, x; 0x69d26a..0x69d27b: x 2.0), and every fraction is a distance divided by it with no test for a
  // zero step (0x69d466, 0x69d5e8, 0x69dc68). (Ours: |p1 - p0| summed x, y, z, and 0 for a zero step.)
  const float len = StepSegmentExe(p0, p1).e * 2.0f;
  const TerrainMap* m = sim.map();
  float w = WaterLevel(sim);
  auto record = [&](float dist, Vec3 at, int type, uint32_t entity) {
    p->hitFraction = dist / len;
    p->hitPos = at;
    p->impactType = type;
    p->hitEntity = entity;
  };
  if (p->collideSurface) {
    int crossing = 0;  // 1 entering, -1 leaving the water
    // The crossing (0x69d2a8..0x69d4a8): the line from p0 along the step's unit direction (0x44f7e0 of p1 - p0:
    // ((x*x + y*y) + z*z), above 0 times 1 / len, else zero; 0x69d36b) meets the plane (0, 1, 0, level) by
    // 0x577540 (den = (d.y*1 + d.z*0) + 0*d.x, none at den == 0; t = (level - ((p0.x*0 + 1*p0.y) + 0*p0.z)) / den
    // within -inf..+inf, so of either sign; the point p0 + d t, none when a part is NaN, 0x5657f0). waterT is that
    // t (world units along the step), 0 when there is no point (0x69d2a8). A shell destroyed on water takes the
    // point and the fraction t / (2 x the segment's half length) (0x69d455..0x69d466), or p0 and 0. The layer
    // changes unless the terrain was recorded nearer than waterT (0x69db46..0x69db53). (Ours took the fraction of
    // the step where y reached the level, only within [0, 1], and the terrain hit whether recorded or not.)
    float waterT = 0.0f;
    if (m && m->hasWater) {
      if (!p->underwater && p1.y < w) crossing = 1;
      else if (p->underwater && p1.y > w) crossing = -1;
      if (crossing) {
        const Vec3 dd{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const float dl = std::sqrt((dd.x * dd.x + dd.y * dd.y) + dd.z * dd.z);
        Vec3 dir{0.0f, 0.0f, 0.0f};
        if (dl > 0.0f) {
          const float inv = 1.0f / dl;
          dir = {dd.x * inv, inv * dd.y, inv * dd.z};
        }
        const float den = (dir.y * 1.0f + dir.z * 0.0f) + 0.0f * dir.x;
        bool met = false;
        Vec3 wp{};
        if (den != 0.0f || std::isnan(den)) {
          const float t = (w - ((p0.x * 0.0f + 1.0f * p0.y) + 0.0f * p0.z)) / den;
          if (!std::isnan(t)) {
            wp = {p0.x + t * dir.x, dir.y * t + p0.y, dir.z * t + p0.z};
            met = !(std::isnan(wp.x) || std::isnan(wp.y) || std::isnan(wp.z));
            if (met) waterT = t;
          }
        }
        if (p->destroyOnWater) {
          if (met) {
            p->hitPos = wp;
            p->hitFraction = waterT / (StepSegmentExe(p0, p1).e * 2.0f);
          } else {
            p->hitPos = p0;
            p->hitFraction = 0.0f;
          }
          p->impactType = 2;
          p->hitEntity = 0;
        }
      }
    }
    float td;
    Vec3 th;
    bool terrain = TerrainSegmentHit(m, p0, p1, &td, &th);
    bool terrainRecorded = false;
    if (terrain && (p->hitFraction < 0 || td < p->hitFraction * len)) {
      record(td, th, 1, 0);
      terrainRecorded = true;
    }
    if (crossing && (!terrainRecorded || td > waterT)) {
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
      // A shapeless projectile target: close enough to the segment. Both the target's place and its velocity are
      // the COMMITTED ones (0x69d671: the place +0xac; 0x69d6a7: vt+0x3c = Entity::GetVelocity 0x678dc0, +0xac
      // minus the previous +0xc8, x the scale +0xd4), which AdvanceCoords 0x678f10 changes only at the beat's end:
      // a target shell that has taken its step earlier in this pass is still met where, and as fast as, the beat
      // began. Seen (Naval, beat 511): a torpedo homing on a UEF anti-torpedo (timantitorpedo01, turned onto it
      // by its depth charge, which stepped first) ends its step 0.544 from the anti-torpedo's place of the beat's
      // start (0.296 <= 0.59 = its 0.09 + 0.5: a hit) and 0.795 from its new one (0.633: none); the original's
      // OnImpact ran in 512, ours in 513 (the first random draw apart, 80014). The torpedo's shell lines now agree
      // to the last digit, hit and turn included.
      const bool held = target->shapeHeld;  // (a shell's place before its own step: ProjectilesTick)
      const Vec3 tp = held ? target->shapePos : target->position;
      const Vec3 tv = held ? static_cast<const Projectile*>(target)->heldVel : EntityVelocity(target);
      // 0x69d67f..0x69d692: the squared distance from the step's segment (Wm3's form, see above);
      // 0x69d6a9..0x69d6e1: hit when |v|^2 + 0.5 >= it (an unordered compare is no hit)
      const StepSegment s = StepSegmentExe(p0, p1);
      Vec3 C;
      const float d2 = DistSqPointSegmentExe(s, tp, &C);
      if (Len2(tv) + 0.5f >= d2) {
        PushObject(L, p);
        if (CallMethodBool(sim, L, target, "OnCollisionCheck", 1) == 1) {
          // 0x69d70b..0x69d7f1: the fraction is sqrt(|C - p0|^2 / |p1 - p0|^2) (each summed x, y, z), at most 1
          // (and 1 when the quotient is not a number: a zero step), at least 0
          const Vec3 a{C.x - p0.x, C.y - p0.y, C.z - p0.z}, b{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
          float t = std::sqrt(((a.x * a.x + a.y * a.y) + a.z * a.z) / ((b.x * b.x + b.y * b.y) + b.z * b.z));
          if (!(1.0f > t)) t = 1.0f;
          if (0.0f > t) t = 0.0f;
          p->hitFraction = t;
          p->hitPos = p1;
          p->hitEntity = EntityRef(target);
          p->impactType = 5;
          return;
        }
      }
      // That was the whole check (0x69d803 leaves the function, hit or not): a shell flying at a shapeless shell
      // meets nothing else, neither another entity nor a detonation height. So a torpedo a decoy has turned onto
      // its anti-torpedo shell is not turned again by the next decoy whose sphere it crosses.
      return;
    }
    Unit* launcherUnit = p->launcher && p->launcher->kind == Entity::Kind::Unit ? static_cast<Unit*>(p->launcher) : nullptr;
    if (len >= 0.01f) {
      // 0x69da62..0x69dae7: the step lengthened by k at both ends, k = [0xe4f950] = 0.0100000007 (the float above
      // 0.01; the length test before it, 0x69d831, is [0xdfeb80] = 0.00999999978). The entity line test meets the
      // segment a -> b, so its hit point follows a's and b's last bits. Seen (probe cp_hitpoint, Experimental2 356):
      // the original's a.x 732.661194 and b.x 733.511047 for the shell 0x10000024 (ours with 0.01f: 732.661255,
      // 733.510986), and its hit on the unit 0x100004 at x 733.422546 (ours 733.422485).
      const float k = 0.0100000007f;
      Vec3 a = Sub(p0, Mul(seg, k)), b2 = Add(p1, Mul(seg, k));
      std::vector<Entity*> cands;
      if (sim.entityGrid().ready()) {
        // 0x69db05: ONE query of the entity grid for everything with a collision primitive, GetEntityCollisionsInLine
        // 0x7229c0: the cells the (lengthened) segment passes, from its start to its end; in each cell first the
        // shells, shields and script entities, then the units, each list with what was registered last in front.
        // The candidates are asked in that order, and one is only asked while no nearer hit stands. So a script
        // entity is asked before a unit of the same cell: FAF's missile flare (a sphere riding on the flare shell,
        // which answers no to OnCollisionCheck and turns the missile with SetNewTarget) still turns, and counts, a
        // missile that strikes a ship in the very step it enters the sphere. And of several decoys crossed in one
        // step the last one asked keeps the shell.
        sim.entityGrid().GatherLine(a.x, a.z, b2.x, b2.z, &cands);
      } else {
        // (no map, no grid: by kind)
        float x0 = std::min(a.x, b2.x) - 32, x1 = std::max(a.x, b2.x) + 32, z0 = std::min(a.z, b2.z) - 32,
              z1 = std::max(a.z, b2.z) + 32;
        std::vector<Unit*> units;
        sim.ForUnitsInRect(x0, z0, x1, z1, [&](Unit* u) { units.push_back(u); });
        std::sort(units.begin(), units.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
        for (Unit* u : units) cands.push_back(u);
        for (Projectile* o : sim.projectiles)
          if (o != p && o->shape.type != ShapeType::None && o->position.x >= x0 && o->position.x <= x1 &&
              o->position.z >= z0 && o->position.z <= z1)
            cands.push_back(o);
        for (ShieldEntity* sh : sim.shields) cands.push_back(sh);
        for (auto it = sim.scriptEntities.rbegin(); it != sim.scriptEntities.rend(); ++it)
          if ((*it)->kind == Entity::Kind::Entity && (*it)->shape.type != ShapeType::None) cands.push_back(*it);
      }
      for (Entity* e : cands) {
        // (an entity whose Destroy is queued is still in the world until the beat's end and is met like any
        // other: an anti-torpedo runs into the sphere of a torpedo that struck its ship earlier in this pass)
        WorldShape ws;
        Vec3 hit;
        float d;
        // (the record's distance is from the extended start: 0x69dc49 compares and divides it as it is)
        if (!GetWorldShape(e, &ws) || !SegmentHit(ws, a, b2, &hit, &d)) continue;
        if (e != target) {
          if (e == p || (launcherUnit && e == launcherUnit) || e == p->launcher) continue;
          // 0x69dbfd..0x69dc2d: IgnoresAlly skips whatever is in layer Air (Entity +0x118 == 0x10, read off the
          // entity itself, not its unit part) and not an enemy (GetArmyIndex 0x678b90, IArmy::IsEnemy 0x5d5540).
          // A shell in the air is in layer Air too (the ctor 0x69bca6 and the water crossings 0x69db6d set it), so
          // two allied shells never meet. Seen (Naval, beat 570): two Aeon flares (aimantimissile01, a sphere of
          // radius 1 each, SetCollisionShape in AIMFlareProjectile.OnCreate) of one ship rise and fall in the same
          // column; ours had each strike the other where it left the other's sphere, the original let them fly
          // on to their lifetimes' end (its "wi 574" / "wi 584" Air). (The beam's filter, GetClosestCollision
          // 0x6d6a00, asks the unit part and stays for units: BeamCheck.)
          const bool inAir = (e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->layer == "Air") ||
                             (e->kind == Entity::Kind::Projectile && static_cast<Projectile*>(e)->layer == "Air");
          if (p->ignoresAlly && p->army && inAir && !IsEnemy(p->army, e->army)) continue;
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
      // A step shorter than [0xdfeb80] = 0.00999999978 (0x69d831..0x69d9ef): no line test, but the sphere of radius
      // [0xdfec20] = 1 around the committed place p0 (+0xac) goes to COGrid::ForAllEntitiesIterator 0x721fb0 with the
      // mask 0xd00 (units, shells, the rest of the third list; no props): the entity-grid cells under the sphere's
      // box (AABoxToRect 0x4fcbe0), in grid order, each entity with a collision primitive (+0x178) whose sphere test
      // (vt+0x20) passes (R is 1, below the 3.0 of the cube shortcut). The candidates are asked in that order: the
      // launcher's unit (vt+0x10 of the candidate, +8, against the weak link +0x278) and the shell itself are
      // passed over, and while no hit stands (+0x2a4 < 0) OnCollisionCheck (func_OnCollisionCheck 0x69f6b0); the
      // first yes is the hit at fraction 0 and p0 (0x69d984..0x69d9d0) and the check ends there. Nothing skips a
      // destroy-queued entity. Seen (MP1 10888 / 10889): army 1's torpedoes 0x1010000a and 0x10100053
      // (aantorpedo03, slowed to 0.05 by OnEnterWater's SetVelocity(0)) lie 0.6 and 0.9 below and off the end of
      // army 2's ship 0x2000e5, whose centre is 2.3 and 3.0 away; the original took the ship (AdjustHealth -150 at
      // 10889 and 10890, probe mp1_10889). Ours asked only the units whose CENTRE lay within 1 of p0, by id, and so
      // never the ship.
      std::vector<Entity*> cands;
      if (sim.entityGrid().ready()) {
        sim.entityGrid().GatherBox(p0.x - 1.0f, p0.z - 1.0f, p0.x + 1.0f, p0.z + 1.0f, 1 | 4 | 8, &cands);
      } else {
        // (no map, no grid: by kind)
        sim.ForUnitsInRect(p0.x - 1, p0.z - 1, p0.x + 1, p0.z + 1, [&](Unit* u) { cands.push_back(u); });
        std::sort(cands.begin(), cands.end(), [](const Entity* x, const Entity* y) { return x->id < y->id; });
        for (Projectile* o : sim.projectiles)
          if (o != p) cands.push_back(o);
        for (ShieldEntity* sh : sim.shields) cands.push_back(sh);
        for (Entity* se : sim.scriptEntities)
          if (se->kind == Entity::Kind::Entity && se->shape.type != ShapeType::None) cands.push_back(se);
      }
      for (Entity* e : cands) {
        WorldShape ws;
        if (!GetWorldShape(e, &ws) || !SphereOverlap(ws, p0, 1.0f)) continue;
        // 0x69d905..0x69d930: the candidate's unit part against the launcher; a candidate that is no unit gives
        // null, so with no launcher at all every such candidate is passed over too
        const Entity* asUnit = e->kind == Entity::Kind::Unit ? e : nullptr;
        if (asUnit == p->launcher || e == p) continue;
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
  ++sim.impactsSeen;
  lua_pushstring(L, ImpactTypeName(type));
  // 0x69defa..0x69df24: the entity the hit recorded (the weak link +0x328) goes to OnImpact as long as the link
  // holds, i.e. until the destroy queue has taken the entity (beat step 15); a Destroy queued earlier in this beat
  // does not drop it. (Ours passed nil for an entity whose Destroy was queued. Seen: Naval 247, the Phalanx shell
  // 0x10100232 hits the missile 0x1000002f, whose own Water impact earlier in the same pass destroyed it: the
  // original's OnImpact gets "Projectile" and that missile, ours got nil; 12 Naval watch lines.)
  Entity* const met = e && e->HasLuaObject() ? e : nullptr;
  PushObject(L, met);
  // a host's lists (render REQUESTS 36; reads only): the impact where the shell stands (its hit point), and the
  // shell as what the damage its OnImpact deals is from
  if (sim.collectImpacts) RecordImpact(sim, 0, p, ImpactTypeName(type), met, p->position, p->velocity);
  const bool credit = sim.collectHits;
  const Entity* const shotBefore = sim.impactShot;
  if (credit) sim.impactShot = p;
  sim.CallMethod(L, p, "OnImpact", 2);
  if (credit) sim.impactShot = shotBefore;
  // 0x69e084: after the script, the weapon that fired it hears how the shot went (its acquire task listens:
  // CAiAttackerImpl::TransmitProjectileImpactEvent 0x5d7800 hooked it up when the shell was made)
  if (UnitWeapon* w = p->weapon) {
    const Unit* lu = p->launcher && p->launcher->kind == Entity::Kind::Unit ? static_cast<const Unit*>(p->launcher) : nullptr;
    if (lu && !lu->destroyQueued && !lu->combatGone &&
        std::find(lu->weapons.begin(), lu->weapons.end(), w) != lu->weapons.end())
      WeaponImpactEvent(sim, w, ShotEvent(sim, type, p->target, e, true));
  }
}

// MotionTick 0x69bdd0
void MotionTick(Sim& sim, Projectile* p) {
  // 0x69be40: the drawn size grows by the scale velocity, in drawn-size units per second ([0xe4f714] = 0.1)
  p->drawSize = {p->drawSize.x + p->scaleVel.x * 0.1f, p->drawSize.y + p->scaleVel.y * 0.1f,
                 p->drawSize.z + p->scaleVel.z * 0.1f};
  if (p->hitFraction >= 0) {
    // no movement this tick, so the entity is not on the moved list and its previous coords stay those of
    // the hit tick's AdvanceCoords: OnImpact's GetVelocity is the last step to the hit point (projectile.md 3.1,
    // 3.5 GetVelocity). (Was prevPos = position: speed 0, and the Salvation's fragment math divided by it.)
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
  if (!p->trackTarget) {
    v = Add(v, Mul(p->ballisticAccel, 0.1f));
    v = Add(v, Mul(Forward(q), p->accel * 0.1f));
    // 0x69c115..0x69c13b: a VelocityAlign shell turns whatever its rate. At TurnRate 0 the clamp (0x4eb740) makes the
    // turn (0, 0, 0, 1), except when the forward and the velocity lie exactly along each other (f x h = 0, not
    // above sin(0)^2): the half-vector rotation stays as built and its w = f.h scales q by an ulp.
    if (p->velocityAlign) q = TurnToward(q, v, p->turnRate * 0.00174532924f);  // 0x69c11e
  } else {
    UpdateTracking(sim, p, q, v);
    if (p->destroyQueued) return;
    v = Add(v, Mul(Forward(q), p->accel * 0.1f));
  }
  if (p->maxSpeed != 0) {  // LimitLengthTo 0x5d1e70 (0x69c183): scaled unless L^2 >= |v|^2 (a NaN scales too)
    const float l2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (!(p->maxSpeed * p->maxSpeed >= l2)) {
      const float k = p->maxSpeed / std::sqrt(l2);
      v = {v.x * k, v.y * k, k * v.z};
    }
  }
  if (p->stayUpright) {  // 0x69c1b7: the look rotation 0x50b480 of the forward
    const Vec3 f = Forward(q);
    q = LookQuat(f.x, f.y, f.z);
  }
  Vec3 p0 = p->position;
  Vec3 p1 = Add(p0, Mul(Add(v, v0), 0.05f));
  float av = Len(p->angVel);
  if (av != 0) {
    if (p->velocityAlign || p->trackTarget) p->angVel = {0, 0, av};
    else if (p->stayUpright) p->angVel = {0, av, 0};
    // 0x69c376: q x r, r = the quaternion of the rotation vector (0x697360: the vector normalised by 0x452af0, half
    // its length, fsin / fcos), summed in the order below. The product is NOT normalised.
    Vec3 n = Mul(p->angVel, 0.1f);
    const float h = static_cast<float>(NormalizeExe(n)) * 0.5f;
    const float s = dmath::Sin(h), c = dmath::Cos(h);
    const Quat r{n.x * s, n.y * s, s * n.z, c};
    const Quat b = q;
    q = {((r.x * b.w + r.z * b.y) + b.x * r.w) - r.y * b.z, ((r.y * b.w + r.x * b.z) + b.y * r.w) - r.z * b.x,
         ((r.z * b.w + r.y * b.x) + b.z * r.w) - r.x * b.y, ((r.w * b.w - r.x * b.x) - r.y * b.y) - r.z * b.z};
  }
  float w = WaterLevel(sim);
  if (p->stayUnderwater && p->underwater) p1.y = std::min(p1.y, w - 0.01f);
  p->velocity = v;
  p->warpedInStep = false;
  CheckCollision(sim, p, p0, p1);
  if (p->destroyQueued) return;
  if (sim.tick() >= p->expireTick && p->hitFraction < 0) {
    p->hitFraction = 1;
    p->hitPos = p1;
    p->impactType = p->underwater ? 4 : 3;
  }
  p->prevPosBefore = p->prevPos;
  p->prevPosTick = sim.tick();
  p->prevPos = p0;
  p->velScale = 1;
  {
    // MOHO64_DEBUG_PROJ=<a>-<b>: every projectile's step in ticks a..b (and what it hit)
    static const char* dbg = getenv("MOHO64_DEBUG_PROJ");
    static const uint32_t dbgA = dbg ? static_cast<uint32_t>(atol(dbg)) : 1u;
    static const uint32_t dbgB = dbg && strchr(dbg, '-') ? static_cast<uint32_t>(atol(strchr(dbg, '-') + 1)) : 0u;
    if (sim.tick() >= dbgA && sim.tick() <= dbgB)
      Logf(LogLevel::Info, "pdbg %u %u %s p0 %.9g %.9g %.9g p1 %.9g %.9g %.9g v %.9g %.9g %.9g hit %.9g type %d terrain %.9g %.9g",
           sim.tick(), p->id, p->blueprint ? p->blueprint->id.c_str() : "?", p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, v.x,
           v.y, v.z, p->hitFraction, p->impactType, sim.map() ? sim.map()->TerrainHeight(p0.x, p0.z) : 0.0f,
           sim.map() ? sim.map()->TerrainHeight(p1.x, p1.z) : 0.0f);
  }
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
      // the reflection's dot summed y, z, x, then doubled (0x69c692..0x69c6b5)
      p->bouncedVel = Sub(vd, Mul(n, ((n.y * vd.y + n.z * vd.z) + n.x * vd.x) * 2.0f));
      p->hitPos = Add(p->hitPos, Mul(Norm(Mul(v, -1)), 0.05f));
      p->hitFraction = -1;
      p->bouncePending = true;
    }
    // (seen in the original, per shell and tick: velocity = step x 1 / fraction, the fraction being the hit's
    // distance from the 1% extended start over the step's length; 1000 at fraction 0)
    p->velScale = t > 0.001f ? 1 / t : 1000;
    float f = p->hitFraction >= 0 ? std::clamp(p->hitFraction, 0.0f, 1.0f) : 0.0f;
    // (0x69c7ec: func_QuatLERP 0x4eba80 of the committed and the pending orientation; seen: a torpedo's turn at its
    // hit on the sea floor one ulp off with the textbook form)
    p->orientation = NlerpExe(qStart, q, f);
    p->position = p->hitPos;
    if (p->bouncePending) p->hitPos = {};
  } else if (p->warpedInStep) {
    // The step's transform is the PENDING one, set before the sweep (0x69c4fd: SetPendingTransform 0x678e90 of p1 and
    // q, then CheckCollision 0x69d1d0), and with no hit nothing writes it again (0x69c5f7..0x69c606: +0x2a4 < 0 jumps
    // to the end). A script that warps the shell from inside the sweep (Entity::Warp 0x679210: pending, then
    // AdvanceCoords twice) so leaves it where it warped it. Seen: MP1 13120, FAF's ShieldCollider under army 6's
    // crashing scout 0x6000ee meets a bubble shield; shield.lua's OnCollisionCheck calls its OnImpact('Shield'),
    // which warps it to the plane and hangs the plane on it; the original's collider (and so the plane at 13121)
    // stands at the warped place, ours put it back on p1. (Its previous transform is the warped one too: velocity 0.)
    p->warpedInStep = false;
    p->prevPos = p->position;
  } else {
    p->orientation = q;
    p->position = p1;
  }
  if (p->position.x != p0.x || p->position.y != p0.y || p->position.z != p0.z) p->lastMoveTick = sim.tick();
}

}  // namespace

void ProjectilesTick(Sim& sim) {
  // By index over the live list: an impact's script may create projectiles (fragmentation shells), and the entity
  // stage reaches a task that was added during its pass in that same pass: a child shell makes its first step in
  // the beat it is made. (Destroyed ones leave the list at the end of the beat only.)
  // That holds for every shell made in the pass (CTaskStage 0x409ac0: a thread made while the stage runs is
  // linked at its new end), also for the debris a shot-down missile drops from OnKilled; what looked like such a
  // piece waiting a beat was where it starts: at its parent's place of the beat's START (l_CreateChildProjectile).
  // A shell whose Destroy is QUEUED still takes its step: neither Entity::TaskTick 0x679c40 nor
  // Projectile::MotionTick 0x69bdd0 looks at the flag, and the entity is only taken away when the beat's destroy
  // queue runs. Seen: a missile that a script thread killed in the Lua stage (FAF's flare, 10 beats after it
  // turned the missile) still made its zig-zag draws in that beat's entity stage.
  // (The projectiles made before a unit have taken their step between the units already: ProjectileStep.)
  for (size_t i = 0; i < sim.projectiles.size(); ++i) ProjectileStep(sim, sim.projectiles[i]);
}

void ProjectileStep(Sim& sim, Projectile* p) {
  if (!p->HasLuaObject() || p->steppedTick == sim.tick()) return;
  p->steppedTick = sim.tick();
  PurgeShooters(sim, p);
  sim.HoldShape(p);
  p->heldVel = EntityVelocity(p);
  sim.steppingProjectile = p;
  const bool queued = p->destroyQueued;
  MotionTick(sim, p);
  if (!queued && p->destroyQueued) p->queuedInStepTick = sim.tick();
  sim.steppingProjectile = nullptr;
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
  if (sim.rngTrace_) {
    lua_Debug ar;
    std::string tb;
    for (int lv = 1; lv < 6 && lua_getstack(L, lv, &ar); ++lv)
      if (lua_getinfo(L, "Sl", &ar)) tb += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
    Logf(LogLevel::Info, "projtb %u %s%s", sim.tick(), lua_tostring(L, 2) ? lua_tostring(L, 2) : "?", tb.c_str());
  }
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
  // (EntityCreateProjectile 0x68a578: COORDS_Orient 0x50b480 of the direction as given, not vm::Orient's atan2 / asin)
  return PushProj(L, sim.CreateProjectile(L, *bp, e->army, e, pos, LookQuat(d.x, d.y, d.z), 0, 0, "Normal", AiTarget{}, true));
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
  if (BitsZero(d)) d = Forward(q);  // (0x68a8db..0x68a8e9: RandomDirection's vector against zero by memcmp 0x4f0a50)
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
    static std::atomic<bool> logged{false};  // (once a process)
    if (!logged.exchange(true)) {
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
  // (0x6d65d0..0x6d662f: the look rotation 0x50b480 of straight down, or of the normalised solution 0x44f7e0)
  if (pb.straightDown) {
    q = LookQuat(0.0f, -1.0f, 0.0f);
  } else if (w->bp->useFiringSolutionInsteadOfAimBone && !IsNaN(w->solution)) {
    const Vec3 s = Norm(w->solution);
    q = LookQuat(s.x, s.y, s.z);
  }
  float R = w->firingRandomness;
  static const bool dbgProj = getenv("MOHO64_DEBUG_PROJ") != nullptr;
  if (dbgProj) {
    const Vec3 tpDbg = w->target.type ? TargetPos(sim, w->target, false) : Vec3{};
    Logf(LogLevel::Info, "wproj %u unit %u %s weapon %s randomness %g moving %d target type %d point %.9g %.9g %.9g",
         sim.tick(), u->id, u->blueprint ? u->blueprint->id.c_str() : "?", w->label.c_str(), R,
         static_cast<int>(u->unitStates.Has(US("Moving"))), w->target.type, tpDbg.x, tpDbg.y, tpDbg.z);
    Vec3 bp0;
    Quat bq0;
    BoneWorld(u, bone, &bp0, &bq0);
    Logf(LogLevel::Info,
         "wproj %u unit %u   bone %d at %.9g %.9g %.9g bone q %.9g %.9g %.9g %.9g, shell q %.9g %.9g %.9g %.9g, solution %.9g "
         "%.9g %.9g instead of the bone %d, straight down %d, unit at %.9g %.9g %.9g q %.9g %.9g %.9g %.9g posed %d",
         sim.tick(), u->id, bone, bp0.x, bp0.y, bp0.z, bq0.x, bq0.y, bq0.z, bq0.w, q.x, q.y, q.z, q.w, w->solution.x,
         w->solution.y, w->solution.z, w->bp->useFiringSolutionInsteadOfAimBone ? 1 : 0, pb.straightDown ? 1 : 0,
         u->position.x, u->position.y, u->position.z, u->orientation.x, u->orientation.y, u->orientation.z,
         u->orientation.w, u->poseSet ? 1 : 0);
    // the bone's chain, root first: each bone's local rotation as posed this beat and at rest
    if (u->skeleton && bone >= 0 && bone < u->skeleton->Count()) {
      const auto& bones = u->skeleton->bones();
      int chain[64];
      int n = 0;
      for (int i = bone; i >= 0 && n < 64; i = bones[static_cast<size_t>(i)].parent) chain[n++] = i;
      const bool posed = u->poseQ.size() == bones.size();
      Logf(LogLevel::Info, "wproj %u unit %u   pose base q %.9g %.9g %.9g %.9g", sim.tick(), u->id,
           (u->poseSet ? u->poseBaseOri : u->orientation).x, (u->poseSet ? u->poseBaseOri : u->orientation).y,
           (u->poseSet ? u->poseBaseOri : u->orientation).z, (u->poseSet ? u->poseBaseOri : u->orientation).w);
      for (int k = n - 1; k >= 0; --k) {
        const size_t bi = static_cast<size_t>(chain[k]);
        const Quat pq = posed ? u->poseQ[bi] : bones[bi].localRot;
        Logf(LogLevel::Info, "wproj %u unit %u   chain bone %d posed q %.9g %.9g %.9g %.9g rest q %.9g %.9g %.9g %.9g",
             sim.tick(), u->id, chain[k], pq.x, pq.y, pq.z, pq.w, bones[bi].localRot.x, bones[bi].localRot.y,
             bones[bi].localRot.z, bones[bi].localRot.w);
      }
    }
  }
  if (R > 0) {
    // 0x6d667e..0x6d67d2: two Gauss draws times the randomness times [0xdff090] (degrees to radians, in that
    // order), the rotation r = yaw about +y times pitch about +x (0x50b300 with its third angle 0: fcos / fsin of the
    // half angles, then single products), and q x r summed in the order below. The product is NOT normalised.
    const float g1 = Gauss(sim), g2 = Gauss(sim);
    const float pitch = (g1 * R) * kDeg2Rad, yaw = (g2 * R) * kDeg2Rad;
    const float cA = dmath::Cos(pitch * 0.5f), sA = dmath::Sin(pitch * 0.5f);
    const float cB = dmath::Cos(yaw * 0.5f), sB = dmath::Sin(yaw * 0.5f);
    const Quat r{sA * cB, sB * cA, -(sB * sA), cB * cA};
    const Quat b = q;
    q = {((r.z * b.y + b.x * r.w) + r.x * b.w) - r.y * b.z, ((r.x * b.z + b.y * r.w) + r.y * b.w) - r.z * b.x,
         ((r.y * b.x + b.z * r.w) + r.z * b.w) - r.x * b.y, ((r.w * b.w - r.x * b.x) - r.y * b.y) - r.z * b.z};
  }
  float damage = w->ovDamage >= 0 ? w->ovDamage : w->bp->damage;
  float radius = w->ovDamageRadius >= 0 ? w->ovDamageRadius : w->bp->damageRadius;
  const std::string& type = w->ovDamageType.empty() ? w->bp->damageType : w->ovDamageType;
  Projectile* p = sim.CreateProjectile(L, *w->projBp, u->army, u, pos, q, damage, radius, type, w->target,
                                       w->bp->ignoresAlly);
  if (dbgProj)
    Logf(LogLevel::Info, "wproj %u unit %u   made %u %s destroy queued %d", sim.tick(), u->id, p ? p->id : 0,
         w->projBp->id.c_str(), p ? static_cast<int>(p->destroyQueued) : -1);
  if (!p || p->destroyQueued) return PushProj(L, p);
  if (w->bp->muzzleVelocity != 0) {
    Vec3 tp = TargetPos(sim, w->target, false);
    const Vec3 dd = Sub(pos, tp);
    float dist = IsNaN(tp) ? 0 : std::sqrt((dd.z * dd.z + dd.y * dd.y) + dd.x * dd.x);  // (0x6d68d7..0x6d68ee)
    float mv = w->bp->muzzleVelocity + (w->bp->muzzleVelocityRandom != 0 ? Gauss(sim) * w->bp->muzzleVelocityRandom : 0);
    if (!IsNaN(tp) && dist < w->bp->muzzleVelocityReduceDistance)
      mv *= std::sqrt(dist / w->bp->muzzleVelocityReduceDistance);
    Vec3 v = p->velocity;  // Normalize 0x452af0 (0x6d6912), then times the muzzle velocity
    vm::NormalizeExe(v);
    p->velocity = {v.x * mv, v.y * mv, mv * v.z};
  }
  if (w->bp->projectileLifetime > 0)
    p->expireTick = sim.tick() + static_cast<uint32_t>(std::nearbyint(w->bp->projectileLifetime * 10));
  if (w->bp->projectileLifetimeUsesMultiplier > 0 && w->bp->muzzleVelocity != 0) {
    // 0x6d6988..0x6d69bd: the BLUEPRINT's MaxRadius (wbp+0x5c), never the ChangeMaxRadius override (weapon+0x64):
    // an enhanced range (the Aeon SACU's StabilitySuppressant, 30 -> 40) keeps the shell's lifetime of the stock range
    const float maxR = w->bp->maxRadius;
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
  // 0x6a3c71..0x6a3cb9: the child's transform is the parent's CURRENT one (parent+0x9c), which a step taken
  // earlier in this beat has not changed yet (the step wrote the pending transform; AdvanceCoords commits it).
  // So debris dropped from OnKilled by a missile that an anti-missile hits later in the pass starts where the
  // missile was when the beat began, facing as it faced then, and takes its first step in this pass like any
  // shell made in it. (The shell whose own step is running makes its children where that step has put it:
  // fragments at the impact, as before.)
  const bool held = p->shapeHeld && p != sim.steppingProjectile;
  return PushProj(L, sim.CreateProjectile(L, *bp, p->army, p, held ? p->shapePos : p->position,
                                          held ? p->shapeOri : p->orientation, p->damage, p->damageRadius,
                                          p->damageType, p->target, true));
}
int l_GetVelocity(lua_State* L) {
  Vec3 v = EntityVelocity(P(L));
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}
int l_GetCurrentSpeed(lua_State* L) {
  const Vec3 v = EntityVelocity(P(L));
  lua_pushnumber(L, std::sqrt((v.z * v.z + v.y * v.y) + v.x * v.x));  // (0x6a2846..0x6a285d)
  return 1;
}
// A shell whose target entity no longer exists has no target position any more: the original hands the script
// the invalid vector (seen in the watch: 1.#QNAN x 3 once the dead unit is destroyed; while the dead unit is
// still there, its position).
static bool TargetGone(Sim& sim, const AiTarget& t) { return t.type == 1 && !TargetEntity(sim, t); }
int l_GetCurrentTargetPosition(lua_State* L) {
  Projectile* p = P(L);
  if (TargetGone(*S(L), p->target)) {
    if (const Entity* blip = BlipOfTarget(*S(L), p->target)) {  // (the blip the unit left behind, for a few beats)
      PushVec(L, BlipPointUnitGone(*S(L), blip));  // (0x6a1797: GetTargetPosGun, centre flag clear)
      return 1;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    PushVec(L, Vec3{nan, nan, nan});
    return 1;
  }
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
  static const bool dbgProj = getenv("MOHO64_DEBUG_PROJ") != nullptr;  // (read once)
  if (dbgProj)
    Logf(LogLevel::Info, "pdbg %u %u SetLifetime %.3f (was until %u)", S(L)->tick(), p->id, lua_tonumber(L, 2), p->expireTick);
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
  // 0x6a196d: the new target is built with CAiTarget::UpdateTarget, which picks one of the entity's target points
  // (a draw when it is a unit with target bones). FAF's Projectile.RetargetThread comes here a beat after a
  // homing shell lost its target, with what its weapon aims at now: one draw per torpedo of a salvo.
  p->target.aimBone = TargetPointBone(*S(L), e);
  static const bool dbgProj = getenv("MOHO64_DEBUG_PROJ") != nullptr;  // (read once)
  if (dbgProj) {  // who gives a shell a new target, and from which script line
    luaL_where(L, 1);
    const std::string at1 = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    luaL_where(L, 2);
    const std::string at2 = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    Logf(LogLevel::Info, "pdbg %u %u SetNewTarget %u (%s) from %s< %s", S(L)->tick(), p->id, e ? e->id : 0,
         e && e->blueprint ? e->blueprint->id.c_str() : "?", at1.c_str(), at2.c_str());
  }
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
    Vec3 v = p->velocity;  // Normalize 0x452af0 (0x6a2bc7), then times the speed
    vm::NormalizeExe(v);
    const float s = static_cast<float>(luaL_checknumber(L, 2));
    p->velocity = {v.x * s, v.y * s, s * v.z};
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
  float vx = sim.U01();
  uint32_t cb = 0x2f733333u;
  float c;
  std::memcpy(&c, &cb, 4);
  float vy = dmath::MulU32(sim.NextUInt32(), c) + 0.05f;  // x87 at PC_24
  float vz = sim.U01();
  Vec3 v{vx, vy, vz};
  const ProjBp& b = GetProjBp(sim, L, *p->blueprint);
  // SetLength 0x5b1c90 to MaxSpeed (0x6a3ead): one factor, no test on MaxSpeed
  if (const float l2 = (v.x * v.x + v.y * v.y) + v.z * v.z; l2 > 0.0f) {
    const float k = b.maxSpeed / std::sqrt(l2);
    v = {v.x * k, k * v.y, k * v.z};
  }
  p->velocity = v;
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
  SetGlobal(L, "IsProjectile", [](lua_State* L) -> int {  // as IsUnit (entities.cpp l_IsKind)
    ScriptObject* o = CheckAnyObject(L, 1);
    if (o->typeBits & kTypeProjectile) lua_pushvalue(L, 1);
    else lua_pushnil(L);
    return 1;
  });
}

}  // namespace moho
