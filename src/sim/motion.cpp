// Unit motion (see motion.h for what the original does and how it was checked).
#include "core/dmath.h"
#include "sim/motion.h"
#include "sim/formation.h"
#include "sim/combat.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/air.h"
#include "sim/transport.h"
#include "sim/steering.h"

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
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

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
float SinApprox(float a) { return ((a * a * 0.00761000020429492f - 0.1660500019788742f) * a * a + 1.0f) * a; }

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
  if (x * tz - z * tx < 0.0f) s = -s;  // target is clockwise (x toward -z)
  float n = s * s + c * c;
  if (std::fabs(n - 1.0f) > 0.0010000000474974513f) {
    float l = std::sqrt(n);
    s /= l;
    c /= l;
  }
  float nx = c * x - s * z, nz = s * x + c * z;
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

void UpdateLayer(Sim& sim, Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  const TerrainMap* map = sim.map();
  if (!map || !map->hasWater) return;
  bool underWater = map->TerrainHeight(u->position.x, u->position.z) < map->waterElevation;
  switch (b.motionType) {
    case kMotionHover:
    case kMotionAmphibiousFloating:
      SetLayer(sim, u, underWater ? "Water" : "Land");
      break;
    case kMotionAmphibious:
      SetLayer(sim, u, underWater ? "Seabed" : "Land");
      break;
    default:
      break;
  }
}

}  // namespace

Quat YawQuat(float fx, float fz) {
  float h = dmath::Atan2(fx, fz);
  Quat q;
  q.y = dmath::Sin(h * 0.5f);
  q.w = dmath::Cos(h * 0.5f);
  return q;
}

const MotionBlueprint& GetMotionBlueprint(lua_State* L, const BlueprintInfo& bp, const SimBlueprints& bps) {
  static std::map<const BlueprintInfo*, MotionBlueprint> cache;
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
      m.turnRadius = GetNum(L, p, "TurnRadius", 0);
      m.turnRate = GetNum(L, p, "TurnRate", 0);
      m.turnFacingRate = GetNum(L, p, "TurnFacingRate", 0);
      m.elevation = GetNum(L, p, "Elevation", 0);
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
  return cache.emplace(&bp, m).first->second;
}

void GoalCell(const MotionBlueprint& b, float x, float z, int* cx, int* cz) {
  *cx = static_cast<int>(std::nearbyint(x - b.footprint.sizeX * 0.5f));
  *cz = static_cast<int>(std::nearbyint(z - b.footprint.sizeZ * 0.5f));
}

// CUnitMotion::SnapToGround (FA exe 0x6c1610) / SnapToWater: height and tilt from the terrain
// under the four corners of the unit's size box.
// FindIntersectingRaisedPlatform 0x6c2f00: the nearest immobile unit with RaisedPlatforms whose box
// meets the unit's (the original takes it from the unit's surface-collision list).
const Unit* FindRaisedPlatform(Sim& sim, const Unit* u) {
  const MotionBlueprint& b = *u->motion.bp;
  float r = std::max(b.sizeX, b.sizeZ) * 0.5f;
  const Unit* best = nullptr;
  float bestD = std::numeric_limits<float>::infinity();
  sim.ForUnitsInRect(u->position.x - 16, u->position.z - 16, u->position.x + 16, u->position.z + 16, [&](Unit* p) {
    if (p == u || p->dead || !p->motion.bp || p->motion.bp->motionType != kMotionNone) return;
    const MotionBlueprint& pb = *p->motion.bp;
    if (pb.raisedPlatforms.size() < 12) return;
    if (std::fabs(p->position.x - u->position.x) > pb.sizeX * 0.5f + r) return;
    if (std::fabs(p->position.z - u->position.z) > pb.sizeZ * 0.5f + r) return;
    float dx = p->position.x - u->position.x, dy = p->position.y - u->position.y, dz = p->position.z - u->position.z;
    float d = dx * dx + dy * dy + dz * dz;
    if (d < bestD) {
      bestD = d;
      best = p;
    }
  });
  return best;
}
// 0x62af70: the first quad (unrotated, from the platform's position) holding p, bilinear in it
float PlatformHeight(const Unit* P, float x, float z) {
  if (P->dead) return 0;
  const auto& q = P->motion.bp->raisedPlatforms;
  float px = P->position.x, pz = P->position.z;
  for (size_t i = 0; i + 12 <= q.size(); i += 12) {
    float x0 = q[i] + px, z0 = q[i + 1] + pz, x3 = q[i + 9] + px, z3 = q[i + 10] + pz;
    if (!(x0 <= x && x <= x3 && z0 <= z && z <= z3)) continue;
    float u = (x - x0) / (q[i + 3] - q[i]);
    float v = (z - z0) / (q[i + 7] - q[i + 1]);
    float a = q[i + 2] + (q[i + 8] - q[i + 2]) * v;
    float b = q[i + 5] + (q[i + 11] - q[i + 5]) * v;
    return a + u * (b - a);
  }
  return 0;
}

