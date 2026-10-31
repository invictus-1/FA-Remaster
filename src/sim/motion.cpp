// Unit motion (see motion.h for what the original does and how it was checked).
#include "core/backtrace.h"
#include "core/dmath.h"
#include "sim/motion.h"
#include "sim/formation.h"
#include "sim/intel.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/air.h"
#include "sim/transport.h"
#include "sim/steering.h"
#include "sim/vecmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

#include "core/log.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

bool PathUnitBlocked(Unit* u, int x, int z, int flags);  // sim/landnav.cpp (COGrid::UnitIsBlocked 0x721340)
Quat LookQuat(float dx, float dy, float dz);  // (below; UpdateLayer's blend needs it)
namespace {
void SnapToWaterExe(Sim& sim, Unit* u);  // (below; UpdateLayer's blend needs it)

constexpr float kDegToRadTenth = 0.001745329238474369f;  // TurnRate (deg/s) -> rad per tick
constexpr float kStopSq = 9.999999974752427e-07f;        // |v|^2 below this: at rest

float GetNum(lua_State* L, int t, const char* k, float def) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
  lua_pop(L, 1);
  return v;
}
bool GetBool(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}

const char* const kMotionNames[] = {"RULEUMT_None",  "RULEUMT_Land",     "RULEUMT_Air",
                                    "RULEUMT_Water", "RULEUMT_Biped",    "RULEUMT_SurfacingSub",
                                    "RULEUMT_Amphibious", "RULEUMT_Hover", "RULEUMT_AmphibiousFloating",
                                    "RULEUMT_Special"};

int MotionIndex(const char* s) {
  if (!s) return 0;
  for (int i = 0; i < 10; ++i)
    if (!std::strcmp(kMotionNames[i], s)) return i;
  return 0;
}

// The original's sine approximation in RotateDirectionTowardTargetLimited (FA exe 0x6992c0).
// (0x6993e1..0x699410: the square taken once, t2 = a * a, and the polynomial in t2)
float SinApprox(float a) {
  const float t2 = a * a;
  return ((t2 * 0.00761000020429492f - 0.1660500019788742f) * t2 + 1.0f) * a;
}

// Turn direction (x, z) toward (tx, tz) by at most `maxAngle` radians; returns true when the
// target direction was reached. (CAiPathSpline::RotateDirectionTowardTargetLimited)
bool RotateToward(float& x, float& z, float tx, float tz, float maxAngle) {
  if (maxAngle > 3.1415927410125732f) maxAngle = 3.1415927410125732f;
  float lc = std::sqrt(x * x + z * z), lt = std::sqrt(tx * tx + tz * tz);
  float c = dmath::Cos(maxAngle);
  float lp = lc * lt;
  if (lp == 0.0f) return true;
  if (c * lp <= x * tx + z * tz) {
    float k = lc / lt;
    x = tx * k;
    z = tz * k;
    return true;
  }
  float s = SinApprox(maxAngle);
  // 0x6993c4..0x6993df works in the (x, -z) frame: cross' = to'.y f'.x - f'.y to'.x, which is minus ours, and turns
  // by +sin unless cross' < 0. So ours turns by -sin when OUR cross is <= 0: a target exactly behind (cross 0) is
  // turned toward the same side as in the original. (Seen: Economy 1869, support commander 39 at x 860.5 facing +z
  // with its spline target at x 860.5 behind it: the original turned toward +x, ours toward -x.)
  if (x * tz - z * tx <= 0.0f) s = -s;  // target is clockwise (x toward -z), or exactly behind
  float n = s * s + c * c;
  if (std::fabs(n - 1.0f) > 0.0010000000474974513f) {  // 0x6999f0: times 1 / length, zero at 1e-6 or less
    const float l = std::sqrt(n);
    if (l > 9.99999997e-07f) {
      const float inv = 1.0f / l;
      s *= inv;
      c *= inv;
    } else {
      s = 0.0f;
      c = 0.0f;
    }
  }
  float nx = c * x - s * z, nz = s * x + c * z;
  static const bool dbgRot = getenv("MOHO64_DEBUG_ROT") != nullptr;  // every limited turn, as bits
  if (dbgRot) {
    auto bits = [](float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      return v;
    };
    Logf(LogLevel::Info, "rot in %08x %08x to %08x %08x angle %08x c %08x s %08x out %08x %08x", bits(x), bits(z), bits(tx),
         bits(tz), bits(maxAngle), bits(c), bits(s), bits(nx), bits(nz));
  }
  x = nx;
  z = nz;
  return false;
}

Quat Mul(const Quat& a, const Quat& b) {  // a * b (apply b, then a)
  Quat r;
  r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return r;
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
  // matrix form (the original converts the quaternion to a matrix, FA exe 0x452d40)
  float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  Vec3 r;
  r.x = (1 - 2 * (yy + zz)) * v.x + 2 * (xy - wz) * v.y + 2 * (xz + wy) * v.z;
  r.y = 2 * (xy + wz) * v.x + (1 - 2 * (xx + zz)) * v.y + 2 * (yz - wx) * v.z;
  r.z = 2 * (xz - wy) * v.x + 2 * (yz + wx) * v.y + (1 - 2 * (xx + yy)) * v.z;
  return r;
}

// Wm3 Quaternion::Align: the shortest rotation taking unit vector a to unit vector b.
Quat Align(const Vec3& a, const Vec3& b) {
  Vec3 h{a.x + b.x, a.y + b.y, a.z + b.z};
  float l = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
  Quat q;
  if (l > 1e-6f) {
    h.x /= l;
    h.y /= l;
    h.z /= l;
    q.w = a.x * h.x + a.y * h.y + a.z * h.z;
    q.x = a.y * h.z - a.z * h.y;
    q.y = a.z * h.x - a.x * h.z;
    q.z = a.x * h.y - a.y * h.x;
  } else {
    q.w = 0;
    q.x = 1;
    q.y = 0;
    q.z = 0;
  }
  return q;
}

bool IsWaterLayerType(int mt) { return mt == kMotionWater || mt == kMotionSurfacingSub; }

void SetLayer(Sim& sim, Unit* u, const char* layer) {
  if (u->layer == layer) return;
  std::string old = u->layer;
  u->layer = layer;
  lua_State* L = sim.L();
  lua_pushstring(L, layer);
  lua_pushstring(L, old.c_str());
  sim.CallMethod(L, u, "OnLayerChange", 2);
}

// CUnitMotion::UpdateCurrentLayer 0x6c35c0 (engine-ref layer_change.md): run by MotionTick before this tick's
// move, on the committed position (end of the previous tick), only while the last move |v|^2 > 1e-6.
// thr = water elevation (-10000 without water) + LayerChangeOffsetHeight when the footprint occupies Land
// (Entity::GetFootprint 0x6c368b: the AltFootprint's caps while it is the unit's; a Salem on Water4x4 has none);
// strictly above: Seabed->Land (Land, Amphibious), Water->Land (Hover, AmphibiousFloating, Water); strictly
// below: Land->Seabed (Land, Amphibious), Land->Water (Hover, AmphibiousFloating).
void UpdateLayer(Sim& sim, Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  const TerrainMap* map = sim.map();
  if (!map) return;
  const int mt = b.motionType;
  float thr = map->hasWater ? map->waterElevation : -10000.0f;
  // footprint OccupancyCaps & Land (the Footprint's: by motion type Land, Biped, Amphibious, Hover,
  // AmphibiousFloating)
  if (UnitFootprint(u).caps & kLayerLand) thr = thr + b.layerChangeOffset;
  float h = map->TerrainHeight(u->position.x, u->position.z);
  const std::string& L = u->layer;
  auto set = [&](const char* to) { SetLayer(sim, u, to); };
  // layer_change.md 4.1 (0x6c372b / 0x6c3804), only for the two rules between Water and Land and only with a
  // LayerTransitionDuration: before the layer is set, the blend's counter starts at 1 and its two ends are taken.
  // Out of the water: the transform ten beats of the motion's velocity ahead, set on the ground there
  // (SnapToGround 0x6c1610). Into it: the transform as it is, set on the water (SnapToWater 0x6c1cb0).
  auto blend = [&](bool toLand) {
    if (b.layerTransitionDuration == 0.0f) return;
    UnitMotion& m = u->motion;
    const Vec3 p0 = u->position;
    const Quat q0 = u->orientation;
    m.layerBlend = 1.0f;
    m.blendFromPos = p0;
    m.blendFromOri = q0;
    if (toLand) {
      u->position = Vec3{m.vel.x * 10.0f + p0.x, m.vel.y * 10.0f + p0.y, m.vel.z * 10.0f + p0.z};
      m.snapTerrain = true;
      SnapUnit(sim, u);
      m.snapTerrain = false;
    } else {
      // SnapToWater 0x6c1cb0 of the transform as it is (0x6c381b..0x6c382a): CalcRollHack 0x6c1350, the height,
      // and COORDS_Tilt 0x50b820 turning the transform's OWN up axis onto the roll's (the shortest arc), not a
      // level look along the facing. Seen: Naval 803, the Salem 0x45 (urs0201) walks into the sea tilted by the
      // beach; the blend's end orientation of ours, LookQuat(facing), had another roll part, and the blended
      // orientation's z was 1 ulp off the original's from the blend's second beat (805) to its end; its two guns'
      // directions with it (117 watch lines).
      SnapToWaterExe(sim, u);
    }
    m.blendToPos = u->position;
    m.blendToOri = u->orientation;
    u->position = p0;
    u->orientation = q0;
  };
  if (h > thr) {
    if (L == "Seabed" && (mt == kMotionAmphibious || mt == kMotionLand)) {
      set("Land");
    } else if (L == "Water" && (mt == kMotionHover || mt == kMotionAmphibiousFloating || mt == kMotionWater)) {
      blend(true);
      set("Land");
    }
  } else if (thr > h) {
    if (L == "Land") {
      if (mt == kMotionAmphibious || mt == kMotionLand) {
        set("Seabed");
      } else if (mt == kMotionHover || mt == kMotionAmphibiousFloating) {
        blend(false);
        set("Water");
      }
    }
  }
}

const char* const kVertEventNames[] = {"Top", "Bottom", "Up", "Down", "Hover", "Straight"};  // 0xf58360

const char* LayerNameOf(int bit) {
  switch (bit) {
    case kLayerLand: return "Land";
    case kLayerSeabed: return "Seabed";
    case kLayerSub: return "Sub";
    case kLayerWater: return "Water";
    case kLayerAir: return "Air";
    default: return "Orbit";
  }
}

// 0x6b8f70: the vertical event of a non-flying unit (m+0x80) and its script callback
void SetVertEvent(Sim& sim, Unit* u, int e) {
  UnitMotion& m = u->motion;
  if (m.vertEvent == e) return;
  int old = m.vertEvent;
  m.vertEvent = e;
  lua_State* L = sim.L();
  lua_pushstring(L, kVertEventNames[e]);
  lua_pushstring(L, kVertEventNames[old]);
  sim.CallMethod(L, u, "OnMotionVertEventChange", 2);
}

// CUnitMotion::HandleDivingAndSurfacing 0x6c3220 (CalcMoveWater, after the move): while MovingUp / MovingDown the
// depth moves by sin(pi * progress) x DiveSurfaceSpeed / 10 per tick (progress folded at the half, at least a
// tenth of the full step) between 0 and the unit's elevation (not below 0.25 over the bottom); at the end the
// unit takes the target layer. Returns whether the unit can dive at all and is diving or surfacing.
bool HandleDivingAndSurfacing(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  const float elev = m.elevationAttr;
  if (elev == 0.0f) return false;
  const bool up = u->unitStates.Has(US("MovingUp")) != 0;
  if (!up && !u->unitStates.Has(US("MovingDown"))) {
    m.depthStep = 0;
    return false;
  }
  const TerrainMap* map = sim.map();
  const float water = map->hasWater ? map->waterElevation : -10000.0f;
  float bottom = (map->TerrainHeight(u->position.x, u->position.z) + 0.25f) - water;
  if (bottom > 0.0f) bottom = 0.0f;
  const float target = bottom > elev ? bottom : elev;
  float frac = std::fabs(m.depth / target);
  const float speed = m.bp->diveSurfaceSpeed * 0.1f;
  if (frac > 0.5f) frac = 1.0f - frac;
  double s, c;
  dmath::SinCos(static_cast<double>(frac * 3.1415927f), &s, &c);
  const float sinStep = static_cast<float>(s * static_cast<double>(speed));  // (x87: fsin wide, one rounding)
  const float minStep = speed * 0.1f;
  m.depthStep = minStep > sinStep ? minStep : sinStep;
  if (up) {
    float d = m.depth + m.depthStep;
    if (d > 0.0f) d = 0.0f;
    m.depth = d;
    if (d == 0.0f) {
      if (m.targetLayer) SetLayer(sim, u, LayerNameOf(m.targetLayer));
      u->unitStates.Erase(US("MovingUp"));
      SetVertEvent(sim, u, 0);
    }
  } else {
    float d = m.depth - m.depthStep;
    if (!(d > target)) d = target;
    m.depth = d;
    if (target >= d) {
      if (m.targetLayer) SetLayer(sim, u, LayerNameOf(m.targetLayer));
      u->unitStates.Erase(US("MovingDown"));
      SetVertEvent(sim, u, 1);
    }
  }
  return true;
}

}  // namespace

void MotionCreated(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!m.bp) return;
  m.elevationAttr = m.bp->elevation;
  if (u->layer == "Sub") {
    m.depth = m.elevationAttr;
    SetVertEvent(sim, u, 1);
  }
}

void SetNewTargetLayer(Sim& sim, Unit* u, int layer) {
  UnitMotion& m = u->motion;
  if (u->layer == "Sub") {
    if (layer == kLayerWater) {
      u->unitStates.Insert(US("MovingUp"));
      SetVertEvent(sim, u, 2);
    }
  } else if (u->layer == "Water") {
    if (layer == kLayerSub) {
      u->unitStates.Insert(US("MovingDown"));
      SetVertEvent(sim, u, 3);
    }
  }
  m.targetLayer = layer;
}

// CalcMoveCommon's facing: the look rotation 0x50b480 (COORDS_Orient), core/dmath.h LookQuat.
Quat LookQuat(float dx, float dy, float dz) {
  float q[4];
  dmath::LookQuat(dx, dy, dz, q);
  return Quat{q[0], q[1], q[2], q[3]};
}

// 0x699940: the quaternion of a hull that has turned on the spot, from its new forward (d0, d1) = (x, -z) as the
// limited turn 0x6992c0 left it, NOT normalised again: the matrix with the rows (-d1, 0, -d0), (0, 1, 0),
// (d0, 0, -d1) through MatrixToQuat 0x4eb3f0. (The spline point's facing goes through 0x50b480 = LookQuat, which
// normalises three times on the way: the same heading with another last digit, and a ship that has turned on the
// spot carries that digit in every turret angle afterwards.)
Quat TurnQuat(float d0, float d1) {
  const float X[3] = {-0.0f - d1, 0.0f, -0.0f - d0}, Y[3] = {0.0f, 1.0f, 0.0f}, Z[3] = {d0, 0.0f, -0.0f - d1};
  const float* A[3] = {X, Y, Z};
  float q[3], w;
  float tr = (A[0][0] + A[1][1]) + A[2][2];
  if (tr > 0.0f) {
    float r = std::sqrt(tr + 1.0f);
    w = r * 0.5f;
    float inv = 0.5f / r;
    q[0] = (A[1][2] - A[2][1]) * inv;
    q[1] = (A[2][0] - A[0][2]) * inv;
    q[2] = (A[0][1] - A[1][0]) * inv;
  } else {
    static const int next[3] = {1, 2, 0};
    int i = (A[1][1] > A[0][0]) ? 1 : 0;
    if (A[2][2] > A[i][i]) i = 2;
    int j = next[i], k = next[j];
    float r = std::sqrt((A[i][i] - (A[k][k] + A[j][j])) + 1.0f);
    float inv = 0.5f / r;
    q[i] = r * 0.5f;
    w = (A[j][k] - A[k][j]) * inv;
    q[j] = (A[j][i] + A[i][j]) * inv;
    q[k] = (A[i][k] + A[k][i]) * inv;
  }
  return Quat{q[0], q[1], q[2], w};
}

