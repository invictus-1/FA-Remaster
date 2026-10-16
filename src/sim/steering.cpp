// Land steering between units (engine-ref unit_collision.md): contact pushes, the right of way,
// predicted collisions and the reactions to them; and the steering's per-tick spline handling
// (land_motion_blocking.md 2.4).
#include "sim/steering.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "script/script_state.h"
#include "sim/navigation.h"
#include "sim/combat.h"
#include "sim/formation.h"
#include "sim/landnav.h"
#include "sim/motion.h"
#include "sim/sim.h"
#include "sim/units.h"

namespace moho {
namespace {

bool St(const Unit* u, const char* s) { return u->unitStates.count(s) != 0; }

bool Naval(const Unit* u) {
  const MotionBlueprint* b = u->motion.bp;
  if (!b || !u->blueprint) return false;
  if (b->naval < 0) b->naval = BpInCategory(*Sim::From(u->luaState()), u->blueprint, "NAVAL") ? 1 : 0;
  return b->naval == 1;
}
bool AirCat(const Unit* u) {
  const MotionBlueprint* b = u->motion.bp;
  if (!b || !u->blueprint) return false;
  if (b->airCat < 0) b->airCat = BpInCategory(*Sim::From(u->luaState()), u->blueprint, "AIR") ? 1 : 0;
  return b->airCat == 1;
}
bool Mobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }
bool Immobile(const Unit* u) { return u->immobile || St(u, "Immobile"); }
// Entity::GetFootprint (DoCollisionsFor 0x59808b.., ResolvePossibleCollision 0x59741c / 0x59742a): the AltFootprint
// while the unit uses it
int FpFlags(const Unit* u) { return u->motion.bp ? UnitFootprint(u).flags : 0; }
float FpMax(const Unit* u) {
  if (!u->motion.bp) return 1.0f;
  const NamedFootprint& f = UnitFootprint(u);
  return static_cast<float>(std::max(f.sizeX, f.sizeZ));
}
float Mass(const MotionBlueprint& b) { return b.averageDensity * b.sizeZ * b.sizeY * b.sizeX; }
// Units that have a land steering (CAiSteeringImpl): mobile, not aircraft.
bool HasSteering(const Unit* u) {
  const MotionBlueprint* b = u->motion.bp;
  return b && b->mobile() && b->motionType != kMotionAir && !u->motion.ballistic;
}
UnitCommand* Current(const Unit* u) { return u->commands.empty() ? nullptr : u->commands.front().get(); }

float Len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(const Vec3& a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 Norm(const Vec3& v) {
  float l = Len(v);
  return l > 0 ? Scale(v, 1.0f / l) : Vec3{};
}
float Dist2(const Vec3& a, const Vec3& b) {
  Vec3 d = Sub(a, b);
  return Dot(d, d);
}
// Quaternion axes (w, x, y, z at u+0xa4).
Vec3 Forward(const Quat& q) {
  return {2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
}
Vec3 Right(const Quat& q) {
  return {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.w * q.z), 2 * (q.x * q.z - q.w * q.y)};
}
Vec3 Up(const Quat& q) {
  return {2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x)};
}
Vec3 RotY(float a, const Vec3& v) {  // MultQuadVec with (cos a, 0, sin a, 0): a rotation by 2a about +y
  Quat q;
  q.w = std::cos(a);
  q.y = std::sin(a);
  Vec3 f{2 * q.w * q.y * v.z + (1 - 2 * q.y * q.y) * v.x, v.y, (1 - 2 * q.y * q.y) * v.z - 2 * q.w * q.y * v.x};
  return f;
}

Unit* UnitByRef(Sim& sim, uint32_t ref) {
  Entity* e = ref ? sim.FindEntity(ref) : nullptr;
  return e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
}
Sim& SimOf(const Unit* u) { return *Sim::From(u->luaState()); }

// The steering's current waypoint target is valid (st+0x90).
bool Driving(const Unit* u) {
  const UnitMotion& m = u->motion;
  return m.hasGoal && m.navDriven && m.hasWaypoint;
}
int GetVal(const UnitMotion& m) { return m.passThrough ? 1 : 0; }

// UnitMoreInLineToOther 0x62eac0: of two units, the one that points less at the other (null: neither
// points at the other).
const Unit* MoreInLine(const Unit* A, const Unit* B) {
  Vec3 fa = Forward(A->orientation), fb = Forward(B->orientation);
  fa.y = 0;
  fb.y = 0;
  fa = Norm(fa);
  fb = Norm(fb);
  float a = Dot(Norm(Sub(B->position, A->position)), fa);
  float b = Dot(Norm(Sub(A->position, B->position)), fb);
  if (a <= 0 && b <= 0) return nullptr;
  return a <= b ? A : B;
}

}  // namespace

bool SameFormationLayer(const Unit* a, const Unit* b) {
  if (St(a, "Attacking") || St(b, "Attacking")) return false;
  return a->form && a->form == b->form;
}

bool UnitPriority(const Unit* A, const Unit* B) {
  if (Immobile(A) || St(A, "Upgrading")) return true;
  if (Immobile(B) || St(B, "Upgrading")) return false;
  bool na = Naval(A), nb = Naval(B);
  if (na != nb) return na;
  bool fa = (FpFlags(A) & 1) != 0, fb = (FpFlags(B) & 1) != 0;
  if (fa != fb) return fa;
  if (A->motion.bp->canFly && A->layer != "Air") return true;  // landed air-capable unit
  if (B->motion.bp->canFly && B->layer != "Air") return false;
  if (St(A, "WaitingForTransport") && !St(B, "WaitingForTransport")) return true;
  if (A->guardedId && A->guardedId == EntityRef(B)) return false;  // the guarded unit has priority
  if (B->guardedId && B->guardedId == EntityRef(A)) return true;
  bool same = A->form && A->form == B->form;
  if (same && A->formLeader) {
    if (A->formLeader == EntityRef(A)) return true;
    if (A->formLeader == EntityRef(B)) return false;
  }
  bool am = St(A, "Moving"), bm = St(B, "Moving");
  if (am && !bm) return false;  // a standing unit has priority over a mover
  if (!am && bm) return true;
  if (same) {
    if (A->formPathDelay != B->formPathDelay) return A->formPathDelay < B->formPathDelay;
    return B->formRank > A->formRank;
  }
  float sa = FpMax(A), sb = FpMax(B);
  if (sa > sb) return true;
  if (sa < sb) return false;
  if (const Unit* w = MoreInLine(A, B)) return w == A;
  return A->id < B->id;
}

