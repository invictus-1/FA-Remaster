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
#include "sim/air.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/intel.h"
#include "sim/commands.h"
#include "sim/formation.h"
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

// One terrain triangle of a cell (0x4778f0 / 0x477b80): the plane through (cx, hA, cz) with normal n. A segment
// part P0 -> P1 that starts under the plane hits at tIn (the t the ray entered the CELL at, also for the second
// triangle of a cell); one that ends under it hits where the ray meets the plane.
static bool TerrainTriHit(float nx, float ny, float nz, float fcx, float fcz, float hA, const Vec3& O, const Vec3& D,
                          const Vec3& P0, const Vec3& P1, float tIn, float* out) {
  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);  // 0x452af0
  if (len > 9.99999997e-07f) {
    const float inv = 1.0f / len;
    nx *= inv;
    ny *= inv;
    nz *= inv;
  } else {
    nx = ny = nz = 0.0f;
  }
  const float d = nz * fcz + nx * fcx + ny * hA;
  const float s0 = (P0.z * nz + P0.y * ny + P0.x * nx) - d;
  if (0.0f > s0) {
    *out = tIn;
    return true;
  }
  const float s1 = (P1.z * nz + P1.y * ny + P1.x * nx) - d;
  if (0.0f > s1) {
    const float num = (O.z * nz + O.y * ny + nx * O.x) - d;
    const float den = D.z * nz + D.y * ny + nx * D.x;
    *out = -0.0f - num / den;
    return true;
  }
  return false;
}

// CHeightField's line test 0x477030 (behind STIMap::Intersection 0x476f30): the first t in [nearT, farT] at which
// the ray O + D t is under the terrain. The exe walks a min/max pyramid of the heights; what it ends up testing
// are the ray's cells in order, each cut along its (x, z) -> (x + 1, z + 1) diagonal into two triangles. (The
// pyramid only skips blocks the ray passes above, and the clip against the highest point of the map is left out
// for the same reason.)
static bool TerrainLineHit(const TerrainMap* map, const Vec3& O, const Vec3& D, float nearT, float farT, float* dist) {
  const int w = map->width() + 1, h = map->height() + 1;  // height samples per row / column
  // 0x4776e0: the part of the ray over the map
  auto clip = [&](float o, float dv, int n) {
    if (dv != 0.0f) {
      const float tA = (-1.0f / dv) * o, tB = (static_cast<float>(n - 1) - o) / dv;
      if (tB > tA) {
        if (tA > nearT) nearT = tA;
        if (!(tB > farT)) farT = tB;
      } else {
        if (tB > nearT) nearT = tB;
        if (!(tA > farT)) farT = tA;
      }
      return true;
    }
    return !(0.0f > o) && !(o > static_cast<float>(n - 1));
  };
  if (!clip(O.x, D.x, w) || !clip(O.z, D.z, h)) return false;
  if (farT < nearT) return false;
  if (D.x == 0.0f && D.z == 0.0f) {  // straight down or up: the elevation under it
    const float t = (map->TerrainHeight(O.x, O.z) - O.y) / D.y;
    if (!(t >= nearT) || !(farT >= t)) return false;
    *dist = t;
    return true;
  }
  // 0x40d860: the walk runs mirrored so that the ray goes toward +x and +z (a mirrored cell index is ~index)
  const float xN = D.x * nearT + O.x, xF = D.x * farT + O.x, zN = D.z * nearT + O.z, zF = D.z * farT + O.z;
  const bool mx = !(xF >= xN), mz = !(zF >= zN);
  const float x0 = mx ? -0.0f - xN : xN, x1 = mx ? -0.0f - xF : xF;
  const float z0 = mz ? -0.0f - zN : zN, z1 = mz ? -0.0f - zF : zF;
  const float dx = x1 - x0, dz = z1 - z0;
  int curX = static_cast<int>(std::floor(x0)), curZ = static_cast<int>(std::floor(z0));
  int cx = mx ? ~curX : curX, cz = mz ? ~curZ : curZ;
  float tIn = nearT;
  for (int guard = 0; guard < 1000000; ++guard) {
    // the next cell: across x when the cell's far corner lies on the z side of the line (0x477230)
    const int nX = curX + 1, nZ = curZ + 1;
    if ((static_cast<float>(nZ) - z1) * dx > (static_cast<float>(nX) - x1) * dz) curX = nX;
    else curZ = nZ;
    bool last = false;
    float tOut;
    int ncx = cx, ncz = cz;
    if (static_cast<float>(curX) > x1 || static_cast<float>(curZ) > z1) {
      last = true;
      tOut = farT;
    } else {
      ncx = mx ? ~curX : curX;
      ncz = mz ? ~curZ : curZ;
      if (cx != ncx) tOut = (static_cast<float>(std::max(cx, ncx)) - O.x) / D.x;
      else tOut = (static_cast<float>(std::max(cz, ncz)) - O.z) / D.z;
    }
    if (cx >= 0 && cx < w - 1 && cz >= 0 && cz < h - 1) {
      const Vec3 P0{O.x + D.x * tIn, O.y + D.y * tIn, O.z + D.z * tIn};
      const Vec3 P1{O.x + D.x * tOut, O.y + D.y * tOut, O.z + D.z * tOut};
      const float fcx = static_cast<float>(cx), fcz = static_cast<float>(cz);
      const float hA = map->HeightAt(cx, cz), hC = map->HeightAt(cx + 1, cz + 1);
      // 0x4778f0: the triangle holding (cx, cz + 1); 0x477b80: the one holding (cx + 1, cz)
      auto triA = [&](const Vec3& a, const Vec3& b) {
        const float ay = map->HeightAt(cx, cz + 1) - hA, by = hC - hA;
        return TerrainTriHit(ay - by, 1.0f, by * 0.0f - ay, fcx, fcz, hA, O, D, a, b, tIn, dist);
      };
      auto triB = [&](const Vec3& a, const Vec3& b) {
        const float ay = map->HeightAt(cx + 1, cz) - hA, by = hC - hA;
        return TerrainTriHit(by * 0.0f - ay, 1.0f, ay - by, fcx, fcz, hA, O, D, a, b, tIn, dist);
      };
      const float k = static_cast<float>(cx - cz);
      const float e0 = P0.x - P0.z, e1 = P1.x - P1.z;
      auto mid = [&]() {
        const float tm = (k - (O.x - O.z)) / (D.x - D.z);
        return Vec3{O.x + D.x * tm, O.y + D.y * tm, O.z + D.z * tm};
      };
      bool hit;
      if (k > e0) {
        if (k > e1) {
          hit = triA(P0, P1);
        } else {
          const Vec3 M = mid();
          hit = triA(P0, M) || triB(M, P1);
        }
      } else if (!(e1 < k)) {
        hit = triB(P0, P1);
      } else {
        const Vec3 M = mid();
        hit = triB(P0, M) || triA(M, P1);
      }
      if (hit) return true;
    }
    if (last) return false;
    tIn = tOut;
    cx = ncx;
    cz = ncz;
  }
  return false;
}