Quat YawQuat(float fx, float fz) {
  float h = dmath::Atan2(fx, fz);
  Quat q;
  q.y = dmath::Sin(h * 0.5f);
  q.w = dmath::Cos(h * 0.5f);
  return q;
}

const MotionBlueprint& GetMotionBlueprint(lua_State* L, const BlueprintInfo& bp, const SimBlueprints& bps) {
  static const SimCache<std::map<const BlueprintInfo*, MotionBlueprint>> cacheSlot;  // (grid: the sim's navigation)
  auto& cache = cacheSlot.For(*Sim::From(L));
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  MotionBlueprint m;
  int top = lua_gettop(L);
  bps.PushTable(L, bp);
  int t = lua_gettop(L);
  if (lua_istable(L, t)) {
    m.sizeX = GetNum(L, t, "SizeX", 1);
    m.sizeY = GetNum(L, t, "SizeY", 1);
    m.sizeZ = GetNum(L, t, "SizeZ", 1);
    m.averageDensity = GetNum(L, t, "AverageDensity", 1);
    lua_pushstring(L, "Air");
    lua_rawget(L, t);
    if (lua_istable(L, -1)) m.canFly = GetBool(L, lua_gettop(L), "CanFly");
    lua_pop(L, 1);
    lua_pushstring(L, "Physics");
    lua_rawget(L, t);
    int p = lua_gettop(L);
    if (lua_istable(L, p)) {
      lua_pushstring(L, "MotionType");
      lua_rawget(L, p);
      m.motionType = MotionIndex(lua_tostring(L, -1));
      lua_pop(L, 1);
      m.maxSpeed = GetNum(L, p, "MaxSpeed", 0);
      m.maxSpeedReverse = GetNum(L, p, "MaxSpeedReverse", 0);
      m.maxAccel = GetNum(L, p, "MaxAcceleration", 0);
      m.maxBrake = GetNum(L, p, "MaxBrake", 0);
      m.maxSteerForce = GetNum(L, p, "MaxSteerForce", 0);
      m.bankingSlope = GetNum(L, p, "BankingSlope", 0);
      m.wobbleFactor = GetNum(L, p, "WobbleFactor", 0);
      m.wobbleSpeed = GetNum(L, p, "WobbleSpeed", 0);
      m.rollStability = GetNum(L, p, "RollStability", 0);
      m.rollDamping = GetNum(L, p, "RollDamping", 0);
      m.turnRadius = GetNum(L, p, "TurnRadius", 0);
      m.turnRate = GetNum(L, p, "TurnRate", 0);
      m.turnFacingRate = GetNum(L, p, "TurnFacingRate", 0);
      m.elevation = GetNum(L, p, "Elevation", 0);
      m.diveSurfaceSpeed = GetNum(L, p, "DiveSurfaceSpeed", 1);
      m.layerChangeOffset = GetNum(L, p, "LayerChangeOffsetHeight", -0.1f);
      m.layerTransitionDuration = GetNum(L, p, "LayerTransitionDuration", 0);
      m.rotateOnSpot = GetBool(L, p, "RotateOnSpot");
      m.rotateOnSpotThreshold = GetNum(L, p, "RotateOnSpotThreshold", 0.5f);
      m.backUpDistance = GetNum(L, p, "BackUpDistance", 0);
      m.standUpright = GetBool(L, p, "StandUpright");
      m.rotateBodyWhileMoving = GetBool(L, p, "RotateBodyWhileMoving");
      m.sinkLower = GetBool(L, p, "SinkLower");
      lua_pushstring(L, "RaisedPlatforms");
      lua_rawget(L, p);
      if (lua_istable(L, -1))
        for (int i = 1;; ++i) {
          lua_rawgeti(L, -1, i);
          if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            break;
          }
          m.raisedPlatforms.push_back(static_cast<float>(lua_tonumber(L, -1)));
          lua_pop(L, 1);
        }
      lua_pop(L, 1);
    }
  }
  lua_settop(L, top);
  if (bp.hasFootprint) m.footprint = bp.footprint;
  if (!m.footprint.sizeX) m.footprint.sizeX = 1;
  if (!m.footprint.sizeZ) m.footprint.sizeZ = 1;
  m.altFootprint = bp.hasFootprint ? bp.altFootprint : m.footprint;
  if (!m.altFootprint.sizeX) m.altFootprint.sizeX = 1;
  if (!m.altFootprint.sizeZ) m.altFootprint.sizeZ = 1;
  return cache.emplace(&bp, m).first->second;
}

