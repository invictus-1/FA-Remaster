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
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>

#include "core/log.h"
#include "core/speculate.h"
#include "script/script_state.h"
#include "sim/air.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/enum_arg.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/intel.h"
#include "sim/commands.h"
#include "sim/formation.h"
#include "sim/luautil.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

// Shared with sim/damage.cpp and sim/projectile.cpp.
namespace combat {
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

// The segment p0 -> p1 as 0x4fe130 makes it: the half vector, its length (summed (y y + z z) + x x), the unit
// direction (times 1 / that length) and the centre p0 + the half vector. False for no length.
static bool MakeSegmentExe(Vec3 p0, Vec3 p1, Vec3* C, Vec3* D, float* ext) {
  const Vec3 hv{(p1.x - p0.x) * 0.5f, (p1.y - p0.y) * 0.5f, (p1.z - p0.z) * 0.5f};
  *ext = std::sqrt(hv.y * hv.y + hv.z * hv.z + hv.x * hv.x);
  if (!(*ext > 0.0f)) return false;
  const float inv = 1.0f / *ext;
  *D = {inv * hv.x, hv.y * inv, hv.z * inv};
  *C = {p0.x + hv.x, p0.y + hv.y, p0.z + hv.z};
  return true;
}

// STIMap::Intersection 0x476f30 along a segment given as centre C, direction D and half length ext: the line from
// its start C - D ext over twice ext. (The segment forms below make (C, D, ext) by 0x4fe130; a collision beam makes
// its own, CreateCollisionBeamHelper 0x6d6d47..0x6d6e0f.)
bool TerrainSegmentHitCDE(const TerrainMap* map, Vec3 C, Vec3 D, float ext, float* dist, Vec3* hit) {
  if (!map) return false;
  const Vec3 O{C.x - D.x * ext, C.y - D.y * ext, C.z - D.z * ext};
  float t;
  if (!TerrainLineHit(map, O, D, 0.0f, ext * 2.0f, &t)) return false;
  *dist = t;
  *hit = {O.x + D.x * t, O.y + D.y * t, O.z + D.z * t};
  return true;
}

// STIMap::Intersection 0x476f30 along the segment p0 -> p1, as Projectile::CheckCollision 0x69d4a8 asks it: the
// segment as centre, unit direction and half length (0x4fe130), the line from its start over twice that length.
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit) {
  if (!map) return false;
  Vec3 C, D;
  float ext;
  return MakeSegmentExe(p0, p1, &C, &D, &ext) && TerrainSegmentHitCDE(map, C, D, ext, dist, hit);
}

// STIMap::SurfaceIntersection 0x577bc0 on a map with water, when the terrain point is under the water or there is
// none (0x577c8f): the same line (0x4fe130: start, unit direction, 0 .. twice the half length) against the water
// plane n = (0, 1, 0), d = water (0x577c94..0x577cb3), by CColHitResult::PlaneIntersection 0x577540: n.D (summed
// (D.y n.y + D.z n.z) + n.x D.x) zero is no hit; t = (d - ((O.x n.x + n.y O.y) + n.z O.z)) / n.D, no hit outside
// [0, twice the half length]; the point O.x + t D.x, D.y t + O.y, D.z t + O.z.
bool WaterPlaneSegmentHitCDE(Vec3 C, Vec3 D, float ext, float water, float* dist, Vec3* hit) {
  const Vec3 O{C.x - D.x * ext, C.y - D.y * ext, C.z - D.z * ext};
  const float den = (D.y * 1.0f + D.z * 0.0f) + 0.0f * D.x;
  if (den == 0.0f) return false;
  const float t = (water - ((O.x * 0.0f + 1.0f * O.y) + 0.0f * O.z)) / den;
  if (t < 0.0f || ext * 2.0f < t) return false;
  *dist = t;
  *hit = {O.x + t * D.x, D.y * t + O.y, D.z * t + O.z};
  return true;
}
bool WaterPlaneSegmentHit(Vec3 p0, Vec3 p1, float water, float* dist, Vec3* hit) {
  Vec3 C, D;
  float ext;
  return MakeSegmentExe(p0, p1, &C, &D, &ext) && WaterPlaneSegmentHitCDE(C, D, ext, water, dist, hit);
}

// STIMap::SurfaceIntersection 0x577bc0's first test on a map with water (0x577bdf..0x577c47): when the line's near
// end (O + D near, near 0, which is always above -inf, 0x10ad410) lies under the water (water > its y), that point
// is the hit, at distance near, and neither the terrain nor the water plane is asked.
bool SegmentStartUnderWaterCDE(Vec3 C, Vec3 D, float ext, float water, float* dist, Vec3* hit) {
  const Vec3 O{C.x - D.x * ext, C.y - D.y * ext, C.z - D.z * ext};
  const float nearT = 0.0f;
  const Vec3 S{O.x + D.x * nearT, O.y + D.y * nearT, O.z + D.z * nearT};
  if (!(water > S.y)) return false;
  *dist = nearT;
  *hit = S;
  return true;
}
bool SegmentStartUnderWater(Vec3 p0, Vec3 p1, float water, float* dist, Vec3* hit) {
  Vec3 C, D;
  float ext;
  return MakeSegmentExe(p0, p1, &C, &D, &ext) && SegmentStartUnderWaterCDE(C, D, ext, water, dist, hit);
}

Entity* TargetEntity(Sim& sim, const AiTarget& t) {
  if (t.type != 1 || !t.entityId) return nullptr;
  Entity* e = sim.FindEntity(t.entityId);
  // CAiTarget::GetEntity 0x5e2ce0 is the weak pointer and nothing else: an entity a script destroyed is still
  // there until the beat's destroy queue deletes it, and HasTarget 0x5e2a10 asks only a unit (dead?) or a blip.
  // So a shell destroyed in the weapon stage is aimed at once more in the same beat's manipulator stage, and a
  // tracking shell made with it as its target is not without a target (the Cybran missile deflector destroys
  // the missile and fires its copy "at" it in one script function). The same for a unit: HasTarget asks a unit
  // only whether it is dead (0x5e2a40 vt+0x10, then vt+0x28), so a unit destroyed without a kill is a target until
  // the destroy queue deletes it. (Seen: Skirmish 2949, the unfinished 0x57 decays away in the entity stage; the
  // two torpedoes homing on it keep tracking in that beat's shell stage and lose it at 2950, OnLostTarget then,
  // and turn from 2952; ours dropped it at 2949 and they hit the water a beat early.)
  return e;
}
// CAiTarget::HasTarget 0x5e2a10
bool HasTarget(Sim& sim, const AiTarget& t) {
  if (t.type == 2) return true;
  if (t.type != 1) return false;
  if (t.fakeId) {  // a jammer's fake blip: while the fake exists (a blip is not dead while it exists)
    const Entity* f = sim.FindEntity(t.fakeId);
    return f && !f->destroyQueued;
  }
  Entity* e = TargetEntity(sim, t);
  if (!e) return false;
  if (e->kind == Entity::Kind::Unit) {
    // A target held as the army's recon BLIP (a player's order, or a blip a weapon took after its unit died): the
    // blip itself is asked (0x5e2a5a: its +0x99, which only Entity::Kill 0x679a80 sets, and nothing kills a blip),
    // so it lasts as long as the blip, past its unit's death; EntityLost says that unit is dead. Seen (Subs 1240):
    // two torpedo bombers ordered onto a submarine that died at 1239 kept the dead sub's blip at 1240 (the
    // weapon reports 4 = lost, the order ends) and only auto-attacked the next sub at 1242, when SetDesiredTarget's
    // two-beat wait was over; ours took the next sub at 1240.
    if (t.viaBlip) return static_cast<const Unit*>(e)->blip != nullptr;
    return !e->dead;
  }
  return true;
}
// CAiTarget::EntityLost 0x5e2db0: the entity behind the target (a blip's unit, GetEntity 0x5e2ce0) is killed (+0x99)
bool EntityLost(Sim& sim, const AiTarget& t) {
  if (t.type != 1) return false;
  const Entity* e = TargetEntity(sim, t);
  return e && e->dead;
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
  if (bone == kAimBoneMissing) {
    // A target point whose bone the model does not have (ues0201 lists 'UES0202', the cruiser's root): the point
    // is asked of Unit::GetBoneWorldTransform with bone -1, which is the pose's own place and half the unit's
    // SizeY above it (0x6aa637: straight up in the world's axes, the offset composed as the outer transform),
    // without the collision offset the centre below has.
    const Vec3& bp = u->poseSet ? u->poseBasePos : u->position;
    return Add(bp, Vec3{0.0f, u->bpSizeY * 0.5f, 0.0f});
  }
  if (u->blueprint) return Add(u->position, RotateM(u->orientation, u->bpCenter));  // bone -1 (0x679d19)
  return u->position;
}

namespace combat {
// ReconBlip::GetTargetPoint 0x5bf5f0 (vt+0x98) of a blip whose unit (+0x270) is gone (0x5bf666..0x5bf6d3): its own
// GetBoneWorldTransform with bone -1 (vt+0x48 = Entity's 0x679ce0, read in the blip vtable 0xe1d824: the blueprint
// (+0x6c, the unit's) centre (CollisionOffsetX, SizeY x 0.5 + CollisionOffsetY, CollisionOffsetZ) turned by its
// committed orientation, plus its committed position, 0x679d19..0x679dc5), and then SizeY x 0.5 + CollisionOffsetY
// once more on the height. So the half height goes on twice. Seen: MP3 2451, the Cybran bomber's cluster bomblets
// 0x10100007..0x1010000c on the unit 0x20001e, which died then: the original's GetCurrentTargetPosition (65.8188324,
// 70.757225, 438.194519), 0.35 above the blip; ours the blip's own place (65.8221512, 70.4072571, 438.194946).
// (A CZAR's blip gives its own place: SizeY 3, CollisionOffsetY -1.5.)
Vec3 BlipPointUnitGone(Sim& sim, const Entity* b) {
  if (!b->blueprint) return b->position;
  const UnitBpData& d = GetUnitBpData(sim.L(), *b->blueprint);
  const Vec3 c{d.collisionOffsetX, d.sizeY * 0.5f + d.collisionOffsetY, d.collisionOffsetZ};
  const Vec3 p = Add(b->position, RotateM(b->orientation, c));
  return Vec3{p.x, p.y + (d.collisionOffsetY + d.sizeY * 0.5f), p.z};
}

// GetTargetPosGun 0x5e2a90
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre) {
  if (t.type == 2) return t.pos;
  if (t.type == 1) {
    if (t.fakeId) {  // a jammer's fake blip (the CAiTarget's entity is the blip itself)
      const Entity* f = sim.FindEntity(t.fakeId);
      if (!f) return NaNVec();
      // with the centre flag its own place (0x5e2b44: +0xac); without it ReconBlip::GetTargetPoint (vt+0x98,
      // 0x5bf5f0): its unit's target point (Unit::GetTargetPoint 0x6ab340, the CAiTarget's pick +0x18) moved by the
      // fake's offset (+0x27c), and with the unit gone its own place raised by half the blueprint's SizeY plus its
      // CollisionOffsetY (0x5bf666..0x5bf6d3). Seen: MP1 7806-7813, army 6's torpedo launcher 0x600006 aims at a fake
      // of army 5's frigate 0x50007c (ues0103, target bone 0): the original's turret goal leads that bone's point, and
      // its first torpedo (7813) leaves 0.0006 lower; ours aimed at the fake's place, and its torpedo, circling,
      // dived into the seabed at 7857 (one OnImpact Random too many at 7858).
      if (!centre && f->kind == Entity::Kind::Blip) {
        const ReconBlip* fb = static_cast<const ReconBlip*>(f);
        const Unit* src = fb->source && sim.FindEntity(EntityRef(fb->source)) ? fb->source : nullptr;
        if (src) {
          const Vec3 tp = UnitTargetPoint(src, t.aimBone);
          return Vec3{fb->offset.x + tp.x, fb->offset.y + tp.y, fb->offset.z + tp.z};
        }
        if (f->blueprint) return BlipPointUnitGone(sim, f);
      }
      return f->position;
    }
    Entity* e = TargetEntity(sim, t);
    if (!e) return NaNVec();
    if (e->kind == Entity::Kind::Unit) return centre ? e->position : UnitTargetPoint(static_cast<Unit*>(e), t.aimBone);
    // A recon blip as the target's entity (Projectile:SetNewTarget with a blip: FAF's RetargetThread hands a homing
    // shell its weapon's current target, which can be a blip): the blip is no unit (vt+0x10 0x5bdb10 gives null), so
    // 0x5e2b27 asks vt+0x1c (the blip itself): with the centre flag its own place (0x5e2b44, +0xac); else
    // ReconBlip::GetTargetPoint 0x5bf5f0 (vt+0x98): its unit's target point for the CAiTarget's pick (Unit::
    // GetTargetPoint 0x6ab340) moved by the blip's offset (+0x27c), or with the unit gone its own transform's centre
    // plus the half height again (BlipPointUnitGone). Seen: Experimental2 823, the torpedo 0x1000002d (tananglertorpedo02)
    // retargeted onto the blip 0x30100000 of the UEF Atlantis 0x100000: the original's target point is y 18.535 (the
    // unit's target point), ours the blip's place, 17.5 (24 watch lines).
    if (e->kind == Entity::Kind::Blip) {
      if (centre) return e->position;
      const ReconBlip* b = static_cast<const ReconBlip*>(e);
      const Unit* src = b->source && sim.FindEntity(EntityRef(b->source)) ? b->source : nullptr;
      if (src) {
        const Vec3 tp = UnitTargetPoint(src, t.aimBone);
        return Vec3{b->offset.x + tp.x, b->offset.y + tp.y, b->offset.z + tp.z};
      }
      return BlipPointUnitGone(sim, e);
    }
    // A shell as a target (an anti-torpedo's, a missile defence's): its bone -1 too, the centre its blueprint
    // gives (half its SizeY above its origin: 0.5 with the default size), turned with the shell. Seen in the
    // original's shell lines: an anti-torpedo steers at a point 0.5 from the torpedo's origin, along its up axis.
    // For the range test too (centre): 0x5e2a90 asks a unit for its place (vt+0x14) and a recon blip for its own
    // (vt+0x1c, entity+0xac), and everything else falls to GetBoneWorldTransform(-1) whatever the flag says. Seen:
    // a missile 68 above a cruiser, its origin 1.96 from the ship (inside MinRadius 2) and its centre 2.31: the
    // original's missile defence fires.
    // The transform it reads is the COMMITTED one, entity+0x9c / +0xac (the centre flag: +0xac..+0xb4,
    // 0x5e2b44..0x5e2b55; else GetBoneWorldTransform vt+0x98, 0x5e2b6b -> 0x679ce0 on +0x9c), so a shell that has
    // already stepped in this beat is aimed at where the beat began with it (ours: its held shape).
    const Vec3& cp = e->shapeHeld ? e->shapePos : e->position;
    const Quat& cq = e->shapeHeld ? e->shapeOri : e->orientation;
    if (e->kind == Entity::Kind::Projectile && e->blueprint) return Add(cp, RotateM(cq, e->bpCenter));
    return cp;
  }
  return NaNVec();
}

// The same point for code that runs inside the motion pass (flight, a projectile's steering). An entity's new
// transform is committed at the beat's end, so a target WITHOUT an aim bone - the centre of its collision
// primitive, from the entity's own transform - still stands and lies as the beat began, also when the target
// has taken its step already. (With an aim bone the point comes from the pose, which moved with that step.)
Vec3 TargetPosBeatStart(Sim& sim, const AiTarget& t) {
  if (t.type == 1 && t.aimBone == -1 && !t.fakeId)
    if (Entity* e = TargetEntity(sim, t); e && e->shapeHeld) {
      if (e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->blueprint)
        return Add(e->shapePos, RotateM(e->shapeOri, static_cast<Unit*>(e)->bpCenter));
      if (e->kind == Entity::Kind::Projectile && e->blueprint) return Add(e->shapePos, RotateM(e->shapeOri, e->bpCenter));
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
    // Inside a beat a beam's transform, as its script and everybody else read it, is the committed one: where the
    // beat before left it (its own check of this beat works on the new one). FAF's CollisionBeam.OnImpact places
    // its area damage at self:GetPosition(1): so a beam's splash at its far end trails the turret by one beat,
    // while a direct hit lands at once. (Seen in the original's beam lines: start and end at a check are the
    // muzzle and direction of the beat before.)
    const Vec3& bp = e->shapeHeld ? e->shapePos : e->position;
    const Quat& bq = e->shapeHeld ? e->shapeOri : e->orientation;
    *rot = bq;
    *pos = bone >= 1 ? Add(bp, Mul(Forward(bq), static_cast<const CollisionBeam*>(e)->length)) : bp;
    return;
  }
  if (bone < 0 || !e->skeleton || bone >= e->skeleton->Count()) {
    *pos = e->position;
    *rot = e->orientation;
    return;
  }
  const auto& bones = e->skeleton->bones();
  float s = e->meshScale * e->scale[0];
  if (e->kind == Entity::Kind::Projectile || e->kind == Entity::Kind::Prop) {
    // Entity::GetBoneWorldTransform 0x679ce0 for an entity that is no unit (a projectile, a prop: Prop has no
    // override of slots 0x48 / 0x4c): its CURRENT transform (entity+0x9c,
    // which a step taken earlier in this beat has not changed) x GetBoneLocalTransform 0x679e20 = the inverse of
    // the skeleton's rest-pose inverse, place times the entity's scale; one composition (0x549c20), no walk down
    // the chain. Seen: a wreck hanging on FAF's sinker shell by the shell's bone 'anchor'; with the chain's
    // product the wreck stood one float step beside the original's, and a ship that turned to face it ended
    // 1e-5 off in heading.
    const Bone& b = bones[static_cast<size_t>(bone)];
    const Vec3& ep = e->shapeHeld ? e->shapePos : e->position;
    const Quat& eq = e->shapeHeld ? e->shapeOri : e->orientation;
    const Vec3 lp{b.entPos.x * s, (e->meshScale * e->scale[1]) * b.entPos.y, (e->meshScale * e->scale[2]) * b.entPos.z};
    *rot = ComposeQ(eq, b.entRot);
    const Vec3 r = RotateM(eq, lp);
    *pos = {ep.x + r.x, ep.y + r.y, ep.z + r.z};
    return;
  }
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
  ++e->poseVersion;
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
  ++e->poseVersion;
}

const std::vector<bool>& BpCategoryMask(Sim& sim, const char* name) {
  static const SimCache<std::map<std::string, std::vector<bool>>> cacheSlot;  // (over the sim's blueprint indices)
  auto& cache = cacheSlot.For(sim);
  auto it = cache.find(name);
  if (it == cache.end()) {
    std::vector<bool> bits(static_cast<size_t>(sim.blueprints().EntityCount()), false);
    auto c = sim.blueprints().Categories().find(name);
    if (c != sim.blueprints().Categories().end())
      for (int i : c->second)
        if (i >= 0 && static_cast<size_t>(i) < bits.size()) bits[static_cast<size_t>(i)] = true;
    it = cache.emplace(name, std::move(bits)).first;
  }
  return it->second;
}

bool BpInCategory(Sim& sim, const BlueprintInfo* bp, const char* name) {
  if (!bp || bp->entityIndex < 0) return false;
  const std::vector<bool>& m = BpCategoryMask(sim, name);
  return static_cast<size_t>(bp->entityIndex) < m.size() && m[static_cast<size_t>(bp->entityIndex)];
}

// ---- blueprint data --------------------------------------------------------------------------------

const WeaponBp& GetWeaponBp(lua_State* L, int bpRef) {
  // (by the table's address in the sim state)
  static const SimCache<std::map<const void*, std::unique_ptr<WeaponBp>>> cacheSlot;
  auto& cache = cacheSlot.For(*Sim::From(L));
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
  bool autoSurfaceToAttack = true;  // AI.AutoSurfaceToAttack (ubp+0x4cc; RUnitBlueprintAI ctor 0x51f89c: true)
  std::vector<std::string> targetBones;
  float sizeY = 1, sizeZ = 1;
  float intel[5] = {0, 0, 0, 0, 0};  // Vision, WaterVision, Radar, Sonar, Omni radii
  bool radarStealth = false, sonarStealth = false, cloak = false;
  bool command = false, benign = false, structure = false;
};

static const CombatBpData& GetCombatBpData(Sim& sim, lua_State* L, const BlueprintInfo& bp) {
  static const SimCache<std::map<const BlueprintInfo*, CombatBpData>> cacheSlot;
  auto& cache = cacheSlot.For(sim);
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
  d.autoSurfaceToAttack = lu::Bool(L, ai, "AutoSurfaceToAttack", true);
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
  // times 1 / g (0x62d1a0 divss, then 0x62d1b9..0x62d1c4 and 0x62d273..0x62d282), not over g: 1 / 4.9 is inexact
  const float sd = std::sqrt(disc), invG = 1.0f / G;
  float t = -0.0f - (sd - vu) * invG;
  if (t < 0) t = -0.0f - ((-0.0f - vu) - sd) * invG;
  if (t < 0) return NaNVec();
  Vec3 fall{v.x * t, v.y * t - 0.5f * G * t * t, v.z * t};
  return Sub(T, fall);
}