bool UnitIgnores(const Unit* self, const Unit* e, int flags) {
  if (!e || e->dead || e->destroyQueued || e == self || !Mobile(e)) return true;
  if (flags == 1 && (e->position.x != e->lastPosition.x || e->position.y != e->lastPosition.y ||
                     e->position.z != e->lastPosition.z))
    return true;  // it moved this tick
  if (e->parentId || St(e, "Attached")) return true;
  if (self->layer != e->layer) return true;
  if (Naval(self) && !Naval(e)) return true;
  if (AirCat(e) && (e->layer == "Air" || e->transport)) return true;
  if ((FpFlags(self) & 1) && FpFlags(e) == 0) return true;
  if (St(self, "WaitingForTransport") && St(e, "WaitingForTransport") && self->focusId == e->focusId) return true;
  if (St(self, "Upgrading") && e->creatorId && e->creatorId == EntityRef(self)) return true;
  if (flags == 2) return false;
  if (AirCat(e) && self->army == e->army) return true;
  if (!St(self, "WaitingForTransport") && St(e, "WaitingForTransport")) return false;
  return UnitPriority(self, e);
}

namespace {

// ---- contact push ------------------------------------------------------------------------------

struct Obb {
  Vec3 c, ax[3], h;
};
// Box-box penetration (separating axes, the smallest overlap); <= 0: apart.
float ObbDepth(const Obb& A, const Obb& B) {
  float best = 1e30f;
  auto axis = [&](Vec3 L) {
    float l = Len(L);
    if (l < 1e-5f) return true;
    L = Scale(L, 1.0f / l);
    float ra = 0, rb = 0;
    for (int i = 0; i < 3; ++i) {
      ra += std::fabs(Dot(A.ax[i], L)) * (i == 0 ? A.h.x : i == 1 ? A.h.y : A.h.z);
      rb += std::fabs(Dot(B.ax[i], L)) * (i == 0 ? B.h.x : i == 1 ? B.h.y : B.h.z);
    }
    float d = std::fabs(Dot(Sub(B.c, A.c), L));
    float o = ra + rb - d;
    if (o < best) best = o;
    return o > 0;
  };
  for (int i = 0; i < 3; ++i)
    if (!axis(A.ax[i])) return 0;
  for (int i = 0; i < 3; ++i)
    if (!axis(B.ax[i])) return 0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      const Vec3& a = A.ax[i];
      const Vec3& b = B.ax[j];
      if (!axis({a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x})) return 0;
    }
  return best;
}
float ObbSphereDepth(const Obb& A, const Vec3& c, float r) {
  Vec3 d = Sub(c, A.c);
  Vec3 q = A.c;
  float hs[3] = {A.h.x, A.h.y, A.h.z};
  for (int i = 0; i < 3; ++i) {
    float t = std::max(-hs[i], std::min(hs[i], Dot(d, A.ax[i])));
    q = Add(q, Scale(A.ax[i], t));
  }
  float dist = std::sqrt(Dist2(q, c));
  return r - dist;
}

struct Hit {
  Unit* u;       // a unit's hit, or null for a prop's
  float depth;
  Entity* prop = nullptr;
  Vec3 n{};      // (props: the contact normal handed to OnCollision)
};