void GoalCell(const MotionBlueprint& b, float x, float z, int* cx, int* cz) { GoalCell(b.footprint, x, z, cx, cz); }
void GoalCell(const NamedFootprint& f, float x, float z, int* cx, int* cz) {
  *cx = static_cast<int>(std::nearbyint(x - f.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(z - f.sizeZ * 0.5f));
}

const NamedFootprint& UnitFootprint(const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  return (u->altFootprintNav || u->altFootprintForced) ? b.altFootprint : b.footprint;
}

const NamedFootprint& PathFinderFootprint(const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  return u->altFootprintNav ? b.altFootprint : b.footprint;
}

// CUnitMotion::SnapToGround (FA exe 0x6c1610) / SnapToWater: height and tilt from the terrain
// under the four corners of the unit's size box.
// FindIntersectingRaisedPlatform 0x6c2f00: drops the last pick (m+0x114), then takes the nearest live unit with
// RaisedPlatforms among the contacts of the last contact pass (m+0x178), by (dx^2 + dz^2) + dy^2. Called by
// CalcMoveLand / Hover / Water only on a beat the unit moved (or m+0x90 asks); SnapToGround stands on the pick,
// which therefore lasts while the unit stands still.
void FindRaisedPlatform(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  m.platformRef = 0;
  float bestD = std::numeric_limits<float>::max();
  for (uint32_t ref : m.contacts) {
    Entity* e = sim.FindEntity(ref);
    if (!e || e->kind != Entity::Kind::Unit) continue;
    const Unit* p = static_cast<const Unit*>(e);
    if (p->dead || !p->motion.bp || p->motion.bp->raisedPlatforms.size() < 12) continue;
    const float dx = u->position.x - p->position.x, dy = u->position.y - p->position.y, dz = u->position.z - p->position.z;
    const float d = (dx * dx + dz * dz) + dy * dy;
    if (bestD > d) {
      bestD = d;
      m.platformRef = ref;
    }
  }
}
// m+0x114 as SnapToGround reads it (a weak reference: gone with its unit)
const Unit* RaisedPlatform(const Sim& sim, const Unit* u) {
  if (!u->motion.platformRef) return nullptr;
  Entity* e = sim.FindEntity(u->motion.platformRef);
  return e && e->kind == Entity::Kind::Unit ? static_cast<const Unit*>(e) : nullptr;
}
// 0x62af70: the first quad (unrotated, from the platform's position) holding p, bilinear in it. The quad's width
// and depth are taken from its corners in the world (0x62b0e4: (x1 + px) - (x0 + px), (z1 + pz) - (z0 + pz)), not
// from the blueprint's numbers: beside a coordinate of several hundred they keep fewer digits, and an engineer
// rolling off a Seraphim factory's deck stands a float step higher or lower with it.
float PlatformHeight(const Unit* P, float x, float z) {
  if (P->dead) return 0;
  const auto& q = P->motion.bp->raisedPlatforms;
  float px = P->position.x, pz = P->position.z;
  for (size_t i = 0; i + 12 <= q.size(); i += 12) {
    float x0 = q[i] + px, z0 = pz + q[i + 1], x1 = q[i + 3] + px, z1 = q[i + 7] + pz, x3 = q[i + 9] + px,
          z3 = q[i + 10] + pz;
    if (!(x0 <= x && x <= x3 && z0 <= z && z <= z3)) continue;
    float u = (x - x0) / (x1 - x0);
    float v = (z - z0) / (z1 - z0);
    float a = q[i + 2] + (q[i + 8] - q[i + 2]) * v;
    float b = q[i + 5] + (q[i + 11] - q[i + 5]) * v;
    return a + u * (b - a);
  }
  return 0;
}

namespace {
void AlignUp(Quat& q, Vec3 n);
Vec3 RollHack(Unit* u);

// SnapToWater 0x6c1cb0 on the unit's transform: at water + depth, but never closer than 0.25 to the bottom; a
// submerged unit stays under the surface and its depth follows the bottom.
// SnapToWater's first call is CalcRollHack 0x6c1350 (0x6c1cea), its last COORDS_Tilt 0x50b820 (0x6c1e06): the
// transform's up axis is turned onto the roll's and the dive's up vector (level (0, 1, 0) for a ship that
// neither rolls nor dives). Seen: a surfacing T2 submarine standing still (Subs, unit 8 at 1136-1205) pitched
// its bow up by up to 19 degrees, and down when it dived again (1231-1300); ours stayed level.
void SnapToWaterExe(Sim& sim, Unit* u) {
  const TerrainMap* map = sim.map();
  UnitMotion& m = u->motion;
  const Vec3 up = RollHack(u);
  const float water = map->hasWater ? map->waterElevation : -10000.0f;
  float y = map->TerrainHeight(u->position.x, u->position.z) + 0.25f;
  if (water + m.depth > y) y = water + m.depth;
  if (0.0f > m.depth) {
    if (y > water) y = water;
    m.depth = y - water;
  }
  u->position.y = y;
  AlignUp(u->orientation, up);
}
}

void SnapUnit(const Sim& csim, Unit* u) {
  Sim& sim = const_cast<Sim&>(csim);
  const TerrainMap* map = sim.map();
  if (!map || !u->motion.bp) return;
  const MotionBlueprint& b = *u->motion.bp;
  // SnapToGround 0x6c1610 tilts the transform as it is (CalcMoveCommon set the facing when the unit moved)
  // CUnitMotion::MotionTick 0x6b9f77..0x6b9fe5 picks the calculation by the LAYER (a hover aside): Land and Seabed
  // take CalcMoveLand 0x6c3180 (SnapToGround), every other layer CalcMoveWater 0x6c3480, whose only snap is
  // SnapToWater 0x6c1cb0 (0x6c3564), whatever the motion type. So an AmphibiousFloating unit (a Cybran destroyer) in
  // the Water layer sits at the water's height + its depth (not nearer than 0.25 to the bottom under its centre),
  // levelled by its roll; it does not ride the surface under its four corners. Seen (Naval 843): a Salem let go by
  // its factory beside a beach, one corner over land 0.145 above the sea: the original stays level at 17.5, ours
  // lifted it by a quarter of that and tilted it for two beats.
  // The same holds for a Water or SurfacingSub unit: one in the Land layer runs CalcMoveLand and SnapToGround,
  // which takes the ground under its corners whatever the motion type (0x6c18e7, only MotionType 7 reads the
  // surface), and the Water -> Land blend's end is SnapToGround too (UpdateCurrentLayer 0x6c378d). A ship created
  // on land starts in the Land layer (Entity::GetStartingLayer 0x62ad30) and UpdateCurrentLayer never takes a
  // MotionType Water unit out of it (0x6c37c1..0x6c37de: Land -> Water is for Hover and AmphibiousFloating only),
  // so it sits on the ground, tilted, and on the bottom where it drives into the sea. (Ours took the motion type
  // here: battleships spawned on land in the interface host, SETONS_R, floated 0.25 above the ground, level, and
  // rode the surface once in the sea. Their drive over the land is the original's: pathfinding.md 7.)
  const bool waterCalc = (b.motionType == kMotionWater || b.motionType == kMotionSurfacingSub ||
                          b.motionType == kMotionAmphibiousFloating) &&
                         !u->motion.snapTerrain && u->layer != "Land" && u->layer != "Seabed";
  if (waterCalc) {
    SnapToWaterExe(sim, u);
    return;
  }
  if (b.motionType == kMotionAir) return;
  bool hover = b.motionType == kMotionHover;
  // Only MotionType 7 (Hover) takes the surface (0x6c180b: [bp+0x290] == 7 -> 0x62b160, the higher of ground and
  // water); every other type takes the ground (0x6c18e7 -> STIMap::GetElevation 0x44fb90, plus the platform), an
  // AmphibiousFloating one too: in the Land or Seabed layer it stands on the bottom, under the sea where the bottom is
  // below it (the Cybran destroyer comes ashore at 17.17 under a sea of 17.5, wading). Seen: Skirmish 1306-1313, UEF
  // engineer 0x400013 (uel0105, AmphibiousFloating) driving into the sea still in the Land layer: the original goes
  // down with the bottom to 17.37 and keeps its tilt, ours stopped at the sea's 17.5 and levelled out; both are on
  // the water at 1314, when the layer turns.
  const bool floating = false;
  float hx = b.sizeX * 0.5f, hz = b.sizeZ * 0.5f;
  const Vec3 offs[4] = {{hx, 0, hz}, {-hx, 0, hz}, {-hx, 0, -hz}, {hx, 0, -hz}};
  Vec3 c[4];
  // SnapToGround 0x6c1610 takes the platform (motion+0x114, found by CalcMoveHover / Land / Water through
  // FindIntersectingRaisedPlatform 0x6c2f00) for every type: a hover unit's corners are the surface (0x62b160:
  // the higher of ground and water) plus the platform's height. (Seen: Aeon and Seraphim engineers, which hover,
  // stand on their factory's deck when they are let go: 0.5 and 0.65 higher than the ground.)
  const Unit* plat = RaisedPlatform(sim, u);
  for (int i = 0; i < 4; ++i) {
    Vec3 r = Rotate(u->orientation, offs[i]);
    c[i] = {r.x + u->position.x, 0, r.z + u->position.z};
    c[i].y = (hover || floating) ? map->SurfaceHeight(c[i].x, c[i].z) : map->TerrainHeight(c[i].x, c[i].z);
    if (plat) c[i].y += PlatformHeight(plat, c[i].x, c[i].z);
  }
  float y = (c[3].y + c[2].y + c[1].y + c[0].y) * 0.25f;
  Vec3 n{0, 1, 0};
  if (!b.standUpright) {
    const Vec3& A = c[0];
    const Vec3& B = c[1];
    const Vec3& C = c[2];
    const Vec3& D = c[3];
    float d1x = D.x - B.x, d1y = D.y - B.y, d1z = D.z - B.z;
    float d2x = C.x - A.x, d2y = C.y - A.y, d2z = C.z - A.z;
    n = {d1y * d2z - d1z * d2y, d1z * d2x - d1x * d2z, d1x * d2y - d1y * d2x};
  }
  if (b.standUpright || b.sinkLower) {  // the centre's terrain (no platform) joins the range
    float t = map->TerrainHeight(u->position.x, u->position.z);
    float lo = std::min({c[0].y, c[1].y, c[2].y, c[3].y, t});
    float hi = std::max({c[0].y, c[1].y, c[2].y, c[3].y, t});
    y -= (hi - lo) * 0.25f;
  }
  if (hover) y += b.elevation;
  u->position.y = y;
  // MOHO64_DEBUG_SNAP=<unit id>: the exact floats (as bits) going into and coming out of the tilt
  static const long dbgSnap = getenv("MOHO64_DEBUG_SNAP") ? atol(getenv("MOHO64_DEBUG_SNAP")) : -1;
  const bool dbg = dbgSnap >= 0 && static_cast<long>(u->id) == dbgSnap;
  if (dbg)
    Logf(LogLevel::Info, "snapy %u unit %u type %d floating %d platform %d corners %.4f %.4f %.4f %.4f -> y %.4f", sim.tick(),
         u->id, b.motionType, floating ? 1 : 0, plat ? static_cast<int>(plat->id) : -1, c[0].y, c[1].y, c[2].y, c[3].y, y);
  auto bits = [](float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
  };
  const Quat before = u->orientation;
  AlignUp(u->orientation, n);  // 0x50b820: the transform's own up axis turns onto the normal
  if (dbg)
    Logf(LogLevel::Info,
         "snap %u unit %u face %08x %08x heading %08x %08x pre wxyz %08x %08x %08x %08x n %08x %08x %08x post wxyz %08x %08x %08x %08x "
         "corners y %08x %08x %08x %08x",
         sim.tick(), u->id, bits(u->motion.fx), bits(u->motion.fz), bits(u->motion.bx), bits(u->motion.bz), bits(before.w),
         bits(before.x), bits(before.y), bits(before.z), bits(n.x), bits(n.y), bits(n.z), bits(u->orientation.w),
         bits(u->orientation.x), bits(u->orientation.y), bits(u->orientation.z), bits(c[0].y), bits(c[1].y), bits(c[2].y),
         bits(c[3].y));
}

void MotionSetGoal(Sim& sim, Unit* u, const std::vector<Vec3>& path, bool passThrough, uint32_t driveTick) {
  UnitMotion& m = u->motion;
  if (!m.bp || path.empty()) return;
  if (m.bp->motionType == kMotionAir) {
    m.passThrough = passThrough;
    m.failed = false;
    AirSetGoal(sim, u, path.back(), driveTick);
    return;
  }
  // land, hover and naval units: the navigator plans and steers (sim/landnav.cpp)
  LandNavSetGoal(sim, u, path.back(), passThrough);
}

// CAiSteeringImpl::Stop 0x5d35e0: the spline and the collision record go; the unit coasts.
// Both go only when there IS a spline: Stop tests +0x80 first (0x5d35e3 je 0x5d366b) and with none it only calls
// CUnitMotion::Stop (0x5d3675); the record reset 0x596560 (0x5d365a) is inside the spline branch, as it is in
// SetWaypoints(0, 0) (0x5d2ae3). So a unit standing without a spline keeps its record (its link to the other unit,
// the meeting point, the type) through the navigator's Stop and MakeIdle at the end of a path (Execute 0x5a43dd /
// 0x5a43fb), and a later prediction on it is passed over unless it is nearer than that point (0x597be5..0x597c81).
// Seen: MP2 6206, army 1's Striker 0x1000bc reaches the end of its path (MotionNavDone) with no spline and a record
// against 0x100064 made at 6205 (due 6217); the original keeps it (resolved at 6217 to nothing: link and point stay),
// so 0x100035's re-prediction at 6238 does not put a record on it, it does not brake at 6239, its stop in FAF's
// DistributeOrders clear (Lua of 6239) makes no prediction, and at 6240 0x100090 side-steps 0x100083 while 0x100083
// takes its brake point. Ours had wiped the record at 6206: 0x1000bc braked hard at 6239 (a mode-4 spline), its stop
// re-predicted onto both, and 0x100083 braked hard at 6240 while 0x100090 stood.
void SteeringStop(UnitMotion& m) {
  m.pointNow = false;
  if (!m.hasSpline) return;
  m.spline.clear();
  m.splineIdx = 0;
  m.hasSpline = false;
  m.colType = 0;
  m.colUnit = 0;
  m.colTick = 0xffffffffu;
  m.sideStep = {};
}

// CUnitMotion::Stop 0x6b8460 with no point (0x6b84c6): while the motion drives through (m+0x8c), the target is 100
// ahead along its velocity m+0x38 (normalised copy 0x44f7e0: ((x*x + y*y) + z*z), above 0 times 1 / len, else zero;
// each part times 100 (0xe4f744), added to the position, 0x6b84d7..0x6b8544); otherwise the position (0x6b854c..).
// (The air path is air.cpp's Stop.)
void MotionStopTarget(Unit* u) {
  UnitMotion& m = u->motion;
  if (m.air) return;
  const Vec3& p = u->position;
  if (m.passThrough) {
    const float l = std::sqrt((m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z);
    Vec3 n{};
    if (l > 0.0f) {
      const float inv = 1.0f / l;
      n = {m.vel.x * inv, inv * m.vel.y, inv * m.vel.z};
    }
    m.motionTarget = {p.x + n.x * 100.0f, p.y + n.y * 100.0f, p.z + n.z * 100.0f};
  } else {
    m.motionTarget = p;
  }
}

void MotionNavBegin(Unit* u) {  // CAiNavigatorImpl::SetGoal 0x5a3ed0
  UnitMotion& m = u->motion;
  // a new goal while the steering drives: it keeps its waypoint until the navigator gives the next one
  bool keep = m.hasGoal && m.navDriven && m.hasWaypoint;
  bool braking = !m.hasWaypoint;  // after an abort: the brake spline runs on (SetGoal makes no steering call)
  m.hasGoal = true;
  m.arrived = false;
  m.failed = false;
  m.navDriven = true;
  if (keep) return;
  m.hasWaypoint = false;
  m.path.clear();
  m.pathIndex = 0;
  if (!braking) {
    SteeringStop(m);
    MotionStopTarget(u);
  }
}

void MotionSetWaypoint(Sim& sim, Unit* u, const Vec3& p, bool through) {
  UnitMotion& m = u->motion;
  (void)sim;
  m.path.assign(1, p);
  m.pathIndex = 0;
  m.passThrough = through;
  m.hasWaypoint = true;
  m.steerWpSet = true;
  m.steerWp = p;
  m.driveTick = 0;
  GoalCell(*m.bp, p.x, p.z, &m.goalCellX, &m.goalCellZ);
  m.newSegment = true;  // the steering takes it at its next tick (DriveToNextWaypoint)
}

void MotionNavDone(Unit* u, bool succeeded) {
  UnitMotion& m = u->motion;
  const bool wake = NavEventWakesCommandThread(u);  // (asked before the goal is gone)
  m.hasGoal = false;
  m.hasWaypoint = false;
  m.steerWpSet = false;  // (Execute's tail 0x5a43ef calls the navigator's MakeIdle: SetWaypoints(0, 0))
  m.steerValid = false;
  m.navDriven = false;
  m.path.clear();
  // CAiNavigatorLand::Execute 0x5a43dd: a finished path (state <= 1) first calls the steering's Stop(0) (vt+0x2c,
  // 0x5d35e0), then MakeIdle (0x5a43fb). Stop re-predicts with the spline gone (CheckCollisions, 0x5d3666) when the
  // deleted batch still had points to hand out (idx < points.size, 0x5d360e): a ship that reaches its goal cell
  // while its brake batch runs puts the records on the movers around it. Seen: Naval 545, the battleship 0x13
  // reaches its goal with 5 of its 10 points left; the Salem 0x44 braking behind it gets the record, clears it in
  // the same beat (not approaching: case 0), and that case's CheckCollisions puts the cruiser 0x3d's record on it;
  // at 546 it side-steps (twice the acceleration and the turn), and 0x3d brakes again (type 5) and stands until
  // 553. Ours made no prediction there: 0x44 stood until 549, 0x3d until 551.
  const bool unused = m.hasSpline && m.splineIdx < m.spline.size();
  SteeringStop(m);
  MotionStopTarget(u);
  if (unused) SteeringCheckCollisions(*Sim::From(u->luaState()), u);
  if (succeeded) m.arrived = true;
  else m.failed = true;
  if (wake) Sim::From(u->luaState())->ResumeCommandThread(u);  // the navigator's event wakes the move task
}

// AbortMove 0x5a3750 (land: SetSpeedThroughGoal(0) + MakeIdle -> SetWaypoints(0, 0): a brake spline).
void MotionStop(Unit* u) {
  UnitMotion& m = u->motion;
  ++m.stopCount;
  Sim& sim = *Sim::From(u->luaState());
  // MOHO64_DEBUG_STOP=<unit id> (or -2: every unit): each abort of a unit's move, with the calling frames
  static const long dbgStop = getenv("MOHO64_DEBUG_STOP") ? atol(getenv("MOHO64_DEBUG_STOP")) : -1;
  if (dbgStop == -2 || dbgStop == static_cast<long>(u->id)) {
    void* fr[8];
    const int n = CaptureFrames(fr, 8);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
      s += b;
    }
    Logf(LogLevel::Info, "stop %u unit %u goal %d from%s", sim.tick(), u->id, m.hasGoal ? 1 : 0, s.c_str());
  }
  if (m.air) {
    AirAbort(sim, u);
    m.hasGoal = false;
    m.hasWaypoint = false;
    m.navDriven = false;
    m.path.clear();
    SteeringStop(m);
    return;
  }
  if (m.nav) {
    LandNavSetSpeedThrough(u, false);
    LandNavStop(sim, u);
  }
  u->unitStates.Erase(US("ProblemGettingToGoal"));
  // (the motion's drive-through byte is only what SetSpeedThroughGoal(0) above makes of it: it changes when the
  // target point lies in the goal, else it keeps the last waypoint's value until the next waypoint. Seen: a tank
  // stopped on the way to a drive-through waypoint still reports 1 until its next goal.)
  if (!m.nav) m.passThrough = false;
  m.hasGoal = false;
  m.navDriven = false;
  SteeringClearWaypoints(sim, u);
}

const PathGrid* FootprintGrid(Sim& sim, const MotionBlueprint& b) {
  Navigation* nav = &sim.navigation();
  if (b.gridOwner != nav) {
    b.grid = nav->Grid(b.footprint);
    b.gridOwner = nav;
  }
  return b.grid;
}

const PathGrid* AltFootprintGrid(Sim& sim, const MotionBlueprint& b) {
  Navigation* nav = &sim.navigation();
  if (b.altGridOwner != nav) {
    b.altGrid = nav->Grid(b.altFootprint);
    b.altGridOwner = nav;
  }
  return b.altGrid;
}

bool UnitFootprintGridReady(Sim& sim, const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  return (u->altFootprintNav || u->altFootprintForced) ? b.altGridOwner == &sim.navigation()
                                                        : b.gridOwner == &sim.navigation();
}

const PathGrid* UnitFootprintGrid(Sim& sim, const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  return (u->altFootprintNav || u->altFootprintForced) ? AltFootprintGrid(sim, b) : FootprintGrid(sim, b);
}

namespace {

// The body follows the steering facing: at once, or at TurnFacingRate when the body may turn
// while moving (hover units).
void UpdateBody(UnitMotion& m) {
  const MotionBlueprint& b = *m.bp;
  if (b.motionType == kMotionHover && b.rotateBodyWhileMoving && b.turnFacingRate > 0) {  // (as GenBody)
    RotateToward(m.bx, m.bz, m.fx, m.fz, b.turnFacingRate * m.turnMult * kDegToRadTenth);
    float l = std::sqrt(m.bx * m.bx + m.bz * m.bz);
    if (l > 0) {
      m.bx /= l;
      m.bz /= l;
    }
  } else {
    m.bx = m.fx;
    m.bz = m.fz;
  }
}

// Can the unit's footprint stand at world position (x, z)? (Unit::WontFitAt 0x62aa90, negated:
// inside the map, terrain and structures at the footprint-origin cell; Entity::GetFootprint 0x62aaa0)
bool StandableAt(Sim& sim, const Unit* u, float x, float z) {
  // STIMap::IsWithin (0x62aad4): margin 0, the unit's army's whole-map flag
  if (sim.map() && !sim.IsWithin(x, z, 0.0f, Sim::UseWholeMap(u->army))) return false;
  const PathGrid* g = UnitFootprintGrid(sim, u);
  if (!g) return true;
  int cx, cz;
  GoalCell(UnitFootprint(u), x, z, &cx, &cz);
  // (0x62ab3f..0x62ab7f: OCCUPY_MobileCheck's caps without Sub for a unit on the Water layer, as Reach and the path
  // finder take them (landnav.cpp FitsReach). Seen (probe mp1_8660): MP1 8660, army 4's Seraphim destroyer 0x40004e
  // (SurfacingSub4x4) turning back toward (590, 280): its batch's look-ahead from point 5 reaches the cells of the
  // floating structure 0x4000cd (xsb4201); the original's point 6 brakes (state 4: speed 0.2940 -> 0.2427), ours,
  // keeping Sub, drove on in state 7 (0.2604).)
  return g->Passable(cx, cz, u->layer == "Water" ? kLayerSub : 0);
}

// The spline's look-ahead point (CAiPathSpline::Generate 0x5b2ff0): the new point plus the step
// set to the length max(SizeX, SizeZ) + stopping distance (the point itself when it did not move).
Vec3 LookAheadPoint(const MotionBlueprint& b, const Vec3& p, float sx, float sz, float stopDist) {
  float l = std::sqrt(sx * sx + sz * sz);
  float x = p.x, z = p.z;
  if (l > 0.0f) {
    float k = (std::max(b.sizeX, b.sizeZ) + stopDist) / l;
    x += sx * k;
    z += sz * k;
  }
  return Vec3{x, p.y, z};
}
bool LookAheadFits(Sim& sim, const Unit* u, const MotionBlueprint& b, const Vec3& p, float sx, float sz, float stopDist) {
  const Vec3 la = LookAheadPoint(b, p, sx, sz, stopDist);
  return StandableAt(sim, u, la.x, la.z);
}

// CAiSteeringImpl / CAiPathSpline::SteeringParams + the speed a turn allows (FA exe 0x699760):
// when the target lies inside the circle the unit can turn on, it slows to R * turnRate / 2,
// R being the radius of the arc from its position and facing to the target. RotateOnSpot units
// moving slower than RotateOnSpotThreshold (fraction of top speed) only drive when facing the
// target within cos^-1(0.98).
float TurnSpeedLimit(const MotionBlueprint& b, float maxF, float turnRadius, float turnRate, float fx, float fz,
                     float dx, float dz, float speedFrac) {
  if (b.rotateOnSpot && b.rotateOnSpotThreshold > speedFrac) {
    float n = 1.0f / std::sqrt(dx * dx + dz * dz);
    float dot = fz * (dz * n) + fx * (dx * n);  // (0x6997bf: the direction is scaled first)
    return dot < 0.9800000190734863f ? 0.0f : maxF;
  }
  float cross = fz * dx - fx * dz;
  float d2 = dx * dx + dz * dz;
  float r = cross == 0.0f ? 0.0f : std::fabs(d2 * 0.5f / cross);
  if (r >= turnRadius) return turnRadius;
  if (r == 0.0f) return maxF;
  return turnRate * r * 0.5f;
}

// The spline generator's working state: one step at a time from a position, velocity and facing.
struct Gen {
  Vec3 p, vel;
  float fx = 0, fz = 1, bx = 0, bz = 1;
  int state = 7;
  bool reverse = false;
  // the first step of a batch uses the real unit's speed fraction and facing dot (set before the loop)
  bool first = false;
  float frac0 = 0, dot0 = 0;
};

struct StepParams {
  float maxF, maxR, acc, brake, steer, turnRadius, turnRate;
  bool wide;
};

// The limits a batch is generated with (read when the batch is made: u+0x594 is the formation cap).
// The unit's top speed as the steering's parameters take it (u+0x594; see Params).
float SteerTopSpeed(const UnitMotion& m) {
  const MotionBlueprint& b = *m.bp;
  return m.maxSpeedU594 > 0 ? m.maxSpeedU594 : (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult);
}

StepParams Params(const UnitMotion& m) {
  const MotionBlueprint& b = *m.bp;
  StepParams P;
  // CAiSteeringImpl's parameters 0x6990e0 (0x6991d5): the top speed handed in is the unit's u+0x594, which
  // Unit::UpdateInfoCache already made of MaxSpeed x the speed multiplier (and the formation's cap); here it is
  // cut to the blueprint's MaxSpeed and multiplied by the speed multiplier AGAIN. So a multiplier below 1 counts
  // twice in what the unit drives (a submerged Seraphim destroyer, SubSpeedMultiplier 0.9 on MaxSpeed 5: 4.05,
  // while its u+0x594 says 4.5), a multiplier above 1 once. The reverse speed likewise (0x699206).
  // u+0x594 is what UpdateInfoCache LAST wrote, in the unit's own turn of a beat: a batch made in the steering
  // stage of the beat after a multiplier changed still finds the old value there. (Seen: a unit entering the
  // water, speed multiplier 0.9 from its layer change: the batch made the next beat tops at MaxSpeed x 0.9, the
  // one after it at MaxSpeed x 0.9 x 0.9.) Before the unit's first turn the live value stands in.
  const float u594 = SteerTopSpeed(m);
  P.maxF = std::min(b.maxSpeed, u594) * m.speedMult * 0.1f;
  P.maxR = std::min(b.maxSpeedReverse, u594) * m.speedMult * 0.1f;
  P.acc = b.maxAccel * m.accMult * 0.01f;
  P.brake = (b.maxBrake != 0.0f ? b.maxBrake : b.maxAccel) * m.accMult * 0.01f;
  P.steer = (b.maxSteerForce != 0.0f ? b.maxSteerForce : b.maxAccel) * m.accMult * 0.01f;
  P.turnRadius = b.turnRadius != 0.0f ? b.turnRadius / m.turnMult : 1e30f;
  P.turnRate = b.turnRate * m.turnMult * kDegToRadTenth;
  P.wide = b.turnRate < b.turnRadius;  // turns wider than it can rotate: slows in turns
  return P;
}

// The point's body direction. For a unit that is not a hover unit it is the point's direction itself: CalcMoveCommon
// 0x6c2297 makes the orientation the look rotation of point.dir (the front F of the step, spline_generate.md 3 / 4.1),
// with no turn at TurnFacingRate, whatever RotateBodyWhileMoving says. Seen: Skirmish 2012, army 6's Aeon Flare
// 0x600029 (ual0106: RotateBodyWhileMoving, TurnFacingRate 180) taking a sharp bend at about 20 degrees a beat: the
// original's heading was each point's direction (-68.1, -47.1, -26.9, -7.7 degrees); ours lagged at 18 a beat.
// (A hover unit's hull turns in HoverPointTurn, which does not read this; its body here is kept as it was.)
void GenBody(const MotionBlueprint& b, const UnitMotion& m, Gen& g) {
  if (b.motionType == kMotionHover && b.rotateBodyWhileMoving && b.turnFacingRate > 0) {
    RotateToward(g.bx, g.bz, g.fx, g.fz, b.turnFacingRate * m.turnMult * kDegToRadTenth);
    float l = std::sqrt(g.bx * g.bx + g.bz * g.bz);
    if (l > 0) {
      g.bx /= l;
      g.bz /= l;
    }
  } else {
    g.bx = g.fx;
    g.bz = g.fz;
  }
}

// How a fresh batch starts (Generate with a new path): the state from the speed, the facing and
// where the target lies.
// (spline_generate.md 1: dot and frac are the real unit's; back = the velocity points against the facing)
void ChooseStart(const MotionBlueprint& b, const StepParams& P, Gen& g, float dot, float frac, bool back) {
  g.reverse = false;
  g.state = 7;
  if (b.maxSpeedReverse <= 0.0f || b.rotateOnSpot) return;
  if (dot < 0.0f && (frac < 0.5f || P.wide)) g.state = (frac <= 0.0099999998f || back) ? 5 : 4;
  else if (!back) g.state = 7;
  else g.state = dot < 0.0f ? 5 : 6;
}

// One spline step (the loop body of CAiPathSpline::Generate 0x5b2ff0) toward tgt. Mode 0 stops at
// the target, 1 drives through it, 2 (a side-step) drives through with twice the accelerations and
// turn. False when the target is reached (no point).
bool StepMove(const MotionBlueprint& b, const StepParams& P, const UnitMotion& m, Gen& g, const Vec3& tgt, int mode) {
  float dx = tgt.x - g.p.x, dz = tgt.z - g.p.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  // at the target: no turn (the mode-0 cap brakes it to a stop)
  float tx = g.fx, tz = g.fz;
  if (dist > 0) {  // times 1 / dist (0x5b4281..)
    const float inv = 1.0f / dist;
    tx = dx * inv;
    tz = dz * inv;
  }
  float speed = std::sqrt(g.vel.x * g.vel.x + g.vel.z * g.vel.z);
  const int s = g.state;
  const bool backward = s == 5 || s == 6 || s == 2;
  float acc = P.acc, brake = P.brake;
  float turn = P.turnRate;
  if (speed / P.turnRadius >= turn) turn = speed / P.turnRadius;
  if (mode == 2) {
    acc *= 2;
    brake *= 2;
    turn *= 2;
  }
  float speedFrac0 = P.maxF > 0 ? speed / P.maxF : 0;
  float dotFG = g.fx * tx + g.fz * tz;
  if (g.first) {  // the first step of a batch: the real unit's values
    speedFrac0 = g.frac0;
    dotFG = g.dot0;
    g.first = false;
  }
  // The turn starts from a NORMALISED copy of the facing: TurnInfo 0x6990e0 keeps one in its result (out+0x28,
  // normalised by 0x6999f0: times 1 / length) and Generate hands that copy to the limited turn (0x5b386c). The
  // turned vector itself goes into the point. So the facing is brought back to length 1 once per step; without
  // it the last bit of a turning unit's heading differed from the original's on some beats.
  float nfx = g.fx, nfz = g.fz;
  {
    const float l = std::sqrt(nfx * nfx + nfz * nfz);
    if (l > 9.99999997e-07f) {
      const float inv = 1.0f / l;
      nfx *= inv;
      nfz *= inv;
    } else {
      nfx = nfz = 0.0f;
    }
  }
  // TurnSpeedLimit 0x699760 reads the same normalised copy (K+0x28 / +0x2c), not the facing as the last step left
  // it (after a brake that is the hull's 3-D forward, shortened by its pitch)
  float limit = dist > 0 ? TurnSpeedLimit(b, P.maxF, P.turnRadius, P.turnRate, nfx, nfz, dx, dz, speedFrac0) : P.maxF;
  // (the limited turn 0x6992c0 is handed TurnInfo's difference vector as it is, target - place, not a unit
  // vector: when the turn ends inside the step the facing becomes that vector times |facing| / |vector|, and
  // with a unit vector in its place the last digit of a ship's heading on a straight run differed)
  const float rtx = dist > 0 ? dx : tx, rtz = dist > 0 ? dz : tz;
  if (!g.reverse) {
    RotateToward(nfx, nfz, rtx, rtz, turn);
    g.fx = nfx;
    g.fz = nfz;
  } else {
    float bx = -nfx, bz = -nfz;
    RotateToward(bx, bz, rtx, rtz, turn);
    g.fx = -bx;
    g.fz = -bz;
  }
  float mx = backward ? -g.fx : g.fx, mz = backward ? -g.fz : g.fz;
  float ml2 = mx * mx + mz * mz;
  // remove the sideways velocity (at most the steering force)
  float px = 0, pz = 0;
  if (ml2 > 0.0f) {
    float k = (g.vel.x * mx + g.vel.z * mz) / ml2;
    px = k * mx;
    pz = k * mz;
  }
  float lx = g.vel.x - px, lz = g.vel.z - pz;
  float ll = lx * lx + lz * lz;
  if (P.steer * P.steer < ll) {
    float k = P.steer / std::sqrt(ll);
    lx *= k;
    lz *= k;
  }
  g.vel.x -= lx;
  g.vel.z -= lz;
  float target = 0;
  bool braking = s == 1 || s == 3 || s == 4 || s == 6;
  if (!braking) {
    target = std::min(limit, (s == 5 || s == 2) ? P.maxR : P.maxF);
    if (mode == 0) {
      float d = dist;
      if (brake < dist) d = std::sqrt(dist * brake + dist * brake);
      if (d <= target) target = d;
    }
    if (P.wide) target = (std::max(-0.5f, g.reverse ? -dotFG : dotFG) + 1.0f) * 0.5f * target;
    if (target < 0.0010000000474974513f) braking = true;  // (the small target is kept)
  } else {
    target = 0;
  }
  if (braking) {  // keep the speed, ease the direction toward the move direction, brake
    float vl = std::sqrt(g.vel.x * g.vel.x + g.vel.z * g.vel.z);
    float sx = mx, sz = mz;
    if (ml2 != 0.0f) {
      float k = vl / std::sqrt(ml2);
      sx = mx * k;
      sz = mz * k;
    }
    g.vel.x = sx * 0.200000003f + g.vel.x * 0.800000012f;
    g.vel.z = sz * 0.200000003f + g.vel.z * 0.800000012f;
  }
  float dvx = mx * target - g.vel.x, dvz = mz * target - g.vel.z;
  float lim = (dvx * g.vel.x + dvz * g.vel.z > 0.0f) ? acc : brake;
  float dl = dvx * dvx + dvz * dvz;
  if (lim * lim < dl) {
    float k = lim / std::sqrt(dl);
    dvx *= k;
    dvz *= k;
  }
  g.vel.x += dvx;
  g.vel.z += dvz;
  float cap = backward ? P.maxR : P.maxF;
  float vl2 = g.vel.x * g.vel.x + g.vel.z * g.vel.z;
  if (cap * cap < vl2) {
    float k = cap / std::sqrt(vl2);
    g.vel.x *= k;
    g.vel.z *= k;
  }
  g.p.x += g.vel.x;
  g.p.z += g.vel.z;
  GenBody(b, m, g);
  return true;
}

// The state rules after a point (land_motion_blocking.md 2.2). True: the batch ends here without
// saving a state (the continuation starts in state 7).
bool StepRules(Sim& sim, const Unit* u, const MotionBlueprint& b, const StepParams& P, Gen& g, const Vec3& tgt, int mode) {
  float nspeed = std::sqrt(g.vel.x * g.vel.x + g.vel.z * g.vel.z);
  float frac = P.maxF > 0 ? nspeed / P.maxF : 0;
  float ndx = tgt.x - g.p.x, ndz = tgt.z - g.p.z;
  float ndist = std::sqrt(ndx * ndx + ndz * ndz);
  // ndot: the direction times 1 / ndist (divss), summed z, x, then the zero y (0x5b4281..0x5b42ec)
  float ndot = 1.0f;
  if (ndist > 0) {
    const float ninv = 1.0f / ndist;
    ndot = (g.fz * (ndz * ninv) + (ndx * ninv) * g.fx) + (ninv * 0.0f) * 0.0f;
  }
  float stopDist = 0;
  float a = std::max(b.maxAccel, b.maxBrake);
  if (a > 0.0f) {  // (n * 10)^2 / (a * 2) (0x5b43ca..0x5b43e0)
    const float s10 = nspeed * 10.0f;
    stopDist = (s10 * s10) / (a * 2.0f);
  }
  switch (g.state) {
    case 3:
      if (frac <= 0.0099999998f) return true;
      break;
    case 4:
      if (frac <= 0.0099999998f) g.state = (b.maxSpeedReverse <= 0.0f || b.rotateOnSpot) ? 7 : 5;
      break;
    case 5:
      if (!LookAheadFits(sim, u, b, g.p, g.vel.x, g.vel.z, stopDist)) g.state = 6;
      else if (!g.reverse ? ndot > 0.150000006f : stopDist > ndist) g.state = 6;
      break;
    case 6:
      if (frac <= 0.0099999998f) {
        if (g.reverse) return true;
        g.state = 7;
      }
      break;
    case 7:
      if (mode == 0 && ndist < stopDist) g.state = 3;
      if (ndot < 0.865999997f) {
        // 0x5b456b..0x5b4654: the look-ahead point must fit (Unit::WontFitAt 0x62aa90, 0x5b460e); and for a unit that
        // turns wider than it rotates (P.wide, [esp+0x3b]) no unit may stand there either: 0x62aba0 with flags 2, the
        // footprint's cell at the point (fistp of x, z less half the footprint) through COGrid::UnitIsBlocked 0x721340,
        // everyone blocking. Either brakes (state 4). Seen (probe mp1_15420): MP1 15419, army 0's Tempest 0x169
        // (TurnRate 35 < TurnRadius 40) side-steps army 6's frigate 0x6000a5; its new batch's look-ahead from point 1
        // falls on the frigate 0x600094 (960.6, 577.9): the original brakes from point 2 (0.198 -> 0.144 -> 0.091 ->
        // 0.041, then backs up); ours, with the terrain test alone, drove on at 0.2.
        const Vec3 la = LookAheadPoint(b, g.p, g.vel.x, g.vel.z, stopDist);
        if (!StandableAt(sim, u, la.x, la.z)) {
          g.state = 4;
        } else if (P.wide) {
          int cx, cz;
          GoalCell(UnitFootprint(u), la.x, la.z, &cx, &cz);
          if (PathUnitBlocked(const_cast<Unit*>(u), cx, cz, 2)) g.state = 4;
        }
      }
      break;
    default:
      break;
  }
  return false;
}

UnitMotion::SplinePoint PointOf(const Gen& g) {
  UnitMotion::SplinePoint q;
  q.pos = g.p;
  q.vel = g.vel;
  q.fx = g.fx;
  q.fz = g.fz;
  q.bx = g.bx;
  q.bz = g.bz;
  q.state = g.state;
  return q;
}

// The height a point of a batch is stored with: STIMap::GetElevation 0x44fb90 at the point, and for MotionType 3,
// 7 and 8 (Water, Hover, AmphibiousFloating) the water surface where that is higher (map+0x1534 / +0x1538).
// Generate 0x5b40fd..0x5b417c and Update 0x5b2ecb..0x5b2f4d write it just before they append the point. The motion
// takes its own height (CalcMoveCommon), so only the readers of the stored points see it: the collision
// prediction (Predict 0x597800: its 3-D distance test, and its 3-beat "velocities" whose stopping distances count
// the climb) and ResolvePossibleCollision 0x596f30. (Ours kept the unit's height at generation for every point.
// Seen: LAND, Pillar 10 and Archer 3 both climbing about 0.05 a beat; with the climb counted, the meeting that
// Pillar 10's batch of 1242 predicts comes due at +6 (1248), where it no longer holds, instead of +9 (1251), where
// ours side-stepped the Pillar while the original drove it on and braked the Archer by its own record at 1252.)
float PointHeight(const Sim& sim, const MotionBlueprint& b, float x, float z, float fallback) {
  const TerrainMap* map = sim.map();
  if (!map) return fallback;
  const bool surface =
      b.motionType == kMotionWater || b.motionType == kMotionHover || b.motionType == kMotionAmphibiousFloating;
  return surface ? map->SurfaceHeight(x, z) : map->TerrainHeight(x, z);
}

// Batch size: 20 points, 5 in a form formation, three times that for wide turners.
int BatchSize(Sim& sim, Unit* u) {
  int n = 20;
  Formation* F = GetFormation(sim, u);
  if (F && FormationIsForm(*F)) n = 5;
  if (u->motion.bp->turnRadius > u->motion.bp->turnRate) n *= 3;
  return n;
}

// CAiPathSpline::Generate 0x5b2ff0.
void Generate(Sim& sim, Unit* u, const Vec3& tgt, int mode, bool fresh) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  const bool cont = !fresh && m.hasSpline;
  const UnitMotion::SplinePoint last = m.genLast;
  m.spline.clear();
  m.splineIdx = 0;
  m.hasSpline = true;
  m.splineMode = mode;
  float ex = tgt.x - u->position.x, ez = tgt.z - u->position.z;
  if (std::sqrt(ex * ex + ez * ez) < 0.0010000000474974513f) return;
  StepParams P = Params(m);
  Gen g;
  // the real unit: its (terrain-tilted) forward, distance, facing dot and speed fraction
  const Quat& q = u->orientation;
  Vec3 D{2 * (q.w * q.y + q.x * q.z), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
  if (std::fabs(D.y) > 0.99f) D = {0, 0, 1};
  float dist = std::sqrt(ex * ex + ez * ez);
  // the 3-D forward (shortened on a slope) against the direction times 1 / dist, summed z, y, x (0x5b32de,
  // 0x5b3343..0x5b3374; dist is at least 0.001 here)
  const float dinv = 1.0f / dist;
  float dot = ((ez * dinv) * D.z + (dinv * 0.0f) * D.y) + (ex * dinv) * D.x;
  const Vec3& V = UnitVelocity(u);
  // (0x5b3381..0x5b33b6: the squares summed x, z, y)
  float frac = b.maxSpeed > 0 ? std::sqrt((V.x * V.x + V.z * V.z) + V.y * V.y) * 10.0f / b.maxSpeed : 0;
  if (cont) {
    g.p = last.pos;
    g.vel = last.vel;
    g.fx = last.fx;
    g.fz = last.fz;
    g.bx = last.bx;
    g.bz = last.bz;
    g.state = m.savedState ? m.savedState : 7;
    g.reverse = false;
  } else {
    g.p = u->position;
    g.vel = {V.x, 0, V.z};
    // (TurnInfo's forward, normalised by 0x6999f0: times 1 / length. A division gives another last digit on some
    // headings, and the first point of a fresh batch then turns a ship one float step beside the original's.)
    float l = std::sqrt(D.x * D.x + D.z * D.z);
    const float li = l > 9.99999997e-07f ? 1.0f / l : 0.0f;
    g.bx = l > 9.99999997e-07f ? D.x * li : 0;
    g.bz = l > 9.99999997e-07f ? D.z * li : 1;
    // The first step is handed the RAW forward: the loop's pre-step state is D as the quaternion gives it (0x5b35a6:
    // +0x258 = D), and SteeringParams 0x6990e0 normalises its horizontal part once (0x6999f0), which StepMove does.
    // Normalised here too, the facing went through 0x6999f0 twice, and twice is not always once: on some headings
    // the second pass moved its last digit. (Seen: MP3 3297, army 1's commander 0x100000 starts a batch from
    // heading (0.76704, 0.64157): normalised once, its first point's x is 266.107788 as in the original; twice, it
    // was one float step short, the gap lived on, and a side-step at 3396 then turned another unit the other way.)
    g.fx = l > 9.99999997e-07f ? D.x : 0;
    g.fz = l > 9.99999997e-07f ? D.z : 1;
    ChooseStart(b, P, g, dot, frac, (V.z * D.z + V.y * D.y) + V.x * D.x < 0.0f);  // (0x5b34c5..0x5b34e9)
  }
  // the back-up flag is chosen when a batch starts (fresh, or continuing in state 5), from the real unit
  if (g.state == 5 && dist < b.backUpDistance && dot < -0.5f) g.reverse = true;
  g.first = true;
  g.frac0 = frac;
  g.dot0 = dot;
  const int N = BatchSize(sim, u);
  const bool realAt = AtPosition(u, tgt);  // tested with the unit's real position
  m.savedState = 0;
  // spline+0x24c..0x264 hold the state before the last step: the batch is renewed while its last
  // point is still unused, and the continuation recomputes that point
  UnitMotion::SplinePoint before = PointOf(g);
  static const long dbgPts = getenv("MOHO64_DEBUG_MOTION") ? atol(getenv("MOHO64_DEBUG_MOTION")) : -1;
  if (dbgPts == static_cast<long>(u->id))
    fprintf(stderr, "spline start: tick %u state %d rev %d pos %.4f %.4f vel %.4f %.4f f %.4f %.4f cont %d\n", sim.tick(),
            g.state, g.reverse ? 1 : 0, g.p.x, g.p.z, g.vel.x, g.vel.z, g.fx, g.fz, cont ? 1 : 0);
  // CAiPathSpline::Generate 0x5b364b: a unit of motion type AmphibiousFloating (8; the Cybran destroyer that
  // walks ashore) whose LayerChangeOffsetHeight is below -0.5 slows down in the shallows. Per point, at the place
  // the step starts from: with h = terrain - water, between 3 x the offset and 0 the top speed is multiplied by
  // h / (3 x offset), not below a half; deeper, or on land, it is whole.
  const bool shallows = b.motionType == kMotionAmphibiousFloating && -0.5f > b.layerChangeOffset && sim.map() &&
                        sim.map()->hasWater;
  for (int n = 0;;) {
    before = PointOf(g);
    StepParams Pn = P;
    if (shallows) {
      // (0x5b364b..0x5b36ea: the factor f (1, or h / deep but not below 0.5) multiplies the top speed handed to the
      // steering's parameters 0x6990e0 (0x5b36ea: f x u+0x594, [esp+0x148] from 0x5b30c6), which cut it to the
      // blueprint's MaxSpeed and MaxSpeedReverse and multiply by the speed multiplier and 0.1 (0x6991d5..0x699226):
      // so the reverse cap is scaled too, and a MaxSpeed below f x u+0x594 is the cap whole. Ours scaled the
      // finished forward cap, P.maxF x f, and left the reverse one.)
      const float deep = b.layerChangeOffset * 3.0f;
      const float h = sim.map()->TerrainHeight(g.p.x, g.p.z) - sim.map()->waterElevation;
      float f = 1.0f;
      if (h > deep && 0.0f > h) {
        f = h / deep;
        if (!(f > 0.5f)) f = 0.5f;
      }
      const float top = f * SteerTopSpeed(m);
      Pn.maxF = std::min(b.maxSpeed, top) * m.speedMult * 0.1f;
      Pn.maxR = std::min(b.maxSpeedReverse, top) * m.speedMult * 0.1f;
    }
    if (!StepMove(b, Pn, m, g, tgt, mode)) break;
    g.p.y = PointHeight(sim, b, g.p.x, g.p.z, u->position.y);  // (0x5b40fd)
    m.spline.push_back(PointOf(g));
    ++n;
    float rx = tgt.x - g.p.x, rz = tgt.z - g.p.z;
    bool end = false;
    if (n >= N || std::sqrt(rx * rx + rz * rz) < 0.0010000000474974513f) {
      m.savedState = g.state;
      end = true;
    }
    if (realAt) end = true;
    const int s0 = g.state;
    // (the state rules' speed fraction is the point's: nspeed / its own forward cap, 0x5b3eec)
    if (!end && StepRules(sim, u, b, Pn, g, tgt, mode)) end = true;
    if (dbgPts == static_cast<long>(u->id))
      fprintf(stderr, "  pt %d state %d->%d rev %d pos %.4f %.4f vel %.4f %.4f f %.4f %.4f end %d\n", n, s0, g.state,
              g.reverse ? 1 : 0, g.p.x, g.p.z, g.vel.x, g.vel.z, g.fx, g.fz, end ? 1 : 0);
    if (end) break;
  }
  m.genLast = before;
  m.genReverse = g.reverse;
  static const long dbgId = getenv("MOHO64_DEBUG_MOTION") ? atol(getenv("MOHO64_DEBUG_MOTION")) : -1;
  if (dbgId == static_cast<long>(u->id))
    fprintf(stderr, "spline: tick %u mode %d fresh %d n %zu tgt %.2f %.2f saved %d end %.4f %.4f\n", sim.tick(), mode,
            fresh ? 1 : 0, m.spline.size(), tgt.x, tgt.z, m.savedState, g.p.x, g.p.z);
}

// CAiPathSpline::Update 0x5b26c0 (modes 3/4): brake to a stop along the facing (move_handoff.md 5.5).
void BrakeSpline(Sim& sim, Unit* u, int mode) {
  // mode 4 (collision brake) is the same stop sequence with doubled limits (recorder: pond_path's brake and
  // its two standing ticks match only this form)
  UnitMotion& m = u->motion;
  StepParams P = Params(m);
  m.spline.clear();
  m.splineIdx = 0;
  m.hasSpline = true;
  m.splineMode = mode;
  float acc = P.acc, brake = P.brake;
  if (mode == 4) {
    acc *= 2;
    brake *= 2;
  }
  // CAiPathSpline::Update 0x5b26c0 asks the unit for its orientation and position (vt+0x18 0x5b270d, vt+0x14
  // 0x5b278e): the entity's committed transform, which this beat's step only replaces in Entity::AdvanceCoords
  // (step 13). A unit stopped after its own turn of the motion loop and before step 13 brakes from where the beat
  // found it (its velocity is the new one, vt+0x3c); one stopped by an order of the command stream, between beats,
  // from where it is (Skirmish 680, army 7's 0x700005).
  // (Seen with the probe: Skirmish 2949, engineer 0x2f stopped by IssueClearCommands from M28's death handler of
  // 0x57, which the motion loop tears down after 0x2f's step: Update(3) runs with the unit at 734.4062, the beat's
  // start, and 0x2f's 2950 step lands on that brake's second point, 734.1437. Ours braked from 734.2187.)
  const bool stepped = sim.coordsPending && u->lastPosTick == sim.tick();
  const Quat& q0 = stepped ? u->lastOrientation : u->orientation;
  // F: the quaternion's forward column as 0x5b270f..0x5b2785 forms it (2 (wy + zx), 2 (zy - wx), 1 - 2 (yy + xx)),
  // not a rotation of (0, 0, 1); V: SetLength 0x5b1c90 of F to the speed, one factor speed / |F| times each
  // component, not each component times the speed over |F|. The two forms differ in the last bit. (The factor is
  // never stored: fsqrt 0x40eaf0, fdivr, then the three fmul from the x87 stack (0x5b1cd6..0x5b1cf7); at PC_24 each
  // of those rounds to 24 significant bits as the float operations here do, only the exponent range is wider.)
  // Seen: MP3 9750, army 1's 0x10006e brakes for 0x100105 (collision brake): ours' first brake point lay one float
  // step off in x (284.7557678 for the original's 284.7557373), its tilt and height followed, and at 9965 a box test
  // that tied two axes to the bit went the other way (a push the original does not make).
  Vec3 F = vm::Forward(q0);
  m.brakeDir = F;
  int pointState = 1;  // (the brake points' state word: 1 from the start, 0x5b27f0)
  const Vec3& lm = UnitVelocity(u);
  float s = std::sqrt(lm.x * lm.x + lm.y * lm.y + lm.z * lm.z);
  const float fl2 = (F.x * F.x + F.y * F.y) + F.z * F.z;
  Vec3 V = F;
  if (fl2 > 0.0f) {
    const float k = s / std::sqrt(fl2);
    V = Vec3{F.x * k, k * F.y, k * F.z};
  }
  {  // the velocity times 1 / s (FLT_MAX parts at s == 0), its dot with F summed y, z, x (0x5b2889..0x5b2997)
    Vec3 un{3.40282347e+38f, 3.40282347e+38f, 3.40282347e+38f};
    if (s != 0.0f) {
      const float inv = 1.0f / s;
      un = {lm.x * inv, lm.y * inv, lm.z * inv};
    }
    if (0.0f > (un.y * F.y + un.z * F.z) + un.x * F.x) {
      V = Vec3{-0.0f - V.x, -0.0f - V.y, -0.0f - V.z};
      pointState = 2;  // (0x5b299d: braking a backward motion)
    }
  }
  float fxz = std::sqrt(F.x * F.x + F.z * F.z);
  float dfx = fxz > 0 ? F.x / fxz : m.fx, dfz = fxz > 0 ? F.z / fxz : m.fz;
  Gen g;
  g.p = stepped ? u->lastPosition : u->position;
  g.vel = Vec3{V.x, 0, V.z};
  // Every brake point carries dir = F (CAiPathSpline::Update: spline+0x258 too), so the batch that CONTINUES
  // after the stop (PSM's used-up rule, Generate with fresh = 0) starts from the hull's forward axis as it lay
  // when the brake began, flattened by SteeringParams: not from the flat facing the steering had. (The hull
  // itself looks along F while it brakes: UnitMotion::brakeDir. The body direction is left alone.)
  g.fx = dfx;
  g.fz = dfz;
  g.bx = m.bx;
  g.bz = m.bz;
  for (int n = 0; n < 400; ++n) {
    m.genLast = PointOf(g);
    // The continuation's start (spline+0x258) is F itself, neither projected nor renormalised: Generate hands it to
    // SteeringParams 0x6990e0, whose 0x6999f0 normalises its (x, -z) ONCE, by times 1 / length. Ours started from
    // F.xz / |F.xz| (a division) and StepMove normalised that again: one float step beside it. (Seen: LAND, a Titan's
    // first step after a stop at 1472 looked one step aside, so its quaternion was one ulp off; the original's,
    // brought back to unit length at 1473, moved by one ulp, and the walk animation, which advances while the hull
    // "turns", took one more step at 1474: its gun's aim then differed and it fired a shot the original did not.)
    m.genLast.fx = F.x;
    m.genLast.fz = F.z;
    bool stop = false;
    float vl2 = g.vel.x * g.vel.x + g.vel.z * g.vel.z;
    if (vl2 > 0) {
      float dvx = -g.vel.x, dvz = -g.vel.z;
      float lim = (dvx * g.vel.x + dvz * g.vel.z > 0.0f) ? acc : brake;
      float dl = dvx * dvx + dvz * dvz;
      if (lim * lim < dl) {
        float k = lim / std::sqrt(dl);
        dvx *= k;
        dvz *= k;
      }
      g.vel.x += dvx;
      g.vel.z += dvz;
      float v2 = g.vel.x * g.vel.x + g.vel.z * g.vel.z;
      if (P.maxF * P.maxF < v2) {
        float k = P.maxF / std::sqrt(v2);
        g.vel.x *= k;
        g.vel.z *= k;
        v2 = g.vel.x * g.vel.x + g.vel.z * g.vel.z;
      }
      if (v2 <= 1e-6f) {
        g.vel.x = g.vel.z = 0;
      } else {
        g.p.x += g.vel.x;
        g.p.z += g.vel.z;
      }
    }
    bool zero = g.vel.x == 0 && g.vel.z == 0;
    if (zero && (mode == 3 || (mode == 4 && m.spline.size() > 5))) stop = true;
    g.p.y = PointHeight(sim, *m.bp, g.p.x, g.p.z, g.p.y);  // (0x5b2ecb)
    m.spline.push_back(PointOf(g));
    m.spline.back().state = stop ? 8 : pointState;  // (0x5b2ec3: the last point 8, then 0x5b2fb3..0x5b2fb7)
    if (stop) break;
  }
  m.savedState = 0;
  m.genReverse = false;
}

}  // namespace

void SteeringClearWaypoints(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  m.path.clear();
  m.pathIndex = 0;
  m.hasWaypoint = false;
  m.steerWpSet = false;
  m.steerValid = false;
  m.newSegment = false;
  if (!m.hasSpline) return;
  m.colType = 0;
  m.colUnit = 0;
  m.colTick = 0xffffffffu;
  m.sideStep = {};
  if (!u->dead && !u->destroyQueued && m.bp) BrakeSpline(sim, u, 3);
  // 0x5d29c0: "notify neighbours" = CheckCollisions with the brake spline, before the hand-out. A unit that is
  // told to stop while another one is heading for its path puts the record on that one at once (seen: a flak
  // brakes hard the beat after the unit crossing in front of it was stopped by its attack order).
  SteeringCheckCollisions(sim, u);
  if (m.splineIdx < m.spline.size()) {
    m.point = m.spline[m.splineIdx++];
    m.pointNow = true;
    m.handOutTick = sim.tick();
  }
}

void UpdatePath(Sim& sim, Unit* u, const Vec3& tgt, bool fresh, int mode) {
  UnitMotion& m = u->motion;
  m.colType = 0;
  m.colUnit = 0;
  m.colTick = 0xffffffffu;
  if (u->dead || u->destroyQueued || !m.bp) return;
  if (mode == 3 || mode == 4) BrakeSpline(sim, u, mode);
  else Generate(sim, u, tgt, mode, fresh);
}

bool AtPosition(const Unit* u, const Vec3& q) {
  const NamedFootprint& f = UnitFootprint(u);  // (Unit::IsAtPosition 0x62cc40: Entity::GetFootprint 0x62cc52)
  int ax, az, bx, bz;
  GoalCell(f, u->position.x, u->position.z, &ax, &az);
  GoalCell(f, q.x, q.z, &bx, &bz);
  return ax == bx && az == bz;
}

void AddImpulse(Unit* u, const Vec3& imp) {
  if (u->dead || u->destroyQueued || u->beingBuilt) return;
  UnitMotion& m = u->motion;
  if (!m.bp) return;
  if (m.pointNow) m.wasMoving = true;  // "was moving when pushed"
  // 0x6b8b9d..0x6b8c23: vel = 0.5*vel + imp and accel = 0.5*accel + imp, all three components (imp.y is 0, so the
  // vertical velocity halves too)
  m.vel = {m.vel.x * 0.5f + imp.x, m.vel.y * 0.5f + imp.y, m.vel.z * 0.5f + imp.z};
  m.accel = {m.accel.x * 0.5f + imp.x, m.accel.y * 0.5f + imp.y, m.accel.z * 0.5f + imp.z};
  // 0x6b8e60: |vel| (3-D) > u+0x594 * 0.2 -> SetLength (0x5b1c90) and accel = vel. u+0x594 is what the unit's
  // last UpdateInfoCache wrote; the Unit ctor leaves it 0 (0x6a567b), so a unit pushed before its first
  // MotionTick (spawned this beat, later in the entity order than its pusher) keeps no velocity at all: it is
  // only marked pushed
  float cap = m.maxSpeedU594 * 0.2f;
  float l2 = (m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z;
  if (std::sqrt(l2) > cap && l2 > 0) {
    float k = cap / std::sqrt(l2);
    m.vel = {m.vel.x * k, m.vel.y * k, m.vel.z * k};
    m.accel = m.vel;
  }
  m.surfaceNext = true;
  m.pushed = true;
}

namespace {

// Without a goal: the unit coasts to a stop (CUnitMotion::CalcMoveCommon, no spline). turned: the hull turned on
// the spot in this beat; the "nearly stopped" test is then skipped (0x6c27a1) and the formula below runs on
// whatever velocity is left, so a unit that turns while its last crawl dies away takes one more tiny step.
bool Coast(Unit* u, bool turned = false) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  // 0x6c2761..0x6c278a: every path without a spline point (after the turn, and the two jumps to 0x6c2768 from
  // 0x6c25d4 / 0x6c25e3) sets the acceleration (m+0x44) to zero before the velocity is read. CalcMoveHover leans on
  // it (0x6c2c0a), so a hover unit pushed by a collision does not lean on the push. (Seen: bench 654, Seraphim
  // engineer 0x20000a pushed back (velocity -0.19 -> +0.10) at its factory's edge next to 0x200008: its hull stays
  // level in the original; ours kept the impulse in m.accel and leant 0.07, 0.12, 0.17 over three beats.)
  m.accel = {};
  // v = the entity velocity (m+0x38 for a land unit, 3-D: a fresh spline point's terrain y counts too)
  float v2 = (m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z;  // LimitLengthTo 0x5d1e70's own sum
  // the "nearly stopped" test sums y, z, x (0x6c27a8..0x6c27db)
  if (!turned && kStopSq > (m.vel.y * m.vel.y + m.vel.z * m.vel.z) + m.vel.x * m.vel.x) {
    m.vel = {};
    m.prevPos = u->position;
    return false;
  }
  float brake = (b.maxBrake > 0.0f ? b.maxBrake : b.maxAccel) * (m.accMult * 0.01f);  // (0x6c2845, then 0x6c2866)
  float bx = m.vel.x, by = m.vel.y, bz = m.vel.z;
  if (v2 > brake * brake) {
    float k = brake / std::sqrt(v2);
    bx *= k;
    by *= k;
    bz *= k;
  }
  m.vel.x = m.vel.x * 0.800000011920929f - bx;
  m.vel.y = m.vel.y * 0.800000011920929f - by;
  m.vel.z = m.vel.z * 0.800000011920929f - bz;
  // CalcMoveCommon applies the new velocity even when it is tiny (|v| < brake gives -0.2 v: a small step back)
  u->position.x += m.vel.x;
  u->position.y += m.vel.y;
  u->position.z += m.vel.z;
  m.prevPos = u->position;
  return m.vel.x != 0 || m.vel.z != 0 || m.vel.y != 0;
}

}  // namespace

namespace combat {
bool HasTarget(Sim& sim, const AiTarget& t);
}  // namespace combat

// RotateToward 0x6992c0: the vector c turned toward w by at most th, in the original's order of operations (the
// last digit of the result is the hull's quaternion's). False: one of the two has no length, c stays.
static bool RotateTowardExe(float th, float cx, float cz, float wx, float wz, float* nx, float* nz) {
  const float lc = std::sqrt(cz * cz + cx * cx), lw = std::sqrt(wz * wz + wx * wx);
  const float k0 = dmath::Cos(th);  // fcos
  const float ll = lw * lc;
  if (ll == 0) return false;
  if (wz * cz + wx * cx >= k0 * ll) {  // within one step: the wanted direction at c's length (0x6993a7)
    const float ratio = lc / lw;
    *nx = wx * ratio;
    *nz = wz * ratio;
    return true;
  }
  const float t2 = th * th;
  float sn = ((t2 * 0.00761f - 0.16605f) * t2 + 1.0f) * th;
  if (wz * cx - cz * wx < 0) sn = -0.0f - sn;
  float k = k0;
  if (std::fabs((sn * sn + k * k) - 1.0f) > 0.001f) {  // 0x6999f0: times 1 / length
    const float r = std::sqrt(k * k + sn * sn);
    if (r > 9.99999997e-07f) {
      const float ri = 1.0f / r;
      k *= ri;
      sn *= ri;
    } else {
      k = sn = 0;
    }
  }
  *nx = k * cx - sn * cz;
  *nz = k * cz + sn * cx;
  return true;
}

// CalcMoveCommon's no-spline branch (formations.md 12): an idle unit of a form formation turns on the
// spot to the formation's forward vector (Unit::GetFormationVector), TurnInfo 0x6990e0 / RotateToward
// 0x6992c0, in the negated-z frame. Returns true when it turned.
bool ArrivalTurn(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  // a weapon slaved to the body comes first (0x6c1e9b); only without one the formation's forward (0x6c25ca)
  Vec3 fv{};
  if (!SlavedFaceVector(sim, u, &fv)) {
    if (LandNavActive(u)) return false;  // the navigator is not idle (0x6c25d4)
    if (u->form && FormationIsForm(*u->form) && !u->unitStates.Has(US("TransportLoading")) &&
        !u->unitStates.Has(US("Refueling")) && !combat::HasTarget(sim, u->desiredTarget))
      fv = FormationVector(sim, u);
    // (both tests are memcmps with the zero vector, 0x6c25fe and 0x6c2631: a -0.0 part is not zero)
    if (vm::BitsZero(fv)) fv = m.faceDir;  // (0x6c2607: the motion's own facing)
    if (vm::BitsZero(fv)) return false;
    // T = pos + fv (0x6c2620), and TurnInfo takes the place off again: the vector as the float sum left it
    const Vec3 T{u->position.x + fv.x, u->position.y + fv.y, u->position.z + fv.z};
    fv = Vec3{T.x - u->position.x, T.y - u->position.y, T.z - u->position.z};
  }
  if (fv.x == 0 && fv.y == 0 && fv.z == 0) return false;
  const MotionBlueprint& b = *m.bp;
  // current forward (x, -z): the hull's own forward axis, tilt included, flattened and normalised (TurnInfo
  // 0x698ff0 -> 0x6990e0 / 0x6999f0). On a slope it is a little off the flat heading the hull was built from, so
  // "face where you face" (a slaved weapon with its target inside the arc) is no turn at all.
  const Quat& q = u->orientation;
  float cx = (q.w * q.y + q.z * q.x) * 2.0f, cz = -(1.0f - (q.y * q.y + q.x * q.x) * 2.0f);
  if (const float cl = std::sqrt(cx * cx + cz * cz); cl > 9.99999997e-07f) {  // 0x6999f0: times 1 / length
    const float ci = 1.0f / cl;
    cx *= ci;
    cz *= ci;
  } else {
    cx = m.bx;
    cz = -m.bz;
  }
  float wx = fv.x, wz = -fv.z;          // toward T = pos + fv
  float len = std::sqrt(wx * wx + wz * wz);
  if (len <= 0) return false;
  // (0x6c2515: the direction is the vector times 1 / len, not divided by len; near the threshold the last bit
  // of the product decides whether a slowly turning ship takes its next degree this beat or the next)
  const float inv = 1.0f / len;
  if (cz * (wz * inv) + cx * (wx * inv) >= 0.9999f) return false;
  float rate = (b.motionType == kMotionHover ? b.turnFacingRate : b.turnRate) * m.turnMult * 0.0017453292f;
  float th = std::min(rate, 3.14159274f);
  float nx, nz;
  if (!RotateTowardExe(th, cx, cz, wx, wz, &nx, &nz)) return false;
  float l = std::sqrt(nx * nx + nz * nz);
  if (l <= 0) return false;
  m.turnQ = TurnQuat(nx, nz);  // (0x699940: from the turned vector as it is)
  m.fx = m.bx = nx / l;
  m.fz = m.bz = -nz / l;
  return true;
}

// CalcMoveCommon's spline-point branch for MotionType 7 (0x6c21d9..0x6c2295): a hovering unit's hull does not take
// the point's direction. It turns, at TurnFacingRate and from where the hull points now (TurnInfo, as above),
// toward a face point: its slaved weapon's target when it has one (0x6c1fe2: with a point it faces the target
// whatever the arc says), else the point plus the point's direction. So a hover tank drives backwards or sideways
// with its gun on the enemy. There is no 0.9999 test here. Sets the hull (m.bx / m.bz and the orientation).
void HoverPointTurn(Sim& sim, Unit* u, float dirX, float dirZ) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  Vec3 fv{};
  if (!SlavedFaceVector(sim, u, &fv)) {
    // 0x6c21eb: without a slaved target the face POINT is the point's place plus its direction (addss), and
    // TurnInfo 0x6990e0 takes the place off again (0x699143: T - the transform's position, which 0x6c21bb has just
    // set to the point's): the direction as the float sum left it, a unit vector beside a coordinate of a few
    // hundred keeping only its first five digits. (Skirmish: Aeon engineer 0x600002 driving out of its factory,
    // beats 345-348: with the exact direction its quaternion was 3e-7 to 1e-6 off the original's each beat, with
    // the float sum it is within a last bit. Carried on by its hover wobble, that turned its height one float step
    // at 530 / 531 while it stood reclaiming in engineer 0x600006's goal cell, so that 0x600006's navigator took
    // it for a mover at 532 (UnitClearTo's filter 0x62eee9: position bytes changed) and did not ask for a
    // continuation path as the original did.)
    const float tx = u->position.x + dirX, tz = u->position.z + dirZ;
    fv = Vec3{tx - u->position.x, 0, tz - u->position.z};
  }
  const Quat& q = u->orientation;
  float cx = (q.w * q.y + q.z * q.x) * 2.0f, cz = -(1.0f - (q.y * q.y + q.x * q.x) * 2.0f);
  if (const float cl = std::sqrt(cx * cx + cz * cz); cl > 9.99999997e-07f) {  // 0x6999f0: times 1 / length
    const float ci = 1.0f / cl;
    cx *= ci;
    cz *= ci;
  } else {
    cx = m.bx;
    cz = -m.bz;
  }
  float nx = cx, nz = cz;
  const float wx = fv.x, wz = -fv.z;
  const float th = std::min(b.turnFacingRate * m.turnMult * 0.0017453292f, 3.14159274f);
  RotateTowardExe(th, cx, cz, wx, wz, &nx, &nz);
  const float l = std::sqrt(nx * nx + nz * nz);
  if (l > 0) {
    m.bx = nx / l;
    m.bz = -nz / l;
  }
  // 0x6c2289: the orientation is 0x699940 of the turned vector as RotateToward 0x6992c0 left it (TurnQuat), as
  // for a turn without a point; not the look rotation of the normalised heading
  u->orientation = TurnQuat(nx, nz);
}

// ---- hover: CUnitMotion::CalcMoveHover 0x6c2bc0 (lean and wobble) and SnapToGround 0x6c1610's hover branch ----
namespace {
bool ClampLength(Vec3& v, float p) {  // 0x5d1e70
  float l2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (l2 <= p * p) return false;
  float s = p / std::sqrt(l2);
  v.x *= s;
  v.y *= s;
  v.z = s * v.z;
  return true;
}
// lo + u * (hi - lo) * 2^-32 on the x87 stack (precision control 24: each op rounds to float)
float WobbleDraw(Sim& sim, float W) {
  float lo = -0.0f - W;
  uint32_t r = sim.NextUInt32();
  // (0x6c2d0d..0x6c2d13: fild of the draw as a signed int, fadd 2^32 when negative: a draw with its top bit set is
  // rounded to a float before the product, dmath::MulU32. Seen: bench, engineer 0x200002's wobble target at 350,
  // one float step off in x with one rounding; the tilt it gives decided its heading later.)
  return dmath::MulU32(r, W - lo) * 2.3283064365386963e-10f + lo;
}
// 0x452af0: normalise in place, returns the length (0 and a zero vector at or below 1e-6)
float NormalizeExe(Vec3& v) {
  float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (l > 1e-6f) {
    float s = 1.0f / l;
    v.x *= s;
    v.y *= s;
    v.z *= s;
    return l;
  }
  v = {};
  return 0;
}
// 0x50b820: turn q so that its up axis becomes n (the shortest arc 0x50cb50, applied on the left)
void AlignUp(Quat& q, Vec3 n) {
  if (!(NormalizeExe(n) > 0)) return;
  const float q0 = q.w, q1 = q.x, q2 = q.y, q3 = q.z;
  Vec3 up{(q2 * q1 - q0 * q3) * 2.0f, 1.0f - (q3 * q3 + q1 * q1) * 2.0f, (q3 * q2 + q0 * q1) * 2.0f};
  Vec3 h{up.x + n.x, up.y + n.y, up.z + n.z};
  NormalizeExe(h);
  float rw = up.z * h.z + up.x * h.x + up.y * h.y, rx, ry, rz;
  if (rw != 0.0f) {
    rx = up.y * h.z - up.z * h.y;
    ry = up.z * h.x - up.x * h.z;
    rz = up.x * h.y - up.y * h.x;
  } else {  // opposite vectors (not reached on a map)
    rx = 1;
    ry = rz = 0;
  }
  Quat o;
  o.w = ((q0 * rw - q1 * rx) - q2 * ry) - q3 * rz;
  o.x = ((q3 * ry + q1 * rw) + q0 * rx) - q2 * rz;
  o.y = ((q0 * ry + q1 * rz) + q2 * rw) - q3 * rx;
  o.z = ((q0 * rz + q3 * rw) + q2 * rx) - q1 * ry;
  q = o;
}
// CUnitMotion::CalcRollHack 0x6c1350: the up vector SnapToWater tilts a ship or a submarine to.
// The roll: velocity B (m+0xcc) and offset A (m+0xc0) lose their y; B *= 1 - RollDamping; A += B;
// B -= A x RollStability (only Unit:RecoilImpulse ever starts it). The dive pitch C (m+0x68): v from the
// COMMITTED transform's forward axis F (rows of 0x4ec590: F = (2(wy + xz), ., 1 - 2(xx + yy))) and this beat's
// depth step s (m+0x60, HandleDivingAndSurfacing ran first): MovingUp (state 11) v = (-F.x, -0, -F.z) x s x 4,
// MovingDown (state 10) v = (F.x, 0, F.z) x s x 4, else 0; C = C x 0.75 + v x 0.25. Up = A + (0, 1, 0) + C,
// normalised by 0x452af0 (and again by COORDS_Tilt).
Vec3 RollHack(Unit* u) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  Vec3& A = m.hoverLean;
  Vec3& B = m.rollVel;
  A.y = 0.0f;
  B.y = 0.0f;
  const float keep = 1.0f - b.rollDamping;
  B = {B.x * keep, B.y * keep, B.z * keep};
  A = {A.x + B.x, B.y + A.y, B.z + A.z};
  const float k = b.rollStability;
  B = {B.x - A.x * k, B.y - A.y * k, B.z - A.z * k};
  Vec3 v{0.0f, 0.0f, 0.0f};
  const bool up = u->unitStates.Has(US("MovingUp")) != 0;
  if (up || u->unitStates.Has(US("MovingDown"))) {
    const Quat& q = m.committedOri;
    const float f6 = (q.w * 2.0f) * q.y + (q.z * 2.0f) * q.x;           // 0x4ec590: m[6]
    const float f8 = 1.0f - ((q.y * 2.0f) * q.y + (q.x * 2.0f) * q.x);  // m[8]
    const float s = m.depthStep;
    if (up) v = {((-0.0f - f6) * s) * 4.0f, (s * -0.0f) * 4.0f, ((-0.0f - f8) * s) * 4.0f};
    else v = {(f6 * s) * 4.0f, (s * 0.0f) * 4.0f, (f8 * s) * 4.0f};
  }
  Vec3& C = m.diveTilt;
  C = {v.x * 0.25f + C.x * 0.75f, v.y * 0.25f + C.y * 0.75f, v.z * 0.25f + C.z * 0.75f};
  Vec3 n{C.x + A.x, C.y + (A.y + 1.0f), C.z + A.z};
  NormalizeExe(n);
  return n;
}
// SnapToGround 0x6c1610 for MotionType 7: corners from the transform as it is (tilted by last beat's snap
// unless the move just rebuilt it), surface heights, normal + wobble + lean, then AlignUp
void HoverSnap(Sim& sim, Unit* u) {
  const TerrainMap* map = sim.map();
  const MotionBlueprint& b = *u->motion.bp;
  const UnitMotion& m = u->motion;
  float hx = b.sizeX * 0.5f, hz = b.sizeZ * 0.5f;
  const Vec3 offs[4] = {{hx, 0, hz}, {-0.0f - hx, 0, hz}, {-0.0f - hx, 0, -0.0f - hz}, {hx, 0, -0.0f - hz}};
  Vec3 c[4];
  // the corners are the surface (0x62b160: the higher of ground and water) plus the raised platform the unit
  // stands on, when there is one (motion+0x114, CalcMoveHover 0x6c2c05 -> FindIntersectingRaisedPlatform): an Aeon
  // or Seraphim engineer let go by its factory stands on the deck, 0.5 to 0.65 above the ground
  const Unit* plat = RaisedPlatform(sim, u);
  for (int i = 0; i < 4; ++i) {
    Vec3 r = vm::RotateM(u->orientation, offs[i]);  // (0x6c16b2...: 0x452d40, through the quaternion's matrix)
    c[i] = {u->position.x + r.x, 0, u->position.z + r.z};
    c[i].y = map->SurfaceHeight(c[i].x, c[i].z);
    if (plat) c[i].y += PlatformHeight(plat, c[i].x, c[i].z);
  }
  float y = (c[3].y + c[2].y + c[1].y + c[0].y) * 0.25f;
  Vec3 n{0, 1, 0};
  if (!b.standUpright) {
    n = {(c[3].y - c[1].y) * (c[2].z - c[0].z) - (c[3].z - c[1].z) * (c[2].y - c[0].y),
         (c[3].z - c[1].z) * (c[2].x - c[0].x) - (c[2].z - c[0].z) * (c[3].x - c[1].x),
         (c[3].x - c[1].x) * (c[2].y - c[0].y) - (c[3].y - c[1].y) * (c[2].x - c[0].x)};
  }
  if (b.standUpright || b.sinkLower) {
    float t = map->TerrainHeight(u->position.x, u->position.z);
    float lo = std::min({c[0].y, c[1].y, c[2].y, c[3].y, t});
    float hi = std::max({c[0].y, c[1].y, c[2].y, c[3].y, t});
    y -= (hi - lo) * 0.25f;
  }
  u->position.y = b.elevation + y;
  const Vec3 nTerrain = n;
  const Quat qPre = u->orientation;
  // 0x6c1c15..0x6c1c57: the lean (m+0xc0) is added to the normal first, then the wobble (m+0xd8)
  n = {m.wobble.x + (m.hoverLean.x + n.x), m.wobble.y + (m.hoverLean.y + n.y), m.wobble.z + (m.hoverLean.z + n.z)};
  AlignUp(u->orientation, n);
  {  // MOHO64_DEBUG_HOVERSNAP=<unit id>: the tilt's inputs and result as exact (hex) floats, for a float32 replay
    static const long dbgHs = getenv("MOHO64_DEBUG_HOVERSNAP") ? atol(getenv("MOHO64_DEBUG_HOVERSNAP")) : -1;
    if (dbgHs == static_cast<long>(u->id)) {
      fprintf(stderr, "hsnap %u pos %a %a %a pre %a %a %a %a nt %a %a %a lean %a %a %a wob %a %a %a post %a %a %a %a c",
              sim.tick(), u->position.x, u->position.y, u->position.z, qPre.x, qPre.y, qPre.z, qPre.w, nTerrain.x,
              nTerrain.y, nTerrain.z, m.hoverLean.x, m.hoverLean.y, m.hoverLean.z, m.wobble.x, m.wobble.y, m.wobble.z,
              u->orientation.x, u->orientation.y, u->orientation.z, u->orientation.w);
      for (int i = 0; i < 4; ++i) fprintf(stderr, " %a %a %a", c[i].x, c[i].y, c[i].z);
      fprintf(stderr, " plat %d accel %a %a %a\n", plat ? 1 : 0, m.accel.x, m.accel.y, m.accel.z);
    }
  }
}
// the CalcMoveHover part after CalcMoveCommon
void HoverMove(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  const float k = b.bankingSlope / b.maxAccel;
  const float lx = ((m.accel.x * 10.0f) * k) * 0.2f, ly = (k * 0.0f) * 0.2f, lz = ((m.accel.z * 10.0f) * k) * 0.2f;
  m.hoverLean = {m.hoverLean.x * 0.8f + lx, m.hoverLean.y * 0.8f + ly, m.hoverLean.z * 0.8f + lz};
  if (sim.tick() % 5 == 0) {  // two draws of the sim stream (sim_random.md)
    m.wobbleTarget.x = WobbleDraw(sim, b.wobbleFactor);
    m.wobbleTarget.z = WobbleDraw(sim, b.wobbleFactor);
  }
  Vec3 d{m.wobbleTarget.x - m.wobble.x, m.wobbleTarget.y - m.wobble.y, m.wobbleTarget.z - m.wobble.z};
  ClampLength(d, b.wobbleSpeed * 0.1f);
  m.wobbleVel = {d.x + m.wobbleVel.x, m.wobbleVel.y + d.y, m.wobbleVel.z + d.z};
  ClampLength(m.wobbleVel, b.wobbleSpeed);
  m.wobble = {m.wobble.x * 0.98f + m.wobbleVel.x, m.wobbleVel.y + m.wobble.y * 0.98f,
              m.wobbleVel.z + m.wobble.z * 0.98f};
  // CalcMoveHover 0x6c2e8d..0x6c2ee7: SnapToGround unless Teleporting, or when motion+0x90 asks; m+0x90 cleared
  if (!u->unitStates.Has(US("Teleporting")) || m.needSnap || m.surfaceNext) HoverSnap(sim, u);
  m.needSnap = false;
  static const long dbgHov = getenv("MOHO64_DEBUG_HOVER") ? atol(getenv("MOHO64_DEBUG_HOVER")) : -1;
  if (dbgHov == static_cast<long>(u->id))
    fprintf(stderr,
            "hover: tick %u accel %.6f %.6f %.6f lean %.6f %.6f %.6f wob %.9g %.9g %.9g wvel %.9g %.9g %.9g wtgt %.9g "
            "%.9g plat %d q %.5f %.5f %.5f %.5f\n",
            sim.tick(), m.accel.x, m.accel.y, m.accel.z, m.hoverLean.x, m.hoverLean.y, m.hoverLean.z, m.wobble.x,
            m.wobble.y, m.wobble.z, m.wobbleVel.x, m.wobbleVel.y, m.wobbleVel.z, m.wobbleTarget.x, m.wobbleTarget.z,
            RaisedPlatform(sim, u) ? 1 : 0, u->orientation.x, u->orientation.y, u->orientation.z, u->orientation.w);
}
bool HoverRuns(const Unit* u) {  // CUnitMotion::MotionTick reaches CalcMoveHover (0x6b9f10..0x6b9f87)
  return !u->beingBuilt && !u->immobile && !u->unitStates.Has(US("Immobile")) && !u->stunned;
}

const char* const kHorzEventNames[] = {"Cruise", "TopSpeed", "Stopping", "Stopped"};

// CUnitMotion::SetMotionHorzEvent 0x6b8f30
void SetHorzEvent(Sim& sim, Unit* u, int e) {
  UnitMotion& m = u->motion;
  if (m.horzEvent == e) return;
  const int old = m.horzEvent;
  m.horzEvent = e;
  lua_State* L = sim.L();
  lua_pushstring(L, kHorzEventNames[e]);
  lua_pushstring(L, kHorzEventNames[old]);
  sim.CallMethod(L, u, "OnMotionHorzEventChange", 2);
  if (m.horzEvent == 3) IntelExactUpdate(sim, u);  // 0x6b8f5a..0x6b8f65: Entity::UpdateIntel
}

// CUnitMotion::ProcessCommonMotionState 0x6c2a40, the end of CalcMoveLand / Hover / Water. `moving` is what
// CalcMoveCommon 0x6c1e20 returned: it took a spline point, turned on the spot, or still rolls (|v|^2 >= 1e-6).
// Stopped when not; TopSpeed above 80% of the unit's top speed (u+0x594); Cruise when it starts; then Stopping
// once the motion's target is nearer than a second at full speed, else Cruise.
void ProcessCommonMotionState(Sim& sim, Unit* u, bool moving) {
  UnitMotion& m = u->motion;
  if (!moving) {
    SetHorzEvent(sim, u, 3);
    return;
  }
  const MotionBlueprint& b = *m.bp;
  const float speed = std::sqrt((m.vel.z * m.vel.z + m.vel.y * m.vel.y) + m.vel.x * m.vel.x);
  const float top = m.maxSpeedU594 > 0.0f ? m.maxSpeedU594 : (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult);
  if (speed > top * 0.08f) {
    SetHorzEvent(sim, u, 1);
    return;
  }
  if (m.horzEvent == 3) {
    SetHorzEvent(sim, u, 0);
    return;
  }
  // (to the motion's target m+0x14, 0x6c2b18: the waypoint, or where CUnitMotion::Stop left it)
  const float dx = m.motionTarget.x - u->lastPosition.x, dz = m.motionTarget.z - u->lastPosition.z;
  const float dist = std::sqrt(dz * dz + dx * dx);
  // Stopping when a second at full speed (MaxSpeed bp+0x2a0 x the speed multiplier) reaches the target
  // (0x6c2b40..0x6c2b96), or when the point the steering handed out this beat (m+4, written by the steering at
  // 0x5d2fcd / 0x5d2bbc, cleared on MotionTick's held path 0x6ba00f..0x6ba012) is a forward brake point, state 1
  // (0x6c2b98..0x6c2ba7). Seen (probe land_horzevent, LAND): the original's units braking say Stopping 7 to 18
  // from their target, where the distance alone gives Cruise.
  const bool stopping = m.speedMult * b.maxSpeed > dist || (m.pointNow && m.point.state == 1);
  SetHorzEvent(sim, u, stopping ? 2 : 0);
}
}  // namespace

void MotionTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (m.bp && m.bp->motionType == kMotionAir) {  // aircraft (also dead ones: they fall)
    AirMotionTick(sim, u);
    return;
  }
  // CUnitMotion::MotionTick 0x6b9e88 switches on the motion's state (motion+0x78): 1 attached, 2 ballistic (0x6ba078:
  // CalcMoveBallistic), 3 at rest: 0x6ba1ca returns with nothing moved (no CalcMove*, no MoveTo). State 3 is set only
  // where a dead unit's fall ends (CalcMoveBallistic 0x6c0bdc, LandBallisticTick). Seen: MP1 6056, army 4's nine
  // dead xsl0103 (hover artillery killed with their transport) land in the water: the original's stay as they came
  // down, tilted, at 17.5; ours went on as dead hovers, wobbled and were set level on the water (17.61) at 6057.
  m.noMoveTo = false;
  if (m.atRest) {
    m.lastMove = {};
    m.noMoveTo = true;
    return;
  }
  if (m.ballistic) {  // dropped from a transport: falls (also when dead)
    LandBallisticTick(sim, u);
    return;
  }
  if (m.bp && m.bp->motionType == kMotionHover && u->dead && HoverRuns(u)) {  // dead hovers still wobble
    Vec3 s0 = u->position;
    HoverMove(sim, u);
    m.lastMove = {u->position.x - s0.x, u->position.y - s0.y, u->position.z - s0.z};
    return;
  }
  if (!m.bp || !m.bp->mobile() || u->dead) {
    m.lastMove = {};
    if (m.bp && m.bp->mobile() && u->dead) ProcessCommonMotionState(sim, u, false);  // (CalcMoveLand 0x6c3212)
    // MotionTick 0x6b9d10 has no IsDead test: a dead unit takes the held test like a live one (0x6b9eb5 / 0x6b9f10:
    // vt+0x3c(1) Immobile, u+0x29c stunned) and, held, ends on 0x6ba00d without MoveTo (no blend frame) or on
    // 0x6b8f30, so its quaternion is not brought back to unit length. Seen (probe cp_deadmotion2, MP2): 0x100056
    // (uel0104, dead 8031) and 0x10007b beside it (uel0201, dead 8055) both have u+0x29c = 29 at 8032, 0x10007b 29
    // again in the beat after its death; neither reaches MoveTo after death: the original's quaternion stays as it
    // was; ours, normalised every beat, went back and forth between two last bits. (Dead units with no stun do reach
    // MoveTo on their transform, probe cp_deadmotion: MP2 0xd3 flips like ours.)
    if (u->dead && (u->immobile || u->unitStates.Has(US("Immobile")) > 0 || u->stunned)) m.noMoveTo = true;
    return;
  }
  Vec3 start = u->position;
  m.committedOri = u->orientation;  // (CalcRollHack reads the committed transform, u+0xa4)
  const MotionBlueprint& b = *m.bp;
  bool moved = false;
  bool common = false;  // CalcMoveCommon's result (for ProcessCommonMotionState)
  if (b.motionType == kMotionAir) {
    AirMotionTick(sim, u);
    return;
  } else {
    // CUnitMotion::MotionTick 0x6b9eb3..0x6b9f27: a unit in the Immobile state, or a stunned one, takes no step
    // (0x6ba00d): its spline point is dropped, CalcMoveCommon still runs for what it does to the motion (the
    // velocity dies away) but the transform it worked on is thrown away, and CalcMoveLand / Hover / Water with
    // their SnapToGround are not reached. A unit made immobile the moment it is created therefore sits flat, at
    // the height it was created with, until it is let go.
    const bool held = u->immobile || u->unitStates.Has(US("Immobile")) > 0 || u->stunned;
    // UpdateCurrentLayer 0x6b9f64: before the move, on the committed position, while the last move is non-zero
    // and Unit::UpdateTerrainType right after it (0x6b9f6f, same condition), at the position of the transform
    // MotionTick copied at its start (0x6b9d93..0x6b9de9, [esp+0x2c]), before CalcMoveLand / Hover / Water and so
    // before this beat's OnMotionHorzEventChange (ProcessCommonMotionState at their end)
    if (!held && (m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z > 1e-6f) {
      UpdateLayer(sim, u);
      UpdateTerrainType(sim, u, start.x, start.z);
    }
    const bool oldFits = StandableAt(sim, u, start.x, start.z);
    // MotionTick pre-step: contacts with other units (they push each other apart)
    if (!m.prevPosSet) {
      m.prevPos = start;
      m.prevPosSet = true;
    }
    if ((m.vel.x * m.vel.x + m.vel.y * m.vel.y) + m.vel.z * m.vel.z > 1e-6f || m.surfaceNext) ProcessSurfaceCollision(sim, u);
    if (m.layerBlend != 0.0f) {
      // TransitionBetweenLayers 0x6c3070, called by CalcMoveCommon (0x6c1e59) in place of the move while the
      // counter runs: the transform is the blend of the two taken when the layer changed, the motion's velocity
      // is left as it was and no spline point is taken. An immobile unit blends too, but its last frame (the
      // counter back at 0) is not applied (0x6ba00d..0x6ba065). The motion's last position (m+0x2c) stays where
      // the blend began, so the first spline point after it gives one beat of a large velocity (seen: ten times).
      const float n = b.layerTransitionDuration * 10.0f;
      const float t = m.layerBlend / n;
      const Vec3 np{(m.blendToPos.x - m.blendFromPos.x) * t + m.blendFromPos.x,
                    (m.blendToPos.y - m.blendFromPos.y) * t + m.blendFromPos.y,
                    (m.blendToPos.z - m.blendFromPos.z) * t + m.blendFromPos.z};
      // func_QuatLERP 0x4eba80 (from +0x120, to +0x13c: 0x6c30e5..0x6c3120): the from rotation itself when every part
      // is within 1e-6 of the other's (0x4eded0), else the blend, b negated as -0.0 - b (0xe4f748) when the two point
      // apart, normalised (a length of 1e-6 or less gives zero): vm::NlerpExe
      const Quat nq = vm::NlerpExe(m.blendFromOri, m.blendToOri, t);
      m.layerBlend += 1.0f;
      m.surfaceNext = false;
      if (m.layerBlend >= n) m.layerBlend = 0.0f;
      if (!held || m.layerBlend != 0.0f) {
        u->position = np;
        u->orientation = nq;
      } else {
        m.noMoveTo = true;
      }
      m.lastMove = {u->position.x - start.x, u->position.y - start.y, u->position.z - start.z};
      if (!u->beingBuilt) ProcessCommonMotionState(sim, u, false);
      return;
    }
    // CalcMoveCommon 0x6c1e69..0x6c20de, which runs every beat the layer blend does not (with a spline point or
    // without): the weapon slaved to the hull is asked for its target, and the "turning toward it" latch (m+0x91) is
    // set past the full arc, cleared inside half of it or when there is no such weapon or target. Ours asked only
    // on beats without a point, so a latch set while standing survived a whole move. (Seen: an Atlantis that had
    // started a turn at beat 134, then moved off, stopped at 344 with its target inside the arc but outside half
    // of it: the original stood, ours went on turning.) What the direction is used for stays where it was.
    {
      Vec3 slavedDir{};
      SlavedFaceVector(sim, u, &slavedDir);
    }
    if (held) {
      m.pointNow = false;
      const Quat q0 = u->orientation;
      const float bx0 = m.bx, bz0 = m.bz, fx0 = m.fx, fz0 = m.fz;
      const bool turned = ArrivalTurn(sim, u);
      common = turned || !(kStopSq > (m.vel.y * m.vel.y + m.vel.z * m.vel.z) + m.vel.x * m.vel.x);  // (0x6c27a8..)
      Coast(u, turned);
      u->position = start;
      u->orientation = q0;
      m.bx = bx0;
      m.bz = bz0;
      m.fx = fx0;
      m.fz = fz0;
      m.prevPos = start;
      m.lastMove = {};
      m.noMoveTo = true;
      if (!u->beingBuilt) ProcessCommonMotionState(sim, u, common);
      return;
    }
    if (m.pointNow && !m.pushed) {
      // CalcMoveCommon: the unit moves onto the spline point the steering handed out
      const UnitMotion::SplinePoint& q = m.point;
      // (no snap request here: CalcMoveCommon 0x6c1e69.. writes nothing to motion+0x90, whose writers are AddImpulse,
      // Warp, NotifyDetached and MotionTick's landing (0x6b8ed7, 0x6b9445, 0x6b971d, 0x6ba160). The point path snaps
      // anyway where the move calc does, but a hover under Teleporting, which CalcMoveHover 0x6c2e99..0x6c2ea6 snaps
      // only on +0x90, stays at the point's height. Seen: MP1 5068, army 4's hover artillery 0x400074 turning on
      // its last brake point under the beam: the original stays at 18.6719, ours asked for a snap and rose to 18.7819.)
      u->position.x = q.pos.x;
      u->position.z = q.pos.z;
      // (and the height: 0x6c21bb..0x6c21cc copies the point's x, y and z into the new position; the snap that
      // follows usually puts it back on the ground. Without one, the unit stands at the point's stored height. Seen:
      // MP1 5064, army 4's hover artillery 0x400074 (Elevation 0.11) under Teleporting, braking at its pickup: no
      // SnapToGround (CalcMoveHover 0x6c2e8d), so the original is at the brake point's ground height 18.6719; ours kept
      // its hover height 18.7819.)
      u->position.y = q.pos.y;
      // vel = point - prevPos (3-D) and prevPos = the point (0x6c21bb..0x6c230f: the point's own x, y, z): the y is the
      // height the point was STORED with when its batch was made (terrain at its centre, water: max with the surface
      // for MotionType 3/7/8; 0x5b40fd), not the ground as it is now (spline_generate.md 3, land_motion_blocking.md
      // 3.1). They differ where the ground was flattened after the batch was made. Seen: MP3 2366-2374, a Seraphim
      // engineer (0x300015) walking onto a factory's skirt being flattened: the original's velocity has y +0.0156
      // from the stale points, ours -0.0025; the collision brake at 2374 takes the 3-D speed, so the first braking
      // step went 0.1146 in the original and 0.1140 in ours.
      const float py = q.pos.y;
      Vec3 nv{q.pos.x - m.prevPos.x, py - m.prevPos.y, q.pos.z - m.prevPos.z};
      m.prevPos = Vec3{q.pos.x, py, q.pos.z};
      m.accel = {nv.x - m.vel.x, nv.y - m.vel.y, nv.z - m.vel.z};
      m.vel = nv;
      // CalcMoveCommon's facing (0x6c2297). The points of a brake spline all carry the hull's 3-D forward axis
      // as it was when the brake began (CAiPathSpline::Update mode 3: `dir = F (unchanged)`, tilt included), so
      // the look direction there is not flat: a braking tank's heading stays put on a steady slope and moves by
      // hundredths of a degree where the slope changes, as the original's does.
      const bool brakePoint = m.hasSpline && (m.splineMode == 3 || m.splineMode == 4);
      m.fx = q.fx;
      m.fz = q.fz;
      if (b.motionType == kMotionHover) {
        // (a hover unit's hull turns toward its weapon's target or the point's direction: HoverPointTurn)
        HoverPointTurn(sim, u, brakePoint ? m.brakeDir.x : q.fx, brakePoint ? m.brakeDir.z : q.fz);
      } else {
        if (brakePoint) u->orientation = LookQuat(m.brakeDir.x, m.brakeDir.y, m.brakeDir.z);
        else u->orientation = LookQuat(q.bx, 0.0f, q.bz);
        {
          // MOHO64_DEBUG_FACING=<unit id>: the point's direction and the quaternion made of it, all digits
          static const long dbgFace = getenv("MOHO64_DEBUG_FACING") ? atol(getenv("MOHO64_DEBUG_FACING")) : -1;
          if (dbgFace >= 0 && static_cast<long>(u->id) == dbgFace)
            fprintf(stderr, "facing: tick %u dir %.9g %.9g (brake %d) -> q %.9g %.9g %.9g %.9g\n", sim.tick(), q.bx, q.bz,
                    brakePoint ? 1 : 0, u->orientation.x, u->orientation.y, u->orientation.z, u->orientation.w);
        }
        m.bx = q.bx;
        m.bz = q.bz;
      }
      // CalcMoveCommon returns 1 on this path whether or not the point moved the unit (0x6c2a33), so
      // CalcMoveLand 0x6c3180 snaps: a unit whose point is its own position keeps its terrain tilt (it was
      // flattened by the facing above and not re-tilted: probe unit 0x90002f at ticks 142-143)
      moved = true;
      common = true;
    } else {
      // (no spline point: standing, coasting to a stop, pushed, or with a goal the navigator is still thinking
      // about. A hull with a weapon slaved to it turns toward that weapon's target in all of these: a launcher
      // re-ordered every other beat turned 9 degrees in the one beat it had no spline, and one pushed aside by a
      // passing tank turned after its target on the third beat of the push. The formation's forward vector is only
      // taken while the navigator is idle, which ArrivalTurn tests itself.)
      bool turned = ArrivalTurn(sim, u);  // (also while it coasts to a stop)
      common = turned || !(kStopSq > (m.vel.y * m.vel.y + m.vel.z * m.vel.z) + m.vel.x * m.vel.x);  // (0x6c27a8..)
      moved = Coast(u, turned);
      if (turned) {
        m.needSnap = true;
        u->orientation = m.turnQ;
      }
    }
    if (moved && oldFits && !StandableAt(sim, u, u->position.x, u->position.z)) {
      float dx = start.x - u->position.x, dz = start.z - u->position.z;
      u->position = start;
      m.vel = {};
      m.accel = {};
      m.prevPos = start;
      if (!m.pushed) {
        // 0x6c29a7..0x6c29fb: (start - position, y 0) brought to the length u+0x594 x 0.01 by SetLength 0x5b1c90: one
        // factor, length / sqrt((x x + y y) + z z) (fsqrt, fdivr at PC_24), times each component; not each component
        // over the length, times the size. The two differ in the last bit, and this impulse is the push's whole
        // velocity, whose length the push's end tests against MaxSpeed x 0.01 (ProcessSplineMovement 0x5d2cc0): the same
        // size, so the last bit decides whether the push ends. Seen: MP2 10877, army 1's Striker 0x100125 (MaxSpeed 3.4) whose
        // steps are blocked every other beat: its 10876 impulse came out 0.0339999981 long, under 0.0340000018, so ours
        // ended the push at 10877 and took a point; the original stays pushed and stands for that beat.
        const float imp = (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult) * 0.010000001f;  // u+0x594
        Vec3 iv{dx, 0.0f, dz};
        const float l2 = (iv.x * iv.x + iv.y * iv.y) + iv.z * iv.z;
        if (l2 > 0.0f) {
          const float k = imp / std::sqrt(l2);
          iv = {iv.x * k, iv.y * k, iv.z * k};
        }
        AddImpulse(u, iv);
      }
      // (moved stays true: CalcMoveCommon answers 1 after putting the unit back (0x6c2a33), so CalcMoveLand snaps it to
      // the ground where it stands (0x6c31cd), with its orientation of now. A pushed unit has no impulse (0x6c299e) and
      // so no motion+0x90 to ask for the snap: only this answer gives it. Seen: MP2 6704 and 6707, army 0's Mantis 0x32
      // turning on a steep slope, its steps blocked at 6703 and 6706 (the impulse's snap) and its pushed coast blocked
      // at 6704 and 6707: the original snaps again there (y 61.9126 -> 61.8516, the tilt follows), ours did not.)
    }
    bool waterCalcRan = false;  // (CalcMoveWater 0x6c3480 ran this step: it leaves motion+0x90 as it is, below)
    if (b.motionType == kMotionHover) {
      // (No rebuild of the orientation from m.bx / m.bz here: CalcMoveHover 0x6c2bc0 tilts the transform as
      // CalcMoveCommon left it. A turn in place has set it already (0x699940, m.turnQ), and a unit let go or
      // turned by a script keeps the quaternion it was given, sign included: FAF's factory gives an engineer
      // SetRotation(spin) = SetOrientation (0x68f6e0: the transform, nothing of the motion) before DetachFrom, and
      // the original's quaternion there has w < 0 (bench, Seraphim engineer 0x200002 at 344). The look rotation
      // of the heading made a w > 0 quaternion with other last digits, which the hover's tilt carried on.)
      // CalcMoveHover 0x6c2bf5: the platform search when CalcMoveCommon moved the unit or m+0x90 asks
      if (HoverRuns(u) && (common || m.surfaceNext)) FindRaisedPlatform(sim, u);
      if (HoverRuns(u)) HoverMove(sim, u);
      else if (moved || m.needSnap) {
        SnapUnit(sim, u);
        m.needSnap = false;
      }
    } else {
      // CalcMoveWater 0x6c3480 (every layer but Land and Seabed): the dive / surfacing step comes after the
      // move, and the unit is put at its depth while it lasts
      const bool landCalc = u->layer == "Land" || u->layer == "Seabed";
      waterCalcRan = !landCalc;
      const bool diving = !landCalc && HandleDivingAndSurfacing(sim, u);
      // the platform search: CalcMoveLand 0x6c31a2 when CalcMoveCommon moved the unit and it is not Teleporting, or
      // m+0x90 asks; CalcMoveWater 0x6c34b0 when CalcMoveCommon moved it
      if (landCalc ? (common && !u->unitStates.Has(US("Teleporting"))) || m.surfaceNext : common) FindRaisedPlatform(sim, u);
      // CalcMoveLand 0x6c3180: SnapToGround runs when the unit moved AND is not Teleporting (state 19: a unit
      // in a transport's beam, whose task places it), or when motion+0x90 asks for it: 0x6c31b7 tests m+0x90 on its
      // own, so a unit standing still snaps too (a flattening under it, 0x74b35a), and clears it (0x6c31fa)
      const bool beamed = u->unitStates.Has(US("Teleporting")) > 0 && !m.surfaceNext;
      // CalcMoveWater 0x6c3523: a unit that neither moved nor dives is still set on the water while its dive pitch
      // (m+0x68, CalcRollHack) dies away: |pitch|^2 > 1e-6 (so a surfaced submarine keeps a last trace of it)
      const Vec3& C = m.diveTilt;
      const bool pitched = !landCalc && ((C.x * C.x + C.y * C.y) + C.z * C.z) > 9.99999997e-07f;
      if (((moved || m.needSnap) && !beamed) || diving || (landCalc && m.surfaceNext) || pitched) {
        SnapUnit(sim, u);
        m.needSnap = false;
      }
    }
    // motion+0x90 is cleared by CalcMoveLand (0x6c31fa, after its SnapToGround) and CalcMoveHover (0x6c2ee7); CalcMoveWater
    // never clears it (the byte's only other clears: TransitionBetweenLayers 0x6c315b and AddImpulse's no-cap path
    // 0x6b8e48). A unit that moves by the water calc keeps the request it was made with (CUnitMotion ctor 0x6b7c9a) or
    // was last given (AddImpulse 0x6b8ed7, Warp, NotifyDetached), and MotionTick runs its contacts every beat while it
    // stands (0x6b9e21: ProcessSurfaceCollisionFromLastMove when |vel|^2 > 1e-6 or +0x90). Seen (probe mp1_7020): MP1
    // 7020, army 2's T2 engineer 0x20009c (url0208) standing in the Water layer, +0x90 = 1 at every turn: in its own turn
    // its DoCollisionsFor (caller 0x6b9231) meets 0x20006b coming at it and pushes both, so 0x20009c moves (-0.0219,
    // -0.0512) in that beat and 0x20006b starts its turn pushed (0.0062, 0.1725). Ours had cleared the request after
    // 0x20009c's first water step; 0x20006b pushed in its own turn and 0x20009c moved a beat later.
    if (!waterCalcRan) m.surfaceNext = false;
    static const long dbgStep = getenv("MOHO64_DEBUG_MOTION") ? atol(getenv("MOHO64_DEBUG_MOTION")) : -1;
    if (dbgStep == static_cast<long>(u->id))
      fprintf(stderr, "step: tick %u pos %.4f %.4f vel %.5f %.5f %.5f pushed %d wasMoving %d point %d spline %d goal %d waypoint %d moved %d\n",
              sim.tick(), u->position.x, u->position.z, m.vel.x, m.vel.y, m.vel.z, m.pushed ? 1 : 0, m.wasMoving ? 1 : 0,
              m.pointNow ? 1 : 0, m.hasSpline ? 1 : 0, m.hasGoal ? 1 : 0, m.hasWaypoint ? 1 : 0, moved ? 1 : 0);
  }
  m.lastMove = {u->position.x - start.x, u->position.y - start.y, u->position.z - start.z};
  if (!u->beingBuilt) ProcessCommonMotionState(sim, u, common);
}