// UnitWeapon::CanFire 0x6d4c80
bool CanFire(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  if (u->stunned) return false;
  if (u->unitStates.Has(US("Busy"))) return false;
  const CombatBpData& cb = CB(u);
  if (cb.canFly && u->layer != "Air") return false;
  if (cb.needUnpack && !u->unitStates.Has(US("Immobile"))) return false;
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
      if (!u->unitStates.Has(US("MakingAttackRun"))) return false;
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
  if (e->kind == Entity::Kind::Blip) {
    // CAiTarget::PickTargetPoint 0x5e28f0 asks a blip for its unit through the blip's weak link +0x270 (0x5bf4b0),
    // which the unit's destruction cleared: a blip left over from a destroyed unit gives -1 and no draw. (Ours
    // keeps the blip's source pointer for the blip's memory; destroyQueued stands for the link's end, as in
    // SetCommandEntityTarget.) Seen: MP1 9800, army 6's Attack on the blip 0x30500077 of army 5's 0x500090
    // (destroyed earlier): the original makes no 0x6aaf94 draw for it, ours made one.
    const Entity* src = static_cast<ReconBlip*>(e)->source;
    if (!src || src->destroyQueued) return -1;
    e = static_cast<ReconBlip*>(e)->source;
  }
  if (!e || e->kind != Entity::Kind::Unit) return -1;
  const CombatBpData& cb = CB(static_cast<Unit*>(e));
  if (cb.targetBones.empty()) return -1;
  return static_cast<int>(sim.IntRange(static_cast<uint32_t>(cb.targetBones.size())));
}

int TargetBoneOf(const Entity* e, const std::string& name);

// CAiTarget::UpdateTarget 0x5d55b0 for a target that keeps its pick (a shell's): the draw, and the bone it names
// (-1: no target bones, or no unit).
int TargetPointBone(Sim& sim, Entity* e) {
  const int idx = TargetPointDraw(sim, e);
  if (idx < 0 || !e) return -1;
  if (e->kind == Entity::Kind::Blip) e = static_cast<ReconBlip*>(e)->source;
  if (!e || e->kind != Entity::Kind::Unit || !e->skeleton) return -1;
  const CombatBpData& cb = CB(static_cast<Unit*>(e));
  return idx < static_cast<int>(cb.targetBones.size()) ? TargetBoneOf(e, cb.targetBones[static_cast<size_t>(idx)]) : -1;
}