// Sim::DoCollisionsFor 0x597cd0
void DoCollisionsFor(Sim& sim, Unit* self, const std::vector<Hit>& hits) {
  if (Naval(self)) return;  // naval units never push
  if (self->dead || self->destroyQueued) return;
  const MotionBlueprint& b = *self->motion.bp;
  float mSelf = Mass(b);
  float vPush = std::min(Len(UnitVelocity(self)), b.maxSpeed * 0.1f);
  UnitCommand* c = Current(self);
  // the position before its last move (u+0xd0), as stored: "position - last move" is not it to the bit, and not
  // at all after a step that was cut short (a falling unit's landing step, whose last move is the step over its
  // scale)
  Vec3 prev = self->lastPosTick == sim.tick() ? self->prevLastPosition : Sub(self->position, self->motion.lastMove);
  // u+0xd0 is the entity's transform before its last AdvanceCoords. A unit in a transport's beam is advanced a
  // second time each beat, by its task (CUnitCallTransport state 3), so there it is where the unit stood when
  // the last beat ended: last beat's beam position plus last beat's coast.
  if (self->unitStates.count("Teleporting") && self->lastMoveTick == sim.tick() && self->lastPosTick)
    prev = Add(self->prevLastPosition, self->motion.lastMove);
  static const long dbg = getenv("MOHO64_DEBUG_PUSH") ? atol(getenv("MOHO64_DEBUG_PUSH")) : -1;
  static const bool dbgProp = getenv("MOHO64_DEBUG_PROPHIT") != nullptr;
  lua_State* L = sim.L();
  for (const Hit& h : hits) {
    if (h.prop && dbgProp)
      Logf(LogLevel::Info, "prophit %u unit %u prop %u %s at %.3f %.3f n %.4f %.4f %.4f d %.4f", sim.tick(), self->id, h.prop->id,
           h.prop->blueprint ? h.prop->blueprint->id.c_str() : "?", h.prop->position.x, h.prop->position.z, h.n.x, h.n.y,
           h.n.z, h.depth);
    if (h.depth < 0.001f) continue;
    if (h.prop) {  // 0x597d8b: a prop (Entity vt slot 5) gets RunScript_OnCollision 0x598660, then is no unit
      if (!h.prop->HasLuaObject() || !self->HasLuaObject()) continue;
      PushObject(L, self);
      lua_pushnumber(L, h.n.x);
      lua_pushnumber(L, h.n.y);
      lua_pushnumber(L, h.n.z);
      lua_pushnumber(L, h.depth);
      sim.CallMethod(L, h.prop, "OnCollision", 5);
      continue;
    }
    Unit* o = h.u;
    if (UnitIgnores(self, o, 2)) continue;
    if (AirCat(o)) continue;
    if (c && Current(o) == c) return;  // one order: never pushed apart (ends the whole list)
    if (!Mobile(o)) continue;
    if (SameFormationLayer(self, o)) continue;
    // (o.pos is the other's committed position: where it stood when the beat began, also when it has moved)
    const Vec3& op = o->shapeHeld ? o->shapePos : o->position;
    Vec3 d{prev.x - op.x, 0, prev.z - op.z};
    if (d.x * d.x + d.z * d.z < 1e-6f) {
      d.x = static_cast<float>(sim.FRand(-1, 1));
      d.z = static_cast<float>(sim.FRand(-1, 1));
    }
    d = Norm(d);
    float mag = std::max(h.depth, 0.5f * std::max(b.sizeX, b.sizeZ)) + vPush;
    float mOther = Mass(*o->motion.bp);
    float fSelf = mSelf / (mOther + mSelf);
    float fOther = 1 - fSelf;
    auto flagOk = [](const Unit* a, const Unit* bb) { return FpFlags(a) == 0 || ((FpFlags(a) & 1) && (FpFlags(bb) & 1)); };
    if (dbg >= 0 && (static_cast<long>(self->id) == dbg || static_cast<long>(o->id) == dbg))
      Logf(LogLevel::Info, "push %u self %u other %u depth %.3f mag %.3f shares %.3f %.3f", sim.tick(), self->id, o->id,
           h.depth, mag, fOther, fSelf);
    if (fOther > 0.1f && flagOk(self, o) && !Immobile(self)) AddImpulse(self, Scale(d, mag * fOther));
    if (fSelf > 0.1f && flagOk(o, self) && !Immobile(o)) AddImpulse(o, Scale(d, -mag * fSelf));
  }
}

}  // namespace

