// Combat: weapons, aim controllers, attack commands, intel. See combat.h; projectiles are in
// sim/projectile.cpp, damage, killing and beams in sim/damage.cpp.
//
// Weapons (FA exe, read 2026-10-08):
// - Every non-ManualFire weapon has an acquire task (CAcquireTargetTask 0x5d8d10) that runs every
//   max(1, ceil(TargetCheckInterval*10)) ticks; it reports a state code for the unit's attack
//   command, takes the commanded target when it is in range, else the best enemy among the units
//   its army has a blip on (FindBestEnemy 0x5d7a10: the script's TargetPriorities first, then
//   distance, or the angle off the muzzle for turrets). Targets change only in SetTarget
//   (0x6d5f30), which calls OnLostTarget (to none) / OnGotTarget (from none).
// - A fire task (CFireWeaponTask 0x6d3dc0) runs every tick: when the clock is 0, the fire state is
//   not HoldFire, the target is attackable and in range (2-D, unit centre to target centre) and
//   CanFire holds (not stunned or busy, the aim controller's on-target flag, bomb-drop maths for
//   bombers), it calls OnFire() and restarts the clock at round(10 / RateOfFire).
// - Aim controllers (CAimManipulator, MoveManipulator 0x630db0) turn their yaw and pitch bones
//   toward the firing solution at their slew rates within their arcs, once per tick after the
//   unit's motion; the one whose label is the weapon's fire control decides "on target".
//   Projectiles launch along the muzzle bone as aimed, so the pose matters to the sim.
// Intel (approximation, TODO(M4d): the original's recon grids): every tick each army's units
// see enemies within their vision / radar / sonar / omni radii; allies share what they see.
#include "core/dmath.h"
#include "sim/combat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/intel.h"
#include "sim/commands.h"
#include "sim/luautil.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

// Shared with sim/damage.cpp and sim/projectile.cpp.
namespace combat {
uint64_t g_fires = 0, g_impacts = 0, g_projectiles = 0;
int LayerBit(const std::string& l);
int EntityLayerBit(const Entity* e);
int Relation(const Army* a, const Army* b);
bool IsEnemy(const Army* a, const Army* b) { return a && b && Relation(a, b) == 0; }
bool IsAlly(const Army* a, const Army* b) { return a && b && Relation(a, b) == 2; }
float WaterLevel(const Sim& sim);
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs);
bool CallMethodNumber(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs, float* out);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool HasTarget(Sim& sim, const AiTarget& t);
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
Entity* TargetEntity(Sim& sim, const AiTarget& t);
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
bool IsMobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }

}  // namespace

// ---- shared helpers ------------------------------------------------------------------------------

namespace combat {

int LayerBit(const std::string& l) {
  switch (l.empty() ? 0 : l[0]) {
    case 'L': return l == "Land" ? 1 : 0;
    case 'S': return l == "Seabed" ? 2 : (l == "Sub" ? 4 : 0);
    case 'W': return l == "Water" ? 8 : 0;
    case 'A': return l == "Air" ? 16 : 0;
    case 'O': return l == "Orbit" ? 32 : 0;
    default: return 0;
  }
}
int EntityLayerBit(const Entity* e) {
  if (e->kind == Entity::Kind::Unit) return LayerBit(static_cast<const Unit*>(e)->layer);
  if (e->kind == Entity::Kind::Projectile) return LayerBit(static_cast<const Projectile*>(e)->layer);
  if (e->kind == Entity::Kind::Prop) return 1;
  return 0;
}
int Relation(const Army* a, const Army* b) {
  if (!a || !b) return 1;
  if (a == b) return 2;
  size_t i = static_cast<size_t>(b->index - 1);
  return i < a->alliance.size() ? a->alliance[i] : 1;
}
float WaterLevel(const Sim& sim) {
  const TerrainMap* m = sim.map();
  return m && m->hasWater ? m->waterElevation : -10000.0f;
}

// obj:method(args) with nargs args pushed: -1 missing function or error, else 0/1.
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs) {
  lua_checkstack(L, 10);
  int base = lua_gettop(L) - nargs;
  if (!obj->HasLuaObject()) {
    lua_settop(L, base);
    return -1;
  }
  PushObject(L, obj);
  int self = lua_gettop(L);
  lua_pushstring(L, method);
  lua_gettable(L, self);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, base);
    return -1;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  lua_insert(L, base + 2);  // fn
  lua_insert(L, base + 3);  // self
  int r = -1;
  if (lua_pcall(L, nargs + 1, 1, base + 1) == 0) r = lua_toboolean(L, -1) ? 1 : 0;
  else LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : method);
  lua_settop(L, base);
  (void)sim;
  return r;
}
bool CallMethodNumber(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs, float* out) {
  lua_checkstack(L, 10);
  int base = lua_gettop(L) - nargs;
  if (!obj->HasLuaObject()) {
    lua_settop(L, base);
    return false;
  }
  PushObject(L, obj);
  int self = lua_gettop(L);
  lua_pushstring(L, method);
  lua_gettable(L, self);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, base);
    return false;
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  lua_insert(L, base + 2);
  lua_insert(L, base + 3);
  bool ok = false;
  if (lua_pcall(L, nargs + 1, 1, base + 1) == 0) {
    ok = true;
    *out = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
  } else {
    LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : method);
  }
  lua_settop(L, base);
  (void)sim;
  return ok;
}

// The first point of the segment below the terrain (STIMap::Intersection, approximated by
// marching at most half a unit at a time and refining).
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit) {
  if (!map) return false;
  Vec3 d = Sub(p1, p0);
  float len = Len(d);
  auto below = [&](const Vec3& p) { return p.y < map->TerrainHeight(p.x, p.z); };
  if (below(p0)) {
    *dist = 0;
    *hit = p0;
    return true;
  }
  int n = std::max(1, static_cast<int>(std::ceil(len / 0.5f)));
  float prev = 0;
  for (int i = 1; i <= n; ++i) {
    float t = static_cast<float>(i) / n;
    Vec3 p = Add(p0, Mul(d, t));
    if (below(p)) {
      float lo = prev, hi = t;
      for (int k = 0; k < 8; ++k) {
        float m = 0.5f * (lo + hi);
        if (below(Add(p0, Mul(d, m)))) hi = m;
        else lo = m;
      }
      Vec3 h = Add(p0, Mul(d, hi));
      h.y = map->TerrainHeight(h.x, h.z);
      *hit = h;
      *dist = hi * len;
      return true;
    }
    prev = t;
  }
  return false;
}

Entity* TargetEntity(Sim& sim, const AiTarget& t) {
  if (t.type != 1 || !t.entityId) return nullptr;
  Entity* e = sim.FindEntity(t.entityId);
  return e && !e->destroyQueued ? e : nullptr;
}
// CAiTarget::HasTarget 0x5e2a10
bool HasTarget(Sim& sim, const AiTarget& t) {
  if (t.type == 2) return true;
  if (t.type != 1) return false;
  Entity* e = TargetEntity(sim, t);
  if (!e) return false;
  if (e->kind == Entity::Kind::Unit) return !e->dead;
  return true;
}

}  // namespace combat

// The point a weapon aims at on a unit (Unit::GetTargetPoint): its aim bone, else the centre of
// its collision primitive.
static Vec3 UnitTargetPoint(const Unit* u, int bone) {
  if (bone >= 0) {
    Vec3 p;
    Quat q;
    BoneWorld(u, bone, &p, &q);
    return p;
  }
  if (u->shape.type != ShapeType::None) return Add(u->position, Rotate(u->orientation, u->shape.center));
  return u->position;
}

namespace combat {
// GetTargetPosGun 0x5e2a90
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre) {
  if (t.type == 2) return t.pos;
  if (t.type == 1) {
    Entity* e = TargetEntity(sim, t);
    if (!e) return NaNVec();
    if (e->kind == Entity::Kind::Unit) return centre ? e->position : UnitTargetPoint(static_cast<Unit*>(e), t.aimBone);
    return e->position;
  }
  return NaNVec();
}
}  // namespace combat

Vec3 EntityVelocity(const Entity* e) {
  if (e->kind == Entity::Kind::Unit) return UnitVelocity(static_cast<const Unit*>(e));  // vtbl+0x3c
  if (e->kind == Entity::Kind::Projectile) {
    const Projectile* p = static_cast<const Projectile*>(e);
    return Mul(Sub(p->position, p->prevPos), p->velScale);
  }
  return {};
}

void BoneWorld(const Entity* e, int bone, Vec3* pos, Quat* rot) {
  if (e->kind == Entity::Kind::Beam) {  // bones 1 and 2: the end of the beam
    *rot = e->orientation;
    *pos = bone >= 1 ? Add(e->position, Mul(Forward(e->orientation), static_cast<const CollisionBeam*>(e)->length))
                     : e->position;
    return;
  }
  if (bone < 0 || !e->skeleton || bone >= e->skeleton->Count()) {
    *pos = e->position;
    *rot = e->orientation;
    return;
  }
  const auto& bones = e->skeleton->bones();
  float s = e->meshScale * e->scale[0];
  if (e->poseRot.empty()) {
    const Bone& b = bones[static_cast<size_t>(bone)];
    Vec3 m{b.modelPos.x * s, b.modelPos.y * s, b.modelPos.z * s};
    *pos = Add(e->position, Rotate(e->orientation, m));
    *rot = QMul(e->orientation, b.modelRot);
    return;
  }
  // walk the chain from the root: rot_i = rot_parent * local_i * extra_i
  int chain[64];
  int n = 0;
  for (int i = bone; i >= 0 && n < 64; i = bones[static_cast<size_t>(i)].parent) chain[n++] = i;
  Quat r{};
  Vec3 p{};
  for (int k = n - 1; k >= 0; --k) {
    const Bone& b = bones[static_cast<size_t>(chain[k])];
    Vec3 lp{b.localPos.x * s, b.localPos.y * s, b.localPos.z * s};
    p = Add(p, Rotate(r, lp));
    r = QMul(r, b.localRot);
    if (static_cast<size_t>(chain[k]) < e->poseRot.size()) r = QMul(r, e->poseRot[static_cast<size_t>(chain[k])]);
  }
  *pos = Add(e->position, Rotate(e->orientation, p));
  *rot = QMul(e->orientation, r);
}

bool BpInCategory(Sim& sim, const BlueprintInfo* bp, const char* name) {
  if (!bp || bp->entityIndex < 0) return false;
  static std::map<std::string, std::vector<bool>> cache;
  auto it = cache.find(name);
  if (it == cache.end()) {
    std::vector<bool> bits(static_cast<size_t>(sim.blueprints().EntityCount()), false);
    auto c = sim.blueprints().Categories().find(name);
    if (c != sim.blueprints().Categories().end())
      for (int i : c->second)
        if (i >= 0 && static_cast<size_t>(i) < bits.size()) bits[static_cast<size_t>(i)] = true;
    it = cache.emplace(name, std::move(bits)).first;
  }
  return static_cast<size_t>(bp->entityIndex) < it->second.size() && it->second[static_cast<size_t>(bp->entityIndex)];
}

// ---- blueprint data --------------------------------------------------------------------------------

const WeaponBp& GetWeaponBp(lua_State* L, int bpRef) {
  static std::map<const void*, std::unique_ptr<WeaponBp>> cache;
  int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, bpRef);
  int t = lua_gettop(L);
  const void* key = lua_topointer(L, t);
  auto it = cache.find(key);
  if (it != cache.end()) {
    lua_settop(L, top);
    return *it->second;
  }
  auto w = std::make_unique<WeaponBp>();
  w->label = lu::Str(L, t, "Label");
  std::string rc = lu::Str(L, t, "RangeCategory", "UWRC_Undefined");
  const char* rcs[] = {"UWRC_Undefined", "UWRC_DirectFire", "UWRC_IndirectFire", "UWRC_AntiAir", "UWRC_AntiNavy",
                       "UWRC_Countermeasure"};
  for (int i = 0; i < 6; ++i)
    if (rc == rcs[i]) w->rangeCategory = i;
  w->prefersPrimaryWeaponTarget = lu::Bool(L, t, "PrefersPrimaryWeaponTarget");
  w->stopOnPrimaryWeaponBusy = lu::Bool(L, t, "StopOnPrimaryWeaponBusy");
  w->slavedToBody = lu::Bool(L, t, "SlavedToBody");
  w->slavedToBodyArcRange = lu::Num(L, t, "SlavedToBodyArcRange", 1);
  w->autoInitiateAttackCommand = lu::Bool(L, t, "AutoInitiateAttackCommand");
  w->targetCheckInterval = lu::Num(L, t, "TargetCheckInterval", 3);
  w->alwaysRecheckTarget = lu::Bool(L, t, "AlwaysRecheckTarget", true);
  w->minRadius = lu::Num(L, t, "MinRadius", 0);
  w->maxRadius = lu::Num(L, t, "MaxRadius", 0);
  w->maximumBeamLength = lu::Num(L, t, "MaximumBeamLength", 0);
  w->maxHeightDiff = lu::Num(L, t, "MaxHeightDiff", std::numeric_limits<float>::infinity());
  w->trackingRadius = lu::Num(L, t, "TrackingRadius", 1);
  w->headingArcCenter = lu::Num(L, t, "HeadingArcCenter", 0);
  w->headingArcRange = lu::Num(L, t, "HeadingArcRange", 180);
  w->firingTolerance = lu::Num(L, t, "FiringTolerance", 0.01f);
  w->firingRandomness = lu::Num(L, t, "FiringRandomness", 0);
  w->muzzleVelocity = lu::Num(L, t, "MuzzleVelocity", 0);
  w->muzzleVelocityRandom = lu::Num(L, t, "MuzzleVelocityRandom", 0);
  w->muzzleVelocityReduceDistance = lu::Num(L, t, "MuzzleVelocityReduceDistance", 0);
  w->leadTarget = lu::Bool(L, t, "LeadTarget", true);
  w->projectileLifetime = lu::Num(L, t, "ProjectileLifetime", 0);
  w->projectileLifetimeUsesMultiplier = lu::Num(L, t, "ProjectileLifetimeUsesMultiplier", 0);
  w->damage = lu::Num(L, t, "Damage", 0);
  w->damageRadius = lu::Num(L, t, "DamageRadius", 0);
  w->damageType = lu::Str(L, t, "DamageType", "Normal");
  w->rateOfFire = lu::Num(L, t, "RateOfFire", 1);
  w->projectileId = lu::Str(L, t, "ProjectileId");
  std::string ba = lu::Str(L, t, "BallisticArc", "RULEUBA_None");
  w->ballisticArc = ba == "RULEUBA_LowArc" ? 1 : ba == "RULEUBA_HighArc" ? 2 : 0;
  w->targetRestrictOnlyAllow = lu::Str(L, t, "TargetRestrictOnlyAllow");
  w->targetRestrictDisallow = lu::Str(L, t, "TargetRestrictDisallow");
  w->manualFire = lu::Bool(L, t, "ManualFire");
  w->nukeWeapon = lu::Bool(L, t, "NukeWeapon");
  w->overChargeWeapon = lu::Bool(L, t, "OverChargeWeapon");
  w->needPrep = lu::Bool(L, t, "NeedPrep");
  w->countedProjectile = lu::Bool(L, t, "CountedProjectile");
  w->ignoresAlly = lu::Bool(L, t, "IgnoresAlly", true);
  std::string tt = lu::Str(L, t, "TargetType", "RULEWTT_Unit");
  w->targetType = tt == "RULEWTT_Projectile" ? 1 : tt == "RULEWTT_Prop" ? 2 : 0;
  w->attackGroundTries = static_cast<int>(lu::Num(L, t, "AttackGroundTries", 3));
  w->aimsStraightOnDisable = lu::Bool(L, t, "AimsStraightOnDisable");
  w->turreted = lu::Bool(L, t, "Turreted");
  w->yawOnlyOnTarget = lu::Bool(L, t, "YawOnlyOnTarget");
  w->aboveWaterFireOnly = lu::Bool(L, t, "AboveWaterFireOnly");
  w->belowWaterFireOnly = lu::Bool(L, t, "BelowWaterFireOnly");
  w->aboveWaterTargetsOnly = lu::Bool(L, t, "AboveWaterTargetsOnly");
  w->belowWaterTargetsOnly = lu::Bool(L, t, "BelowWaterTargetsOnly");
  w->reTargetOnMiss = lu::Bool(L, t, "ReTargetOnMiss");
  w->needToComputeBombDrop = lu::Bool(L, t, "NeedToComputeBombDrop");
  w->bombDropThreshold = lu::Num(L, t, "BombDropThreshold", 1.5f);
  w->useFiringSolutionInsteadOfAimBone = lu::Bool(L, t, "UseFiringSolutionInsteadOfAimBone");
  w->ignoreIfDisabled = lu::Bool(L, t, "IgnoreIfDisabled");
  w->cannotAttackGround = lu::Bool(L, t, "CannotAttackGround");
  lua_settop(L, top);
  return *cache.emplace(key, std::move(w)).first->second;
}

