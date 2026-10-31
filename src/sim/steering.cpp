// Land steering between units (engine-ref unit_collision.md): contact pushes, the right of way,
// predicted collisions and the reactions to them; and the steering's per-tick spline handling
// (land_motion_blocking.md 2.4).
#include "sim/steering.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/speculate.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "script/script_state.h"
#include "sim/air.h"
#include "sim/build.h"
#include "sim/navigation.h"
#include "sim/combat.h"
#include "sim/formation.h"
#include "sim/landnav.h"
#include "sim/motion.h"
#include "sim/sim.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {
namespace {

bool St(const Unit* u, const char* s) { return u->unitStates.count(s) != 0; }
bool St(const Unit* u, int i) { return u->unitStates.Has(i); }  // (i: US("Name"))

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
bool Immobile(const Unit* u) { return u->immobile || St(u, US("Immobile")); }
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
// The four turns ResolvePossibleCollision applies (0x5976f3..0x597713): quaternions (cos a, 0, sin a, 0), each a
// rotation by 2a about +y, made once by static initialisers (0xbcbbd0, 0xbcbc30, 0xbcbc90, 0xbcbcf0) from the float
// angles -pi/4, pi/4, -pi/8, pi/8 (0xe4fa28, 0xe4f8b0, 0xe4fa2c, 0xe4f970: 0x3f490fdb / 0x3ec90fdb with their signs)
// with x87 fcos / fsin (0x5734d0 / 0x5734e0) stored as floats. Ours had worked them out from 0.785398f / 0.392699f,
// three float steps short of those angles, so its sines and cosines were two steps off. Seen: MP3 3528, army 2's
// hover engineer 0x200020 side-steps (Generate mode 2); its target's x was one float step from the original's
// (0x423cf829 / 0x423cf82a), the batch's facings 1e-6 rad apart, and the unit (army 0's 0x70 after a transfer) a
// float step off from then on, which at 4916 made the navigator of 0x72 see its cell taken.
struct TurnY {
  float w, y;
};
constexpr TurnY kTurnMinusQuarterPi{0x1.6a09e6p-1f, -0x1.6a09e6p-1f};  // 0x10ae1cc (cos 0x3f3504f3, sin 0xbf3504f3)
constexpr TurnY kTurnQuarterPi{0x1.6a09e6p-1f, 0x1.6a09e6p-1f};        // 0x10ae278
constexpr TurnY kTurnMinusEighthPi{0x1.d906bcp-1f, -0x1.87de2cp-2f};   // 0x10ae1dc (cos 0x3f6c835e, sin 0xbec3ef16)
constexpr TurnY kTurnEighthPi{0x1.d906bcp-1f, 0x1.87de2cp-2f};         // 0x10ae288
// 0x452d40: the quaternion as a matrix (0x452fd0), then each row times v (v.x m0 + v.y m1 + v.z m2): for (w, 0, y, 0)
// the terms below, the zero ones dropped
Vec3 RotY(const TurnY& q, const Vec3& v) {
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
  // each direction normalised with its squares summed z, y, x, times 1 / length (0x62ec20..0x62ec8f,
  // 0x62ecef..0x62ed84; zero for no length); the dots summed y, z, x and z, y, x (0x62ed8f..0x62edd4)
  auto unit = [](Vec3 d) {
    const float l = std::sqrt((d.z * d.z + d.y * d.y) + d.x * d.x);
    if (!(l > 0.0f)) return Vec3{};
    const float i = 1.0f / l;
    return Vec3{i * d.x, d.y * i, d.z * i};
  };
  const Vec3 u1 = unit(Sub(B->position, A->position)), u2 = unit(Sub(A->position, B->position));
  float a = (u1.y * fa.y + u1.z * fa.z) + u1.x * fa.x;
  float b = (u2.z * fb.z + u2.y * fb.y) + u2.x * fb.x;
  if (!(0.0f < a) && !(0.0f < b)) return nullptr;
  return a > b ? B : A;
}

}  // namespace

// The quaternion SweptPathBlockedByUnit 0x7216d0 turns its line's direction with (0x721785: ecx = 0x10b965c, then the
// matrix product 0x452d40): (cos a, 0, sin a, 0) made by the static initialiser 0xbda970 from the float pi/4 (0xe4f8b0)
// with x87 fcos / fsin, the same numbers as kTurnQuarterPi (0x10ae278). Through the matrix, 1 - 2y^2 is not quite 0
// and 2wy not quite 1, so the side axis is a float step off (fz, 0, -fx) now and then.
Vec3 RotateQuarterTurnY(const Vec3& v) { return RotY(kTurnQuarterPi, v); }

bool SameFormationLayer(const Unit* a, const Unit* b) {
  if (St(a, US("Attacking")) || St(b, US("Attacking"))) return false;
  return a->form && a->form == b->form;
}