namespace {
bool g_satDebug = false;
// BoxBox 0x474830 (prop_collision.md 3.2): A = prop, B = unit. True with the minimum-overlap face normal (pointing
// from the unit to the prop) and depth; an edge axis gives a zero normal and depth.
bool BoxBoxSat(const Obb& A, const Obb& B, Vec3* normal, float* depth) {
  auto dot = [](const Vec3& a, const Vec3& b) { return (a.y * b.y + a.z * b.z) + a.x * b.x; };
  auto cross = [](const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  auto project = [&](const Obb& o, const Vec3& a, float* lo, float* hi) {  // 0x475550
    float c = (o.c.y * a.y + o.c.z * a.z) + o.c.x * a.x;
    // x87 at precision control 24: each op rounds to float
    float rf = (std::fabs(dot(a, o.ax[2])) * o.h.z + std::fabs(dot(a, o.ax[1])) * o.h.y) +
               std::fabs(dot(a, o.ax[0])) * o.h.x;
    *lo = c - rf;
    *hi = c + rf;
  };
  Vec3 axes[15] = {A.ax[0], A.ax[2], A.ax[1], B.ax[0], B.ax[2], B.ax[1],
                   cross(A.ax[0], B.ax[0]), cross(A.ax[0], B.ax[2]), cross(A.ax[0], B.ax[1]),
                   cross(A.ax[2], B.ax[0]), cross(A.ax[2], B.ax[2]), cross(A.ax[2], B.ax[1]),
                   cross(A.ax[1], B.ax[0]), cross(A.ax[1], B.ax[2]), cross(A.ax[1], B.ax[1])};
  float ov[15];
  for (int i = 0; i < 15; ++i) {
    float a0, a1, b0, b1;
    project(A, axes[i], &a0, &a1);
    project(B, axes[i], &b0, &b1);
    if (b0 > a1 || a0 > b1) return false;
    ov[i] = std::min(a1, b1) - std::max(a0, b0);
  }
  float best = 1e9f;
  int bi = -1;
  for (int i = 0; i < 15; ++i) {
    const Vec3& ax = axes[i];
    float l2 = (ax.x * ax.x + ax.y * ax.y) + ax.z * ax.z;
    if (1e-6f > l2) continue;
    float inv = 1.0f / std::sqrt(l2);  // x87 at PC_24
    ov[i] = ov[i] * inv;
    axes[i] = {inv * ax.x, inv * ax.y, inv * ax.z};
    if (best > ov[i]) {
      best = ov[i];
      bi = i;
    }
  }
  if (g_satDebug) {  // MOHO64_DEBUG_COL: the fifteen normalised overlaps (0-5 faces, 6-14 edge pairs)
    std::string s;
    for (int i = 0; i < 15; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %d:%.5f", i, ov[i]);
      s += b;
    }
    Logf(LogLevel::Info, "sat best %d%s", bi, s.c_str());
  }
  if (bi < 0) return false;
  Vec3 n = axes[bi];
  if (((B.c.z - A.c.z) * n.z + (B.c.x - A.c.x) * n.x) + (B.c.y - A.c.y) * n.y > 0)
    n = {n.x * -1.0f, n.y * -1.0f, n.z * -1.0f};
  if (bi < 6) {
    *normal = n;
    *depth = best;
  } else {
    *normal = {};
    *depth = 0;
  }
  return true;
}

// The unit box's AABB as COGrid::CollectEntitiesInBox 0x721dc0 takes it (0x4fe2d0: the eight corners; 0xa3ae40: the
// centre and half size of their bounds; then centre -/+ half size).
void BoxAabb(const Obb& box, Vec3* mnOut, Vec3* mxOut) {
  Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  for (int i = 0; i < 8; ++i) {
    float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f, sz = (i & 4) ? 1.f : -1.f;
    Vec3 p = Add(Add(Add(box.c, Scale(box.ax[0], sx * box.h.x)), Scale(box.ax[1], sy * box.h.y)), Scale(box.ax[2], sz * box.h.z));
    mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
    mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
  }
  Vec3 c{(mx.x + mn.x) * 0.5f, (mx.y + mn.y) * 0.5f, (mx.z + mn.z) * 0.5f};
  Vec3 e{(mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f};
  *mnOut = Sub(c, e);
  *mxOut = Add(c, e);
}

// A prop in CollectEntitiesInBox: the AABB pre-test against the box's AABB (touching passes), then the primitive's
// CollideBox (a Box prop: BoxBox 0x474830, the normal pointing from the unit toward the prop).
bool PropHitTest(Entity* p, const Obb& box, const Vec3& mn, const Vec3& mx, Vec3* n, float* d) {
  WorldShape s;
  if (!GetWorldShape(p, &s) || s.type != ShapeType::Box) return false;  // (sphere props: not handled)
  Vec3 pmn, pmx;
  ShapeBounds(s, &pmn, &pmx);
  if (mn.x > pmx.x || pmn.x > mx.x || mn.y > pmx.y || pmn.y > mx.y || mn.z > pmx.z || pmn.z > mx.z) return false;
  Obb a;
  a.c = s.c;
  a.ax[0] = s.ax[0];
  a.ax[1] = s.ax[1];
  a.ax[2] = s.ax[2];
  a.h = s.half;
  *d = 0;
  return BoxBoxSat(a, box, n, d);
}
}  // namespace

void ProcessSurfaceCollision(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  m.contacts.clear();  // (0x6b904f: emptied before any of the early returns)
  if (u->layer == "Air" || u->layer == "Sub") return;
  if (u->dead || u->beingBuilt || u->parentId || St(u, "Attached")) return;
  if (!m.bp || m.bp->canFly) return;
  if (m.pushed) return;  // a pushed unit never pushes
  const MotionBlueprint& b = *m.bp;
  Obb box;
  box.c = u->position;
  box.ax[0] = Right(u->orientation);
  box.ax[1] = Up(u->orientation);
  box.ax[2] = Forward(u->orientation);
  box.h = {b.sizeX * 0.5f, b.sizeY, b.sizeZ * 0.5f};
  // every 5th tick (id % 5 == tick % 5) a unit larger than 0.2 also collects props, in the same list as the units
  // (mask 0x300, prop_collision.md 1.3): DoCollisionsFor goes through it in the grid's order, a prop's hit calls
  // prop:OnCollision there, and a mate under the same order ends the list, the props after it included (1.4).
  // (Read, not seen: no gate replay changes with it.)
  const bool withProps = b.sizeX * b.sizeZ > 0.2f && u->id % 5u == sim.tick() % 5u;
  std::vector<Hit> hits;
  // COGrid::CollectEntitiesInBox 0x721dc0: the units registered in the entity-grid cells under the box, in the
  // grid's order (rows, columns, the most recently registered first: rect_queries.md 2.4). The order decides who
  // is pushed which way when a unit overlaps two neighbours, and where a same-order mate ends the list.
  float hx = 0, hz = 0;
  {
    const float hs[3] = {box.h.x, box.h.y, box.h.z};
    for (int i = 0; i < 3; ++i) {
      hx += std::fabs(box.ax[i].x) * hs[i];
      hz += std::fabs(box.ax[i].z) * hs[i];
    }
  }
  std::vector<Entity*> cand;
  Vec3 amn, amx;
  if (withProps) {
    BoxAabb(box, &amn, &amx);
    sim.entityGrid().GatherBox(amn.x, amn.z, amx.x, amx.z, 3, &cand);
  } else {
    sim.entityGrid().GatherBox(box.c.x - hx, box.c.z - hz, box.c.x + hx, box.c.z + hz, 1, &cand);
  }
  auto visit = [&](Unit* e) {
    if (e == u) return;
    WorldShape s;
    if (!GetWorldShape(e, &s)) return;
    float depth = 0;
    bool meets = false;
    if (s.type == ShapeType::Box) {
      Obb o;
      o.c = s.c;
      o.ax[0] = s.ax[0];
      o.ax[1] = s.ax[1];
      o.ax[2] = s.ax[2];
      o.h = s.half;
      // the primitive's own test (0x4ff260 -> BoxBox 0x474830, the one the props use): the depth along the
      // shallowest FACE axis; when the shallowest of the fifteen is an edge-to-edge axis it reports depth 0, and
      // DoCollisionsFor skips a hit below 0.001: two hulls crossing corner to corner do not push each other
      Vec3 n;
      static const long dbgSat = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
      g_satDebug = dbgSat >= 0 && static_cast<long>(u->id) == dbgSat;
      meets = BoxBoxSat(o, box, &n, &depth);
      if (!meets) depth = 0;
      g_satDebug = false;
    } else if (s.type == ShapeType::Sphere) {
      depth = ObbSphereDepth(box, s.c, s.r);
      meets = depth >= 0;
    }
    // every entity whose primitive meets the box goes into the motion's contact list (0x6b9260: the whole result of
    // CollectEntitiesInBox, touching and edge-to-edge included), the raised-platform search's candidates
    if (meets) m.contacts.push_back(EntityRef(e));
    if (e->dead || e->destroyQueued) return;
    static const long dbgCol = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
    if (dbgCol >= 0 && (static_cast<long>(u->id) == dbgCol || static_cast<long>(e->id) == dbgCol))
      Logf(LogLevel::Info,
           "col %u overlap %u (%.4f %.4f %.4f fwd %.4f %.4f half %.3f %.3f %.3f) with %u (type %d c %.4f %.4f %.4f ax2 %.4f %.4f "
           "half %.3f %.3f %.3f r %.3f) depth %.5f",
           sim.tick(), u->id, box.c.x, box.c.y, box.c.z, box.ax[2].x, box.ax[2].z, box.h.x, box.h.y, box.h.z, e->id,
           static_cast<int>(s.type), s.c.x, s.c.y, s.c.z, s.ax[2].x, s.ax[2].z, s.half.x, s.half.y, s.half.z, s.r, depth);
    if (depth > 0) hits.push_back({e, depth});
  };
  for (Entity* e : cand) {
    if (!e) continue;
    if (e->kind == Entity::Kind::Unit) {
      visit(static_cast<Unit*>(e));
    } else if (withProps) {
      Vec3 n;
      float d = 0;
      if (PropHitTest(e, box, amn, amx, &n, &d)) hits.push_back({nullptr, d, e, n});
    }
  }
  if (!hits.empty()) DoCollisionsFor(sim, u, hits);
}

namespace {

// ---- steering avoidance ------------------------------------------------------------------------

// WillCollide 0x596930: boxes (SizeX+SizeZ)/2 wide, SizeZ + stop long, moved forward by stop.
bool WillCollide(const Unit* Y, const Unit* X, const Vec3& vY, const Vec3& pY, const Vec3& pX, const Vec3& vX,
                 bool sameForm) {
  struct B2 {
    float cx, cz, a0x, a0z, e0, a1x, a1z, e1;
  } box[2];
  const Unit* U[2] = {Y, X};
  const Vec3* V[2] = {&vY, &vX};
  const Vec3* P[2] = {&pY, &pX};
  float r[2], stop[2];
  for (int i = 0; i < 2; ++i) {
    const MotionBlueprint& b = *U[i]->motion.bp;
    float v = Len(*V[i]) * 10.0f;
    stop[i] = sameForm ? 0 : (b.maxAccel > 0 ? v * v / (2 * b.maxAccel) : 0);
    r[i] = b.sizeZ + stop[i];
  }
  if (Dist2(pY, pX) > (r[0] + r[1]) * (r[0] + r[1])) return false;
  for (int i = 0; i < 2; ++i) {
    const Quat& q = U[i]->orientation;
    const MotionBlueprint& b = *U[i]->motion.bp;
    float fx = 2 * (q.x * q.z + q.w * q.y), fz = 1 - 2 * (q.x * q.x + q.y * q.y);
    float rx = 1 - 2 * (q.y * q.y + q.z * q.z), rz = 2 * (q.x * q.z - q.w * q.y);
    box[i] = {P[i]->x + fx * stop[i], P[i]->z + fz * stop[i], rx, rz, (b.sizeX + b.sizeZ) * 0.25f, fx, fz, r[i] * 0.5f};
  }
  // Wm3 IntrBox2Box2::Test 0xa47d40 (box 0 = Y's, box 1 = X's, read 2026-10-15): the axes A0, A1, B0, B1 in that
  // order, and on a box's own axis its own extent counts whole. The axes here are the x and z parts of the unit's
  // 3-D right and forward, so on a slope they are shorter than 1 and not quite square to each other; a general
  // projection (|axis . own axis| x extent) shortens a box along its own axes there.
  const B2 &A = box[0], &B = box[1];
  const float dx = B.cx - A.cx, dz = B.cz - A.cz;
  const float c00 = std::fabs(B.a0x * A.a0x + B.a0z * A.a0z), c01 = std::fabs(B.a1z * A.a0z + B.a1x * A.a0x);
  const float c10 = std::fabs(A.a1z * B.a0z + A.a1x * B.a0x), c11 = std::fabs(B.a1z * A.a1z + B.a1x * A.a1x);
  float gap[4];  // (negative: the boxes overlap on that axis by that much)
  gap[0] = std::fabs(A.a0x * dx + dz * A.a0z) - ((B.e0 * c00 + A.e0) + B.e1 * c01);
  gap[1] = std::fabs(A.a1z * dz + A.a1x * dx) - ((B.e0 * c10 + A.e1) + B.e1 * c11);
  gap[2] = std::fabs(B.a0x * dx + B.a0z * dz) - ((A.e0 * c00 + B.e0) + A.e1 * c10);
  gap[3] = std::fabs(B.a1z * dz + B.a1x * dx) - ((A.e0 * c01 + B.e1) + A.e1 * c11);
  static const long dbgW = getenv("MOHO64_DEBUG_WILL") ? atol(getenv("MOHO64_DEBUG_WILL")) : -1;
  if (dbgW >= 0 && (static_cast<long>(Y->id) == dbgW || static_cast<long>(X->id) == dbgW))
    Logf(LogLevel::Info, "will Y %u X %u stop %.4f %.4f r %.4f %.4f centres %.4f %.4f / %.4f %.4f gaps %.5f %.5f %.5f %.5f", Y->id,
         X->id, stop[0], stop[1], r[0], r[1], box[0].cx, box[0].cz, box[1].cx, box[1].cz, gap[0], gap[1], gap[2], gap[3]);
  for (int i = 0; i < 4; ++i)
    if (gap[i] > 0) return false;
  return true;
}

// Approaching 0x596e00
bool Approaching(const Unit* A, const Unit* B, const Vec3& pA, const Vec3& vA, const Vec3& pB, const Vec3& vB) {
  bool sameForm = SameFormationLayer(A, B);
  if (!WillCollide(A, B, vA, pA, pB, vB, sameForm)) return false;
  Vec3 dv = Sub(vB, vA);
  if (Dot(dv, dv) <= 1e-6f) return false;
  return Dot(Sub(B->position, A->position), dv) < 0;
}

bool g_inSteeringStage = false;  // SteeringTickAll is running (see UnitMotion::colLateTick)
int g_navStageArmy = 0;          // the army whose navigators run (SteeringNavStageArmy), 0 outside that stage

// Predict 0x597800: X has the right of way, Y yields; the record goes to Y.
void Predict(Sim& sim, Unit* X, Unit* Y) {
  UnitMotion& mx = X->motion;
  UnitMotion& my = Y->motion;
  const bool splA = mx.hasSpline, splB = my.hasSpline;
  if (!splA && !splB) return;
  Vec3 pX = X->position, prevX = pX, pY = Y->position, prevY = pY, vX{}, vY{};
  const uint32_t now = sim.tick();
  if (my.colUnit == EntityRef(X)) {
    my.colType = 0;
    my.colUnit = 0;
    my.colPos = {};
    my.colTick = 0xffffffffu;
  }
  bool sameForm = SameFormationLayer(Y, X);
  for (size_t k = 0;; k += 3) {
    if (my.colType == 1 && my.colTick < now + k) return;
    if (splB) {
      size_t i = my.splineIdx + k;
      if (i >= my.spline.size()) return;
      pY = my.spline[i].pos;
      vY = k == 0 ? UnitVelocity(Y) : Sub(pY, prevY);
      prevY = pY;
    }
    if (splA) {
      size_t j = mx.splineIdx + k;
      if (j >= mx.spline.size()) return;
      pX = mx.spline[j].pos;
      vX = k == 0 ? UnitVelocity(X) : Sub(pX, prevX);
      prevX = pX;
    }
    if (WillCollide(Y, X, vY, pY, pX, vX, sameForm)) {
      float dPrev = my.colUnit ? Dist2(pY, my.colPos) : 9999.0f;
      if (Dist2(pY, pX) < dPrev) {
        my.colType = 1;
        my.colPos = pX;
        my.colUnit = EntityRef(X);
        my.colTick = now + static_cast<uint32_t>(k);
        // (written in the navigator stage: the original's steering of Y's army runs after it in this beat unless
        // Y's army comes before the navigator's)
        const int ya = Y->army ? Y->army->index : 0;
        my.colLateTick = g_inSteeringStage || (g_navStageArmy > 0 && ya >= g_navStageArmy) ? 0xffffffffu : now;
        static const long dbg = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
        if (dbg >= 0 && (static_cast<long>(X->id) == dbg || static_cast<long>(Y->id) == dbg))
          Logf(LogLevel::Info, "col %u predict Y %u (point %zu of %zu, made %llu) X %u (point %zu of %zu, made %llu) at +%zu", now,
               Y->id, my.splineIdx, my.spline.size(), static_cast<unsigned long long>(Y->entSeq), X->id, mx.splineIdx,
               mx.spline.size(), static_cast<unsigned long long>(X->entSeq), k);
        return;
      }
      // (0x597c7e: a meeting that is not nearer than the one already on record is passed over and the walk
      // along the two point lists goes on)
    }
  }
}

}  // namespace

namespace {

// CAiSteeringImpl::CheckCollisions 0x5d3740
void CheckCollisions(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (u->dead || u->beingBuilt || u->destroyQueued || u->layer == "Sub") return;
  const MotionBlueprint& b = *m.bp;
  float n = m.hasSpline ? static_cast<float>(m.spline.size()) : 0;
  float R = n * b.maxSpeed * 0.1f + (b.maxAccel > 0 ? b.maxSpeed * b.maxSpeed / (2 * b.maxAccel) : 0) +
            std::max(b.sizeX, b.sizeZ);
  m.colType = 0;
  m.colUnit = 0;
  m.colPos = {};
  m.colTick = 0xffffffffu;
  std::vector<Unit*> first, second;
  // The candidates come from the entity grid (ForAllEntitiesIterator 0x721fb0, the units' list) in the grid's
  // order: rows, then columns, in a cell the last registered first. The order decides which meeting is put on
  // record: a meeting that is no nearer than the one on record is passed over, so of two units the one asked
  // first can keep the record. Seen: a frigate given a new waypoint is predicted to meet one ship three beats
  // on and another ship NOW; asked in the grid's order the "now" one is on record first, comes due in the same
  // beat and the frigate brakes hard instead of taking the new course.
  std::vector<Entity*> gridUnits;
  auto visit = [&](Unit* e) {
    if (e == u) return;
    WorldShape s;
    if (!GetWorldShape(e, &s) || !SphereOverlap(s, u->position, R)) return;
    if (UnitIgnores(u, e, 2)) return;
    if (!HasSteering(e)) return;
    bool sp = e->motion.hasSpline;
    if (sp && e->motion.splineMode == 2) return;  // side-stepping: not seen
    if (e->army == u->army && !UnitPriority(e, u)) first.push_back(e);  // u has the right of way
    else if (sp || e->motion.bp->canFly) second.push_back(e);
  };
  if (sim.entityGrid().ready()) {
    sim.entityGrid().GatherBox(u->position.x - R, u->position.z - R, u->position.x + R, u->position.z + R, 1, &gridUnits);
    for (Entity* e : gridUnits)
      if (e->kind == Entity::Kind::Unit) visit(static_cast<Unit*>(e));
  } else {
    float q = R + 8.0f;
    sim.ForUnitsInRect(u->position.x - q, u->position.z - q, u->position.x + q, u->position.z + q, visit);
  }
  for (Unit* e : second) Predict(sim, e, u);  // u yields
  for (Unit* e : first) Predict(sim, u, e);   // e yields
}

void SetCol(Unit* u, int type, const Vec3& v) {
  u->motion.colType = type;
  u->motion.sideStep = v;
}

// ResolvePossibleCollision 0x596f30 for Y's record. Returns 1 when a reaction was chosen.
int Resolve(Sim& sim, Unit* Y) {
  UnitMotion& my = Y->motion;
  Unit* X = UnitByRef(sim, my.colUnit);
  if (!X || !HasSteering(Y)) return 0;  // the record stays; retried next tick
  auto clear = [&]() {
    SetCol(Y, 0, Y->position);
    return 0;
  };
  Vec3 pY = Y->position, pX = X->position;
  Vec3 vY = UnitVelocity(Y), vX = UnitVelocity(X);  // vtbl+0x3c
  bool Ymoving = Dot(vY, vY) > 0;
  if (X->motion.bp->canFly) return clear();  // (an air-capable blocker would lift off: not modelled)
  if (my.hasSpline && my.splineIdx < my.spline.size()) pY = my.spline[my.splineIdx].pos;
  const UnitMotion& mx = X->motion;
  if (mx.hasSpline && mx.splineIdx < mx.spline.size()) pX = mx.spline[mx.splineIdx].pos;
  if (Len(vX) <= 0) return clear();
  {
    static const long dbgR = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
    if (dbgR >= 0 && (static_cast<long>(X->id) == dbgR || static_cast<long>(Y->id) == dbgR)) {
      const bool sf = SameFormationLayer(Y, X);
      const Vec3 dv = Sub(vX, vY);
      Logf(LogLevel::Info,
           "col %u resolve? Y %u X %u pY %.4f %.4f %.4f (idx %zu of %zu) pX %.4f %.4f %.4f (idx %zu of %zu) vY %.5f %.5f %.5f vX %.5f %.5f "
           "%.5f will %d dv2 %.6f dot %.5f",
           sim.tick(), Y->id, X->id, pY.x, pY.y, pY.z, my.splineIdx, my.spline.size(), pX.x, pX.y, pX.z, mx.splineIdx,
           mx.spline.size(), vY.x, vY.y, vY.z, vX.x, vX.y, vX.z, WillCollide(Y, X, vY, pY, pX, vX, sf) ? 1 : 0, Dot(dv, dv),
           Dot(Sub(X->position, Y->position), dv));
    }
  }
  if (!Approaching(Y, X, pY, vY, pX, vX)) return clear();
  const MotionBlueprint& by = *Y->motion.bp;
  const MotionBlueprint& bx = *X->motion.bp;
  float clr = std::max(by.sizeX, by.sizeZ) + std::max(bx.sizeX, bx.sizeZ) + 0.5f;
  bool sfl = SameFormationLayer(Y, X);
  Vec3 fwdY = Forward(Y->orientation);
  Vec3 dYX = Norm(Sub(Y->position, X->position));
  Vec3 vXn = Norm(vX);
  static const long dbg = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
  bool log = dbg >= 0 && (static_cast<long>(X->id) == dbg || static_cast<long>(Y->id) == dbg);
  if (Dot(dYX, vXn) <= 0.707f) {  // Y is not in X's 45-degree forward cone: Y brakes and waits
    SetCol(Y, 4, Y->position);
    if (log) Logf(LogLevel::Info, "col %u resolve Y %u X %u: brake", sim.tick(), Y->id, X->id);
    return 1;
  }
  if (HasSteering(X) && !sfl) {
    bool ok = Naval(X) ? Naval(Y) : (!(FpFlags(X) & 1) || (FpFlags(Y) & 1));
    if (ok) SetCol(X, 5, X->position);  // X brakes hard
  }
  bool facing = Dot(dYX, fwdY) < 0;  // Y points toward X
  Vec3 B, s;
  if (Ymoving && facing) {
    Vec3 vYn = Norm(vY);
    Vec3 rX = Right(X->orientation);
    if (Dot(rX, dYX) <= 0) rX = Scale(rX, -1);
    float c = Dot(vYn, vXn);
    if (c > 0.707f) {
      B = pY;
      s = Norm(Add(dYX, rX));
    } else if (c < -0.707f) {
      B = X->position;
      s = rX;
    } else {
      B = pY;
      s = Norm(Add(vYn, dYX));
    }
  } else {
    Vec3 d = Sub(Y->position, X->position);
    float cy = d.z * vXn.x - d.x * vXn.z;
    float a = cy > 0 ? (sfl ? -0.392699f : -0.785398f) : (sfl ? 0.392699f : 0.785398f);
    B = pY;
    s = RotY(a, vXn);
  }
  Vec3 T = Add(B, Scale(s, clr));
  // FindFreeSpot 0x62b200 gates the side-step (outline): the point inside the playable rect (0x59778e passes size
  // 1, whole map 0: STIMap::IsWithin margin 1, whatever the army), and the footprint must fit there
  if (sim.map() && !sim.IsWithin(T.x, T.z, 1.0f, false)) return 0;
  const PathGrid* g = UnitFootprintGrid(sim, Y);
  if (g) {
    int cx, cz;
    GoalCell(UnitFootprint(Y), T.x, T.z, &cx, &cz);
    if (!g->Passable(cx, cz)) return 0;
  }
  SetCol(Y, 2, T);
  if (log)
    Logf(LogLevel::Info, "col %u resolve Y %u X %u: side-step to %.2f %.2f", sim.tick(), Y->id, X->id, T.x, T.z);
  return 1;
}

// CAiSteeringImpl::ProcessSplineMovement 0x5d2c00 (and DriveToNextWaypoint 0x5d3000 for a new waypoint)
void ProcessSplineMovement(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (m.pushed) {
    if (m.hasSpline) {  // "pushed!": Stop (vt+0x2c, 0x5d35e0)
      // Stop re-predicts (CheckCollisions, with the spline already gone) when the deleted batch still had
      // unhanded points: idx(+0x240) < points.size (0x5d35ee..0x5d3666)
      const bool unused = m.splineIdx < m.spline.size();
      SteeringStop(m);
      if (unused) CheckCollisions(sim, u);
    }
    float v = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
    if (v < m.bp->maxSpeed * m.speedMult * 0.01f) {
      m.pushed = false;
      if (m.wasMoving) {
        m.wasMoving = false;
        LandNavPoke(u);
        // 0x5d2cf4..0x5d2d09: the fresh batch is made only when the target st+0x90 is not the no-target vector
        // (0x4ead40, compared by 0x4f0a50); the navigator is not asked. SetWaypoints(0, 0) writes the no-target
        // vector, and a new waypoint is taken up only by DriveToNextWaypoint, after this. Seen: Skirmish 2358-2361,
        // engineer 0x50001a pushed while its Move was replaced by a Guard: the push ends at 2360 with the new path
        // laid but not yet taken up; the original makes no batch and turns toward the new path at 2361, ours
        // made one toward the old Move's target (446.5, 850.5) and turned 18 degrees the wrong way first.
        if (m.steerValid) {
          UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
          CheckCollisions(sim, u);
        }
      }
    }
  } else if (m.hasSpline && m.splineIdx + 1 >= m.spline.size()) {  // the batch is used up (one point left)
    // 0x5d2d88: "is there a target" is st+0x90 against the no-target vector. A unit whose waypoints were cleared
    // (a brake spline) and that has been handed a new waypoint since has none here yet: the brake ends in a Stop,
    // and DriveToNextWaypoint then takes the new waypoint up. (Seen: a ship stopped by a new goal and handed the
    // cell it stands in; measured against its waypoint of before, it took a continuing spline and rolled on.)
    const bool driving = Driving(u) && m.steerValid;
    if (!driving || AtPosition(u, m.steerTarget)) {  // arrived at this waypoint
      // Stop(0) (vt+0x2c, 0x5d2dbf -> 0x5d35e0), as in the pushed branch: the batch still has its last point
      // (idx < points.size, 0x5d360e), so Stop re-predicts with the spline gone (CheckCollisions, 0x5d3666), as
      // the navigator's Stop does when a path ends (MotionNavDone). (Read in the exe; no replay seen to need it.)
      // Stop(0) then return 1 (0x5d2dc9); OnTick 0x5d32fa only moves the waypoint index on (index == count:
      // nothing more to take). Neither touches the target st+0x90: only SetWaypoints(0, 0) (0x5d2abb) writes the
      // no-target vector. So the unit keeps its target after it arrived, and a collision record resolved to
      // nothing (case 0, 0x5d2f29: "target valid and no points left") still makes it a fresh batch toward that
      // target. Ours dropped the waypoint here. (Seen: LAND, a heavy artillery (ual0304) had stopped 0.012 short
      // of its waypoint, the cell it stands in, while its navigator went on thinking; a neighbour's record
      // resolved to nothing at 1614 and again at 1618, and each time the original took one step onto that point
      // and coasted back; ours stood still.)
      const bool unused = m.splineIdx < m.spline.size();
      SteeringStop(m);
      if (unused) CheckCollisions(sim, u);
      return;
    }
    UpdatePath(sim, u, m.steerTarget, false, GetVal(m));
    CheckCollisions(sim, u);
  }
  // 0x5d2d24..0x5d2e4b: a unit in the Immobile state, or a stunned one, loses its spline once (Stop, and the
  // byte steering+0xa0 remembers it) and gets no point while that lasts. When it is let go, a unit that is
  // Moving, Patrolling or Attacking gets a fresh batch toward the same target, from where it stands now.
  const bool held = u->immobile || u->unitStates.count("Immobile") > 0 || u->stunned;
  if (!m.heldLatch) {
    if (held) {
      m.heldLatch = true;
      const bool unused = m.hasSpline && m.splineIdx < m.spline.size();
      SteeringStop(m);
      if (unused) CheckCollisions(sim, u);
      return;
    }
  } else {
    if (held) return;
    if (Driving(u) && (u->unitStates.count("Moving") > 0 || u->unitStates.count("Patrolling") > 0 ||
                       u->unitStates.count("Attacking") > 0)) {
      UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
      CheckCollisions(sim, u);
    }
    m.heldLatch = false;
  }
  // the collision record
  if (m.colType == 5) {
    // (0x5d2e62: no test of the target here, nor in case 4 below: a unit that has been told to stop and has no
    // waypoint left still takes the hard brake when a collision record waits for it. Seen: one unit of thirteen
    // stopped by one order braked at twice the rate from the beat after it, the one with a neighbour ahead.)
    UpdatePath(sim, u, m.steerTarget, true, 4);
  } else if (m.colType == 1 && sim.tick() >= m.colTick && m.colLateTick != sim.tick()) {
    Resolve(sim, u);
    switch (m.colType) {
      case 0:
        if (Driving(u) && (!m.hasSpline || m.splineIdx >= m.spline.size()))
          UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
        if (Driving(u)) CheckCollisions(sim, u);
        break;
      case 2: {
        Vec3 T = m.sideStep;
        m.sideStep = {};
        UpdatePath(sim, u, T, true, 2);
        break;
      }
      case 4:
        UpdatePath(sim, u, m.steerTarget, true, 4);  // (0x5d2f12: whatever the target)
        break;
      default:
        break;
    }
  }
  if (m.hasSpline && m.splineIdx < m.spline.size()) {
    m.point = m.spline[m.splineIdx++];
    m.pointNow = true;
  }
}

// CAiSteeringImpl::OnTick / DriveToNextWaypoint 0x5d3000: the current spline first, then a new waypoint
// (a fresh batch, whose first point replaces the one just handed out; none when the unit already stands in
// the waypoint's cell).
void SteeringTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (m.handOutTick == sim.tick()) return;  // SetWaypoints(0, 0) already handed this beat's point
  m.pointNow = false;
  if (!HasSteering(u) || u->dead || u->parentId) return;
  ProcessSplineMovement(sim, u);
  if (!m.newSegment) return;
  m.newSegment = false;
  if (!Driving(u)) return;
  m.steerTarget = m.path[m.pathIndex];
  m.steerValid = true;
  if (AtPosition(u, m.steerTarget)) return;
  UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
  CheckCollisions(sim, u);
  ProcessSplineMovement(sim, u);
}

}  // namespace

// (for SetWaypoints(0, 0) in motion.cpp: the prediction that follows the brake spline)
void SteeringCheckCollisions(Sim& sim, Unit* u) {
  if (HasSteering(u)) CheckCollisions(sim, u);
}

void SteeringNavStageArmy(int army) { g_navStageArmy = army; }

void SteeringTickAll(Sim& sim) {
  const std::vector<Unit*> all = sim.ArmyStageOrder();  // (brain+0xac: creation order inside each army)
  g_inSteeringStage = true;
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    if (u->destroyQueued) continue;
    SteeringTick(sim, u);
  }
  g_inSteeringStage = false;
}

}  // namespace moho