// Unit blueprint values combat reads.
struct CombatBpData {
  bool canFly = false, winged = false;
  float maxAirspeed = 0, engageDistance = 0, predictAheadForBombDrop = 0;
  float guardScanRadius = 0, guardReturnRadius = 0, attackAngle = 0;
  bool needUnpack = false;
  std::vector<std::string> targetBones;
  float sizeY = 1, sizeZ = 1;
  float intel[5] = {0, 0, 0, 0, 0};  // Vision, WaterVision, Radar, Sonar, Omni radii
  bool radarStealth = false, sonarStealth = false, cloak = false;
  bool command = false, benign = false, structure = false;
};

static const CombatBpData& GetCombatBpData(Sim& sim, lua_State* L, const BlueprintInfo& bp) {
  static std::map<const BlueprintInfo*, CombatBpData> cache;
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  CombatBpData d;
  int top = lua_gettop(L);
  sim.blueprints().PushTable(L, bp);
  int t = lua_gettop(L);
  d.sizeY = lu::Num(L, t, "SizeY", 1);
  d.sizeZ = lu::Num(L, t, "SizeZ", 1);
  int air = lu::Sub(L, t, "Air");
  d.canFly = lu::Bool(L, air, "CanFly");
  d.winged = lu::Bool(L, air, "Winged");
  d.maxAirspeed = lu::Num(L, air, "MaxAirspeed", 0);
  d.engageDistance = lu::Num(L, air, "EngageDistance", 0);
  d.predictAheadForBombDrop = lu::Num(L, air, "PredictAheadForBombDrop", 0);
  int ai = lu::Sub(L, t, "AI");
  d.guardScanRadius = lu::Num(L, ai, "GuardScanRadius", 25.0f);  // RUnitBlueprintAI ctor default
  d.guardReturnRadius = lu::Num(L, ai, "GuardReturnRadius", 50);
  d.attackAngle = lu::Num(L, ai, "AttackAngle", 0);
  d.needUnpack = lu::Bool(L, ai, "NeedUnpack");
  int tb = lu::Sub(L, ai, "TargetBones");
  if (lua_istable(L, tb))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, tb, i);
      if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      d.targetBones.push_back(lua_tostring(L, -1));
      lua_pop(L, 1);
    }
  int intel = lu::Sub(L, t, "Intel");
  d.intel[0] = lu::Num(L, intel, "VisionRadius", 0);
  d.intel[1] = lu::Num(L, intel, "WaterVisionRadius", 0);
  d.intel[2] = lu::Num(L, intel, "RadarRadius", 0);
  d.intel[3] = lu::Num(L, intel, "SonarRadius", 0);
  d.intel[4] = lu::Num(L, intel, "OmniRadius", 0);
  d.radarStealth = lu::Bool(L, intel, "RadarStealth");
  d.sonarStealth = lu::Bool(L, intel, "SonarStealth");
  d.cloak = lu::Bool(L, intel, "Cloak");
  lua_settop(L, top);
  d.command = BpInCategory(sim, &bp, "COMMAND");
  d.benign = BpInCategory(sim, &bp, "BENIGN");
  d.structure = BpInCategory(sim, &bp, "STRUCTURE");
  return cache.emplace(&bp, std::move(d)).first->second;
}

static const CombatBpData& CB(Unit* u) {
  if (!u->combat) {
    Sim* sim = Sim::From(u->luaState());
    u->combat = &GetCombatBpData(*sim, sim->L(), *u->blueprint);
  }
  return *u->combat;
}

// ---- weapons: values and predicates ---------------------------------------------------------------

namespace {

float MaxR(const UnitWeapon* w) { return w->ovMaxRadius >= 0 ? w->ovMaxRadius : w->bp->maxRadius; }
float MinR(const UnitWeapon* w) { return w->ovMinRadius >= 0 ? w->ovMinRadius : w->bp->minRadius; }
float RoF(const UnitWeapon* w) { return w->ovRateOfFire >= 0 ? w->ovRateOfFire : w->bp->rateOfFire; }
float Tolerance(const UnitWeapon* w) { return w->ovFiringTolerance >= 0 ? w->ovFiringTolerance : w->bp->firingTolerance; }
float MaxHeightDiff(const UnitWeapon* w) { return w->ovMaxHeightDiff >= 0 ? w->ovMaxHeightDiff : w->bp->maxHeightDiff; }
float MaxR2(UnitWeapon* w) {
  if (w->maxR2 < 0) w->maxR2 = w->bp->maxRadius * w->bp->maxRadius;
  return w->maxR2;
}
float MinR2(UnitWeapon* w) {
  if (w->minR2 < 0) w->minR2 = w->bp->minRadius * w->bp->minRadius;
  return w->minR2;
}
int AcquirePeriod(const UnitWeapon* w) {
  float x = w->bp->targetCheckInterval * 10;
  int r = static_cast<int>(std::nearbyint(x));
  if (static_cast<float>(r) < x) ++r;
  return std::max(1, r);
}
bool InSet(const std::vector<uint64_t>& set, const BlueprintInfo* bp) {
  return bp && bp->entityIndex >= 0 && !set.empty() && CategoryHas(set.data(), bp->entityIndex);
}
bool SetEmpty(const std::vector<uint64_t>& set) {
  for (uint64_t w : set)
    if (w) return false;
  return true;
}

// The weapon's aim bone position (UnitWeapon::GetTransform 0x6d5330).
Vec3 WeaponPos(const UnitWeapon* w) {
  if (w->aimBone < 0) return w->unit->position;
  Vec3 p;
  Quat q;
  BoneWorld(w->unit, w->aimBone, &p, &q);
  return p;
}
Vec3 WeaponForward(const UnitWeapon* w) {
  if (w->aimBone < 0) return Forward(w->unit->orientation);
  Vec3 p;
  Quat q;
  BoneWorld(w->unit, w->aimBone, &p, &q);
  return Forward(q);
}
float UnitHeading(const Unit* u) {
  Vec3 f = Forward(u->orientation);
  return dmath::Atan2(f.x, f.z);
}

// PickTargetPoint 0x6d5590: whether the weapon may target the entity at all.
bool Targetable(UnitWeapon* w, Entity* e) {
  if (!e) return false;
  if (w->bp->ignoreIfDisabled && !w->enabled) return false;
  if ((w->layerCaps & EntityLayerBit(e)) == 0) return false;
  if (!e->blueprint) return false;
  if (!SetEmpty(w->disallow) && InSet(w->disallow, e->blueprint)) return false;
  if (!SetEmpty(w->onlyAllow) && !InSet(w->onlyAllow, e->blueprint)) return false;
  return true;
}

// UnitWeapon::CanAttackTarget 0x6d5720
bool CanAttackTarget(Sim& sim, UnitWeapon* w, const AiTarget& t) {
  if (t.type == 0) return false;
  if (w->bp->ignoreIfDisabled && !w->enabled) return false;
  if (t.type == 1) return Targetable(w, TargetEntity(sim, t));
  if (t.type == 2) {
    if (w->bp->cannotAttackGround) return false;
    const TerrainMap* m = sim.map();
    float h = m ? m->TerrainHeight(t.pos.x, t.pos.z) : 0;
    float wl = WaterLevel(sim);
    if (h > wl && (w->layerCaps & 1)) return true;
    if (h < wl && (w->layerCaps & 8)) return true;
    return false;
  }
  return true;
}

// TargetSolutionStatusGun 0x6d5b40: 0 ok, 1 too close, 2 outside the heading arc, 3 out of range.
int SolutionStatus(UnitWeapon* w, Vec3 pos, float* distSq) {
  const Unit* u = w->unit;
  float d2 = (distSq && *distSq > 0) ? *distSq : Dist2XZ(pos, u->position);
  if (d2 > MaxR2(w)) return 3;
  if (d2 <= MinR2(w)) return 1;
  if (std::fabs(pos.y - u->position.y) > MaxHeightDiff(w)) return 3;
  if (w->bp->headingArcRange < 180) {
    Vec3 o = WeaponPos(w);
    float a = dmath::Atan2(pos.x - o.x, pos.z - o.z);
    float rel = WrapPi(a - UnitHeading(u) - w->bp->headingArcCenter * kDeg2Rad);
    if (std::fabs(rel) > w->bp->headingArcRange * kDeg2Rad) return 2;
  }
  if (distSq) *distSq = d2;
  return 0;
}
int TargetStatus(Sim& sim, UnitWeapon* w, const AiTarget& t) {
  Vec3 p = TargetPos(sim, t, true);
  if (IsNaN(p)) return 3;
  return SolutionStatus(w, p, nullptr);
}

bool CheckSilo(UnitWeapon* w) {
  if (!w->bp->countedProjectile) return true;
  return w->unit->siloAmmo[w->bp->nukeWeapon ? 1 : 0] > 0;
}

// Unit::CalcBombDrop 0x6d3a40: where the bomber must be to release now and hit T.
Vec3 CalcBombDrop(const Unit* u, Vec3 T) {
  Vec3 v = Mul(u->motion.lastMove, 10);
  Vec3 P = u->position;
  const float G = 4.9f;
  float vu = std::fabs(v.y);
  float h = P.y - T.y, hm = std::fabs(h);
  float disc = (h < 0 ? -2 : 2) * hm * G + vu * vu;
  if (disc < 0) return NaNVec();
  float t = (vu - std::sqrt(disc)) / G;
  if (t < 0) t = (vu + std::sqrt(disc)) / G;
  if (t < 0) return NaNVec();
  Vec3 fall{v.x * t, v.y * t - 0.5f * G * t * t, v.z * t};
  return Sub(T, fall);
}

// UnitWeapon::CanFire 0x6d4c80
bool CanFire(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  if (u->stunned) return false;
  if (u->unitStates.count("Busy")) return false;
  const CombatBpData& cb = CB(u);
  if (cb.canFly && u->layer != "Air") return false;
  if (cb.needUnpack && !u->unitStates.count("Immobile")) return false;
  if (w->bp->aboveWaterFireOnly || w->bp->belowWaterFireOnly) {
    float y = WeaponPos(w).y, wl = WaterLevel(sim);
    if (w->bp->aboveWaterFireOnly && !(y > wl)) return false;
    if (w->bp->belowWaterFireOnly && !(y <= wl)) return false;
  }
  if (cb.winged) {
    if (w->bp->autoInitiateAttackCommand) {
      float speed = Len(u->motion.lastMove) * 10;
      if (speed < u->motion.speedMult * cb.maxAirspeed * 0.25f) return false;
    }
    if (w->bp->needToComputeBombDrop && HasTarget(sim, w->target)) {
      if (!u->unitStates.count("MakingAttackRun")) return false;
      Vec3 T = TargetPos(sim, w->target, false);
      Vec3 R = CalcBombDrop(u, T);
      if (IsNaN(R)) return false;
      float d = DistXZ(u->position, R), thr = w->bp->bombDropThreshold;
      if (d >= 2 * thr) return false;
      if (d >= thr) {
        Vec3 f = Forward(u->orientation);
        if ((R.x - u->position.x) * f.x + (R.z - u->position.z) * f.z > 0) return false;
        if ((T.x - u->position.x) * f.x + (T.z - u->position.z) * f.z < 0.866f) return false;
      }
    }
  }
  return w->onTarget;
}

}  // namespace

// ---- weapons: target changes -----------------------------------------------------------------------