bool UnitPriority(const Unit* A, const Unit* B) {
  if (Immobile(A) || St(A, US("Upgrading"))) return true;
  if (Immobile(B) || St(B, US("Upgrading"))) return false;
  bool na = Naval(A), nb = Naval(B);
  if (na != nb) return na;
  bool fa = (FpFlags(A) & 1) != 0, fb = (FpFlags(B) & 1) != 0;
  if (fa != fb) return fa;
  if (A->motion.bp->canFly && A->layer != "Air") return true;  // landed air-capable unit
  if (B->motion.bp->canFly && B->layer != "Air") return false;
  if (St(A, US("WaitingForTransport")) && !St(B, US("WaitingForTransport"))) return true;
  if (A->guardedId && A->guardedId == EntityRef(B)) return false;  // the guarded unit has priority
  if (B->guardedId && B->guardedId == EntityRef(A)) return true;
  bool same = A->form && A->form == B->form;
  if (same && A->formLeader) {
    if (A->formLeader == EntityRef(A)) return true;
    if (A->formLeader == EntityRef(B)) return false;
  }
  bool am = St(A, US("Moving")), bm = St(B, US("Moving"));
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
  if (flags == 1 && !vm::SameBits(e->position, e->lastPosition))
    return true;  // it moved this tick (func_IsSourceUnit 0x62eee9..0x62eefd: memcmp 0x4f0a50 of +0xb4 / +0xd0)
  if (e->parentId || St(e, US("Attached"))) return true;
  if (self->layer != e->layer) return true;
  if (Naval(self) && !Naval(e)) return true;
  if (AirCat(e) && (e->layer == "Air" || e->transport)) return true;
  if ((FpFlags(self) & 1) && FpFlags(e) == 0) return true;
  if (St(self, US("WaitingForTransport")) && St(e, US("WaitingForTransport")) && self->focusId == e->focusId) return true;
  if (St(self, US("Upgrading")) && e->creatorId && e->creatorId == EntityRef(self)) return true;
  if (flags == 2) return false;
  if (AirCat(e) && self->army == e->army) return true;
  if (!St(self, US("WaitingForTransport")) && St(e, US("WaitingForTransport"))) return false;
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

using Hit = SurfaceHit;  // (steering.h)

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
  if (self->unitStates.Has(US("Teleporting")) && self->lastMoveTick == sim.tick() && self->lastPosTick)
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
    // The impulses as 0x5980ba..0x5980f2 and 0x598171..0x5981b6 form them: each component times mag, then times the
    // share ((d x mag) x fOther; ((0 - d) x mag) x fSelf), not times the one product mag x share: the two orders differ
    // in the last bit. Seen: MP2 10053, army 1's engineer 0x10012d pushes its T2 engineer 0x100132: the push's last bit
    // survived the coast's cancellation (0.8 v less the brake leaves 1/25 of v at 10055), so the side-step that
    // 0x100132's collision record made at 10056 aimed a hair off; at 10086 its spline passed the side-step point
    // dead astern and turned the other way round (the original to +x, ours to -x).
    if (fOther > 0.1f && flagOk(self, o) && !Immobile(self))
      AddImpulse(self, Vec3{d.x * mag * fOther, d.y * mag * fOther, d.z * mag * fOther});
    if (fSelf > 0.1f && flagOk(o, self) && !Immobile(o))
      // (-0.0 - d: 0x598171 loads -0.0 from 0xe4f748, so a +0 part of d gives -0 here)
      AddImpulse(o, Vec3{(-0.0f - d.x) * mag * fSelf, (-0.0f - d.y) * mag * fSelf, (-0.0f - d.z) * mag * fSelf});
  }
}

}  // namespace

