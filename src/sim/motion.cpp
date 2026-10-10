// Unit motion (see motion.h for what the original does and how it was checked).
#include "core/dmath.h"
#include "sim/motion.h"
#include "sim/formation.h"
#include "sim/combat.h"
#include "sim/landnav.h"
#include "sim/navigation.h"
#include "sim/air.h"
#include "sim/transport.h"

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

void MotionNavBegin(Unit* u) {
  UnitMotion& m = u->motion;
  m.hasGoal = true;
  m.arrived = false;
  m.failed = false;
  m.navDriven = true;
  m.hasWaypoint = false;
  m.path.clear();
  m.pathIndex = 0;
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
  m.newSegment = true;
}

void MotionNavDone(Unit* u, bool succeeded) {
  UnitMotion& m = u->motion;
  m.hasGoal = false;
  m.hasWaypoint = false;
  m.navDriven = false;
  m.path.clear();
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

// One spline step (CAiPathSpline::Generate's loop body, FA exe 0x5b2ff0) toward the current
// waypoint. Returns false if the unit did not move.
bool DriveStep(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  const MotionBlueprint& b = *m.bp;
  float maxF = (m.speedCap > 0 ? m.speedCap : b.maxSpeed * m.speedMult) * 0.1f;  // u+0x594
  float maxR = b.maxSpeedReverse * m.speedMult * 0.1f;
  float acc = b.maxAccel * m.accMult * 0.01f;
  float brake = (b.maxBrake != 0.0f ? b.maxBrake : b.maxAccel) * m.accMult * 0.01f;
  float steer = (b.maxSteerForce != 0.0f ? b.maxSteerForce : b.maxAccel) * m.accMult * 0.01f;
  float turnRadius = b.turnRadius != 0.0f ? b.turnRadius / m.turnMult : 1e30f;
  float turnRate = b.turnRate * m.turnMult * kDegToRadTenth;
  bool wideTurner = b.turnRate < b.turnRadius;  // turns wider than it can rotate: slows in turns

  Vec3& p = u->position;
  const Vec3& tgt = m.path[m.pathIndex];
  bool last = m.pathIndex + 1 == m.path.size();
  int mode = (last && !m.passThrough) ? 0 : 1;  // 0: stop at the target, 1: drive through
  float dx = tgt.x - p.x, dz = tgt.z - p.z;
  float dist = std::sqrt(dx * dx + dz * dz);
  if (dist < 0.0010000000474974513f) return false;
  float tx = dx / dist, tz = dz / dist;
  float speed = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
  float dotFG = m.fx * tx + m.fz * tz;

  bool batchStart = m.newSegment;
  if (m.state == 8) {  // the last batch ended: the next one continues in state 7, reverse off
    m.state = 7;
    m.reverse = false;
    batchStart = true;
  }
  if (m.newSegment) {  // choose how to start (Generate with a new path)
    m.newSegment = false;
    m.reverse = false;
    m.state = 7;
    if (!(b.maxSpeedReverse <= 0.0f || b.rotateOnSpot)) {
      float speedFrac = b.maxSpeed > 0 ? speed * 10.0f / b.maxSpeed : 0;
      bool forward = m.vel.x * m.fx + m.vel.z * m.fz >= 0.0f;
      if (dotFG >= 0.0f || (speedFrac >= 0.5f && !wideTurner)) {
        if (forward) m.state = 7;
        else if (dotFG >= 0.0f) m.state = 6;
        else m.state = 5;
      } else if (speedFrac > 0.0099999998f && forward) {
        m.state = 4;
      } else {
        m.state = 5;
      }
    }
  }
  // the back-up flag is chosen when a batch starts (fresh, or continuing in state 5), not when
  // the state machine enters state 5 within a batch (land_motion_blocking.md 2.1)
  if (batchStart && m.state == 5 && dist < b.backUpDistance && dotFG < -0.5f) m.reverse = true;
  const int s = m.state;
  const bool backward = s == 5 || s == 6 || s == 2;

  // turn: at most max(speed / radius, turn rate)
  float turn = turnRate;
  if (speed / turnRadius >= turn) turn = speed / turnRadius;
  float speedFrac0 = maxF > 0 ? speed / maxF : 0;
  float limit = TurnSpeedLimit(b, maxF, turnRadius, turnRate, m.fx, m.fz, dx, dz, speedFrac0);
  if (!m.reverse) {
    RotateToward(m.fx, m.fz, tx, tz, turn);
  } else {
    float bx = -m.fx, bz = -m.fz;
    RotateToward(bx, bz, tx, tz, turn);
    m.fx = -bx;
    m.fz = -bz;
  }
  float mx = backward ? -m.fx : m.fx, mz = backward ? -m.fz : m.fz;
  float ml2 = mx * mx + mz * mz;
  // remove the sideways velocity (at most the steering force)
  float px = 0, pz = 0;
  if (ml2 > 0.0f) {
    float k = (m.vel.x * mx + m.vel.z * mz) / ml2;
    px = k * mx;
    pz = k * mz;
  }
  float lx = m.vel.x - px, lz = m.vel.z - pz;
  float ll = lx * lx + lz * lz;
  if (steer * steer < ll) {
    float k = steer / std::sqrt(ll);
    lx *= k;
    lz *= k;
  }
  m.vel.x -= lx;
  m.vel.z -= lz;
  float target = 0;
  bool braking = s == 1 || s == 3 || s == 4 || s == 6 || m.yielding;
  if (m.yielding) brake *= 2;
  if (!braking) {
    target = std::min(limit, (s == 5 || s == 2) ? maxR : maxF);
    if (mode == 0) {
      float d = dist;
      if (brake < dist) d = std::sqrt(dist * brake + dist * brake);
      if (d <= target) target = d;
    }
    if (wideTurner) target = (std::max(-0.5f, dotFG) + 1.0f) * 0.5f * target;
    if (target < 0.0010000000474974513f) braking = true;
  }
  if (braking) {  // keep the speed, ease the direction toward the move direction, brake
    target = 0;
    float vl = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
    float sx = mx, sz = mz;
    if (ml2 != 0.0f) {
      float k = vl / std::sqrt(ml2);
      sx = mx * k;
      sz = mz * k;
    }
    m.vel.x = sx * 0.200000003f + m.vel.x * 0.800000012f;
    m.vel.z = sz * 0.200000003f + m.vel.z * 0.800000012f;
  }
  float dvx = mx * target - m.vel.x, dvz = mz * target - m.vel.z;
  float lim = (dvx * m.vel.x + dvz * m.vel.z > 0.0f) ? acc : brake;
  float dl = dvx * dvx + dvz * dvz;
  if (lim * lim < dl) {
    float k = lim / std::sqrt(dl);
    dvx *= k;
    dvz *= k;
  }
  m.vel.x += dvx;
  m.vel.z += dvz;
  float cap = backward ? maxR : maxF;
  float vl2 = m.vel.x * m.vel.x + m.vel.z * m.vel.z;
  if (cap * cap < vl2) {
    float k = cap / std::sqrt(vl2);
    m.vel.x *= k;
    m.vel.z *= k;
  }
  p.x += m.vel.x;
  p.z += m.vel.z;
  UpdateBody(m);

  // state changes after the step
  float nspeed = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
  float frac = maxF > 0 ? nspeed / maxF : 0;
  float ndx = tgt.x - p.x, ndz = tgt.z - p.z;
  float ndist = std::sqrt(ndx * ndx + ndz * ndz);
  float ndot = ndist > 0 ? (m.fx * ndx + m.fz * ndz) / ndist : 1.0f;
  float stopDist = 0;
  float a = std::max(b.maxAccel, b.maxBrake);
  if (a > 0.0f) stopDist = (nspeed * 10.0f * nspeed * 10.0f) / (a * 2.0f);
  switch (s) {
    case 3:
      if (frac <= 0.0099999998f) m.state = 8;
      break;
    case 4:
      if (frac <= 0.0099999998f) m.state = (b.maxSpeedReverse <= 0.0f || b.rotateOnSpot) ? 7 : 5;
      break;
    case 5:
      if (!LookAheadFits(sim, b, p, m.vel.x, m.vel.z, stopDist)) m.state = 6;
      else if (!m.reverse ? ndot > 0.150000006f : stopDist > ndist) m.state = 6;
      break;
    case 6:
      if (frac <= 0.0099999998f) m.state = m.reverse ? 8 : 7;
      break;
    case 7:
      if (mode == 0 && ndist < stopDist) m.state = 3;
      if (ndot < 0.865999997f) {
        if (!LookAheadFits(sim, b, p, m.vel.x, m.vel.z, stopDist)) m.state = 4;
      }
      break;
    default:
      break;
  }
  if (m.state == 8) m.reverse = false;
  static const long dbgId = getenv("MOHO64_DEBUG_MOTION") ? atol(getenv("MOHO64_DEBUG_MOTION")) : -1;
  if (dbgId == static_cast<long>(u->id))
    fprintf(stderr, "motion: tick %u state %d->%d rev %d pos %.4f %.4f vel %.4f %.4f f %.4f %.4f tgt %.2f %.2f mode %d\n",
            sim.tick(), s, m.state, m.reverse ? 1 : 0, p.x, p.z, m.vel.x, m.vel.z, m.fx, m.fz, tgt.x, tgt.z, mode);
  return true;
}

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

namespace {
float Radius(const MotionBlueprint& b) { return (b.sizeX + b.sizeZ) * 0.25f; }
bool Driving(const Unit* u) { return u->motion.hasGoal; }
}  // namespace

void CollisionTick(Sim& sim) {
  // uniform grid of land/naval units (8x8 world-unit cells)
  constexpr float kCell = 8.0f;
  std::unordered_map<int64_t, std::vector<Unit*>> grid;
  std::vector<Unit*> movers;
  auto key = [](int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); };
  for (Unit* u : sim.units()) {
    if (u->destroyQueued || u->dead) continue;
    if (!u->motion.bp || u->motion.bp->motionType == kMotionAir) continue;
    if (u->fractionComplete < 1.0f) continue;
    if (u->parentId || u->layer == "Air") continue;  // carried units (transport cargo) do not collide
    grid[key(static_cast<int>(std::floor(u->position.x / kCell)), static_cast<int>(std::floor(u->position.z / kCell)))]
        .push_back(u);
    if (u->motion.bp->mobile() && Driving(u)) movers.push_back(u);
  }
  for (Unit* u : movers) {
    UnitMotion& m = u->motion;
    if (sim.tick() < m.driveTick) continue;
    const MotionBlueprint& b = *m.bp;
    float ru = Radius(b);
    float vx = m.vel.x, vz = m.vel.z;
    float sp = std::sqrt(vx * vx + vz * vz);
    if (sp <= 0.0f) continue;
    float fwx = vx / sp, fwz = vz / sp;
    float reach = ru + sp * 20.0f + 4.0f;
    int x0 = static_cast<int>(std::floor((u->position.x - reach) / kCell));
    int x1 = static_cast<int>(std::floor((u->position.x + reach) / kCell));
    int z0 = static_cast<int>(std::floor((u->position.z - reach) / kCell));
    int z1 = static_cast<int>(std::floor((u->position.z + reach) / kCell));
    Unit* hit = nullptr;
    int hitT = 1 << 30;
    bool push = false;
    for (int gx = x0; gx <= x1; ++gx)
      for (int gz = z0; gz <= z1; ++gz) {
        auto it = grid.find(key(gx, gz));
        if (it == grid.end()) continue;
        for (Unit* o : it->second) {
          if (o == u) continue;
          const UnitMotion& n = o->motion;
          float ro = n.bp ? Radius(*n.bp) : 0.5f;
          float rr = (ru + ro) * (ru + ro);
          float dx = o->position.x - u->position.x, dz = o->position.z - u->position.z;
          // only units ahead of it
          if (dx * fwx + dz * fwz <= 0.0f) continue;
          bool oMoving = Driving(o);
          float ovx = oMoving ? n.vel.x : 0, ovz = oMoving ? n.vel.z : 0;
          for (int t = 0; t <= 18; t += 3) {
            float px = dx + (ovx - vx) * t, pz = dz + (ovz - vz) * t;
            if (px * px + pz * pz < rr) {
              bool idle = !oMoving && o->commands.empty() && n.bp && n.bp->mobile();
              if (t < hitT) {
                hitT = t;
                hit = o;
                push = idle;
              }
              break;
            }
          }
        }
      }
    if (!hit) continue;
    if (push) {
      // close enough to touch: the idle unit is shoved along (AddImpulse: v = v/2 + impulse)
      float dx = hit->position.x - u->position.x, dz = hit->position.z - u->position.z;
      float d = std::sqrt(dx * dx + dz * dz);
      float ro = hit->motion.bp ? Radius(*hit->motion.bp) : 0.5f;
      if (d < ru + ro + sp && d > 0.0f) {
        UnitMotion& n = hit->motion;
        n.vel.x = n.vel.x * 0.5f + dx / d * sp;
        n.vel.z = n.vel.z * 0.5f + dz / d * sp;
      }
      continue;
    }
    if (!m.yielding && m.state == 7) {
      m.yielding = true;
      m.yieldTarget = EntityRef(hit);
    }
  }
}

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
  Vec3 fv = u->form->fwd;
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
    bool coastPush = false;
    if (m.pushed) {  // ProcessSplineMovement: the push decays, then the steering drives again
      float v = std::sqrt(m.vel.x * m.vel.x + m.vel.z * m.vel.z);
      if (v < b.maxSpeed * m.speedMult * 0.01f) {
        m.pushed = false;
        if (m.wasMoving) {
          m.wasMoving = false;
          LandNavPoke(u);
          if (m.hasWaypoint) m.newSegment = true;
        }
      } else {
        coastPush = true;
      }
    }
    if (coastPush) {
      moved = Coast(u);
    } else if (m.hasGoal && m.navDriven) {
      if (!m.hasWaypoint) {
        moved = Coast(u);  // the navigator is thinking: keeps rolling and slows down
      } else {
        moved = DriveStep(sim, u);
        if (m.yielding && m.vel.x * m.vel.x + m.vel.z * m.vel.z <= kStopSq) {
          m.vel = {};
          m.yielding = false;
          m.newSegment = true;
        }
        if (moved) {
          // the steering arrived at its waypoint (its spline ended in the waypoint's cell): it
          // stops and the unit coasts until the navigator gives the next one
          int cx, cz;
          GoalCell(b, u->position.x, u->position.z, &cx, &cz);
          if (cx == m.goalCellX && cz == m.goalCellZ && m.state == 8) m.hasWaypoint = false;
        } else {
          m.hasWaypoint = false;
        }
      }
    } else if (m.hasGoal) {
      if (sim.tick() < m.driveTick) {
        moved = Coast(u);  // waiting for the path: keeps rolling and slows down
      } else {
        moved = DriveStep(sim, u);
        if (m.yielding && m.vel.x * m.vel.x + m.vel.z * m.vel.z <= kStopSq) {
          m.vel = {};
          m.yielding = false;
          m.newSegment = true;
          m.driveTick = sim.tick() + 2;  // stands for a tick, then drives on
        }
        if (moved) {
          int cx, cz;
          GoalCell(b, u->position.x, u->position.z, &cx, &cz);
          const Vec3& wp = m.path[m.pathIndex];
          int wx, wz;
          GoalCell(b, wp.x, wp.z, &wx, &wz);
          bool inCell = cx == wx && cz == wz;
          if (m.pathIndex + 1 == m.path.size()) {
            if (inCell || m.state == 8) {  // the goal cell, or stopped short of it
              m.hasGoal = false;
              m.arrived = true;
              sim.ResumeCommandThread(u);
            }
          } else if (inCell || m.state == 8) {  // next waypoint
            ++m.pathIndex;
            m.newSegment = true;
          }
        }
      }
    } else {
      bool turned = ArrivalTurn(sim, u);  // (also while it coasts to a stop)
      moved = Coast(u);
      if (turned) m.needSnap = true;
    }
    // CalcMoveCommon 0x6c1e20: from a cell the footprint fits into one it does not: undone, and
    // pushed back by MaxSpeed/100 along the step (AddImpulse 0x6b8ac0)
    if (moved && oldFits && !StandableAt(sim, b, u->position.x, u->position.z)) {
      float dx = start.x - u->position.x, dz = start.z - u->position.z;
      u->position = start;
      m.vel = {};
      if (!m.pushed) {
        float l = std::sqrt(dx * dx + dz * dz);
        float imp = b.maxSpeed * m.speedMult * 0.010000001f;
        if (l > 0) {
          m.vel.x = dx / l * imp;
          m.vel.z = dz / l * imp;
        }
        if (m.hasGoal && m.navDriven && m.hasWaypoint) m.wasMoving = true;
        m.pushed = true;
      }
      moved = false;
    }
    if (moved || m.needSnap) {
      SnapUnit(sim, u);
      m.needSnap = false;
    }
    if (moved) UpdateLayer(sim, u);
  }
  m.lastMove = {u->position.x - start.x, u->position.y - start.y, u->position.z - start.z};
}

}  // namespace moho