int TargetPointDraw(Sim& sim, Entity* e) {
  if (!e) return -1;
  if (e->kind == Entity::Kind::Blip) e = static_cast<ReconBlip*>(e)->source;
  if (!e || e->kind != Entity::Kind::Unit) return -1;
  const CombatBpData& cb = CB(static_cast<Unit*>(e));
  if (cb.targetBones.empty()) return -1;
  return static_cast<int>(sim.IntRange(static_cast<uint32_t>(cb.targetBones.size())));
}

namespace {

void WeaponCall(Sim& sim, UnitWeapon* w, const char* method) {
  if (w->HasLuaObject()) sim.CallMethod(sim.L(), w, method, 0);
}

int PickAimBone(Sim& sim, Entity* e) {
  if (!e || e->kind != Entity::Kind::Unit || !e->skeleton) return -1;
  Unit* tu = static_cast<Unit*>(e);
  const CombatBpData& cb = CB(tu);
  if (cb.targetBones.empty()) return -1;
  size_t i = sim.IntRange(static_cast<uint32_t>(cb.targetBones.size()));
  return e->skeleton->Find(cb.targetBones[i]);
}

// Blacklist decay (0x6d78f0), run whenever a weapon gets a target.
void DecayBlacklist(Sim& sim, UnitWeapon* w) {
  auto& bl = w->blacklist;
  for (auto& b : bl) --b.counter;
  bl.erase(std::remove_if(bl.begin(), bl.end(),
                          [&](const UnitWeapon::BlackEntry& b) {
                            Entity* e = sim.FindEntity(b.id);
                            if (!e || e->destroyQueued || b.counter < 1) return true;
                            return e->position.x != b.pos.x || e->position.y != b.pos.y || e->position.z != b.pos.z;
                          }),
           bl.end());
}

}  // namespace

// UnitWeapon::SetTarget 0x6d5f30
static void SetTarget(Sim& sim, UnitWeapon* w, const AiTarget& nt) {
  if (w->target == nt) return;
  bool had = w->target.type != 0, has = nt.type != 0;
  if (had && !has) WeaponCall(sim, w, "OnLostTarget");
  if (has) DecayBlacklist(sim, w);
  if (Entity* old = TargetEntity(sim, w->target))
    if (old->shooters > 0) --old->shooters;
  w->target = nt;
  Entity* e = TargetEntity(sim, w->target);
  if (e && e->kind == Entity::Kind::Unit) w->target.mobile = IsMobile(static_cast<Unit*>(e));
  w->target.aimBone = PickAimBone(sim, e);
  if (e && !e->dead) ++e->shooters;
  if (!had && has) WeaponCall(sim, w, "OnGotTarget");
  w->missCount = 0;
  w->canReach = true;
  w->shots = 0;
}

static AiTarget EntityTarget(Entity* e) {
  AiTarget t;
  if (e) {
    t.type = 1;
    t.entityId = EntityRef(e);
  }
  return t;
}

void InitUnitWeapon(lua_State* L, UnitWeapon* w) {
  Sim& sim = *S(L);
  w->bp = &GetWeaponBp(L, w->bpRef);
  const WeaponBp& b = *w->bp;
  w->ovMaxHeightDiff = std::numeric_limits<float>::infinity();
  w->minR2 = b.minRadius * b.minRadius;
  w->maxR2 = b.maxRadius * b.maxRadius;
  w->firingRandomness = b.firingRandomness;
  w->solution = NaNVec();
  if (!b.projectileId.empty()) w->projBp = sim.blueprints().Find(b.projectileId);
  w->nextAcquire = sim.tick() + 1;
  if (!b.targetRestrictDisallow.empty()) ParseCategory(L, b.targetRestrictDisallow.c_str(), w->disallow);
  if (!b.targetRestrictOnlyAllow.empty()) ParseCategory(L, b.targetRestrictOnlyAllow.c_str(), w->onlyAllow);
}

static bool HasBlip(const Unit* t, int a) { return ArmyHasBlip(t, a); }

// ---- target acquisition ----------------------------------------------------------------------------

