// Collision primitives: see collision.h.
#include "sim/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "script/script_state.h"
#include "sim/entity_grid.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

// In the exe's order of operations: the doubled components first, then the products. On a unit standing level its
// quaternion's x and z can be the smallest denormals, and there the order decides the axes' last bits: (2w) * z is
// one step of 2^-149, 2 * (w * z) two. (Seen in the original's memory: engineer 0x400033's box primitive, Skirmish
// 2485-2491, holds exactly these axes.)
void BoxAxes(const Quat& q, Vec3 ax[3]) {
  const float tw = q.w * 2.0f, tx = q.x * 2.0f, ty = q.y * 2.0f, tz = q.z * 2.0f;
  const float yy = ty * q.y, zz = tz * q.z, xz = tz * q.x, xy = ty * q.x, xx = tx * q.x;
  const float wx = tw * q.x, yz = tz * q.y, wy = tw * q.y, wz = tw * q.z;
  ax[0] = {1.0f - (zz + yy), wz + xy, xz - wy};
  ax[1] = {xy - wz, 1.0f - (zz + xx), yz + wx};
  ax[2] = {wy + xz, yz - wx, 1.0f - (yy + xx)};
}

bool GetWorldShape(const Entity* e, WorldShape* out) {
  const CollisionShape& s = e->shape;
  if (s.type == ShapeType::None) return false;
  out->type = s.type;
  const Vec3& pos = e->shapeHeld ? e->shapePos : e->position;
  const Quat& ori = e->shapeHeld ? e->shapeOri : e->orientation;
  // the primitive's SetTransform (box 0x4ff470, sphere 0x4febc0): its offset turned by MultQuadVec 0x452d40, through
  // the quaternion's matrix 0x452fd0 (RotateM), plus the position; not the cross-product form, which differs from it
  // in the last bits
  out->c = Add(pos, RotateM(ori, s.center));
  if (s.type == ShapeType::Box) {
    BoxAxes(ori, out->ax);  // (the box primitive's SetTransform 0x4ff470)
    out->half = s.half;
  } else {
    out->r = s.radius;
  }
  return true;
}

bool SegmentHit(const WorldShape& s, Vec3 p0, Vec3 p1, Vec3* hit, float* dist) {
  // The segment as centre, unit direction and half length (0x4fe130)
  const Vec3 h{(p1.x - p0.x) * 0.5f, (p1.y - p0.y) * 0.5f, (p1.z - p0.z) * 0.5f};
  const float ext = std::sqrt(h.y * h.y + h.z * h.z + h.x * h.x);
  Vec3 dir{};
  if (ext > 0.0f) {
    const float inv = 1.0f / ext;
    dir = {inv * h.x, h.y * inv, h.z * inv};
  }
  const Vec3 org{p0.x + h.x, p0.y + h.y, p0.z + h.z};
  const Vec3 diff{org.x - s.c.x, org.y - s.c.y, org.z - s.c.z};
  float t;
  if (s.type == ShapeType::Sphere) {
    // 0x4fe9d0: Wild Magic's IntrSegment3Sphere3::Find 0xa471c0, its first point
    const float a0 = (diff.y * diff.y + diff.x * diff.x + diff.z * diff.z) - s.r * s.r;
    const float a1 = diff.y * dir.y + diff.x * dir.x + diff.z * dir.z;
    const float discr = a1 * a1 - a0;
    if (discr < 0.0f) return false;
    const float tmp0 = ext * ext + a0, tmp1 = (a1 + a1) * ext;
    const float qm = tmp0 - tmp1, qp = tmp0 + tmp1;
    if (static_cast<double>(qm * qp) <= 0.0) {  // one end inside: where it crosses the surface
      const float root = std::sqrt(discr);
      t = 0.0f < qm ? -a1 - root : root - a1;
    } else if (qm > 0.0f && std::fabs(a1) < ext) {  // both ends outside, through it: where it enters
      t = discr >= 1e-6f ? -a1 - std::sqrt(discr) : -a1;
    } else {
      return false;  // misses, or lies inside
    }
  } else if (s.type == ShapeType::Box) {
    // 0x4ff2d0: Wild Magic's IntrSegment3Box3 (not solid) 0xa462a0 -> DoClipping 0xa50220, its first point: where
    // the segment enters the box, or THE SEGMENT'S START when it begins inside and leaves; nothing when it lies
    // inside or misses
    const float he[3] = {s.half.x, s.half.y, s.half.z};
    float bo[3], bd[3];
    for (int i = 0; i < 3; ++i) {
      bo[i] = s.ax[i].y * diff.y + s.ax[i].x * diff.x + s.ax[i].z * diff.z;
      bd[i] = s.ax[i].y * dir.y + dir.x * s.ax[i].x + s.ax[i].z * dir.z;
    }
    float t0 = -ext, t1 = ext;
    const float save0 = t0, save1 = t1;
    auto clip = [&](float denom, float numer) {  // 0xa4fcc0
      if (denom > 0.0f) {
        if (numer > denom * t1) return false;
        if (numer > denom * t0) t0 = numer / denom;
        return true;
      }
      if (denom < 0.0f) {
        if (numer > denom * t0) return false;
        if (numer > denom * t1) t1 = numer / denom;
        return true;
      }
      return numer <= 0.0f;
    };
    for (int i = 0; i < 3; ++i)
      if (!clip(bd[i], -bo[i] - he[i]) || !clip(-bd[i], bo[i] - he[i])) return false;
    if (t0 == save0 && t1 == save1) return false;
    t = t0;
  } else {
    return false;
  }
  *hit = {org.x + dir.x * t, org.y + dir.y * t, org.z + dir.z * t};
  const Vec3 fromStart{hit->x - p0.x, hit->y - p0.y, hit->z - p0.z};
  *dist = std::sqrt(fromStart.x * fromStart.x + fromStart.y * fromStart.y + fromStart.z * fromStart.z);
  return true;
}