const Vec3& UnitVelocity(const Unit* u) {
  const UnitMotion& m = u->motion;
  const bool land = m.bp && m.bp->mobile() && u->layer != "Air" && !m.ballistic;
  // Unit::GetVelocity 0x6a9c90: with a motion and off the Air layer the motion's velocity; else the entity's
  // (committed position - previous position) x scale (0x678dc0), which changes in AdvanceCoords only: a flyer
  // that already moved this beat still reports last beat's displacement. The test is the layer only (0x6a9c9a:
  // +0x118 against 0x10), not the motion type: an aircraft on the Land layer reports its motion's velocity, (0, 0, 0)
  // for one that flies (m+0x38 is not kept for it). Seen: MP1 4915, army 5's Seraphim transport 0x400067 sets down
  // for a tick; its AA turrets' aim (the muzzle in last beat's pose moved on by GetVelocity, Aim 0x6317b0) took the
  // displacement, and from 4916 their heading and pitch were up to 0.05 off the original's.
  return land ? m.vel : m.velCommitted;
}

bool UnitVelocityBitsSet(const Unit* u) {
  const Vec3& v = UnitVelocity(u);
  uint32_t b[3];
  std::memcpy(&b[0], &v.x, 4);
  std::memcpy(&b[1], &v.y, 4);
  std::memcpy(&b[2], &v.z, 4);
  return (b[0] | b[1] | b[2]) != 0;
}

}  // namespace moho