namespace {

float WeaponReach(const UnitWeapon* w) { return std::max(MaxR(w), w->bp->trackingRadius * MaxR(w)); }

float MaxWeaponRange(const Unit* u) {  // CAiAttackerImpl::GetMaxWeaponRange 0x5d6e80
  float r = 0;
  for (const UnitWeapon* w : u->weapons)
    if (w->enabled && !w->bp->manualFire) r = std::max(r, WeaponReach(w));
  return r;
}

// Unit::GetBlipsInRange 0x6ad060: enemies (and neutrals) its army has a blip on, within reach.
const std::vector<Unit*>& BlipsInRange(Sim& sim, Unit* u, int period) {
  if (u->blipCacheValid && sim.tick() - u->blipCacheTick < static_cast<uint32_t>(period)) return u->blipCache;
  u->blipCacheValid = true;
  u->blipCacheTick = sim.tick();
  u->blipCache.clear();
  float R = MaxWeaponRange(u);
  // FAF patch 0x128f426: wider while the head command's type has bit 0x10 or 0x20 (types 16..63)
  if (!u->commands.empty() && (static_cast<int>(u->commands.front()->type) & 0x30) != 0)
    R = std::max(R, CB(u).guardScanRadius);
  if (R <= 0 || !u->army) return u->blipCache;
  int a = u->army->index - 1;
  Vec3 p = u->position;
  sim.ForUnitsInRect(p.x - R, p.z - R, p.x + R, p.z + R, [&](Unit* t) {
    if (t->dead || t->destroyQueued || !t->army) return;
    if (Relation(u->army, t->army) == 2) return;
    if (Dist2XZ(p, t->position) > R * R) return;
    if (a < 0 || a >= 16 || !HasBlip(t, a)) return;
    u->blipCache.push_back(t);
  });
  std::sort(u->blipCache.begin(), u->blipCache.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  return u->blipCache;
}

// AI_TestForTerrainBlockage 0x5d6490: terrain between the unit and the target point.
bool TerrainBlocked(Sim& sim, Unit* u, Vec3 target, int arc) {
  if (u->layer == "Air") return false;
  const TerrainMap* m = sim.map();
  if (!m) return false;
  Vec3 a = u->position;
  a.y += CB(u).sizeZ * 2.0f;
  Vec3 b = target;
  b.y += 0.25f;
  float d;
  Vec3 h;
  if (arc == 0) return TerrainSegmentHit(m, a, b, &d, &h);
  Vec3 step = Mul(Sub(b, a), 0.25f);
  float sl = Len(step);
  const float prof[4] = {0.707f, 0.293f, -0.293f, -0.707f};
  float k = arc == 1 ? 0.5f : 2.0f;
  Vec3 cur = a;
  for (int i = 0; i < 4; ++i) {
    Vec3 nxt{cur.x + step.x, cur.y + step.y + prof[i] * sl * k, cur.z + step.z};
    if (TerrainSegmentHit(m, cur, nxt, &d, &h)) return true;
    cur = nxt;
  }
  return false;
}

bool Blacklisted(const UnitWeapon* w, const Entity* e) {
  for (const auto& b : w->blacklist)
    if (b.id == EntityRef(e)) return true;
  return false;
}

// IsTargetExempt 0x5d7340: reclaim / capture targets are never shot.
bool TargetExempt(const Unit* u, const Entity* e) {
  if (!e) return false;
  for (const auto& c : u->commands)
    if ((c->type == CommandType::Reclaim || c->type == CommandType::Capture) && c->targetId == EntityRef(e)) return true;
  return false;
}

bool InMap(Sim& sim, Vec3 p) {
  const TerrainMap* m = sim.map();
  if (!m) return true;
  return p.x >= 0 && p.z >= 0 && p.x <= m->width() && p.z <= m->height();
}

// CAiAttackerImpl::FindBestEnemy 0x5d7a10
Unit* FindBestEnemy(Sim& sim, UnitWeapon* w, const std::vector<Unit*>& list, float range, bool useAngle) {
  if (list.empty()) return nullptr;
  Unit* u = w->unit;
  int a = u->army ? u->army->index - 1 : -1;
  Unit* best = nullptr;
  float bestScore = std::numeric_limits<float>::infinity();
  int bestStatus = 3, bestPrio = 9999;
  Vec3 P = u->position;
  Vec3 fwd = WeaponForward(w);
  Entity* cur = TargetEntity(sim, w->target);
  const int n = static_cast<int>(w->priorities.size());
  for (Unit* e : list) {
    if (!Alive(e)) continue;
    float d2 = Dist2XZ(P, e->position);
    if (d2 > range * range) continue;
    if (!IsEnemy(u->army, e->army)) continue;
    const CombatBpData& ecb = CB(e);
    if (ecb.benign) continue;
    if (e->parentId) {
      if (e->layer == "Air" || e->beingBuilt) continue;
    } else if (e->layer == "Air" && e->beingBuilt) {
      continue;
    }
    if (Blacklisted(w, e) || TargetExempt(u, e) || e->unitStates.count("DoNotTarget")) continue;
    if (e->layer != "Air" && !InMap(sim, e->position)) continue;
    if (!Targetable(w, e)) continue;
    int s = SolutionStatus(w, e->position, &d2);
    if (s == 1 || s == 2) {
      if (!(IsMobile(u) && (w->bp->autoInitiateAttackCommand || w->bp->slavedToBody))) continue;
    }
    if (TerrainBlocked(sim, u, e->position, w->bp->ballisticArc)) continue;
    if (CB(u).needUnpack && s != 0) continue;
    float score = useAngle ? Dot(Norm(Sub(P, e->position)), fwd) : d2;
    if (s != 0 && !w->bp->autoInitiateAttackCommand) score = useAngle ? score + 4 : score * 4;
    bool seen = a >= 0 && a < 16 && (e->recon[a] & 0x10);
    int prio = 9999;
    for (int i = 0; i < n; ++i) {
      if (i > bestPrio) break;
      if (s > bestStatus && best) break;
      if (!InSet(w->priorities[static_cast<size_t>(i)], e->blueprint)) continue;
      if (seen) prio = i;
      bool take = prio < bestPrio || (!cur && score < bestScore) ||
                  (cur && cur != best && (score < bestScore || cur == e));
      if (take) {
        best = e;
        bestScore = score;
        bestStatus = s;
        bestPrio = prio;
      }
    }
  }
  return best;
}

UnitWeapon* PrimaryWeapon(Unit* u) { return u->weapons.empty() ? nullptr : u->weapons[0]; }
// GetTargetWeapon 0x5d6dc0
UnitWeapon* TargetWeapon(Sim& sim, Unit* u, const AiTarget& t) {
  for (UnitWeapon* w : u->weapons)
    if (w->bp && CanAttackTarget(sim, w, t)) return w;
  return nullptr;
}

// LeashOrInvisible 0x5d8ad0 (the leash part): while Attacking, the unit is more than GuardReturnRadius (3D)
// from its leash point - the guarded unit's position, else u+0x4e8 - when there is one
bool LeashOrInvisible(Sim& sim, Unit* u) {
  if (!u->unitStates.count("Attacking")) return false;
  Vec3 leash = u->leashPos;
  if (u->guardedId)
    if (Entity* g = sim.FindEntity(u->guardedId)) leash = g->position;
  if (leash.x == 0 && leash.y == 0 && leash.z == 0) return false;
  float dx = u->position.x - leash.x, dy = u->position.y - leash.y, dz = u->position.z - leash.z;
  float r = CB(u).guardReturnRadius;
  return dx * dx + dy * dy + dz * dz > r * r;
}

void ReportState(Unit* u, int code) {
  if (u->attackState != code) {
    u->attackState = code;
    u->attackStateSignal = true;
  }
}

// CAcquireTargetTask::TaskTick 0x5d8d10. Returns the ticks until it runs again.
int AcquireTick(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  const WeaponBp& b = *w->bp;
  const CombatBpData& cb = CB(u);
  int period = AcquirePeriod(w);
  int ret = period;
  if (cb.needUnpack && (u->unitStates.count("Moving") || u->unitStates.count("TransportLoading") ||
                        u->unitStates.count("WaitingForTransport")))
    return ret;
  if (u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y || u->position.z != u->lastPosition.z)
    w->blacklist.clear();
  if (u->beingBuilt || !w->enabled) return ret;
  UnitWeapon* prim = PrimaryWeapon(u);
  if (b.stopOnPrimaryWeaponBusy && prim && prim != w && HasTarget(sim, prim->target) &&
      CanAttackTarget(sim, prim, prim->target)) {
    SetTarget(sim, w, AiTarget{});
    return ret;
  }
  if (b.prefersPrimaryWeaponTarget && prim && prim != w && HasTarget(sim, prim->target) &&
      CanAttackTarget(sim, w, prim->target)) {
    Vec3 p = TargetPos(sim, prim->target, false);
    if (!IsNaN(p) && SolutionStatus(w, p, nullptr) == 0) {
      SetTarget(sim, w, prim->target);
      return ret;
    }
  }
  // the commanded target, and the state code for the attack command
  const AiTarget& d = u->desiredTarget;
  bool dHas = HasTarget(sim, d);
  UnitWeapon* tw = dHas ? TargetWeapon(sim, u, d) : nullptr;
  bool reporter = (dHas && tw) ? (w == tw) : (w == prim);
  int tc = dHas ? TargetStatus(sim, w, d) : 3;
  int code;
  if (!dHas) code = 4;
  else if (LeashOrInvisible(sim, u) || TargetExempt(u, TargetEntity(sim, d))) code = 5;
  else if (w->suppress != 0 || !w->canReach) code = 6;
  else if (tc == 0 && !CanAttackTarget(sim, w, d)) code = 3;
  else if (!cb.canFly && tc != 0) code = tc == 1 ? 7 : 2;
  else if (d.type == 2 && w->shots >= b.attackGroundTries && u->commands.size() > 1) code = 8;
  else if (CanAttackTarget(sim, w, d)) {
    SetTarget(sim, w, d);
    code = 1;
  } else {
    code = 0;
  }
  if (reporter) ReportState(u, code);
  if (w->suppress > 0) --w->suppress;
  if (u->unitStates.count("Attacking") && (b.autoInitiateAttackCommand || cb.needUnpack || code == 1)) return 1;
  // keep the current target
  if (!b.alwaysRecheckTarget && TargetStatus(sim, w, w->target) == 0 && CanAttackTarget(sim, w, w->target) &&
      HasTarget(sim, w->target) && !TargetExempt(u, TargetEntity(sim, w->target)))
    return ret;
  if (u->fireState == 1) {
    SetTarget(sim, w, AiTarget{});
    return ret;
  }
  float maxR = MaxR(w);
  if (b.targetType == 1) {
    // anti-projectile weapons (TrackToTarget 0x5d8000)
    Projectile* bestP = nullptr;
    if (CheckSilo(w) && !u->stunned && !u->unitStates.count("Busy")) {
      float reach = WeaponReach(w), bestD = std::numeric_limits<float>::infinity();
      float wl = WaterLevel(sim);
      for (Projectile* p : sim.projectiles) {
        if (!Alive(p) || !IsEnemy(u->army, p->army)) continue;
        float d2 = Dist2XZ(u->position, p->position);
        if (d2 > reach * reach) continue;
        if (!((w->layerCaps & 8) || p->position.y > wl)) continue;
        if (!((w->layerCaps & 16) || p->position.y <= wl)) continue;
        if (!Targetable(w, p)) continue;
        if (SolutionStatus(w, p->position, nullptr) != 0) continue;
        int cap = 3;
        if (p->blueprint) {
          lua_State* L = sim.L();
          int top = lua_gettop(L);
          sim.blueprints().PushTable(L, *p->blueprint);
          cap = static_cast<int>(lu::Num(L, lua_gettop(L), "DesiredShooterCap", 3));
          lua_settop(L, top);
        }
        if (p->shooters >= cap && TargetEntity(sim, w->target) != p) continue;
        if (d2 < bestD) {
          bestD = d2;
          bestP = p;
        }
      }
    }
    if (bestP) SetTarget(sim, w, EntityTarget(bestP));
    else if (dHas) return ret;
    else SetTarget(sim, w, AiTarget{});
    return ret;
  }
  if (maxR <= 0) return ret;
  float range = WeaponReach(w);
  bool useAngle = b.turreted || b.slavedToBody;
  const std::vector<Unit*>& list = BlipsInRange(sim, u, period);
  Unit* best = FindBestEnemy(sim, w, list, range, useAngle);
  if (best) {
    SetTarget(sim, w, EntityTarget(best));
  } else if (dHas && CanAttackTarget(sim, w, d)) {
    SetTarget(sim, w, d);
  } else {
    SetTarget(sim, w, AiTarget{});
  }
  return ret;
}

// CFireWeaponTask::Execute 0x6d3dc0
void FireTick(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  if (w->fireClock != 0) --w->fireClock;
  if (!w->enabled || w->bp->manualFire) return;
  if (w->fireClock != 0 || u->fireState == 1) return;
  if (!HasTarget(sim, w->target)) return;
  if (!CanAttackTarget(sim, w, w->target)) return;
  if (!CanFire(sim, w)) return;
  if (!CheckSilo(w)) return;
  if (TargetStatus(sim, w, w->target) != 0) return;
  if (w->bp->cannotAttackGround && w->target.type == 2) return;
  // UnitWeapon::Fire 0x6d61f0
  if (u->stunned) return;
  WeaponCall(sim, w, "OnFire");
  ++g_fires;
  ++w->shots;
  float rof = RoF(w);
  w->fireClock = rof > 0 ? static_cast<int>(std::nearbyint(10.0f / rof)) : 0;
}

}  // namespace

void WeaponsTick(Sim& sim) {
  const auto& units = sim.units();
  static const bool dbg = getenv("MOHO64_DEBUG_COMBAT") != nullptr;
  if (dbg && sim.tick() % 300 == 0) {
    int withTarget = 0, weapons = 0;
    for (Unit* u : units)
      for (UnitWeapon* w : u->weapons)
        if (w->bp && !u->dead) {
          ++weapons;
          if (w->target.type) ++withTarget;
        }
    int alive = 0;
    for (Unit* u : units)
      if (!u->dead && !u->destroyQueued) ++alive;
    Logf(LogLevel::Debug, "moho64: tick %u units %d", sim.tick(), alive);
    Logf(LogLevel::Debug, "moho64: tick %u combat: weapons %d with target %d, fires %llu, projectiles %llu (live %zu), impacts %llu",
         sim.tick(), weapons, withTarget, static_cast<unsigned long long>(g_fires),
         static_cast<unsigned long long>(g_projectiles), sim.projectiles.size(), static_cast<unsigned long long>(g_impacts));
  }
  for (size_t i = 0; i < units.size(); ++i) {
    Unit* u = units[i];
    if (u->destroyQueued || u->combatGone) continue;
    for (size_t k = 0; k < u->weapons.size(); ++k) {
      UnitWeapon* w = u->weapons[k];
      if (!w->bp || !w->HasLuaObject()) continue;
      FireTick(sim, w);
      if (u->destroyQueued || u->combatGone) break;
      if (!w->bp->manualFire && sim.tick() >= w->nextAcquire) {
        int r = AcquireTick(sim, w);
        w->nextAcquire = sim.tick() + static_cast<uint32_t>(std::max(1, r));
      }
    }
  }
}

// SetDesiredTarget 0x5d75b0
static void SetDesiredTarget(Sim& sim, Unit* u, const AiTarget& t) {
  u->desiredTarget = t;
  bool has = HasTarget(sim, t);
  for (UnitWeapon* w : u->weapons) {
    if (!w->bp) continue;
    SetTarget(sim, w, AiTarget{});
    if (w->bp->needPrep && has) WeaponCall(sim, w, "OnGotTarget");
    if (t.type != 0) w->nextAcquire = sim.tick();
    else if (w->bp->autoInitiateAttackCommand) w->nextAcquire = sim.tick() + 2;
    else w->nextAcquire = sim.tick() + static_cast<uint32_t>(AcquirePeriod(w));
  }
  ReportState(u, 0);
}

// ---- aim controllers ------------------------------------------------------------------------------

namespace {

// CheckTracking 0x6309f0. Returns bit0 outside tolerance, bit1 heading still moving.
int CheckTracking(AimController* c, bool heading, bool raw, Vec3 goal, int bone, float tol) {
  float& cur = heading ? c->heading : c->pitch;
  float center = heading ? c->hCenter : c->pCenter, half = heading ? c->hHalf : c->pHalf,
        slew = heading ? c->hSlew : c->pSlew;
  Vec3 v = goal;
  if (!raw) {
    Vec3 bp;
    Quat bq;
    BoneWorld(c->unit, bone, &bp, &bq);
    v = Rotate(Conj(bq), goal);
  }
  float target;
  if (heading) {
    target = dmath::Atan2(v.x, v.z) + c->headingOffset;
  } else {
    // v' = Rx(center) * v, target = center - FastNegAsin(v'.y / |v'|). FastNegAsin is the
    // Abramowitz-Stegun acos polynomial minus pi/2, used for negative arguments too (no reflection),
    // so pitches below the arc centre carry its error (a 45-degree arc rests at 0.0508, not 0).
    float sc = std::sin(center * 0.5f), cc = std::cos(center * 0.5f);
    float s2 = 2 * sc * cc, c2 = cc * cc - sc * sc;
    float y = v.y * c2 - v.z * s2;
    float l = Len(v);
    if (l > 0) {
      float t = std::fmax(-1.0f, std::fmin(1.0f, y / l));
      float neg = std::sqrt(1.0f - t) * (1.5707288f - 0.2121144f * t + 0.0742610f * t * t - 0.0187293f * t * t * t) -
                  1.5707964f;
      target = center - neg;
    } else {
      target = center;
    }
  }
  float delta;
  if (half >= 3.1405928f) {
    delta = WrapPi(target - cur);
  } else {
    float rel = std::clamp(WrapPi(target - center), -half, half);
    delta = (center + rel) - cur;
  }
  float step = std::fabs(delta) > slew ? std::copysign(slew, delta) : delta;
  cur = WrapPi(cur + step);
  int bits = 0;
  if (heading && std::fabs(delta) > 0.001f) bits |= 2;
  if (!(!heading && c->weapon && c->weapon->bp && c->weapon->bp->yawOnlyOnTarget))
    if (std::fabs(WrapPi(cur - target)) > tol) bits |= 1;
  return bits;
}

void ApplyHeading(AimController* c, int bone) {
  if (bone < 0 || static_cast<size_t>(bone) >= c->unit->poseRot.size()) return;
  Quat& r = c->unit->poseRot[static_cast<size_t>(bone)];
  r = QMul(r, AxisAngle({0, 1, 0}, c->heading));
}
void ApplyPitch(AimController* c, int bone) {
  if (bone < 0 || static_cast<size_t>(bone) >= c->unit->poseRot.size()) return;
  Quat& r = c->unit->poseRot[static_cast<size_t>(bone)];
  r = QMul(r, AxisAngle({1, 0, 0}, -c->pitch));
}
void Hold(AimController* c) {
  ApplyHeading(c, c->yawBone);
  ApplyPitch(c, c->pitchBone);
}

// Track 0x630760: returns on target.
bool Track(Sim& sim, AimController* c, Vec3 goal, bool raw) {
  float tol = (c->weapon ? Tolerance(c->weapon) : 0.01f) * kDeg2Rad;
  int bits = 0;
  if (c->yawBone >= 0 && c->yawBone == c->pitchBone && c->yawVertical) {
    bits |= CheckTracking(c, false, raw, goal, c->pitchBone, tol);
    ApplyPitch(c, c->pitchBone);
    bits |= CheckTracking(c, true, raw, goal, c->yawBone, tol);
    ApplyHeading(c, c->yawBone);
  } else {
    if (c->yawBone >= 0) {
      bits |= CheckTracking(c, true, raw, goal, c->yawBone, tol);
      ApplyHeading(c, c->yawBone);
    }
    if (c->pitchBone >= 0) {
      bits |= CheckTracking(c, false, raw, goal, c->pitchBone, tol);
      ApplyPitch(c, c->pitchBone);
    }
  }
  if (c->weapon && c->weapon->HasLuaObject()) {
    if ((bits & 2) && !c->tracking) {
      c->tracking = true;
      lua_pushstring(sim.L(), c->label.c_str());
      sim.CallMethod(sim.L(), c->weapon, "OnStartTracking", 1);
    } else if (!(bits & 2) && c->tracking) {
      c->tracking = false;
      lua_pushstring(sim.L(), c->label.c_str());
      sim.CallMethod(sim.L(), c->weapon, "OnStopTracking", 1);
    }
  }
  return !(bits & 1);
}

// PredictInterceptPointConstantSpeed 0x6312b0 (reproduces the original's approximation)
Vec3 LeadConstantSpeed(float c, Vec3 M, Vec3 T, Vec3 V) {
  float s = Len(V);
  if (s < 0.001f) return T;
  Vec3 D = Sub(M, T);
  float d = Len(D);
  if (d * d < 1e-4f) return T;
  Vec3 dHat = Mul(D, 1 / d), vHat = Mul(V, 1 / s);
  float x = 1.5707964f - Dot(vHat, dHat);
  float k = ((x * x * 0.00761f - 0.16605f) * x * x + 1) * x;
  float b = k * d * s;
  float a = s * s - c * c;
  if (a * a < 1e-4f) return T;
  float disc = b * b - a * d * d;
  if (disc < 0) return T;
  float t = (b - std::sqrt(disc)) / a;
  return Add(T, Mul(V, t));
}
// ...FromForwardVelocity 0x631580
Vec3 LeadForwardVelocity(float mv, Vec3 Mpos, Quat Mrot, Vec3 T, Vec3 V) {
  Vec3 f = Forward(Mrot);
  float h = std::sqrt(f.x * f.x + f.z * f.z);
  if (h * mv <= 0.001f) return T;
  float inv = 1 / (h * mv);
  float t = DistXZ(Mpos, T) * inv, tn = t;
  for (int i = 1; i <= 10; ++i) {
    tn = DistXZ(Mpos, Add(T, Mul(V, t))) * inv;
    if (i == 10 || std::fabs(tn - t) <= 0.1f) break;
    t = tn;
  }
  return Add(T, Mul(V, tn));
}

// Aim 0x6317b0: the goal direction in the world (NaN: no solution).
Vec3 Aim(Sim& sim, AimController* c) {
  UnitWeapon* w = c->weapon;
  Unit* u = c->unit;
  if (c->muzzleBone < 0) return Forward(u->orientation);
  Vec3 Mpos;
  Quat Mrot;
  BoneWorld(u, c->muzzleBone, &Mpos, &Mrot);
  if (IsMobile(u)) Mpos = Add(Mpos, u->motion.lastMove);
  Entity* te = TargetEntity(sim, w->target);
  Vec3 tVel = te && !te->dead ? EntityVelocity(te) : Vec3{};
  Vec3 tPos = TargetPos(sim, w->target, false);
  if (IsNaN(tPos)) return NaNVec();
  if (te) {  // FAF: the target has not moved yet this tick
    Entity* m = te;
    if (te->kind == Entity::Kind::Unit && static_cast<Unit*>(te)->parentId)
      if (Entity* p = sim.FindEntity(static_cast<Unit*>(te)->parentId)) m = p;
    if (m->lastMoveTick != sim.tick()) tPos = Add(tPos, tVel);
  }
  float hDist = DistXZ(tPos, Mpos);
  float mv = w->bp->muzzleVelocity;
  if (hDist < w->bp->muzzleVelocityReduceDistance) mv *= std::sqrt(hDist / w->bp->muzzleVelocityReduceDistance);
  bool phys = w->projBp != nullptr;
  if (w->bp->leadTarget && w->target.type == 1) {
    Vec3 v10 = Mul(tVel, 10);
    if (phys && c->tracksTarget) tPos = LeadConstantSpeed(c->projMaxSpeed, Mpos, tPos, v10);
    else if (phys && c->usesGravity) tPos = LeadForwardVelocity(mv, Mpos, Mrot, tPos, v10);
    else tPos = LeadConstantSpeed(mv, Mpos, tPos, v10);
  }
  if (phys && !c->tracksTarget && c->usesGravity) {
    float d = DistXZ(tPos, Mpos), dy = tPos.y - Mpos.y;
    const float g = -4.9f;
    if (mv <= 0) return NaNVec();
    float A = (-d * d * g) / (mv * mv * 2);
    if (A <= 0) return Norm(Sub(tPos, Mpos));
    float disc = d * d - (dy + A) * A * 4;
    if (disc < 0) return NaNVec();
    float high = -dmath::Atan((std::sqrt(disc) + d) / (2 * A));
    float low = -dmath::Atan((d - std::sqrt(disc)) / (2 * A));
    float p = w->bp->ballisticArc == 2 ? high : low;
    Vec3 hz{tPos.x - Mpos.x, 0, tPos.z - Mpos.z};
    hz = Norm(hz);
    return {hz.x * dmath::Cos(p), -dmath::Sin(p), hz.z * dmath::Cos(p)};
  }
  return Norm(Sub(tPos, Mpos));
}

// MoveManipulator 0x630db0
void MoveManipulator(Sim& sim, AimController* c) {
  Unit* u = c->unit;
  UnitWeapon* w = c->weapon;
  if (u->beingBuilt) return;
  bool straight = w && w->bp && w->bp->aimsStraightOnDisable;
  if (!c->enabled && !straight) return;
  if (!w || !w->HasLuaObject() || u->dead || u->stunned) {
    c->onTarget = false;
    Hold(c);
    return;
  }
  w->canReach = true;
  w->solution = NaNVec();
  const AiTarget& tgt = w->target;
  if (tgt.type == 0 || (!c->enabled && straight)) {
    if (c->resetCountdown > 0) {
      --c->resetCountdown;
      Hold(c);
    } else {
      Track(sim, c, {0, 0, 1}, true);
    }
    c->onTarget = false;
  } else if (!HasTarget(sim, tgt)) {
    c->onTarget = false;
    Hold(c);
  } else {
    c->resetCountdown = c->resetPoseTicks > 0 ? c->resetPoseTicks : AcquirePeriod(w);
    Vec3 dir = Aim(sim, c);
    if (!IsNaN(dir)) {
      c->onTarget = Track(sim, c, dir, false);
      w->solution = dir;
    } else {
      w->canReach = false;
      c->onTarget = false;
      Hold(c);
    }
  }
  if (strcasecmp(w->fireControl.c_str(), c->label.c_str()) == 0) w->onTarget = c->onTarget;
}

}  // namespace

void UnitAimTick(Sim& sim, Unit* u) {
  if (u->aimControllers.empty() && u->rotators.empty() && u->builderArms.empty()) return;
  if (!u->skeleton) return;
  u->poseRot.assign(static_cast<size_t>(u->skeleton->Count()), Quat{});
  RotatorsTick(u);
  for (BuilderArm* a : u->builderArms) {  // the arm's angles from this beat's MoveManipulator
    if (!a->alive || !a->enabled) continue;
    if (a->yawBone >= 0 && static_cast<size_t>(a->yawBone) < u->poseRot.size())
      u->poseRot[static_cast<size_t>(a->yawBone)] = QMul(u->poseRot[static_cast<size_t>(a->yawBone)], AxisAngle({0, 1, 0}, a->heading));
    if (a->pitchBone >= 0 && static_cast<size_t>(a->pitchBone) < u->poseRot.size())
      u->poseRot[static_cast<size_t>(a->pitchBone)] = QMul(u->poseRot[static_cast<size_t>(a->pitchBone)], AxisAngle({1, 0, 0}, -a->pitch));
  }
  for (size_t i = 0; i < u->aimControllers.size(); ++i) {
    AimController* c = u->aimControllers[i];
    if (c->alive) MoveManipulator(sim, c);
  }
}

namespace {

AimController* CheckAim(lua_State* L) { return CheckObject<AimController>(L, 1); }

int ResolveBoneArg(lua_State* L, Unit* u, int idx) {
  if (lua_isnoneornil(L, idx)) return -1;
  if (lua_type(L, idx) == LUA_TNUMBER) {
    int b = static_cast<int>(lua_tonumber(L, idx));
    if (b < -1 || (u->skeleton ? b >= u->skeleton->Count() : b > 0)) luaL_error(L, "Invalid bone index %d", b);
    return b;
  }
  const char* n = luaL_checkstring(L, idx);
  int b = u->skeleton ? u->skeleton->Find(n) : -1;
  if (b < 0) luaL_error(L, "Invalid bone name \"%s\"", n);
  return b;
}

// CreateAimController(weapon, label, yawBone, [pitchBone], [muzzleBone]) 0x631fa0
int l_CreateAimController(lua_State* L) {
  UnitWeapon* w = CheckObject<UnitWeapon>(L, 1);
  const char* label = luaL_checkstring(L, 2);
  Unit* u = w->unit;
  if (!u->skeleton) return luaL_error(L, "Unit has no skeleton.");
  int yaw = ResolveBoneArg(L, u, 3), pitch = ResolveBoneArg(L, u, 4), muzzle = ResolveBoneArg(L, u, 5);
  w->aimBone = muzzle >= 0 ? muzzle : (pitch >= 0 ? pitch : yaw);
  auto c = std::make_unique<AimController>();
  c->weapon = w;
  c->unit = u;
  c->label = label;
  c->yawBone = yaw;
  c->pitchBone = pitch;
  c->muzzleBone = w->aimBone;
  if (yaw >= 0) {
    Quat q = u->skeleton->bones()[static_cast<size_t>(yaw)].modelRot;
    Vec3 f = Forward(q);
    c->hCenter = dmath::Atan2(f.x, f.z) + (w->bp ? w->bp->headingArcCenter : 0) * kDeg2Rad;
    c->hHalf = (w->bp ? w->bp->headingArcRange : 180) * kDeg2Rad;
    c->yawVertical = std::fabs(f.y) > 0.707f;
  }
  w->onTarget = false;
  if (w->projBp) {
    Sim& sim = *S(L);
    int top = lua_gettop(L);
    sim.blueprints().PushTable(L, *w->projBp);
    int ph = lu::Sub(L, lua_gettop(L), "Physics");
    c->usesGravity = lu::Bool(L, ph, "UseGravity", true);
    c->tracksTarget = lu::Bool(L, ph, "TrackTarget");
    c->projMaxSpeed = lu::Num(L, ph, "MaxSpeed", 0);
    lua_settop(L, top);
  }
  AimController* raw = c.get();
  u->aimControllers.push_back(raw);
  w->aimControllers.push_back(raw);
  CreateObject(L, raw, "CAimManipulator");
  S(L)->Own(std::move(c));
  return 1;
}

int l_aim_SetFiringArc(lua_State* L) {
  AimController* c = CheckAim(L);
  float a[6];
  for (int i = 0; i < 6; ++i) a[i] = static_cast<float>(luaL_checknumber(L, i + 2)) * kDeg2Rad;
  c->hCenter = WrapPi((a[0] + a[1]) / 2);
  c->hHalf = std::fabs(a[1] - a[0]) / 2;
  c->hSlew = a[2] * 0.1f;
  c->pCenter = WrapPi((a[3] + a[4]) / 2);
  c->pHalf = std::fabs(a[4] - a[3]) / 2;
  c->pSlew = a[5] * 0.1f;
  return 0;
}
int l_aim_SetResetPoseTime(lua_State* L) {
  CheckAim(L)->resetPoseTicks = static_cast<int>(luaL_checknumber(L, 2) * 10);
  return 0;
}
int l_aim_OnTarget(lua_State* L) {
  lua_pushboolean(L, CheckAim(L)->onTarget);
  return 1;
}
int l_aim_SetEnabled(lua_State* L) {
  AimController* c = CheckAim(L);
  c->enabled = lua_toboolean(L, 2) != 0;
  c->onTarget = false;
  return 0;
}
int l_aim_GetHeadingPitch(lua_State* L) {
  AimController* c = CheckAim(L);
  lua_pushnumber(L, c->heading);
  lua_pushnumber(L, c->pitch);
  return 2;
}
int l_aim_SetHeadingPitch(lua_State* L) {
  AimController* c = CheckAim(L);
  c->heading = static_cast<float>(luaL_checknumber(L, 2));
  c->pitch = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
int l_aim_SetAimHeadingOffset(lua_State* L) {
  CheckAim(L)->headingOffset = static_cast<float>(luaL_checknumber(L, 2)) * kDeg2Rad;
  return 0;
}
int l_aim_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* c = dynamic_cast<AimController*>(o)) {
    c->alive = false;
    auto& v = c->unit->aimControllers;
    v.erase(std::remove(v.begin(), v.end(), c), v.end());
    if (c->weapon) {
      auto& wv = c->weapon->aimControllers;
      wv.erase(std::remove(wv.begin(), wv.end(), c), wv.end());
      c->weapon->onTarget = true;
    }
  }
  if (o) o->UnbindLua();
  return 0;
}
int l_aim_SetPrecedence(lua_State* L) {
  lua_settop(L, 1);
  return 1;
}

}  // namespace