void SnapUnit(const Sim& csim, Unit* u) {
  Sim& sim = const_cast<Sim&>(csim);
  const TerrainMap* map = sim.map();
  if (!map || !u->motion.bp) return;
  const MotionBlueprint& b = *u->motion.bp;
  u->orientation = YawQuat(u->motion.bx, u->motion.bz);
  if (b.motionType == kMotionWater || b.motionType == kMotionSurfacingSub) {
    u->position.y = map->hasWater ? map->waterElevation : map->TerrainHeight(u->position.x, u->position.z);
    return;
  }
  if (b.motionType == kMotionAir) return;
  bool hover = b.motionType == kMotionHover;
  bool floating = b.motionType == kMotionAmphibiousFloating;
  float hx = b.sizeX * 0.5f, hz = b.sizeZ * 0.5f;
  const Vec3 offs[4] = {{hx, 0, hz}, {-hx, 0, hz}, {-hx, 0, -hz}, {hx, 0, -hz}};
  Vec3 c[4];
  const Unit* plat = (hover || floating) ? nullptr : FindRaisedPlatform(sim, u);
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
  float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (l > 0) {
    if (n.y < 0) l = -l;
    Vec3 nn{n.x / l, n.y / l, n.z / l};
    u->orientation = Mul(Align({0, 1, 0}, nn), u->orientation);
  }
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
void SteeringStop(UnitMotion& m) {
  m.spline.clear();
  m.splineIdx = 0;
  m.hasSpline = false;
  m.pointNow = false;
  m.colType = 0;
  m.colUnit = 0;
  m.colTick = 0xffffffffu;
  m.sideStep = {};
}

void MotionNavBegin(Unit* u) {
  UnitMotion& m = u->motion;
  // a new goal while the steering drives: it keeps its waypoint until the navigator gives the next one
  // (SetGoal resets the path navigator only)
  bool keep = m.hasGoal && m.navDriven && m.hasWaypoint;
  m.hasGoal = true;
  m.arrived = false;
  m.failed = false;
  m.navDriven = true;
  if (keep) return;
  m.hasWaypoint = false;
  m.path.clear();
  m.pathIndex = 0;
  SteeringStop(m);
}

void MotionSetWaypoint(Sim& sim, Unit* u, const Vec3& p, bool through) {
  UnitMotion& m = u->motion;
  (void)sim;
  m.path.assign(1, p);
  m.pathIndex = 0;
  m.passThrough = through;
  m.hasWaypoint = true;
  m.driveTick = 0;
  GoalCell(*m.bp, p.x, p.z, &m.goalCellX, &m.goalCellZ);
  m.newSegment = true;  // the steering takes it at its next tick (DriveToNextWaypoint)
}

void MotionNavDone(Unit* u, bool succeeded) {
  UnitMotion& m = u->motion;
  m.hasGoal = false;
  m.hasWaypoint = false;
  m.navDriven = false;
  m.path.clear();
  SteeringStop(m);
  if (succeeded) m.arrived = true;
  else m.failed = true;
  Sim::From(u->luaState())->ResumeCommandThread(u);  // the navigator's event wakes the move task
}

void MotionStop(Unit* u) {
  UnitMotion& m = u->motion;
  if (m.air) AirAbort(*Sim::From(u->luaState()), u);
  if (m.nav) LandNavStop(*Sim::From(u->luaState()), u);
  m.hasGoal = false;
  m.hasWaypoint = false;
  m.navDriven = false;
  m.path.clear();
  SteeringStop(m);
}

const PathGrid* FootprintGrid(Sim& sim, const MotionBlueprint& b) {
  Navigation* nav = &sim.navigation();
  if (b.gridOwner != nav) {
    b.grid = nav->Grid(b.footprint);
    b.gridOwner = nav;
  }
  return b.grid;
}

namespace {

// The body follows the steering facing: at once, or at TurnFacingRate when the body may turn
// while moving (hover units).
void UpdateBody(UnitMotion& m) {
  const MotionBlueprint& b = *m.bp;
  if (b.rotateBodyWhileMoving && b.turnFacingRate > 0) {
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
// inside the map, terrain and structures at the footprint-origin cell)
bool StandableAt(Sim& sim, const MotionBlueprint& b, float x, float z) {
  const TerrainMap* map = sim.map();
  if (map && (x < 0 || z < 0 || x > static_cast<float>(map->width()) || z > static_cast<float>(map->height())))
    return false;
  const PathGrid* g = FootprintGrid(sim, b);
  if (!g) return true;
  int cx, cz;
  GoalCell(b, x, z, &cx, &cz);
  return g->Passable(cx, cz);
}

// The spline's look-ahead point (CAiPathSpline::Generate 0x5b2ff0): the new point plus the step
// set to the length max(SizeX, SizeZ) + stopping distance (the point itself when it did not move).
bool LookAheadFits(Sim& sim, const MotionBlueprint& b, const Vec3& p, float sx, float sz, float stopDist) {
  float l = std::sqrt(sx * sx + sz * sz);
  float x = p.x, z = p.z;
  if (l > 0.0f) {
    float k = (std::max(b.sizeX, b.sizeZ) + stopDist) / l;
    x += sx * k;
    z += sz * k;
  }
  return StandableAt(sim, b, x, z);
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
    float dot = fz * dz * n + fx * dx * n;
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
StepParams Params(const UnitMotion& m) {
  const MotionBlueprint& b = *m.bp;
  StepParams P;
  P.maxF = (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult) * 0.1f;  // u+0x594
  P.maxR = b.maxSpeedReverse * m.speedMult * 0.1f;
  P.acc = b.maxAccel * m.accMult * 0.01f;
  P.brake = (b.maxBrake != 0.0f ? b.maxBrake : b.maxAccel) * m.accMult * 0.01f;
  P.steer = (b.maxSteerForce != 0.0f ? b.maxSteerForce : b.maxAccel) * m.accMult * 0.01f;
  P.turnRadius = b.turnRadius != 0.0f ? b.turnRadius / m.turnMult : 1e30f;
  P.turnRate = b.turnRate * m.turnMult * kDegToRadTenth;
  P.wide = b.turnRate < b.turnRadius;  // turns wider than it can rotate: slows in turns
  return P;
}

void GenBody(const MotionBlueprint& b, const UnitMotion& m, Gen& g) {
  if (b.rotateBodyWhileMoving && b.turnFacingRate > 0) {
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
  if (dist > 0) {
    tx = dx / dist;
    tz = dz / dist;
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
  float limit = dist > 0 ? TurnSpeedLimit(b, P.maxF, P.turnRadius, P.turnRate, g.fx, g.fz, dx, dz, speedFrac0) : P.maxF;
  if (!g.reverse) {
    RotateToward(g.fx, g.fz, tx, tz, turn);
  } else {
    float bx = -g.fx, bz = -g.fz;
    RotateToward(bx, bz, tx, tz, turn);
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
bool StepRules(Sim& sim, const MotionBlueprint& b, const StepParams& P, Gen& g, const Vec3& tgt, int mode) {
  float nspeed = std::sqrt(g.vel.x * g.vel.x + g.vel.z * g.vel.z);
  float frac = P.maxF > 0 ? nspeed / P.maxF : 0;
  float ndx = tgt.x - g.p.x, ndz = tgt.z - g.p.z;
  float ndist = std::sqrt(ndx * ndx + ndz * ndz);
  float ndot = ndist > 0 ? (g.fx * ndx + g.fz * ndz) / ndist : 1.0f;
  float stopDist = 0;
  float a = std::max(b.maxAccel, b.maxBrake);
  if (a > 0.0f) stopDist = (nspeed * 10.0f * nspeed * 10.0f) / (a * 2.0f);
  switch (g.state) {
    case 3:
      if (frac <= 0.0099999998f) return true;
      break;
    case 4:
      if (frac <= 0.0099999998f) g.state = (b.maxSpeedReverse <= 0.0f || b.rotateOnSpot) ? 7 : 5;
      break;
    case 5:
      if (!LookAheadFits(sim, b, g.p, g.vel.x, g.vel.z, stopDist)) g.state = 6;
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
        if (!LookAheadFits(sim, b, g.p, g.vel.x, g.vel.z, stopDist)) g.state = 4;
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
  return q;
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
  float dot = (D.x * ex + D.z * ez) / dist;  // the 3-D forward: shortened on a slope
  const Vec3& V = m.lastMove;
  float frac = b.maxSpeed > 0 ? std::sqrt(V.x * V.x + V.y * V.y + V.z * V.z) * 10.0f / b.maxSpeed : 0;
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
    float l = std::sqrt(D.x * D.x + D.z * D.z);
    g.fx = l > 1e-6f ? D.x / l : 0;
    g.fz = l > 1e-6f ? D.z / l : 1;
    g.bx = g.fx;
    g.bz = g.fz;
    ChooseStart(b, P, g, dot, frac, V.x * D.x + V.y * D.y + V.z * D.z < 0.0f);
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
  for (int n = 0;;) {
    before = PointOf(g);
    if (!StepMove(b, P, m, g, tgt, mode)) break;
    g.p.y = u->position.y;
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
    if (!end && StepRules(sim, b, P, g, tgt, mode)) end = true;
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

// CAiPathSpline::Update 0x5b26c0 (modes 3/4, outline): a stop sequence from the unit's position and
// velocity, at twice the deceleration in mode 4, at least 6 points in mode 4.
void BrakeSpline(Sim& sim, Unit* u, int mode) {
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
  Gen g;
  g.p = u->position;
  g.vel = m.vel;
  g.fx = m.fx;
  g.fz = m.fz;
  g.bx = m.bx;
  g.bz = m.bz;
  for (int n = 0; n < 400;) {
    float dvx = -g.vel.x, dvz = -g.vel.z;
    float lim = (dvx * g.fx + dvz * g.fz > 0.0f) ? acc : brake;
    float dl = dvx * dvx + dvz * dvz;
    if (lim * lim < dl) {
      float k = lim / std::sqrt(dl);
      dvx *= k;
      dvz *= k;
    }
    g.vel.x += dvx;
    g.vel.z += dvz;
    float vl2 = g.vel.x * g.vel.x + g.vel.z * g.vel.z;
    float cap = std::max(P.maxF, P.maxR);
    if (cap * cap < vl2) {
      float k = cap / std::sqrt(vl2);
      g.vel.x *= k;
      g.vel.z *= k;
    }
    g.p.x += g.vel.x;
    g.p.z += g.vel.z;
    m.spline.push_back(PointOf(g));
    ++n;
    if (dvx * dvx + dvz * dvz <= 1e-6f && (mode == 3 || n > 5)) break;
  }
  m.savedState = 0;
  m.genLast = m.spline.size() >= 2 ? m.spline[m.spline.size() - 2] : PointOf(g);
  m.genReverse = false;
  (void)sim;
}

}  // namespace

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
  const MotionBlueprint& b = *u->motion.bp;
  int ax, az, bx, bz;
  GoalCell(b, u->position.x, u->position.z, &ax, &az);
  GoalCell(b, q.x, q.z, &bx, &bz);
  return ax == bx && az == bz;
}

void AddImpulse(Unit* u, const Vec3& imp) {
  if (u->dead || u->destroyQueued || u->beingBuilt) return;
  UnitMotion& m = u->motion;
  if (!m.bp) return;
  if (m.pointNow) m.wasMoving = true;  // "was moving when pushed"
  m.vel.x = m.vel.x * 0.5f + imp.x;
  m.vel.z = m.vel.z * 0.5f + imp.z;
  float cap = (m.speedCap > 0 ? m.speedCap : m.bp->maxSpeed * m.speedMult) * 0.2f;  // u+0x594 * 0.2
  float l2 = m.vel.x * m.vel.x + m.vel.z * m.vel.z;
  if (l2 > cap * cap && l2 > 0) {
    float k = cap / std::sqrt(l2);
    m.vel.x *= k;
    m.vel.z *= k;
  }
  m.surfaceNext = true;
  m.pushed = true;
}

namespace {

// Without a goal: the unit coasts to a stop (CUnitMotion::CalcMoveCommon, no spline).
bool Coast(Unit* u) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  float v2 = m.vel.x * m.vel.x + m.vel.z * m.vel.z;
  if (v2 < kStopSq) {
    m.vel = {};
    return false;
  }
  float brake = (b.maxBrake > 0.0f ? b.maxBrake : b.maxAccel) * m.accMult * 0.01f;
  float bx = m.vel.x, bz = m.vel.z;
  if (v2 > brake * brake) {
    float k = brake / std::sqrt(v2);
    bx *= k;
    bz *= k;
  }
  m.vel.x = m.vel.x * 0.800000011920929f - bx;
  m.vel.z = m.vel.z * 0.800000011920929f - bz;
  if (m.vel.x * m.vel.x + m.vel.z * m.vel.z < kStopSq) {
    m.vel = {};
    return false;
  }
  u->position.x += m.vel.x;
  u->position.z += m.vel.z;
  return true;
}

}  // namespace

namespace combat {
bool HasTarget(Sim& sim, const AiTarget& t);
}  // namespace combat

// CalcMoveCommon's no-spline branch (formations.md 12): an idle unit of a form formation turns on the
// spot to the formation's forward vector (Unit::GetFormationVector), TurnInfo 0x6990e0 / RotateToward
// 0x6992c0, in the negated-z frame. Returns true when it turned.
bool ArrivalTurn(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!u->form || !FormationIsForm(*u->form)) return false;
  if (u->unitStates.count("TransportLoading") || u->unitStates.count("Refueling")) return false;
  if (combat::HasTarget(sim, u->desiredTarget)) return false;  // (weapon facing has priority: not carried out)
  if (LandNavActive(u)) return false;                          // the navigator is not idle
  Vec3 fv = FormationVector(sim, u);
  if (fv.x == 0 && fv.y == 0 && fv.z == 0) return false;
  const MotionBlueprint& b = *m.bp;
  float cx = m.bx, cz = -m.bz;          // current forward (x, -z)
  float wx = fv.x, wz = -fv.z;          // toward T = pos + fv
  float len = std::sqrt(wx * wx + wz * wz);
  if (len <= 0) return false;
  if ((wx / len) * cx + (wz / len) * cz >= 0.9999f) return false;
  float rate = (b.motionType == kMotionHover ? b.turnFacingRate : b.turnRate) * m.turnMult * 0.0017453292f;
  float th = std::min(rate, 3.14159274f);
  float lc = std::sqrt(cx * cx + cz * cz), lw = len;
  if (lc * lw == 0) return false;
  float nx, nz;
  float k = std::cos(th);
  if (k * lc * lw <= cx * wx + cz * wz) {  // within one step: snap exactly
    nx = wx * lc / lw;
    nz = wz * lc / lw;
  } else {
    float sn = ((th * th * 0.00761f - 0.16605f) * th * th + 1) * th;
    if (wz * cx - cz * wx < 0) sn = -sn;
    if (std::fabs(sn * sn + k * k - 1) > 0.001f) {
      float r = std::sqrt(sn * sn + k * k);
      sn /= r;
      k /= r;
    }
    nx = k * cx - sn * cz;
    nz = k * cz + sn * cx;
  }
  float l = std::sqrt(nx * nx + nz * nz);
  if (l <= 0) return false;
  m.fx = m.bx = nx / l;
  m.fz = m.bz = -nz / l;
  return true;
}

void MotionTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (m.bp && m.bp->motionType == kMotionAir) {  // aircraft (also dead ones: they fall)
    AirMotionTick(sim, u);
    return;
  }
  if (m.ballistic) {  // dropped from a transport: falls (also when dead)
    LandBallisticTick(sim, u);
    return;
  }
  if (!m.bp || !m.bp->mobile() || u->dead) {
    m.lastMove = {};
    return;
  }
  Vec3 start = u->position;
  const MotionBlueprint& b = *m.bp;
  bool moved = false;
  if (b.motionType == kMotionAir) {
    AirMotionTick(sim, u);
    return;
  } else {
    const bool oldFits = StandableAt(sim, b, start.x, start.z);
    // MotionTick pre-step: contacts with other units (they push each other apart)
    if (m.vel.x * m.vel.x + m.vel.z * m.vel.z > 1e-6f || m.surfaceNext) ProcessSurfaceCollision(sim, u);
    if (m.pointNow && !m.pushed) {
      // CalcMoveCommon: the unit moves onto the spline point the steering handed out
      const UnitMotion::SplinePoint& q = m.point;
      if (q.bx != m.bx || q.bz != m.bz) m.needSnap = true;  // turning on the spot
      u->position.x = q.pos.x;
      u->position.z = q.pos.z;
      m.vel = q.vel;
      m.fx = q.fx;
      m.fz = q.fz;
      m.bx = q.bx;
      m.bz = q.bz;
      moved = u->position.x != start.x || u->position.z != start.z;
    } else if (!m.hasGoal && !m.pushed) {
      bool turned = ArrivalTurn(sim, u);  // (also while it coasts to a stop)
      moved = Coast(u);
      if (turned) m.needSnap = true;
    } else {
      moved = Coast(u);  // pushed, or no spline point (the navigator is thinking): keeps rolling and slows down
    }
    if (moved && oldFits && !StandableAt(sim, b, u->position.x, u->position.z)) {
      float dx = start.x - u->position.x, dz = start.z - u->position.z;
      u->position = start;
      m.vel = {};
      if (!m.pushed) {
        float l = std::sqrt(dx * dx + dz * dz);
        float imp = (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult) * 0.010000001f;  // u+0x594
        Vec3 iv{};
        if (l > 0) iv = {dx / l * imp, 0, dz / l * imp};
        AddImpulse(u, iv);
      }
      moved = false;
    }
    if (moved || m.needSnap) {
      SnapUnit(sim, u);
      m.needSnap = false;
    }
    if (moved) UpdateLayer(sim, u);
    m.surfaceNext = false;  // (cleared after SnapToGround)
  }
  m.lastMove = {u->position.x - start.x, u->position.y - start.y, u->position.z - start.z};
}

}  // namespace moho
