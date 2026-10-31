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
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/backtrace.h"
#include "core/dmath.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "sim/transport.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/enum_arg.h"
#include "sim/combat.h"
#include "sim/entity_grid.h"
#include "sim/vecmath.h"
#include "sim/intel.h"
#include "sim/landnav.h"
#include "sim/luautil.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/air.h"
#include "sim/ai_platoon.h"
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

const char* const kLayerOfBit[] = {"Land", "Seabed", "Sub", "Water", "Air", "Orbit"};
uint8_t LayerBit(const char* name) {
  for (int i = 0; i < 6; ++i)
    if (!std::strcmp(name, kLayerOfBit[i])) return static_cast<uint8_t>(1u << i);
  return 0;
}
const char* LayerName(uint8_t bit) {
  for (int i = 0; i < 6; ++i)
    if (bit == (1u << i)) return kLayerOfBit[i];
  return "Land";
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

// 0x6d1e30 + Wm3 FromRotationMatrix 0x4f0cb0 (sim_random.md 7): M = Ry(h) Rx(p) Rz(r) in float, then to a
// quaternion with the original's sign (w > 0 when the trace is positive).
Quat HprQuat(float h, float pi, float r) {
  float ch = dmath::Cos(h), sh = dmath::Sin(h), cp = dmath::Cos(pi), sp = dmath::Sin(pi), cr = dmath::Cos(r),
        sr = dmath::Sin(r);
  float H[3][3] = {{ch, 0, sh}, {0, 1, 0}, {-sh, 0, ch}};
  float P[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
  float R[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
  auto mul = [](const float A[3][3], const float B[3][3], float O[3][3]) {
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        float acc = 0.0f;
        for (int k = 0; k < 3; ++k) acc = A[i][k] * B[k][j] + acc;
        O[i][j] = acc;
      }
  };
  float T[3][3], m[3][3];
  mul(P, R, T);
  mul(H, T, m);
  float q[3], w;
  float tr = (m[0][0] + m[1][1]) + m[2][2];
  if (tr > 0) {
    double rt = std::sqrt(static_cast<double>(tr + 1.0f));
    float rf = static_cast<float>(rt);
    w = static_cast<float>(rt * 0.5);
    float inv = 0.5f / rf;
    q[0] = (m[2][1] - m[1][2]) * inv;
    q[1] = (m[0][2] - m[2][0]) * inv;
    q[2] = (m[1][0] - m[0][1]) * inv;
  } else {
    int i = m[0][0] < m[1][1] ? 1 : 0;
    if (m[i][i] < m[2][2]) i = 2;
    int j = (i + 1) % 3, k = (j + 1) % 3;
    double rt = std::sqrt(static_cast<double>(((m[i][i] - m[j][j]) - m[k][k]) + 1.0f));
    float rf = static_cast<float>(rt);
    q[i] = static_cast<float>(rt * 0.5);
    float inv = 0.5f / rf;
    w = (m[k][j] - m[j][k]) * inv;
    q[j] = (m[j][i] + m[i][j]) * inv;
    q[k] = (m[k][i] + m[i][k]) * inv;
  }
  return Quat{q[0], q[1], q[2], w};
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
  // 0x6cf90a..0x6cf984: a quaternion of four zeros (the 16-byte memcmp 0x4f0b40 with the zero quaternion 0x10c7a58)
  // is reported and replaced by the identity
  if (vm::BitsZero(q)) {
    Logf(LogLevel::Warning, "Passed in an invalid quaternion when creating unit %s", bp->id.c_str());
    q = Quat{0.0f, 0.0f, 0.0f, 1.0f};
  }
  bool layer = lua_gettop(L) >= 10 && !lua_isnil(L, 10);  // an explicit layer keeps the given y (0x585ab0 +0x25)
  uint8_t want = layer && lua_isstring(L, 10) ? LayerBit(lua_tostring(L, 10)) : 0;
  return PushNew(L, S(L)->CreateUnit(L, *bp, a, p, q, true, nullptr, layer, want));
}

// CreateUnitHPR(bp, army, x, y, z, pitch, yaw, roll)
int l_CreateUnitHPR(lua_State* L) {
  const BlueprintInfo* bp = CheckBlueprint(L, 1);
  Army* a = CheckArmy(L, 2);
  Vec3 p{static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
         static_cast<float>(luaL_checknumber(L, 5))};
  Quat q = HprQuat(static_cast<float>(luaL_optnumber(L, 7, 0)), static_cast<float>(luaL_optnumber(L, 6, 0)),
                   static_cast<float>(luaL_optnumber(L, 8, 0)));  // radians (yaw = arg 7)
  return PushNew(L, S(L)->CreateUnit(L, *bp, a, p, q, true));
}

// FlattenMapRect(x, z, sizeX, sizeZ, elevation) 0x74b120: the grid vertices [x, x+sx] x [z, z+sz] take
// the elevation (truncated to the height scale); the units standing there (Land / Seabed) take the new ground:
// structures (no motion) are warped to it, units with a motion snap to it in their next motion tick.
int l_FlattenMapRect(lua_State* L) {
  Sim& sim = *S(L);
  TerrainMap* m = sim.mutableMap();
  if (!m) return 0;
  int x = static_cast<int>(luaL_checknumber(L, 1)), z = static_cast<int>(luaL_checknumber(L, 2));
  int sx = static_cast<int>(luaL_checknumber(L, 3)), sz = static_cast<int>(luaL_checknumber(L, 4));
  float h = static_cast<float>(luaL_checknumber(L, 5));
  prof::SectionTimer sec(prof::kSecFlatHeights);  // (MOHO64_PROFILE_HITCH only: the parts' times, core/profile.h)
  // When no vertex takes another height (FAF's skirts flattened again over ground that is flat already: a structure
  // made again where it stood, an army handed over), what is made from the heights stays as it is: the cluster data
  // made before the change, the air tiers, the path grids' terrain caps and the data made ahead are left alone (speed
  // only). The units standing there are still warped or marked, as the original does whatever the heights. Seen:
  // Chris's game 2, 19665: 5094 of 5803 flattenings in the beat change nothing, 34 of their 45 ms.
  const bool changed = m->WouldChangeHeights(x, z, x + sx, z + sz, h);
  if (changed) {
    LandNavBeforeTerrain(sim, x, z, x + sx, z + sz);  // (cluster data the background paid for: from the old heights)
    for (int j = z; j <= std::min(z + sz, m->height()); ++j)
      for (int i = x; i <= std::min(x + sx, m->width()); ++i) m->SetHeightAt(i, j, h);
  }
  sec.To(prof::kSecFlatDrawn);
  m->NoteHeightChange(x, z, x + sx, z + sz);  // (for the drawn ground: render request 25)
  sec.To(prof::kSecFlatAir);
  if (changed) AirTerrainChanged(sim, m, x, z, x + sx, z + sz);
  sec.To(prof::kSecFlatPaths);
  // (the path grids keep each origin's terrain caps; the original's tests read them from the map as it is, so a
  // flattened rect changes passability at once. Seen: MP3 1985, the skirt of army 2's first land factory, flattened
  // vertex by vertex at 58, made cell (67, 450) too steep for a hover engineer; the original's path to (72, 451)
  // went round it, ours through it.)
  if (changed) {
    sim.navigation().TerrainChanged(x, z, x + sx, z + sz);
    LandNavTerrainChanged(sim, x, z, x + sx, z + sz);  // (cluster data made ahead from the old heights)
  }
  sec.To(prof::kSecFlatUnits);
  // A unit WITH a motion (u+0x4b0) found by the box below only gets motion+0x90 = 1 (0x74b350..0x74b35a): its next
  // motion tick works out its contacts (MotionTick 0x6b9e21) and CalcMoveLand / CalcMoveHover snap it to the ground
  // (0x6c31b7 / 0x6c2e9f) although it stands still. Seen (Economy 1804): FAF's FlattenGradientMapRect flattens an
  // Aeon land factory's skirt vertex by vertex (rows z 426..434); support commander 39 stands still at z 435.27,
  // 1.27 past the last row: inside the box of that row's flattenings (z 432..436) but more than 1 from them (the
  // old selection here: centre within 1 of the rect), and the original's re-snap lifts it 0.006 (its size
  // box's back corners stand in the cell the row 434 bounds). Before, motion+0x90 alone did not snap in ours (only
  // with a move or needSnap), which is why setting it here alone had changed Economy at 959.
  // A unit without a motion is Warped to where it stands (0x74b363: IUnit::GetTransform, then the entity's Warp,
  // Unit::Warp 0x6aa310: the layer from Entity::GetStartingLayer for the place, the height from CalcSpawnElevation,
  // SetCurrentLayer), when it is not dead nor being destroyed (IUnit vt+0x28 / +0x2c) and is on Land or the Seabed
  // (0x74b340). Which units: COGrid::CollectEntitiesInBox 0x721dc0 (the units' list, mask 0x100: grid cells, the
  // collision AABB, then the primitive against the box, touching counts) with a box centred on the rect grown by
  // one vertex each way (x - 1 not below 0, x + sx + 1 not past the last vertex) whose EXTENTS are that grown rect's
  // full width and depth, and 100 in y around y = 0 (0x74b203..0x74b2ed: Box3 extents are half sizes, so the box
  // is twice the rect). Seen: a UEF construction drone's guard dummy (no motion, AttachTo(drone, -1), Seabed since
  // CreateUnitHPR at 0,0,0) turns Land in the beat a shield generator is begun 0.67 / 0.1 beyond one corner of a
  // skirt vertex's grown rect (FAF's FlattenSkirt flattens vertex by vertex); a drone let go elsewhere keeps Seabed.
  {
    const int x0m = std::max(x - 1, 0), z0m = std::max(z - 1, 0);
    const int x1c = std::min(m->width(), x + sx + 1), z1c = std::min(m->height(), z + sz + 1);
    const Vec3 centre{static_cast<float>(x0m + x1c) * 0.5f, 0.0f, static_cast<float>(z0m + z1c) * 0.5f};
    const float half[3] = {static_cast<float>(x1c - x0m), 100.0f, static_cast<float>(z1c - z0m)};
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const Vec3 boxMn{centre.x - half[0], -half[1], centre.z - half[2]};
    const Vec3 boxMx{centre.x + half[0], half[1], centre.z + half[2]};
    std::vector<Entity*> cand;
    sim.entityGrid().GatherBox(boxMn.x, boxMn.z, boxMx.x, boxMx.z, 1, &cand);
    std::vector<Unit*> warp;
    for (Entity* e : cand) {
      if (e->kind != Entity::Kind::Unit) continue;
      Unit* u = static_cast<Unit*>(e);
      WorldShape ws;
      if (!GetWorldShape(u, &ws)) continue;
      Vec3 mn, mx;
      ShapeBounds(ws, &mn, &mx);
      if (boxMn.x > mx.x || mn.x > boxMx.x || boxMn.y > mx.y || mn.y > boxMx.y || boxMn.z > mx.z || mn.z > boxMx.z) continue;
      if (!ShapeOverlapsOBox(ws, centre, axes, half)) continue;
      warp.push_back(u);
    }
    static const bool dbg = getenv("MOHO64_DEBUG_FLATTEN") != nullptr;  // every unit the flattening warps or marks
    for (Unit* u : warp) {
      if (u->dead || u->destroyQueued || (u->layer != "Land" && u->layer != "Seabed")) continue;
      if (u->motion.bp && u->motion.bp->motionType != kMotionNone) {
        u->motion.surfaceNext = true;  // motion+0x90 = 1
        if (dbg)
          Logf(LogLevel::Info, "flatten %u rect %d %d %d %d: unit %u (%s) at %.4f %.4f: motion+0x90", sim.tick(), x, z, sx, sz,
               u->id, u->blueprint ? u->blueprint->id.c_str() : "?", u->position.x, u->position.z);
        continue;
      }
      const std::string oldLayer = u->layer;
      const float oldY = u->position.y;
      sim.WarpUnit(L, u, u->position, u->orientation);
      if (dbg)
        Logf(LogLevel::Info, "flatten %u rect %d %d %d %d: unit %u (%s) at %.4f %.4f: %s -> %s, y %.6f -> %.6f", sim.tick(), x, z,
             sx, sz, u->id, u->blueprint ? u->blueprint->id.c_str() : "?", u->position.x, u->position.z, oldLayer.c_str(),
             u->layer.c_str(), oldY, u->position.y);
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

// CreatePropHPR(bp, x, y, z, heading, pitch, roll) 0x6fc010: exactly 7 args, the angles in degrees.
int l_CreatePropHPR(lua_State* L) {
  if (lua_gettop(L) != 7) return luaL_error(L, "%s\n  expected %d args, but got %d", "CreatePropHPR", 7, lua_gettop(L));
  const BlueprintInfo* bp = CheckBlueprint(L, 1);
  auto num = [&](int i) {
    if (lua_type(L, i) != LUA_TNUMBER) luaL_typerror(L, i, "number");
    return lua_tonumber(L, i);
  };
  float z = static_cast<float>(num(4)), y = static_cast<float>(num(3)), x = static_cast<float>(num(2));
  const double d2r = static_cast<double>(0.0174532924f);
  float h = static_cast<float>(num(5) * d2r), pi = static_cast<float>(num(6) * d2r), r = static_cast<float>(num(7) * d2r);
  return PushNew(L, S(L)->CreateProp(L, *bp, Vec3{x, y, z}, HprQuat(h, pi, r), Vec3{1, 1, 1}));
}

// GenerateRandomOrientation() 0x75a600: four Gaussian draws, normalised (sim_random.md 6).
int l_GenerateRandomOrientation(lua_State* L) {
  if (lua_gettop(L) != 0) return luaL_error(L, "%s\n  expected %d args, but got %d", "GenerateRandomOrientation", 0, lua_gettop(L));
  Sim& sim = *S(L);
  float g1 = static_cast<float>(sim.Gauss()), g2 = static_cast<float>(sim.Gauss()), g3 = static_cast<float>(sim.Gauss());
  float g4 = static_cast<float>(sim.Gauss());
  // the x87 sum at PC_24 rounds each step to float (0x75a67b..0x75a696), the root 0x452fc0 (0x75a69b), 1e-6 test
  const float sum = ((g3 * g3 + g2 * g2) + g1 * g1) + g4 * g4;
  const float len = std::sqrt(sum);
  Quat q{0, 0, 0, 0};
  if (len > 9.99999997e-07f) {
    float inv = 1.0f / len;
    q = Quat{g1 * inv, g2 * inv, g3 * inv, g4 * inv};
  }
  PushVector(L, 0, 0, 0);  // {x, y, z, w} with the vector metatable, as GetOrientation
  const float v[4] = {q.x, q.y, q.z, q.w};
  for (int i = 0; i < 4; ++i) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// CreateProp(location, bp)
int l_CreateProp(lua_State* L) {
  Vec3 p = CheckVec(L, 1);
  const BlueprintInfo* bp = CheckBlueprint(L, 2);
  return PushNew(L, S(L)->CreateProp(L, *bp, p, Quat{}, Vec3{1, 1, 1}));
}

// Entity:CreatePropAtBone(bone, bp) 0x6fc610 (engine-ref prop_split.md): a prop at the bone's world
// position and orientation (OnCreate inside the call); its own scale; no army, no terrain snap.
int ResolveBone(lua_State* L, Entity* e, int arg);
int l_CreatePropAtBone(lua_State* L) {
  if (lua_gettop(L) != 3) return luaL_error(L, "%s\n  expected %d args, but got %d", "Entity:CreatePropAtBone", 3, lua_gettop(L));
  Entity* e = CheckObject<Entity>(L, 1);
  int bone = ResolveBone(L, e, 2);
  const char* id = luaL_checkstring(L, 3);
  const BlueprintInfo* bp = S(L)->blueprints().Find(id);
  if (!bp) return luaL_error(L, "Unable to create prop '%s'", id);
  Vec3 wp;
  Quat wq;
  BoneWorld(e, bone < 0 ? 0 : bone, &wp, &wq);
  Prop* p = S(L)->CreateProp(L, *bp, wp, wq, Vec3{1, 1, 1});
  // after OnCreate: warp so the new prop's own bone 0 sits on the bone's transform (W o L0^-1; tree meshes
  // have a rotated bone 0, so the split trees stand upright). In the exe's own arithmetic (prop_split.md 2):
  // L0 = the new prop's GetBoneLocalTransform(0) 0x679e20 = the inverse of its bone 0's rest-pose inverse
  // (entRot / entPos), the place times the prop's scale; Inverse 0x46fbf0 (conjugate, the negated place turned by
  // it, 0x452d40); then Compose 0x549c20 (W o L0^-1). W itself is the group's bone by the same reading
  // (BoneWorld, a prop: entity transform x GetBoneLocalTransform, one composition). Seen: Skirmish 1522, ours
  // made the split tree 804267725 stand exactly upright, and the ACU's box test met it on a face (depth 0.0088;
  // the edge axis tree-y x unit-z is then the unit's x axis, a tie the face wins). In the exe's digits the
  // tree leans by about 1e-7 (q.x -8.9e-8, q.z 3e-8) and is not knocked over, as in the original (oracle
  // TREEDBG run); ours fell one tree more (the first differing draw, a FallThread's Random at 1523). Other
  // split trees stood one float step beside the original's (now the same, to the bit, in that run).
  if (p && !p->destroyQueued && p->skeleton && p->skeleton->Count() > 0) {
    const Bone& b0 = p->skeleton->bones()[0];
    const Vec3 l0p{b0.entPos.x * (p->meshScale * p->scale[0]), (p->meshScale * p->scale[1]) * b0.entPos.y,
                   (p->meshScale * p->scale[2]) * b0.entPos.z};
    const Quat iq{-0.0f - b0.entRot.x, -0.0f - b0.entRot.y, -0.0f - b0.entRot.z, b0.entRot.w};
    const Vec3 ip = vm::RotateM(iq, Vec3{-0.0f - l0p.x, -0.0f - l0p.y, -0.0f - l0p.z});
    const Vec3 r = vm::RotateM(wq, ip);
    p->position = Vec3{wp.x + r.x, wp.y + r.y, wp.z + r.z};
    p->orientation = vm::ComposeQ(wq, iq);
    GridUpdate(*S(L), p, false);
  }
  return PushNew(L, p);
}

// CAiBrain:GetMapWaterRatio 0x58feb0 -> 0x62d7e0 (engine-ref map_water_ratio.md): every 8th heightfield vertex
// (edges excluded) below the water level, recomputed per call.
int l_brain_GetMapWaterRatio(lua_State* L) {
  const TerrainMap* map = S(L)->map();
  float water = 0.0f, total = 0.0f;
  if (map) {
    const int nx = static_cast<int>(static_cast<unsigned>(map->width()) >> 3) - 1;
    const int ny = static_cast<int>(static_cast<unsigned>(map->height()) >> 3) - 1;
    const float level = map->hasWater ? map->waterElevation : -10000.0f;
    if (nx > 1)
      for (int i = 1; i < nx; ++i)
        for (int j = 1; j < ny; ++j) {
          if (map->HeightAt(8 * i, 8 * j) < level) water += 1.0f;
          total += 1.0f;
        }
  }
  lua_pushnumber(L, water / total);
  return 1;
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
  S(L)->GenerateArmyStart(a);  // two draws (sim_random.md 4.2)
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

}  // namespace

Entity::~Entity() { ReleaseScriptPosition(); }

void Entity::ReleaseScriptPosition() {
  if (positionRef != LUA_NOREF && positionState) luaL_unref(positionState, LUA_REGISTRYINDEX, positionRef);
  positionRef = LUA_NOREF;
}

// CTextureScroller::Tick 0x777730 (switch on the type, 0x777746; type 0 and anything above 3 do nothing).
void TextureScrollerTick(Entity* e, const Vec3& cp, const Quat& cq, const Vec3& pp, const Quat& pq) {
  TextureScroller* s = e->scroller.get();
  if (!s) return;
  switch (s->type) {
    case TextureScroller::kPingPong: {  // 0x77774d
      // per offset: count down; at 0 flip between ping and pong and hold the new value floor(seconds x 10) beats
      // (x 10.0f 0xdff31c, then frndint corrected down: 0x777791..0x7777a4)
      bool flipped = false;
      for (int i = 0; i < 2; ++i) {
        if (--s->beatsLeft[i] > 0) continue;
        s->ping[i] = !s->ping[i];
        flipped = true;
        const float beats = (s->ping[i] ? s->f[2 * i] : s->f[2 * i + 1]) * 10.0f;
        s->beatsLeft[i] = static_cast<int32_t>(std::floor(beats));
      }
      if (!flipped) return;
      // either flipped: both offsets jump to their values, before = after (no blending, 0x77781c..0x777834)
      const float a = s->ping[0] ? s->f[4] : s->f[6];
      const float b = s->ping[1] ? s->f[5] : s->f[7];
      e->scrollPrev[0] = e->scroll[0] = a;
      e->scrollPrev[1] = e->scroll[1] = b;
      return;
    }
    case TextureScroller::kManual:  // 0x777844: a constant amount per beat
      e->scrollPrev[0] = e->scroll[0];
      e->scrollPrev[1] = e->scroll[1];
      e->scroll[0] = e->scroll[0] + s->f[0];
      e->scroll[1] = s->f[1] + e->scroll[1];
      return;
    case TextureScroller::kThread: {  // 0x77788d: treads
      // The entity's committed transform (+0x9c: quaternion w, x, y, z, then the position +0xac) against the one
      // before it (+0xb8, +0xc8): nothing at all when the position did not change (0x4f0a50, a 12-byte memcmp),
      // so neither offset is touched while a unit stands, nor while it turns without its position changing.
      if (std::memcmp(&cp, &pp, sizeof(Vec3)) == 0) return;
      // Each transform's x axis (the quaternion's matrix, first column) and the point sideDist out along it on
      // either side: how far each point moved along the mean of the two transforms' z axes (forward), times
      // scrollMult, is added to its offset. Offset 0 is the +x side's, offset 1 the -x side's. In the exe's
      // order of operations (0x7778cb..0x777d00).
      const float two = 2.0f, sd = s->sideDist;
      const float bXx = 1.0f - (pq.z * pq.z + pq.y * pq.y) * two;
      const float bXy = (pq.z * pq.w + pq.y * pq.x) * two;
      const float bXz = (pq.z * pq.x - pq.y * pq.w) * two;
      const float aXx = 1.0f - (cq.z * cq.z + cq.y * cq.y) * two;
      const float aXy = (cq.y * cq.x + cq.z * cq.w) * two;
      const float aXz = (cq.z * cq.x - cq.y * cq.w) * two;
      const float dpx = (cp.x + aXx * sd) - (pp.x + bXx * sd);
      const float dpy = (cp.y + aXy * sd) - (pp.y + bXy * sd);
      const float dpz = (cp.z + aXz * sd) - (pp.z + bXz * sd);
      const float dmx = (cp.x - aXx * sd) - (pp.x - bXx * sd);
      const float dmy = (cp.y - aXy * sd) - (pp.y - bXy * sd);
      const float dmz = (cp.z - aXz * sd) - (pp.z - bXz * sd);
      const float zx = ((cq.y * cq.w + cq.z * cq.x) * two + (pq.y * pq.w + pq.z * pq.x) * two) * 0.5f;
      const float zy = ((cq.z * cq.y - cq.x * cq.w) * two + (pq.z * pq.y - pq.w * pq.x) * two) * 0.5f;
      const float zz = ((1.0f - (cq.x * cq.x + cq.y * cq.y) * two) + (1.0f - (pq.x * pq.x + pq.y * pq.y) * two)) * 0.5f;
      const float up = ((zz * dpz + zy * dpy) + zx * dpx) * s->scrollMult;
      const float um = ((zx * dmx + zz * dmz) + zy * dmy) * s->scrollMult;
      e->scrollPrev[0] = e->scroll[0];
      e->scrollPrev[1] = e->scroll[1];
      e->scroll[0] = e->scroll[0] + up;
      e->scroll[1] = e->scroll[1] + um;
      return;
    }
    default:
      return;
  }
}

namespace {

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
// The transform a script is told (entity+0x9c.. / +0xac..): the COMMITTED one. An entity that has taken its step in
// this beat still stands, for a script that asks before the beat's end, where the beat began; nearly all scripts
// run before the moves, so it only shows in callbacks of the motion and projectile passes (an OnImpact, a
// collision check). Seen: a projectile's impact script asks a walking commander's position; FAF's carpet bombs
// then aim at that very table (GetPosition's one table per entity) for the rest of the salvo, a beat's walk off.
static const Vec3& ScriptPos(const Entity* e) { return e->shapeHeld ? e->shapePos : e->position; }
static const Quat& ScriptOri(const Entity* e) { return e->shapeHeld ? e->shapeOri : e->orientation; }

int l_GetPosition(lua_State* L) {  // (bone?) the entity's or a bone's world position
  // exe 0x68fc90: a destroyed entity (or a non-entity) gives (0, 0, 0), no error (probe v9).
  Entity* e = ToObject<Entity>(L, 1);
  if (!e) {
    PushVec(L, Vec3{0, 0, 0});
    return 1;
  }
  if (lua_gettop(L) > 1) {  // with a bone: a new vector every call
    PushVec(L, lua_isnoneornil(L, 2) ? ScriptPos(e) : BonePosition(e, ResolveBone(L, e, 2)));
    return 1;
  }
  // Without a bone the entity keeps ONE Lua vector (a LuaObject at entity+0x258): the first call makes it, every
  // later call writes x, y, z into that same table and returns it. So every position a script got from this
  // entity is the same table and follows the entity (and the script's own edits): FAF's CreateAdjacencyBeams
  // shifts its "hub" copies and then compares the "original" ones, which are the same tables.
  const Vec3& sp = ScriptPos(e);
  if (e->positionRef == LUA_NOREF) {
    PushVec(L, sp);
    lua_pushvalue(L, -1);
    e->positionRef = luaL_ref(L, LUA_REGISTRYINDEX);
    e->positionState = ScriptState::From(L)->L();
    return 1;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, e->positionRef);
  lua_gpg_setnum3(L, -1, sp.x, sp.y, sp.z);  // (t[1], t[2], t[3] as three pushnumber + rawseti: the same stores)
  return 1;
}
int l_GetPositionXYZ(lua_State* L) {
  Entity* e = E(L);
  Vec3 p = lua_isnoneornil(L, 2) ? ScriptPos(e) : BonePosition(e, ResolveBone(L, e, 2));
  lua_pushnumber(L, p.x);
  lua_pushnumber(L, p.y);
  lua_pushnumber(L, p.z);
  return 3;
}
int l_GetOrientation(lua_State* L) {
  const Quat& q = ScriptOri(E(L));
  PushVector(L, 0, 0, 0);  // quaternion = {x,y,z,w} with the vector metatable
  float v[4] = {q.x, q.y, q.z, q.w};
  for (int i = 0; i < 4; ++i) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
int l_GetHeading(lua_State* L) {
  lua_pushnumber(L, Heading(ScriptOri(E(L))));
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
  if (e->kind == Entity::Kind::Unit) {  // the motion's facing follows (it re-derives the orientation when placed)
    Unit* u = static_cast<Unit*>(e);
    const Quat& q = e->orientation;
    float h = dmath::Atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x));
    u->motion.fx = u->motion.bx = dmath::Sin(h);
    u->motion.fz = u->motion.bz = dmath::Cos(h);
  }
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
  Entity* e = E(L);
  const float beforeMax = e->maxHealth;
  e->maxHealth = static_cast<float>(luaL_checknumber(L, 2));
  if (Sim* sim = S(L); sim && sim->collectHealth && e->maxHealth != beforeMax)  // (a host's list; reads only)
    RecordHealth(*sim, e, e->health, beforeMax);
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
  static const bool dbgDestroy = getenv("MOHO64_DEBUG_DESTROY") != nullptr;  // (read once)
  if (dbgDestroy) {
    Entity* de = ToObject<Entity>(L, 1);
    lua_Debug ar;
    std::string tb;
    for (int lv = 1; lv < 12 && lua_getstack(L, lv, &ar); ++lv)
      if (lua_getinfo(L, "Sl", &ar)) tb += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
    Logf(LogLevel::Info, "destroydbg %u id %u%s", S(L)->tick(), de ? de->id : 0, tb.c_str());
  }
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
// GetReclaimablesInRect(rect) / (x0, z0, x1, z1) (0x75b280): every unit and prop (trees, rocks, wrecks)
// registered in the entity-grid cells the rect touches, unfiltered, in grid order; nil when none
// (rect_queries.md).
int l_GetReclaimablesInRect(lua_State* L) {
  float r[4];
  if (lua_istable(L, 1)) {
    const char* k[4] = {"x0", "y0", "x1", "y1"};
    for (int i = 0; i < 4; ++i) {
      lua_pushstring(L, k[i]);
      lua_gettable(L, 1);
      r[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
  } else {
    for (int i = 0; i < 4; ++i) r[i] = static_cast<float>(luaL_checknumber(L, i + 1));
  }
  std::vector<Entity*> found;
  S(L)->entityGrid().Gather(r[0], r[1], r[2], r[3], 3, &found);
  if (found.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtablesized(L, static_cast<int>(found.size()), 0);
  int n = 0;
  for (Entity* e : found) {
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
  // (0x690fa9: the three numbers go straight into the entity's scale, the size it is drawn at)
  if (e->kind == Entity::Kind::Projectile)
    static_cast<Projectile*>(e)->drawSize = {e->scale[0], e->scale[1], e->scale[2]};
  return 0;
}
// Entity:SetMesh(meshBlueprint, keepActor), Entity:SetDrawScale(scale), Entity:SetVizToFocusPlayer / Allies /
// Enemies / Neutrals('Always' | 'Never' | 'Intel'): what the entity is drawn as, how large, and for whom. Kept
// for a host that draws the game (EntityViews, PropEvents, UnitView::mesh); the simulation does not go by them
// (an entity's bones stay those of its blueprint's mesh).
// Entity:RequestRefreshUI() 0x68bb60: sets entity+0x9a for the beat's sync (entity.h refreshUI; ui REQUESTS 47)
int l_RequestRefreshUI(lua_State* L) {
  S(L)->RequestRefreshUI(CheckObject<Entity>(L, 1));
  return 0;
}
int l_SetMesh(lua_State* L) {
  Entity* e = E(L);
  e->mesh = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}
int l_SetDrawScale(lua_State* L) {
  Entity* e = E(L);
  e->drawScale = static_cast<float>(luaL_checknumber(L, 2));
  // (0x690b35: the same three fields as SetScale, all set to the one number)
  if (e->kind == Entity::Kind::Projectile)
    static_cast<Projectile*>(e)->drawSize = {e->drawScale, e->drawScale, e->drawScale};
  return 0;
}
template <int Which>
int l_SetVizTo(lua_State* L) {
  Entity* e = E(L);
  // EVisibilityMode by name (sim/enum_arg.h, SCR_GetEnum 0x4d2550: 0x68d9d3 ..; VIZMODE_, Always 2, Never 1,
  // Intel 4) in our view's numbers (1 'Always', 2 'Never', 3 'Intel'); another value (a number, '|') as 0
  const int v = EnumArgAt(L, 2, kEnumVizMode);
  e->viz[Which] = v == 2 ? 1 : v == 1 ? 2 : v == 4 ? 3 : 0;
  return 0;
}
// ---- texture scrollers (entity.h TextureScroller) ---------------------------------------------
// Entity:AddThreadScroller(sideDist, scrollMult) 0x6912e0, AddPingPongScroller(ping1, pingSpeed1, pong1,
// pongSpeed1, ping2, pingSpeed2, pong2, pongSpeed2) 0x691500, AddManualScroller(scrollSpeed1, scrollSpeed2)
// 0x6910b0, RemoveScroller() 0x6918c0. Each tests its count of arguments first ("%s\n  expected %d args, but got
// %d", 0xe0a220, with its usage line, 0xe28990 / 0xe289d8 / 0xe28944 / 0xe28a54), then the entity, then each
// number in order (lua_type must be number: TypeError "number", 0xe09a50), makes the entity's scroller if it
// has none (0x691436: new 0x3c bytes, 0x676ba0) and hands it the setup (SetScroller 0x777690). Each returns 1
// result (the last argument). FAF calls AddThreadScroller / RemoveScroller from TreadComponent
// (defaultcomponents.lua, when a unit with Display.MovementEffects.Land.Treads.ScrollTreads starts / stops
// moving) and AddPingPongScroller from AirUnit (Display.PingPongScroller).
void ScrollerArgs(lua_State* L, const char* usage, int n) {
  const int got = lua_gettop(L);
  if (got != n) luaL_error(L, "%s\n  expected %d args, but got %d", usage, n, got);
}
float ScrollerNum(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "number");
  return static_cast<float>(lua_tonumber(L, idx));
}
// CTextureScroller::SetScroller 0x777690: the setup (type, the eight numbers, sideDist, scrollMult) is copied
// over the old one. RemoveScroller's (type 0) puts the offsets back where they were before the last step
// (+0x100 := +0xf8, +0x104 := +0xfc); a ping-pong's two states are zeroed (it flips at its first step). The
// offsets themselves are kept: a new thread scroller goes on from where the last one left them.
void SetScroller(Entity* e, const TextureScroller& s) {
  if (!e->scroller) e->scroller = std::make_unique<TextureScroller>();
  TextureScroller& t = *e->scroller;
  t.type = s.type;
  for (int i = 0; i < 8; ++i) t.f[i] = s.f[i];
  t.sideDist = s.sideDist;
  t.scrollMult = s.scrollMult;
  if (t.type == TextureScroller::kNone) {
    e->scroll[0] = e->scrollPrev[0];
    e->scroll[1] = e->scrollPrev[1];
  } else if (t.type == TextureScroller::kPingPong) {
    t.ping[0] = t.ping[1] = false;
    t.beatsLeft[0] = t.beatsLeft[1] = 0;
  }
}
int l_AddThreadScroller(lua_State* L) {
  ScrollerArgs(L, "Entity:AddThreadScroller(sideDist, scrollMult)", 3);
  Entity* e = E(L);
  TextureScroller s;
  s.type = TextureScroller::kThread;
  s.sideDist = ScrollerNum(L, 2);
  s.scrollMult = ScrollerNum(L, 3);
  SetScroller(e, s);
  return 1;
}
int l_AddPingPongScroller(lua_State* L) {
  ScrollerArgs(L,
               "Entity:AddPingPongScroller(ping1, pingSpeed1, pong1, pongSpeed1, ping2, pingSpeed2, pong2, "
               "pongSpeed2)",
               9);
  Entity* e = E(L);
  TextureScroller s;
  s.type = TextureScroller::kPingPong;
  // (the arguments' places in the setup, 0x691606..0x6917cd)
  static const int kSlot[8] = {4, 0, 6, 1, 5, 2, 7, 3};
  for (int i = 0; i < 8; ++i) s.f[kSlot[i]] = ScrollerNum(L, 2 + i);
  SetScroller(e, s);
  return 1;
}
int l_AddManualScroller(lua_State* L) {
  ScrollerArgs(L, "Entity:AddManualScroller(scrollSpeed1, scrollSpeed2)", 3);
  Entity* e = E(L);
  TextureScroller s;
  s.type = TextureScroller::kManual;
  s.f[0] = ScrollerNum(L, 2);
  s.f[1] = ScrollerNum(L, 3);
  SetScroller(e, s);
  return 1;
}
int l_RemoveScroller(lua_State* L) {
  ScrollerArgs(L, "Entity:RemoveScroller()", 1);
  SetScroller(E(L), TextureScroller{});
  return 1;
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
// The weapons are the attacker's (unit+0x544). Unit::KillCleanup 0x6a8790, the beat after a kill, deletes the attacker
// and sets +0x544 to null (0x6a87d4..0x6a87ea); ours keeps the weapon records and marks the unit combatGone
// (combat.cpp, DestroyWeapons). GetWeaponCount 0x6c4a70 then gives 0 (0x6c4af8..0x6c4b0b: no attacker -> 0, else the
// attacker's count, vt+0x18), and GetWeapon 0x6c4880 raises "Invalid weapon index %d; must be between 1 and %d
// (inclusive)" (0xe2af74, 0x6c4968..0x6c498c) for any index past the count, so a dead unit has no weapons for Lua.
// Seen: MP1 8715 (after its compare stops at 8391), a player's WeaponPriorities callback (lua/WeaponPriorities.lua)
// names an urs0203 that is dead in ours (alive in the original's recording); ours gave its 3 weapon slots and
// GetWeapon(1) a nil (the Lua object had gone with DestroyWeapons), the callback stopped at
// weapon:SetTargetingPriorities and the ships after it kept their old priorities. With the attacker gone the count is
// 0, the script's "weaponCount > 0" skips the dead unit and the rest are set. The same for
// ScenarioFramework.GetAllowedOffMapTime (wep:GetCurrentTarget on a dead unit's weapon, MP1 10201).
int UnitWeaponCount(const Unit* u) { return u->combatGone ? 0 : static_cast<int>(u->weapons.size()); }
int l_GetWeaponCount(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(UnitWeaponCount(U(L))));
  return 1;
}
int l_GetWeapon(lua_State* L) {
  Unit* u = U(L);
  int i = static_cast<int>(luaL_checknumber(L, 2));
  const int n = UnitWeaponCount(u);
  if (i < 1 || i > n) return luaL_error(L, "Invalid weapon index %d; must be between 1 and %d (inclusive)", i, n);
  return PushNew(L, u->weapons[i - 1]);
}
// FAF's Unit.lua sets a unit state "Sacrificing" in OnStartSacrifice and OnStopSacrifice. The original has no such
// state; its SetUnitState does nothing for the name (no error: see below), so the two callbacks run on (deviation 1
// in docs/deviations.md, withdrawn). EUnitState by name (sim/enum_arg.h: no case, "UNITSTATE_", numbers, '|'). IsUnitState 0x6c5980 goes through
// SCR_GetEnum (0x6c5a53): a name the parse does not take raises "Invalid enum value". SetUnitState 0x6c6d30 calls
// the parse itself (0x6c6e02) and does NOTHING for such a name (0x6c6e06 -> 0x6c6e64: no error); the state's bit
// is 1 << value (0x6c6e2a, u+0x4a0). Ours keeps the states by their names: a value without a name (a number or
// names or-ed) is no state here.
// The state a name asks for, as EnumNameOf(kEnumUnitState, EnumValue(name)) gives it, as an index into
// kUnitStateNames (-1: none). The parse (a scan of 45 names without case) is remembered per name: a small table by
// the string's address, each entry checked against its own copy of the text (so a string that was collected and
// whose address now holds another one is parsed again). A name the parse refuses is never remembered: EnumValue
// raises its error as before. Per thread (two sims of one process run side by side).
int UnitStateIndex(lua_State* L, const char* name) {
  struct Entry {
    const char* at = nullptr;
    std::string text;
    int index = -1;
  };
  static thread_local Entry cache[64];
  Entry& c = cache[(reinterpret_cast<uintptr_t>(name) >> 4) & 63];
  if (c.at == name && c.text == name) return c.index;
  const char* state = EnumNameOf(kEnumUnitState, EnumValue(L, kEnumUnitState, name));
  int index = -1;
  for (int i = 0; state && i < kEnumUnitState.count; ++i)
    if (kEnumUnitState.names[i].name == state) {
      index = i;
      break;
    }
  c.at = name;
  c.text = name;
  c.index = index;
  return index;
}
// The names as std::strings, made once: unitStates.count(name) without a temporary string per call.
const std::string& UnitStateString(int index) {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (int i = 0; i < kEnumUnitState.count; ++i) v.emplace_back(kEnumUnitState.names[i].name);
    return v;
  }();
  return names[static_cast<size_t>(index)];
}
int l_IsUnitState(lua_State* L) {
  const char* name = luaL_checkstring(L, 2);
  const int index = UnitStateIndex(L, name);  // (before U(L): a bad name's error comes first, as before)
  lua_pushboolean(L, index >= 0 && U(L)->unitStates.count(UnitStateString(index)) > 0);
  return 1;
}
int l_SetUnitState(lua_State* L) {
  Unit* u = U(L);
  const char* name = luaL_checkstring(L, 2);
  int v = 0;
  if (!EnumLexical(kEnumUnitState, name, &v)) return 0;
  const char* state = EnumNameOf(kEnumUnitState, v);
  if (!state) return 0;
  if (lua_toboolean(L, 3)) u->unitStates.insert(state);
  else u->unitStates.erase(state);
  return 0;
}
int l_IsBeingBuilt(lua_State* L) {
  lua_pushboolean(L, U(L)->beingBuilt);
  return 1;
}

// Engine values that scripts set and read back: Set<X>(v) / Get<X>() pairs, kept per object.
int l_AttrSet(lua_State* L) {
  ScriptObject* o = CheckAnyObject(L, 1);
  const char* key = lua_tostring(L, lua_upvalueindex(1));
  SetObjectValue(L, o, key, 2);
  // (the shield's ratio is also kept where the plain unit view can read it without asking the script state)
  if (key && !strcmp(key, "ShieldRatio"))  // (the keys are the setters' names without "Set")
    if (Unit* u = ToObject<Unit>(L, 1)) {
      u->shieldRatio = static_cast<float>(lua_tonumber(L, 2));
      static const bool dbg = getenv("MOHO64_DEBUG_SHIELD") != nullptr;  // every call, with the unit and the value
      if (dbg)
        Logf(LogLevel::Info, "shield %u unit %u %s ratio %.4f", S(L)->tick(), u->id,
             u->blueprint ? u->blueprint->id.c_str() : "?", u->shieldRatio);
    }
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

// ---- Platoon: sim/ai_platoon.cpp ----------------------------------------------------------------

// ---- Brain (unit and platoon queries) -------------------------------------------------------

AiBrain* B(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

int l_brain_GetArmyStartPos(lua_State* L) {
  Army* a = B(L)->army;
  lua_pushnumber(L, a->startX);
  lua_pushnumber(L, a->startZ);
  return 2;
}
// GetListOfUnits(category, needToBeIdle[, requireBuilt]) (0x5878f0): 3 or 4 arguments (a boolean or nil each), but
// neither flag changes the answer. CArmyImpl::GetUnits 0x700eb0 (army vt+0x9c) fills an EntitySet (sorted by entity
// id, 0x582eb0) from the army's per-blueprint sets: a unit is added by the Unit ctor (AddUnitToCategorySet 0x700e20,
// so from the start of its construction) and taken off by ~Unit 0x6a6bf0 (0x6a6c3a, ConsumeUnitFromCategorySet
// 0x700e70). The binding then tests each unit (IsDead vt+0x28, DestroyQueued vt+0x2c, IsBeingBuilt vt+0x34 with
// requireBuilt, IsIdleState 0x6a7d60 with needToBeIdle) and adds the ones that pass with EntitySet::Add 0x57ddd0 into
// THE SAME set (0x587a7b: the set at esp+0x60), where they already are; the table is built from that set's whole
// size (0x587aaf) and contents (0x587aca). So every unit of the army in the category comes back, dead, destroy-queued
// and unfinished ones included, until the unit is deleted. Seen: Skirmish, M28's IdleFactoryMonitor (one factory
// per tick: its cycle is the list's length + 1) lists factories from the tick their construction starts in the
// oracle's MONDBG run (frac 0 at 49-53); ours started at their completion (251-255), and its cycle ran a tick
// apart from the original's in every army, which decided which factory built first at 1548.
int l_brain_GetListOfUnits(lua_State* L) {
  AiBrain* b = B(L);
  const uint64_t* cat = ToCategory(L, 2);
  lua_newtable(L);
  int n = 0;
  if (!b->army) return 1;
  for (Unit* u : b->army->units) {
    if (cat && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(cat, u->blueprint->entityIndex)))
      continue;
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
// GetCurrentUnits(category) (0x587760): not a list. It sums the army stat "Units_Active" (CArmyStats::GetStat
// 0x70b860) over the blueprints in the category (CArmyStatItem::SumCategory 0x70b580), truncated (cvttss2si). That
// stat counts a unit from its completion: the Unit ctor adds 1 for its blueprint when it is made complete (params
// +0x30, 0x6a6450; a construction adds to "Units_BeingBuilt" instead, 0x6a6741), Unit::Materialize 0x6a9f40 moves
// it over when its build reaches 1 (0x6aa0dd), and ~Unit 0x6a6bf0 takes it off (0x6a6e13): a dead unit counts
// until it is destroyed. Only blueprints with CapCost (bp+0x1f8) > 0 are counted (0x6a6621, 0x6aa0e5, 0x6a6e06).
// Seen: Skirmish 1119, Gibson's new air factory: M28 counts its air subteam's transports with GetCurrentUnits;
// Chloe's transport 0x600018 is at 14 % in its factory, the original counts 0 and builds a transport.
// The stat's values (Army::bpStats) are kept where the exe keeps them: Sim::CreateUnit, Materialize, the destroy
// queue's ~Unit step (sim.cpp).
int l_brain_GetCurrentUnits(lua_State* L) {
  AiBrain* b = B(L);
  const uint64_t* cat = ToCategory(L, 2);
  const float n = b->army ? b->army->SumBpStat("Units_Active", cat) : 0.0f;
  lua_pushnumber(L, static_cast<float>(static_cast<int>(n)));  // cvttss2si, cvtsi2ss (0x58783a)
  return 1;
}

}  // namespace

Vec3 EntityBonePosition(const Entity* e, int bone) { return BonePosition(e, bone); }

// ---- health ----------------------------------------------------------------------------------

void EntityAdjustHealth(lua_State* L, Entity* e, Entity* /*instigator*/, float amount) {
  if (amount == 0.0f) return;
  if (e->dead && amount > 0.0f) return;  // Entity::AdjustHealth 0x6798cf: nothing heals a dead entity (regeneration)
  float h = e->health + amount;
  if (e->maxHealth <= h) h = e->maxHealth;
  if (h < 0.0f) h = 0.0f;
  if (h == e->health) return;
  // Entity::SetHealth: OnHealthChanged(new, old) with both rounded down to quarters of the maximum
  float inv = 1.0f / e->maxHealth;
  float qNew = std::floor(inv * h * 4.0f) * 0.25f;
  float qOld = std::floor(inv * e->health * 4.0f) * 0.25f;
  const float before = e->health;
  e->health = h;
  // (Entity::SetHealth 0x679a3c..0x679a6b: the entity goes to the newest end of the dirty list, linked or not)
  if (e->kind == Entity::Kind::Unit)
    if (Sim* sim = S(L)) sim->MarkDirty(static_cast<Unit*>(e));
  if (Sim* sim = S(L); sim && sim->collectHealth) RecordHealth(*sim, e, before, e->maxHealth);  // (a host's list; reads only)
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

// (rawset of nil only on a key that is there: a new key would grow the table)
void DropObjectValues(lua_State* L, ScriptObject* obj) {
  lua_gpg_rawgetstr(L, LUA_REGISTRYINDEX, kValuesKey);
  if (lua_istable(L, -1)) {
    lua_pushlightuserdata(L, obj);
    lua_rawget(L, -2);
    const bool had = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (had) {
      lua_pushlightuserdata(L, obj);
      lua_pushnil(L);
      lua_rawset(L, -3);
    }
  }
  lua_pop(L, 1);
}

// The same for a host's view (sim/sim_view.cpp), which must make and check nothing in the state (lua_gpg_rawgetstr).
void PeekObjectValue(lua_State* L, ScriptObject* obj, const char* key) {
  lua_gpg_rawgetstr(L, LUA_REGISTRYINDEX, kValuesKey);
  if (!lua_istable(L, -1)) return;
  lua_pushlightuserdata(L, obj);
  lua_rawget(L, -2);
  lua_remove(L, -2);
  if (!lua_istable(L, -1)) return;
  lua_gpg_rawgetstr(L, -1, key);
  lua_remove(L, -2);
}

void RegisterEntityBindings(lua_State* L) {
  SetGlobal(L, "CreateUnit", l_CreateUnit);
  SetGlobal(L, "CreateUnitHPR", l_CreateUnitHPR);
  SetGlobal(L, "CreateInitialArmyUnit", l_CreateInitialArmyUnit);
  SetGlobal(L, "CreatePropHPR", l_CreatePropHPR);
  SetGlobal(L, "CreateProp", l_CreateProp);
  SetGlobal(L, "GenerateRandomOrientation", l_GenerateRandomOrientation);
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
  SetMethod(L, "Entity", "SetMesh", l_SetMesh);
  SetMethod(L, "Entity", "RequestRefreshUI", l_RequestRefreshUI);
  SetMethod(L, "Entity", "SetDrawScale", l_SetDrawScale);
  SetMethod(L, "Entity", "SetVizToFocusPlayer", l_SetVizTo<0>);
  SetMethod(L, "Entity", "SetVizToAllies", l_SetVizTo<1>);
  SetMethod(L, "Entity", "SetVizToEnemies", l_SetVizTo<2>);
  SetMethod(L, "Entity", "SetVizToNeutrals", l_SetVizTo<3>);
  SetMethod(L, "Entity", "AddThreadScroller", l_AddThreadScroller);
  SetMethod(L, "Entity", "AddPingPongScroller", l_AddPingPongScroller);
  SetMethod(L, "Entity", "AddManualScroller", l_AddManualScroller);
  SetMethod(L, "Entity", "RemoveScroller", l_RemoveScroller);
  SetMethod(L, "Entity", "GetBoneCount", l_GetBoneCount);
  SetMethod(L, "Entity", "CreatePropAtBone", l_CreatePropAtBone);
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


  SetMethod(L, "CAiBrain", "GetArmyStartPos", l_brain_GetArmyStartPos);
  SetMethod(L, "CAiBrain", "GetMapWaterRatio", l_brain_GetMapWaterRatio);
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
        // ARMOR_GetArmorDefinations 0x564660 walks the rows [1 .. GetCount] (0x564767) for the first whose [1] is the
        // armor type, then takes its [2 .. GetCount] (0x564803; both read again each pass) as strings. Ours reads every
        // row once: the first row of a name wins, a row that is no table and an entry that is no string are left out
        // (each a Lua error of the original's GetByIndex / GetString; the rows are armordefinition.lua's own).
        int defs = lua_gettop(L);
        const int rows = ObjectCount(L, defs);
        for (int i = 1; i <= rows; ++i) {
          lua_rawgeti(L, defs, i);
          if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
          }
          lua_rawgeti(L, -1, 1);
          std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
          lua_pop(L, 1);
          if (armorTypes_.count(name)) {
            lua_pop(L, 1);
            continue;
          }
          auto& mults = armorTypes_[name];
          const int entries = ObjectCount(L, -1);
          for (int k = 2; k <= entries; ++k) {
            lua_rawgeti(L, -1, k);
            if (lua_type(L, -1) == LUA_TSTRING) {
              std::string entry = lua_tostring(L, -1);  // "DamageType 0.5"
              size_t sp = entry.find(' ');
              if (sp != std::string::npos)
                mults[entry.substr(0, sp)] = static_cast<float>(std::atof(entry.c_str() + sp + 1));
            }
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
  // MOHO64_DEBUG_CALL=<method name>: every engine call of that script method, with the calling C++ frames
  static const char* dbgCall = getenv("MOHO64_DEBUG_CALL");
  if (dbgCall && strcmp(dbgCall, method) == 0) {
    void* fr[8];
    const int n = CaptureFrames(fr, 8);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
      s += b;
    }
    lua_Debug ar;
    for (int lv = 0; lv < 16 && lua_getstack(L, lv, &ar); ++lv)  // the Lua frames below, when called from a script
      if (lua_getinfo(L, "Sl", &ar)) s += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
    const Entity* e = dynamic_cast<const Entity*>(obj);
    Logf(LogLevel::Info, "call %u %s entity %u from%s", tick_, method, e ? e->id : 0u, s.c_str());
  }
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);   // handler below the args
  // stack: base+1 handler, args..., self, fn  ->  fn, self, args...
  lua_insert(L, base + 2);   // fn
  lua_insert(L, base + 3);   // self
  if (prof::On()) {  // MOHO64_PROFILE: the Lua timer's source for this callback (core/profile.h)
    int src = prof::kCallOther;
    if (const Entity* e = dynamic_cast<const Entity*>(obj))
      src = e->kind == Entity::Kind::Unit ? prof::kCallUnit : e->kind == Entity::Kind::Projectile ? prof::kCallProjectile : prof::kCallEntity;
    else if (dynamic_cast<const UnitWeapon*>(obj)) src = prof::kCallWeapon;
    else if (dynamic_cast<const AiBrain*>(obj)) src = prof::kCallBrain;
    prof::NextSource(src);
  }
  std::string hitchLabel;  // MOHO64_PROFILE_HITCH: the callback's name in the beat breakdown (core/profile.h)
  const bool hitchNested = prof::Hitch() && prof::Depth() > 0;
  const double hitchT0 = prof::Hitch() ? prof::NowMs() : 0;
  if (prof::Hitch()) {
    hitchLabel = method;
    if (const Entity* e = dynamic_cast<const Entity*>(obj); e && e->blueprint) hitchLabel += " " + e->blueprint->id;
    else if (const UnitWeapon* w = dynamic_cast<const UnitWeapon*>(obj); w && w->unit && w->unit->blueprint)
      hitchLabel += " (a weapon of " + w->unit->blueprint->id + ")";
    if (!hitchNested) prof::NextLabel("callback " + hitchLabel);
  }
  bool ok = lua_pcall(L, nargs + 1, 0, base + 1) == 0;
  if (hitchNested) prof::AddCall(hitchLabel, prof::NowMs() - hitchT0);
  if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, base);
  return ok;
}

namespace {
// The layer a unit starts on (Entity::GetStartingLayer 0x62ad30; `want` = the construction params' layer, 0 =
// choose). The footprint's caps decide, not the motion type: an EXPERIMENTAL aircraft starts on Land (it appears on
// the ground and takes off), an experimental submarine on the surface, a land unit under water on the Seabed.
// (Entity::GetFootprint 0x62ad51: the AltFootprint while the unit uses it, for a unit warped by its script)
uint8_t StartingLayer(Sim& sim, const BlueprintInfo& bp, const TerrainMap* map, const Vec3& p, uint8_t want,
                      bool alt = false) {
  const NamedFootprint& fp = alt ? bp.altFootprint : bp.footprint;
  // the cell's caps (OCCUPY_FootprintFits 0x7209e0 with caps -1: the terrain's answer for this footprint)
  int cx = static_cast<int16_t>(std::lrintf(p.x - static_cast<float>(fp.sizeX) * 0.5f));
  int cz = static_cast<int16_t>(std::lrintf(p.z - static_cast<float>(fp.sizeZ) * 0.5f));
  uint8_t caps = 0;
  if (fp.caps & 0xf) caps = sim.navigation().CellCaps(fp, cx, cz, true);  // (FootprintFits with caps -1, 0x62adca)
  if (map && cx >= 0 && cz >= 0 && cx + fp.sizeX <= map->width() && cz + fp.sizeZ <= map->height())
    caps |= fp.caps & 0x30;  // the terrain never takes the Air / Orbit bits away
  if (want & caps) return want;
  if (caps & kLayerAir) return BpInCategory(sim, &bp, "EXPERIMENTAL") ? kLayerLand : kLayerAir;
  float water = map && map->hasWater ? map->waterElevation : -10000.0f;
  if (map && water > map->TerrainHeight(p.x, p.z)) {
    if ((fp.caps & kLayerSub) && !BpInCategory(sim, &bp, "EXPERIMENTAL")) return kLayerSub;
    if (!(fp.caps & kLayerWater) && !BpInCategory(sim, &bp, "FERRYBEACON")) return kLayerSeabed;
    return kLayerWater;
  }
  return kLayerLand;
}
}  // namespace

Unit* Sim::CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete,
                      Unit* builder, bool keepY, uint8_t wantLayer, bool ownHeading) {
  prof::SectionTimer secAll(prof::kSecCreateUnit);  // (MOHO64_PROFILE_HITCH only: the parts' times, core/profile.h)
  prof::SectionTimer sec(prof::kSecCuHeading);
  lua_checkstack(L, 40);
  int top = lua_gettop(L);
  if (ownHeading) {
    // Unit ctor 0x6a5924, construction params +0x24 == 0: the given orientation is not used. A mobile unit
    // (MotionType not None, vf+0x10 0x51e460) whose blueprint says Display.SpawnRandomRotation (ubp+0x274) is
    // turned about the vertical by FRand(-pi/6, pi/6) (0x6a5969, quaternion by 0x570750); every other unit stands
    // unturned. The draw is made whether or not the unit is attached to its factory right after.
    q = Quat{};
    bps_.PushTable(L, bp);
    const int di = lu::Sub(L, lua_gettop(L), "Display");
    const bool spin = di && lu::Bool(L, di, "SpawnRandomRotation", false);
    lua_settop(L, top);
    if (spin && GetMotionBlueprint(L, bp, bps_).motionType != 0) {
      const float half = static_cast<float>(FRand(-0.523598790f, 0.523598790f)) * 0.5f;
      q = {0.0f, dmath::Sin(half), 0.0f, dmath::Cos(half)};
    }
  }
  auto owned = std::make_unique<Unit>();
  Unit* u = owned.get();
  u->kind = Entity::Kind::Unit;
  u->blueprint = &bp;
  u->army = army;
  sec.To(prof::kSecCuSkeleton);
  AttachSkeleton(L, u);
  sec.To(prof::kSecCuId);
  u->id = ReserveId(army, 0);  // EntityDB::DoReserveId: the lowest free serial of the army's unit pool
  u->position = pos;
  u->orientation = q;
  u->lastPosition = pos;
  u->motion.motionTarget = pos;  // (the CUnitMotion ctor 0x6b7b8f..0x6b7ba6: m+0x14 = the unit's place, vt+0x14)
  sec.To(prof::kSecCuShape);
  RevertCollisionShape(L, u);
  sec.To(-1);
  u->fractionComplete = complete ? 1.0f : 0.0f;
  u->beingBuilt = !complete;
  if (!complete) u->unitStates.Insert(US("BeingBuilt"));
  u->builderId = EntityRef(builder);
  u->lastMaterializeTick = tick_;
  sec.To(prof::kSecCuEconomy);
  UnitEconomyInit(L, u);
  sec.To(prof::kSecCuMotion);
  u->motion.bp = &GetMotionBlueprint(L, bp, bps_);
  sec.To(-1);
  u->motion.surfaceNext = true;
  static const bool dbgCreate = getenv("MOHO64_DEBUG_CREATE") != nullptr;  // (read once)
  if (dbgCreate)
    Logf(LogLevel::Info, "create %u unit %u bp %s pos %.2f %.2f motion %d", tick_, u->id, bp.id.c_str(), u->position.x, u->position.z, u->motion.bp->motionType);  // CUnitMotion ctor: m+0x90 = 1, so the first MotionTick processes contacts (0x6b7c9a)
  sec.To(prof::kSecCuPlace);
  const uint8_t startLayer = StartingLayer(*this, bp, map_.get(), pos, wantLayer);
  if (!keepY) {
    // (the construction params' +0x25 == 0, 0x6a5b73: also a factory's unit that cannot fly, see TickFactoryBuild;
    // a mobile build and an upgrade keep the height)
    // IUnit::CalcSpawnElevation 0x541540 (ids_repair_placement.md 3.2): the given y is ignored. Land / Seabed:
    // the terrain; Water: the water; Sub: the water + the unit's elevation; Air: flyers appear at their height
    const TerrainMap* m = map_.get();
    float water = m && m->hasWater ? m->waterElevation : -10000.0f;
    if (startLayer & (kLayerLand | kLayerSeabed)) {
      if (m) u->position.y = m->TerrainHeight(pos.x, pos.z);
    } else if (startLayer == kLayerWater) {
      u->position.y = water;
    } else if (startLayer == kLayerSub) {
      bps_.PushTable(L, bp);
      lua_pushstring(L, "Physics");
      lua_rawget(L, -2);
      u->position.y = water + FieldNumber(L, -1, "Elevation", 0);
      lua_pop(L, 2);
    } else if (u->motion.bp->motionType == kMotionAir) {
      u->position.y = AirSpawnHeight(*this, bp, L, pos.x, pos.z, pos.y);
    } else if (startLayer & (kLayerAir | 0x20)) {  // (Air / Orbit without a flying motion: AirLayerSpawnHeight)
      u->position.y = AirLayerSpawnHeight(*this, bp, L, pos.x, pos.z);
    }
    u->lastPosition = u->position;
  }
  // The entity grid takes the unit by its box WIDENED to its skirt from the start. The Entity ctor (0x677c90 ->
  // StandardInit 0x678370 -> RevertCollisionShape 0x67ae70) makes the primitive and registers its plain box at the
  // entity's initial transform, the origin (SSTIEntityVariableData 0x558760). The Unit ctor then sets the pending
  // transform to the unit's own (0x6a5bf9..0x6a5c51) and calls AdvanceCoords 0x678f10 twice (0x6a5c59, 0x6a5c5f):
  // the first sees the transform change (origin -> here, 0x67904b memcmp) and calls vt+0x84, Unit::UpdateCollision
  // 0x6ab520 (0xe2a5ec + 0x84 = 0xe2a670), the skirt-wide box. So a structure that never moves is in the cells of
  // its skirt: GetUnitsInRect around a build site next to a finished mex returns the mex. (Only a later shape change
  // from Lua, e.g. AirUnit's RevertCollisionShape when it lands, puts a unit back on its plain box until it moves.)
  // Seen: Skirmish 969, M28's IsEngineerAvailable finds a finished mex in GetUnitsInRect of engineer 0x11's pgen
  // site (676.99, 349.99; the mex 0x3 at 680.5 has a 2.5-wide skirt, x from 679.25: cell 169) and makes it
  // available: one Lua Random more (m28factory.lua:273), and at 979 the two engineers stop to build closer.
  sec.To(prof::kSecCuGrid);
  GridUpdate(*this, u, true);
  sec.To(-1);
  // the constructor gives both poses the unit's transform (0x6a5c74, 0x6a5c82)
  u->poseBasePos = u->prevPoseBasePos = u->position;
  u->poseBaseOri = u->prevPoseBaseOri = u->orientation;
  u->poseSet = true;
  sec.To(prof::kSecCuAirTransport);
  if (u->motion.bp->motionType == kMotionAir) AirInit(*this, u);
  TransportCreate(*this, u);
  sec.To(prof::kSecCuIntel);
  CreateUnitIntel(*this, L, u);
  sec.To(prof::kSecCuFields);
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
  sec.To(prof::kSecCuScript);  // (to the end: the lists, the occupancy and the callbacks too)
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
  if (u->isFactoryBuilder && u->motion.bp->motionType == kMotionNone) {
    // the Unit constructor reserves a factory structure's skirt in the o-grid (air_extra.md 3.5): no unit's
    // PrepareMove (a builder's spot, a landing spot, a guard's anchor) may end inside it
    float sk[4];
    UnitSkirtRect(u, sk);
    const int rr[4] = {static_cast<int>(std::floor(sk[0])), static_cast<int>(std::floor(sk[1])),
                       static_cast<int>(std::ceil(sk[2])), static_cast<int>(std::ceil(sk[3]))};
    GroundReserveRect(*this, u, rr);
  }
  if (builder) u->creatorId = EntityRef(builder);
  if (u->isFactoryBuilder) SetUpInitialRally(*this, u);  // Unit ctor end (0x6a6413)
  if (army) AssignNewUnitToPool(army, u);  // Unit ctor 0x6a63e6 (sim/ai_platoon.cpp)

  u->isPod = BpInCategory(*this, u->blueprint, "POD") || BpInCategory(*this, u->blueprint, "STATIONASSISTPOD");
  u->isEngineer = BpInCategory(*this, u->blueprint, "ENGINEER");
  // Unit ctor 0x6a5800: SetAutoMode(blueprint AI.InitialAutoMode), which tells the script either way, then
  // OnPreCreate
  u->autoMode = u->bpData && u->bpData->initialAutoMode;
  CallMethod(L, u, u->autoMode ? "OnAutoModeOn" : "OnAutoModeOff", 0);
  CallMethod(L, u, "OnPreCreate", 0);
  u->layer = LayerName(startLayer);
  u->createLayer = startLayer;
  {  // CalcMoveWater 0x6c3480 (any layer but Land and Seabed, no hover, no flyer) sets a unit on the water only when it
     // moved, dives or still pitches (0x6c34b0 / 0x6c3523): no creation snap there (as NotifyDetached, build.cpp).
     // Seen: MP1 9085, the stream's GiveUnitsToPlayer hands army 7's surfacing Seraphim destroyer 0x700034 (Sub,
     // MovingUp, y 17.0, pitched) to army 1 as 0x1000c8, made where it was: the original's stays at y 17.0, pitched,
     // until it moves; ours set it at its depth (15.5) and levelled it in its first motion step.
    const MotionBlueprint* mb = u->motion.bp;
    if (mb && mb->motionType != kMotionNone && !mb->canFly && mb->motionType != kMotionHover && startLayer != kLayerLand &&
        startLayer != kLayerSeabed)
      u->motion.needSnap = false;
  }
  lua_pushstring(L, u->layer.c_str());
  lua_pushstring(L, "None");
  CallMethod(L, u, "OnLayerChange", 2);
  // Unit ctor 0x6a5b6e, right after SetCurrentLayer: the terrain type at the given place (from 0, so a unit on
  // any typed cell hears OnTerrainTypeChange(type, nil) before its weapons and OnCreate)
  UpdateTerrainType(*this, u, pos.x, pos.z);
  MotionCreated(*this, u);  // (the CUnitMotion ctor follows SetCurrentLayer: 0x6a5b65, 0x6a5df2)

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
      w->bpIndex = i;
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
      // (the UnitWeapon ctor 0x6d4310 makes the fire task's thread, CreateWeapon 0x5d76e0 then the acquire task's)
      u->weaponThreads.emplace_back(w, false);
      u->weaponThreads.emplace_back(w, true);
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
  // the army's unit statistics (Unit ctor): a construction counts for its blueprint in "Units_BeingBuilt" (0x6a6741,
  // every blueprint; the item's own value is left alone) before OnStartBeingBuilt; a complete unit of a blueprint with
  // CapCost > 0 (bp+0x1f8, 0x6a6621) counts in "Units_Active" and "Units_History", its blueprint's value + 1 and the
  // item + 1 as an int (0x6a663b, 0x6a66c7), after OnStopBeingBuilt
  if (!complete && army) army->AddBpStat("Units_BeingBuilt", &bp, 1.0f);
  PushObject(L, builder);
  lua_pushstring(L, u->layer.c_str());
  CallMethod(L, u, complete ? "OnStopBeingBuilt" : "OnStartBeingBuilt", 2);
  if (complete && army && u->capCost > 0.0f) {
    for (const char* name : {"Units_Active", "Units_History"}) {
      army->AddBpStat(name, &bp, 1.0f);
      army->stats[name] += 1.0f;
    }
  }
  lua_settop(L, top);
  return u;
}

// Unit::Warp 0x6aa310: Entity::GetStartingLayer for the new place, IUnit::CalcSpawnElevation for its height
// (the y given is dropped), CUnitMotion::Warp 0x6b93d0 (the entity's two transforms both become the new one, so
// it has no velocity; the motion's target is the place; on land the next motion tick takes the ground) and the
// navigator's AbortMove; a unit without a motion just takes the transform. Then SetCurrentLayer.
void Sim::WarpUnit(lua_State* L, Unit* u, Vec3 pos, Quat q) {
  if (!u->blueprint) return;
  const BlueprintInfo& bp = *u->blueprint;
  const TerrainMap* m = map_.get();
  const uint8_t layer = StartingLayer(*this, bp, m, pos, 0, u->altFootprintNav || u->altFootprintForced);
  const float water = m && m->hasWater ? m->waterElevation : -10000.0f;
  const bool mobile = u->motion.bp && u->motion.bp->mobile();
  if (layer & (kLayerLand | kLayerSeabed)) {
    if (m) pos.y = m->TerrainHeight(pos.x, pos.z);
  } else if (layer == kLayerWater) {
    pos.y = water;
  } else if (layer == kLayerSub) {
    bps_.PushTable(L, bp);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    pos.y = water + FieldNumber(L, -1, "Elevation", 0);
    lua_pop(L, 2);
  } else if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {
    pos.y = AirSpawnHeight(*this, bp, L, pos.x, pos.z, pos.y);
  } else if (layer & (kLayerAir | 0x20)) {  // (CalcSpawnElevation for Air / Orbit, as CreateUnit)
    pos.y = AirLayerSpawnHeight(*this, bp, L, pos.x, pos.z);
  }
  u->position = pos;
  u->orientation = q;
  u->lastPosition = pos;
  if (mobile) {
    MotionStop(u);
    u->motion.vel = {};
    u->motion.motionTarget = pos;  // (CUnitMotion::Warp: SetTarget 0x6b85e0 with the new place, 0x6b9421)
    if (u->motion.air) AirWarp(*this, u);
    if (layer == kLayerLand) u->motion.surfaceNext = true;
  }
  // Entity::Warp 0x679210 (called by Unit::Warp at 0x6aa415, after CUnitMotion::Warp and before SetCurrentLayer):
  // for an entity with intel (+0x1d8, its CIntel), CIntel::Update 0x76e4c0 at the new place, 1 up, with the game
  // tick (0x67923a..0x67927f): every handle re-rasters there at once and its 6-tick clock restarts. (Its two
  // AdvanceCoords before that run CIntel::ForceUpdate 0x76e490 at the same place; the exact update ends the same.)
  // Ours moved the circles only at the next AdvanceIntelCoords, by the r / 3 or 6-tick rule.
  IntelExactUpdate(*this, u);
  const std::string name = LayerName(layer);
  if (u->layer != name) {
    const std::string old = u->layer;
    u->layer = name;
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, old.c_str());
    CallMethod(L, u, "OnLayerChange", 2);
  }
  UpdateTerrainType(*this, u, pos.x, pos.z);  // Unit::Warp 0x6aa428, after SetCurrentLayer, at the given x / z
  MarkUnitsMoved();
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
  // Prop::Prop, after OnCreate (0x6f9f1f..0x6f9fde): a prop whose blueprint gives reclaim (Economy.ReclaimMassMax or
  // ReclaimEnergyMax > 0: +0x270 / +0x274, read from bp+0x1a4 / +0x1a8 at 0x6f9ed6) is marked (+0x278 = 1) and
  // enters the occupancy grid with its blueprint footprint (bp+0xd8 SizeX, +0xd9 SizeZ, +0xda OccupancyCaps; the
  // default caps are 0, REntityBlueprint ctor 0x511d18) at the place it was created at (the transform passed in,
  // not where OnCreate warped it): COGrid::ExecuteOccupy 0x721a90 sets the land / water layer bits only for caps & 7
  // / caps & 8, but always dirties the path clusters around (0x721ae0: PathQueue::DirtyClusters 0x76bbd0 -> every
  // ClusterMap::DirtyRect). So a tree (no footprint caps) blocks nothing, yet each one made or destroyed makes the
  // path searches rebuild the clusters around it. ~Prop releases it (0x6fa05a, ProcessDestroyQueue).
  {
    bps_.PushTable(L, bp);
    int bpIdx = lua_gettop(L);
    auto num = [&](const char* a, const char* b) {
      PushPath(L, bpIdx, a, b);
      double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
      lua_pop(L, 1);
      return v;
    };
    const float mass = static_cast<float>(num("Economy", "ReclaimMassMax"));
    const float energy = static_cast<float>(num("Economy", "ReclaimEnergyMax"));
    p->bpReclaimMass = mass;
    p->bpReclaimEnergy = energy;
    if (mass > 0 || energy > 0) {
      p->occupies = true;
      p->occSizeX = static_cast<uint8_t>(static_cast<int>(num("Footprint", "SizeX")));
      p->occSizeZ = static_cast<uint8_t>(static_cast<int>(num("Footprint", "SizeZ")));
      p->occCaps = static_cast<uint8_t>(static_cast<int>(num("Footprint", "OccupancyCaps")));
      PropOccupancy(*this, p, pos, true);
    }
    lua_settop(L, top);
  }
  return p;
}

Entity* Sim::AdoptScriptEntity(lua_State* L, int t, int spec, bool shield) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  std::unique_ptr<Entity> owned = shield ? std::make_unique<ShieldEntity>() : std::make_unique<Entity>();
  Entity* e = owned.get();
  e->kind = shield ? Entity::Kind::Shield : Entity::Kind::Entity;
  // The army (it is also the id's, (army | 0x500) << 20 at 0x6924f3; 0xff: none). _c_CreateEntity 0x692330:
  // spec.Owner first (0x6923b4..0x692483): a number is taken as the 0-based army index itself (LuaStackObject::
  // GetInteger 0x41b520, truncated), anything else as an entity (func_GetEntityOpt 0x6208d0), whose army it gives;
  // with no Owner, spec.Army (0x69248c..0x6924bf): a 1-based index, "Army < 0 passed in" below 1. FAF's pings
  // (SimPing.lua 95: {Owner = data.Owner}, the pinging army's index) are made so; with no army they were shown to
  // the enemy (ui REQUESTS 43). _c_CreateShield 0x776aa0 reads spec.Owner as an entity only (0x776b1e..0x776b93).
  if (lua_istable(L, spec)) {
    lua_pushstring(L, "Owner");
    lua_gettable(L, spec);
    const int ownerIdx = lua_gettop(L);
    lua_pushstring(L, "Army");
    lua_gettable(L, spec);
    const int armyIdx = lua_gettop(L);
    auto armyAt = [&](int i) -> Army* {
      return i >= 0 && i < static_cast<int>(armies_.size()) ? armies_[static_cast<size_t>(i)].get() : nullptr;
    };
    if (!shield && lua_type(L, ownerIdx) == LUA_TNUMBER) {
      e->army = armyAt(static_cast<int>(static_cast<float>(lua_tonumber(L, ownerIdx))));
    } else if (Entity* owner = ToObject<Entity>(L, ownerIdx)) {
      // (the owner gives the army only: Entity::Entity 0x677f40 gets the army index and no transform, 0x69251b..
      // 0x69252a, so a script entity starts at the origin, unturned, and a SetCollisionShape in its OnCreate puts its
      // primitive in the grid there (0x67ad9a..0x67ada5). FAF's DepthCharge (defaultantiprojectile.lua 135-143: the
      // sphere first, then AttachTo(spec.Owner, -1)) so gets its cells by the shell only at the beat's AdvanceCoords,
      // the two of one launch in their making order. Seen (probe mp1_13863): MP1 13856, army 6's torpedo 0x10600046
      // crosses both spheres of army 5's anti-torpedo shells 0x105000ee / 0x105000ef in one cell; the original asks
      // 0x105000ef's first and 0x105000ee's last, whose SetNewTarget wins, and the torpedo strikes 0x105000ee at 13862
      // (its impact and Lua Random at 13863). Ours put each sphere in the grid at its shell when made and moved
      // 0x105000ee's once more at the beat's end, which put it in front: it was asked first, 0x105000ef last.)
      e->army = owner->army;
    } else if (!shield && lua_isnil(L, ownerIdx) && !lua_isnil(L, armyIdx)) {
      if (lua_type(L, armyIdx) != LUA_TNUMBER) luaL_error(L, "integer expected");
      const int a = static_cast<int>(static_cast<float>(lua_tonumber(L, armyIdx))) - 1;
      if (a < 0) luaL_error(L, "Army < 0 passed in");
      e->army = armyAt(a);
    }
    lua_pop(L, 2);
  }
  e->id = ReserveId(e->army, shield ? 0x4 : 0x5);  // shields 0x4AAxxxxx, script entities 0x5AAxxxxx
  BindObject(L, t, e);
  owned_.push_back(std::move(owned));
  RegisterEntity(e);
  if (shield) shields.push_back(static_cast<ShieldEntity*>(e));
  scriptEntities.push_back(e);
  return e;
}

// Sim::CreatePlatoon: sim/ai_platoon.cpp

}  // namespace moho