namespace {
// MOHO64_DEBUG_COL: the unit whose collisions are tested, read by BoxBox. Per thread (multicore step 1), as
// g_bodyDebug.
thread_local bool g_satDebug = false;
// BoxBoxSat's x87 step at precision control 24 (below): v's 53-bit significand rounded to 24 bits, to nearest, ties to
// even, the exponent kept as it is (no float range: a tiny product stays whole). What frexp, the float conversion of
// the fraction and ldexp give, without their calls: the bits below the 24 kept ones (29 of them) are rounded away by an
// add (half of them, less one, plus the last kept bit: ties go to even) that carries into the kept bits and, when they
// were all ones, into the exponent (the next power of two, as the float conversion of 0.11..1 gives 1.0). Both zeros
// give +0 (as the frexp version's early return), infinities and NaNs come back as they are; a double subnormal (never
// made here: the inputs are float products and sums) takes the frexp version. Speed only (B1 2026-10-30: the two
// library calls were 2.2% of the beat time, B3-16k 3.7%).
double X24Frexp(double v) {
  if (v == 0.0) return 0.0;
  int e;
  const double m = std::frexp(v, &e);
  return std::ldexp(static_cast<double>(static_cast<float>(m)), e);
}
double X24Bits(double v) {
  if (v == 0.0) return 0.0;
  uint64_t b;
  std::memcpy(&b, &v, sizeof b);
  const uint64_t ex = b & 0x7FF0000000000000ull;
  if (ex == 0x7FF0000000000000ull) return v;
  if (ex == 0) return X24Frexp(v);
  b = (b + 0x0FFFFFFFull + ((b >> 29) & 1u)) & ~uint64_t{0x1FFFFFFF};
  std::memcpy(&v, &b, sizeof v);
  return v;
}
// MOHO64_VERIFY_X24=1: every x24 is also made the frexp way and compared bit for bit; the counts go to stderr at exit
// (and the first mismatches as they come). The bit version's answer is the one used.
struct X24Verify {
  const bool on = std::getenv("MOHO64_VERIFY_X24") != nullptr;
  std::atomic<uint64_t> checked{0}, mismatches{0};
  ~X24Verify() {
    if (on)
      std::fprintf(stderr, "moho64 verify x24: checked %llu, mismatches %llu\n",
                   static_cast<unsigned long long>(checked.load()), static_cast<unsigned long long>(mismatches.load()));
  }
};
X24Verify g_x24Verify;
double X24(double v) {
  const double r = X24Bits(v);
  if (g_x24Verify.on) [[unlikely]] {
    g_x24Verify.checked.fetch_add(1, std::memory_order_relaxed);
    const double f = X24Frexp(v);
    if (std::memcmp(&f, &r, sizeof r) != 0 && g_x24Verify.mismatches.fetch_add(1, std::memory_order_relaxed) < 20)
      std::fprintf(stderr, "moho64 verify x24: MISMATCH %a: bits %a, frexp %a\n", v, r, f);
  }
  return r;
}
// BoxBox 0x474830 (prop_collision.md 3.2): A = prop, B = unit. True with the minimum-overlap face normal (pointing
// from the unit to the prop) and depth; an edge axis gives a zero normal and depth.
bool BoxBoxSat(const Obb& A, const Obb& B, Vec3* normal, float* depth) {
  auto dot = [](const Vec3& a, const Vec3& b) { return (a.y * b.y + a.z * b.z) + a.x * b.x; };
  auto cross = [](const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  // The radius is made on the x87 (0x47555e..0x475602): at precision control 24 every step keeps 24 significant
  // bits, but the x87's exponent range keeps a tiny product whole where a float would round it to a denormal; only
  // the fstp at the end makes a float of it. An axis whose components are denormals (the cross product of two
  // nearly level up axes) is projected that way.
  auto x24 = [](double v) { return X24(v); };  // (above: the x87's 24 bits)
  auto xdot = [&](const Vec3& v, const Vec3& a) {  // (z + y) + x, each product and sum rounded as the x87 does
    const double z = x24(static_cast<double>(v.z) * a.z), y = x24(static_cast<double>(v.y) * a.y);
    return x24(x24(z + y) + x24(static_cast<double>(v.x) * a.x));
  };
  auto project = [&](const Obb& o, const Vec3& a, float* lo, float* hi) {  // 0x475550
    float c = (o.c.y * a.y + o.c.z * a.z) + o.c.x * a.x;
    const double t2 = x24(std::fabs(xdot(o.ax[2], a)) * o.h.z), t1 = x24(std::fabs(xdot(o.ax[1], a)) * o.h.y);
    const double t0 = x24(std::fabs(xdot(o.ax[0], a)) * o.h.x);
    float rf = static_cast<float>(x24(x24(t2 + t1) + t0));
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

}  // namespace

// CColPrimitive_Box::CollideBox 0x4ff260 -> BoxBox 0x474830 for an entity's box against an oriented query box, as
// CollectEntitiesInBox 0x721dc0 asks it (touching counts).
bool BoxPrimitiveMeetsBox(const WorldShape& s, const Vec3& centre, const Vec3 axes[3], const float half[3]) {
  Obb a;
  a.c = s.c;
  a.ax[0] = s.ax[0];
  a.ax[1] = s.ax[1];
  a.ax[2] = s.ax[2];
  a.h = s.half;
  Obb b;
  b.c = centre;
  b.ax[0] = axes[0];
  b.ax[1] = axes[1];
  b.ax[2] = axes[2];
  b.h = Vec3{half[0], half[1], half[2]};
  Vec3 n;
  float d = 0;
  return BoxBoxSat(a, b, &n, &d);
}

namespace {
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

void OrientedBoxAabb(const Vec3& centre, const Vec3 axes[3], const float half[3], Vec3* mn, Vec3* mx) {
  Obb b;
  b.c = centre;
  b.ax[0] = axes[0];
  b.ax[1] = axes[1];
  b.ax[2] = axes[2];
  b.h = Vec3{half[0], half[1], half[2]};
  BoxAabb(b, mn, mx);
}

namespace {

// ProcessSurfaceCollision's tests before its search: the unit takes part at all (0x6b904f..)
bool SurfaceTakesPart(const Unit* u) {
  const UnitMotion& m = u->motion;
  if (u->layer == "Air" || u->layer == "Sub") return false;
  if (u->dead || u->beingBuilt || u->parentId || St(u, US("Attached"))) return false;
  if (!m.bp || m.bp->canFly) return false;
  if (m.pushed) return false;  // a pushed unit never pushes
  return true;
}

// every 5th tick (id % 5 == tick % 5) a unit larger than 0.2 also collects props, in the same list as the units
// (mask 0x300, prop_collision.md 1.3): DoCollisionsFor goes through it in the grid's order, a prop's hit calls
// prop:OnCollision there, and a mate under the same order ends the list, the props after it included (1.4).
// (Read, not seen: no gate replay changes with it.)
bool SurfaceWithProps(Sim& sim, const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  return b.sizeX * b.sizeZ > 0.2f && u->id % 5u == sim.tick() % 5u;
}

// A unit's collision primitive as GetWorldShape reads it (held or not): the derived value a search made ahead notes
// in its ReadLog (core/speculate.h).
struct ShapeRead {
  float c[3], half[3], r, pos[3], ori[4];
  uint32_t type;
};
static_assert(sizeof(ShapeRead) <= ReadLog::kMaxDerived, "ShapeRead");
void ReadShape(const void* obj, void* out) {
  const Entity* e = static_cast<const Entity*>(obj);
  ShapeRead& v = *static_cast<ShapeRead*>(out);
  v.type = static_cast<uint32_t>(e->shape.type);
  v.c[0] = e->shape.center.x, v.c[1] = e->shape.center.y, v.c[2] = e->shape.center.z;
  v.half[0] = e->shape.half.x, v.half[1] = e->shape.half.y, v.half[2] = e->shape.half.z;
  v.r = e->shape.radius;
  const Vec3& p = e->shapeHeld ? e->shapePos : e->position;
  const Quat& q = e->shapeHeld ? e->shapeOri : e->orientation;
  v.pos[0] = p.x, v.pos[1] = p.y, v.pos[2] = p.z;
  v.ori[0] = q.x, v.ori[1] = q.y, v.ori[2] = q.z, v.ori[3] = q.w;
}

// A search made ahead: whether its grid query finds the same entities in the same order now (1) or not (0). While the
// grid's unit lists are as they were, it does without asking.
void ReadSameCandidates(const void* obj, void* out) {
  const SurfaceContacts::Query& q = *static_cast<const SurfaceContacts::Query*>(obj);
  const EntityGrid& g = q.sim->entityGrid();
  bool same = g.version(0) == q.version;
  if (!same) {
    static thread_local std::vector<Entity*> now;
    g.GatherBoxNoMarks(q.x0, q.z0, q.x1, q.z1, 1, &now);
    same = now == q.found;
  }
  *static_cast<unsigned char*>(out) = same ? 1 : 0;
}

// ProcessSurfaceCollision's search: the entities under the unit's box and what its box test finds, into out (the
// motion's contact list and the hits DoCollisionsFor acts on). It only reads. One code path for the serial turn (log
// null: the grid's own marking gather) and for the search made ahead (log: every read the result depends on noted;
// the gather without marks, which gives the same list).
void SurfaceSearch(Sim& sim, Unit* u, bool withProps, SurfaceContacts* out, ReadLog* log) {
  UnitMotion& m = u->motion;
  out->contacts.clear();
  out->hits.clear();
  const MotionBlueprint& b = *m.bp;
  if (log) {
    log->Raw(u->position);
    log->Raw(u->orientation);
    log->Raw(m.bp);  // (the sizes below are the blueprint's)
  }
  Obb box;
  box.c = u->position;
  // the box from the unit's transform, 0x672b50: its axes are VAxes3's rows (0x4ec590). Seen: Skirmish 2488, engineer
  // 0x400033 standing level beside the scout 0x400039, its quaternion's z the smallest denormal; with VAxes3's axes
  // the box test 0x474830 finds the two apart on the up x up axis (14), whose components are denormals, at 2488 and
  // 2489 (no push, as the original's motion shows), and touching at 2485 and 2487 (the pushes it made). Our
  // Right / Up / Forward gave that axis twice the denormal and the two always met.
  BoxAxes(u->orientation, box.ax);
  box.h = {b.sizeX * 0.5f, b.sizeY, b.sizeZ * 0.5f};
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
  // (the candidates live in a buffer kept from call to call, one per thread: they are all read before
  // DoCollisionsFor, the only step that can lead to another unit's ProcessSurfaceCollision)
  static thread_local std::vector<Entity*> cand;
  cand.clear();
  Vec3 amn, amx;
  if (withProps) {
    BoxAabb(box, &amn, &amx);
    if (log) sim.entityGrid().GatherBoxNoMarks(amn.x, amn.z, amx.x, amx.z, 3, &cand);
    else sim.entityGrid().GatherBox(amn.x, amn.z, amx.x, amx.z, 3, &cand);
  } else if (log) {
    SurfaceContacts::Query& q = out->query;
    q.sim = &sim;
    q.x0 = box.c.x - hx, q.z0 = box.c.z - hz, q.x1 = box.c.x + hx, q.z1 = box.c.z + hz;
    q.version = sim.entityGrid().version(0);
    sim.entityGrid().GatherBoxNoMarks(q.x0, q.z0, q.x1, q.z1, 1, &cand);
    q.found = cand;
    // (the units' lists change during the pass when a unit is made or a shape set: only a change that this query
    // would see throws the search away; a search with props is not made ahead)
    log->Derived(&ReadSameCandidates, &q, 1);
  } else {
    sim.entityGrid().GatherBox(box.c.x - hx, box.c.z - hz, box.c.x + hx, box.c.z + hz, 1, &cand);
  }
  auto visit = [&](Unit* e) {
    if (e == u) return;
    if (log) {
      log->Derived(&ReadShape, e, sizeof(ShapeRead));
      log->Raw(e->dead);
      log->Raw(e->destroyQueued);
    }
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
    if (meets) out->contacts.push_back(EntityRef(e));
    if (e->dead || e->destroyQueued) return;
    static const long dbgCol = getenv("MOHO64_DEBUG_COL") ? atol(getenv("MOHO64_DEBUG_COL")) : -1;
    if (dbgCol >= 0 && (static_cast<long>(u->id) == dbgCol || static_cast<long>(e->id) == dbgCol))
      Logf(LogLevel::Info,
           "col %u overlap %u (%.4f %.4f %.4f fwd %.4f %.4f half %.3f %.3f %.3f) with %u (type %d c %.4f %.4f %.4f ax2 %.4f %.4f "
           "half %.3f %.3f %.3f r %.3f) depth %.5f",
           sim.tick(), u->id, box.c.x, box.c.y, box.c.z, box.ax[2].x, box.ax[2].z, box.h.x, box.h.y, box.h.z, e->id,
           static_cast<int>(s.type), s.c.x, s.c.y, s.c.z, s.ax[2].x, s.ax[2].z, s.half.x, s.half.y, s.half.z, s.r, depth);
    if (depth > 0) out->hits.push_back({e, depth});
  };
  for (Entity* e : cand) {
    if (!e) continue;
    if (e->kind == Entity::Kind::Unit) {
      visit(static_cast<Unit*>(e));
    } else if (withProps) {
      if (log) log->Derived(&ReadShape, e, sizeof(ShapeRead));  // (a prop: PropHitTest reads its primitive)
      Vec3 n;
      float d = 0;
      if (PropHitTest(e, box, amn, amx, &n, &d)) out->hits.push_back({nullptr, d, e, n});
    }
  }
}

// (MOHO64_PAR_MOTION=verify) a search made ahead against the serial one: the same entities in the same order, the
// same bits; "" when they agree
std::string SurfaceDiff(const SurfaceContacts& a, const SurfaceContacts& b) {
  if (a.contacts != b.contacts) return "contact lists differ";
  if (a.hits.size() != b.hits.size()) return "hit counts differ";
  for (size_t i = 0; i < a.hits.size(); ++i) {
    const Hit &x = a.hits[i], &y = b.hits[i];
    if (x.u != y.u || x.prop != y.prop || std::memcmp(&x.depth, &y.depth, sizeof x.depth) != 0 ||
        std::memcmp(&x.n, &y.n, sizeof x.n) != 0)
      return "hit " + std::to_string(i) + " differs";
  }
  return {};
}

}  // namespace

void SurfaceContactsAhead(Sim& sim, const std::vector<Unit*>& order) {
  Speculation<SurfaceContacts>& spec = sim.surfaceSpec();
  // (the search's debug lines are written at the unit's own turn: with MOHO64_DEBUG_COL set nothing runs ahead)
  static const bool dbgCol = getenv("MOHO64_DEBUG_COL") != nullptr;
  if (!spec.active() || dbgCol) return;
  const uint32_t round = sim.tick();
  for (size_t i = 0; i < order.size(); ++i) {
    order[i]->specItem = static_cast<uint32_t>(i);
    order[i]->specRound = round;
  }
  // (MOHO64_PAR_MOTION_GRAIN: units per task, for measuring; 8 by default)
  static const size_t grain = getenv("MOHO64_PAR_MOTION_GRAIN") ? std::max(1L, atol(getenv("MOHO64_PAR_MOTION_GRAIN"))) : 8;
  spec.Run(sim.workers(), "surface contacts ahead", round, order.size(), grain,
           [&](size_t i, ReadLog& log, SurfaceContacts& out) {
             Unit* u = order[i];
             // (only the units MotionTick would hand to ProcessSurfaceCollision as things stand now: moving land, hover
             // and water units; one pushed or stopped later in the pass is searched at its turn, or not at all)
             const UnitMotion& m = u->motion;
             if (!m.bp || !m.bp->mobile() || m.bp->motionType == kMotionAir || m.atRest || m.ballistic) return false;
             if (!((m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z > 1e-6f || m.surfaceNext)) return false;
             if (!SurfaceTakesPart(u)) return false;
             if (SurfaceWithProps(sim, u)) return false;  // (a turn with props is searched at the unit's turn)
             SurfaceSearch(sim, u, false, &out, &log);
             return true;
           });
}

void ProcessSurfaceCollision(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  m.contacts.clear();  // (0x6b904f: emptied before any of the early returns)
  if (!SurfaceTakesPart(u)) return;
  const bool withProps = SurfaceWithProps(sim, u);
  // the search: made ahead (MOHO64_PAR_MOTION) and still good, or made now
  Speculation<SurfaceContacts>& spec = sim.surfaceSpec();
  const SurfaceContacts* ahead =
      spec.active() && u->specRound == sim.tick() && !withProps ? spec.Take(u->specItem) : nullptr;
  SurfaceContacts now;  // (its own: DoCollisionsFor can call Lua, and Lua another unit's ProcessSurfaceCollision)
  if (!ahead || spec.mode() == SpecMode::kVerify) {
    SurfaceSearch(sim, u, withProps, &now, nullptr);
    if (ahead) {
      std::string d = SurfaceDiff(*ahead, now);
      if (!d.empty()) {
        char b[96];
        std::snprintf(b, sizeof b, "tick %u unit 0x%x: ", sim.tick(), u->id);
        d = b + d;
      }
      spec.Checked(d.empty(), u->specItem, d);
    }
  }
  const SurfaceContacts& found = ahead ? *ahead : now;
  m.contacts = found.contacts;
  if (!found.hits.empty()) DoCollisionsFor(sim, u, found.hits);
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
        my.colLateTick =
            sim.inSteeringStage || (sim.navStageArmy > 0 && ya >= sim.navStageArmy) ? 0xffffffffu : now;
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
    // The sphere query (0x721fb0, mask 0x100): with R above 3 (0xe4f718) a candidate whose bounding box (entity
    // +0x240 / +0x24c) meets the cube of half side R x 0.707 (0xdff27c) around the centre is taken without the exact
    // test (0x7220c8..0x72218f, touching counts); every other one goes to its primitive's sphere test (vt+0x20).
    // The cube reaches past the sphere at its corners: a unit up to 1.22 R away can be a candidate. Seen: MP2 6125,
    // army 0's 0x95 (R 10.35) re-predicts and takes 0x83, 10.7 away in 3-D, whose box lies inside its cube; the
    // record it gives 0x83 (due 6128) starts the chain that clears 0xc1's, so 0x12 drives on at 6132. Ours left 0x83
    // out and 0x12 braked.
    WorldShape s;
    if (!GetWorldShape(e, &s)) return;
    bool inCube = false;
    if (R > 3.0f) {
      const float h = R * 0.707f;
      const Vec3& c = u->position;
      Vec3 mn, mx;
      ShapeBounds(s, &mn, &mx);
      inCube = !(c.x - h > mx.x || mn.x > c.x + h || c.y - h > mx.y || mn.y > c.y + h || c.z - h > mx.z || mn.z > c.z + h);
    }
    if (!inCube && !SphereOverlap(s, u->position, R)) return;
    if (UnitIgnores(u, e, 2)) return;
    // e.steering (+0x550): the Unit ctor makes one (AI_CreateSteering 0x5d3c30 at 0x6a5e4e) for every blueprint with
    // a Physics.MotionType (1..8, 0x6a5d9f..0x6a5dab), aircraft included. A landed aircraft is therefore a candidate:
    // no spline, air-capable, so on the 'second' side, and its meeting comes to Resolve's air-capable branch. Seen
    // (oracle probe mp2_3510): MP2 3499, army 0's land scout 0x67 predicts against the landed interceptor 0x3 in
    // its way first (a record for 3505), whose resolve and re-prediction lead to the hard brake for engineer 0x5d at
    // 3509; ours never looked at the interceptor and drove on.
    if (!e->motion.bp || !e->motion.bp->mobile()) return;
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

// FindFreeSpot 0x62b200 as ResolvePossibleCollision calls it (0x597783..0x59778e: Sim, size 1, whole map 0, flags
// 0, 0; the blueprint, unit vt+0x1c, in edi): is there room for Y's footprint at (x, z) or near it?
//   - STIMap::IsWithin 0x62ca60 with margin 1 on the playable rect. A point outside fails at once; the rings
//     (0x62b4ed) test the ORIGINAL point again each time, so they can never pass either;
//   - FootprintFitsStructure 0x720d90 (ours: LocationIsFree) at the point: OCCUPY_Check 0x5652e0's mobile path at
//     the footprint's origin cell, then the land / water structure grids (GetRectOr 0x8d8460);
//   - else square rings around the point as given (not snapped), stepping by 1: i over x outside, j over z inside,
//     a ring's sides stepping by 2r, until a ring ends past 900 candidates (0x62b3f6..0x62b760).
// Resolve keeps only the answer: the place found (FindFreeSpot writes it back over its copy of T at esp+0x20) is
// not used, the side-step goes to T as computed (SetCol with esp+0x64, written before the call).
bool SideStepRoom(Sim& sim, const Unit* Y, float x, float z) {
  if (!sim.map() || !Y->blueprint) return true;
  if (!sim.IsWithin(x, z, 1.0f, false)) return false;
  if (LocationIsFree(sim, *Y->blueprint, x, z)) return true;
  int tested = 0;
  for (int r = 1;; ++r) {
    for (int i = -r; i <= r; ++i) {
      const int jstep = (i == -r || i == r) ? 1 : 2 * r;
      for (int j = -r; j <= r; j += jstep) {
        ++tested;
        if (LocationIsFree(sim, *Y->blueprint, static_cast<float>(i) + x, static_cast<float>(j) + z)) return true;
      }
    }
    if (tested >= 900) return false;
  }
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
  // 0x596f30: an air-capable blocker (bp Air.CanFly, with a motion) is never braked for: without a transport
  // interface (+0x55c) and when Y comes at it (Approaching 0x596e00 on the two positions and velocities), its
  // motion is given its own position on the Air layer (CUnitMotion::SetTarget 0x6b85e0 (pos, no facing, 0x10)) and
  // it lifts off; the record is cleared either way. Seen: MP2 3525, army 0's interceptor 0x3 standing landed in
  // the land scout 0x67's way goes up (MovingUp) and lands again at the same spot when its auto-land time is out.
  if (X->motion.bp->canFly) {
    if (!X->transport && Approaching(Y, X, pY, vY, pX, vX)) AirSetTargetNow(sim, X, X->position, 0x10);
    return clear();
  }
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
  // (the four dots here are summed y, z, x: 0x597393..0x5973bf, 0x597452..0x59747d, 0x59750e..0x597548,
  // 0x597565..0x597580)
  if (!((dYX.y * vXn.y + dYX.z * vXn.z) + dYX.x * vXn.x > 0.707f)) {  // Y is not in X's 45-degree forward cone: Y brakes and waits
    SetCol(Y, 4, Y->position);
    if (log) Logf(LogLevel::Info, "col %u resolve Y %u X %u: brake", sim.tick(), Y->id, X->id);
    return 1;
  }
  if (HasSteering(X) && !sfl) {
    bool ok = Naval(X) ? Naval(Y) : (!(FpFlags(X) & 1) || (FpFlags(Y) & 1));
    if (ok) SetCol(X, 5, X->position);  // X brakes hard
  }
  bool facing = 0.0f > (dYX.y * fwdY.y + dYX.z * fwdY.z) + dYX.x * fwdY.x;  // Y points toward X
  Vec3 B, s;
  if (Ymoving && facing) {
    Vec3 vYn = Norm(vY);
    Vec3 rX = Right(X->orientation);
    if (!((rX.y * dYX.y + rX.z * dYX.z) + rX.x * dYX.x > 0.0f)) rX = Scale(rX, -1);
    float c = (vYn.y * vXn.y + vYn.z * vXn.z) + vYn.x * vXn.x;
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
    const TurnY& turn = cy > 0 ? (sfl ? kTurnMinusEighthPi : kTurnMinusQuarterPi) : (sfl ? kTurnEighthPi : kTurnQuarterPi);
    B = pY;
    s = RotY(turn, vXn);
  }
  Vec3 T = Add(B, Scale(s, clr));
  // FindFreeSpot 0x62b200 only gates the side-step: when it finds no room (0x597798) the record stays as it is.
  // Seen: Skirmish 2410, engineer 0x1e (uel0105) braked beside the land factory 0x36; its side-step point
  // (551.37, 470.85) lies in the factory's footprint, a spot one ring out is free, and the original backs away
  // toward the point itself from 2410 on (its steps run along (0.968, -0.251), T's direction). Ours asked for the
  // point's own cell only and kept the unit braked.
  if (!SideStepRoom(sim, Y, T.x, T.z)) return 0;
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
      MotionStopTarget(u);
      if (unused) CheckCollisions(sim, u);
    }
    // 0x5d2c7c..0x5d2cc4: the unit's velocity (GetVelocity, vt+0x3c) as a 3-D length, (x^2 + y^2) + z^2, against the
    // blueprint's MaxSpeed (bp+0x2a0) x 0.01, no speed multiplier. (Seen: Skirmish 2839, army 5's 0x50002a pushed by an
    // engineer it caught up with: its velocity (0.0268, -0.0124, -0.0193) is 0.0353 long in 3-D, over the 0.034, so
    // the push goes on and the original stands; on x and z alone it was 0.0330 and ours drove off a beat early.)
    const Vec3& uv = UnitVelocity(u);
    float v = std::sqrt((uv.x * uv.x + uv.y * uv.y) + uv.z * uv.z);
    if (m.bp->maxSpeed * 0.01f > v) {
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
      MotionStopTarget(u);
      if (unused) CheckCollisions(sim, u);
      return;
    }
    UpdatePath(sim, u, m.steerTarget, false, GetVal(m));
    CheckCollisions(sim, u);
  }
  // 0x5d2d24..0x5d2e4b: a unit in the Immobile state, or a stunned one, loses its spline once (Stop, and the
  // byte steering+0xa0 remembers it) and gets no point while that lasts. When it is let go, a unit that is
  // Moving, Patrolling or Attacking gets a fresh batch toward the same target, from where it stands now.
  const bool held = u->immobile || u->unitStates.Has(US("Immobile")) > 0 || u->stunned;
  if (!m.heldLatch) {
    if (held) {
      m.heldLatch = true;
      const bool unused = m.hasSpline && m.splineIdx < m.spline.size();
      SteeringStop(m);
      MotionStopTarget(u);
      if (unused) CheckCollisions(sim, u);
      return;
    }
  } else {
    if (held) return;
    if (Driving(u) && (u->unitStates.Has(US("Moving")) > 0 || u->unitStates.Has(US("Patrolling")) > 0 ||
                       u->unitStates.Has(US("Attacking")) > 0)) {
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
        // 0x5d2f29..0x5d2f76: only while the steering's own target (st+0x90) is not the no-target vector, that is
        // a waypoint DriveToNextWaypoint took up and no SetWaypoints(0, 0) wrote over since: a fresh batch toward it
        // when no points are left, then the prediction. A waypoint the navigator handed this beat is not taken up
        // yet. (Seen: Skirmish 3041, army 3's 0x300046 stopped at 3032 with a record due at 3041, given a Move to
        // its own cell at 3040: the original stands, ours made a batch toward the old target and stepped 0.028.)
        if (m.steerValid) {
          if (!m.hasSpline || m.splineIdx >= m.spline.size()) {
            m.faceDir = {};  // (0x5d2f70: SetTarget 0x6b88f0 with the target, no facing)
            m.motionTarget = m.steerTarget;
            UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
          }
          CheckCollisions(sim, u);
        }
        break;
      case 2: {
        m.faceDir = {};  // (0x5d2ebf: SetTarget 0x6b88f0 with the side-step point, no facing)
        Vec3 T = m.sideStep;
        m.motionTarget = T;
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
  // 0x5d310e: CUnitMotion::SetTarget 0x6b88f0 with the waypoint and no facing: the motion's facing (+0x20) becomes zero
  // for a unit that cannot fly (0x6b878d -> 0x6b8872). Only here, not at the navigator's SetGoal: a goal that is set and
  // dropped before the steering takes up a waypoint leaves the facing as it was. Seen: MP1 5544, army 1's frigate
  // 0x100056 (uas0103, AttackAngle 60): its attack task, woken by the weapon's "target lost", sets the dead target's
  // blip as the navigator's destination and ends in the same beat (AbortMove); the facing its engaged state set at 5541
  // (SetFacing 0x6b8590) stays, and when the brake spline is used up (5550) the idle hull turns toward it (CalcMoveCommon's
  // no-point branch, 0x6c2607). Ours cleared the facing with the goal and stood until its guns found a target (5552).
  m.faceDir = {};
  m.motionTarget = m.steerTarget;
  UpdatePath(sim, u, m.steerTarget, true, GetVal(m));
  CheckCollisions(sim, u);
  ProcessSplineMovement(sim, u);
}

}  // namespace

// (for SetWaypoints(0, 0) in motion.cpp: the prediction that follows the brake spline)
void SteeringCheckCollisions(Sim& sim, Unit* u) {
  if (HasSteering(u)) CheckCollisions(sim, u);
}

void SteeringNavStageArmy(Sim& sim, int army) { sim.navStageArmy = army; }

void SteeringTickUnits(Sim& sim, const std::vector<Unit*>& all) {  // (brain+0xac: creation order inside the army)
  sim.inSteeringStage = true;
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    if (u->destroyQueued) continue;
    SteeringTick(sim, u);
  }
  sim.inSteeringStage = false;
}

}  // namespace moho