// ---- unit kill clean-up and release ---------------------------------------------------------------

namespace {
// CAiAttackerImpl::WeaponsOnDestroy 0x5d6d30, then the attacker and its weapons are deleted.
void DestroyWeapons(Sim& sim, Unit* u) {
  if (u->combatGone) return;
  u->combatGone = true;
  for (UnitWeapon* w : u->weapons)
    if (w->HasLuaObject()) sim.CallMethod(sim.L(), w, "OnDestroy", 0);
  for (UnitWeapon* w : u->weapons) {
    if (w->bp)
      if (Entity* e = TargetEntity(sim, w->target))
        if (e->shooters > 0) --e->shooters;
    w->target = AiTarget{};
    w->UnbindLua();
  }
}
}  // namespace

// Unit::KillCleanup 0x6a8790: the beat after a kill, the unit's weapons and commands go.
void KillCleanupTick(Sim& sim) {
  const auto& units = sim.units();
  for (size_t i = 0; i < units.size(); ++i) {
    Unit* u = units[i];
    if (!u->killCleanup) continue;
    u->killCleanup = false;
    DestroyWeapons(sim, u);
    ForgetUnitCommands(u);
  }
}

void ReleaseUnitCombat(Sim& sim, Unit* u) {
  DestroyWeapons(sim, u);
  for (AimController* c : u->aimControllers) {
    c->alive = false;
    c->UnbindLua();
  }
  u->aimControllers.clear();
  u->combatGone = true;
}

// ---- attack commands -------------------------------------------------------------------------------

namespace {

AiTarget CommandTarget(Sim& sim, const BuildTask& t) {
  AiTarget a;
  if (t.goalId) {
    a.type = 1;
    a.entityId = t.goalId;
    if (Entity* e = sim.FindEntity(t.goalId))
      if (e->kind == Entity::Kind::Unit) a.mobile = IsMobile(static_cast<Unit*>(e));
  } else {
    a.type = 2;
    a.pos = t.site;
  }
  return a;
}

bool WithinWeaponRange(Sim& sim, UnitWeapon* w, const AiTarget& t) {
  return w->enabled && CanAttackTarget(sim, w, t) && TargetStatus(sim, w, t) == 0;
}

}  // namespace

// The guard task's enemy (GetBestEnemy 0x612af0): the primary weapon's best enemy among the unit's blips
// within GuardScanRadius of the unit (score = distance; no angle). The blip cache is read as the weapons
// last built it (not refreshed here).
float GuardScanRadiusOf(Unit* u) { return CB(u).guardScanRadius; }
Unit* GuardBestEnemy(Sim& sim, Unit* u) {
  UnitWeapon* w = PrimaryWeapon(u);
  if (!w || !w->bp) return nullptr;
  float r = CB(u).guardScanRadius;
  Unit* best = FindBestEnemy(sim, w, u->blipCache, r, false);
  if (best && DistXZ(best->position, u->position) > r) return nullptr;
  return best;
}

Unit* PatrolFindTarget(Sim& sim, Unit* u, const PatrolBox& box) {
  UnitWeapon* w = PrimaryWeapon(u);
  if (!w || !w->bp) return nullptr;
  std::vector<Unit*> cand;
  for (Unit* e : u->blipCache) {  // as the weapons last built it (not refreshed here)
    if (!e || e->dead || e->destroyQueued) continue;
    WorldShape s;
    if (!GetWorldShape(e, &s)) continue;
    // the shape against the box (it is 200 high, so only xz matters): the shape's horizontal reach
    float reach = s.type == ShapeType::Sphere ? s.r : std::max(s.half.x, s.half.z);
    float rx = s.c.x - box.cx, rz = s.c.z - box.cz;
    float along = std::fabs(rx * box.dx + rz * box.dz), across = std::fabs(-rx * box.dz + rz * box.dx);
    if (along > box.along + reach || across > box.side + reach) continue;
    cand.push_back(e);
  }
  float r = CB(u).guardScanRadius;
  Unit* best = FindBestEnemy(sim, w, cand, r, false);
  if (best && DistXZ(best->position, u->position) > r) return nullptr;
  return best;
}

// ---- CUnitAttackTargetTask (engine-ref attack_move.md 3) ---------------------------------------------

struct AttackData {
  Vec3 lastPos;              // +0x80: the target's position at the last goal update (zero: none)
  bool trackMobile = false;  // +0x8c: the target is a MOBILE unit
  bool coordinated = false;  // +0x8d: FormAttack
  bool firstUpdate = true;   // +0x8e
  bool listening = false;    // the attacker-event listener is linked (from the first UpdateAttacker on)
  int result = 0;            // 1 success, 2 failure (the listener's result)
};

