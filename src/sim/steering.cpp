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
int FpFlags(const Unit* u) { return u->motion.bp ? u->motion.bp->footprint.flags : 0; }
float FpMax(const Unit* u) {
  const MotionBlueprint* b = u->motion.bp;
  return b ? static_cast<float>(std::max(b->footprint.sizeX, b->footprint.sizeZ)) : 1.0f;
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
  Unit* u;
  float depth;
};

// Sim::DoCollisionsFor 0x597cd0
void DoCollisionsFor(Sim& sim, Unit* self, const std::vector<Hit>& hits) {
  if (Naval(self)) return;  // naval units never push
  if (self->dead || self->destroyQueued) return;
  const MotionBlueprint& b = *self->motion.bp;
  float mSelf = Mass(b);
  float vPush = std::min(Len(self->motion.lastMove), b.maxSpeed * 0.1f);
  UnitCommand* c = Current(self);
  Vec3 prev = Sub(self->position, self->motion.lastMove);  // the position before its last move (u+0xd0)
  static const long dbg = getenv("MOHO64_DEBUG_PUSH") ? atol(getenv("MOHO64_DEBUG_PUSH")) : -1;
  for (const Hit& h : hits) {
    if (h.depth < 0.001f) continue;
    Unit* o = h.u;
    if (UnitIgnores(self, o, 2)) continue;
    if (AirCat(o)) continue;
    if (c && Current(o) == c) return;  // one order: never pushed apart (ends the whole list)
    if (!Mobile(o)) continue;
    if (SameFormationLayer(self, o)) continue;
    Vec3 d{prev.x - o->position.x, 0, prev.z - o->position.z};
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

void ProcessSurfaceCollision(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
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
  // (every 5th tick a unit larger than 0.2 also knocks props: prop:OnCollision, not modelled)
  std::vector<Hit> hits;
  float r = std::max(b.sizeX, b.sizeZ) + 8.0f;
  sim.ForUnitsInRect(u->position.x - r, u->position.z - r, u->position.x + r, u->position.z + r, [&](Unit* e) {
    if (e == u || e->dead || e->destroyQueued) return;
    WorldShape s;
    if (!GetWorldShape(e, &s)) return;
    float depth = 0;
    if (s.type == ShapeType::Box) {
      Obb o;
      o.c = s.c;
      o.ax[0] = s.ax[0];
      o.ax[1] = s.ax[1];
      o.ax[2] = s.ax[2];
      o.h = s.half;
      depth = ObbDepth(box, o);
    } else if (s.type == ShapeType::Sphere) {
      depth = ObbSphereDepth(box, s.c, s.r);
    }
    if (depth > 0) hits.push_back({e, depth});
  });
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
  // separating axes of two 2-D boxes
  float dx = box[1].cx - box[0].cx, dz = box[1].cz - box[0].cz;
  auto sep = [&](float ax, float az) {
    float ra = std::fabs(box[0].a0x * ax + box[0].a0z * az) * box[0].e0 + std::fabs(box[0].a1x * ax + box[0].a1z * az) * box[0].e1;
    float rb = std::fabs(box[1].a0x * ax + box[1].a0z * az) * box[1].e0 + std::fabs(box[1].a1x * ax + box[1].a1z * az) * box[1].e1;
    return std::fabs(dx * ax + dz * az) > ra + rb;
  };
  for (int i = 0; i < 2; ++i) {
    if (sep(box[i].a0x, box[i].a0z)) return false;
    if (sep(box[i].a1x, box[i].a1z)) return false;
  }
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
      vY = k == 0 ? Y->motion.lastMove : Sub(pY, prevY);
      prevY = pY;
    }
    if (splA) {
      size_t j = mx.splineIdx + k;
      if (j >= mx.spline.size()) return;
      pX = mx.spline[j].pos;
      vX = k == 0 ? X->motion.lastMove : Sub(pX, prevX);
      prevX = pX;
    }
    if (WillCollide(Y, X, vY, pY, pX, vX, sameForm)) {
      float dPrev = my.colUnit ? Dist2(pY, my.colPos) : 9999.0f;
      if (Dist2(pY, pX) < dPrev) {
        my.colType = 1;
        my.colPos = pX;
        my.colUnit = EntityRef(X);
        my.colTick = now + static_cast<uint32_t>(k);
        static const long dbg = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
        if (dbg >= 0 && (static_cast<long>(X->id) == dbg || static_cast<long>(Y->id) == dbg))
          Logf(LogLevel::Info, "col %u predict Y %u X %u at +%zu", now, Y->id, X->id, k);
      }
      return;
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
  float q = R + 8.0f;
  sim.ForUnitsInRect(u->position.x - q, u->position.z - q, u->position.x + q, u->position.z + q, [&](Unit* e) {
    if (e == u) return;
    WorldShape s;
    if (!GetWorldShape(e, &s) || !SphereOverlap(s, u->position, R)) return;
    if (UnitIgnores(u, e, 2)) return;
    if (!HasSteering(e)) return;
    bool sp = e->motion.hasSpline;
    if (sp && e->motion.splineMode == 2) return;  // side-stepping: not seen
    if (e->army == u->army && !UnitPriority(e, u)) first.push_back(e);  // u has the right of way
    else if (sp || e->motion.bp->canFly) second.push_back(e);
  });
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
  Vec3 vY = Y->motion.lastMove, vX = X->motion.lastMove;
  bool Ymoving = Dot(vY, vY) > 0;
  if (X->motion.bp->canFly) return clear();  // (an air-capable blocker would lift off: not modelled)
  if (my.hasSpline && my.splineIdx < my.spline.size()) pY = my.spline[my.splineIdx].pos;
  const UnitMotion& mx = X->motion;
  if (mx.hasSpline && mx.splineIdx < mx.spline.size()) pX = mx.spline[mx.splineIdx].pos;
  if (Len(vX) <= 0) return clear();
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
  // FindFreeSpot 0x62b200 gates the side-step (outline): the footprint must fit there
  const PathGrid* g = FootprintGrid(sim, by);
  if (g) {
    int cx, cz;
    GoalCell(by, T.x, T.z, &cx, &cz);
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
    if (m.hasSpline) SteeringStop(m);
    float v = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
    if (v < m.bp->maxSpeed * m.speedMult * 0.01f) {
      m.pushed = false;
      if (m.wasMoving) {
        m.wasMoving = false;
        LandNavPoke(u);
        if (Driving(u)) {
          UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
          CheckCollisions(sim, u);
        }
      }
    }
  } else if (m.hasSpline && m.splineIdx + 1 >= m.spline.size()) {  // the batch is used up (one point left)
    if (!Driving(u) || AtPosition(u, m.steerTarget)) {  // arrived at this waypoint
      SteeringStop(m);
      if (Driving(u) && !m.newSegment) m.hasWaypoint = false;
      return;
    }
    UpdatePath(sim, u, m.steerTarget, false, GetVal(m));
    CheckCollisions(sim, u);
  }
  // the collision record
  if (m.colType == 5) {
    if (Driving(u)) UpdatePath(sim, u, m.steerTarget, true, 4);
    else m.colType = 0;
  } else if (m.colType == 1 && sim.tick() >= m.colTick) {
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
        if (Driving(u)) UpdatePath(sim, u, m.steerTarget, true, 4);
        else m.colType = 0;
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
  m.pointNow = false;
  if (!HasSteering(u) || u->dead || u->parentId) return;
  ProcessSplineMovement(sim, u);
  if (!m.newSegment) return;
  m.newSegment = false;
  if (!Driving(u)) return;
  m.steerTarget = m.path[m.pathIndex];
  if (AtPosition(u, m.steerTarget)) return;
  UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
  CheckCollisions(sim, u);
  ProcessSplineMovement(sim, u);
}

}  // namespace

void SteeringTickAll(Sim& sim) {
  const auto& all = sim.units();
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    if (u->destroyQueued) continue;
    SteeringTick(sim, u);
  }
}

}  // namespace moho