// The bone a target point's name stands for; a name the model does not have is still a target point
// (kAimBoneMissing: UnitTargetPoint).
int TargetBoneOf(const Entity* e, const std::string& name) {
  const int b = e->skeleton ? e->skeleton->Find(name) : -1;
  return b >= 0 ? b : kAimBoneMissing;
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
  return TargetBoneOf(e, cb.targetBones[i]);
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
    // (Unit::GetTargetPoint 0x6ab340 for each index: a name the model lacks gives the point half the unit's
    // height above its origin, which for a ship is above the water: it is in the list and can be drawn)
    const int b = TargetBoneOf(e, name);
    const Vec3 p = UnitTargetPoint(tu, b);
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
// 0x6d78f0 (from UnitWeapon::SetTarget 0x6d5f84): each entry's count goes down by one (0x6d7910..0x6d791a), and the
// entry goes when its entity is gone (its WeakPtr empty: deleted, not merely queued for it), when that entity moved
// in this beat (its position, +0xac, against the one before, +0xc8, by memcmp 0x4f0a50: 0x6d7924..0x6d7938), or
// when the count is down to 0 (0x6d793a). The entry keeps no place of its own: an entity that moved some beats ago
// and stands again stays on the list.
void DecayBlacklist(Sim& sim, UnitWeapon* w) {
  auto& bl = w->blacklist;
  bl.erase(std::remove_if(bl.begin(), bl.end(),
                          [&](UnitWeapon::BlackEntry& b) {
                            const int left = --b.counter;
                            Entity* e = sim.FindEntity(b.id);
                            if (!e) return true;
                            const Vec3& before = e->kind == Entity::Kind::Unit
                                                     ? static_cast<const Unit*>(e)->lastPosition
                                                     : e->kind == Entity::Kind::Projectile
                                                           ? static_cast<const Projectile*>(e)->prevPos
                                                           : e->position;
                            if (!SameBits(e->position, before)) return true;
                            return left <= 0;
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

// UnitWeapon::PickNewTargetAimSpot 0x6d5de0: which of its target's target points the weapon aims at (one draw
// when the target has target bones)
static void PickNewTargetAimSpot(Sim& sim, UnitWeapon* w) {
  Entity* e = TargetEntity(sim, w->target);
  int aimBone = -1;
  if (w->bp->aboveWaterTargetsOnly) PickPointAboveWater(sim, e, &aimBone, false);
  else if (w->bp->belowWaterTargetsOnly) PickPointBelowWater(sim, e, &aimBone, false);
  else aimBone = PickAimBone(sim, e);
  w->target.aimBone = aimBone;
}

static void SetTarget(Sim& sim, UnitWeapon* w, const AiTarget& nt);

// CAcquireTargetTask's impact listener (0x5d97f0 for a shell, 0x5d9830 for a beam; the handler 0x5d9630), only
// for a weapon with ReTargetOnMiss. A shot that hit its target (0) or something that does not count (2) clears
// the weapon's miss count and the task's put-off. A MISS (1) while neither the unit nor the target has moved and
// the target is not in the air counts one up and makes the weapon aim at another of the target's points; the
// unit's first weapon on the unit's ordered target is put off for 30 beats after 5 misses (the attack task then
// hears "cannot hit from here"), any weapon drops and blacklists its own target after 10.
void WeaponImpactEvent(Sim& sim, UnitWeapon* w, int event) {
  if (!w || !w->bp || !w->bp->reTargetOnMiss || !w->unit) return;
  if (event != 1) {
    if (event == 0 || event == 2) {
      w->suppress = 0;
      w->missCount = 0;
    }
    return;
  }
  Unit* u = w->unit;
  // (each "did it move" is a memcmp of the position, entity +0xac, with the one before, +0xc8: 0x5d96a0, 0x5d96c9,
  // 0x5d9727, 0x5d9758. Both are the COMMITTED transforms: "moved between the last two AdvanceCoords", also for an
  // entity that has already taken its step in this beat. Then its committed position is what the step started
  // from: a unit's lastPosition, the one before its prevLastPosition; a shell's prevPos, the one before
  // prevPosBefore.)
  auto moved = [&sim](const Entity* e) {
    if (e->kind == Entity::Kind::Unit) {
      const Unit* x = static_cast<const Unit*>(e);
      if (x->lastPosTick == sim.tick()) return !SameBits(x->lastPosition, x->prevLastPosition);
      return !SameBits(x->position, x->lastPosition);
    }
    if (e->kind == Entity::Kind::Projectile) {
      const Projectile* x = static_cast<const Projectile*>(e);
      if (x->prevPosTick == sim.tick()) return !SameBits(x->prevPos, x->prevPosBefore);
      return !SameBits(x->position, x->prevPos);
    }
    return false;
  };
  if (!u->weapons.empty() && u->weapons[0] == w) {  // (weapon +0xa4 == 0: the unit's first weapon)
    Entity* de = u->desiredTarget.type == 1 ? TargetEntity(sim, u->desiredTarget) : nullptr;
    if (de && IsMobile(u) && !moved(u) && EntityLayerBit(de) != 16 && !moved(de)) {
      if (++w->missCount > 5) w->suppress = 30;  // ([0xf599ec])
      else PickNewTargetAimSpot(sim, w);
      return;
    }
  }
  Entity* te = TargetEntity(sim, w->target);
  if (!te || moved(u) || EntityLayerBit(te) == 16 || moved(te)) return;
  if (++w->missCount > 10) {
    // 0x6d77b0: on the blacklist for 10 target changes, then no target
    const uint32_t ref = EntityRef(te);
    bool found = false;
    for (auto& b : w->blacklist)
      if (b.id == ref) {
        b.counter = 10;
        found = true;
      }
    if (!found) w->blacklist.push_back({ref, 10});
    SetTarget(sim, w, AiTarget{});
    return;
  }
  PickNewTargetAimSpot(sim, w);
}

// Whether a shot met what it was sent at (0x62a900). An entity target: the entity hit (impact type 5 and up) is
// the target, or hangs on it. A ground target: nothing was hit, and the impact is terrain, water, air or
// underwater. No target: yes.
bool ShotHitTarget(Sim& sim, int impactType, const AiTarget& t, const Entity* hit) {
  if (t.type == 1) {
    if (!hit || impactType < 5) return false;
    const Entity* te = TargetEntity(sim, t);
    if (hit == te) return true;
    if (hit->kind == Entity::Kind::Unit) {
      const Unit* hu = static_cast<const Unit*>(hit);
      const Entity* parent = hu->parentId ? sim.FindEntity(hu->parentId) : nullptr;
      return parent == te;  // (both gone counts as met, as in the original)
    }
    return te == nullptr;
  }
  if (t.type == 2) return !hit && impactType >= 1 && impactType <= 4;
  return true;
}

// The event a shot sends (Projectile::Impact 0x69e084, CollisionBeamEntity::CheckCollision 0x673337): 0 it met
// its target, 2 it met a shield (entity id family 4) or, a shell, another shell (impact types 5 and 6): no
// verdict, 1 it missed.
int ShotEvent(Sim& sim, int impactType, const AiTarget& t, const Entity* hit, bool shell) {
  if (ShotHitTarget(sim, impactType, t, hit)) return 0;
  if (shell && (impactType == 5 || impactType == 6)) return 2;
  if (hit && hit->kind == Entity::Kind::Shield) return 2;
  return 1;
}

namespace {
UnitWeapon* TargetWeapon(Sim& sim, Unit* u, const AiTarget& t);  // (below)
}  // namespace

// UnitWeapon::SetTarget 0x6d5f30
static void SetTarget(Sim& sim, UnitWeapon* w, const AiTarget& nt) {
  if (w->target == nt) return;
  bool had = w->target.type != 0, has = nt.type != 0;
  if (had && !has) WeaponCall(sim, w, "OnLostTarget");
  if (has) DecayBlacklist(sim, w);
  // 0x6d5f89: the weapon's UNIT leaves the old target's shooters (whether or not another weapon of the unit still
  // holds that target), unless the old target is dead or going
  if (Entity* old = TargetEntity(sim, w->target); old && !old->dead && !old->destroyQueued && w->unit) {
    const uint32_t ref = EntityRef(w->unit);
    old->shooters.erase(std::remove(old->shooters.begin(), old->shooters.end(), ref), old->shooters.end());
  }
  w->target = nt;
  Entity* e = TargetEntity(sim, w->target);
  if (e && e->kind == Entity::Kind::Unit) w->target.mobile = IsMobile(static_cast<Unit*>(e));
  if (e && e->kind == Entity::Kind::Unit && w->target.viaBlip) {
    const Unit* tu = static_cast<const Unit*>(e);
    w->target.blipId = w->target.fakeId ? w->target.fakeId : tu->blip ? EntityRef(tu->blip) : 0;
  }
  // 0x6d5fce..0x6d6006: the unit's attacker is asked which of its weapons takes the new target
  // (CAiAttackerImpl::GetTargetWeapon 0x5d6dc0, vt+0x38: nothing for a unit being built, else the first weapon in
  // order whose CanAttackTarget 0x6d5720 says yes; with none, GetPrimaryWeapon): only that weapon's change goes to
  // the unit's target blip (unit+0x4d8 / +0x68e, not modelled). The asking itself draws: CanAttackTarget of a
  // weapon with AboveWaterTargetsOnly / BelowWaterTargetsOnly on a target on the seabed picks one of its target
  // points (Targetable). Seen (probe mp1_10366): MP1 10366, army 6's destroyer 0x60009d (uas0201): its Torpedo1
  // takes 0x70003e (on the seabed); between the UpdateTarget pick and the aim spot the original draws in
  // PickTargetPointAboveWater from 0x6d55fb (the FrontTurret, weapon 0, asked first); ours drew nothing there.
  if (w->unit && !w->unit->beingBuilt) (void)TargetWeapon(sim, w->unit, nt);
  PickNewTargetAimSpot(sim, w);
  // 0x6d6131: the weapon's UNIT joins the target's shooters (once), unless the target is dead or going
  if (e && !e->dead && !e->destroyQueued && w->unit) {
    const uint32_t ref = EntityRef(w->unit);
    if (std::find(e->shooters.begin(), e->shooters.end(), ref) == e->shooters.end()) e->shooters.push_back(ref);
  }
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
  // (the views' shared copies: WeaponView, ui request 37)
  w->disallowSet = sim.InternCategorySet(w->disallow);
  w->onlyAllowSet = sim.InternCategorySet(w->onlyAllow);
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
  u->blipCacheBlip.clear();
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
    // EntitiesAroundPoint 0x722350 keeps a unit when R^2 > dx^2 + dz^2, dx = its x - the point's (comiss 0x7224b5,
    // jbe: strict, and a NaN distance is left out); ours kept one at exactly R.
    if (!(R * R > Dist2XZ(t->position, p))) return;
    if (a < 0 || a >= 16 || !HasBlip(t, a)) return;
    u->blipCache.push_back(t);
  });
  std::sort(u->blipCache.begin(), u->blipCache.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  // EntitiesAroundPoint walks the entity grid, so the list comes in GRID order (rect_queries.md 2.4: rows by z,
  // columns by x, in a bucket the most recently registered first; ids play no part). FindBestEnemy depends on it:
  // the current target only holds its place against candidates that come after it.
  // And only the grid: a unit with no collision primitive is in none of its cells and is never found (0x6acda0, the
  // rebuild's EntitiesAroundPoint 0x722350 with the unit mask 0x100). Seen: MP3, army 1's urb1202 0x1001ba, the
  // stalled upgrade of the extractor 0x100100 (FAF's Unit.OnStartBuild gives an upgrade SetCollisionShape('None'),
  // Unit.lua 2905): never in the original's lists (probe mp3_9469b: army 0's interceptor 0x1a, 9455-9475), ours
  // listed it after the others.
  if (!u->blipCache.empty() && sim.entityGrid().ready()) {
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
    u->blipCache.swap(ordered);
  }
  // 0x6acf27: behind a unit whose jammer is on come the blips the army holds of it that it does not know to be
  // fakes (ReconGetJamingBlips 0x5c20c0: every blip of the unit in the army's database, the real one too). The
  // list is only as fresh as the weapon that asks needs it (a 20-beat weapon reads what a 10-beat one built), and
  // a blip does not die with its unit: so a weapon can take the blip of a jammer that died since the list was
  // made. The query collects them in a set sorted by the blips' ADDRESSES (0x5c3a90); here the real blip, made
  // first, and then the fakes in the order they were made, which is what a heap that only grows gives.
  u->blipCacheFake.clear();
  {
    std::vector<Unit*> withBlips;
    for (Unit* t : u->blipCache) {
      withBlips.push_back(t);
      u->blipCacheBlip.push_back(0);
      u->blipCacheFake.push_back(0);
      if (!(t->intel && t->intel->has[9] && t->intel->on[9])) continue;
      if (t->blip && (t->recon[a] & kReconInUse) && !(t->recon[a] & 0x20)) {
        withBlips.push_back(t);
        u->blipCacheBlip.push_back(1);
        u->blipCacheFake.push_back(0);
      }
      for (const ReconBlip* f : t->fakeBlips) {
        if (f->destroyQueued || !(f->rflags[a] & kReconInUse) || (f->rflags[a] & 0x20)) continue;
        withBlips.push_back(t);
        u->blipCacheBlip.push_back(1);
        u->blipCacheFake.push_back(EntityRef(f));
      }
    }
    u->blipCache.swap(withBlips);
  }
  {
    // MOHO64_DEBUG_BLIPCACHE=<unit id>: every rebuild of that unit's list of enemies in reach
    static const long dbgId = getenv("MOHO64_DEBUG_BLIPCACHE") ? atol(getenv("MOHO64_DEBUG_BLIPCACHE")) : -1;
    if (dbgId >= 0 && static_cast<long>(u->id) == dbgId) {
      std::string ids;
      for (size_t i = 0; i < u->blipCache.size(); ++i)
        ids += " " + std::to_string(u->blipCache[i]->id) + (u->blipCacheFake[i] ? "f" : u->blipCacheBlip[i] ? "b" : "");
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
  // blocked only when the hit point is valid (0x5657f0: no NaN part) and the segment's length, its squares summed
  // z, y, x, reaches the hit's distance (0x5d66d7: fcompi / jb: a NaN length is not blocked), as
  // CheckBlockingTerrain 0x5910a0 (ai_threat.cpp) tests it
  auto valid = [](const Vec3& v) { return !std::isnan(v.x) && !std::isnan(v.y) && !std::isnan(v.z); };
  auto reaches = [&](const Vec3& p, const Vec3& q) {
    const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
    return std::sqrt((dz * dz + dy * dy) + dx * dx) >= d;
  };
  if (arc == 0) return TerrainSegmentHit(m, a, b, &d, &h) && valid(h) && reaches(a, b);
  Vec3 step = Mul(Sub(b, a), 0.25f);
  float sl = std::sqrt((step.z * step.z + step.y * step.y) + step.x * step.x);  // (0x5d6740..0x5d6758)
  const float prof[4] = {0.707f, 0.293f, -0.293f, -0.707f};
  float k = arc == 1 ? 0.5f : 2.0f;
  Vec3 cur = a;
  for (int i = 0; i < 4; ++i) {
    Vec3 nxt{cur.x + step.x, cur.y + step.y + prof[i] * sl * k, cur.z + step.z};
    if (TerrainSegmentHit(m, cur, nxt, &d, &h) && valid(h) && reaches(cur, nxt)) return true;  // (0x5d693e, jae)
    cur = nxt;
  }
  return false;
}

bool Blacklisted(const UnitWeapon* w, const Entity* e) {
  for (const auto& b : w->blacklist)
    if (b.id == EntityRef(e)) return true;
  return false;
}

// IsTargetExempt 0x5d7340: reclaim / capture targets are never shot: the unit's own (its queue's Reclaim and
// Capture orders), and (0x5d7423..0x5d74fd) a unit BeingCaptured (IsUnitState 0x1b) that one of the attacker's
// army's ENGINEER units (army vt+0x9c with the category "ENGINEER" 0xe1a690; not dead, not being destroyed) has for
// its focus (unit+0x4d0). Seen: Engineers 2678, a SACU cheat-spawned beside a unit three engineers of its army are
// capturing; the original's leaves it alone, ours shot it
bool TargetExempt(Sim& sim, const Unit* u, const Entity* e) {
  if (!e) return false;
  for (const auto& c : u->commands)
    if ((c->type == CommandType::Reclaim || c->type == CommandType::Capture) && c->targetId == EntityRef(e)) return true;
  if (e->kind != Entity::Kind::Unit || !static_cast<const Unit*>(e)->unitStates.Has(US("BeingCaptured"))) return false;
  const uint32_t ref = EntityRef(e);
  for (Unit* x : sim.units())
    if (x->army == u->army && Alive(x) && x->focusId == ref && BpInCategory(sim, x->blueprint, "ENGINEER")) return true;
  return false;
}

// FindBestEnemy 0x5d7cf6: the candidate inside the playable rect, edges included (no margin, no army flag)
bool InMap(Sim& sim, Vec3 p) {
  if (!sim.map()) return true;
  const int* r = sim.playableRect;
  return !(static_cast<float>(r[0]) > p.x) && !(static_cast<float>(r[1]) > p.z) && !(p.x > static_cast<float>(r[2])) &&
         !(p.z > static_cast<float>(r[3]));
}

// CAiAttackerImpl::FindBestEnemy 0x5d7a10
// isBlip (one flag per list entry, Unit::blipCacheBlip): the entry is the unit's recon blip, an entity of its own
// in the original: the blip's own flags and place count, not the unit's, and what comes back is the blip
// (*bestIsBlip). Without the flags every entry is a unit.
// fakeIds (Unit::blipCacheFake) / bestFake: the entry is one of the unit's FAKE blips; its place and its flags
// are the fake's.
Unit* FindBestEnemy(Sim& sim, UnitWeapon* w, const std::vector<Unit*>& list, float range, bool useAngle,
                    const std::vector<uint8_t>* isBlip = nullptr, bool* bestIsBlip = nullptr,
                    const std::vector<uint32_t>* fakeIds = nullptr, uint32_t* bestFakeOut = nullptr) {
  if (bestIsBlip) *bestIsBlip = false;
  if (bestFakeOut) *bestFakeOut = 0;
  if (list.empty()) return nullptr;
  Unit* u = w->unit;
  int a = u->army ? u->army->index - 1 : -1;
  Unit* best = nullptr;
  bool bestBlip = false;
  uint32_t bestFake = 0;
  const uint32_t curFake = w->target.type == 1 ? w->target.fakeId : 0;
  float bestScore = std::numeric_limits<float>::infinity();
  int bestStatus = 3, bestPrio = 9999;
  Vec3 P = u->position;
  Vec3 fwd = WeaponForward(w);
  Entity* cur = TargetEntity(sim, w->target);
  const bool curBlip = cur && bestIsBlip && w->target.viaBlip;
  const int n = static_cast<int>(w->priorities.size());
  for (size_t li = 0; li < list.size(); ++li) {
    Unit* e = list[li];
    const bool blip = isBlip && li < isBlip->size() && (*isBlip)[li] != 0;
    const uint32_t fakeRef = blip && fakeIds && bestFakeOut && li < fakeIds->size() ? (*fakeIds)[li] : 0;
    const ReconBlip* fake = nullptr;
    if (blip && fakeIds && li < fakeIds->size() && (*fakeIds)[li] != 0) {
      if (!fakeRef || !bestIsBlip || !e) continue;  // (a caller that cannot hold a fake as its target)
      const Entity* fe = sim.FindEntity(fakeRef);
      if (!fe || fe->destroyQueued) continue;
      fake = static_cast<const ReconBlip*>(fe);
    } else if (blip) {
      if (!bestIsBlip || !e || e->destroyQueued || !e->blip || e->blip->destroyQueued) continue;
    } else if (!Alive(e)) {
      continue;
    } else if (!HasBlip(e, a)) {
      // 0x5d7b6a..0x5d7b9f: an entry that is a unit (not a blip) counts only while the attacker's army holds a blip
      // of it NOW (its recon DB's ReconGetBlip 0x5c15a0, a map lookup): the list is only as fresh as the weapon that
      // built it (GetBlipsInRange's period), so it can hold a unit whose blip the army has let go since. Seen: MP3
      // 9469, army 0's interceptor 0x1a on patrol; its list of 9467 holds army 1's Corsair 0x1000be, whose blip army 0
      // lets go in its recon tick of 9468 (out of sight); the original's patrol scan of 9469 passes it over
      // (probe mp3_9469b: FindBestEnemy never reaches its blacklist test for it) and attacks only at 9475, the
      // interceptor 0x100052 (passing over the Corsair 0x10010f, let go at 9474); ours took 0x1000be at 9469.
      continue;
    }
    const Vec3 ep = fake ? fake->position : blip ? e->blip->position : e->position;
    float d2 = Dist2XZ(P, ep);
    if (d2 > range * range) continue;
    if (!IsEnemy(u->army, e->army)) continue;
    const CombatBpData& ecb = CB(e);
    if (ecb.benign) continue;
    if (!blip) {  // (a blip has no parent, is never being built, has no unit states and is on no blacklist)
      if (e->parentId) {
        if (e->layer == "Air" || e->beingBuilt) continue;
      } else if (e->layer == "Air" && e->beingBuilt) {
        continue;
      }
      if (Blacklisted(w, e) || TargetExempt(sim, u, e) || e->unitStates.Has(US("DoNotTarget"))) continue;
    }
    if (e->layer != "Air" && !InMap(sim, ep)) continue;
    if (!Targetable(sim, w, e)) continue;
    int s = SolutionStatus(w, ep, &d2);
    if (s == 1 || s == 2) {
      if (!(IsMobile(u) && (w->bp->autoInitiateAttackCommand || w->bp->slavedToBody))) continue;
    }
    if (TerrainBlocked(sim, u, ep, w->bp->ballisticArc)) continue;
    if (CB(u).needUnpack && s != 0) continue;
    // the angle score: the normalised copy 0x44f7e0, then its dot with the forward summed y, z, x (0x5d7e53..0x5d7e88)
    float score = d2;
    if (useAngle) {
      const Vec3 nv = Norm(Sub(P, ep));
      score = (nv.y * fwd.y + nv.z * fwd.z) + nv.x * fwd.x;
    }
    if (s != 0 && !w->bp->autoInitiateAttackCommand) score = useAngle ? score + 4 : score * 4;
    bool seen = a >= 0 && a < 16 && ((fake ? fake->rflags[a] : e->recon[a]) & 0x10);
    int prio = 9999;
    const bool curIsE = cur == e && curBlip == blip && curFake == fakeRef;
    for (int i = 0; i < n; ++i) {
      if (i > bestPrio) break;
      if (s > bestStatus && best) break;
      if (!InSet(w->priorities[static_cast<size_t>(i)], e->blueprint)) continue;
      if (seen) prio = i;
      const bool curIsBest = cur == best && curBlip == bestBlip && curFake == bestFake;
      bool take = prio < bestPrio || (!cur && score < bestScore) || (cur && !curIsBest && (score < bestScore || curIsE));
      if (take) {
        best = e;
        bestBlip = blip;
        bestFake = fakeRef;
        bestScore = score;
        bestStatus = s;
        bestPrio = prio;
      }
    }
  }
  if (bestIsBlip) *bestIsBlip = bestBlip;
  if (bestFakeOut) *bestFakeOut = bestFake;
  return best;
}

UnitWeapon* PrimaryWeapon(Unit* u) { return u->weapons.empty() ? nullptr : u->weapons[0]; }
// CAiAttackerImpl::IsTooClose 0x5d7210: nothing for a unit being built; else every weapon in order that can attack
// the target (CanAttackTarget 0x6d5720) is asked its status (0x6d5d80): an enabled one with the target in its
// solution (0) ends it at once with no; one too close (1) makes the answer yes unless a later enabled weapon has it
// in its solution. Seen: MP1 10373, army 6's five Aeon gunships (uaa0203) begin an Attack on army 7's 0x70003e (on
// the seabed) out of range: the original asks each of their weapons here (one PickTargetPointAboveWater draw each,
// AboveWaterTargetsOnly), ours asked only the target weapon's status (no draw).
bool AttackerTooClose(Sim& sim, Unit* u, const AiTarget& t) {
  if (u->beingBuilt) return false;
  bool tooClose = false;
  for (UnitWeapon* w : u->weapons) {
    if (!w->bp || !CanAttackTarget(sim, w, t)) continue;
    const int st = TargetStatus(sim, w, t);
    if (st == 0) {
      if (w->enabled) return false;
    } else if (st == 1) {
      tooClose = true;
    }
  }
  return tooClose;
}

// GetTargetWeapon 0x5d6dc0
UnitWeapon* TargetWeapon(Sim& sim, Unit* u, const AiTarget& t) {
  for (UnitWeapon* w : u->weapons)
    if (w->bp && CanAttackTarget(sim, w, t)) return w;
  return nullptr;
}

// LeashOrInvisible 0x5d8ad0 (the leash part): while Attacking, the unit is more than GuardReturnRadius (3D)
// from its leash point - the guarded unit's position, else u+0x4e8 - when there is one
bool LeashOrInvisible(Sim& sim, Unit* u) {
  if (!u->unitStates.Has(US("Attacking"))) return false;
  Vec3 leash = u->leashPos;
  if (u->guardedId)
    if (Entity* g = sim.FindEntity(u->guardedId)) leash = g->position;
  if (BitsZero(leash)) return false;  // (0x5d8b5d..0x5d8b6d: memcmp 0x4f0a50 with the zero vector)
  float dx = u->position.x - leash.x, dy = u->position.y - leash.y, dz = u->position.z - leash.z;
  float r = CB(u).guardReturnRadius;
  // the distance itself, the squares summed z, y, x (0x5d8bad..0x5d8bc6), the root 0x452fc0, against the radius
  return std::sqrt((dz * dz + dy * dy) + dx * dx) > r;
}

void RelinkSuspendedAttackThread(Unit* u);  // (below AttackData)
void ReportState(Unit* u, int code) {
  if (u->attackState != code) {
    u->attackState = code;
    u->attackStateSignal = true;
    // the attack task's listener (Listener_AiAttackerEvent_CUnitAttackTargetTask::Receive 0x5f3ee0): after it sets
    // the task's state, the thread's counter goes to 0 and a SUSPENDED thread (the in-range branch's -2, 0x5f3a63;
    // ours: waitUntil = ~0) is linked back at the command stage's new end (0x5f3f9f..0x5f3fd5). The report comes in
    // the army's attacker stage, so the task runs after every other command thread in this beat's command stage.
    // Seen (probe mp1_5234): MP1 5220, army 1's frigate 0x100056 comes in reach (5219: -2) and its weapon reports
    // (5220); at 5233 its new formation Attack runs after the other ten members' in the original (their slots are
    // reserved first: its place (498.98, 200.55), its square (482,184)..(515,217)); ours kept its old place in the
    // run list, so its slot was the free raw place and its square left its cell out.
    RelinkSuspendedAttackThread(u);
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
  // The order's CAiTarget names the entity it was given (0x5d8cc8 -> 0x5d5980): for a player's order that is the army's
  // recon BLIP, which lives on after its unit died and after the unit is gone, until the recon database lets it go
  // (DestroyIfUnused, intel.cpp). The order stands while that entity is neither dead (+0x99) nor going (+0x1b9,
  // 0x5d8cd8..0x5d8ce8). Seen: MP2 7382, army 0's Corsair 0x10c on a player's Attack of a structure's blip; the
  // structure was destroyed at 7380, its blip lives on: the original's acquire task keeps waiting, the attack ends in
  // that beat's command stage, and the weapon auto-attacks only at 7384 (the Stop's two beats); ours looked at the gone
  // unit, ordered the auto-attack at once (7382) and flew a different attack.
  if (c.targetViaBlip && c.targetBlipRef && !c.targetFakeId) {
    const Entity* b = sim.FindEntity(c.targetBlipRef);
    if (b) return b->dead || b->destroyQueued;
  }
  const Entity* e = sim.FindEntity(c.targetId);
  // (0x5d5980: the entity the order names; for a blip taken after its unit died that is the blip, which lives)
  if (c.targetFakeId) {  // a jammer's fake blip: the order stands while the fake does
    const Entity* f = sim.FindEntity(c.targetFakeId);
    return !f || f->destroyQueued;
  }
  // (a player's order names the army's blip: it stands while the blip does, also after its unit died)
  if (e && c.targetViaBlip && e->kind == Entity::Kind::Unit)
    return e->destroyQueued || !static_cast<const Unit*>(e)->blip;
  return !e || e->dead || e->destroyQueued;
}

// CAcquireTargetTask::TaskTick 0x5d8d10. Returns the ticks until it runs again.
int AcquireTick(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  const WeaponBp& b = *w->bp;
  const CombatBpData& cb = CB(u);
  int period = AcquirePeriod(w);
  int ret = period;
  if (cb.needUnpack && (u->unitStates.Has(US("Moving")) || u->unitStates.Has(US("TransportLoading")) ||
                        u->unitStates.Has(US("WaitingForTransport"))))
    return ret;
  if (!SameBits(u->position, u->lastPosition))  // (0x5d8e7f..0x5d8e99, memcmp 0x4f0a50)
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
  if (!dHas || EntityLost(sim, d)) code = 4;  // (0x5d8fca: EntityLost 0x5e2db0, then HasTarget)
  else if (LeashOrInvisible(sim, u) || TargetExempt(sim, u, TargetEntity(sim, d))) code = 5;
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
  const bool engaging = u->unitStates.Has(US("Attacking")) &&
                        ((b.autoInitiateAttackCommand && !CheckAutoInitiate(sim, w)) || cb.needUnpack || code == 1);
  const bool keeps = !engaging && !b.alwaysRecheckTarget && TargetStatus(sim, w, w->target) == 0 &&
                     CanAttackTarget(sim, w, w->target) && HasTarget(sim, w->target) &&
                     !TargetExempt(sim, u, TargetEntity(sim, w->target));
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
    if (CheckSilo(w) && !u->stunned && !u->unitStates.Has(US("Busy"))) {
      float reach = WeaponReach(w), bestD = std::numeric_limits<float>::infinity();
      float wl = WaterLevel(sim);
      // The candidates come from the entity grid (EntitiesAroundPoint 0x722350, mask projectiles: the cells of the
      // square about the unit, row by row, in each cell the shell registered last first), and the nearest is taken
      // with a plain "nearer than the best so far" (0x5d8288). So of shells at one place - a missile's children
      // in the beat they split - the one the grid gives first is taken. (Seen: three children at one point, the
      // original's missile deflector takes one, ours took another, and every gun after it a different one.)
      std::vector<Projectile*> cands;
      if (sim.entityGrid().ready()) {
        std::vector<Entity*> cells;
        sim.entityGrid().GatherBox(u->position.x - reach, u->position.z - reach, u->position.x + reach,
                                   u->position.z + reach, 4, &cells);
        for (Entity* e : cells) cands.push_back(static_cast<Projectile*>(e));
      } else {
        for (Projectile* p : sim.projectiles) cands.push_back(p);
      }
      for (Projectile* p : cands) {
        if (!Alive(p) || !IsEnemy(u->army, p->army)) continue;
        // An entity without a collision primitive is in no cell of the grid. A projectile has none unless its
        // blueprint or its script gives it one: FAF's torpedo and missile classes call SetCollisionShape('Sphere',
        // ...) when they are made, a mod's class that does not (BlackOps' AssaultTorpedoSubProjectile) makes a
        // shell that no counter-weapon ever finds. (Seen: 89 such torpedoes in a replay, none taken as a target.)
        if (p->shape.type == ShapeType::None) continue;
        float d2 = Dist2XZ(u->position, p->position);
        if (!(reach * reach > d2)) continue;  // 0x7224b5
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
        if (static_cast<int>(p->shooters.size()) >= cap) continue;  // (0x5d8241: no word for its own target)
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
  bool bestBlip = false;
  uint32_t bestFake = 0;
  Unit* best = FindBestEnemy(sim, w, list, range, useAngle, &u->blipCacheBlip, &bestBlip, &u->blipCacheFake, &bestFake);
  if (best) {
    // an idle unit's AutoInitiateAttackCommand weapon also orders the unit to attack what it found (0x5d93d5)
    const bool autoAttack = CheckAutoInitiate(sim, w);
    // the task first builds a CAiTarget of what it found (UpdateTarget 0x5d55b0: one target-point draw, every
    // time), then hands it to the weapon, whose own draw (PickNewTargetAimSpot) only comes with a new target
    {
      static const bool dbgAcq = getenv("MOHO64_DEBUG_ACQUIRE") != nullptr;
      if (dbgAcq)
        Logf(LogLevel::Info, "acq %u unit %u %s -> %u%s (had %u)", sim.tick(), u->id, b.label.c_str(), best->id,
             bestFake ? " (a fake blip of it)" : bestBlip ? (best->dead ? " (its blip, the unit is dead)" : " (its blip)") : "",
             TargetEntity(sim, w->target) ? TargetEntity(sim, w->target)->id : 0u);
    }
    TargetPointDraw(sim, best);
    // (Unit::GetBlipsInRange 0x6acc60 lists the units themselves, and behind a jammer its blips)
    AiTarget nt = EntityTarget(best);
    if (bestBlip) {
      nt.viaBlip = true;
      nt.pastDeath = best->dead;
      nt.fakeId = bestFake;
    }
    SetTarget(sim, w, nt);
    if (autoAttack) IssueAutoAttack(sim, u, best, bestBlip, nt.pastDeath, bestFake);
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

// UnitWeapon::Fire 0x6d61f0: the script's OnFire and the shot count (+0x184, 0x6d64b5..0x6d64c2). It does not
// touch the fire clock: that is the fire task's own (CFireWeaponTask+0x24), refilled only after the task's own
// shot (FireTick). So a manual-fire weapon fired by the FireAt task (0x60b76b) or the overcharge keeps a clock of 0,
// and its fire task, woken by the SetTarget before the shot, suspends again at once (0x6d3ed6..0x6d3ee0).
void FireNow(Sim& sim, UnitWeapon* w) {
  if (w->unit->stunned) return;
  WeaponCall(sim, w, "OnFire");
  ++sim.firesSeen;
  ++w->shots;
}

// CFireWeaponTask::Execute 0x6d3dc0
void FireTick(Sim& sim, UnitWeapon* w) {
  Unit* u = w->unit;
  if (w->fireClock != 0) --w->fireClock;
  static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
  if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit) {  // why it does or does not fire this tick
    const bool has = HasTarget(sim, w->target);
    const Entity* te = has ? TargetEntity(sim, w->target) : nullptr;
    const Vec3 tp = has ? TargetPos(sim, w->target, false) : Vec3{};
    Logf(LogLevel::Info,
         "wdbg %u unit %u %s clock %d enabled %d target %d(%u = entity %d%s at %.2f %.2f %.2f) canAttack %d canFire %d "
         "onTarget %d status %d stun %d",
         sim.tick(), u->id, w->label.c_str(), w->fireClock, static_cast<int>(w->enabled), w->target.type,
         w->target.entityId, te ? static_cast<int>(te->id) : -1, w->target.viaBlip ? ", its blip" : "", tp.x, tp.y, tp.z,
         has ? static_cast<int>(CanAttackTarget(sim, w, w->target, true)) : -1,
         has ? static_cast<int>(CanFire(sim, w)) : -1, static_cast<int>(w->onTarget),
         has ? TargetStatus(sim, w, w->target) : -1, u->stunned);
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
  // the clock restarts after the task's own shot: round(10 / RateOfFire) (0x6d3e8d..0x6d3ec8)
  const float rof = RoF(w);
  w->fireClock = rof > 0 ? static_cast<int>(std::nearbyint(10.0f / rof)) : 0;
}

}  // namespace

void WeaponsStageDebug(Sim& sim) {
  static const bool dbg = getenv("MOHO64_DEBUG_COMBAT") != nullptr;
  if (dbg && sim.tick() % 300 == 0) {
    const std::vector<Unit*>& units = sim.ArmyStageOrder();
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
         sim.tick(), weapons, withTarget, static_cast<unsigned long long>(sim.firesSeen),
         static_cast<unsigned long long>(sim.projectilesMade), sim.projectiles.size(),
         static_cast<unsigned long long>(sim.impactsSeen));
  }
}

// One army's attacker stage (brain+0xb0): its units in creation order, not in id order (an id used a second time
// belongs to a younger unit), in that army's turn (landnav.h: LandNavStageBegin). The caller hands a copy: a script
// fired from here may create units.
void WeaponsTickUnits(Sim& sim, const std::vector<Unit*>& units) {
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
    static const long dbgUnit2 = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
    if (dbgUnit2 >= 0 && static_cast<long>(c->unit->id) == dbgUnit2)
      Logf(LogLevel::Info, "wdbg   frame %s %s goal %.9g %.9g %.9g bone q %.9g %.9g %.9g %.9g -> v %.9g %.9g %.9g",
           c->label.c_str(), heading ? "yaw" : "pitch", goal.x, goal.y, goal.z, bq.x, bq.y, bq.z, bq.w, v.x, v.y, v.z);
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
    // (0x50b750..0x50b760: t = y / |r| with no test of the length and no clamp: a zero or NaN vector gives NaN)
    const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    const float t = r.y / l;
    float p = t * -0.0187293f + 0.0742610f;
    p = p * t - 0.2121144f;
    p = p * t + 1.5707288f;
    const float neg = -(1.5707964f - std::sqrt(1.0f - t) * p);
    target = center - neg;
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
    Logf(LogLevel::Info, "wdbg   aim %s %s cur %.9g target %.9g delta %.9g slew %.6f tol %.6f goal %.5f %.5f %.5f",
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
  float d = std::sqrt((D.y * D.y + D.z * D.z) + D.x * D.x);  // (0x63135b..0x63137b)
  if (d * d < 1e-4f) return T;
  Vec3 dHat = Mul(D, 1 / d), vHat = Mul(V, 1 / s);
  // (0x63145f..0x6314e4: the dot product summed z, x, then y; the polynomial in x squared)
  const float x = 1.5707964f - ((vHat.z * dHat.z + vHat.x * dHat.x) + vHat.y * dHat.y);
  const float x2 = x * x;
  float k = ((x2 * 0.00761f - 0.16605f) * x2 + 1.0f) * x;
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
  // The target's velocity is asked of the CAiTarget's entity (0x6319f5..0x631a2b: target+4, vtable +0x3c), and a
  // target held through a recon blip (an order's enemy target, or a jammer's fake) IS that blip: its GetVelocity is
  // Entity::GetVelocity 0x678dc0, its committed place minus its previous one = the unit's move of the beat before,
  // not the unit's motion velocity (as the shells' lead, projectile.cpp). Seen: MP3 3305, army 3's artillery 0x30002d
  // (LeadTarget, a HighArc gun) on army 0's commander through its blip: led with the unit's velocity its pitch goal
  // was 0.9397 for the original's 0.9291 (watch ww lines), it came on target a beat early and fired at 3379 (3380).
  if (w->target.type == 1 && (w->target.fakeId || (w->target.viaBlip && w->target.blipId)))
    if (const Entity* b = sim.FindEntity(w->target.fakeId ? w->target.fakeId : w->target.blipId);
        b && b->kind == Entity::Kind::Blip)
      tVel = Sub(b->position, static_cast<const ReconBlip*>(b)->prevPos);
  // Unit::GetTargetPoint 0x6ab340: a target bone comes from the unit's pose (0x6aa5c0: where its motion already
  // put it this beat); without target bones it is the blueprint centre on the entity's committed transform
  // (0x679ce0, entity+0x9c), which this beat's motion has not replaced yet (AdvanceCoords is step 13): such a
  // target that already moved this beat is still seen where it stood
  Vec3 tPos;
  Unit* tu = te && te->kind == Entity::Kind::Unit ? static_cast<Unit*>(te) : nullptr;
  if (tu && w->target.aimBone == -1 && tu->lastPosTick == sim.tick()) {
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
    // AI_CalculateFiringDirection 0x5d6440: the horizontal difference set to the length cos(p) by 0x5b1c90, i.e.
    // each component times (cos(p) / length), one rounding; then y = -sin(p). (Normalising first and multiplying
    // after is another last digit of the turret's yaw, and a gun with FiringTolerance 0 notices.)
    Vec3 hz{tPos.x - Mpos.x, 0, tPos.z - Mpos.z};
    const float l2 = (hz.x * hz.x + hz.y * hz.y) + hz.z * hz.z;
    if (l2 > 0.0f) {
      const float k = dmath::Cos(p) / std::sqrt(l2);
      hz.x *= k;
      hz.z = k * hz.z;
    }
    return {hz.x, -dmath::Sin(p), hz.z};
  }
  {  // the normalised copy 0x44f7e0 (0x631ec7): times 1 / length, the +0 vector for none
    const Vec3 d = Sub(tPos, Mpos);
    const float l = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
    if (!(l > 0.0f)) return Vec3{0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / l;
    return Vec3{d.x * inv, inv * d.y, inv * d.z};
  }
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

void UnitAimTickManipulators(Sim& sim, Unit* u, bool anims);
bool BuilderArmSlotStep(Sim& sim, Unit* u, BuilderArm* a);

void UnitAimTick(Sim& sim, Unit* u) {
  const bool anims = !u->animators.empty();
  if (u->aimControllers.empty() && u->rotators.empty() && u->builderArms.empty() && u->sliders.empty() &&
      u->slavers.empty() && u->footPlants.empty() && u->thrusters.empty() && !anims) {
    if (!u->colliders.empty()) CollidersTick(sim, u);
    return;
  }
  if (!u->skeleton) {
    if (anims) AnimatorsUpdate(sim, u);  // (their time still runs)
    return;
  }
  AimReadLastPose(sim, u);
  // slavers read their source bone from the pose that is about to be replaced (last beat's: 0x646164)
  if (!u->slavers.empty()) {
    auto& sv = u->slavers;
    sv.erase(std::remove_if(sv.begin(), sv.end(), [](SlaveManipulator* m) { return !m->alive; }), sv.end());
    const auto& bones = u->skeleton->bones();
    const bool posed = u->poseQ.size() == bones.size();
    // CSlaveManipulator::MoveManipulator 0x646140: the source bone's turn AWAY FROM ITS REST, through matrices: the
    // axes (VAxes3 0x4ec590) of its rest rotation (the skeleton's, 0x63ee30 + 8) and of its local rotation in last
    // beat's pose (+0x20), multiplied local x rest^T row by row (0x645ad0), back to a quaternion (0x4f0ae0). Ours
    // took the source's whole local rotation, its rest with it: a bone slaved to a turret bone whose rest is turned
    // carried that turn twice (FAF's RackSlavedToTurret racks: Experimental2's baa0001 Rapid_Muzzle01 on Rapid_Cannons,
    // the muzzle's direction 3.5e-7 aside where the original's is 1.7e-7).
    for (SlaveManipulator* m : sv)
      if (m->srcBone >= 0 && static_cast<size_t>(m->srcBone) < bones.size()) {
        const size_t sb = static_cast<size_t>(m->srcBone);
        float R[9], L[9];
        AxesFromQuatExe(bones[sb].localRot, R);
        AxesFromQuatExe(posed ? u->poseQ[sb] : bones[sb].localRot, L);
        const float P[9] = {
            (L[0] * R[0] + L[1] * R[1]) + L[2] * R[2], (R[4] * L[1] + R[5] * L[2]) + L[0] * R[3],
            (R[7] * L[1] + R[8] * L[2]) + L[0] * R[6], (L[3] * R[0] + L[4] * R[1]) + L[5] * R[2],
            (R[4] * L[4] + R[5] * L[5]) + L[3] * R[3], (R[7] * L[4] + R[8] * L[5]) + L[3] * R[6],
            (L[6] * R[0] + R[1] * L[7]) + R[2] * L[8], (R[4] * L[7] + R[5] * L[8]) + R[3] * L[6],
            (R[7] * L[7] + R[8] * L[8]) + L[6] * R[6]};
        m->srcLocal = QuatFromAxesExe(P);
      }
  }
  PoseReset(u);
  UnitAimTickManipulators(sim, u, anims);
}

// Where each aim controller's muzzle stood in last beat's pose (actor+8: last beat's transform and rotations),
// which Aim 0x6317b0 measures the firing direction from. It is read once a beat, while the pose is still last
// beat's: before the builder arms' step, which writes its own bones (a commander's arm and gun both turn the
// torso; read after the arm's step, the torso was back at rest and the gun aimed from where the muzzle is with
// the torso unturned: Chris, 2026-10-13, "the acu is not aiming properly and shooting the tanks").
namespace {

// The muzzle bones of the unit's live aim controllers in last beat's pose, walked down from the base (p0, r0), in
// the controllers' order (those that are dead or have no muzzle bone are passed over). (BoneWorld's walk down each
// muzzle's chain, shared: a bone's world transform is made from its parent's with the steps BoneWorld takes, in its
// order, so each muzzle gets the same bits; the controllers of a unit share their turret's chain.) A muzzle past the
// skeleton, or on a chain deeper than BoneWorld's 64, is BoneWorld's own answer, which reads the unit's pose base:
// with boneWorldToo false the walk gives up there instead (false), as a walk made ahead must (core/speculate.h).
bool MuzzlesFromLastPose(const Unit* u, const Vec3& p0, const Quat& r0, bool boneWorldToo,
                         std::vector<std::pair<Vec3, Quat>>* out) {
  out->clear();
  const auto& bones = u->skeleton->bones();
  const float s = u->meshScale * u->scale[0];
  const bool posed = u->poseQ.size() == bones.size();
  static thread_local std::vector<Vec3> wp;
  static thread_local std::vector<Quat> wr;
  static thread_local std::vector<uint32_t> made;
  static thread_local uint32_t stamp = 0;
  if (++stamp == 0) {
    std::fill(made.begin(), made.end(), 0u);
    stamp = 1;
  }
  if (made.size() < bones.size()) {
    wp.resize(bones.size());
    wr.resize(bones.size());
    made.resize(bones.size(), 0u);
  }
  for (const AimController* c : u->aimControllers) {
    if (!c->alive || c->muzzleBone < 0) continue;
    const int bone = c->muzzleBone;
    int depth = 0;
    if (bone < u->skeleton->Count())
      for (int j = bone; j >= 0 && depth <= 64; j = bones[static_cast<size_t>(j)].parent) ++depth;
    if (bone >= u->skeleton->Count() || depth > 64) {  // (deeper than BoneWorld walks: it starts 64 bones up)
      if (!boneWorldToo) return false;
      Vec3 p;
      Quat q;
      BoneWorld(u, bone, &p, &q);
      out->push_back({p, q});
      continue;
    }
    int chain[64];
    int n = 0;
    int i = bone;
    for (; i >= 0; i = bones[static_cast<size_t>(i)].parent) {
      if (made[static_cast<size_t>(i)] == stamp) break;  // (made from the root, as BoneWorld's walk)
      chain[n++] = i;
    }
    Quat r = i >= 0 ? wr[static_cast<size_t>(i)] : r0;
    Vec3 p = i >= 0 ? wp[static_cast<size_t>(i)] : p0;
    for (int k = n - 1; k >= 0; --k) {
      const size_t bi = static_cast<size_t>(chain[k]);
      const Bone& b = bones[bi];
      const Vec3 lp = posed ? u->poseP[bi] : Vec3{b.localPos.x * s, b.localPos.y * s, b.localPos.z * s};
      p = Add(p, RotateM(r, lp));
      r = ComposeQ(r, posed ? u->poseQ[bi] : b.localRot);
      wp[bi] = p;
      wr[bi] = r;
      made[bi] = stamp;
    }
    out->push_back({p, r});
  }
  return true;
}

bool MuzzleBits(const Vec3& a, const Vec3& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool MuzzleBits(const Quat& a, const Quat& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

AimAhead::AimAhead() : mode(SpecModeFromEnvironment("AIM")) {}

void AimMuzzlesAhead(Sim& sim, const std::vector<Unit*>& order) {
  AimAhead& st = sim.aimAhead();
  static const bool dbgW = getenv("MOHO64_DEBUG_WEAPON") != nullptr;  // (its lines are written at the unit's turn)
  if (!st.active() || dbgW) return;
  const uint32_t round = sim.tick();
  std::atomic<uint64_t> made{0};
  sim.workers().ParallelFor("aim muzzles ahead", order.size(), 16, [&](size_t b, size_t e, int) {
    static thread_local std::vector<std::pair<Vec3, Quat>> m;
    uint64_t n = 0;
    for (size_t i = b; i < e; ++i) {
      Unit* u = order[i];
      if (u->aimPoseTick == round || !u->skeleton || u->aimControllers.empty()) continue;
      // (the base the unit's turn will walk from: PoseAdvance keeps this as last beat's, and AimReadLastPose takes it)
      const Vec3 p0 = u->poseSet ? u->poseBasePos : u->position;
      const Quat r0 = u->poseSet ? u->poseBaseOri : u->orientation;
      if (!MuzzlesFromLastPose(u, p0, r0, false, &m)) continue;
      // (each task writes only its own unit and that unit's controllers, which nothing reads before the unit's turn)
      size_t k = 0;
      for (AimController* c : u->aimControllers) {
        if (!c->alive || c->muzzleBone < 0) continue;
        c->aheadMuzzlePos = m[k].first;
        c->aheadMuzzleRot = m[k].second;
        c->aheadBone = c->muzzleBone;
        c->aheadTick = round;
        ++k;
      }
      u->aimAheadP0 = p0;
      u->aimAheadR0 = r0;
      u->aimAheadPoseVersion = u->poseVersion;
      u->aimAheadSkeleton = u->skeleton;
      u->aimAheadScale = u->meshScale * u->scale[0];
      u->aimAheadList = u->aimControllers.data();
      u->aimAheadCount = u->aimControllers.size();
      u->aimAheadTick = round;
      ++n;
    }
    made += n;
  });
  st.counts.made += made.load();
}

namespace {
// The muzzles walked ahead, if what they were walked from is what the turn has now: the base (p0, r0) to the bit,
// the pose's version, the skeleton, the scale, and the same controllers, live as they were, on the same bones.
bool AimAheadHolds(const Unit* u, uint32_t tick, const Vec3& p0, const Quat& r0) {
  if (u->aimAheadTick != tick) return false;
  if (!MuzzleBits(u->aimAheadP0, p0) || !MuzzleBits(u->aimAheadR0, r0)) return false;
  const float s = u->meshScale * u->scale[0];
  if (u->aimAheadPoseVersion != u->poseVersion || u->aimAheadSkeleton != u->skeleton ||
      std::memcmp(&u->aimAheadScale, &s, sizeof s) != 0 || u->aimAheadList != u->aimControllers.data() ||
      u->aimAheadCount != u->aimControllers.size())
    return false;
  for (const AimController* c : u->aimControllers) {
    const bool live = c->alive && c->muzzleBone >= 0;
    if (live != (c->aheadTick == tick) || (live && c->aheadBone != c->muzzleBone)) return false;
  }
  return true;
}
}  // namespace

void AimReadLastPose(Sim& sim, Unit* u) {
  if (u->aimPoseTick == sim.tick()) return;
  u->aimPoseTick = sim.tick();
  if (!u->skeleton) return;
  if (!u->aimControllers.empty()) {  // (the pose is still last beat's)
    const Vec3 curPos = u->poseBasePos;
    const Quat curOri = u->poseBaseOri;
    u->poseBasePos = u->prevPoseBasePos;
    u->poseBaseOri = u->prevPoseBaseOri;
    const Quat r0 = u->poseSet ? u->poseBaseOri : u->orientation;
    const Vec3 p0 = u->poseSet ? u->poseBasePos : u->position;
    // the muzzles: walked ahead (MOHO64_PAR_AIM) from the same base and still good, or walked now
    AimAhead& st = sim.aimAhead();
    bool ahead = false;
    if (st.active()) {
      if (u->aimAheadTick != sim.tick()) {
        ++st.counts.none;
      } else if (!AimAheadHolds(u, sim.tick(), p0, r0)) {
        ++st.counts.changed;
      } else if (st.mode == SpecMode::kRedo) {
        ++st.counts.thrown;
      } else {
        ++st.counts.taken;
        ahead = true;
      }
    }
    if (!ahead || st.mode == SpecMode::kVerify) {
      static thread_local std::vector<std::pair<Vec3, Quat>> now;
      MuzzlesFromLastPose(u, p0, r0, true, &now);
      size_t k = 0;
      for (AimController* c : u->aimControllers) {
        if (!c->alive || c->muzzleBone < 0) continue;
        if (ahead) {  // (verify: the walk made ahead against this one, bit for bit)
          ++st.counts.checked;
          if (!MuzzleBits(c->aheadMuzzlePos, now[k].first) || !MuzzleBits(c->aheadMuzzleRot, now[k].second)) {
            char b[128];
            std::snprintf(b, sizeof b, "tick %u unit 0x%x controller %zu: the muzzle walked ahead differs", sim.tick(), u->id, k);
            SpecMismatch("AIM", k, b);
          }
        }
        c->lastMuzzlePos = now[k].first;
        c->lastMuzzleRot = now[k].second;
        ++k;
      }
    } else {
      for (AimController* c : u->aimControllers) {
        if (!c->alive || c->muzzleBone < 0) continue;
        c->lastMuzzlePos = c->aheadMuzzlePos;
        c->lastMuzzleRot = c->aheadMuzzleRot;
      }
    }
    static const long dbgUnit = getenv("MOHO64_DEBUG_WEAPON") ? atol(getenv("MOHO64_DEBUG_WEAPON")) : -1;
    if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit)
      for (AimController* c : u->aimControllers)
        if (c->alive && c->muzzleBone >= 0 && c->yawBone >= 0 && static_cast<size_t>(c->yawBone) < u->poseQ.size())
          Logf(LogLevel::Info,
               "wdbg   last pose %s (%s): muzzle %.9g %.9g %.9g, yaw bone %d local q %.9g %.9g %.9g %.9g, base %.9g %.9g "
               "%.9g q %.9g %.9g",
               c->label.c_str(), c->weapon ? c->weapon->label.c_str() : "-", c->lastMuzzlePos.x, c->lastMuzzlePos.y,
               c->lastMuzzlePos.z, c->yawBone, u->poseQ[static_cast<size_t>(c->yawBone)].x,
               u->poseQ[static_cast<size_t>(c->yawBone)].y, u->poseQ[static_cast<size_t>(c->yawBone)].z,
               u->poseQ[static_cast<size_t>(c->yawBone)].w, u->poseBasePos.x, u->poseBasePos.y, u->poseBasePos.z,
               u->poseBaseOri.y, u->poseBaseOri.w);
    u->poseBasePos = curPos;
    u->poseBaseOri = curOri;
  }
}

// The arc cosine the foot plant takes (Abramowitz and Stegun 4.4.45, in the exe's x87 order at PC_24: inline at
// 0x639900..0x63996d, 0x639b20..0x639b7d, 0x639c4f..0x639c71), and 0x50ccf0's arc sine: pi / 2 (the float of
// atan(1) x 4, halved: [0xfb7914]) less it.
static float FootAcosExe(float c) {
  const float r = std::sqrt(1.0f - c);
  const float p = c * ((c * -0x1.32dc6p-6f + 0x1.302c4ep-4f) * c - 0x1.b26908p-3f);  // [0xe4f778], [0xe4f774], [0xe4f770]
  return (p + 0x1.921b48p+0f) * r;                                                    // [0xe4f76c]
}
static float FootAsinExe(float c) { return 0x1.921fb6p+0f - FootAcosExe(c); }
// Moho::EulerRollToQuat 0x570750 about (1, 0, 0): cos(a / 2), the axis times sin(a / 2)
static Quat FootRollXExe(float a) {
  const float h = a * 0.5f;
  const float s = dmath::Sin(h);
  return Quat{1.0f * s, s * 0.0f, s * 0.0f, dmath::Cos(h)};
}
// CFootPlantManipulator::MoveManipulator 0x6395f0, on this beat's pose as the manipulators before it left it
static void FootPlantStep(Sim& sim, Unit* u, FootPlantManipulator* m) {
  if (u->dead) return;  // 0x639615: the unit's IsDead (vt+0x28)
  const int n = static_cast<int>(u->poseQ.size());
  if (m->footBone < 0 || m->kneeBone < 0 || m->hipBone < 0 || m->footBone >= n || m->kneeBone >= n ||
      m->hipBone >= n)
    return;
  // the hip's, the knee's and the foot's world transforms (0x54bec0), the hip's axes (VAxes3 0x4ec590)
  Vec3 hipP, kneeP, footP;
  Quat hipQ, q;
  BoneWorld(u, m->hipBone, &hipP, &hipQ);
  BoneWorld(u, m->kneeBone, &kneeP, &q);
  BoneWorld(u, m->footBone, &footP, &q);
  float A[9];
  AxesFromQuatExe(hipQ, A);
  // the ground under the foot (STIMap::GetElevation 0x44fb90), plus the height of the raised platform the unit
  // stands on (motion+0x114: 0x62af70)
  const TerrainMap* map = sim.map();
  float g = map ? map->TerrainHeight(footP.x, footP.z) : 0.0f;
  if (const Unit* P = RaisedPlatform(sim, u)) g = PlatformHeight(P, footP.x, footP.z) + g;
  // 0x63984d..0x6398a2: the goal is the higher of that ground (or the ground + halfLeg, when lower) and the foot
  // raised by MaxFootFall; half the way up goes to each turn
  {
    const float t1 = m->halfLeg + g;
    if (!(t1 > g)) g = t1;
    const float t2 = m->maxFootFall + footP.y;
    if (t2 > g) g = t2;
  }
  const float d = (g - footP.y) * 0.5f;
  // the thigh: from a drop of a = |hip y - knee y| over its length L1 to a - d (at most L1)
  {
    const float dz = kneeP.z - hipP.z, dx = kneeP.x - hipP.x, dy = kneeP.y - hipP.y;
    const float L1 = std::sqrt((dz * dz + dy * dy) + dx * dx);
    const float a = std::fabs(hipP.y - kneeP.y);
    const float inv = 1.0f / L1;
    const float A1 = FootAcosExe(inv * a);
    const float amd = a - d;
    const float b = L1 > amd ? amd : L1;
    const float ang = FootAcosExe(inv * b) - A1;
    PoseRotate(u, m->hipBone, FootRollXExe(-ang));  // 0x639a0f: the hip by -ang, 0x639a1c: the knee by ang
    PoseRotate(u, m->kneeBone, FootRollXExe(ang));
  }
  // the shin, from where the turns put the knee and the foot: from a drop of e = |knee y - foot y| over its
  // length L2 to e - d; a shin that leans forward along the hip's forward axis (A[6], A[8]) of a leg that is not
  // straight (+0x94) takes the inline arc cosine, every other the arc sine of 0x50ccf0
  BoneWorld(u, m->kneeBone, &kneeP, &q);
  BoneWorld(u, m->footBone, &footP, &q);
  const float dx = footP.x - kneeP.x, dz = footP.z - kneeP.z, dy = footP.y - kneeP.y;
  const bool forward = (A[8] * dz + A[6] * dx) + dy * 0.0f > 0.0f && !m->straightLegs;
  const float L2 = std::sqrt((dz * dz + dy * dy) + dx * dx);
  const float e = std::fabs(kneeP.y - footP.y);
  const float inv = 1.0f / L2;
  const float emd = e - d;
  const float b = L2 > emd ? emd : L2;
  const float turn =
      forward ? FootAcosExe(inv * b) - FootAcosExe(inv * e) : FootAsinExe(inv * b) - FootAsinExe(inv * e);
  PoseRotate(u, m->kneeBone, FootRollXExe(-0.0f - turn));  // 0x639c3c
}

// The shortest arc 0x50cb50 that turns the unit vector a onto b: h = a + b normalised (0x452af0), w = a.h,
// xyz = a x h. Opposite vectors (w exactly 0, 0x50cbde..0x50cbed) give half a turn about an axis across a: from
// x and z when |a.x| >= |a.y|, else from y and z, times 1 / sqrt (0x50d150).
static Quat ThrustArcExe(const Vec3& a, const Vec3& b) {
  Vec3 h{a.x + b.x, b.y + a.y, b.z + a.z};
  vm::NormalizeExe(h);
  Quat r;
  r.w = (a.z * h.z + a.x * h.x) + a.y * h.y;
  if (r.w != 0.0f) {
    r.x = a.y * h.z - a.z * h.y;
    r.y = a.z * h.x - a.x * h.z;
    r.z = a.x * h.y - a.y * h.x;
  } else if (std::fabs(a.x) >= std::fabs(a.y)) {  // 0x50cc65 fcompi, jb (unordered: the y branch)
    const float s = 1.0f / std::sqrt(a.x * a.x + a.z * a.z);
    r.x = -(s * a.z);
    r.y = 0.0f;
    r.z = s * a.x;
  } else {
    const float s = 1.0f / std::sqrt(a.y * a.y + a.z * a.z);
    r.x = 0.0f;
    r.y = s * a.z;
    r.z = -(s * a.y);
  }
  return r;
}
// 0x4eb830: from c toward t by at most the angle `rate`. d = conj(c) x t (0x4eb833..0x4eb942); 0x4eb740 caps it (only
// when rate / 2 is below pi / 2, and only when |d.xyz| is above the sine of rate / 2: then d = (cos, the axis x sin,
// the sine negated when d.w < 0)); a capped d gives c x d (0x4eb966..0x4eba48, not normalised), an uncapped one t
// itself (0x4eba55).
static Quat ThrustRateStepExe(const Quat& c, const Quat& t, float rate) {
  const float cx = -0.0f - c.x, cy = -0.0f - c.y, cz = -0.0f - c.z;
  Quat d;
  d.w = ((t.w * c.w - t.x * cx) - t.y * cy) - t.z * cz;
  d.x = ((t.z * cy + t.x * c.w) + t.w * cx) - t.y * cz;
  d.y = ((t.y * c.w + t.x * cz) + t.w * cy) - t.z * cx;
  d.z = ((t.z * c.w + t.y * cx) + t.w * cz) - t.x * cy;
  const float half = std::fabs(rate * 0.5f);
  if (half >= 1.57079637f) return t;  // [0xe4f7a0] (0x4eb760 fcomi, jb: unordered goes on)
  float s = dmath::Sin(half);
  const float len2 = (d.z * d.z + d.x * d.x) + d.y * d.y;
  if (!(len2 > s * s)) return t;
  if (0.0f > d.w) s = -0.0f - s;
  Vec3 n{d.x, d.y, d.z};
  vm::NormalizeExe(n);
  d = {n.x * s, n.y * s, n.z * s, dmath::Cos(half)};
  Quat o;
  o.w = ((c.w * d.w - c.x * d.x) - c.y * d.y) - c.z * d.z;
  o.x = ((c.y * d.z + c.x * d.w) + c.w * d.x) - c.z * d.y;
  o.y = ((c.z * d.x + c.y * d.w) + c.w * d.y) - c.x * d.z;
  o.z = ((c.z * d.w + c.x * d.y) + c.w * d.z) - c.y * d.x;
  return o;
}
// CThrustManipulator::MoveManipulator 0x64a800, on this beat's pose as the manipulators before it left it
static void ThrustStep(Unit* u, ThrustManipulator* m) {
  if (u->dead) return;  // 0x64a825: the unit's IsDead (vt+0x28)
  if (m->bone < 0 || static_cast<size_t>(m->bone) >= u->poseQ.size()) return;
  // the motion's last control force (m+0xfc) and the yaw torque (m+0x10c): only the flight writes them; any other
  // motion keeps the ctor's zeros (0x6b7d6d)
  Vec3 f{}, d{};
  float yawTorque = 0.0f;
  if (u->motion.air) {
    f = u->motion.air->ctrlForce;
    yawTorque = u->motion.air->torque108.y;
  }
  // 0x64a8ee: the turn about (0, 1, 0) by the yaw torque (EulerRollToQuat 0x570750: cos(a / 2), the axis x sin(a / 2))
  const float h = yawTorque * 0.5f;
  const float sh = dmath::Sin(h);
  const Quat yaw{0.0f * sh, 1.0f * sh, 0.0f * sh, dmath::Cos(h)};
  // 0x64a917..0x64a96b: the bone's world position (GetBoneWorldTransform, vt+0x48 of unit+8: the pose built on this
  // beat's pending transform) less the unit's committed position (IUnit::GetTransform 0x6a49c0, u+0xa4: the
  // transform the beat began with)
  Vec3 bp;
  Quat bq;
  BoneWorld(u, m->bone, &bp, &bq);
  const Vec3& up = u->shapeHeld ? u->shapePos : u->position;
  d = {bp.x - up.x, bp.y - up.y, bp.z - up.z};
  // 0x64a971..0x64a9cc: the force plus (the offset turned less the offset) x turnMult
  const Vec3 r = vm::RotateM(yaw, d);
  Vec3 v{(r.x - d.x) * m->turnMult + f.x, (r.y - d.y) * m->turnMult + f.y, (r.z - d.z) * m->turnMult + f.z};
  // 0x64a9d2..0x64aa1c: into the bone's frame (the conjugate of its composite rotation 0x54bec0)
  const Vec3 l = vm::RotateM(Quat{-0.0f - bq.x, -0.0f - bq.y, -0.0f - bq.z, bq.w}, v);
  // 0x64aa21..0x64aab5: each axis capped below by capMin, then above by capMax
  Vec3 c;
  {
    const float x1 = m->capMin.x > l.x ? m->capMin.x : l.x;
    c.x = m->capMax.x > x1 ? x1 : m->capMax.x;
    const float y1 = m->capMin.y > l.y ? m->capMin.y : l.y;
    c.y = m->capMax.y > y1 ? y1 : m->capMax.y;
    const float z1 = m->capMin.z > l.z ? m->capMin.z : l.z;
    c.z = m->capMax.z > z1 ? z1 : m->capMax.z;
  }
  vm::NormalizeExe(c);  // 0x64aab9
  // 0x64aad3: the arc from the bone's forward axis to there; 0x64aaf4: last beat's turn toward it by at most
  // turnSpeed; 0x64ab02: the bone's local rotation x that (CAniPoseBone::Rotate 0x54bc00); kept for the next beat
  const Quat goal = ThrustArcExe(m->fwd, c);
  const Quat o = ThrustRateStepExe(m->cur, goal, m->turnSpeed);
  PoseRotate(u, m->bone, o);
  m->cur = o;
  static const long dbg = getenv("MOHO64_DEBUG_THRUST") ? atol(getenv("MOHO64_DEBUG_THRUST")) : -2;
  if (dbg == -1 || static_cast<long>(u->id) == dbg)
    Logf(LogLevel::Info, "thrust unit %u bone %d f %.9g %.9g %.9g yawT %.9g l %.9g %.9g %.9g c %.9g %.9g %.9g turn %.9g %.9g %.9g %.9g",
         u->id, m->bone, f.x, f.y, f.z, yawTorque, l.x, l.y, l.z, c.x, c.y, c.z, o.x, o.y, o.z, o.w);
}

void UnitAimTickManipulators(Sim& sim, Unit* u, bool anims) {
  // The unit's manipulators, one list in the order of their precedence, the ones of equal precedence in the
  // order they were put there (CAniActor::UpdateManipulators 0x63aa80 walks the list 0x63ace0 keeps; the
  // constructor, SetPrecedence and an animator's PlayAnim each put a manipulator behind its equals). FAF's
  // numbers: animators and rotators 0 unless set, a weapon's slavers 9, its aim controller 10, its recoil
  // sliders 11, a builder arm 0 / 5 / 20. The UEF cruiser's Phalanx spins its barrel, which is also the gun's
  // pitch bone, with a rotator of precedence 10 made at the first unpacking: behind the aim controller, which
  // therefore reads the barrel's frame before the spin is on it.
  struct Manip {
    int prec;
    uint32_t seq;
    int kind;  // 0 animator, 1 rotator, 2 slaver, 3 slider, 4 builder arm, 5 aim controller, 6 foot plant, 7 thrust
    void* p;
  };
  // (The list is made for every unit with manipulators every beat: it lives in a buffer kept from call to call. A
  // manipulator's step may run Lua (a builder arm's tracking callback), so a call made from inside it takes the next
  // buffer, and one nested deeper than the pool its own.)
  static thread_local std::vector<Manip> pool[4];
  static thread_local int depth = 0;
  std::vector<Manip> own;
  std::vector<Manip>& order = depth < 4 ? pool[depth] : own;
  order.clear();
  struct Nest {
    int& d;
    explicit Nest(int& x) : d(x) { ++d; }
    ~Nest() { --d; }
  } nest(depth);
  if (anims) {
    static thread_local std::vector<AnimatorRef> al;  // (filled and read here, before any step runs)
    al.clear();
    AnimatorsCollect(sim, u, &al);
    for (const AnimatorRef& r : al) order.push_back({r.precedence, r.seq, 0, r.a});
  }
  {
    auto& v = u->rotators;
    v.erase(std::remove_if(v.begin(), v.end(), [](RotateManipulator* r) { return !r->alive; }), v.end());
    for (RotateManipulator* r : v) order.push_back({r->precedence, r->seq, 1, r});
  }
  for (SlaveManipulator* m : u->slavers) order.push_back({m->precedence, m->seq, 2, m});
  {
    auto& v = u->sliders;
    v.erase(std::remove_if(v.begin(), v.end(), [](SlideManipulator* m) { return !m->alive; }), v.end());
    for (SlideManipulator* m : v) order.push_back({m->precedence, m->seq, 3, m});
  }
  for (BuilderArm* a : u->builderArms) order.push_back({a->precedence, a->seq, 4, a});
  for (AimController* c : u->aimControllers) order.push_back({c->precedence, c->seq, 5, c});
  {
    auto& v = u->footPlants;
    v.erase(std::remove_if(v.begin(), v.end(), [](FootPlantManipulator* m) { return !m->alive; }), v.end());
    for (FootPlantManipulator* m : v) order.push_back({m->precedence, m->seq, 6, m});
  }
  {
    auto& v = u->thrusters;
    v.erase(std::remove_if(v.begin(), v.end(), [](ThrustManipulator* m) { return !m->alive; }), v.end());
    for (ThrustManipulator* m : v) order.push_back({m->precedence, m->seq, 7, m});
  }
  const auto before = [](const Manip& a, const Manip& b) { return a.prec != b.prec ? a.prec < b.prec : a.seq < b.seq; };
  // (a list already in strict order is what the sort would give: most units' manipulators were made in order)
  bool strict = true;
  for (size_t i = 1; i < order.size() && strict; ++i) strict = before(order[i - 1], order[i]);
  if (!strict) std::sort(order.begin(), order.end(), before);
  // (a manipulator that becomes signalled in its step wakes its waiting threads there and then: Sim::SignalEvent)
  for (const Manip& e : order) {
    if (e.kind == 0) {
      ScriptObject* a = static_cast<ScriptObject*>(e.p);
      const int was = a->EventState();
      AnimatorStep(sim, u, a);
      if (was == 0 && a->EventState() == 1) sim.SignalEvent(a);
      continue;
    }
    if (e.kind == 1) {
      RotateManipulator* r = static_cast<RotateManipulator*>(e.p);
      if (r->alive) {
        const bool was = r->signalled;
        RotatorStep(u, r);
        if (!was && r->signalled) sim.SignalEvent(r);
      }
      continue;
    }
    if (e.kind == 3) {
      SlideManipulator* s = static_cast<SlideManipulator*>(e.p);
      if (s->alive) {
        const bool was = s->signalled;
        SliderStep(u, s);
        if (!was && s->signalled) sim.SignalEvent(s);
      }
      continue;
    }
    if (e.kind == 4) {  // the arm's MoveManipulator in its slot (BuilderArmsTick read its aim direction)
      BuilderArm* a = static_cast<BuilderArm*>(e.p);
      const bool stepped = a->alive && BuilderArmSlotStep(sim, u, a);
      const int call = a->trackCall;  // (its step's tracking callback, after the bones turned: see ArmStep)
      a->trackCall = 0;
      if (!stepped && a->alive && a->enabled) {  // (no step this beat: the last angles)
        PoseRotate(u, a->yawBone, AxisAngle({0, 1, 0}, a->heading));
        PoseRotate(u, a->pitchBone, AxisAngle({1, 0, 0}, -a->pitch));
      }
      if (call && a->alive) sim.CallMethod(sim.L(), u, call == 1 ? "OnStartBuilderTracking" : "OnStopBuilderTracking", 0);
      continue;
    }
    if (e.kind == 5) {
      AimController* c = static_cast<AimController*>(e.p);
      if (c->alive) MoveManipulator(sim, c);
      continue;
    }
    if (e.kind == 6) {  // (UpdateManipulators 0x63ab1b: an enabled one only)
      FootPlantManipulator* m = static_cast<FootPlantManipulator*>(e.p);
      if (m->alive && m->enabled) FootPlantStep(sim, u, m);
      continue;
    }
    if (e.kind == 7) {  // (UpdateManipulators 0x63ab1b: an enabled one only)
      ThrustManipulator* m = static_cast<ThrustManipulator*>(e.p);
      if (m->alive && m->enabled) ThrustStep(u, m);
      continue;
    }
    SlaveManipulator* m = static_cast<SlaveManipulator*>(e.p);  // CSlaveManipulator::MoveManipulator 0x646140
    if (!m->alive || !m->enabled || m->destBone < 0 || m->srcBone < 0) continue;
    if (m->maxRate >= 0.0f) {
      // (0x4eb830: toward the source's rotation by at most the rate; no script in the game files sets one, and
      // this is a plain shortest-arc step, not read from the exe)
      const Quat d = ComposeQ(Conj(m->cur), m->srcLocal);
      const float w = std::fmin(1.0f, std::fabs(d.w));
      const float ang = 2.0f * std::acos(w);
      if (ang <= m->maxRate || ang < 1e-6f) {
        m->cur = m->srcLocal;
      } else {
        const float k = m->maxRate / ang, s = std::sin(ang * 0.5f), sk = std::sin(ang * 0.5f * k);
        const float sign = d.w < 0 ? -1.0f : 1.0f, f = s > 1e-9f ? sk / s * sign : 0.0f;
        m->cur = QNorm(ComposeQ(m->cur, Quat{d.x * f, d.y * f, d.z * f, std::cos(ang * 0.5f * k)}));
      }
    } else {
      m->cur = m->srcLocal;
    }
    PoseRotate(u, m->destBone, m->cur);  // (0x646289: the destination's local rotation x that)
  }
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
  c->seq = S(L)->NextManipSeq();
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
  if (auto* c = dynamic_cast<AimController*>(GetObject(L, 1))) {
    c->precedence = static_cast<int>(luaL_checknumber(L, 2));
    c->seq = S(L)->NextManipSeq();  // (0x63bc2b: taken out and put back behind its equals)
  }
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
    w->target = AiTarget{};
    w->UnbindLua();
    DropObjectValues(sim.L(), w);  // (its Set<X> values went with the weapon object)
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
    // ... and its builder is deleted (0x6a8834..0x6a884a: unit+0x554): CAiBuilderImpl's dtor 0x59f9c0 clears the
    // rally queue first (BuilderClearFactoryCommandQueue 0x59f5a0: RemoveUnit 0x6e8c20 from the back), so a killed
    // factory's rally orders lose their factory, and an order no unit holds then is deleted at the Sync
    // (CCommandDb::PublishSyncData 0x6e0f50). (Read; the case seen is the destroy without a kill: sim.cpp, ~Unit.)
    ClearFactoryCommandQueue(u);
    // ... and its navigator and steering are deleted (unit+0x54c and +0x550 set to null, 0x6a8804..0x6a8832): the
    // spline and the collision record go with the steering. ResolvePossibleCollision asks a unit with no steering for
    // no spline point (0x597103: [X+0x550] null) and takes its position. Seen: MP2 6877, army 0's Mantis 0xda resolves
    // its record against the Mantis 0x51 ahead of it, killed at 6875: the original takes 0x51's position (178.64), the
    // two meet, and 0xda brakes hard; ours took the next point of 0x51's last spline (178.27), found no meeting and
    // drove on.
    UnitMotion& m = u->motion;
    AirNavigatorGone(u);  // (an aircraft's navigator: its status, for the escorts that follow it)
    EndNavigatorObject(u);  // (the navigator's Lua side goes with it)
    m.spline.clear();
    m.splineIdx = 0;
    m.hasSpline = false;
    m.pointNow = false;
    m.colType = 0;
    m.colUnit = 0;
    m.colTick = 0xffffffffu;
    m.sideStep = {};
  }
}

// ~CAniActor 0x63a930 walks the actor's manipulator list (actor+0x10) and deletes each one (vt[0] with 1);
// IAniManipulator's dtor 0x62fc70 ends in ~CScriptEvent 0x4c94c0 -> ~CScriptObject 0x4c7340, which clears the Lua
// object (IsDestroyed true from then on). A thread waiting on one (WaitFor) is resumed: CWaitForTask::Execute 0x4ca660
// finds the event's weak pointer null and returns -1 (ThreadScheduler::StillWaiting: an unbound object ends the wait).
// The animators are on the same actor (sim/anim.cpp keeps them in u->animators; one that lost its Lua side is gone).
// (Aim controllers went with the weapons, ReleaseUnitCombat.)
void EndUnitManipulators(Unit* u) {
  auto end = [](auto& v) {
    for (auto* m : v) {
      m->alive = false;
      m->UnbindLua();
    }
    v.clear();
  };
  end(u->rotators);
  end(u->sliders);
  end(u->slavers);
  end(u->colliders);
  end(u->footPlants);
  end(u->thrusters);
  end(u->builderArms);
  for (ScriptObject* a : u->animators) a->UnbindLua();
  u->animators.clear();
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
    // A player's order holds the army's recon BLIP (the task's CAiTarget is a copy of the command's, 0x5f29fb), and
    // the blip outlives its unit: once the unit is gone (purged) the blip stays until the armies' recon passes let
    // go of it (DestroyIfUnused, intel.cpp), and while it exists the task has its target (HasTarget 0x5e2a10: a blip
    // is not dead), at the blip's own place; no weapon can attack it (CanAttackTarget 0x6d57f0 asks GetEntity 0x5e2ce0,
    // the blip's unit), and the target is LOST (EntityLost 0x5e2db0: GetEntity 0x5e2ce0, the blip's link to its unit, is null).
    // Seen: MP1 7411, eleven of army 1's ships on a player's Attack of the blip 0x3040008c of army 4's structure
    // 0x400066, replaced by its upgrade at 7410: in the original the six still chasing go on until the blip goes
    // (7418), and the five in reach end at once (the lost branch, 0x5f3994: in reach of the last place); ours looked
    // at the gone unit and ended all eleven at 7411.
    if (t.targetViaBlip && t.targetBlipRef && !t.targetFakeId && !sim.FindEntity(t.goalId))
      if (Entity* b = sim.FindEntity(t.targetBlipRef); b && b->kind == Entity::Kind::Blip) {
        a.entityId = t.targetBlipRef;
        return a;
      }
    if (Entity* e = sim.FindEntity(t.goalId))
      if (e->kind == Entity::Kind::Unit) {
        Unit* tu = static_cast<Unit*>(e);
        a.mobile = IsMobile(tu);
        a.viaBlip = t.targetViaBlip;
        a.pastDeath = t.targetViaBlip && t.targetPastDeath;
        a.fakeId = t.targetViaBlip ? t.targetFakeId : 0;
        // the task's CAiTarget is a copy of the command's (0x5f29fb): it keeps the command's target point, which
        // is where GetTargetPosGun aims (an aircraft that is not yet on its attack run flies at that bone)
        const CombatBpData& cb = CB(tu);
        if (t.targetPoint >= 0 && t.targetPoint < static_cast<int>(cb.targetBones.size()) && e->skeleton)
          a.aimBone = TargetBoneOf(e, cb.targetBones[static_cast<size_t>(t.targetPoint)]);
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

// CUnitCommand::GetPosition 0x6e8a30 without a formation (0x6e8a64): GetTargetPosGun 0x5e2a90 of the command's
// CAiTarget (cmd+0x11c) with the centre flag clear (bl = 0, 0x6e8a6a). For a unit target that is its target point:
// the bone of the command's AI.TargetBones pick, or (no pick) its collision centre (bone -1); a ground target's point.
Vec3 CommandPosGun(Sim& sim, const UnitCommand& c) {
  if (!c.targetId) return c.pos;
  Entity* e = sim.FindEntity(c.targetId);
  if (!e) return c.pos;
  AiTarget a;
  a.type = 1;
  a.entityId = c.targetId;
  if (e->kind == Entity::Kind::Unit) {
    const CombatBpData& cb = CB(static_cast<Unit*>(e));
    if (c.targetPoint >= 0 && c.targetPoint < static_cast<int>(cb.targetBones.size()) && e->skeleton)
      a.aimBone = TargetBoneOf(e, cb.targetBones[static_cast<size_t>(c.targetPoint)]);
  }
  const Vec3 p = combat::TargetPos(sim, a, false);
  return std::isnan(p.x) ? c.pos : p;
}

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
  // +0x58, the command's formation (the ctor's argument): dropped for good once the target is in reach (state 3's
  // in-range branch, 0x5f3a10), and from then on Update 0x5f3020 takes its branch without a formation
  bool formation = true;
  bool firstUpdate = true;   // +0x8e
  bool listening = false;    // the attacker-event listener is linked (from the first UpdateAttacker on)
  int result = 0;            // 1 success, 2 failure (the listener's result)
  int ocWeapon = -1;         // +0x5c: the overcharge weapon's index (an OverCharge order only)
  // +0x54: the command heading the unit's queue when the task was made (the ctor, 0x5f2ab7..0x5f2ae4: unit+0x4b4's
  // first entry; a guard's or a patrol's own command for their child attacks), marked started (+0x154 = 1, 0x5f2ae7)
  std::weak_ptr<UnitCommand> cmd;
};

namespace {
void RelinkSuspendedAttackThread(Unit* u) {
  // (the head attack, or a guard's attack child: ThreadAttackTask)
  const BuildTask* t = ThreadAttackTask(u);
  if (t && t->adata->listening && t->waitUntil == 0xffffffffu)
    if (Sim* sim = Sim::From(u->luaState())) sim->ResumeCommandThread(u);
}
}  // namespace

namespace {

// !Unit::WontFitAt 0x62aa90: the point inside the playable rect (STIMap::IsWithin, margin 0, the unit's army's
// whole-map flag, 0x62aad4), and the unit's footprint fits at world position p
bool StandableFor(Sim& sim, Unit* u, const Vec3& p) {
  if (sim.map() && !sim.IsWithin(p.x, p.z, 0.0f, Sim::UseWholeMap(u->army))) return false;
  const PathGrid* g = UnitFootprintGrid(sim, u);  // (Entity::GetFootprint 0x62aaa0)
  if (!g) return true;
  int cx, cz;
  GoalCell(UnitFootprint(u), p.x, p.z, &cx, &cz);
  return g->Passable(cx, cz);
}
bool CanFlyU(const Unit* u) { return u->motion.bp && u->motion.bp->canFly; }

// CAiTarget::HasTarget 0x5e2a10 as the attack task asks it of its own target. A player's order holds the army's
// recon BLIP of the enemy (CommandTarget: viaBlip), and of a blip HasTarget asks the blip itself (0x5e2a5a: its
// +0x99, which only Entity::Kill 0x679a80 sets - Unit::Kill's call, and slot 31 - and nothing kills a blip): the
// order's target lasts as long as the blip, past its unit's death. (HasTarget itself now says the same.)
bool TaskHasTarget(Sim& sim, const AiTarget& t) {
  if (t.type == 1 && t.viaBlip && !t.fakeId)
    if (Entity* e = TargetEntity(sim, t); e && e->kind == Entity::Kind::Unit)
      return static_cast<const Unit*>(e)->blip != nullptr;
  return HasTarget(sim, t);
}
// CAiTarget::EntityLost 0x5e2db0: the entity behind the target (a blip's unit, GetEntity 0x5e2ce0) is killed (its
// +0x99). A dead unit's blip HAS a target that is LOST.
bool TaskTargetLost(Sim& sim, const AiTarget& t) {
  if (t.type != 1) return false;
  const Entity* e = TargetEntity(sim, t);
  // a recon blip held as the target (CommandTarget, its unit gone): GetEntity is its unit, and no unit is lost too
  // (0x5e2dc6: je 0x5e2dd6, al = 1)
  if (e && e->kind == Entity::Kind::Blip) {
    const Unit* s = static_cast<const ReconBlip*>(e)->source;
    return !s || s->dead || !sim.FindEntity(EntityRef(s));
  }
  return e && e->dead;
}

void InitAttack(Sim& sim, Unit* u, BuildTask& t, bool coordinated) {
  auto d = std::make_shared<AttackData>();
  d->coordinated = coordinated;
  t.adata = d;
  t.state = 0;
  u->unitStates.Insert(US("Attacking"));
  if (!coordinated) u->navIgnoreFormation = true;
  AiTarget target = CommandTarget(sim, t);
  if (target.type == 1)
    if (Entity* e = sim.FindEntity(target.entityId))
      d->trackMobile = e->kind == Entity::Kind::Unit && BpInCategory(sim, e->blueprint, "MOBILE");
  // UpdatePos 0x5f2e90: lastPos = the target's position when the target holds (HasTarget 0x5e2a10); then, only when a
  // component is NaN (0x5657f0: _isnan per component), the unit's own position. The zero vector the ctor starts with
  // is a valid point and stays: a task whose target is already gone keeps lastPos (0, 0, 0), which is never within
  // its attack range, so the lost-target branch (0x5f3994) does not end it: it asks the navigator every beat. Seen
  // (probe mp2_8416b): MP2 8416, army 1's Striker 0x10000d starts the stream's queued Attack of 8351 on army 0's
  // 0x104 (dead at 8353, its blip gone at 8369): the original's task holds a null target and lastPos (0, 0, 0) and
  // returns 1 every beat (state 3) with the unit standing Attacking; ours put lastPos at the unit, in reach, and
  // ended the task at once.
  if (TaskHasTarget(sim, target)) d->lastPos = TargetPos(sim, target, false);
  if (IsNaN(d->lastPos)) d->lastPos = u->position;
  // 0x5f2c22..0x5f2c72: a unit standing unpacked (Immobile) whose blueprint has AI.NeedUnpack (bp+0x48c) and that
  // has an attacker is told SetDesiredTarget(none) (attacker vt+0x20 with an empty CAiTarget): every weapon drops
  // what it holds and packs up, and the task's own target is taken up later. (Seen: LAND, a heavy artillery
  // (url0304) unpacking for a target of its own was given a patrol's attack child at 1638; the original's MainGun
  // went to WeaponPackingState in that beat and the unit could move again at 1644, ours kept its target and stood
  // unpacked until the child's first run six beats later.)
  AttackerUnpackStop(sim, u);
  if (CanFlyU(u)) d->firstUpdate = false;
  d->cmd = MarkHeadCoordinationStarted(u);
}

// CAiAttackerImpl::VectorIsWithinAttackRange 0x5d70e0: any enabled weapon reaches p (2D)
bool VectorInAttackRange(Unit* u, const Vec3& p) {
  for (UnitWeapon* w : u->weapons)
    if (w->bp && w->enabled && DistXZ(u->position, p) <= MaxR(w)) return true;
  return false;
}

void AttackSetGoalRect(Sim& sim, Unit* u, int x0, int z0, int x1, int z1) {
  {
    static const long dbgGoal = getenv("MOHO64_DEBUG_ATTACKGOAL") ? atol(getenv("MOHO64_DEBUG_ATTACKGOAL")) : -1;
    if (dbgGoal >= 0 && static_cast<long>(u->id) == dbgGoal)
      Logf(LogLevel::Info, "attackgoal %u unit %u rect %d %d .. %d %d", sim.tick(), u->id, x0, z0, x1, z1);
  }
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {
    // CAiNavigatorAir::SetGoal 0x5a4c60 (air_nav.md 4.2): the goal cell is the rect's own cell when it is 1 x 1,
    // else the BORDER cell nearest the unit's cell (ClosestGoalCell 0x5a4b00: x outer, z inner, 16-bit
    // differences, the first of equals), and the point flown to is that cell's footprint centre (CellToWorld
    // 0x50b0f0: cell + size / 2), not the rect's middle. The navigator's goal is no order to move: the unit's
    // states stay as they are.
    const NamedFootprint& fp = UnitFootprint(u);
    const int ux = static_cast<int16_t>(std::nearbyint(u->position.x - fp.sizeX * 0.5f));
    const int uz = static_cast<int16_t>(std::nearbyint(u->position.z - fp.sizeZ * 0.5f));
    int bx = x0, bz = z0;
    if (!(x0 == x1 - 1 && z0 == z1 - 1)) {
      float bestD = std::numeric_limits<float>::infinity();
      for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z) {
          if (!(x == x0 || x == x1 - 1 || z == z0 || z == z1 - 1)) continue;
          const float dx = static_cast<float>(static_cast<int16_t>(ux - x));
          const float dz = static_cast<float>(static_cast<int16_t>(uz - z));
          const float D = dx * dx + dz * dz;
          if (D < bestD) {
            bestD = D;
            bx = x;
            bz = z;
          }
        }
    }
    u->motion.failed = false;
    MotionSetGoal(sim, u, {Vec3{bx + fp.sizeX * 0.5f, u->position.y, bz + fp.sizeZ * 0.5f}}, false, sim.tick());
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
  const NamedFootprint& f = UnitFootprint(u);  // (0x5f2df9: Entity::GetFootprint)
  int cx = static_cast<int>(std::nearbyint(p.x - f.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(p.z - f.sizeZ * 0.5f));
  AttackSetGoalRect(sim, u, cx, cz, cx + 1, cz + 1);
}
// CAiNavigatorLand::SetDestUnit 0x5a4180: a 1x1 goal at (fistp(e.x - 0.5), fistp(e.z - 0.5))
void AttackSetDestUnit(Sim& sim, Unit* u, const Entity* e) {
  {
    static const long dbgGoal = getenv("MOHO64_DEBUG_ATTACKGOAL") ? atol(getenv("MOHO64_DEBUG_ATTACKGOAL")) : -1;
    if (dbgGoal >= 0 && static_cast<long>(u->id) == dbgGoal)
      Logf(LogLevel::Info, "attackgoal %u unit %u dest unit %u at %.9g %.9g %.9g", sim.tick(), u->id, e->id, e->position.x,
           e->position.y, e->position.z);
  }
  if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {  // CAiNavigatorAir::SetDestUnit: the exact spot
    u->motion.failed = false;
    AirSetDestEntity(sim, u, e);
    return;
  }
  int cx = static_cast<int>(std::nearbyint(e->position.x - 0.5f));
  int cz = static_cast<int>(std::nearbyint(e->position.z - 0.5f));
  AttackSetGoalRect(sim, u, cx, cz, cx + 1, cz + 1);
}

// The command whose formation the attack task holds (task+0x58), or null: the task lets go of it when the target
// first comes within reach (d.formation).
UnitCommand* AttackFormationCommand(Sim& sim, Unit* u, BuildTask& t) {
  if (t.adata->formation && !t.child && !u->commands.empty() && u->commands.front()->form && u->motion.bp &&
      u->motion.bp->mobile() && !BpInCategory(sim, u->blueprint, "TARGETCHASER") &&
      FormationHas(*u->commands.front(), u))
    return u->commands.front().get();
  return nullptr;
}

// Update 0x5f3020 (no formation)
void AttackUpdate(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  // 0x5f303e..0x5f30a5: a unit standing unpacked (IsUnitState(Immobile)) whose blueprint has AI.NeedUnpack (bp+0x48c)
  // and that has an attacker is told SetDesiredTarget(none) first (as in the ctor 0x5f2c22): its weapons pack up so
  // it can drive to the goal set below.
  AttackerUnpackStop(sim, u);
  bool has = TaskHasTarget(sim, target);
  // The formation branch (0x5f30ae..0x5f3270): units ordered to attack as a group each go for THEIR place in the
  // command's formation, which is laid out around the target, not for the target itself. (Only while the task
  // still holds the formation: it lets go of it when the target first comes within reach, d.formation.)
  UnitCommand* fc = AttackFormationCommand(sim, u, t);
  if (fc) {
    Vec3 cell;
    if (d.trackMobile) {  // a mobile target: the formation is centred on where it is now (SetCentre)
      if (has) d.lastPos = TargetPos(sim, target, false);  // UpdatePos 0x5f2e90 (see InitAttack)
      if (IsNaN(d.lastPos)) d.lastPos = u->position;
      FormationSetCentre(*fc, d.lastPos.x, d.lastPos.z);
    } else {
      d.lastPos = FormationGoalOfForm(*fc, u, &cell) ? cell : u->position;
    }
    if (d.firstUpdate) {
      UnitWeapon* fw = TargetWeapon(sim, u, target);
      float ax = 0, az = 0;
      const bool adj = fw && FormationAdjustedOfForm(*fc, u, &ax, &az);
      static const bool dbgForm = getenv("MOHO64_DEBUG_FORM") != nullptr;  // (read once)
      if (dbgForm)
        Logf(LogLevel::Info, "attack form %u unit %u track %d has %d lastPos %.3f %.3f weapon %d adjusted %d %.3f %.3f",
             sim.tick(), u->id, d.trackMobile ? 1 : 0, has ? 1 : 0, d.lastPos.x, d.lastPos.z, fw ? 1 : 0, adj ? 1 : 0, ax, az);
      if (adj) {
        // SetWeaponGoal 0x5f2ce0 around the unit's adjusted place
        const int R = static_cast<int>(fw->bp->maxRadius);
        const int x0 = static_cast<int>(std::nearbyint(ax - R * 0.5f)), z0 = static_cast<int>(std::nearbyint(az - R * 0.5f));
        if (R < 1) AttackSetPosGoal(sim, u, Vec3{ax, 0, az});
        else AttackSetGoalRect(sim, u, x0, z0, x0 + R, z0 + R);
      } else if (FormationGoalOfForm(*fc, u, &cell)) {
        AttackSetPosGoal(sim, u, cell);  // (SetPosGoal 0x5f2d90 at its formation cell)
      }
    } else {
      AttackSetPosGoal(sim, u, has ? TargetPos(sim, target, false) : d.lastPos);
    }
    d.firstUpdate = false;
    return;
  }
  if (has) d.lastPos = TargetPos(sim, target, false);  // UpdatePos 0x5f2e90 (see InitAttack)
  if (IsNaN(d.lastPos)) d.lastPos = u->position;
  UnitWeapon* w = d.firstUpdate ? TargetWeapon(sim, u, target) : nullptr;
  Entity* e = target.type == 1 ? sim.FindEntity(target.entityId) : nullptr;
  {
    // MOHO64_DEBUG_ATTACKGOAL=<unit id>: what the attacker's goal is made from (the target, its points)
    static const long dbgGoal = getenv("MOHO64_DEBUG_ATTACKGOAL") ? atol(getenv("MOHO64_DEBUG_ATTACKGOAL")) : -1;
    if (dbgGoal >= 0 && static_cast<long>(u->id) == dbgGoal && e && e->kind == Entity::Kind::Unit) {
      Unit* tu = static_cast<Unit*>(e);
      const CombatBpData& cb = CB(tu);
      const Vec3 pt = TargetPos(sim, target, false);
      std::string bones;
      for (size_t i = 0; i < cb.targetBones.size(); ++i) {
        const int b = TargetBoneOf(e, cb.targetBones[i]);
        Vec3 bp{};
        Quat bq;
        if (b >= 0) BoneWorld(e, b, &bp, &bq);
        char buf[160];
        std::snprintf(buf, sizeof buf, " [%zu %s bone %d at %.4f %.4f %.4f]", i, cb.targetBones[i].c_str(), b, bp.x, bp.y, bp.z);
        bones += buf;
      }
      Logf(LogLevel::Info,
           "attackgoal %u unit %u first %d weapon %s target %u %s point index %d aim bone %d: point %.6f %.6f %.6f, place "
           "%.6f %.6f %.6f, track mobile %d has %d;%s",
           sim.tick(), u->id, d.firstUpdate ? 1 : 0, w ? w->bp->label.c_str() : "-", e->id,
           tu->blueprint ? tu->blueprint->id.c_str() : "?", t.targetPoint, target.aimBone, pt.x, pt.y, pt.z, e->position.x,
           e->position.y, e->position.z, d.trackMobile ? 1 : 0, has ? 1 : 0, bones.c_str());
    }
  }
  if (w) {
    // SetWeaponGoal 0x5f2ce0: a square of side trunc(MaxRadius) around the target's aim point. The point is asked
    // of GetTargetPosGun 0x5e2a90 with its flag CLEAR (0x5f32bc: xor bl, bl), which for a unit is its target POINT
    // (Unit::GetTargetPoint 0x6ab340: the target bone the order drew), not its place. A ship's target bone can
    // sit two units from its centre, and the square's corner is a whole cell: one cell off, another path.
    Vec3 p = TargetPos(sim, target, false);
    {
      static const long dbgGoal0 = getenv("MOHO64_DEBUG_ATTACKGOAL") ? atol(getenv("MOHO64_DEBUG_ATTACKGOAL")) : -1;
      if (dbgGoal0 >= 0 && static_cast<long>(u->id) == dbgGoal0) {
        const Vec3 c = TargetPos(sim, target, true);
        Logf(LogLevel::Info, "attackgoal %u unit %u aim bone %d: point %.9g %.9g %.9g, place %.9g %.9g %.9g", sim.tick(),
             u->id, target.aimBone, p.x, p.y, p.z, c.x, c.y, c.z);
      }
    }
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
  } else if (d.trackMobile && has && e && !e->destroyQueued) {
    // 0x5f32e3..0x5f3325: a mobile target goes to the navigator's SetDestUnit (vt+0xc) whenever HasTarget says so,
    // with the target's own entity (+0x64): for a blip-held target that is the blip, which outlives its unit. No
    // test of the unit's death: the Update of state 1 that follows the weapon's "lost" report (code 4) sets the
    // destination on the dead unit's blip, and the air SetTarget turns the facing toward it before the task ends.
    // Seen: Experimental2 1310, a Tempest's drones whose target died at 1309 turned toward the dead ship's place;
    // ours took the goal cell of its aim bone (another facing for the hover after the abort).
    AttackSetDestUnit(sim, u, e);
  } else {
    AttackSetPosGoal(sim, u, has ? TargetPos(sim, target, false) : d.lastPos);  // (0x5f3332: the flag clear too)
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

// CUnitAttackTargetTask's AbortNavigation 0x5f3420: the navigator's IgnoreFormation(false) (vt+0x30) and AbortMove
// (vt+0x10), with no test of a goal: an idle navigator's AbortMove still asks SetSpeedThroughGoal(0), which with the
// last waypoint in the goal turns the drive-through byte off. Seen: MP1 5243, army 1's frigate 0x100056, engaged
// (AttackAngle: state 4 every ten beats) after the AI's appended Attack turned its byte on at 5234: the original's
// byte is 0 from its next engaged tick; ours, with no goal to abort, kept 1.
void AbortNavigation(Unit* u) {
  u->navIgnoreFormation = false;
  if (u->motion.bp && u->motion.bp->mobile()) MotionStop(u);
  u->unitStates.Erase(US("Moving"));
}

// the weapon's state report (attacker-event listener 0x5f3ee0)
void AttackEvent(Sim& sim, Unit* u, BuildTask& t, const AiTarget& target) {
  AttackData& d = *t.adata;
  if (t.state == 5) return;
  // U.IsMobile (vt+0x30 = IUnit::IsMobile 0x6a4a10: the unit has a motion, u+0x4b0) at 0x5f3f04 / 0x5f3f32 /
  // 0x5f3f54 / 0x5f3f76: not the Immobile state; an artillery standing unpacked is still mobile here
  bool mobile = IsMobile(u);
  if (!TaskHasTarget(sim, target)) {
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
  if (!TaskHasTarget(sim, target) && u->commands.size() >= 2) return -1;  // more orders: give up at once
  // 0x5f3526..0x5f3611: no weapon for the target ends the task unless it is coordinated (+0x8d), the unit is
  // DIRECTFIRE, or it is a sub under water that surfaces to attack (AI.AutoSurfaceToAttack, MotionType
  // SurfacingSub (bp+0x290 == 5) and layer Sub (unit+0x120 == 4)). Seen: MP1 7411, army 1's Aeon sub 0x100056
  // (uas0203) on the blip of a structure that its upgrade replaced: no weapon can take a blip without its unit
  // (CanAttackTarget 0x6d57f0), and the original's sub keeps the Attack until the blip goes (7418).
  const bool surfaceToAttack = CB(u).autoSurfaceToAttack && u->motion.bp &&
                               u->motion.bp->motionType == kMotionSurfacingSub && u->layer == "Sub";
  if (!w && !d.coordinated && !direct && !surfaceToAttack) return -1;
  // state 1's test (0x5f366e) is U.IsMobile (vt+0x30 = IUnit::IsMobile 0x6a4a10: the unit has a motion, u+0x4b0),
  // not the Immobile state: a heavy artillery standing unpacked takes the mobile branch (Update, then the chase),
  // and Update's first step packs it up (0x5f3020). (Seen: LAND, url0304 (unit 0x20) given a patrol's attack child
  // at 1640 while unpacked; at the child's first run (1646) the original's navigator took the weapon square's goal
  // (276.5, 622.5) and was stopped again the next beat; ours took the structure branch (UpdateAttacker, state 4).)
  bool mobile = IsMobile(u);
  switch (t.state) {
    case 0:
      // 0x5f362a..0x5f364f: a coordinated order (IsCoordinating 0x5f24e0) waits, asking again in 10 beats, until it
      // and every partner still alive have an attack task (HasSatisfiedCoordination 0x6e90a0). (A null +0x54 ends
      // the task, -1: the command was deleted; ours ends the task with its command.)
      if (std::shared_ptr<UnitCommand> c = d.cmd.lock())
        if (IsCoordinating(*c) && !CoordinationSatisfied(*c)) return 10;
      t.state = 1;
      return 0;
    case 1:
      if (!mobile) {
        UpdateAttacker(sim, u, t, target);
        t.state = 4;
        return 0;
      }
      // 0x5f3680..0x5f36f5: such a sub with no weapon for a target it has and has not lost must surface: only in
      // the unit's auto-surface mode (Unit::IsAutoSurfaceMode 0x6a4a40, unit+0x291, vt+0x64; off unless the
      // player's SetAutoSurfaceMode turns it on), else the task ends. Seen: MP1 7090, army 1's Aeon sub 0x1000b6
      // (uas0203) ordered onto a land unit's blip: the original's order fails in the beat it is dispatched.
      // (The surfacing itself, a move to the cell's surface layer (0x579300, 0x5a2cb0, layer 8, 0x60b890), is not
      // built: in auto-surface mode the sub chases as any other unit.)
      if (surfaceToAttack && !w && TaskHasTarget(sim, target) && !TaskTargetLost(sim, target) && !u->autoSurfaceMode)
        return -1;
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
        AttackerUnpackStop(sim, u);  // (after the navigator's SetGoal: 0x5f38b8)
        return 10;
      }
      AttackUpdate(sim, u, t, target);
      t.state = 3;
      return 0;
    }
    case 3: {  // chase
      // 0x5f391d..0x5f3993: a member of a FormAttack's formation (the task holds it, +0x58, and is coordinated, +0x8d)
      // that is not the formation's leader (u+0x584; also none) does not chase: it stays with the formation, asking
      // every beat (return 1) whether the leader's attacker has its desired target (0x5f2fb0: leader +0x544 vt+0x24,
      // HasTarget 0x5e2a10); then it lets go of the formation (+0x58 = 0, +0x8d = 0, the navigator's
      // IgnoreFormation(true)) and goes for the target itself (Update 0x5f3020). Seen: MP1 2600, army 5's FormAttack
      // of three interceptors on a blip: the original's 0x500023, 20 from the target, keeps the formation's speed
      // cap at 2601 and flies to its slot by the leader 0x500028 from 2603; ours took the in-range branch at once.
      if (d.coordinated && AttackFormationCommand(sim, u, t) && u->formLeader != EntityRef(u)) {
        Entity* le = u->formLeader ? sim.FindEntity(u->formLeader) : nullptr;
        Unit* L = le && le->kind == Entity::Kind::Unit ? static_cast<Unit*>(le) : nullptr;
        if (L && !L->weapons.empty() && HasTarget(sim, L->desiredTarget)) {
          d.formation = false;
          d.coordinated = false;
          u->navIgnoreFormation = true;
          AttackUpdate(sim, u, t, target);
        }
        return 1;
      }
      bool has = TaskHasTarget(sim, target);
      bool busy = u->motion.hasGoal;
      if (!has || TaskTargetLost(sim, target)) {  // 0x5f3994: EntityLost 0x5e2db0, then HasTarget 0x5e2a10
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
        // 0x5f3a10: the task lets go of the command's formation (+0x58 = 0) and of FormAttack (+0x8d = 0); then the
        // navigator ignores formations. Seen (Subs 1240): two torpedo bombers given one attack order on a sub
        // (977) are in reach long before it dies; the Update after its death takes the branch WITHOUT a formation
        // and flies at the dead sub's blip (SetDestUnit: its very place, the facing the brake turns to; bit-exact),
        // not at its aim point's cell (the formation branch's SetPosGoal).
        d.formation = false;
        d.coordinated = false;
        u->navIgnoreFormation = true;
        UpdateAttacker(sim, u, t, target);
        if (oc) {  // the overcharge weapon is given the target by the task itself (0x6d5f30)
          SetTarget(sim, oc, target);
          t.state = 4;
          return 1;
        }
        return -2;  // suspended; the navigator keeps driving until the weapon reports
      }
      // 0x5f3a7e..0x5f3a95: the attacker's IsTooClose (vt+0x54, 0x5d7210) decides, over ALL its weapons, not the
      // target weapon's own status (see AttackerTooClose); its CanAttackTarget calls draw for a weapon with
      // AboveWaterTargetsOnly / BelowWaterTargetsOnly on a target on the seabed.
      if (!u->weapons.empty() && AttackerTooClose(sim, u, target)) {
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
      // the squares summed z, y, x; comiss / jbe (0x5f3b37..): a NaN distance counts as not moved
      if (!(std::sqrt((dv.z * dv.z + dv.y * dv.y) + dv.x * dv.x) > (CanFlyU(u) ? 2.0f : 10.0f))) return 1;
      Entity* e = sim.FindEntity(target.entityId);
      if (!e || e->dead || e->destroyQueued) return 1;
      if (!StandableFor(sim, u, tp)) return 1;  // UnitWontFitAt
      // 0x5f3370: while the task still holds the command's formation (+0x58), the formation is moved to the
      // target's point (SetCentre, vt+0x08) and the unit goes to ITS place in it (vt+0x1c, then SetPosGoal
      // 0x5f2d90), not to the target; only without one does the navigator get the target itself (SetDestUnit).
      // Seen: MP3 2009, a bomber and an air scout given one attack order on a radar blip (2002): the original's
      // goal from 2009 on is the bomber's formation cell, (220, 199) while the target is at (216.1, 197.2).
      if (UnitCommand* fc = AttackFormationCommand(sim, u, t)) {
        FormationSetCentre(*fc, tp.x, tp.z);
        Vec3 cell;
        if (FormationGoalOfForm(*fc, u, &cell)) AttackSetPosGoal(sim, u, cell);
      } else {
        AttackSetDestUnit(sim, u, e);
      }
      d.lastPos = tp;
      return 1;
    }
    case 4:  // engaged: stop, the weapons fire; the weapon's reports drive the task
      if (!CanFlyU(u)) AbortNavigation(u);
      // AI.AttackAngle (0x5f3d..): the unit keeps the target that many degrees off its bow; the direction to the
      // target turned about the up axis away from the target's side goes to the motion (SetFacing 0x6b8590)
      if (const float aa = CB(u).attackAngle; aa > 0 && TaskHasTarget(sim, target)) {
        Vec3 tp = TargetPos(sim, target, false);
        if (!IsNaN(tp)) {
          Vec3 v{tp.x - u->position.x, tp.y - u->position.y, tp.z - u->position.z};
          const Quat& q = u->orientation;
          const float fx = (q.w * q.y + q.z * q.x) * 2.0f, fz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
          // (0x5f3cd4: the direction's normalised copy 0x44f7e0, ((x*x + y*y) + z*z), above 0 times 1 / len, else
          // zero; the side test and the turn take it, 0x5f3cd9..0x5f3cfd. The turn is the quaternion 0x570750 of the
          // half angle about (0, 1, 0) (w = cosf, xyz = axis x sinf), applied by 0x452d40 (0x5f3d73), and SetFacing
          // 0x6b8590 normalises the result (0x452af0). Ours turned the raw direction by the full angle's sine and
          // cosine.)
          Vec3 n{0.0f, 0.0f, 0.0f};
          if (const float l = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z); l > 0.0f) {
            const float inv = 1.0f / l;
            n = {v.x * inv, inv * v.y, inv * v.z};
          }
          const float ang = (n.z * fx - fz * n.x > 0.0f) ? aa * 0.017453292f : aa * -0.017453292f;
          const float h = ang * 0.5f, sn = dmath::Sin(h);
          const Quat r{0.0f * sn, 1.0f * sn, sn * 0.0f, dmath::Cos(h)};
          Vec3 f = RotateM(r, n);
          vm::NormalizeExe(f);
          u->motion.faceDir = f;
        }
        return 10;
      }
      if (!oc) return -2;
      // overcharge (0x5f3dc3): the weapon's script says whether it may fire (energy); a standing mobile unit
      // whose overcharge has no firing solution goes back to chasing; otherwise fire once and end
      if (CallMethodBool(sim, sim.L(), oc, "CanWeaponFire", 0) <= 0) return 10;
      // (0x5f3de3..0x5f3e07: the position against the one before by memcmp 0x4f0a50, then IsMobile vt+0x30)
      if (SameBits(u->position, u->lastPosition) && u->motion.bp && u->motion.bp->mobile() && !oc->canReach) {
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

// CUnitAttackTargetTask's command listener 0x5f4000 (linked by the ctor 0x5f2980 at 0x5f2aee..0x5f2b1e to the
// broadcaster of the command heading the unit's queue, +0x54), told by CUnitCommand::SetTarget 0x6e8820 (0x6e94a0):
// - the command's new target has no entity (cmd+0x120) while the task's has one (+0x64): state 5 (0x5f4048), the task
//   ends at its next tick;
// - else the task takes the command's target (0x5d5670: cmd+0x11c into +0x60); an entity target goes to the attacker
//   (UpdateAttacker 0x5f3450) only while the attacker has a desired target (vt+0x24, HasTarget 0x5e2a10); a ground
//   target clears the attacker's (UpdateAttacker with an empty CAiTarget, 0x5f40b4..0x5f40d7); state 1 (0x5f4105):
//   the next tick's Update plans for the new target;
// - the task's thread is woken (0x5f410c..0x5f4140).
// Seen: MP1 14957 / 14961, army 0's player drags the ground Attack 0x2b of its strategic bomber 0x18f (uaa0304) to
// (579.67, 335.37) and (578.74, 333.71): the original's air target turns to (580, 335) at 14958 and (579, 334) at
// 14962; ours kept flying at the first point (581, 333).
// (The patrol's and the guard's child attack tasks link the same listener to their parent's command; they are not
// handled here.)
void AttackTaskCommandEvent(Sim& sim, Unit* u, BuildTask& t, const UnitCommand& c) {
  {
    if (!t.adata || t.child) return;
    AttackData& d = *t.adata;
    if (d.cmd.lock().get() != &c) return;
    const AiTarget old = CommandTarget(sim, t);
    const bool cmdEntity = c.targetId && sim.FindEntity(c.targetId);
    const bool taskEntity = old.type == 1 && sim.FindEntity(old.entityId);
    if (!cmdEntity && taskEntity) {
      t.state = 5;
    } else {
      t.goalId = c.targetId;
      t.targetPoint = c.targetPoint;
      t.targetViaBlip = c.targetViaBlip;
      t.targetPastDeath = c.targetPastDeath;
      t.targetFakeId = c.targetFakeId;
      t.targetBlipRef = c.targetBlipRef;
      t.site = c.pos;
      if (c.targetId)
        if (Entity* e = sim.FindEntity(c.targetId)) t.site = e->position;
      const AiTarget now = CommandTarget(sim, t);
      if (now.type == 1 && sim.FindEntity(now.entityId)) {
        if (!u->weapons.empty() && HasTarget(sim, u->desiredTarget)) UpdateAttacker(sim, u, t, now);
      } else {
        UpdateAttacker(sim, u, t, AiTarget{});
      }
      t.state = 1;
    }
    t.waitUntil = 0;  // (the thread woken)
  }
}

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
  // DispatchTask's Attack case (0x609639 -> CAttackTargetTask::operator new 0x5f2750 -> ctor 0x5f2980) makes the task
  // for any unit, weapons or not; a unit with no weapon for the target ends it in its first tick (TaskTick 0x5f34c0,
  // AttackStep's "no weapon" return), in the beat it was dispatched. Seen (MP3 2003): army 1's air scout 0x10001e
  // (a DummyWeapon only) is ordered with a bomber to attack a blip: the original's queue is empty the next beat; ours
  // made no task and flew at the target as an approach.
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->order = "Attack";
  t->goalId = c.targetId;
  t->targetPoint = c.targetPoint;
  t->targetViaBlip = c.targetViaBlip;
  t->targetPastDeath = c.targetPastDeath;
  t->targetFakeId = c.targetFakeId;
  t->targetBlipRef = c.targetBlipRef;
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
  t->targetPastDeath = c.targetPastDeath;
  t->targetFakeId = c.targetFakeId;
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
        if (!u->unitStates.Has(US("Busy"))) {
          if (nuke) sim.CallMethod(sim.L(), u, "OnNukeLaunched", 0);
          SetTarget(sim, w, target);
          FireNow(sim, w);
          t.state = 3;
        }
        t.waitUntil = sim.tick() + 2;  // return 3
        return kTaskRunning;
      case 3:
        if (u->unitStates.Has(US("Busy"))) t.state = 4;
        t.waitUntil = sim.tick() + 2;
        return kTaskRunning;
      default:
        if (!u->unitStates.Has(US("Busy"))) return kTaskDone;
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
  if (!u->dead) u->unitStates.Erase(US("Busy"));
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
  t->targetPastDeath = c.targetPastDeath;
  t->targetFakeId = c.targetFakeId;
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
      const int st0 = t.state;
      int r = AttackStep(sim, u, t, target);
      {
        // MOHO64_DEBUG_ATTACKTASK=<unit id>: every step of the unit's attack task (state, the step's code, its target)
        static const long dbgAT = getenv("MOHO64_DEBUG_ATTACKTASK") ? atol(getenv("MOHO64_DEBUG_ATTACKTASK")) : -1;
        if (dbgAT >= 0 && static_cast<long>(u->id) == dbgAT) {
          const Entity* te = TargetEntity(sim, target);
          Logf(LogLevel::Info,
               "attacktask %u unit %u state %d -> %d r %d has %d lost %d queue %zu target type %d entity %u (%s) via blip %d "
               "attackState %d lastPos %.4f %.4f %.4f",
               sim.tick(), u->id, st0, t.state, r, TaskHasTarget(sim, target) ? 1 : 0, TaskTargetLost(sim, target) ? 1 : 0,
               u->commands.size(), target.type, te ? te->id : 0u,
               !te ? "-" : te->kind == Entity::Kind::Unit ? "unit" : te->kind == Entity::Kind::Blip ? "blip" : "other",
               target.viaBlip ? 1 : 0, u->attackState, d.lastPos.x, d.lastPos.y, d.lastPos.z);
        }
      }
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
  if (ahead > 0 && d < 60) u->unitStates.Insert(US("MakingAttackRun"));
  else u->unitStates.Erase(US("MakingAttackRun"));
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
  u->unitStates.Erase(US("Attacking"));
  // the dtor 0x5f4160: IgnoreFormation(false), attacker.Stop() 0x5d56f0 = SetDesiredTarget(none) whatever the
  // desired target was (every weapon is told SetTarget(none): one that still holds a target, also a long dead
  // one, gets OnLostTarget - a missile launcher packs up), then AbortMove (the caller)
  // The Stop has no test of the unit's life either (0x5f41f1..0x5f4217: only that the attacker unit+0x544 exists, which
  // KillCleanup deletes a beat later), so a unit killed on an Attack (Unit::Kill empties its queue, 0x6a841a) has its
  // weapons' OnLostTarget run in its Kill. Seen: MP2 7298, army 0's Corsair 0xe0 (dra0202) shot down while its
  // GroundMissile held a target: that weapon's OnLostTarget sets SetSpeedMult(1.0) (DRA0202_Script.lua 66), and the
  // original's falling Corsair has top speed 15 from 7299; ours skipped the Stop for the dead unit and kept 0.67 x 15.
  if (!u->destroyQueued && !u->combatGone && !u->weapons.empty()) SetDesiredTarget(sim, u, AiTarget{});
  if (u->dead || u->destroyQueued) return;
  u->navIgnoreFormation = false;
}

// CAiAttackerImpl::Stop 0x5d56f0: SetDesiredTarget (vt+0x20) with an empty target. Every weapon drops its target
// and its acquire task starts its period again from now (a weapon without AutoInitiateAttackCommand: a whole
// TargetCheckInterval).
void AttackerStop(Sim& sim, Unit* u) {
  if (u->dead || u->destroyQueued || u->combatGone || u->weapons.empty()) return;
  SetDesiredTarget(sim, u, AiTarget{});
}

void AttackerUnpackStop(Sim& sim, Unit* u) {
  if (u->unitStates.Has(US("Immobile")) && CB(u).needUnpack && !u->weapons.empty()) SetDesiredTarget(sim, u, AiTarget{});
}

void ClearEngagement(Sim& sim, Unit* u) {
  if (!u->engageId) return;
  u->engageId = 0;
  if (!u->dead && !u->destroyQueued) {
    SetDesiredTarget(sim, u, AiTarget{});
    u->unitStates.Erase(US("Attacking"));
  }
}

// ---- intel and unit bindings ------------------------------------------------------------------------

namespace {

Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

// GetTargetEntity 0x6c4bc0: the attacker's (unit+0x544) desired target (vt+0x24), nil without an attacker
// (0x6c4c48..0x6c4c50), as after Unit::KillCleanup 0x6a8790 deleted it (ours: combatGone).
int l_GetTargetEntity(lua_State* L) {
  Unit* u = U(L);
  Entity* e = u->combatGone ? nullptr : TargetEntity(*S(L), u->desiredTarget);
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
// Unit:SetStunned(seconds) 0x6c68e0: a number of beats, not a flag: lua_tonumber x 10.0 (0x6c69a5, a float product:
// the x87 runs at 24 bits), truncated (_ftol 0xa89cc0), into u+0x29c, which Unit::MotionTick counts down. (Seen: LAND
// 1469, a dying Loyalist's SetStunned(1.5) holds an Othuum's guns for 15 beats, it fires again at 1484; ours held it
// for good.)
int l_SetStunned(lua_State* L) {
  Unit* u = U(L);
  u->stunned = static_cast<int>(static_cast<float>(luaL_checknumber(L, 2)) * 10.0f);
  return 0;
}
int l_IsStunned(lua_State* L) {
  lua_pushboolean(L, U(L)->stunned != 0);  // (0x6c5d1c)
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
  lua_pushboolean(L, static_cast<int>(e->shooters.size()) >= cap);
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
// UnitWeapon:ChangeMaxRadius(value) 0x6d9c80 (bound at 0x6d9c20): weapon+0x64 = value, +0x6c = value^2, and the
// unit's byte +0x68e = 1 (the mark many interface-visible changes set: SetFocusEntity, SetCreator, the silo
// counts). The unit's blip cache (GetBlipsInRange 0x6ad060: its tick at unit+0x668) is not touched.
int l_w_ChangeMaxRadius(lua_State* L) {
  UnitWeapon* w = W(L);
  float v = static_cast<float>(luaL_checknumber(L, 2));
  w->ovMaxRadius = v;
  w->maxR2 = v * v;
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
    if (w->target.fakeId) {
      // cfunc_UnitWeaponGetCurrentTargetL 0x6d9390 pushes the CAiTarget's entity, the weak pointer at weapon+0xd0
      // (0x6d9418..0x6d943d), or nil when it is empty (0x6d9456): for one of a jammer's FAKE blips that is the fake
      // itself, and once the fake is destroyed nothing, not the unit's real blip. Seen: MP1 10611, army 6's three
      // torpedoes 0x10600000 / 05 / 21 (aantorpedo01 from 0x60000f, homing on a fake of army 5's jammer frigate
      // 0x5000b1) lose their target when the last army lets go of the fakes (probe mp1_10612: the jammer off from
      // 10601 in both, the shells' targets empty at 10611); FAF's Projectile.RetargetThread asks the weapon's
      // GetCurrentTarget at 10612: nil in the original (SetLifetime, no draw), ours gave the frigate's real blip and
      // SetNewTarget drew three target points.
      Entity* fake = S(L)->FindEntity(w->target.fakeId);
      e = fake;  // (a fake queued for destruction is still there until the end of the beat)
    } else if (tu->blip && static_cast<Entity*>(tu->blip)->HasLuaObject()) {
      e = tu->blip;
    }
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
// ELayer by name (sim/enum_arg.h, SCR_GetEnum 0x4d2550: 0x6da4a3; LAYER_, None 0, Land 1, Seabed 2, Sub 4, Water 8,
// Air 16, Orbit 32, All 127): names in any case or-ed with '|', numbers; a part it does not take (also one with a
// space in it) raises "Invalid enum value"
int l_w_SetFireTargetLayerCaps(lua_State* L) {
  UnitWeapon* w = W(L);
  w->layerCaps = EnumValue(L, kEnumLayer, luaL_checkstring(L, 2));
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
// UnitWeapon:SetTargetingPriorities 0x6da560: the list is emptied, then i = 1 .. GetCount (0x6da64e, read again each
// pass; nothing here changes the list) through func_GetCObj_EntityCategory 0x5575e0: a value that is no category (a
// nil hole included) is a Lua error (GetUserData's luaG_typeerror), the categories before it are kept.
int l_w_SetTargetingPriorities(lua_State* L) {
  UnitWeapon* w = W(L);
  if (!lua_istable(L, 2)) return luaL_error(L, "Passed in an invalid table to priority categories");
  w->priorities.clear();
  const int n = ObjectCount(L, 2);
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    const uint64_t* c = ToCategory(L, -1);
    if (!c) luaL_error(L, "attempt to get as UserData a %s value", lua_typename(L, lua_type(L, -1)));
    w->priorities.emplace_back(c, c + CategoryWordCount(L));
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


// MoveManipulator 0x643860
void RotatorStep(Unit* u, RotateManipulator* r) {
  if (!r->enabled || r->bone < 0 || !u->skeleton || r->bone >= u->skeleton->Count()) return;
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

// ---- sliders (CSlideManipulator) --------------------------------------------------------------

// MoveManipulator 0x647300
void SliderStep(Unit* u, SlideManipulator* m) {
  const auto& bones = u->skeleton->bones();
  if (!m->enabled) return;
  if (m->bone < 0 || static_cast<size_t>(m->bone) >= bones.size()) return;
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
  if (bi < u->poseQ.size()) {
    u->poseP[bi] = Add(u->poseP[bi], RotateM(u->poseQ[bi], m->cur));
    ++u->poseVersion;
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
  // (0x636250..0x636296: the bone's conjugate (w, -0.0 - x, -0.0 - y, -0.0 - z) applied by 0x452d40, RotateM)
  Vec3 d = local ? dir : RotateM(Conj(boneRot), dir);
  float center = isHeading ? a->hCenter : a->pCenter, half = isHeading ? a->hHalf : a->pHalf;
  float* cur = isHeading ? &a->heading : &a->pitch;
  float target;
  if (isHeading) {
    target = dmath::Atan2(d.x, d.z);
  } else {
    // (0x6362ec..0x636333: the quaternion (w fcos(c/2), x fsin(c/2), +0, +0) applied by 0x452d40, then 0x50b710:
    // t = y / |e| with no test; ours turned by AxisAngle (-0 parts for a negative centre) through Rotate and
    // took 0 for a zero vector)
    const float hc = center * 0.5f;
    const Vec3 e = RotateM(Quat{dmath::Sin(hc), 0.0f, 0.0f, dmath::Cos(hc)}, d);
    const float len = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
    target = center - (AcosA(e.y / len) - 1.57079637f);
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
  (void)bones;
  if (a->yawBone >= 0 && a->yawBone < n) {
    // the yaw bone's frame this tick, before this manipulator's own rotation: the step runs in the arm's slot of
    // the unit's manipulators (UnitAimTickManipulators), on this beat's pose as the lower-precedence ones (the walk
    // animation) left it
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
    PoseRotate(u, a->yawBone, AxisAngle({0, 1, 0}, a->heading));  // CAniPoseBone::Rotate 0x54bc00: local x q
  }
  if (a->pitchBone >= 0 && a->pitchBone < n) {
    Vec3 p;
    Quat q;
    BoneWorld(u, a->pitchBone, &p, &q);
    r |= ArmAxis(a, dir, local, false, a->pSlew * (slow ? 0.25f : 1.0f), q);
    PoseRotate(u, a->pitchBone, AxisAngle({1, 0, 0}, -a->pitch));
  }
  bool moving = (r & 2) != 0;
  // Step 0x6361a6 / 0x6361e5: the callbacks come from the arm's own MoveManipulator, which runs in its precedence
  // slot of UpdateManipulators 0x63aa80, after the open animation (FAF: BuildingOpenAnimManip precedence 1, the arm
  // 5). Ours computes the arm before the unit's animators, so the call waits for the arm's slot. Seen: Engineers
  // 3190, engineer b's arm comes home and OnStopBuilderTracking reverses the open animation; called before the
  // animation's beat it took a step back, so at 3191 OnPrepareArmToBuild's WaitFor(open animation) waited a beat
  // and the arm (and the repair) started at 3192 / 3195; the original's at 3191 / 3194
  if (moving && !a->tracking) a->trackCall = 1;
  else if (!moving && a->tracking) a->trackCall = 2;
  a->tracking = moving;
  return (r & 1) == 0;
}

}  // namespace

// MoveManipulator 0x636590, split in two. Here, while the pose is still last beat's (before UnitAimTick rebuilds it),
// AimDir 0x6366f0 reads the aim bone from it; the step itself (Step 0x635fe0: the heading and pitch, the bones'
// rotation, the on-target flag) runs in the arm's slot among the unit's manipulators (BuilderArmSlotStep), on this
// beat's pose as the lower-precedence manipulators left it. (Seen: Economy 3562, SACU 0x26 still walking when it
// starts to aim; its yaw bone Torso is moved by the walk animation, precedence 0. With the frame taken from the rest
// pose ours' first heading target was -3.005 against the original's -2.950 (the 'R' records), the arm came on target
// a beat later and the structure 0x4f was made at 3568 instead of 3567. Every arm start of a walking builder in that
// replay differed by 4..8 degrees, every start of a standing one agreed.)
void BuilderArmsTick(Sim& sim, Unit* u, const Vec3& priorPos, const Quat& priorOri) {
  (void)sim;
  (void)priorPos;
  (void)priorOri;
  auto& v = u->builderArms;
  v.erase(std::remove_if(v.begin(), v.end(), [](BuilderArm* a) { return !a->alive; }), v.end());
  if (v.empty() || !HasBuilderObject(u) || u->dead) return;
  for (size_t i = 0; i < v.size(); ++i) {
    BuilderArm* a = v[i];
    if (!a->enabled || !a->alive) continue;
    a->stepPending = true;
    const Vec3 aim = u->armAim;
    if (aim.x == 0 && aim.y == 0 && aim.z == 0 && !std::signbit(aim.x) && !std::signbit(aim.y) &&
        !std::signbit(aim.z)) {  // (bitwise compare with the zero vector)
      a->stepZero = true;
      continue;
    }
    a->stepZero = false;
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
    float len = std::sqrt((d.z * d.z + d.x * d.x) + d.y * d.y);  // (0x636756..0x63677c)
    a->stepDir = len > 0 ? Mul(d, 1.0f / len) : Vec3{};
  }
}

// The arm's step in its slot (see BuilderArmsTick): false when BuilderArmsTick did not ask for one this beat (the
// arm then only puts its last angles on the bones).
bool BuilderArmSlotStep(Sim& sim, Unit* u, BuilderArm* a) {
  if (!a->stepPending) return false;
  a->stepPending = false;
  if (a->stepZero) {
    ArmStep(sim, a, {0, 0, 1}, true, true);
    {
      static const long dbg = getenv("MOHO64_DEBUG_ARM") ? atol(getenv("MOHO64_DEBUG_ARM")) : -1;
      if (dbg >= 0 && static_cast<long>(u->id) == dbg && u->armReady)
        Logf(LogLevel::Info, "armdbg %u zero-target tick: on target 1 -> 0", sim.tick());
    }
    u->armReady = false;
    a->onTarget = false;
    return true;
  }
  bool on = ArmStep(sim, a, a->stepDir, false, false);
  {
    static const long dbg = getenv("MOHO64_DEBUG_ARM") ? atol(getenv("MOHO64_DEBUG_ARM")) : -1;
    if (dbg >= 0 && static_cast<long>(u->id) == dbg && u->armReady != on)
      Logf(LogLevel::Info, "armdbg %u tracking tick: on target %d -> %d", sim.tick(), u->armReady ? 1 : 0, on ? 1 : 0);
  }
  a->onTarget = on;
  u->armReady = on;
  return true;
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
  a->seq = S(L)->NextManipSeq();
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
  CheckArm(L)->seq = S(L)->NextManipSeq();
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
  r->seq = S(L)->NextManipSeq();
  RotateManipulator* raw = r.get();
  if (u) u->rotators.push_back(raw);
  CreateObject(L, raw, "CRotateManipulator");
  S(L)->Own(std::move(r));
  return 1;
}
int l_rot_SetPrecedence(lua_State* L) {  // cfunc_IAniManipulatorSetPrecedenceL 0x63bb40
  RotateManipulator* r = CheckRot(L);
  r->precedence = static_cast<int>(luaL_checknumber(L, 2));
  r->seq = S(L)->NextManipSeq();
  lua_settop(L, 1);
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
  // (0x647599..0x6475a7: the goal, +0x8c, against where the slider is, +0x80, by memcmp 0x4f0a50: a -0.0 part is
  // another place)
  m->signalled = SameBits(m->cur, m->goal);
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
  m->seq = S(L)->NextManipSeq();
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
  CheckSlide(L)->seq = S(L)->NextManipSeq();
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
// CreateSlaver(unit, dest_bone, src_bone) 0x646330
int l_CreateSlaver(lua_State* L) {
  Unit* u = ToObject<Unit>(L, 1);
  auto m = std::make_unique<SlaveManipulator>();
  m->unit = u;
  if (u && u->skeleton) {
    m->destBone = ResolveBoneArg(L, u, 2);
    m->srcBone = ResolveBoneArg(L, u, 3);
  }
  m->seq = S(L)->NextManipSeq();
  SlaveManipulator* raw = m.get();
  if (u && u->skeleton) u->slavers.push_back(raw);
  else raw->unit = nullptr;
  CreateObject(L, raw, "CSlaveManipulator");
  S(L)->Own(std::move(m));
  return 1;
}
SlaveManipulator* CheckSlaver(lua_State* L) { return CheckObject<SlaveManipulator>(L, 1); }
int l_slaver_SetMaxRate(lua_State* L) {  // 0x646510: degrees a second
  CheckSlaver(L)->maxRate = static_cast<float>(luaL_checknumber(L, 2)) * 0.00174532924f;
  lua_settop(L, 1);
  return 1;
}
int l_slaver_SetPrecedence(lua_State* L) {
  CheckSlaver(L)->precedence = static_cast<int>(luaL_checknumber(L, 2));
  CheckSlaver(L)->seq = S(L)->NextManipSeq();
  lua_settop(L, 1);
  return 1;
}
int l_slaver_Enable(lua_State* L) {
  CheckSlaver(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_slaver_Disable(lua_State* L) {
  CheckSlaver(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_slaver_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* m = dynamic_cast<SlaveManipulator*>(o)) m->alive = false;
  if (o) o->UnbindLua();
  return 0;
}
// CreateFootPlantController(unit, footBone, kneeBone, hipBone, [straightLegs = true], [maxFootFall = 0]) 0x639d00
int l_CreateFootPlantController(lua_State* L) {
  const int top = lua_gettop(L);
  if (top < 4 || top > 6)
    return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", "CreateFootPlantController", 4, 6, top);
  Unit* u = ToObject<Unit>(L, 1);
  if (!u || !u->skeleton) return luaL_error(L, "Unit has no skeleton.");  // (0x639d9e: the unit's actor, +0x540)
  auto m = std::make_unique<FootPlantManipulator>();
  m->unit = u;
  m->footBone = ResolveBoneArg(L, u, 2);
  m->kneeBone = ResolveBoneArg(L, u, 3);
  m->hipBone = ResolveBoneArg(L, u, 4);
  if (top > 4) m->straightLegs = lua_toboolean(L, 5) != 0;
  if (top > 5) m->maxFootFall = static_cast<float>(luaL_checknumber(L, 6));
  // 0x6394f0..0x639593: half the foot's height above (or below) the hip, in the pose as it is now
  if (m->footBone >= 0 && m->hipBone >= 0) {
    Vec3 fp, hp;
    Quat q;
    BoneWorld(u, m->footBone, &fp, &q);
    BoneWorld(u, m->hipBone, &hp, &q);
    m->halfLeg = std::fabs(fp.y - hp.y) * 0.5f;
  }
  m->seq = S(L)->NextManipSeq();  // (0x63ace0: behind the others of precedence 0)
  FootPlantManipulator* raw = m.get();
  u->footPlants.push_back(raw);
  CreateObject(L, raw, "CFootPlantManipulator");
  S(L)->Own(std::move(m));
  return 1;
}
FootPlantManipulator* CheckFootPlant(lua_State* L) { return CheckObject<FootPlantManipulator>(L, 1); }
int l_foot_SetPrecedence(lua_State* L) {  // cfunc_IAniManipulatorSetPrecedenceL 0x63bb40
  FootPlantManipulator* m = CheckFootPlant(L);
  m->precedence = static_cast<int>(luaL_checknumber(L, 2));
  m->seq = S(L)->NextManipSeq();
  lua_settop(L, 1);
  return 1;
}
int l_foot_Enable(lua_State* L) {
  CheckFootPlant(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_foot_Disable(lua_State* L) {
  CheckFootPlant(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_foot_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* m = dynamic_cast<FootPlantManipulator*>(o)) m->alive = false;
  if (o) o->UnbindLua();
  return 0;
}
// CreateThrustController(unit, label, thrustBone) 0x64abc0
int l_CreateThrustController(lua_State* L) {
  const int top = lua_gettop(L);
  if (top != 3)
    return luaL_error(L, "%s\n  expected %d args, but got %d", "CreateThrustController(unit, label, thrustBone)", 3, top);
  Unit* u = ToObject<Unit>(L, 1);
  luaL_checkstring(L, 2);  // the label (+0x88): kept by the exe, read by nothing
  if (!u || !u->skeleton) return luaL_error(L, "Unit has no skeleton.");  // (0x64ac7a: the unit's actor, +0x540)
  auto m = std::make_unique<ThrustManipulator>();
  m->unit = u;
  m->bone = ResolveBoneArg(L, u, 3);  // (CAniActor::ResolveBoneIndex 0x63ad40; the ctor's AddWatchBone 0x63b6d0)
  // the ctor 0x64a683..0x64a71d: the bone's forward axis from its local rotation in the pose as it is now (+0x20 of the
  // pose bone), and the first turn: the arc from that axis to (0, 1, 0) (0x50cb50)
  if (m->bone >= 0) {
    const size_t b = static_cast<size_t>(m->bone);
    const Quat& q = u->poseQ.size() == u->skeleton->bones().size() ? u->poseQ[b] : u->skeleton->bones()[b].localRot;
    m->fwd = {(q.w * q.y + q.z * q.x) * 2.0f, (q.z * q.y - q.w * q.x) * 2.0f, 1.0f - (q.y * q.y + q.x * q.x) * 2.0f};
  }
  m->cur = ThrustArcExe(m->fwd, Vec3{0.0f, 1.0f, 0.0f});
  m->seq = S(L)->NextManipSeq();  // (IAniManipulator's ctor 0x63b640, precedence 0: behind the others of precedence 0)
  ThrustManipulator* raw = m.get();
  u->thrusters.push_back(raw);
  CreateObject(L, raw, "CThrustManipulator");
  S(L)->Own(std::move(m));
  return 1;
}
ThrustManipulator* CheckThrust(lua_State* L) { return CheckObject<ThrustManipulator>(L, 1); }
// ThrustManipulator:SetThrustingParam(xCapMin, xCapMax, yCapMin, yCapMax, zCapMin, zCapMax, turnForceMult, turnSpeed)
// 0x64ad90: +0xac..+0xb4 the three minima, +0xb8..+0xc0 the maxima, +0xc4, +0xc8; returns nothing (0x64b098), so
// a chained CreateThrustController(...):SetThrustingParam(...) hands its caller nil.
int l_thrust_SetThrustingParam(lua_State* L) {
  const int top = lua_gettop(L);
  if (top != 9)
    return luaL_error(L, "%s\n  expected %d args, but got %d",
                      "ThrustManipulator:SetThrustingParam(xCapMin, xCapMax, yCapMin, yCapMax, zCapMin, zCapMax, "
                      "turnForceMult, turnSpeed)",
                      9, top);
  ThrustManipulator* m = CheckThrust(L);
  float a[8];
  for (int i = 0; i < 8; ++i) a[i] = static_cast<float>(luaL_checknumber(L, i + 2));
  m->capMin = {a[0], a[2], a[4]};
  m->capMax = {a[1], a[3], a[5]};
  m->turnMult = a[6];
  m->turnSpeed = a[7];
  return 0;
}
int l_thrust_SetPrecedence(lua_State* L) {  // cfunc_IAniManipulatorSetPrecedenceL 0x63bb40
  ThrustManipulator* m = CheckThrust(L);
  m->precedence = static_cast<int>(luaL_checknumber(L, 2));
  m->seq = S(L)->NextManipSeq();
  lua_settop(L, 1);
  return 1;
}
int l_thrust_Enable(lua_State* L) {
  CheckThrust(L)->enabled = true;
  lua_settop(L, 1);
  return 1;
}
int l_thrust_Disable(lua_State* L) {
  CheckThrust(L)->enabled = false;
  lua_settop(L, 1);
  return 1;
}
int l_thrust_Destroy(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  if (auto* m = dynamic_cast<ThrustManipulator*>(o)) m->alive = false;
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
  SetMethod(L, "CRotateManipulator", "SetPrecedence", l_rot_SetPrecedence);
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
  SetGlobal(L, "CreateSlaver", l_CreateSlaver);
  SetMethod(L, "CSlaveManipulator", "SetMaxRate", l_slaver_SetMaxRate);
  SetMethod(L, "CSlaveManipulator", "SetPrecedence", l_slaver_SetPrecedence);
  SetMethod(L, "CSlaveManipulator", "Enable", l_slaver_Enable);
  SetMethod(L, "CSlaveManipulator", "Disable", l_slaver_Disable);
  SetMethod(L, "CSlaveManipulator", "Destroy", l_slaver_Destroy);
  SetGlobal(L, "CreateFootPlantController", l_CreateFootPlantController);
  SetMethod(L, "CFootPlantManipulator", "SetPrecedence", l_foot_SetPrecedence);
  SetMethod(L, "CFootPlantManipulator", "Enable", l_foot_Enable);
  SetMethod(L, "CFootPlantManipulator", "Disable", l_foot_Disable);
  SetMethod(L, "CFootPlantManipulator", "Destroy", l_foot_Destroy);
  SetGlobal(L, "CreateThrustController", l_CreateThrustController);
  SetMethod(L, "CThrustManipulator", "SetThrustingParam", l_thrust_SetThrustingParam);
  SetMethod(L, "CThrustManipulator", "SetPrecedence", l_thrust_SetPrecedence);
  SetMethod(L, "CThrustManipulator", "Enable", l_thrust_Enable);
  SetMethod(L, "CThrustManipulator", "Disable", l_thrust_Disable);
  SetMethod(L, "CThrustManipulator", "Destroy", l_thrust_Destroy);
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

// Entity::ProcessEntitiesShootingAtMe 0x67a190, at the start of the entity's own turn (Entity::TaskTick 0x679cc1):
// the shooters that are gone, dead or queued for destruction leave the set.
void PurgeShooters(Sim& sim, Entity* e) {
  if (e->shooters.empty()) return;
  e->shooters.erase(std::remove_if(e->shooters.begin(), e->shooters.end(),
                                   [&](uint32_t ref) {
                                     const Entity* s = sim.FindEntity(ref);
                                     return !s || s->dead || s->destroyQueued;
                                   }),
                    e->shooters.end());
}

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
  // this runs inside the motion pass: a target that already moved this beat still stands, for the original,
  // where the beat began (its transform is committed in step 13). Seen: a launcher's body starts turning after a
  // target that began to move one beat later than "its new place" would make it.
  // The aim point rides on the target's hull, so it is made from the transform the beat began with (position
  // and orientation), in one step as the original makes it: taking the point of now out of the hull's frame and
  // back in with the held one lands a float step beside it, and the hull's turn a millionth off with that.
  // (Only for a target WITHOUT a target point: a target bone comes out of the target's pose, Unit::GetTargetPoint
  // 0x6ab340 -> 0x6aa5c0, and the pose is this beat's once the target has had its turn. Seen: a destroyer's hull
  // following the turret of a turning battleship one beat late.)
  Vec3 tp = TargetPosBeatStart(sim, w->target);
  if (IsNaN(tp)) return false;
  if (const float aa = CB(u).attackAngle; aa > 0) {
    // AI.AttackAngle (ubp+0x4d0; 0x6c2350): the unit shows the target its side. The direction to the target is
    // turned about the up axis by the angle, away from the side the target is on, so the target ends up
    // AttackAngle degrees off the bow (a Battlecruiser's broadside: 60).
    Vec3 n = Norm(Vec3{tp.x - u->position.x, tp.y - u->position.y, tp.z - u->position.z});
    const Quat& q = u->orientation;
    const float fx = (q.w * q.y + q.z * q.x) * 2.0f, fz = 1.0f - (q.y * q.y + q.x * q.x) * 2.0f;
    const float ang = (n.z * fx - fz * n.x > 0.0f) ? aa * 0.017453292f : aa * -0.017453292f;
    // 0x6c2483: the turn is a quaternion about the up axis (0x570750: the half angle's cosine, the axis times its
    // sine) and the direction is turned by it through its matrix (0x452d40). A plain sine and cosine of the whole
    // angle land a float step beside that, which the rounding below turns into a millionth of a radian.
    const float hs = dmath::Sin(ang * 0.5f), hc = dmath::Cos(ang * 0.5f);
    // 0x6c24b5: what the motion is handed is a POINT, the unit's place + that unit vector, and TurnInfo 0x6990e0
    // takes the place off again. Beside a coordinate of a few hundred a unit vector keeps only its first five
    // digits, so the direction the hull turns to is up to 1e-5 rad off the exact one, and the 0.9999 test that
    // starts the next turn falls a beat earlier or later with it.
    const Vec3 r = RotateM(Quat{0.0f * hs, 1.0f * hs, 0.0f * hs, hc}, n);
    const Vec3 T{r.x + u->position.x, r.y + u->position.y, r.z + u->position.z};
    *dir = Vec3{T.x - u->position.x, T.y - u->position.y, T.z - u->position.z};
    static const long dbgAa = getenv("MOHO64_DEBUG_SLAVED") ? atol(getenv("MOHO64_DEBUG_SLAVED")) : -1;
    if (dbgAa >= 0 && static_cast<long>(u->id) == dbgAa) {
      const Entity* te = TargetEntity(sim, w->target);
      Logf(LogLevel::Info,
           "attack angle %u unit %u target %u bone %d held %d: point %.9g %.9g %.9g | n %.9g %.9g %.9g | side %.9g ang "
           "%.9g | r %.9g %.9g %.9g | dir %.9g %.9g %.9g | q %.9g %.9g",
           sim.tick(), u->id, te ? te->id : 0, w->target.aimBone, te && te->shapeHeld ? 1 : 0, tp.x, tp.y, tp.z, n.x,
           n.y, n.z, n.z * fx - fz * n.x, ang, r.x, r.y, r.z, dir->x, dir->y, dir->z, q.y, q.w);
    }
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