namespace {
// Squared distance from p to the box.
float BoxDist2(const WorldShape& s, Vec3 p) {
  Vec3 m = Sub(p, s.c);
  const float h[3] = {s.half.x, s.half.y, s.half.z};
  float d2 = 0;
  for (int i = 0; i < 3; ++i) {
    float o = Dot(m, s.ax[i]);
    float e = std::fabs(o) - h[i];
    if (e > 0) d2 += e * e;
  }
  return d2;
}
}  // namespace

bool SphereOverlap(const WorldShape& s, Vec3 c, float r) {
  if (s.type == ShapeType::Sphere) {  // CColPrimitive_Sphere::CollideSphere 0x4fe7a0: squares summed z, x, y
    float rr = s.r + r;
    const float dx = c.x - s.c.x, dy = c.y - s.c.y, dz = c.z - s.c.z;
    return rr * rr > (dz * dz + dx * dx) + dy * dy;
  }
  if (s.type == ShapeType::Box) return BoxDist2(s, c) < r * r;
  return false;
}

bool PointInShape(const WorldShape& s, Vec3 p) {
  if (s.type == ShapeType::Sphere) {  // CColPrimitive_Sphere::PointInShape 0x4feb60: squares summed x, z, y
    const float dx = p.x - s.c.x, dy = p.y - s.c.y, dz = p.z - s.c.z;
    return s.r * s.r > (dx * dx + dz * dz) + dy * dy;
  }
  if (s.type == ShapeType::Box) {
    Vec3 m = Sub(p, s.c);
    return std::fabs(Dot(m, s.ax[0])) <= s.half.x && std::fabs(Dot(m, s.ax[1])) <= s.half.y &&
           std::fabs(Dot(m, s.ax[2])) <= s.half.z;
  }
  return false;
}

