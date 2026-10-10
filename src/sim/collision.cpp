// Collision primitives: see collision.h.
#include "sim/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "sim/entity_grid.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/vecmath.h"

namespace moho {

using namespace vm;

bool GetWorldShape(const Entity* e, WorldShape* out) {
  const CollisionShape& s = e->shape;
  if (s.type == ShapeType::None) return false;
  out->type = s.type;
  out->c = Add(e->position, Rotate(e->orientation, s.center));
  if (s.type == ShapeType::Box) {
    out->ax[0] = Rotate(e->orientation, {1, 0, 0});
    out->ax[1] = Rotate(e->orientation, {0, 1, 0});
    out->ax[2] = Rotate(e->orientation, {0, 0, 1});
    out->half = s.half;
  } else {
    out->r = s.radius;
  }
  return true;
}

bool SegmentHit(const WorldShape& s, Vec3 p0, Vec3 p1, Vec3* hit, float* dist) {
  Vec3 d = Sub(p1, p0);
  float len = Len(d);
  float t0 = 0, t1 = 1;  // segment parameter of entry and exit
  if (s.type == ShapeType::Sphere) {
    Vec3 m = Sub(p0, s.c);
    float a = Dot(d, d), b = Dot(m, d), c = Dot(m, m) - s.r * s.r;
    if (a <= 0) return false;
    float disc = b * b - a * c;
    if (disc < 0) return false;
    float sq = std::sqrt(disc);
    t0 = (-b - sq) / a;
    t1 = (-b + sq) / a;
  } else if (s.type == ShapeType::Box) {
    Vec3 m = Sub(p0, s.c);
    float lo = -std::numeric_limits<float>::infinity(), hi = std::numeric_limits<float>::infinity();
    const float h[3] = {s.half.x, s.half.y, s.half.z};
    for (int i = 0; i < 3; ++i) {
      float o = Dot(m, s.ax[i]), v = Dot(d, s.ax[i]);
      if (std::fabs(v) < 1e-12f) {
        if (o < -h[i] || o > h[i]) return false;
        continue;
      }
      float a = (-h[i] - o) / v, b = (h[i] - o) / v;
      if (a > b) std::swap(a, b);
      lo = std::max(lo, a);
      hi = std::min(hi, b);
      if (lo > hi) return false;
    }
    t0 = lo;
    t1 = hi;
  } else {
    return false;
  }
  float t;
  if (t0 >= 0 && t0 <= 1) t = t0;          // enters
  else if (t0 < 0 && t1 >= 0 && t1 <= 1) t = t1;  // starts inside: leaves
  else return false;                        // misses, or entirely inside
  *hit = Add(p0, Mul(d, t));
  *dist = t * len;
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
  if (s.type == ShapeType::Sphere) {
    float rr = s.r + r;
    return Len2(Sub(c, s.c)) < rr * rr;
  }
  if (s.type == ShapeType::Box) return BoxDist2(s, c) < r * r;
  return false;
}

bool PointInShape(const WorldShape& s, Vec3 p) {
  if (s.type == ShapeType::Sphere) return Len2(Sub(p, s.c)) < s.r * s.r;
  if (s.type == ShapeType::Box) {
    Vec3 m = Sub(p, s.c);
    return std::fabs(Dot(m, s.ax[0])) <= s.half.x && std::fabs(Dot(m, s.ax[1])) <= s.half.y &&
           std::fabs(Dot(m, s.ax[2])) <= s.half.z;
  }
  return false;
}

void ShapeBounds(const WorldShape& s, Vec3* mn, Vec3* mx) {
  Vec3 e;
  if (s.type == ShapeType::Sphere) {
    e = {s.r, s.r, s.r};
  } else {
    const float h[3] = {s.half.x, s.half.y, s.half.z};
    float ex = 0, ey = 0, ez = 0;
    for (int i = 0; i < 3; ++i) {
      ex += std::fabs(s.ax[i].x) * h[i];
      ey += std::fabs(s.ax[i].y) * h[i];
      ez += std::fabs(s.ax[i].z) * h[i];
    }
    e = {ex, ey, ez};
  }
  *mn = Sub(s.c, e);
  *mx = Add(s.c, e);
}

static void RevertShape(lua_State* L, Entity* e);
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

}  // namespace

void RegisterCollisionBindings(lua_State* L) {
  SetMethod(L, "Entity", "SetCollisionShape", l_SetCollisionShape);
  SetMethod(L, "Unit", "RevertCollisionShape", l_RevertCollisionShape);
  SetMethod(L, "Entity", "RevertCollisionShape", l_RevertCollisionShape);
}

}  // namespace moho