// STIMap::Intersection 0x476f30 along the segment p0 -> p1, as Projectile::CheckCollision 0x69d4a8 asks it: the
// segment as centre, unit direction and half length (0x4fe130), the line from its start over twice that length.
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit) {
  if (!map) return false;
  const Vec3 hv{(p1.x - p0.x) * 0.5f, (p1.y - p0.y) * 0.5f, (p1.z - p0.z) * 0.5f};
  const float ext = std::sqrt(hv.y * hv.y + hv.z * hv.z + hv.x * hv.x);
  if (!(ext > 0.0f)) return false;
  const float inv = 1.0f / ext;
  const Vec3 D{inv * hv.x, hv.y * inv, hv.z * inv};
  const Vec3 C{p0.x + hv.x, p0.y + hv.y, p0.z + hv.z};
  const Vec3 O{C.x - D.x * ext, C.y - D.y * ext, C.z - D.z * ext};
  float t;
  if (!TerrainLineHit(map, O, D, 0.0f, ext * 2.0f, &t)) return false;
  *dist = t;
  *hit = {O.x + D.x * t, O.y + D.y * t, O.z + D.z * t};
  return true;
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
  if (u->blueprint) return Add(u->position, RotateM(u->orientation, u->bpCenter));  // bone -1 (0x679d19)
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

// The same point for code that runs inside the motion pass (flight, a projectile's steering). An entity's new
// transform is committed at the beat's end, so a target WITHOUT an aim bone - the centre of its collision
// primitive, from the entity's own transform - still stands and lies as the beat began, also when the target
// has taken its step already. (With an aim bone the point comes from the pose, which moved with that step.)
Vec3 TargetPosBeatStart(Sim& sim, const AiTarget& t) {
  if (t.type == 1 && t.aimBone < 0)
    if (Entity* e = TargetEntity(sim, t); e && e->shapeHeld) {
      if (e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->blueprint)
        return Add(e->shapePos, RotateM(e->shapeOri, static_cast<Unit*>(e)->bpCenter));
      return e->shapePos;
    }
  return TargetPos(sim, t, false);
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
  // CAniPoseBone's composite 0x54bec0: down the chain from the pose's world transform (the entity's), each
  // bone parent x local (0x549c20); local = rest x this beat's manipulators
  int chain[64];
  int n = 0;
  for (int i = bone; i >= 0 && n < 64; i = bones[static_cast<size_t>(i)].parent) chain[n++] = i;
  Quat r = e->orientation;
  Vec3 p = e->position;
  if (e->kind == Entity::Kind::Unit) {  // the pose's own world transform (Unit::GetBoneWorldTransform 0x6aa5c0)
    const Unit* u = static_cast<const Unit*>(e);
    if (u->poseSet) {
      r = u->poseBaseOri;
      p = u->poseBasePos;
    }
  }
  const bool posed = e->poseQ.size() == bones.size();
  for (int k = n - 1; k >= 0; --k) {
    const size_t bi = static_cast<size_t>(chain[k]);
    const Bone& b = bones[bi];
    const Vec3 lp = posed ? e->poseP[bi] : Vec3{b.localPos.x * s, b.localPos.y * s, b.localPos.z * s};
    p = Add(p, RotateM(r, lp));
    r = ComposeQ(r, posed ? e->poseQ[bi] : b.localRot);
  }
  *pos = p;
  *rot = r;
}

// CAniPose::UpdateBones 0x54b5f0: every bone back to the skeleton's rest transform
void PoseReset(Entity* e) {
  if (!e->skeleton) return;
  const auto& bones = e->skeleton->bones();
  const float s = e->meshScale * e->scale[0];
  e->poseQ.resize(bones.size());
  e->poseP.resize(bones.size());
  for (size_t i = 0; i < bones.size(); ++i) {
    e->poseQ[i] = bones[i].localRot;
    e->poseP[i] = {bones[i].localPos.x * s, bones[i].localPos.y * s, bones[i].localPos.z * s};
  }
}
// CAniPoseBone::Rotate 0x54bc00: local rotation = local rotation x q
void PoseRotate(Entity* e, int bone, const Quat& q) {
  if (bone < 0 || static_cast<size_t>(bone) >= e->poseQ.size()) return;
  Quat& l = e->poseQ[static_cast<size_t>(bone)];
  l = ComposeQ(l, q);
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

bool PickPointAboveWater(Sim& sim, Entity* e, int* bone, bool peek);
bool PickPointBelowWater(Sim& sim, Entity* e, int* bone, bool peek);

// PickTargetPoint 0x6d5590: whether the weapon may target the entity at all. For an entity on the SEABED a weapon
// that only takes targets above (below) the water must find a target point of it there, and the search draws one
// of them every time it is asked (0x6d55f6 / 0x6d564e), although the pick is thrown away. (peek: no draw, for
// debug output.)
bool Targetable(Sim& sim, UnitWeapon* w, Entity* e, bool peek = false) {
  if (!e) return false;
  if (w->bp->ignoreIfDisabled && !w->enabled) return false;
  const int layerBit = EntityLayerBit(e);
  if ((w->layerCaps & layerBit) == 0) return false;
  if (layerBit == 2) {
    int bone;
    if (w->bp->aboveWaterTargetsOnly) {
      if (!PickPointAboveWater(sim, e, &bone, peek)) return false;
    } else if (w->bp->belowWaterTargetsOnly) {
      if (!PickPointBelowWater(sim, e, &bone, peek)) return false;
    }
  }
  if (!e->blueprint) return false;
  if (!SetEmpty(w->disallow) && InSet(w->disallow, e->blueprint)) return false;
  if (!SetEmpty(w->onlyAllow) && !InSet(w->onlyAllow, e->blueprint)) return false;
  return true;
}

// UnitWeapon::CanAttackTarget 0x6d5720
bool CanAttackTarget(Sim& sim, UnitWeapon* w, const AiTarget& t, bool peek = false) {
  if (t.type == 0) return false;
  if (w->bp->ignoreIfDisabled && !w->enabled) return false;
  if (t.type == 1) return Targetable(sim, w, TargetEntity(sim, t), peek);
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
      // a mobile target is led: where it will be in Air.PredictAheadForBombDrop seconds (0x62cd40). (Weapons
      // run before the beat's moves: nothing is held yet, committed and current are the same here.)
      if (cb.predictAheadForBombDrop > 0 && w->target.mobile)
        if (Entity* te = TargetEntity(sim, w->target); te && te->kind == Entity::Kind::Unit)
          T = PredictAhead(sim, static_cast<Unit*>(te), cb.predictAheadForBombDrop, true);
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

// Unit::PickTargetPointAboveWater 0x6aafb0 / ...BelowWater 0x6ab190 (weapons with AboveWaterTargetsOnly /
// BelowWaterTargetsOnly): the unit's target points above (below) the water and one of them drawn; false when none
// is. A unit without target bones: its own height decides, no draw. (Each its own function, as in the exe: the
// recorder's draw callers then pair up with the original's.)
bool PickPointBeside(Sim& sim, Entity* e, int* bone, bool above, std::vector<int>* pts) {
  *bone = -1;
  if (!e || e->kind != Entity::Kind::Unit) return false;
  Unit* tu = static_cast<Unit*>(e);
  const CombatBpData& cb = CB(tu);
  const float wl = WaterLevel(sim);
  if (cb.targetBones.empty() || !e->skeleton) return above ? tu->position.y > wl : wl > tu->position.y;
  for (const std::string& name : cb.targetBones) {
    int b = e->skeleton->Find(name);
    Vec3 p;
    Quat q;
    BoneWorld(e, b < 0 ? 0 : b, &p, &q);
    if (above ? p.y > wl : wl > p.y) pts->push_back(b);
  }
  return !pts->empty();
}
__attribute__((noinline)) bool PickPointAboveWater(Sim& sim, Entity* e, int* bone, bool peek) {
  std::vector<int> pts;
  if (!PickPointBeside(sim, e, bone, true, &pts)) return false;
  if (!pts.empty() && !peek) *bone = pts[sim.IntRange(static_cast<uint32_t>(pts.size()))];
  return true;
}
__attribute__((noinline)) bool PickPointBelowWater(Sim& sim, Entity* e, int* bone, bool peek) {
  std::vector<int> pts;
  if (!PickPointBeside(sim, e, bone, false, &pts)) return false;
  if (!pts.empty() && !peek) *bone = pts[sim.IntRange(static_cast<uint32_t>(pts.size()))];
  return true;
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

// The wake-up of a weapon's fire task (inlined in SetTarget 0x6d6186 and SetEnabled 0x6d7e98): the thread's wait
// becomes 0, and a suspended thread is linked back into the attacker's run list at its "new" end. So it runs after
// every other task of the unit from then on, in this pass too when the stage is running: a weapon that was once
// switched off (an unpacking launcher) fires in the very beat its acquire task hands it a target.
static void WakeFireThread(UnitWeapon* w) {
  if (!w->fireSuspended) return;
  w->fireSuspended = false;
  Unit* u = w->unit;
  auto& th = u->weaponThreads;
  for (size_t p = 0; p < th.size(); ++p)
    if (th[p].first == w && !th[p].second) {
      th.erase(th.begin() + static_cast<std::ptrdiff_t>(p));
      th.emplace_back(w, false);
      if (u->weaponThreadPos >= 0 && static_cast<int>(p) < u->weaponThreadPos) --u->weaponThreadPos;
      break;
    }
}

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
  if (e && e->kind == Entity::Kind::Unit && w->target.viaBlip) {
    const Unit* tu = static_cast<const Unit*>(e);
    w->target.blipId = tu->blip ? EntityRef(tu->blip) : 0;
  }
  // PickNewTargetAimSpot 0x6d5de0
  int aimBone = -1;
  if (w->bp->aboveWaterTargetsOnly) PickPointAboveWater(sim, e, &aimBone, false);
  else if (w->bp->belowWaterTargetsOnly) PickPointBelowWater(sim, e, &aimBone, false);
  else aimBone = PickAimBone(sim, e);
  w->target.aimBone = aimBone;
  if (e && !e->dead) ++e->shooters;
  if (!had && has) WeaponCall(sim, w, "OnGotTarget");
  WakeFireThread(w);
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
  // EntitiesAroundPoint walks the entity grid, so the list comes in GRID order (rect_queries.md 2.4: rows by z,
  // columns by x, in a bucket the most recently registered first; ids play no part). FindBestEnemy depends on it:
  // the current target only holds its place against candidates that come after it.
  if (u->blipCache.size() > 1 && sim.entityGrid().ready()) {
    std::vector<Entity*> grid;
    sim.entityGrid().GatherBox(p.x - R, p.z - R, p.x + R, p.z + R, 1, &grid);
    std::vector<Unit*> ordered;
    ordered.reserve(u->blipCache.size());
    for (Entity* e : grid) {
      if (e->kind != Entity::Kind::Unit) continue;
      Unit* t = static_cast<Unit*>(e);
      if (std::binary_search(u->blipCache.begin(), u->blipCache.end(), t,
                             [](const Unit* x, const Unit* y) { return x->id < y->id; }))
        ordered.push_back(t);
    }
    if (ordered.size() != u->blipCache.size())  // (not in the grid: no collision shape; they keep id order, last)
      for (Unit* t : u->blipCache)
        if (std::find(ordered.begin(), ordered.end(), t) == ordered.end()) ordered.push_back(t);
    u->blipCache.swap(ordered);
  }
  {
    // MOHO64_DEBUG_BLIPCACHE=<unit id>: every rebuild of that unit's list of enemies in reach
    static const long dbgId = getenv("MOHO64_DEBUG_BLIPCACHE") ? atol(getenv("MOHO64_DEBUG_BLIPCACHE")) : -1;
    if (dbgId >= 0 && static_cast<long>(u->id) == dbgId) {
      std::string ids;
      for (const Unit* t : u->blipCache) ids += " " + std::to_string(t->id);
      Logf(LogLevel::Info, "blipcache %u unit %u period %d reach %.3f:%s", sim.tick(), u->id, period, R, ids.c_str());
    }
  }
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
    if (!Targetable(sim, w, e)) continue;
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

// CheckAutoInitiate 0x5d8c40: the weapon may order its unit to attack what it acquires while the unit has no
// orders, or only an Attack / FormAttack whose target entity is gone or dead.
bool CheckAutoInitiate(Sim& sim, const UnitWeapon* w) {
  if (!w->bp->autoInitiateAttackCommand) return false;
  const Unit* u = w->unit;
  if (u->commands.empty()) return true;
  if (u->commands.size() != 1) return false;
  const UnitCommand& c = *u->commands.front();
  if (c.type != CommandType::Attack && c.type != CommandType::FormAttack) return false;
  if (!c.targetId) return false;  // (a ground target)
  const Entity* e = sim.FindEntity(c.targetId);
  return !e || e->dead || e->destroyQueued;
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
  // MOHO64_DEBUG_ACQTICK=<tick>: every acquire task that runs in that beat, and how far it gets
  static const long dbgAcqTick = getenv("MOHO64_DEBUG_ACQTICK") ? atol(getenv("MOHO64_DEBUG_ACQTICK")) : -1;
  const bool dbgAcq2 = dbgAcqTick >= 0 && static_cast<long>(sim.tick()) == dbgAcqTick;
  const bool engaging = u->unitStates.count("Attacking") &&
                        ((b.autoInitiateAttackCommand && !CheckAutoInitiate(sim, w)) || cb.needUnpack || code == 1);
  const bool keeps = !engaging && !b.alwaysRecheckTarget && TargetStatus(sim, w, w->target) == 0 &&
                     CanAttackTarget(sim, w, w->target) && HasTarget(sim, w->target) &&
                     !TargetExempt(u, TargetEntity(sim, w->target));
  if (dbgAcq2) {
    const Entity* ce = w->target.type == 1 ? sim.FindEntity(w->target.entityId) : nullptr;
    const Entity* de = d.type == 1 ? sim.FindEntity(d.entityId) : nullptr;
    Logf(LogLevel::Info, "acqtick %u unit %u%s %s code %d period %d holds %d%s wants %d%s -> %s", sim.tick(), u->id,
         u->dead ? " (dead)" : "", b.label.c_str(), code, period, ce ? static_cast<int>(ce->id) : -1,
         ce && ce->dead ? " (dead)" : "", de ? static_cast<int>(de->id) : -1, de && de->dead ? " (dead)" : "",
         engaging ? "engaging, again next beat" : keeps ? "keeps its target" : "looks for a target");
    if (b.autoInitiateAttackCommand)  // what CheckAutoInitiate looks at: the unit's orders
      for (const auto& c : u->commands) {
        const Entity* te = c->targetId ? sim.FindEntity(c->targetId) : nullptr;
        Logf(LogLevel::Info, "acqtick %u unit %u   order type %d target %d%s%s", sim.tick(), u->id,
             static_cast<int>(c->type), te ? static_cast<int>(te->id) : c->targetId ? -2 : -1,
             te && te->dead ? " (dead)" : "", c->targetViaBlip ? " via its blip" : "");
      }
  }
  if (engaging) return 1;
  // keep the current target
  if (keeps) return ret;
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
        // The candidates come from the entity grid (EntitiesAroundPoint 0x722350, mask projectiles), and an entity
        // without a collision primitive is in no cell of it. A projectile has none unless its blueprint or its
        // script gives it one: FAF's torpedo and missile classes call SetCollisionShape('Sphere', ...) when they
        // are made, a mod's class that does not (BlackOps' AssaultTorpedoSubProjectile) makes a shell that no
        // counter-weapon ever finds. (Seen: 89 such torpedoes in a replay, none taken as a target.)
        if (p->shape.type == ShapeType::None) continue;
        float d2 = Dist2XZ(u->position, p->position);
        if (d2 > reach * reach) continue;
        if (!((w->layerCaps & 8) || p->position.y > wl)) continue;
        if (!((w->layerCaps & 16) || p->position.y <= wl)) continue;
        if (!Targetable(sim, w, p)) continue;
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
    // an idle unit's AutoInitiateAttackCommand weapon also orders the unit to attack what it found (0x5d93d5)
    const bool autoAttack = CheckAutoInitiate(sim, w);
    // the task first builds a CAiTarget of what it found (UpdateTarget 0x5d55b0: one target-point draw, every
    // time), then hands it to the weapon, whose own draw (PickNewTargetAimSpot) only comes with a new target
    {
      static const bool dbgAcq = getenv("MOHO64_DEBUG_ACQUIRE") != nullptr;
      if (dbgAcq)
        Logf(LogLevel::Info, "acq %u unit %u %s -> %u (had %u)", sim.tick(), u->id, b.label.c_str(), best->id,
             TargetEntity(sim, w->target) ? TargetEntity(sim, w->target)->id : 0u);
    }
    TargetPointDraw(sim, best);
    SetTarget(sim, w, EntityTarget(best));  // (Unit::GetBlipsInRange 0x6acc60 lists the units themselves)
    if (autoAttack) IssueAutoAttack(sim, u, best);
  } else if (dHas && CanAttackTarget(sim, w, d)) {
    SetTarget(sim, w, d);
  } else if (d.type == 1 && d.viaBlip && TargetEntity(sim, d) && TargetEntity(sim, d)->dead &&
             CanAttackTarget(sim, w, d)) {
    // CAiTarget::HasTarget 0x5e2a10 of a recon blip is "the BLIP is not dead", and the blip of a unit that just died
    // lives on: a weapon that finds nothing else is given the commanded blip (seen: a Fatboy's riot guns draw two
    // above-water target points the beat after the unit they were ordered onto died)
    SetTarget(sim, w, d);
  } else {
    SetTarget(sim, w, AiTarget{});
  }
  return ret;
}

// UnitWeapon::Fire 0x6d61f0: the script's OnFire, and the wait until the next shot
void FireNow(Sim& sim, UnitWeapon* w) {
  if (w->unit->stunned) return;
  WeaponCall(sim, w, "OnFire");
  ++g_fires;
  ++w->shots;
  const float rof = RoF(w);
  w->fireClock = rof > 0 ? static_cast<int>(std::nearbyint(10.0f / rof)) : 0;
}

// CFireWeaponTask::Execute 0x6d3dc0
void FireTick(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  if (w->fireClock != 0) --w->fireClock;
  static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
  if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit) {  // why it does or does not fire this tick
    const bool has = HasTarget(sim, w->target);
    Logf(LogLevel::Info, "wdbg %u unit %u %s clock %d enabled %d target %d(%u) canAttack %d canFire %d onTarget %d status %d",
         sim.tick(), u->id, w->label.c_str(), w->fireClock, static_cast<int>(w->enabled), w->target.type,
         w->target.entityId, has ? static_cast<int>(CanAttackTarget(sim, w, w->target, true)) : -1,
         has ? static_cast<int>(CanFire(sim, w)) : -1, static_cast<int>(w->onTarget),
         has ? TargetStatus(sim, w, w->target) : -1);
  }
  if (!w->enabled || w->bp->manualFire) {
    if (w->fireClock == 0) w->fireSuspended = true;  // (returns -2: off the run list until woken)
    return;
  }
  if (w->fireClock != 0 || u->fireState == 1) return;
  if (!HasTarget(sim, w->target)) return;
  if (!CanAttackTarget(sim, w, w->target)) return;
  if (!CanFire(sim, w)) return;
  if (!CheckSilo(w)) return;
  if (TargetStatus(sim, w, w->target) != 0) return;
  if (w->bp->cannotAttackGround && w->target.type == 2) return;
  FireNow(sim, w);
}

}  // namespace

void WeaponsTick(Sim& sim) {
  // the armies' attacker stages (brain+0xb0): units in creation order inside each army, not in id order (an id
  // used a second time belongs to a younger unit). A copy: a script fired from here may create units.
  const std::vector<Unit*> units = sim.ArmyStageOrder();
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
    // the attacker's stage (attacker+0x44): the unit's fire and acquire threads in their run order. A fire thread
    // woken while the stage runs is at the end of the list by then and so runs in this pass.
    for (u->weaponThreadPos = 0; u->weaponThreadPos < static_cast<int>(u->weaponThreads.size()); ++u->weaponThreadPos) {
      const auto [w, acquire] = u->weaponThreads[static_cast<size_t>(u->weaponThreadPos)];
      if (!w->bp || !w->HasLuaObject()) continue;
      if (!acquire) {
        if (!w->fireSuspended) FireTick(sim, w);
      } else if (!w->bp->manualFire && sim.tick() >= w->nextAcquire) {
        int r = AcquireTick(sim, w);
        w->nextAcquire = sim.tick() + static_cast<uint32_t>(std::max(1, r));
      }
      if (u->destroyQueued || u->combatGone) break;
    }
    u->weaponThreadPos = -1;
  }
}