namespace {

bool IsZeroV(const Vec3& v) { return v.x == 0 && v.y == 0 && v.z == 0; }
// !Unit::WontFitAt 0x62aa90: the unit's footprint fits at world position p
bool StandableFor(Sim& sim, Unit* u, const Vec3& p) {
  const MotionBlueprint& b = *u->motion.bp;
  const PathGrid* g = FootprintGrid(sim, b);
  if (!g) return true;
  int cx, cz;
  GoalCell(b, p.x, p.z, &cx, &cz);
  return g->Passable(cx, cz);
}
bool CanFlyU(const Unit* u) { return u->motion.bp && u->motion.bp->canFly; }

void InitAttack(Sim& sim, Unit* u, BuildTask& t, bool coordinated) {
  auto d = std::make_shared<AttackData>();
  d->coordinated = coordinated;
  t.adata = d;
  t.state = 0;
  u->unitStates.insert("Attacking");
  if (!coordinated) u->navIgnoreFormation = true;
  AiTarget target = CommandTarget(sim, t);
  if (target.type == 1)
    if (Entity* e = sim.FindEntity(target.entityId))
      d->trackMobile = e->kind == Entity::Kind::Unit && BpInCategory(sim, e->blueprint, "MOBILE");
  if (HasTarget(sim, target)) d->lastPos = TargetPos(sim, target, false);  // UpdatePos
  if (IsZeroV(d->lastPos)) d->lastPos = u->position;
  if (CanFlyU(u)) d->firstUpdate = false;
}

// CAiAttackerImpl::VectorIsWithinAttackRange 0x5d70e0: any enabled weapon reaches p (2D)
bool VectorInAttackRange(Unit* u, const Vec3& p) {
  for (UnitWeapon* w : u->weapons)
    if (w->bp && w->enabled && DistXZ(u->position, p) <= MaxR(w)) return true;
  return false;
}

void AttackSetGoalRect(Sim& sim, Unit* u, int x0, int z0, int x1, int z1) {
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {
    TaskMoveToward(sim, u, Vec3{(x0 + x1) * 0.5f, u->position.y, (z0 + z1) * 0.5f});
    return;
  }
  u->motion.failed = false;
  LandNavSetGoalRect(sim, u, x0, z0, x1, z1, false);
  u->unitStates.insert("Moving");
}
// a 1x1 goal at the footprint-origin cell of p
void AttackSetPosGoal(Sim& sim, Unit* u, const Vec3& p) {
  const MotionBlueprint& b = *u->motion.bp;
  int cx = static_cast<int>(std::nearbyint(p.x - b.footprint.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(p.z - b.footprint.sizeZ * 0.5f));
  AttackSetGoalRect(sim, u, cx, cz, cx + 1, cz + 1);
}
// CAiNavigatorLand::SetDestUnit 0x5a4180: a 1x1 goal at (fistp(e.x - 0.5), fistp(e.z - 0.5))
void AttackSetDestUnit(Sim& sim, Unit* u, const Entity* e) {
  int cx = static_cast<int>(std::nearbyint(e->position.x - 0.5f));
  int cz = static_cast<int>(std::nearbyint(e->position.z - 0.5f));
  AttackSetGoalRect(sim, u, cx, cz, cx + 1, cz + 1);
}

// Update 0x5f3020 (no formation)
void AttackUpdate(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  bool has = HasTarget(sim, target);
  if (has) d.lastPos = TargetPos(sim, target, false);
  if (IsZeroV(d.lastPos)) d.lastPos = u->position;
  UnitWeapon* w = d.firstUpdate ? TargetWeapon(sim, u, target) : nullptr;
  Entity* e = target.type == 1 ? sim.FindEntity(target.entityId) : nullptr;
  if (w) {
    // SetWeaponGoal 0x5f2ce0: a square of side trunc(MaxRadius) around the target's aim point
    Vec3 p = TargetPos(sim, target, true);
    int R = static_cast<int>(w->bp->maxRadius);
    int x0 = static_cast<int>(std::nearbyint(p.x - R * 0.5f)), z0 = static_cast<int>(std::nearbyint(p.z - R * 0.5f));
    if (R < 1) AttackSetPosGoal(sim, u, p);
    else AttackSetGoalRect(sim, u, x0, z0, x0 + R, z0 + R);
  } else if (d.trackMobile && has && e && !e->dead && !e->destroyQueued) {
    AttackSetDestUnit(sim, u, e);
  } else {
    AttackSetPosGoal(sim, u, has ? TargetPos(sim, target, true) : d.lastPos);
  }
  d.firstUpdate = false;
}

// UpdateAttacker 0x5f3450
void UpdateAttacker(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  if (u->weapons.empty()) return;
  if (!(u->desiredTarget.type == target.type && u->desiredTarget.entityId == target.entityId &&
        (target.type != 2 || u->desiredTarget == target))) {
    SetDesiredTarget(sim, u, target);
    t.adata->listening = true;
    u->attackStateSignal = false;
  } else {
    u->attackState = 0;  // ResetReportingState: the next report is news
    u->attackStateSignal = false;
    t.adata->listening = true;
  }
}

void AbortNavigation(Unit* u) {
  u->navIgnoreFormation = false;
  TaskStopMoving(u);
}

// the weapon's state report (attacker-event listener 0x5f3ee0)
void AttackEvent(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  if (t.state == 5) return;
  bool mobile = TaskCanMove(u);
  if (!HasTarget(sim, target)) {
    t.state = mobile ? 3 : 5;
  } else {
    switch (u->attackState) {
      case 1: t.state = 4; break;
      case 2:
      case 4:
        if (mobile) t.state = 1;
        else { d.result = 2; t.state = 5; }
        break;
      case 6: t.state = 1; break;
      case 3:
        if (mobile) { d.result = 2; t.state = 5; }
        else t.state = 4;
        break;
      case 5: d.result = 2; t.state = 5; break;
      case 7:
        if (mobile) t.state = 2;
        else { d.result = 2; t.state = 5; }
        break;
      case 8: d.result = 1; t.state = 5; break;
      default: break;
    }
  }
  t.waitUntil = 0;  // wake
}

// TaskTick 0x5f34c0 for a ground (non-flying) attacker. Returns the original's task code.
int AttackStep(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  UnitWeapon* w = u->weapons.empty() ? nullptr : TargetWeapon(sim, u, target);
  bool direct = BpInCategory(sim, u->blueprint, "DIRECTFIRE");
  if (!HasTarget(sim, target) && u->commands.size() >= 2) return -1;  // more orders: give up at once
  if (!w && !d.coordinated && !direct) return -1;
  bool mobile = TaskCanMove(u);
  switch (t.state) {
    case 0:
      t.state = 1;
      return 0;
    case 1:
      if (!mobile) {
        UpdateAttacker(sim, u, t, target);
        t.state = 4;
        return 0;
      }
      AttackUpdate(sim, u, t, target);
      t.state = 3;
      return 0;
    case 2: {  // back off (inside MinRadius)
      if (u->weapons.empty()) return -1;
      if (w && TargetStatus(sim, w, target) == 1) {
        Vec3 tp = TargetPos(sim, target, false);
        Vec3 v = Sub(u->position, tp);
        float l = Len(v);
        float gsr = CB(u).guardScanRadius;
        Vec3 p = l > 0 ? Add(tp, Mul(v, gsr / l)) : tp;
        AttackSetPosGoal(sim, u, p);
        return 10;
      }
      AttackUpdate(sim, u, t, target);
      t.state = 3;
      return 0;
    }
    case 3: {  // chase
      bool has = HasTarget(sim, target);
      bool busy = u->motion.hasGoal;
      if (!has) {
        if (VectorInAttackRange(u, d.lastPos)) return -1;
        if (busy) return 1;
        AttackUpdate(sim, u, t, target);
        return 1;
      }
      if (w && WithinWeaponRange(sim, w, target)) {
        bool any = false;
        for (UnitWeapon* x : u->weapons) any = any || (x->bp && CanAttackTarget(sim, x, target));
        if (!any) return -1;
        u->navIgnoreFormation = true;
        UpdateAttacker(sim, u, t, target);
        return -2;  // suspended; the navigator keeps driving until the weapon reports
      }
      if (w && TargetStatus(sim, w, target) == 1) {
        t.state = 2;
        return 1;
      }
      if (!busy) {
        AttackUpdate(sim, u, t, target);
        return 1;
      }
      if (!d.trackMobile) return 1;
      Vec3 tp = TargetPos(sim, target, false);
      Vec3 dv = Sub(d.lastPos, tp);
      if (Len(dv) <= (CanFlyU(u) ? 2.0f : 10.0f)) return 1;
      Entity* e = sim.FindEntity(target.entityId);
      if (!e || e->dead || e->destroyQueued) return 1;
      if (!StandableFor(sim, u, tp)) return 1;  // UnitWontFitAt
      AttackSetDestUnit(sim, u, e);
      d.lastPos = tp;
      return 1;
    }
    case 4:  // engaged: stop, the weapons fire; the weapon's reports drive the task
      if (!CanFlyU(u)) AbortNavigation(u);
      return -2;
    case 5:
    default:
      AbortNavigation(u);
      return -1;
  }
}

}  // namespace

// new CUnitAttackTargetTask(target e) (the guard's and the patrol's child)
BuildTask* MakeAttackTaskOn(Sim& sim, Unit* u, Entity* e) {
  if (u->weapons.empty() || !e) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Attack;
  t->order = "Attack";
  t->goalId = EntityRef(e);
  t->site = e->position;
  t->child = true;
  InitAttack(sim, u, *t, false);
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

BuildTask* StartAttackTask(Sim& sim, Unit* u, const UnitCommand& c) {
  if (u->weapons.empty()) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->order = "Attack";
  t->goalId = c.targetId;
  t->site = c.pos;
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) t->site = e->position;
  InitAttack(sim, u, *t, c.type == CommandType::FormAttack);
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int TickAttack(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  (void)L;
  AiTarget target = CommandTarget(sim, t);
  bool air = u->motion.bp && u->motion.bp->motionType == kMotionAir;
  if (!air && t.adata) {
    AttackData& d = *t.adata;
    if (d.listening && u->attackStateSignal) {
      u->attackStateSignal = false;
      AttackEvent(sim, u, t, target);
    }
    if (sim.tick() < t.waitUntil) return kTaskRunning;
    for (int guard = 0; guard < 16; ++guard) {
      int r = AttackStep(sim, u, t, target);
      if (r == 0) continue;
      if (r == -1) return d.result == 2 ? kTaskFailed : kTaskDone;
      if (r == -2) t.waitUntil = 0xffffffffu;
      else t.waitUntil = sim.tick() + static_cast<uint32_t>(r >= 2 ? r - 1 : 1);
      return kTaskRunning;
    }
    return kTaskRunning;
  }
  if (!HasTarget(sim, target)) return kTaskDone;
  UnitWeapon* w = TargetWeapon(sim, u, target);
  if (!w) return kTaskDone;
  bool mobile = TaskCanMove(u);
  if (!mobile || air) {
    if (!(u->desiredTarget == target)) SetDesiredTarget(sim, u, target);
    if (!mobile) return kTaskRunning;
  }
  Vec3 tp = TargetPos(sim, target, true);
  // aircraft: attack runs - fly at the target, past it, and come round again
  float d = DistXZ(u->position, tp);
  Vec3 f = Forward(u->orientation);
  float ahead = (tp.x - u->position.x) * f.x + (tp.z - u->position.z) * f.z;
  if (ahead > 0 && d < 60) u->unitStates.insert("MakingAttackRun");
  else u->unitStates.erase("MakingAttackRun");
  if (t.state == 5) {  // flying past
    if (d > 25 || !u->motion.hasGoal) t.state = 3;
    return kTaskRunning;
  }
  if (d < 6) {
    Vec3 past{u->position.x + f.x * 30, tp.y, u->position.z + f.z * 30};
    TaskMoveToward(sim, u, past);
    t.state = 5;
    return kTaskRunning;
  }
  if (!u->motion.hasGoal || Dist2XZ(t.site, tp) > 4) {
    t.site = tp;
    TaskMoveToward(sim, u, tp);
    u->motion.passThrough = true;
  }
  return kTaskRunning;
}

void EndAttack(Sim& sim, Unit* u, BuildTask& t) {
  (void)t;
  if (u->dead || u->destroyQueued) return;
  if (u->desiredTarget.type != 0) SetDesiredTarget(sim, u, AiTarget{});
  u->navIgnoreFormation = false;
  u->unitStates.erase("Attacking");
  u->unitStates.erase("MakingAttackRun");
}

void ClearEngagement(Sim& sim, Unit* u) {
  if (!u->engageId) return;
  u->engageId = 0;
  if (!u->dead && !u->destroyQueued) {
    SetDesiredTarget(sim, u, AiTarget{});
    u->unitStates.erase("Attacking");
  }
}

// ---- intel and unit bindings ------------------------------------------------------------------------

namespace {

Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

int l_GetTargetEntity(lua_State* L) {
  Unit* u = U(L);
  Entity* e = TargetEntity(*S(L), u->desiredTarget);
  if (e) PushObject(L, e);
  else lua_pushnil(L);
  return 1;
}
int l_SetFireState(lua_State* L) {
  Unit* u = U(L);
  int s = 0;
  if (lua_type(L, 2) == LUA_TNUMBER) {
    s = static_cast<int>(lua_tonumber(L, 2));
  } else {
    const char* n = luaL_checkstring(L, 2);
    if (!strcasecmp(n, "ReturnFire")) s = 0;
    else if (!strcasecmp(n, "HoldFire")) s = 1;
    else if (!strcasecmp(n, "HoldGround")) s = 2;
    else if (!strcasecmp(n, "Mix")) s = -1;
    else s = std::atoi(n);
  }
  u->fireState = s;
  return 0;
}
int l_GetFireState(lua_State* L) {
  lua_pushnumber(L, U(L)->fireState);
  return 1;
}
int l_ToggleFireState(lua_State* L) {
  Unit* u = U(L);
  u->fireState = (u->fireState + 1) % 3;
  return 0;
}
int l_SetStunned(lua_State* L) {
  Unit* u = U(L);
  u->stunned = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_IsStunned(lua_State* L) {
  lua_pushboolean(L, U(L)->stunned);
  return 1;
}
int l_ReachedMaxShooters(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int cap = 3;
  if (e->blueprint) {
    Sim& sim = *S(L);
    int top = lua_gettop(L);
    sim.blueprints().PushTable(L, *e->blueprint);
    cap = static_cast<int>(lu::Num(L, lua_gettop(L), "DesiredShooterCap", 3));
    lua_settop(L, top);
  }
  lua_pushboolean(L, e->shooters >= cap);
  return 1;
}

// ---- UnitWeapon methods -----------------------------------------------------------------------------

UnitWeapon* W(lua_State* L) {
  UnitWeapon* w = CheckObject<UnitWeapon>(L, 1);
  if (!w->bp) InitUnitWeapon(L, w);
  return w;
}

int l_w_CanFire(lua_State* L) {
  UnitWeapon* w = W(L);
  Sim& sim = *S(L);
  bool ok = HasTarget(sim, w->target) && CanFire(sim, w) && CheckSilo(w);
  if (ok) {
    Vec3 p = TargetPos(sim, w->target, true);
    ok = !IsNaN(p) && SolutionStatus(w, p, nullptr) == 0;
  }
  lua_pushboolean(L, ok);
  return 1;
}
int l_w_ChangeDamage(lua_State* L) {
  W(L)->ovDamage = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_ChangeDamageRadius(lua_State* L) {
  W(L)->ovDamageRadius = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_ChangeDamageType(lua_State* L) {
  W(L)->ovDamageType = luaL_checkstring(L, 2);
  return 0;
}
int l_w_ChangeFiringTolerance(lua_State* L) {
  W(L)->ovFiringTolerance = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_ChangeMaxHeightDiff(lua_State* L) {
  W(L)->ovMaxHeightDiff = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_ChangeMaxRadius(lua_State* L) {
  UnitWeapon* w = W(L);
  float v = static_cast<float>(luaL_checknumber(L, 2));
  w->ovMaxRadius = v;
  w->maxR2 = v * v;
  w->unit->blipCacheValid = false;
  return 0;
}
int l_w_ChangeMinRadius(lua_State* L) {
  UnitWeapon* w = W(L);
  float v = static_cast<float>(luaL_checknumber(L, 2));
  w->ovMinRadius = v;
  w->minR2 = v * v;
  return 0;
}
int l_w_ChangeProjectileBlueprint(lua_State* L) {
  UnitWeapon* w = W(L);
  const char* id = luaL_checkstring(L, 2);
  if (*id) w->projBp = S(L)->blueprints().Find(id);
  return 0;
}
int l_w_ChangeRateOfFire(lua_State* L) {
  W(L)->ovRateOfFire = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_FireWeapon(lua_State* L) {
  UnitWeapon* w = W(L);
  if (w->unit->stunned) return 0;
  S(L)->CallMethod(L, w, "OnFire", 0);
  ++w->shots;
  return 0;
}
int l_w_GetCurrentTarget(lua_State* L) {
  UnitWeapon* w = W(L);
  Entity* e = w->target.type == 1 ? S(L)->FindEntity(w->target.entityId) : nullptr;
  if (e && e->HasLuaObject()) PushObject(L, e);
  else lua_pushnil(L);
  return 1;
}
int l_w_GetCurrentTargetPos(lua_State* L) {
  UnitWeapon* w = W(L);
  Vec3 p = TargetPos(*S(L), w->target, false);
  if (IsNaN(p)) lua_pushnil(L);
  else PushVec(L, p);
  return 1;
}
int l_w_GetFireClockPct(lua_State* L) {
  UnitWeapon* w = W(L);
  float rof = RoF(w);
  lua_pushnumber(L, rof > 0 ? 1 - w->fireClock / (10.0f / rof) : 1);
  return 1;
}
int l_w_GetFiringRandomness(lua_State* L) {
  lua_pushnumber(L, W(L)->firingRandomness);
  return 1;
}
int l_w_SetFiringRandomness(lua_State* L) {
  W(L)->firingRandomness = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_w_GetProjectileBlueprint(lua_State* L) {
  UnitWeapon* w = W(L);
  if (w->projBp) S(L)->blueprints().PushTable(L, *w->projBp);
  else lua_pushnil(L);
  return 1;
}
int l_w_IsFireControl(lua_State* L) {
  lua_pushboolean(L, strcasecmp(luaL_checkstring(L, 2), W(L)->fireControl.c_str()) == 0);
  return 1;
}
int l_w_SetFireControl(lua_State* L) {
  W(L)->fireControl = luaL_checkstring(L, 2);
  lua_settop(L, 2);
  return 1;
}
int l_w_ResetTarget(lua_State* L) {
  SetTarget(*S(L), W(L), AiTarget{});
  return 0;
}
int l_w_SetEnabled(lua_State* L) {
  UnitWeapon* w = W(L);
  w->enabled = lua_toboolean(L, 2) != 0;
  lua_settop(L, 1);
  return 1;
}
int l_w_SetFireTargetLayerCaps(lua_State* L) {
  UnitWeapon* w = W(L);
  const char* s = luaL_checkstring(L, 2);
  int mask = 0;
  std::string tok;
  auto flush = [&]() {
    if (tok.empty()) return;
    std::string t = tok;
    if (!strncasecmp(t.c_str(), "LAYER_", 6)) t = t.substr(6);
    const char* names[] = {"None", "Land", "Seabed", "Sub", "Water", "Air", "Orbit", "All"};
    const int bits[] = {0, 1, 2, 4, 8, 16, 32, 0x7f};
    bool found = false;
    for (int i = 0; i < 8; ++i)
      if (!strcasecmp(t.c_str(), names[i])) {
        mask |= bits[i];
        found = true;
      }
    if (!found) {
      char* end = nullptr;
      long v = std::strtol(t.c_str(), &end, 10);
      if (end && *end == 0) mask |= static_cast<int>(v);
      else luaL_error(L, "Invalid enum value %s", tok.c_str());
    }
    tok.clear();
  };
  for (const char* p = s; *p; ++p) {
    if (*p == '|') flush();
    else if (*p != ' ') tok += *p;
  }
  flush();
  w->layerCaps = mask;
  return 0;
}
int l_w_SetTargetEntity(lua_State* L) {
  UnitWeapon* w = W(L);
  Unit* t = CheckObject<Unit>(L, 2);
  if (Targetable(w, t)) SetTarget(*S(L), w, EntityTarget(t));
  return 0;
}
int l_w_SetTargetGround(lua_State* L) {
  UnitWeapon* w = W(L);
  AiTarget t;
  t.type = 2;
  t.pos = ReadVec(L, 2);
  if (CanAttackTarget(*S(L), w, t)) SetTarget(*S(L), w, t);
  return 0;
}
int l_w_SetTargetingPriorities(lua_State* L) {
  UnitWeapon* w = W(L);
  if (!lua_istable(L, 2)) return luaL_error(L, "SetTargetingPriorities: expected a table");
  w->priorities.clear();
  int n = static_cast<int>(luaL_getn(L, 2));
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    const uint64_t* c = ToCategory(L, -1);
    if (c) w->priorities.emplace_back(c, c + CategoryWordCount());
    lua_pop(L, 1);
  }
  return 0;
}
int l_w_TransferTarget(lua_State* L) {
  UnitWeapon* w = W(L);
  UnitWeapon* o = CheckObject<UnitWeapon>(L, 2);
  if (!o->bp) InitUnitWeapon(L, o);
  SetTarget(*S(L), o, w->target);
  return 0;
}
int l_w_WeaponHasTarget(lua_State* L) {
  lua_pushboolean(L, W(L)->target.type != 0);
  return 1;
}
int l_w_PlaySound(lua_State* L) {
  (void)L;
  return 0;
}

}  // namespace

void RegisterDamageBindings(lua_State* L);
void RegisterProjectileBindings(lua_State* L);

// ---- rotators (CRotateManipulator) ------------------------------------------------------------

void RotatorsTick(Unit* u) {
  auto& v = u->rotators;
  v.erase(std::remove_if(v.begin(), v.end(), [](RotateManipulator* r) { return !r->alive; }), v.end());
  for (RotateManipulator* r : v) {
    if (r->accel != 0) {  // the speed approaches the target speed
      float d = r->targetSpeed - r->speed, step = std::fabs(r->accel) * 0.1f;
      r->speed = std::fabs(d) <= step ? r->targetSpeed : r->speed + std::copysign(step, d);
    }
    float step = std::fabs(r->speed) * 0.1f;
    if (r->hasGoal) {
      float d = r->goal - r->cur;
      r->cur = std::fabs(d) <= step ? r->goal : r->cur + std::copysign(step, d);
    } else {
      r->cur += r->speed * 0.1f;
      if (r->cur > 360.0f || r->cur < -360.0f) r->cur = std::fmod(r->cur, 360.0f);
    }
    if (!r->enabled || r->bone < 0 || static_cast<size_t>(r->bone) >= u->poseRot.size()) continue;
    Vec3 ax{r->axis == 0 ? 1.0f : 0.0f, r->axis == 1 ? 1.0f : 0.0f, r->axis == 2 ? 1.0f : 0.0f};
    Quat& q = u->poseRot[static_cast<size_t>(r->bone)];
    q = QMul(q, AxisAngle(ax, r->cur * kDeg2Rad));
  }
}

// ---- builder arms (CBuilderArmManipulator, builder_arm.md) -----------------------------------------

namespace {

// func_NormalizeAngle 0x62fb50
float WrapAngle(float a) {
  float r = static_cast<float>(std::fmod(static_cast<double>(a), 6.283185307179586));
  if (r < -3.14159265f) r += 6.28318531f;
  else if (r > 3.14159265f) r -= 6.28318531f;
  return r;
}

// 0x50b710: acos approximation (not reflected for negative t)
float AcosA(float t) {
  return std::sqrt(1.0f - t) * (1.5707288f + t * (-0.2121144f + t * (0.0742610f + t * -0.0187293f)));
}

bool HasBuilderObject(const Unit* u) { return u->bpData && u->bpData->hasBuilder; }

// Axis 0x636220. heading: writes a->heading; pitch: a->pitch. Returns bit 1 off target, bit 2 moving.
int ArmAxis(BuilderArm* a, Vec3 dir, bool local, bool isHeading, float slew, const Quat& boneRot) {
  Vec3 d = local ? dir : Rotate(Conj(boneRot), dir);
  float center = isHeading ? a->hCenter : a->pCenter, half = isHeading ? a->hHalf : a->pHalf;
  float* cur = isHeading ? &a->heading : &a->pitch;
  float target;
  if (isHeading) {
    target = dmath::Atan2(d.x, d.z);
  } else {
    Vec3 e = Rotate(AxisAngle({1, 0, 0}, center), d);
    float len = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
    target = center - (AcosA(len > 0 ? e.y / len : 0.0f) - 1.57079637f);
  }
  if (Sim* sim = a->unit ? Sim::From(a->unit->luaState()) : nullptr; sim && sim->Recording()) {
    // labhook 0.7 'R' record (Axis 0x636220 after the target)
    unsigned char r[32];
    auto put = [&](int o, uint32_t v) { r[o] = v & 0xff; r[o + 1] = (v >> 8) & 0xff; r[o + 2] = (v >> 16) & 0xff; r[o + 3] = v >> 24; };
    auto putf = [&](int o, float f) { uint32_t v; std::memcpy(&v, &f, 4); put(o, v); };
    r[0] = 'R';
    r[1] = static_cast<unsigned char>((isHeading ? 1 : 2) | (local ? 4 : 0));
    r[2] = r[3] = 0;
    put(4, sim->RecorderTick());
    put(8, a->unit->id);
    putf(12, *cur);
    putf(16, target);
    putf(20, d.x);
    putf(24, d.y);
    putf(28, d.z);
    sim->RecorderRaw(r, 32);
  }
  float delta;
  if (half >= 3.14059281f) {
    delta = WrapAngle(target - *cur);
  } else {
    float rel = std::clamp(WrapAngle(target - center), -half, half);
    delta = rel + center - *cur;
  }
  float step = std::fabs(delta) > slew ? std::copysign(slew, delta) : delta;
  *cur = WrapAngle(*cur + step);
  int res = 0;
  if (isHeading) {
    if (std::fabs(delta) > 1e-5f) res |= 2;
    if (std::fabs(WrapAngle(*cur - target)) > 0.261799395f) res |= 1;
  }
  return res;
}

// Step 0x635fe0: returns on target.
bool ArmStep(Sim& sim, BuilderArm* a, Vec3 dir, bool local, bool slow) {
  Unit* u = a->unit;
  const int n = u->skeleton ? u->skeleton->Count() : 0;
  if (static_cast<int>(u->poseRot.size()) != n) u->poseRot.assign(static_cast<size_t>(n), Quat{});
  int r = 0;
  if (a->yawBone >= 0 && a->yawBone < n) {
    // the yaw bone's frame this tick, before this manipulator's own rotation
    u->poseRot[static_cast<size_t>(a->yawBone)] = Quat{};
    if (a->pitchBone >= 0 && a->pitchBone < n) u->poseRot[static_cast<size_t>(a->pitchBone)] = Quat{};
    Vec3 p;
    Quat q;
    BoneWorld(u, a->yawBone, &p, &q);
    {
      static const long dbg = getenv("MOHO64_DEBUG_ARM") ? atol(getenv("MOHO64_DEBUG_ARM")) : -1;
      if (dbg >= 0 && static_cast<long>(u->id) == dbg && !local) {
        Vec3 d = Rotate(Conj(q), dir);
        Logf(LogLevel::Info, "armdbg %u cur %.4f c %.4f h %.4f s %.4f dir %.4f %.4f %.4f yawq %.4f %.4f %.4f %.4f unitq %.4f %.4f %.4f %.4f local %.4f %.4f %.4f tgt %.4f",
             sim.tick(), a->heading, a->hCenter, a->hHalf, a->hSlew, dir.x, dir.y, dir.z, q.x, q.y, q.z, q.w, u->orientation.x, u->orientation.y, u->orientation.z,
             u->orientation.w, d.x, d.y, d.z, dmath::Atan2(d.x, d.z));
      }
    }
    r = ArmAxis(a, dir, local, true, a->hSlew * (slow ? 0.25f : 1.0f), q);
    u->poseRot[static_cast<size_t>(a->yawBone)] = AxisAngle({0, 1, 0}, a->heading);
  }
  if (a->pitchBone >= 0 && a->pitchBone < n) {
    Vec3 p;
    Quat q;
    BoneWorld(u, a->pitchBone, &p, &q);
    r |= ArmAxis(a, dir, local, false, a->pSlew * (slow ? 0.25f : 1.0f), q);
    u->poseRot[static_cast<size_t>(a->pitchBone)] = AxisAngle({1, 0, 0}, -a->pitch);
  }
  bool moving = (r & 2) != 0;
  if (moving && !a->tracking) sim.CallMethod(sim.L(), u, "OnStartBuilderTracking", 0);
  else if (!moving && a->tracking) sim.CallMethod(sim.L(), u, "OnStopBuilderTracking", 0);
  a->tracking = moving;
  return (r & 1) == 0;
}

}  // namespace

void BuilderArmsTick(Sim& sim, Unit* u, const Vec3& priorPos, const Quat& priorOri) {
  auto& v = u->builderArms;
  v.erase(std::remove_if(v.begin(), v.end(), [](BuilderArm* a) { return !a->alive; }), v.end());
  if (v.empty() || !HasBuilderObject(u) || u->dead) return;
  for (size_t i = 0; i < v.size(); ++i) {
    BuilderArm* a = v[i];
    if (!a->enabled || !a->alive) continue;
    const Vec3 aim = u->armAim;
    if (aim.x == 0 && aim.y == 0 && aim.z == 0 && !std::signbit(aim.x) && !std::signbit(aim.y) &&
        !std::signbit(aim.z)) {  // (bitwise compare with the zero vector)
      ArmStep(sim, a, {0, 0, 1}, true, true);
      u->armReady = false;
      a->onTarget = false;
      continue;
    }
    // AimDir 0x6366f0: from the aim bone in last beat's pose (actor+8: last beat's transform and rotations)
    Vec3 p;
    Quat q;
    {
      const Vec3 curPos = u->position;
      const Quat curOri = u->orientation;
      u->position = priorPos;
      u->orientation = priorOri;
      BoneWorld(u, a->aimBone, &p, &q);
      u->position = curPos;
      u->orientation = curOri;
    }
    Vec3 d = Sub(aim, p);
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    d = len > 0 ? Mul(d, 1.0f / len) : Vec3{};
    bool on = ArmStep(sim, a, d, false, false);
    a->onTarget = on;
    u->armReady = on;
  }
}

void SetArmAimTarget(Sim& sim, Unit* u, Vec3 p) {
  if (!HasBuilderObject(u)) return;
  u->armAim = p;
  if (p.x != 0 || p.y != 0 || p.z != 0 || std::signbit(p.x) || std::signbit(p.y) || std::signbit(p.z))
    sim.CallMethod(sim.L(), u, "OnPrepareArmToBuild", 0);
}

namespace {
BuilderArm* CheckArm(lua_State* L) { return CheckObject<BuilderArm>(L, 1); }
// CreateBuilderArmController(unit, yawBone, [pitchBone], [aimBone]) 0x636880
int l_CreateBuilderArmController(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (!u->skeleton) return luaL_error(L, "Unit has no skeleton.");
  auto a = std::make_unique<BuilderArm>();
  a->unit = u;
  a->yawBone = ResolveBoneArg(L, u, 2);
  a->pitchBone = ResolveBoneArg(L, u, 3);
  int aim = ResolveBoneArg(L, u, 4);
  a->aimBone = aim >= 0 ? aim : (a->pitchBone >= 0 ? a->pitchBone : a->yawBone);
  if (a->yawBone >= 0 && a->yawBone < u->skeleton->Count()) {
    const Quat& r = u->skeleton->bones()[static_cast<size_t>(a->yawBone)].localRot;
    a->hCenter = dmath::Atan2(2 * (r.w * r.y + r.x * r.z), 1 - 2 * (r.x * r.x + r.y * r.y));
  }
  if (HasBuilderObject(u)) u->armReady = false;
  BuilderArm* raw = a.get();
  u->builderArms.push_back(raw);
  CreateObject(L, raw, "CBuilderArmManipulator");
  S(L)->Own(std::move(a));
  return 1;
}
// SetAimingArc(minH, maxH, hSlew, minP, maxP, pSlew) 0x636a50: degrees -> rad, slews per tick
int l_arm_SetAimingArc(lua_State* L) {
  BuilderArm* a = CheckArm(L);
  float v[6];
  for (int i = 0; i < 6; ++i) v[i] = static_cast<float>(luaL_checknumber(L, i + 2)) * 0.0174532924f;
  a->hCenter = WrapAngle((v[0] + v[1]) * 0.5f);
  a->hHalf = std::fabs(v[1] - v[0]) * 0.5f;
  a->hSlew = v[2] * 0.1f;
  a->pCenter = WrapAngle((v[3] + v[4]) * 0.5f);
  a->pHalf = std::fabs(v[4] - v[3]) * 0.5f;
  a->pSlew = v[5] * 0.1f;
  lua_settop(L, 1);
  return 1;
}
int l_arm_GetHeadingPitch(lua_State* L) {
  BuilderArm* a = CheckArm(L);
  lua_pushnumber(L, a->heading);
  lua_pushnumber(L, a->pitch);
  return 2;
}
int l_arm_SetHeadingPitch(lua_State* L) {
  BuilderArm* a = CheckArm(L);
  a->heading = static_cast<float>(luaL_checknumber(L, 2));
  a->pitch = static_cast<float>(luaL_checknumber(L, 3));
  lua_settop(L, 1);
  return 1;
}
int l_arm_Enable(lua_State* L) {
  CheckArm(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_arm_Disable(lua_State* L) {
  CheckArm(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_arm_SetPrecedence(lua_State* L) {
  CheckArm(L)->precedence = static_cast<int>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_arm_Destroy(lua_State* L) {
  BuilderArm* a = CheckArm(L);
  a->alive = false;
  a->UnbindLua();
  return 0;
}
}  // namespace

namespace {
RotateManipulator* CheckRot(lua_State* L) { return CheckObject<RotateManipulator>(L, 1); }
// CreateRotator(unit, bone, axis, [goal], [speed], [accel], [goalspeed])
int l_CreateRotator(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  auto r = std::make_unique<RotateManipulator>();
  r->unit = u;
  if (u) r->bone = ResolveBoneArg(L, u, 2);
  const char* ax = luaL_optstring(L, 3, "y");
  r->axis = (ax[0] == 'x' || ax[0] == 'X') ? 0 : (ax[0] == 'z' || ax[0] == 'Z') ? 2 : 1;
  if (lua_isnumber(L, 4)) {
    r->goal = static_cast<float>(lua_tonumber(L, 4));
    r->hasGoal = true;
  }
  if (lua_isnumber(L, 5)) r->speed = r->targetSpeed = static_cast<float>(lua_tonumber(L, 5));
  if (lua_isnumber(L, 6)) r->accel = static_cast<float>(lua_tonumber(L, 6));
  if (lua_isnumber(L, 7)) r->targetSpeed = static_cast<float>(lua_tonumber(L, 7));
  RotateManipulator* raw = r.get();
  if (u) u->rotators.push_back(raw);
  CreateObject(L, raw, "CRotateManipulator");
  S(L)->Own(std::move(r));
  return 1;
}
int l_rot_SetGoal(lua_State* L) {
  RotateManipulator* r = CheckRot(L);
  r->goal = static_cast<float>(luaL_checknumber(L, 2));
  r->hasGoal = true;
  lua_settop(L, 1);
  return 1;
}
int l_rot_ClearGoal(lua_State* L) {
  CheckRot(L)->hasGoal = false;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetSpeed(lua_State* L) {
  RotateManipulator* r = CheckRot(L);
  r->speed = r->targetSpeed = static_cast<float>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetTargetSpeed(lua_State* L) {
  CheckRot(L)->targetSpeed = static_cast<float>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetAccel(lua_State* L) {
  CheckRot(L)->accel = static_cast<float>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetCurrentAngle(lua_State* L) {
  CheckRot(L)->cur = static_cast<float>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_rot_GetCurrentAngle(lua_State* L) {
  lua_pushnumber(L, CheckRot(L)->cur);
  return 1;
}
int l_rot_Enable(lua_State* L) {
  CheckRot(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_rot_Disable(lua_State* L) {
  CheckRot(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_rot_Destroy(lua_State* L) {
  RotateManipulator* r = CheckRot(L);
  r->alive = false;
  r->UnbindLua();
  return 0;
}
}  // namespace

void RegisterCombatBindings(lua_State* L) {
  SetGlobal(L, "CreateAimController", l_CreateAimController);
  SetGlobal(L, "CreateRotator", l_CreateRotator);
  SetGlobal(L, "CreateBuilderArmController", l_CreateBuilderArmController);
  SetMethod(L, "CBuilderArmManipulator", "SetAimingArc", l_arm_SetAimingArc);
  SetMethod(L, "CBuilderArmManipulator", "GetHeadingPitch", l_arm_GetHeadingPitch);
  SetMethod(L, "CBuilderArmManipulator", "SetHeadingPitch", l_arm_SetHeadingPitch);
  SetMethod(L, "CBuilderArmManipulator", "Enable", l_arm_Enable);
  SetMethod(L, "CBuilderArmManipulator", "Disable", l_arm_Disable);
  SetMethod(L, "CBuilderArmManipulator", "SetPrecedence", l_arm_SetPrecedence);
  SetMethod(L, "CBuilderArmManipulator", "Destroy", l_arm_Destroy);
  SetMethod(L, "CRotateManipulator", "SetGoal", l_rot_SetGoal);
  SetMethod(L, "CRotateManipulator", "ClearGoal", l_rot_ClearGoal);
  SetMethod(L, "CRotateManipulator", "SetSpeed", l_rot_SetSpeed);
  SetMethod(L, "CRotateManipulator", "SetTargetSpeed", l_rot_SetTargetSpeed);
  SetMethod(L, "CRotateManipulator", "SetAccel", l_rot_SetAccel);
  SetMethod(L, "CRotateManipulator", "SetCurrentAngle", l_rot_SetCurrentAngle);
  SetMethod(L, "CRotateManipulator", "GetCurrentAngle", l_rot_GetCurrentAngle);
  SetMethod(L, "CRotateManipulator", "Enable", l_rot_Enable);
  SetMethod(L, "CRotateManipulator", "Disable", l_rot_Disable);
  SetMethod(L, "CRotateManipulator", "Destroy", l_rot_Destroy);
  SetMethod(L, "CAimManipulator", "SetFiringArc", l_aim_SetFiringArc);
  SetMethod(L, "CAimManipulator", "SetResetPoseTime", l_aim_SetResetPoseTime);
  SetMethod(L, "CAimManipulator", "OnTarget", l_aim_OnTarget);
  SetMethod(L, "CAimManipulator", "SetEnabled", l_aim_SetEnabled);
  SetMethod(L, "CAimManipulator", "GetHeadingPitch", l_aim_GetHeadingPitch);
  SetMethod(L, "CAimManipulator", "SetHeadingPitch", l_aim_SetHeadingPitch);
  SetMethod(L, "CAimManipulator", "SetAimHeadingOffset", l_aim_SetAimHeadingOffset);
  SetMethod(L, "CAimManipulator", "Destroy", l_aim_Destroy);
  SetMethod(L, "CAimManipulator", "SetPrecedence", l_aim_SetPrecedence);

  SetMethod(L, "Entity", "ReachedMaxShooters", l_ReachedMaxShooters);
  SetMethod(L, "Unit", "GetTargetEntity", l_GetTargetEntity);
  SetMethod(L, "Unit", "SetFireState", l_SetFireState);
  SetMethod(L, "Unit", "GetFireState", l_GetFireState);
  SetMethod(L, "Unit", "ToggleFireState", l_ToggleFireState);
  SetMethod(L, "Unit", "SetStunned", l_SetStunned);
  SetMethod(L, "Unit", "IsStunned", l_IsStunned);

  SetMethod(L, "UnitWeapon", "CanFire", l_w_CanFire);
  SetMethod(L, "UnitWeapon", "ChangeDamage", l_w_ChangeDamage);
  SetMethod(L, "UnitWeapon", "ChangeDamageRadius", l_w_ChangeDamageRadius);
  SetMethod(L, "UnitWeapon", "ChangeDamageType", l_w_ChangeDamageType);
  SetMethod(L, "UnitWeapon", "ChangeFiringTolerance", l_w_ChangeFiringTolerance);
  SetMethod(L, "UnitWeapon", "ChangeMaxHeightDiff", l_w_ChangeMaxHeightDiff);
  SetMethod(L, "UnitWeapon", "ChangeMaxRadius", l_w_ChangeMaxRadius);
  SetMethod(L, "UnitWeapon", "ChangeMinRadius", l_w_ChangeMinRadius);
  SetMethod(L, "UnitWeapon", "ChangeProjectileBlueprint", l_w_ChangeProjectileBlueprint);
  SetMethod(L, "UnitWeapon", "ChangeRateOfFire", l_w_ChangeRateOfFire);
  SetMethod(L, "UnitWeapon", "FireWeapon", l_w_FireWeapon);
  SetMethod(L, "UnitWeapon", "GetCurrentTarget", l_w_GetCurrentTarget);
  SetMethod(L, "UnitWeapon", "GetCurrentTargetPos", l_w_GetCurrentTargetPos);
  SetMethod(L, "UnitWeapon", "GetFireClockPct", l_w_GetFireClockPct);
  SetMethod(L, "UnitWeapon", "GetFiringRandomness", l_w_GetFiringRandomness);
  SetMethod(L, "UnitWeapon", "SetFiringRandomness", l_w_SetFiringRandomness);
  SetMethod(L, "UnitWeapon", "GetProjectileBlueprint", l_w_GetProjectileBlueprint);
  SetMethod(L, "UnitWeapon", "IsFireControl", l_w_IsFireControl);
  SetMethod(L, "UnitWeapon", "SetFireControl", l_w_SetFireControl);
  SetMethod(L, "UnitWeapon", "ResetTarget", l_w_ResetTarget);
  SetMethod(L, "UnitWeapon", "SetEnabled", l_w_SetEnabled);
  SetMethod(L, "UnitWeapon", "SetFireTargetLayerCaps", l_w_SetFireTargetLayerCaps);
  SetMethod(L, "UnitWeapon", "SetTargetEntity", l_w_SetTargetEntity);
  SetMethod(L, "UnitWeapon", "SetTargetGround", l_w_SetTargetGround);
  SetMethod(L, "UnitWeapon", "SetTargetingPriorities", l_w_SetTargetingPriorities);
  SetMethod(L, "UnitWeapon", "TransferTarget", l_w_TransferTarget);
  SetMethod(L, "UnitWeapon", "WeaponHasTarget", l_w_WeaponHasTarget);
  SetMethod(L, "UnitWeapon", "PlaySound", l_w_PlaySound);

  RegisterDamageBindings(L);
  RegisterProjectileBindings(L);
}

// For the flight model (sim/air.cpp).
bool WeaponCanAttackTarget(Sim& sim, UnitWeapon* w, const AiTarget& t) { return CanAttackTarget(sim, w, t); }

}  // namespace moho