// The primitive's GetBoundingBox (vt slot 0), what Entity::UpdateAABox 0x679180 stores at entity+0x240..+0x254 and the
// box queries test (GatherUnmarkedUnitsInBox 0x721c00 at 0x721d00..0x721d1e, CollectEntitiesInBox 0x721dc0, touching
// passes), and what Unit::UpdateCollision 0x6ab520 places in the entity grid (0x6ab608, the skirt-wide box). A sphere
// 0x4ff9a0: centre -/+ radius. A box 0x4ffc20 -> 0x4fe2d0: its eight corners ((c -/+ a0 h0) -/+ a1 h1) -/+ a2 h2
// (0x4fff00), their aligned box (0xa3ae40: min and max, centre (max + min) x 0.5, half size (max - min) x 0.5, axes
// the identity), then centre -/+ half size. Not centre -/+ the axes' parts times the half sizes summed: the same box
// but for the last bit, and a box edge on a whole cell decides whether a path cell is free. Seen: MP1 14687, army 5's
// T2 engineer 0x500105 asks the cell (417,892) past 0x500093 at (417.526184, 891.581726): the summed form put its box's
// z max at 892 exactly (touching the square [892, 893]: blocked), the corners' fit a float step below it; the
// original's path went (416,892) diagonally, ours along x first.
void ShapeBounds(const WorldShape& s, Vec3* mn, Vec3* mx) {
  if (s.type == ShapeType::Sphere) {
    const Vec3 e{s.r, s.r, s.r};
    *mn = Sub(s.c, e);
    *mx = Add(s.c, e);
    return;
  }
  auto times = [](const Vec3& v, float k) { return Vec3{v.x * k, v.y * k, v.z * k}; };
  auto neg = [](const Vec3& v) { return Vec3{-v.x, -v.y, -v.z}; };
  const Vec3 A = times(s.ax[0], s.half.x), B = times(s.ax[1], s.half.y), C = times(s.ax[2], s.half.z);
  Vec3 lo{}, hi{};
  for (int i = 0; i < 8; ++i) {
    const Vec3 a = (i & 1) ? A : neg(A), b = (i & 2) ? B : neg(B), c = (i & 4) ? C : neg(C);
    const Vec3 p = Add(Add(Add(s.c, a), b), c);
    if (i == 0) {
      lo = hi = p;
      continue;
    }
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const Vec3 c{(hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f};
  const Vec3 e{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
  *mn = Sub(c, e);
  *mx = Add(c, e);
}

bool GridCellsMeet(const Vec3& mnA, const Vec3& mxA, const Vec3& mnB, const Vec3& mxB) {
  // 0x4fcbe0: first cell floor(min) >> 2 (not below 0), one past the last (ceil(max) + 3) >> 2
  auto lo = [](float v) { return std::max(0, static_cast<int>(std::floor(v)) >> 2); };
  auto hi = [](float v) { return (static_cast<int>(std::ceil(v)) + 3) >> 2; };
  return lo(mnA.x) < hi(mxB.x) && lo(mnB.x) < hi(mxA.x) && lo(mnA.z) < hi(mxB.z) && lo(mnB.z) < hi(mxA.z);
}

static void RevertShape(lua_State* L, Entity* e);
// A primitive against an oriented box (the primitives' vt+0x1c: box 0x4ff260 -> 0x474830, sphere 0x4fe860), as
// CollectEntitiesInBox 0x721dc0 asks it: touching counts.
bool ShapeOverlapsOBox(const WorldShape& s, Vec3 bc, const Vec3 A[3], const float ah[3]) {
  auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
  const Vec3 T{s.c.x - bc.x, s.c.y - bc.y, s.c.z - bc.z};
  if (s.type == ShapeType::Sphere) {
    float d2 = 0;
    for (int i = 0; i < 3; ++i) {
      const float c = dot(T, A[i]);
      const float v = c < -ah[i] ? -ah[i] - c : (c > ah[i] ? c - ah[i] : 0.0f);
      d2 += v * v;
    }
    return d2 <= s.r * s.r;
  }
  if (s.type != ShapeType::Box) return false;
  const float oh[3] = {s.half.x, s.half.y, s.half.z};
  auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
  auto sep = [&](Vec3 L) {
    if (dot(L, L) < 1e-12f) return false;
    float ra = 0, rb = 0;
    for (int i = 0; i < 3; ++i) {
      ra += ah[i] * std::fabs(dot(A[i], L));
      rb += oh[i] * std::fabs(dot(s.ax[i], L));
    }
    return std::fabs(dot(T, L)) > ra + rb;
  };
  for (int i = 0; i < 3; ++i)
    if (sep(A[i]) || sep(s.ax[i])) return false;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (sep(cross(A[i], s.ax[j]))) return false;
  return true;
}

bool ShapeOverlapsAABox(const WorldShape& s, Vec3 mn, Vec3 mx) {
  Vec3 bc{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
  Vec3 bh{(mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f};
  if (s.type == ShapeType::Sphere) {
    float d2 = 0;
    const float c[3] = {s.c.x, s.c.y, s.c.z}, lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
    for (int i = 0; i < 3; ++i) {
      float v = c[i] < lo[i] ? lo[i] - c[i] : (c[i] > hi[i] ? c[i] - hi[i] : 0.0f);
      d2 += v * v;
    }
    return d2 <= s.r * s.r;
  }
  if (s.type != ShapeType::Box) return false;
  // separating axis test: OBB (s) vs AABB (bc, bh)
  const Vec3 A[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const float ah[3] = {bh.x, bh.y, bh.z}, oh[3] = {s.half.x, s.half.y, s.half.z};
  Vec3 T{s.c.x - bc.x, s.c.y - bc.y, s.c.z - bc.z};
  auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
  auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
  auto sep = [&](Vec3 L) {
    float l2 = dot(L, L);
    if (l2 < 1e-12f) return false;
    float ra = 0, rb = 0;
    for (int i = 0; i < 3; ++i) {
      ra += ah[i] * std::fabs(dot(A[i], L));
      rb += oh[i] * std::fabs(dot(s.ax[i], L));
    }
    return std::fabs(dot(T, L)) > ra + rb;
  };
  for (int i = 0; i < 3; ++i)
    if (sep(A[i]) || sep(s.ax[i])) return false;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (sep(cross(A[i], s.ax[j]))) return false;
  return true;
}

void RevertCollisionShape(lua_State* L, Entity* e) {
  RevertShape(L, e);
  GridUpdate(*Sim::From(L), e, false);  // the plain primitive (a unit widens at its next AdvanceCoords)
}
static void RevertShape(lua_State* L, Entity* e) {
  e->shape = CollisionShape{};
  if (!e->blueprint) return;
  int top = lua_gettop(L);
  Sim::From(L)->blueprints().PushTable(L, *e->blueprint);
  int t = lua_gettop(L);
  std::string type = lu::Str(L, t, "CollisionShape", e->kind == Entity::Kind::Projectile ? "None" : "Box");
  float sx = lu::Num(L, t, "SizeX", 1), sy = lu::Num(L, t, "SizeY", 1), sz = lu::Num(L, t, "SizeZ", 1);
  float ox = lu::Num(L, t, "CollisionOffsetX", 0), oy = lu::Num(L, t, "CollisionOffsetY", 0),
        oz = lu::Num(L, t, "CollisionOffsetZ", 0);
  lua_settop(L, top);
  e->bpCenter = {ox, sy * 0.5f + oy, oz};
  e->bpSizeY = sy;
  if (type == "Box") {
    e->shape.type = ShapeType::Box;
    e->shape.center = {ox, oy + sy * 0.5f, oz};
    e->shape.half = {sx * 0.5f, sy * 0.5f, sz * 0.5f};
  } else if (type == "Sphere") {
    e->shape.type = ShapeType::Sphere;
    e->shape.center = {ox, oy + sx * 0.5f, oz};
    e->shape.radius = sx * 0.5f;
  }
}

namespace {

// Entity:SetCollisionShape(type, cx, cy, cz, sx[, sy, sz]) (0x68f200)
int l_SetCollisionShape(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  const char* type = luaL_checkstring(L, 2);
  std::string t = type;
  if (!strncasecmp(type, "None", 4) && t.size() == 4) {
    e->shape = CollisionShape{};
    GridUpdate(*Sim::From(L), e, false);
    return 0;
  }
  auto num = [&](int i) { return static_cast<float>(luaL_checknumber(L, i)); };
  if (!strcasecmp(type, "Box")) {
    e->shape.type = ShapeType::Box;
    e->shape.center = {num(3), num(4), num(5)};
    e->shape.half = {num(6), num(7), num(8)};
  } else if (!strcasecmp(type, "Sphere")) {
    e->shape.type = ShapeType::Sphere;
    e->shape.center = {num(3), num(4), num(5)};
    e->shape.radius = num(6);
  } else {
    return luaL_error(L, "Unknown shape type %s; should be None, Box, or Sphere", type);
  }
  GridUpdate(*Sim::From(L), e, false);
  return 0;
}

int l_RevertCollisionShape(lua_State* L) {
  RevertCollisionShape(L, CheckObject<Entity>(L, 1));
  return 0;
}

// Entity:GetCollisionExtents() 0x68ca80: one argument (the entity); nil without a collision primitive (+0x178),
// else a new table {Min = Vector, Max = Vector}, the world box the primitive gives (vt-1, GetBoundingBox: a sphere
// 0x4ff9a0 centre -+ radius on each axis; a box 0x4ffc20 the box of its eight corners, 0x4fe2d0: ShapeBounds). The objective arrow sizes itself by it; FAF's
// Prop:CacheAndRemoveCollisionExtents keeps it to put the shape back later.
int l_GetCollisionExtents(lua_State* L) {
  if (lua_gettop(L) != 1) luaL_error(L, "%s\n  expected %d args, but got %d", "Entity:GetCollisionExtents()", 1, lua_gettop(L));
  Entity* e = CheckObject<Entity>(L, 1);
  WorldShape ws;
  if (!GetWorldShape(e, &ws)) {
    lua_pushnil(L);
    return 1;
  }
  Vec3 mn, mx;
  ShapeBounds(ws, &mn, &mx);
  lua_newtable(L);
  lua_pushstring(L, "Min");
  PushVector(L, mn.x, mn.y, mn.z);
  lua_settable(L, -3);
  lua_pushstring(L, "Max");
  PushVector(L, mx.x, mx.y, mx.z);
  lua_settable(L, -3);
  return 1;
}

}  // namespace

void RegisterCollisionBindings(lua_State* L) {
  SetMethod(L, "Entity", "SetCollisionShape", l_SetCollisionShape);
  SetMethod(L, "Unit", "RevertCollisionShape", l_RevertCollisionShape);
  SetMethod(L, "Entity", "RevertCollisionShape", l_RevertCollisionShape);
  SetMethod(L, "Entity", "GetCollisionExtents", l_GetCollisionExtents);
}

}  // namespace moho