// SetDesiredTarget 0x5d75b0
static void SetDesiredTarget(Sim& sim, Unit* u, const AiTarget& t) {
  // A PLAYER's order on an enemy unit holds the army's recon blip of it (the watch: weapons that take the order's
  // target report the blip, weapons that found the same unit themselves report the unit): the target comes with
  // viaBlip set from the command (CommandTarget). An order the engine made itself from what a weapon found (the
  // auto-initiated attack) names the unit: its missiles lose their target the moment the unit dies (seen: one
  // zig-zag draw less in the beat an interceptor died), where a blip's outlive the unit.
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
    v = RotateM(Conj(bq), goal);
  }
  float target;
  if (heading) {
    target = dmath::Atan2(v.x, v.z) + c->headingOffset;
  } else {
    // v' = Rx(center) * v, target = center - FastNegAsin(v'.y / |v'|) (0x50b710). FastNegAsin is the
    // Abramowitz-Stegun acos polynomial minus pi/2, used for negative arguments too (no reflection),
    // so pitches below the arc centre carry its error (a 45-degree arc rests at 0.0508, not 0).
    const float hc = center * 0.5f;
    const Vec3 r = RotateM(Quat{dmath::Sin(hc), 0.0f, 0.0f, dmath::Cos(hc)}, v);
    const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    if (l > 0) {
      const float t = std::fmin(1.0f, r.y / l);
      float p = t * -0.0187293f + 0.0742610f;
      p = p * t - 0.2121144f;
      p = p * t + 1.5707288f;
      const float neg = -(1.5707964f - std::sqrt(1.0f - t) * p);
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
  static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
  if (dbgUnit >= 0 && static_cast<long>(c->unit->id) == dbgUnit)
    Logf(LogLevel::Info, "wdbg   aim %s %s cur %.6f target %.6f delta %.6f slew %.6f tol %.6f goal %.5f %.5f %.5f",
         c->label.c_str(), heading ? "yaw" : "pitch", cur, target, delta, slew, tol, goal.x, goal.y, goal.z);
  cur = WrapPi(cur + step);
  int bits = 0;
  if (heading && std::fabs(delta) > 0.001f) bits |= 2;
  if (!(!heading && c->weapon && c->weapon->bp && c->weapon->bp->yawOnlyOnTarget))
    if (std::fabs(WrapPi(cur - target)) > tol) bits |= 1;
  return bits;
}

void ApplyHeading(AimController* c, int bone) { PoseRotate(c->unit, bone, AxisAngle({0, 1, 0}, c->heading)); }
void ApplyPitch(AimController* c, int bone) { PoseRotate(c->unit, bone, AxisAngle({1, 0, 0}, -c->pitch)); }
void Hold(AimController* c) {
  ApplyHeading(c, c->yawBone);
  ApplyPitch(c, c->pitchBone);
}

// Track 0x630760: returns on target.
bool Track(Sim& sim, AimController* c, Vec3 goal, bool raw) {
  float tol = (c->weapon ? Tolerance(c->weapon) : 0.01f) * kDeg2Rad;
  int bits = 0;
  {
    static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
    if (dbgUnit >= 0 && static_cast<long>(c->unit->id) == dbgUnit)
      Logf(LogLevel::Info, "wdbg   track %s yaw bone %d pitch bone %d muzzle %d vertical %d hCenter %.6f hHalf %.6f pCenter %.6f pHalf %.6f",
           c->label.c_str(), c->yawBone, c->pitchBone, c->muzzleBone, static_cast<int>(c->yawVertical), c->hCenter,
           c->hHalf, c->pCenter, c->pHalf);
  }
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
  // the muzzle in last beat's pose (actor+8), moved on by the unit's velocity
  Vec3 Mpos = c->lastMuzzlePos;
  Quat Mrot = c->lastMuzzleRot;
  if (IsMobile(u)) Mpos = Add(Mpos, UnitVelocity(u));  // (vtbl+0x3c)
  Entity* te = TargetEntity(sim, w->target);
  Vec3 tVel = te && !te->dead ? EntityVelocity(te) : Vec3{};
  // Unit::GetTargetPoint 0x6ab340: a target bone comes from the unit's pose (0x6aa5c0: where its motion already
  // put it this beat); without target bones it is the blueprint centre on the entity's committed transform
  // (0x679ce0, entity+0x9c), which this beat's motion has not replaced yet (AdvanceCoords is step 13): such a
  // target that already moved this beat is still seen where it stood
  Vec3 tPos;
  Unit* tu = te && te->kind == Entity::Kind::Unit ? static_cast<Unit*>(te) : nullptr;
  if (tu && w->target.aimBone < 0 && tu->lastPosTick == sim.tick()) {
    const Vec3 curPos = tu->position;
    const Quat curOri = tu->orientation;
    tu->position = tu->lastPosition;
    tu->orientation = tu->lastOrientation;
    tPos = TargetPos(sim, w->target, false);
    tu->position = curPos;
    tu->orientation = curOri;
  } else {
    tPos = TargetPos(sim, w->target, false);
  }
  if (IsNaN(tPos)) return NaNVec();
  if (te) {
    // FAF patch 0x128b054: a target the entity stage has not ticked yet this beat (entity+0x174 != the sim's
    // tick; for a recon blip (vf+0x1c) the stamp of its source unit, blip+0x270) is taken one tick of its
    // velocity ahead
    const bool ticked = tu ? tu->lastPosTick == sim.tick() : te->lastMoveTick == sim.tick();
    if (!ticked) tPos = Add(tPos, tVel);
  }
  float hDist = DistXZ(tPos, Mpos);
  float mv = w->bp->muzzleVelocity;
  if (hDist < w->bp->muzzleVelocityReduceDistance) mv *= std::sqrt(hDist / w->bp->muzzleVelocityReduceDistance);
  bool phys = w->projBp != nullptr;
  {
    static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
    if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit)
      Logf(LogLevel::Info, "wdbg   solve %s M %.9g %.9g %.9g T %.9g %.9g %.9g V %.9g %.9g %.9g mv %.9g lead %d phys %d grav %d track %d moved %d",
           c->label.c_str(), Mpos.x, Mpos.y, Mpos.z, tPos.x, tPos.y, tPos.z, tVel.x, tVel.y, tVel.z, mv,
           static_cast<int>(w->bp->leadTarget && w->target.type == 1), static_cast<int>(phys),
           static_cast<int>(c->usesGravity), static_cast<int>(c->tracksTarget),
           te ? static_cast<int>(te->lastMoveTick == sim.tick()) : -1);
  }
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

void PoseAdvance(Unit* u) {
  u->prevPoseBasePos = u->poseSet ? u->poseBasePos : u->position;
  u->prevPoseBaseOri = u->poseSet ? u->poseBaseOri : u->orientation;
  u->poseBasePos = u->position;
  u->poseBaseOri = u->orientation;
  u->poseSet = true;
}

void UnitAimTick(Sim& sim, Unit* u) {
  const bool anims = !u->animators.empty();
  if (u->aimControllers.empty() && u->rotators.empty() && u->builderArms.empty() && u->sliders.empty() && !anims) {
    if (!u->colliders.empty()) CollidersTick(sim, u);
    return;
  }
  if (!u->skeleton) {
    if (anims) AnimatorsUpdate(sim, u);  // (their time still runs)
    return;
  }
  if (!u->aimControllers.empty()) {  // (the pose is still last beat's)
    const Vec3 curPos = u->poseBasePos;
    const Quat curOri = u->poseBaseOri;
    u->poseBasePos = u->prevPoseBasePos;
    u->poseBaseOri = u->prevPoseBaseOri;
    for (AimController* c : u->aimControllers)
      if (c->alive && c->muzzleBone >= 0) BoneWorld(u, c->muzzleBone, &c->lastMuzzlePos, &c->lastMuzzleRot);
    u->poseBasePos = curPos;
    u->poseBaseOri = curOri;
  }
  PoseReset(u);
  if (anims) AnimatorsUpdate(sim, u);  // (precedence 0, usually the unit's first manipulators)
  RotatorsTick(u);
  // manipulators run in the order of their precedence (0x63ace0): a slider below every aim controller's goes first
  int aimPrec = INT_MAX;
  for (const AimController* c : u->aimControllers)
    if (c->alive) aimPrec = std::min(aimPrec, c->precedence);
  SlidersTick(u, INT_MIN, aimPrec);
  for (BuilderArm* a : u->builderArms) {  // the arm's angles from this beat's MoveManipulator
    if (!a->alive || !a->enabled) continue;
    PoseRotate(u, a->yawBone, AxisAngle({0, 1, 0}, a->heading));
    PoseRotate(u, a->pitchBone, AxisAngle({1, 0, 0}, -a->pitch));
  }
  for (size_t i = 0; i < u->aimControllers.size(); ++i) {
    AimController* c = u->aimControllers[i];
    if (c->alive) MoveManipulator(sim, c);
  }
  SlidersTick(u, aimPrec, INT_MAX);  // (a weapon's recoil sliders have precedence 11, its aim controller 10)
  if (!u->colliders.empty()) CollidersTick(sim, u);
}

// CCollisionManipulator::ManipulatorUpdate 0x637c90
void CollidersTick(Sim& sim, Unit* u) {
  if (!u->skeleton) return;
  const std::vector<CollisionManipulator*> list = u->colliders;  // (the callbacks may make or destroy some)
  for (CollisionManipulator* c : list) {
    if (!c->alive || !c->enabled) continue;
    for (size_t i = 0; i < c->watch.size(); ++i) {
      const int bone = c->watch[i].bone;
      if (bone < 0 || bone >= u->skeleton->Count()) continue;
      Vec3 p;
      Quat q;
      BoneWorld(u, bone, &p, &q);
      const char* event = nullptr;
      Vec3 arg = p;
      if (c->terrain) {
        // under the surface: the terrain's elevation there, or the water when that is higher (0x637d58)
        const TerrainMap* m = sim.map();
        float h = m ? m->TerrainHeight(p.x, p.z) : 0.0f;
        if (m && m->hasWater && m->waterElevation > h) h = m->waterElevation;
        const bool under = h > p.y;
        if (under == c->watch[i].under) continue;
        c->watch[i].under = under;
        event = under ? "OnAnimTerrainCollision" : "OnNotAnimTerrainCollision";
      } else {
        // the bone in the unit's own frame, less than 0.1 above its base (0x637e6a)
        arg = RotateM(Conj(u->orientation), Sub(p, u->position));
        if (!(0.1f > arg.y)) {
          c->watch[i].atBase = false;
          continue;
        }
        if (c->watch[i].atBase) continue;
        c->watch[i].atBase = true;
        event = "OnAnimCollision";
      }
      if (!u->HasLuaObject()) continue;
      lua_State* L = sim.L();
      lua_pushstring(L, u->skeleton->bones()[static_cast<size_t>(bone)].name.c_str());
      lua_pushnumber(L, arg.x);
      lua_pushnumber(L, arg.y);
      lua_pushnumber(L, arg.z);
      sim.CallMethod(L, u, event, 4);
      if (!c->alive || !c->enabled) break;
    }
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
  if (auto* c = dynamic_cast<AimController*>(GetObject(L, 1))) c->precedence = static_cast<int>(luaL_checknumber(L, 2));
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
      if (e->kind == Entity::Kind::Unit) {
        Unit* tu = static_cast<Unit*>(e);
        a.mobile = IsMobile(tu);
        a.viaBlip = t.targetViaBlip;
        // the task's CAiTarget is a copy of the command's (0x5f29fb): it keeps the command's target point, which
        // is where GetTargetPosGun aims (an aircraft that is not yet on its attack run flies at that bone)
        const CombatBpData& cb = CB(tu);
        if (t.targetPoint >= 0 && t.targetPoint < static_cast<int>(cb.targetBones.size()) && e->skeleton)
          a.aimBone = e->skeleton->Find(cb.targetBones[static_cast<size_t>(t.targetPoint)]);
      }
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
  int ocWeapon = -1;         // +0x5c: the overcharge weapon's index (an OverCharge order only)
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
  // (the navigator's goal only: Moving is the move task's state. The steering's "speed through the goal" flag
  // (+0x9d) is not the goal's: Unit::UpdateSpeedThroughStatus sets it at queue events, and a goal set by an attack
  // leaves it. A patrolling unit that turns on an enemy keeps it: its waypoint inside the weapon's square is a
  // drive-through one, as in the original's unit records.)
  const bool through = u->motion.nav && u->motion.nav->speedThroughGoal;
  LandNavSetGoalRect(sim, u, x0, z0, x1, z1, through);
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
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {  // CAiNavigatorAir::SetDestUnit: the exact spot
    u->motion.failed = false;
    AirSetDestEntity(sim, u, e);
    return;
  }
  int cx = static_cast<int>(std::nearbyint(e->position.x - 0.5f));
  int cz = static_cast<int>(std::nearbyint(e->position.z - 0.5f));
  AttackSetGoalRect(sim, u, cx, cz, cx + 1, cz + 1);
}

// Update 0x5f3020 (no formation)
void AttackUpdate(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  bool has = HasTarget(sim, target);
  // The formation branch (0x5f30ae..0x5f3270): units ordered to attack as a group each go for THEIR place in the
  // command's formation, which is laid out around the target, not for the target itself.
  UnitCommand* fc = nullptr;
  if (!t.child && !u->commands.empty() && u->commands.front()->form && u->motion.bp && u->motion.bp->mobile() &&
      !BpInCategory(sim, u->blueprint, "TARGETCHASER") && FormationHas(*u->commands.front(), u))
    fc = u->commands.front().get();
  if (fc) {
    Vec3 cell;
    if (d.trackMobile) {  // a mobile target: the formation is centred on where it is now (SetCentre)
      if (has) d.lastPos = TargetPos(sim, target, false);
      if (IsZeroV(d.lastPos)) d.lastPos = u->position;
      FormationSetCentre(*fc, d.lastPos.x, d.lastPos.z);
    } else {
      d.lastPos = FormationGoal(sim, *fc, u, &cell) ? cell : u->position;
    }
    if (d.firstUpdate) {
      UnitWeapon* fw = TargetWeapon(sim, u, target);
      float ax = 0, az = 0;
      const bool adj = fw && FormationAdjusted(sim, *fc, u, &ax, &az);
      if (getenv("MOHO64_DEBUG_FORM"))
        Logf(LogLevel::Info, "attack form %u unit %u track %d has %d lastPos %.3f %.3f weapon %d adjusted %d %.3f %.3f",
             sim.tick(), u->id, d.trackMobile ? 1 : 0, has ? 1 : 0, d.lastPos.x, d.lastPos.z, fw ? 1 : 0, adj ? 1 : 0, ax, az);
      if (adj) {
        // SetWeaponGoal 0x5f2ce0 around the unit's adjusted place
        const int R = static_cast<int>(fw->bp->maxRadius);
        const int x0 = static_cast<int>(std::nearbyint(ax - R * 0.5f)), z0 = static_cast<int>(std::nearbyint(az - R * 0.5f));
        if (R < 1) AttackSetPosGoal(sim, u, Vec3{ax, 0, az});
        else AttackSetGoalRect(sim, u, x0, z0, x0 + R, z0 + R);
      } else if (FormationGoal(sim, *fc, u, &cell)) {
        AttackSetPosGoal(sim, u, cell);  // (SetPosGoal 0x5f2d90 at its formation cell)
      }
    } else {
      AttackSetPosGoal(sim, u, has ? TargetPos(sim, target, false) : d.lastPos);
    }
    d.firstUpdate = false;
    return;
  }
  if (has) d.lastPos = TargetPos(sim, target, false);
  if (IsZeroV(d.lastPos)) d.lastPos = u->position;
  UnitWeapon* w = d.firstUpdate ? TargetWeapon(sim, u, target) : nullptr;
  Entity* e = target.type == 1 ? sim.FindEntity(target.entityId) : nullptr;
  if (w) {
    // SetWeaponGoal 0x5f2ce0: a square of side trunc(MaxRadius) around the target's aim point
    Vec3 p = TargetPos(sim, target, true);
    int R = static_cast<int>(w->bp->maxRadius);
    int x0 = static_cast<int>(std::nearbyint(p.x - R * 0.5f)), z0 = static_cast<int>(std::nearbyint(p.z - R * 0.5f));
    {
      // MOHO64_DEBUG_ATTACKGOAL=<unit id>: the square an attacker is sent to (the weapon, the target's point)
      static const long dbgGoal = getenv("MOHO64_DEBUG_ATTACKGOAL") ? atol(getenv("MOHO64_DEBUG_ATTACKGOAL")) : -1;
      if (dbgGoal >= 0 && static_cast<long>(u->id) == dbgGoal)
        Logf(LogLevel::Info, "attackgoal %u unit %u weapon %s R %d target type %d entity %d point %.9g %.9g %.9g square %d %d",
             sim.tick(), u->id, w->bp->label.c_str(), R, target.type, e ? static_cast<int>(e->id) : -1, p.x, p.y, p.z, x0,
             z0);
    }
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
  UnitWeapon* oc = d.ocWeapon >= 0 && d.ocWeapon < static_cast<int>(u->weapons.size())
                       ? u->weapons[static_cast<size_t>(d.ocWeapon)] : nullptr;
  UnitWeapon* w = oc ? oc : u->weapons.empty() ? nullptr : TargetWeapon(sim, u, target);
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
      // ... or the unit is within Air.EngageDistance of the target, 2D (IsWithinHorizontalDistance 0x5f2f00 at
      // 0x5f39e6): an aircraft takes its target, and with it flies its attack runs, as soon as it is that near,
      // long before a weapon reaches. The blueprint's default is 0: never for what has no Air section.
      const auto engageNear = [&] {
        const Vec3 tp = TargetPos(sim, target, false);
        const float dx = u->position.x - tp.x, dz = u->position.z - tp.z;
        return CB(u).engageDistance > std::sqrt(dz * dz + dx * dx);
      };
      if ((w && WithinWeaponRange(sim, w, target)) || engageNear()) {
        bool any = false;
        for (UnitWeapon* x : u->weapons) any = any || (x->bp && CanAttackTarget(sim, x, target));
        if (!any) return -1;
        u->navIgnoreFormation = true;
        UpdateAttacker(sim, u, t, target);
        if (oc) {  // the overcharge weapon is given the target by the task itself (0x6d5f30)
          SetTarget(sim, oc, target);
          t.state = 4;
          return 1;
        }
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
      // AI.AttackAngle (0x5f3d..): the unit keeps the target that many degrees off its bow; the direction to the
      // target turned about the up axis away from the target's side goes to the motion (SetFacing 0x6b8590)
      if (const float aa = CB(u).attackAngle; aa > 0 && HasTarget(sim, target)) {
        Vec3 tp = TargetPos(sim, target, false);
        if (!IsNaN(tp)) {
          Vec3 v{tp.x - u->position.x, tp.y - u->position.y, tp.z - u->position.z};
          const Quat& q = u->orientation;
          const float fx = (q.w * q.y + q.z * q.x) * 2.0f, fz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
          const float ang = (v.z * fx - fz * v.x > 0.0f) ? aa * 0.017453292f : aa * -0.017453292f;
          double s, c;
          dmath::SinCos(static_cast<double>(ang), &s, &c);
          const float sn = static_cast<float>(s), cs = static_cast<float>(c);
          u->motion.faceDir = Norm(Vec3{v.x * cs + v.z * sn, v.y, v.z * cs - v.x * sn});
        }
        return 10;
      }
      if (!oc) return -2;
      // overcharge (0x5f3dc3): the weapon's script says whether it may fire (energy); a standing mobile unit
      // whose overcharge has no firing solution goes back to chasing; otherwise fire once and end
      if (CallMethodBool(sim, sim.L(), oc, "CanWeaponFire", 0) <= 0) return 10;
      if (u->position.x == u->lastPosition.x && u->position.y == u->lastPosition.y &&
          u->position.z == u->lastPosition.z && u->motion.bp && u->motion.bp->mobile() && !oc->canReach) {
        AttackUpdate(sim, u, t, target);
        t.state = 3;
        return 1;
      }
      if (oc->enabled && CanFire(sim, oc)) {
        FireNow(sim, oc);
        t.state = 5;
        return 10;
      }
      return 1;
    case 5:
    default:
      AbortNavigation(u);
      return -1;
  }
}

}  // namespace

// new CUnitAttackTargetTask(target e) (the guard's and the patrol's child)
BuildTask* MakeAttackTaskOn(Sim& sim, Unit* u, Entity* e) {
  // both callers (CUnitPatrolTask::TaskTick 0x61c229, CUnitGuardTask::SetEnemy 0x61390f) first build a CAiTarget of
  // the enemy they found (UpdateTarget 0x5d55b0: one target-point draw when it has AI.TargetBones), then make the task
  const int targetPoint = TargetPointDraw(sim, e);
  if (u->weapons.empty() || !e) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::Attack;
  t->order = "Attack";
  t->goalId = EntityRef(e);
  t->targetPoint = targetPoint;
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
  t->targetPoint = c.targetPoint;
  t->targetViaBlip = c.targetViaBlip;
  t->site = c.pos;
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) t->site = e->position;
  InitAttack(sim, u, *t, c.type == CommandType::FormAttack);
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

// CUnitFireAtTask ctor 0x60b260: the weapon is the first ManualFire weapon that is not the overcharge and whose
// NukeWeapon flag matches the order (Nuke: set, Tactical: clear). Kept as its index in t.count (-1: none).
BuildTask* StartFireAtTask(Sim& sim, Unit* u, const UnitCommand& c) {
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->order = "FireAt";
  t->goalId = c.targetId;
  t->targetPoint = c.targetPoint;
  t->targetViaBlip = c.targetViaBlip;
  t->site = c.pos;
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) t->site = e->position;
  const bool nuke = c.type == CommandType::Nuke;
  t->count = -1;
  for (size_t i = 0; i < u->weapons.size(); ++i) {
    const WeaponBp* b = u->weapons[i]->bp;
    if (!b || !b->manualFire || b->overChargeWeapon || b->nukeWeapon != nuke) continue;
    t->count = static_cast<int>(i);
    break;
  }
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

// TaskTick 0x60b380. States: 0 get in range and see to a missile, 1 wait for it, 2 fire, 3 wait for the launch to
// begin (the unit turns Busy), 4 wait for it to end.
int TickFireAt(Sim& sim, Unit* u, BuildTask& t) {
  if (sim.tick() < t.waitUntil) return kTaskRunning;
  UnitWeapon* w = t.count >= 0 && t.count < static_cast<int>(u->weapons.size()) ? u->weapons[static_cast<size_t>(t.count)] : nullptr;
  if (!w || !w->enabled) return kTaskFailed;
  const bool nuke = t.type == CommandType::Nuke;
  const int kind = nuke ? 1 : 0;
  const bool mobile = u->motion.bp && u->motion.bp->mobile() && !u->immobile;
  const float minR = w->ovMinRadius < 0 ? w->bp->minRadius : w->ovMinRadius;
  const float maxR = w->ovMaxRadius < 0 ? w->bp->maxRadius : w->ovMaxRadius;
  const AiTarget target = CommandTarget(sim, t);
  const Vec3 tp = TargetPos(sim, target, false);
  const float dx = u->position.x - tp.x, dz = u->position.z - tp.z;
  const float d = std::sqrt(dx * dx + dz * dz);
  for (int guard = 0; guard < 4; ++guard) {
    switch (t.state) {
      case 0:
        if (minR > d) {  // too close: back off to 1.1 x the minimum range (the spot's PrepareMove: not built)
          if (!mobile) return kTaskFailed;
          if (!u->motion.hasGoal) {
            Vec3 away{u->position.x - tp.x, u->position.y - tp.y, u->position.z - tp.z};
            const float len = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
            const float k = len > 0 ? minR * 1.1f / len : 0.0f;
            TaskMoveToward(sim, u, Vec3{tp.x + away.x * k, tp.y + away.y * k, tp.z + away.z * k});
          }
          return kTaskRunning;
        }
        if (d > maxR) {  // too far: go to the target
          if (!mobile) return kTaskFailed;
          if (!u->motion.hasGoal) TaskMoveToward(sim, u, tp);
          return kTaskRunning;
        }
        if (mobile) TaskStopMoving(u);  // the navigator's AbortMove
        if (CheckSilo(w)) {
          t.state = 2;
          continue;
        }
        if (SiloIsBusy(sim, u, kind)) {
          t.state = 1;
          return kTaskRunning;
        }
        if (SiloIsFull(sim, u, kind)) {
          t.state = 2;
          continue;
        }
        if (!SiloQueueAmmo(sim, u, kind)) return kTaskFailed;
        t.state = 1;
        return kTaskRunning;
      case 1:
        if (CheckSilo(w)) t.state = 2;
        t.waitUntil = sim.tick() + 9;  // return 10
        return kTaskRunning;
      case 2:
        if (!u->unitStates.count("Busy")) {
          if (nuke) sim.CallMethod(sim.L(), u, "OnNukeLaunched", 0);
          SetTarget(sim, w, target);
          FireNow(sim, w);
          t.state = 3;
        }
        t.waitUntil = sim.tick() + 2;  // return 3
        return kTaskRunning;
      case 3:
        if (u->unitStates.count("Busy")) t.state = 4;
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
      default:
        if (!u->unitStates.count("Busy")) return kTaskDone;
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
    }
  }
  return kTaskRunning;
}

// dtor 0x60b800: the unit's Busy state is cleared
void EndFireAt(Sim& sim, Unit* u, BuildTask& t) {
  (void)sim;
  (void)t;
  if (!u->dead) u->unitStates.erase("Busy");
}

// An OverCharge order (DispatchTask 0x6096ad): the attack task in overcharge mode, only on an entity that is
// not allied. The mode's weapon is the first with OverChargeWeapon; the ctor tells its script OnEnableWeapon.
BuildTask* StartOverchargeTask(Sim& sim, Unit* u, const UnitCommand& c) {
  Entity* e = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
  if (!e || u->weapons.empty()) return nullptr;
  if (e->army && u->army && (e->army == u->army || IsAlly(u->army, e->army))) return nullptr;
  auto t = std::make_unique<BuildTask>();
  t->type = CommandType::OverCharge;
  t->order = "Attack";
  t->goalId = c.targetId;
  t->targetPoint = c.targetPoint;
  t->targetViaBlip = c.targetViaBlip;
  t->site = e->position;
  InitAttack(sim, u, *t, false);
  for (size_t i = 0; i < u->weapons.size(); ++i)
    if (u->weapons[i]->bp && u->weapons[i]->bp->overChargeWeapon) {
      t->adata->ocWeapon = static_cast<int>(i);
      WeaponCall(sim, u->weapons[i], "OnEnableWeapon");
      break;
    }
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int TickAttack(Sim& sim, lua_State* L, Unit* u, BuildTask& t) {
  (void)L;
  AiTarget target = CommandTarget(sim, t);
  bool air = u->motion.bp && u->motion.bp->motionType == kMotionAir;
  // CUnitAttackTargetTask::TaskTick 0x5f34c0 is one task for every unit; an aircraft's attack runs are the
  // motion's own (ComputeAirCombatTactics), the task only keeps the navigator on the target
  static const bool oldAirAttack = getenv("MOHO64_OLD_AIR_ATTACK") != nullptr;
  if ((!air || !oldAirAttack) && t.adata) {
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
  if (t.adata && t.adata->ocWeapon >= 0 && t.adata->ocWeapon < static_cast<int>(u->weapons.size()))
    WeaponCall(sim, u->weapons[static_cast<size_t>(t.adata->ocWeapon)], "OnDisableWeapon");  // dtor 0x5f41e4
  // the task's teardown clears its unit states also when the unit died (seen: a killed unit's queue empties and
  // "Attacking" goes in the same beat)
  // (MakingAttackRun is not the task's: only the flight code sets it and clears it, in its own pass; a killed
  // aircraft keeps it)
  u->unitStates.erase("Attacking");
  if (u->dead || u->destroyQueued) return;
  // the dtor 0x5f4160: IgnoreFormation(false), attacker.Stop() 0x5d56f0 = SetDesiredTarget(none) whatever the
  // desired target was (every weapon is told SetTarget(none): one that still holds a target, also a long dead
  // one, gets OnLostTarget - a missile launcher packs up), then AbortMove (the caller)
  if (!u->weapons.empty()) SetDesiredTarget(sim, u, AiTarget{});
  u->navIgnoreFormation = false;
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
  if (e && w->target.viaBlip && e->kind == Entity::Kind::Unit) {  // the blip its army holds of the unit
    Unit* tu = static_cast<Unit*>(e);
    if (tu->blip && static_cast<Entity*>(tu->blip)->HasLuaObject()) e = tu->blip;
  }
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
  if (w->enabled) WakeFireThread(w);  // 0x6d7e98
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
  if (Targetable(*S(L), w, t)) SetTarget(*S(L), w, EntityTarget(t));
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
  if (UnitWeapon* w = ToObject<UnitWeapon>(L, 1)) RecordPlaySound(L, w->unit, 2);
  return 0;
}

}  // namespace

void RegisterDamageBindings(lua_State* L);
void RegisterProjectileBindings(lua_State* L);

// ---- rotators (CRotateManipulator) ------------------------------------------------------------

void RotatorsTick(Unit* u) {
  auto& v = u->rotators;
  v.erase(std::remove_if(v.begin(), v.end(), [](RotateManipulator* r) { return !r->alive; }), v.end());
  for (RotateManipulator* r : v) {  // MoveManipulator 0x643860
    if (!r->enabled || r->bone < 0 || !u->skeleton || r->bone >= u->skeleton->Count()) continue;
    if (r->followBone >= 0) {  // the goal follows that bone's heading
      Vec3 p;
      Quat q;
      BoneWorld(u, r->followBone, &p, &q);
      r->goal = dmath::Atan2((q.w * q.y + q.z * q.x) * 2.0f, 1.0f - (q.y * q.y + q.x * q.x) * 2.0f);
    }
    if (r->spinDown && r->accel != 0.0f) {  // slow to a fifth of the target speed, then settle at the angle's half
      const float lim = r->targetSpeed * 0.2f;
      if (r->speed > lim) {
        const float d = -r->speed;
        if (r->accel > std::fabs(d)) r->speed = lim;
        else r->speed = std::copysign(r->accel, d) + r->speed;
        r->cur = WrapPi(r->cur + r->speed);
        r->signalled = false;
      } else if (!(lim < std::fabs(r->cur))) {
        r->speed = 0.0f;
        r->cur = r->cur * 0.5f;
        r->signalled = true;
      } else {
        r->cur = WrapPi(r->cur + lim);
        r->signalled = false;
      }
    } else if (r->hasGoal) {  // a step of the speed toward the goal; there: it signals (the speed stays)
      const float delta = WrapPi(r->goal - r->cur);
      if (!(r->speed < std::fabs(delta))) {
        r->cur = r->goal;
        r->signalled = true;
      } else {
        r->cur = std::copysign(r->speed, delta) + r->cur;
        r->signalled = false;
      }
    } else {  // spinning: the speed approaches the target speed by the acceleration; signalled when it is there
      if (r->accel != 0.0f && r->speed != r->targetSpeed) {
        const float d = r->targetSpeed - r->speed;
        if (r->accel > std::fabs(d)) r->speed = r->targetSpeed;
        else r->speed = std::copysign(r->accel, d) + r->speed;
      }
      r->signalled = r->targetSpeed == r->speed;
      r->cur = WrapPi(r->cur + r->speed);
    }
    PoseRotate(u, r->bone, AxisAngle(r->axis, r->cur));
  }
}

// ---- sliders (CSlideManipulator) --------------------------------------------------------------

// MoveManipulator 0x647300
void SlidersTick(Unit* u, int lo, int hi) {
  auto& v = u->sliders;
  if (v.empty()) return;
  v.erase(std::remove_if(v.begin(), v.end(), [](SlideManipulator* m) { return !m->alive; }), v.end());
  const auto& bones = u->skeleton->bones();
  for (SlideManipulator* m : v) {
    if (m->precedence < lo || m->precedence >= hi || !m->enabled) continue;
    if (m->bone < 0 || static_cast<size_t>(m->bone) >= bones.size()) continue;
    if (m->speed > 0.0f) {  // this tick's speed
      bool brake = false;
      if (m->decel > 0.0f) {  // 0x647250: braking from this speed would end past the goal
        const Vec3 d = Sub(m->goal, m->cur);
        const float dist = std::sqrt(d.x * d.x + d.z * d.z + d.y * d.y);
        const float dec = m->decel * 10.0f, sp = m->vel * 10.0f;
        const float stop = dec > 0.0f ? sp * sp / (dec * 2.0f) : 0.0f;
        brake = stop > dist;
      }
      if (brake) {
        float s = m->vel - m->decel * 0.1f;
        if (!(m->speed > s)) s = m->speed;
        if (0.0f > s) s = 0.0f;
        m->vel = s;
      } else if (m->accel > 0.0f) {
        float s = m->accel * 0.1f + m->vel;
        if (!(m->speed > s)) s = m->speed;
        if (0.0f > s) s = 0.0f;
        m->vel = s;
      } else {
        m->vel = m->speed;
      }
    }
    Vec3 d = Sub(m->goal, m->cur);
    bool stepped = false;
    if (m->speed > 0.0f) {  // 0x5d1e70: a step of at most the speed
      const float l2 = d.x * d.x + d.y * d.y + d.z * d.z;
      if (m->vel * m->vel < l2) {
        const float k = m->vel / std::sqrt(l2);
        m->cur = Add(m->cur, Vec3{d.x * k, d.y * k, d.z * k});
        m->signalled = false;
        stepped = true;
      }
    }
    if (!stepped) {
      m->vel = 0.0f;
      m->cur = m->goal;
      m->signalled = true;
    }
    // onto the bone's local position, turned by the bone's local rotation as it is now
    const size_t bi = static_cast<size_t>(m->bone);
    if (bi < u->poseQ.size()) u->poseP[bi] = Add(u->poseP[bi], RotateM(u->poseQ[bi], m->cur));
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
  if (static_cast<int>(u->poseQ.size()) != n) PoseReset(u);
  const auto& bones = u->skeleton->bones();
  int r = 0;
  if (a->yawBone >= 0 && a->yawBone < n) {
    // the yaw bone's frame this tick, before this manipulator's own rotation
    u->poseQ[static_cast<size_t>(a->yawBone)] = bones[static_cast<size_t>(a->yawBone)].localRot;
    if (a->pitchBone >= 0 && a->pitchBone < n)
      u->poseQ[static_cast<size_t>(a->pitchBone)] = bones[static_cast<size_t>(a->pitchBone)].localRot;
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
    u->poseQ[static_cast<size_t>(a->yawBone)] =
        ComposeQ(bones[static_cast<size_t>(a->yawBone)].localRot, AxisAngle({0, 1, 0}, a->heading));
  }
  if (a->pitchBone >= 0 && a->pitchBone < n) {
    Vec3 p;
    Quat q;
    BoneWorld(u, a->pitchBone, &p, &q);
    r |= ArmAxis(a, dir, local, false, a->pSlew * (slow ? 0.25f : 1.0f), q);
    u->poseQ[static_cast<size_t>(a->pitchBone)] =
        ComposeQ(bones[static_cast<size_t>(a->pitchBone)].localRot, AxisAngle({1, 0, 0}, -a->pitch));
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
      {
        static const long dbg = getenv("MOHO64_DEBUG_ARM") ? atol(getenv("MOHO64_DEBUG_ARM")) : -1;
        if (dbg >= 0 && static_cast<long>(u->id) == dbg && u->armReady)
          Logf(LogLevel::Info, "armdbg %u zero-target tick: on target 1 -> 0", sim.tick());
      }
      u->armReady = false;
      a->onTarget = false;
      continue;
    }
    // AimDir 0x6366f0: from the aim bone in last beat's pose (actor+8: last beat's transform and rotations)
    Vec3 p;
    Quat q;
    {
      const Vec3 curPos = u->poseBasePos;
      const Quat curOri = u->poseBaseOri;
      u->poseBasePos = u->prevPoseBasePos;
      u->poseBaseOri = u->prevPoseBaseOri;
      BoneWorld(u, a->aimBone, &p, &q);
      u->poseBasePos = curPos;
      u->poseBaseOri = curOri;
    }
    Vec3 d = Sub(aim, p);
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    d = len > 0 ? Mul(d, 1.0f / len) : Vec3{};
    bool on = ArmStep(sim, a, d, false, false);
    {
      static const long dbg = getenv("MOHO64_DEBUG_ARM") ? atol(getenv("MOHO64_DEBUG_ARM")) : -1;
      if (dbg >= 0 && static_cast<long>(u->id) == dbg && u->armReady != on)
        Logf(LogLevel::Info, "armdbg %u tracking tick: on target %d -> %d", sim.tick(), u->armReady ? 1 : 0, on ? 1 : 0);
    }
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
// degrees per second -> radians per tick (0xe4f768), degrees per second^2 -> radians per tick^2 (0xe4f764)
const float kRotSpeed = 0.001745329238474369f, kRotAccel = 0.0001745329354889691f;
void RotSetGoal(RotateManipulator* r, float degrees) {
  r->goal = degrees * kDeg2Rad;
  r->hasGoal = true;
  r->signalled = r->goal == r->cur;
  if (!r->unit) {  // (on no unit nothing turns it: it is there at once)
    r->cur = r->goal;
    r->signalled = true;
  }
}
// CreateRotator(unit, bone, axis, [goal], [speed], [accel], [goalspeed])
int l_CreateRotator(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  auto r = std::make_unique<RotateManipulator>();
  r->unit = u;
  if (u) r->bone = ResolveBoneArg(L, u, 2);
  // the axis: 'x', 'y', 'z', or with a minus for the other way round
  const char* ax = luaL_optstring(L, 3, "y");
  const float sign = ax[0] == '-' ? -1.0f : 1.0f;
  const char c = ax[0] == '-' ? ax[1] : ax[0];
  r->axis = (c == 'x' || c == 'X') ? Vec3{sign, 0, 0} : (c == 'z' || c == 'Z') ? Vec3{0, 0, sign} : Vec3{0, sign, 0};
  const int n = lua_gettop(L);
  if (n >= 4 && !lua_isnil(L, 4)) {  // a goal alone is reached at once (the default speed is infinite)
    RotSetGoal(r.get(), static_cast<float>(luaL_checknumber(L, 4)));
    r->speed = std::numeric_limits<float>::infinity();
  }
  if (n >= 5 && !lua_isnil(L, 5)) r->speed = static_cast<float>(luaL_checknumber(L, 5)) * kRotSpeed;
  if (n >= 6 && !lua_isnil(L, 6)) r->accel = static_cast<float>(luaL_checknumber(L, 6)) * kRotAccel;
  if (n >= 7 && !lua_isnil(L, 7)) {
    r->targetSpeed = static_cast<float>(luaL_checknumber(L, 7)) * kRotSpeed;
    r->signalled = r->speed == r->targetSpeed;
  }
  RotateManipulator* raw = r.get();
  if (u) u->rotators.push_back(raw);
  CreateObject(L, raw, "CRotateManipulator");
  S(L)->Own(std::move(r));
  return 1;
}
int l_rot_SetGoal(lua_State* L) {  // 0x644470
  RotSetGoal(CheckRot(L), static_cast<float>(luaL_checknumber(L, 2)));
  lua_settop(L, 1);
  return 1;
}
int l_rot_ClearGoal(lua_State* L) {
  CheckRot(L)->hasGoal = false;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetSpeed(lua_State* L) {  // 0x644790: the speed it turns at now (not the target speed)
  CheckRot(L)->speed = static_cast<float>(luaL_checknumber(L, 2)) * kRotSpeed;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetTargetSpeed(lua_State* L) {  // 0x644930
  RotateManipulator* r = CheckRot(L);
  r->targetSpeed = static_cast<float>(luaL_checknumber(L, 2)) * kRotSpeed;
  r->signalled = r->speed == r->targetSpeed;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetAccel(lua_State* L) {  // 0x644b00
  CheckRot(L)->accel = static_cast<float>(luaL_checknumber(L, 2)) * kRotAccel;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetSpinDown(lua_State* L) {
  CheckRot(L)->spinDown = lua_toboolean(L, 2) != 0;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetFollowBone(lua_State* L) {
  RotateManipulator* r = CheckRot(L);
  r->followBone = r->unit ? ResolveBoneArg(L, r->unit, 2) : -1;
  lua_settop(L, 1);
  return 1;
}
int l_rot_ClearFollowBone(lua_State* L) {
  CheckRot(L)->followBone = -1;
  lua_settop(L, 1);
  return 1;
}
int l_rot_SetCurrentAngle(lua_State* L) {  // 0x643cf0: also turns the bone in the pose as it stands
  RotateManipulator* r = CheckRot(L);
  const float a = static_cast<float>(luaL_checknumber(L, 2)) * kDeg2Rad;
  if (r->unit) PoseRotate(r->unit, r->bone, AxisAngle(r->axis, a));
  r->cur = a;
  lua_settop(L, 1);
  return 1;
}
int l_rot_GetCurrentAngle(lua_State* L) {
  lua_pushnumber(L, CheckRot(L)->cur * 57.295780181884766f);
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

SlideManipulator* CheckSlide(lua_State* L) { return CheckObject<SlideManipulator>(L, 1); }
float SlideScale(const SlideManipulator* m) {  // the pose's scale ([pose]+8)
  return m->unit ? m->unit->meshScale * m->unit->scale[0] : 1.0f;
}
// 0x647530
void SlideSetGoal(SlideManipulator* m, float x, float y, float z) {
  m->goal = {x, y, z};
  if (!m->world) {
    const float s = SlideScale(m);
    m->goal = {m->goal.x * s, m->goal.y * s, m->goal.z * s};
  }
  m->signalled = m->cur.x == m->goal.x && m->cur.y == m->goal.y && m->cur.z == m->goal.z;
  if (!m->unit) {  // (not on a unit: nothing moves it, it is there at once)
    m->cur = m->goal;
    m->signalled = true;
  }
}
void SlideSetSpeed(SlideManipulator* m, float speed) {
  if (speed > 0.0f) {
    m->speed = speed * 0.1f;
    if (!m->world) m->speed = SlideScale(m) * m->speed;
  } else {
    m->speed = -1.0f;
    m->vel = 0.0f;
  }
}
// CreateSlider(unit, bone, [goal_x, goal_y, goal_z, [speed, [world_space]]]) 0x6476e0
int l_CreateSlider(lua_State* L) {
  const int n = lua_gettop(L);
  Unit* u = ToObject<Unit>(L, 1);
  auto m = std::make_unique<SlideManipulator>();
  m->unit = u;
  if (u && u->skeleton) m->bone = ResolveBoneArg(L, u, 2);
  if (n >= 7) m->world = lua_toboolean(L, 7) != 0;
  if (n >= 3)
    SlideSetGoal(m.get(), static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                 static_cast<float>(luaL_checknumber(L, 5)));
  if (n >= 6) SlideSetSpeed(m.get(), static_cast<float>(luaL_checknumber(L, 6)));
  SlideManipulator* raw = m.get();
  if (u && u->skeleton) u->sliders.push_back(raw);
  else raw->unit = nullptr;
  CreateObject(L, raw, "CSlideManipulator");
  S(L)->Own(std::move(m));
  return 1;
}
// CreateCollisionDetector(unit) 0x638020; WatchBone(bone), EnableTerrainCheck(bool), Enable(), Disable()
int l_CreateCollisionDetector(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  auto m = std::make_unique<CollisionManipulator>();
  m->unit = u;
  CollisionManipulator* raw = m.get();
  if (u) u->colliders.push_back(raw);
  CreateObject(L, raw, "CCollisionManipulator");
  S(L)->Own(std::move(m));
  return 1;
}
CollisionManipulator* CheckCollider(lua_State* L) { return CheckObject<CollisionManipulator>(L, 1); }
int l_coll_WatchBone(lua_State* L) {
  CollisionManipulator* c = CheckCollider(L);
  if (c->unit && c->unit->skeleton) {
    const int bone = ResolveBoneArg(L, c->unit, 2);
    bool have = false;
    for (const auto& w : c->watch) have = have || w.bone == bone;
    if (bone >= 0 && !have) {
      CollisionManipulator::Watch w;
      w.bone = bone;
      c->watch.push_back(w);
    }
  }
  lua_settop(L, 1);
  return 1;
}
int l_coll_EnableTerrainCheck(lua_State* L) {
  CheckCollider(L)->terrain = lua_toboolean(L, 2) != 0;
  lua_settop(L, 1);
  return 1;
}
int l_coll_Enable(lua_State* L) {
  CheckCollider(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_coll_Disable(lua_State* L) {
  CheckCollider(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_coll_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* c = dynamic_cast<CollisionManipulator*>(o)) {
    c->alive = false;
    if (c->unit) {
      auto& v = c->unit->colliders;
      v.erase(std::remove(v.begin(), v.end(), c), v.end());
    }
  }
  if (o) o->UnbindLua();
  return 0;
}
int l_slide_SetWorldUnits(lua_State* L) {
  CheckSlide(L)->world = lua_toboolean(L, 2) != 0;
  lua_settop(L, 1);
  return 1;
}
int l_slide_SetSpeed(lua_State* L) {
  SlideSetSpeed(CheckSlide(L), static_cast<float>(luaL_checknumber(L, 2)));
  lua_settop(L, 1);
  return 1;
}
int l_slide_SetAcceleration(lua_State* L) {
  const float a = static_cast<float>(luaL_checknumber(L, 2));
  CheckSlide(L)->accel = (a > 0.0f ? a : 0.0f) * 0.1f;
  lua_settop(L, 1);
  return 1;
}
int l_slide_SetDeceleration(lua_State* L) {
  const float a = static_cast<float>(luaL_checknumber(L, 2));
  CheckSlide(L)->decel = (a > 0.0f ? a : 0.0f) * 0.1f;
  lua_settop(L, 1);
  return 1;
}
int l_slide_SetGoal(lua_State* L) {
  SlideSetGoal(CheckSlide(L), static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
               static_cast<float>(luaL_checknumber(L, 4)));
  lua_settop(L, 1);
  return 1;
}
int l_slide_SetPrecedence(lua_State* L) {
  CheckSlide(L)->precedence = static_cast<int>(luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}
int l_slide_Enable(lua_State* L) {
  CheckSlide(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_slide_Disable(lua_State* L) {
  CheckSlide(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_slide_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* m = dynamic_cast<SlideManipulator*>(o)) m->alive = false;
  if (o) o->UnbindLua();
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
  SetMethod(L, "CRotateManipulator", "SetSpinDown", l_rot_SetSpinDown);
  SetMethod(L, "CRotateManipulator", "SetFollowBone", l_rot_SetFollowBone);
  SetMethod(L, "CRotateManipulator", "ClearFollowBone", l_rot_ClearFollowBone);
  SetMethod(L, "CRotateManipulator", "SetCurrentAngle", l_rot_SetCurrentAngle);
  SetMethod(L, "CRotateManipulator", "GetCurrentAngle", l_rot_GetCurrentAngle);
  SetMethod(L, "CRotateManipulator", "Enable", l_rot_Enable);
  SetMethod(L, "CRotateManipulator", "Disable", l_rot_Disable);
  SetMethod(L, "CRotateManipulator", "Destroy", l_rot_Destroy);
  SetGlobal(L, "CreateCollisionDetector", l_CreateCollisionDetector);
  SetMethod(L, "CCollisionManipulator", "WatchBone", l_coll_WatchBone);
  SetMethod(L, "CCollisionManipulator", "EnableTerrainCheck", l_coll_EnableTerrainCheck);
  SetMethod(L, "CCollisionManipulator", "Enable", l_coll_Enable);
  SetMethod(L, "CCollisionManipulator", "Disable", l_coll_Disable);
  SetMethod(L, "CCollisionManipulator", "Destroy", l_coll_Destroy);
  SetGlobal(L, "CreateSlider", l_CreateSlider);
  SetMethod(L, "CSlideManipulator", "SetWorldUnits", l_slide_SetWorldUnits);
  SetMethod(L, "CSlideManipulator", "SetSpeed", l_slide_SetSpeed);
  SetMethod(L, "CSlideManipulator", "SetAcceleration", l_slide_SetAcceleration);
  SetMethod(L, "CSlideManipulator", "SetDeceleration", l_slide_SetDeceleration);
  SetMethod(L, "CSlideManipulator", "SetGoal", l_slide_SetGoal);
  SetMethod(L, "CSlideManipulator", "SetPrecedence", l_slide_SetPrecedence);
  SetMethod(L, "CSlideManipulator", "Enable", l_slide_Enable);
  SetMethod(L, "CSlideManipulator", "Disable", l_slide_Disable);
  SetMethod(L, "CSlideManipulator", "Destroy", l_slide_Destroy);
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

// CUnitMotion::CalcMoveCommon 0x6c1e9b (a unit without a spline point this tick): the first weapon that has a
// target and is SlavedToBody (CAiAttackerImpl::HasSlavedTarget 0x5d72b0) makes the unit turn its body. With an arc
// below 180 the turn starts when the target is more than SlavedToBodyArcRange degrees off the bow and stops inside
// half of it (motion+0x91); in between the unit keeps facing where it faces. *dir = the direction to face.
// False: no such weapon, the formation's facing applies.
bool SlavedFaceVector(Sim& sim, Unit* u, Vec3* dir) {
  UnitMotion& m = u->motion;
  UnitWeapon* w = nullptr;
  for (UnitWeapon* x : u->weapons)
    if (x->bp && x->bp->slavedToBody && HasTarget(sim, x->target)) {
      w = x;
      break;
    }
  if (!w) {
    m.slavedTurning = false;
    return false;
  }
  Vec3 tp = TargetPos(sim, w->target, false);
  if (IsNaN(tp)) return false;
  // this runs inside the motion pass: a target that already moved this beat still stands, for the original,
  // where the beat began (its transform is committed in step 13). Seen: a launcher's body starts turning after a
  // target that began to move one beat later than "its new place" would make it.
  // The aim point rides on the target's hull, so it is taken through the transform the beat began with (position
  // and orientation): out of the hull's frame of now, back in with the held one.
  if (const Entity* te = TargetEntity(sim, w->target); te && te->shapeHeld) {
    const Vec3 local = Rotate(Conj(te->orientation), Sub(tp, te->position));
    tp = Add(te->shapePos, Rotate(te->shapeOri, local));
  }
  if (const float aa = CB(u).attackAngle; aa > 0) {
    // AI.AttackAngle (ubp+0x4d0; 0x6c2350): the unit shows the target its side. The direction to the target is
    // turned about the up axis by the angle, away from the side the target is on, so the target ends up
    // AttackAngle degrees off the bow (a Battlecruiser's broadside: 60).
    Vec3 n = Norm(Vec3{tp.x - u->position.x, tp.y - u->position.y, tp.z - u->position.z});
    const Quat& q = u->orientation;
    const float fx = (q.w * q.y + q.z * q.x) * 2.0f, fz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
    const float ang = (n.z * fx - fz * n.x > 0.0f) ? aa * 0.017453292f : aa * -0.017453292f;
    double s, c;
    dmath::SinCos(static_cast<double>(ang), &s, &c);
    const float sn = static_cast<float>(s), cs = static_cast<float>(c);
    *dir = Vec3{n.x * cs + n.z * sn, n.y, n.z * cs - n.x * sn};
    return true;
  }
  const float arc = w->bp->slavedToBodyArcRange;
  if (!(180.0f > arc)) return false;
  float a = dmath::Atan2(tp.x - u->position.x, tp.z - u->position.z);
  float rel = std::fabs(WrapAngle(a - UnitHeading(u)));
  const bool was = m.slavedTurning;
  if (m.slavedTurning) {
    if (arc * 0.008726646f > rel) m.slavedTurning = false;
  } else if (rel > arc * 0.017453292f) {
    m.slavedTurning = true;
  }
  static const long dbgSl = getenv("MOHO64_DEBUG_SLAVED") ? atol(getenv("MOHO64_DEBUG_SLAVED")) : -1;
  if (dbgSl >= 0 && static_cast<long>(u->id) == dbgSl)
    Logf(LogLevel::Info, "slaved %u unit %u arc %.3f rel %.5f deg turning %d -> %d target %.4f %.4f heading %.4f deg",
         sim.tick(), u->id, arc, rel * 57.29578f, was ? 1 : 0, m.slavedTurning ? 1 : 0, tp.x, tp.z,
         UnitHeading(u) * 57.29578f);
  if (m.slavedTurning || (m.bp && m.bp->motionType == kMotionHover && m.pointNow))
    *dir = Vec3{tp.x - u->position.x, tp.y - u->position.y, tp.z - u->position.z};
  else
    *dir = Forward(u->orientation);
  return true;
}

}  // namespace moho
