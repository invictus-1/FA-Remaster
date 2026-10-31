// Aircraft flight model (see air.h for what the original does). The arithmetic follows the
// original's operation order (float32; the x87 parts too: the sim thread runs at precision control 24), because the
// trajectory is a feedback loop: a last-bit difference grows. Function names and addresses refer
// to the FA exe; the specs in engine-ref/specs/air_*.md give the details.
#include "core/dmath.h"
#include "sim/air.h"
#include "sim/formation.h"
#include "sim/intel.h"
#include "sim/landnav.h"
#include "sim/commands.h"
#include "sim/motion.h"
#include "sim/transport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#include "core/backtrace.h"
#include "core/log.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/commands.h"
#include "sim/economy.h"
#include "sim/combat.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

namespace combat {
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
bool WaterPlaneSegmentHit(Vec3 p0, Vec3 p1, float water, float* dist, Vec3* hit);
bool SegmentStartUnderWater(Vec3 p0, Vec3 p1, float water, float* dist, Vec3* hit);
bool HasTarget(Sim& sim, const AiTarget& t);
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
Vec3 TargetPosBeatStart(Sim& sim, const AiTarget& t);
Entity* TargetEntity(Sim& sim, const AiTarget& t);
}  // namespace combat

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int kLand = 1, kSeabed = 2, kSub = 4, kWater = 8, kAir = 0x10;
constexpr float kGravityY = -4.9f;

// A target's point as the flight code sees it. The flight code runs inside the motion pass, and an entity's new
// transform is committed only at the beat's end: a target WITHOUT an aim bone (the centre of its collision
// primitive) stands and lies, for the original, as the beat began even when the target has already taken its
// step. (With an aim bone the point comes from the pose, which moved with the target's own step.)
Vec3 TargetPosNow(Sim& sim, const AiTarget& t) { return combat::TargetPosBeatStart(sim, t); }

int LayerBits(const std::string& l) {
  if (l == "Land") return kLand;
  if (l == "Seabed") return kSeabed;
  if (l == "Sub") return kSub;
  if (l == "Water") return kWater;
  if (l == "Air") return kAir;
  if (l == "Orbit") return 0x20;
  return 0;
}
const char* LayerName(int b) {
  switch (b) {
    case kLand: return "Land";
    case kSeabed: return "Seabed";
    case kSub: return "Sub";
    case kWater: return "Water";
    case kAir: return "Air";
    case 0x20: return "Orbit";
    default: return "None";
  }
}

// ---------------------------------------------------------------------------------------------
// Math helpers (exact operation order of the original's routines)

struct Mat3 {
  float m[3][3];
};

// func_QuatToMatrix 0x452fd0
Mat3 QuatToMatrix(const QuatW& q) {
  float tx = q.x * 2.0f, ty = q.y * 2.0f, tz = q.z * 2.0f;
  float twx = q.w * tx, twy = q.w * ty, twz = q.w * tz;
  float txx = q.x * tx, txy = q.x * ty, txz = q.x * tz;
  float tyy = q.y * ty, tyz = q.y * tz, tzz = q.z * tz;
  Mat3 M;
  M.m[0][0] = 1.0f - (tzz + tyy);
  M.m[0][1] = txy - twz;
  M.m[0][2] = txz + twy;
  M.m[1][0] = txy + twz;
  M.m[1][1] = 1.0f - (tzz + txx);
  M.m[1][2] = tyz - twx;
  M.m[2][0] = txz - twy;
  M.m[2][1] = tyz + twx;
  M.m[2][2] = 1.0f - (tyy + txx);
  return M;
}

// Moho::MultQuadVec 0x452d40: rotate v by q.
Vec3 MultQuadVec(const QuatW& q, const Vec3& v) {
  Mat3 M = QuatToMatrix(q);
  Vec3 r;
  r.x = ((0.0f + v.x * M.m[0][0]) + v.y * M.m[0][1]) + v.z * M.m[0][2];
  r.y = ((0.0f + v.x * M.m[1][0]) + v.y * M.m[1][1]) + v.z * M.m[1][2];
  r.z = ((0.0f + v.x * M.m[2][0]) + v.y * M.m[2][1]) + v.z * M.m[2][2];
  return r;
}

struct Axes {
  Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
};

// VAxes3::VAxes3(quat) 0x4ec590
Axes VAxes3(const QuatW& q) {
  float tw = q.w * 2.0f, tx = q.x * 2.0f, ty = q.y * 2.0f, tz = q.z * 2.0f;
  float yy = ty * q.y, zz = tz * q.z, xz = tz * q.x, xy = ty * q.x, xx = tx * q.x;
  float wx = tw * q.x, yz = tz * q.y, wy = tw * q.y, wz = tw * q.z;
  Axes a;
  a.X = {1.0f - (zz + yy), wz + xy, xz - wy};
  a.Y = {xy - wz, 1.0f - (zz + xx), yz + wx};
  a.Z = {wy + xz, yz - wx, 1.0f - (yy + xx)};
  return a;
}

// 0x44f7e0: out-of-place normalise (zero vector if the length is not > 0)
Vec3 Normalized(const Vec3& v) {
  float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
  double n = std::sqrt(static_cast<double>(s));
  if (!(n > 0.0)) return {};
  float inv = 1.0f / static_cast<float>(n);
  return {v.x * inv, inv * v.y, inv * v.z};
}

// 0x452af0: normalise in place; returns the length (zero vector and 0 if the length is <= 1e-6: 0x452b92)
double NormalizeInPlace(Vec3& v) {
  float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
  const float n = std::sqrt(s);  // compared rounded to float, as the x87 holds it at PC_24 (0x452b3e..0x452b51)
  if (n > 9.99999997e-07f) {
    float inv = 1.0f / n;
    v.x *= inv;
    v.y *= inv;
    v.z *= inv;
    return n;
  }
  v = {};
  return 0.0;
}

// func_NormalizeQuatInPlace 0x4edaa0
void NormalizeQuat(QuatW& q) {
  float s = ((q.w * q.w + q.x * q.x) + q.y * q.y) + q.z * q.z;
  const float n = std::sqrt(s);  // compared rounded to float (x87 at PC_24)
  if (n > 9.99999997e-07f) {
    float inv = 1.0f / n;
    q.w *= inv;
    q.x *= inv;
    q.y = inv * q.y;
    q.z = inv * q.z;
  } else {
    q = {0, 0, 0, 0};
  }
}

// func_VecSetLengthS 0x5b1c90
bool SetLength(Vec3& v, float L) {
  float n2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
  if (!(n2 > 0.0f)) return false;
  // x87 with precision control 24 bits (the sim thread's control word, confirmed by labhook 0.6 'A' records:
  // 21221 single-only matches, 0 double-only): every x87 op rounds like a float op
  float s = L / std::sqrt(n2);
  v.x = v.x * s;
  v.y = s * v.y;
  v.z = s * v.z;
  return true;
}

bool IsZeroBits(const Vec3& v) {  // 0x4f0a50: memcmp against (0,0,0): -0.0 is not zero
  static const Vec3 z{};
  return std::memcmp(&v, &z, sizeof(Vec3)) == 0;
}

// func_VecToQuatB 0x697360: rotation vector -> quaternion
QuatW VecToQuat(Vec3 v) {
  double n = NormalizeInPlace(v);
  float half = static_cast<float>(n * 0.5);
  float s = dmath::Sin(half), c = dmath::Cos(half);
  return {c, v.x * s, v.y * s, v.z * s};
}

// q (x) p (0x6978d0's product: body quaternion times a body-frame increment)
QuatW QMulBody(const QuatW& q, const QuatW& p) {
  QuatW r;
  r.w = ((q.w * p.w - q.x * p.x) - q.y * p.y) - q.z * p.z;
  r.x = ((q.y * p.z + q.w * p.x) + q.x * p.w) - q.z * p.y;
  r.y = ((q.z * p.x + q.w * p.y) + q.y * p.w) - q.x * p.z;
  r.z = ((q.x * p.y + q.w * p.z) + q.z * p.w) - q.y * p.x;
  return r;
}

// QuaternionMath::QuatToAxisAndAngle 0x6c1070
void QuatToAxisAngle(const QuatW& q, Vec3* axis, float* angle) {
  float s2 = (q.x * q.x + q.y * q.y) + q.z * q.z;
  if (s2 > 9.99999997e-07f) {
    float a = (q.w > -1.0f) ? ((1.0f > q.w) ? dmath::Acos(q.w) : 0.0f) : 3.14159274f;
    *angle = a * 2.0f;
    float inv = 1.0f / std::sqrt(s2);  // x87 at PC_24
    *axis = {q.x * inv, inv * q.y, inv * q.z};
  } else {
    *axis = {1, 0, 0};
    *angle = 0;
  }
}

// VAxes3::OrthoNormalize 0x4ec720
void OrthoNormalize(Axes& a) {
  Vec3& X = a.X;
  Vec3& Y = a.Y;
  Vec3& Z = a.Z;
  X = {Y.y * Z.z - Y.z * Z.y, Z.x * Y.z - Y.x * Z.z, Y.x * Z.y - Z.x * Y.y};
  NormalizeInPlace(X);
  Z = {X.y * Y.z - X.z * Y.y, Y.x * X.z - X.x * Y.z, X.x * Y.y - Y.x * X.y};
  NormalizeInPlace(Z);
  Y = {Z.y * X.z - Z.z * X.y, Z.z * X.x - Z.x * X.z, Z.x * X.y - Z.y * X.x};
}

// func_MatrixToQuat_0 0x4f0ae0 (+ Wm3 FromRotationMatrix 0x4f0cb0): the axes become columns.
QuatW AxesToQuat(const Axes& a) {
  float M[3][3] = {{a.X.x, a.Y.x, a.Z.x}, {a.X.y, a.Y.y, a.Z.y}, {a.X.z, a.Y.z, a.Z.z}};
  QuatW q;
  float tr = (M[0][0] + M[1][1]) + M[2][2];
  if (tr > 0.0f) {
    double r = std::sqrt(static_cast<double>(tr + 1.0f));
    q.w = static_cast<float>(r * 0.5);
    float inv = 0.5f / static_cast<float>(r);
    q.x = (M[2][1] - M[1][2]) * inv;
    q.y = (M[0][2] - M[2][0]) * inv;
    q.z = (M[1][0] - M[0][1]) * inv;
  } else {
    static const int next[3] = {1, 2, 0};
    int i = (M[1][1] > M[0][0]) ? 1 : 0;
    if (M[2][2] > M[i][i]) i = 2;
    int j = next[i], k = next[j];
    double r = std::sqrt(static_cast<double>(((M[i][i] - M[j][j]) - M[k][k]) + 1.0f));
    float v[3];
    v[i] = static_cast<float>(r * 0.5);
    float inv = 0.5f / static_cast<float>(r);
    q.w = (M[k][j] - M[j][k]) * inv;
    v[j] = (M[j][i] + M[i][j]) * inv;
    v[k] = (M[k][i] + M[i][k]) * inv;
    q.x = v[0];
    q.y = v[1];
    q.z = v[2];
  }
  return q;
}

// The two global +-90 degree yaw quaternions (0x10b6178 / 0x10b6158), built like the original.
const QuatW& Q90() {
  static const QuatW q = [] {
    float c = dmath::Cos(0.785398185f), s = dmath::Sin(0.785398185f);
    return QuatW{c, s * 0.0f, s, s * 0.0f};
  }();
  return q;
}
const QuatW& Qm90() {
  static const QuatW q = [] {
    float c = dmath::Cos(-0.785398185f), s = dmath::Sin(-0.785398185f);
    return QuatW{c, s * 0.0f, s, s * 0.0f};
  }();
  return q;
}

QuatW ToW(const Quat& q) { return {q.w, q.x, q.y, q.z}; }
Quat FromW(const QuatW& q) { return {q.x, q.y, q.z, q.w}; }

// floor as the engine does it: round half to even, minus one if above the value
int EngineFloor(float x) {
  float r = std::nearbyint(x);
  int i = static_cast<int>(r);
  return x < r ? i - 1 : i;
}
int EngineCeil(float x) {
  float r = std::nearbyint(x);
  int i = static_cast<int>(r);
  return x > r ? i + 1 : i;
}

// ---------------------------------------------------------------------------------------------
// Terrain helpers

// STIMap::GetElevation 0x44fb90 (bilinear, z first, on the x87 stack)
float Elevation(const TerrainMap* map, float x, float z) {
  if (!map) return 0;
  int ix = EngineFloor(x), iz = EngineFloor(z);
  float fx = x - static_cast<float>(ix), fz = z - static_cast<float>(iz);
  float a = map->HeightAt(ix, iz);  // x87 at PC_24: each step rounds to float
  a = (map->HeightAt(ix, iz + 1) - a) * fz + a;
  float b = map->HeightAt(ix + 1, iz);
  b = (map->HeightAt(ix + 1, iz + 1) - b) * fz + b;
  return (b - a) * fx + a;
}
float WaterOrNone(const TerrainMap* map) { return (map && map->hasWater) ? map->waterElevation : -10000.0f; }
// STIMap::GetElevation 0x6bc400: raw sample at the rounded point, raised to the water
float SampleAt(const TerrainMap* map, int ix, int iz, bool terrainOnly) {
  if (!map) return 0;
  float h = map->HeightAt(ix, iz);
  if (!terrainOnly && map->hasWater && map->waterElevation > h) h = map->waterElevation;
  return h;
}

// The height field's min/max pyramid (CHeightField tiers; built once per map).
struct Tiers {
  const TerrainMap* map = nullptr;
  int W = 0, H = 0;  // samples
  std::vector<int> tw, th;
  std::vector<std::vector<uint32_t>> hi;  // max sample of each tier cell
};
std::map<const TerrainMap*, Tiers>& TierCache(const Sim& sim) {
  // (the map is the sim's, and changes as it is flattened)
  static const SimCache<std::map<const TerrainMap*, Tiers>> slot;
  return slot.For(sim);
}
int BitLen(unsigned v) {
  int n = 0;
  while (v) {
    ++n;
    v >>= 1;
  }
  return n;
}
// One tier cell (CHeightField::UpdateBounds 0x476bb0): tier 1 from the 3x3 samples, higher tiers from 2x2 children.
uint32_t TierCell(const Tiers& t, const TerrainMap* map, int k, int i, int j) {
  auto raw = [&](int x, int z) {
    x = std::clamp(x, 0, t.W - 1);
    z = std::clamp(z, 0, t.H - 1);
    return static_cast<uint32_t>(std::lround(map->HeightAt(x, z) * 128.0f));
  };
  uint32_t m = 0;
  int s = 1 << k;
  if (k == 1) {
    int x0 = i * s, z0 = j * s;
    int x1 = std::min(x0 + s, t.W - 1), z1 = std::min(z0 + s, t.H - 1);
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) m = std::max(m, raw(x, z));
    return m;
  }
  const auto& p = t.hi[k - 2];
  int pw = t.tw[k - 2], ph = t.th[k - 2];
  for (int dz = 0; dz < 2; ++dz)
    for (int dx = 0; dx < 2; ++dx) {
      int ci = std::min(2 * i + dx, pw - 1), cj = std::min(2 * j + dz, ph - 1);
      m = std::max(m, p[static_cast<size_t>(cj) * pw + ci]);
    }
  return m;
}
const Tiers& GetTiers(const Sim& sim, const TerrainMap* map) {
  auto& c = TierCache(sim);
  auto it = c.find(map);
  if (it != c.end()) return it->second;
  Tiers& t = c[map];
  t.map = map;
  int w = map->width(), h = map->height();
  t.W = w + 1;
  t.H = h + 1;
  int n = BitLen(static_cast<unsigned>(std::max(w, h) - 1));
  for (int k = 1; k <= n; ++k) {
    int tw = std::max(1, w >> k), th = std::max(1, h >> k);
    t.tw.push_back(tw);
    t.th.push_back(th);
    t.hi.emplace_back(static_cast<size_t>(tw) * th, 0);
    for (int j = 0; j < th; ++j)
      for (int i = 0; i < tw; ++i) t.hi.back()[static_cast<size_t>(j) * tw + i] = TierCell(t, map, k, i, j);
  }
  return t;
}

// STIMap::LookAheadForMaxTerrain 0x62d620
float LookAheadForMaxTerrain(const Sim& sim, const TerrainMap* map, const Vec3& pos, float radius, bool terrainOnly) {
  if (!map) return 0;
  if (1.0f > radius) {
    float h = Elevation(map, pos.x, pos.z);
    if (!terrainOnly && map->hasWater && map->waterElevation > h) h = map->waterElevation;
    return h;
  }
  const Tiers& t = GetTiers(sim, map);
  unsigned mn = static_cast<unsigned>(std::min(t.W - 1, t.H - 1));
  int levels = BitLen(mn - 1);
  int r = static_cast<int>(std::nearbyint(radius * 0.5f));
  int k = r != 0 ? BitLen(static_cast<unsigned>(r)) : 0;
  int tier = std::max(1, std::min(k, levels));
  if (tier > static_cast<int>(t.hi.size())) tier = static_cast<int>(t.hi.size());
  float h;
  if (tier <= 0) {
    h = Elevation(map, pos.x, pos.z);
  } else {
    int ix = static_cast<int>(std::nearbyint(pos.x)) >> tier, iz = static_cast<int>(std::nearbyint(pos.z)) >> tier;
    int tw = t.tw[tier - 1], th = t.th[tier - 1];
    ix = std::clamp(ix, 0, tw - 1);
    iz = std::clamp(iz, 0, th - 1);
    h = static_cast<float>(t.hi[tier - 1][static_cast<size_t>(iz) * tw + ix]) * 0.0078125f;
  }
  if (!terrainOnly) {
    float w = WaterOrNone(map);
    if (w > h) h = w;
  }
  return h;
}

// ---------------------------------------------------------------------------------------------

AirMotion& A(Unit* u) { return *u->motion.air; }
bool State(const Unit* u, const char* s) { return u->unitStates.count(s) != 0; }
bool State(const Unit* u, int i) { return u->unitStates.Has(i); }  // (i: US("Name"))
void SetState(Unit* u, const char* s, bool on) {
  if (on) u->unitStates.insert(s);
  else u->unitStates.erase(s);
}
void SetState(Unit* u, int i, bool on) {  // (i: US("Name"))
  if (on) u->unitStates.Insert(i);
  else u->unitStates.Erase(i);
}

void Callback2(Sim& sim, Unit* u, const char* method, const char* a, const char* b) {
  lua_State* L = sim.L();
  lua_pushstring(L, a);
  lua_pushstring(L, b);
  sim.CallMethod(L, u, method, 2);
}

void SetLayerBits(Sim& sim, Unit* u, int layer) {
  const char* name = LayerName(layer);
  if (u->layer == name) return;
  std::string old = u->layer;
  u->layer = name;
  Callback2(sim, u, "OnLayerChange", name, old.c_str());
}

const char* kVertNames[] = {"Top", "Bottom", "Up", "Down", "Hover"};
const char* kHorzNames[] = {"Cruise", "TopSpeed", "Stopping", "Stopped"};
const char* kStateNames[] = {"None", "Attached", "Ballistic", "Crashed", "ArmyPool"};

void SetVertEvent(Sim& sim, Unit* u, int e) {
  AirMotion& a = A(u);
  if (a.vertEvent == e) return;
  int old = a.vertEvent;
  a.vertEvent = e;
  Callback2(sim, u, "OnMotionVertEventChange", kVertNames[e], kVertNames[old]);
}
void SetHorzEvent(Sim& sim, Unit* u, int e) {
  AirMotion& a = A(u);
  if (a.horzEvent == e) return;
  int old = a.horzEvent;
  a.horzEvent = e;
  Callback2(sim, u, "OnMotionHorzEventChange", kHorzNames[e], kHorzNames[old]);
  if (a.horzEvent == 3) IntelExactUpdate(sim, u);  // CUnitMotion::SetMotionHorzEvent 0x6b8f5a..0x6b8f65
}
void SetMotionState(Sim& sim, Unit* u, int s) {
  AirMotion& a = A(u);
  if (a.motionState == s) return;
  int old = a.motionState;
  a.motionState = s;
  Callback2(sim, u, "OnMotionStateChange", kStateNames[s], kStateNames[old]);
}

bool InCategory(Sim& sim, Unit* u, const char* cat) { return BpInCategory(sim, u->blueprint, cat); }

// Unit::UpdateInfoCache: the speed cap (u+0x594). The air motion reads the cached value (CalcWingedOrientation
// 0x6bd873, CalcAirMovementDampingFactor 0x6bcae4, CalcMoveAir 0x6bf0fa / 0x6bf105, ...), as UpdateInfoCache wrote it
// in this beat's MotionTick before the fuel callbacks (see Sim::Tick, MP3 5356); computed here only before the
// unit's first MotionTick.
float MaxSpeed(Unit* u) {
  if (u->motion.maxSpeedU594 > 0) return u->motion.maxSpeedU594;
  const AirBp& b = *A(u).bp;
  float ms = b.maxAirspeed * u->motion.speedMult / 1.0f;
  if (u->motion.speedCap > 0) ms = u->motion.speedCap;  // UpdateInfoCache: the formation cap
  return ms;
}

// CUnitMotion::GetElevation 0x6bc8e0
// carrierEvent 1 (approaching a carrier, carriers.md 13.5): climb to the deck bone when it is above the
// aircraft, otherwise a quarter of the elevation (no random term)
float GetElevationWanted(const AirMotion& a, float unitY) {
  if (a.carrierEvent == 1) {
    if (!(a.landHeight == kInf) && a.landHeight > unitY) return a.landHeight - a.curTerrain;
    return a.elevationAttr * 0.25f;
  }
  return a.elevationAttr + a.randElev;
}

// CUnitMotion::ShouldHoverInsteadOfLand 0x6bc820
bool ShouldHover(Unit* u) {
  const AirBp& b = *A(u).bp;
  return b.transportHoverHeight > 0 && (State(u, US("TransportLoading")) || TransportHasCargo(u));
}

bool IsMoveLikeType(CommandType t) {
  switch (t) {
    case CommandType::Move: case CommandType::FormMove: case CommandType::Attack: case CommandType::FormAttack:
    case CommandType::Guard: case CommandType::Patrol: case CommandType::FormPatrol:
      return true;
    default:
      return false;
  }
}

// Unit::UpdateSpeedThroughStatus 0x6ac940 -> navigator.SetSpeedThroughGoal (air_nav.md 5). The exe calls it only on
// events (a command added to the queue, move/patrol task ctors, the guard's re-goal and dtor, refuel ctor/dtor,
// the landing branches below); the flag keeps its value in between. E.g. a build drone (NOFORMATION) gets its
// Guard command (event: not yet Guarding -> false) and never re-goals, so it circles with speed-through off.
void UpdateSpeedThrough(Sim& sim, Unit* u) {
  if (!u->motion.bp) return;
  if (u->beingBuilt || u->dead || u->destroyQueued) return;
  const bool air = u->motion.bp->motionType == kMotionAir;
  // (a land navigator gets the same status: 0x6ac940 is one function for every unit)
  if (!air && (!u->motion.nav || !u->motion.bp->mobile())) return;
  const UnitCommand* cur = u->commands.empty() ? nullptr : u->commands[0].get();
  const UnitCommand* next = u->commands.size() > 1 ? u->commands[1].get() : nullptr;
  bool v;
  if (State(u, US("Refueling"))) v = false;  // (state 30, 0x6ac9c9)
  else if (State(u, US("ForceSpeedThrough")) || State(u, US("CannotFindPlaceToLand"))) v = true;  // (32, 36)
  // 0x6ac9ff..0x6aca58: the guard rule asks the blueprint's Air.CanFly (ubp+0x368) first, so it is for flyers only.
  // (Seen: Economy 3839, land SACU 0xc guarding with a Reclaim queued behind its Guard: the original's speed-through
  // stays 0, ours turned it on.)
  else if (u->motion.bp->canFly && State(u, US("Guarding")) && !State(u, US("Ferrying")) &&
           !InCategory(sim, u, "EXPERIMENTAL"))
    v = true;
  else if (cur && State(u, US("AssistMoving"))) v = false;  // (state 22, 0x6aca87)
  // 0x6aca91..0x6acac2: a formation follower (a leader link, unit+0x584, to another unit) on a move-like head: off
  else if (cur && IsMoveLikeType(cur->type) && u->formLeader && u->formLeader != EntityRef(u)) v = false;
  // 0x6acac4..0x6acaef: a next order, not coordinated (IsCoordinating 0x5f24e0), and both move-like (0x552c10)
  else v = cur && next && !IsCoordinating(*next) && IsMoveLikeType(cur->type) && IsMoveLikeType(next->type);
  if (!air) {
    LandNavSetSpeedThrough(u, v);
    return;
  }
  u->motion.airSpeedThrough = v;
  if (u->motion.air) A(u).fullSpeed = v;
}

// --------------------------------------------------------------------------------------------
// Landing spots (Unit::PrepareMove for flyers; predicates of air_extra.md 3)

}  // namespace

// The sim's landing-spot reservations, one byte per cell (Sim::airState). Multicore step 1: these were in a
// process-wide map by the sim's address (ResMap).
struct AirState {
  int w = 0, h = 0;
  std::vector<uint8_t> bits;
};

namespace {

using Reservations = AirState;
Reservations& Res(Sim& sim) {
  if (!sim.airState) sim.airState = std::make_shared<AirState>();
  Reservations& r = *sim.airState;
  if (r.bits.empty() && sim.map()) {
    r.w = sim.map()->width();
    r.h = sim.map()->height();
    r.bits.assign(static_cast<size_t>(r.w) * r.h, 0);
  }
  return r;
}
bool RectEmpty(const int* r) {  // FreeOgridRect's quirky emptiness test
  auto q = [](int x0, int x1) { return std::make_pair(std::max(0, x0 >> 2), std::max(1, ((x1 + 3) >> 2) - (x0 >> 2))); };
  return q(r[0], r[2]) == q(0, 1) && q(r[1], r[3]) == q(0, 1);
}
void FillRes(Sim& sim, const int* r, uint8_t v) {
  Reservations& R = Res(sim);
  for (int z = std::max(0, r[1]); z < std::min(R.h, r[3]); ++z)
    for (int x = std::max(0, r[0]); x < std::min(R.w, r[2]); ++x) R.bits[static_cast<size_t>(z) * R.w + x] = v;
}
bool AnyRes(Sim& sim, const int* r) {
  Reservations& R = Res(sim);
  if (r[0] < 0 || r[1] < 0 || r[2] >= R.w || r[3] >= R.h) return r[2] > r[0] && r[3] > r[1];
  for (int z = r[1]; z < r[3]; ++z)
    for (int x = r[0]; x < r[2]; ++x)
      if (R.bits[static_cast<size_t>(z) * R.w + x]) return true;
  return false;
}
// A unit holds ONE o-grid rect, unit+0x5a8 (Unit::ogridRect): ReserveOgridRect 0x6ab6f0 frees the one held and takes
// the new, FreeOgridRect 0x6ab760 frees it, CanReserveOgridRect 0x6ab810 tests a rect with it lifted. The move task's
// landing spot (CUnitMoveTask ctor 0x61862c, its command listener 0x618dbf) and the flight's own landing spot
// (0x6bf5e6..0x6bf60a) are both that rect; the flight keeps a copy of what it reserved at motion+0x158 (landRect).
// Seen: MP1 10588, the player drags the unload point of army 2's transport 0x20011a (SetCommandTarget) a cell west
// of its landing spot (428.5, 9.0), which its move task holds; the listener's PrepareMove (0x618d20, before the rect
// is let go at 0x618d83) finds the new cell (427.5, 9.0) free in the original, as its own rect is lifted. Ours lifted
// only the flight's rect, found the cell held by itself and took (436, 9) of the ring.
void GroundFreeRectImpl(Sim& sim, Unit* u);
void GroundReserveRectImpl(Sim& sim, Unit* u, const int r[4]);
// CUnitMotion::SetTarget 0x6b8883..0x6b88d8: the unit's rect is freed only while it is the one the flight reserved;
// the copy is cleared either way
void FreeLandRect(Sim& sim, Unit* u) {
  AirMotion& a = A(u);
  if (std::equal(a.landRect, a.landRect + 4, u->ogridRect)) GroundFreeRectImpl(sim, u);
  std::fill(a.landRect, a.landRect + 4, 0);
}
// the touchdown 0x6bfb20..0x6bfb40: the copy cleared, the unit's rect freed whatever holds it
void FreeLandRectOnTouchdown(Sim& sim, Unit* u) {
  AirMotion& a = A(u);
  std::fill(a.landRect, a.landRect + 4, 0);
  GroundFreeRectImpl(sim, u);
}
void ReserveLandRect(Sim& sim, Unit* u, const int* r) {
  AirMotion& a = A(u);
  std::copy(r, r + 4, a.landRect);
  GroundReserveRectImpl(sim, u, r);
}
bool CanReserve(Sim& sim, Unit* u, const int* r) {
  bool own = !RectEmpty(u->ogridRect);
  if (own) FillRes(sim, u->ogridRect, 0);
  bool blocked = AnyRes(sim, r);
  if (own) FillRes(sim, u->ogridRect, 1);
  return !blocked;
}

// STIMap::IsWithin 0x62ca60 with the unit's army's UseWholeMap (vt+0xb8): PrepareMove's candidates (0x62ba1c,
// 0x62bc62: the flag its callers pass) and ComputeAirCombatTactics (0x6bce16 / 0x6bd036 / 0x6bd110)
bool IsWithin(Sim& sim, const Unit* u, const Vec3& p, float m) {
  if (!sim.map()) return true;
  return sim.IsWithin(p.x, p.z, m, Sim::UseWholeMap(u->army));
}

// COORDS_CanMoveAt 0x720f70 with strict = 1 (air_extra.md 3.6): no live, completed, non-air unit
// other than u whose box touches [x0, x1] x [z0, z1]; carried units never block. A transport (u has
// a transport object) ignores moving non-transport units, and all non-transport units while it is
// TransportLoading; other transports always block (strict).
bool NoBlockingUnits(Sim& sim, Unit* u, float x0, float z0, float x1, float z1) {
  bool blocked = false;
  const bool selfTransport = u->transport != nullptr;
  const bool loading = selfTransport && State(u, US("TransportLoading"));
  sim.ForUnitsInRect(x0 - 8, z0 - 8, x1 + 8, z1 + 8, [&](Unit* o) {
    if (blocked || o == u || o->dead || o->destroyQueued || o->fractionComplete < 1.0f) return;
    if (o->layer == "Air") return;
    if (u->layer == "Sub" && o->layer != "Sub") return;
    if (!o->transport || InCategory(sim, o, "PODSTAGINGPLATFORM")) {
      // (0x721231..0x72124f: GetVelocity, vt+0x3c, against zero by a memcmp of 12 bytes: UnitVelocityBitsSet; the
      // test the formation spots use too, as the same function with strict 0)
      const bool moving = UnitVelocityBitsSet(o);
      if (selfTransport && (moving || loading)) return;
    }
    // (0x7211bf..0x7211cc: unit state Attached (IsUnitState 0xe, vt+0x3c) lets a candidate off, not a parent link: a
    // unit without a motion hung on something never takes that state (SetAttachedState) and blocks. Seen (probe
    // mp1_15012): MP1 15010, army 6's 29-unit Guard of its air factory 0x600057; the hover engineer 0x6000f7's spot
    // search finds (994,568) free in ours, but army 0's dummy zxa0003 0x1cf (attached, no Attached state, Seabed,
    // at 994.0, 568.39) stands on it: the original's SetGoal gives 0x6000f7 (1000,564) and each guard after it the
    // next spot down the east column (0x600102 (1000,562), 0x600017 (1000,558) ...); ours (994,568) and the column
    // one spot early.)
    if (State(o, US("Attached"))) return;
    // GatherUnmarkedUnitsInBox 0x721c00: the unit's collision AABB (Entity::UpdateAABox, from its collision
    // shape as it stands, turned and tilted) against the box, inclusive. A tank lying across reaches further in
    // x than its SizeX: the blueprint's sizes around the position are not it.
    WorldShape s;
    if (GetWorldShape(o, &s)) {
      Vec3 mn, mx;
      ShapeBounds(s, &mn, &mx);
      if (x0 > mx.x || mn.x > x1 || z0 > mx.z || mn.z > z1) return;
      // (the gather 0x721c00 takes its candidates from the entity grid first, the box's cells by 0x4fcbe0, y from
      // -1000 to 1000: a unit whose box only touches the square on a multiple of 4 is in other cells and is not
      // found. Seen: MP3 7778, army 0's engineer 0x3e looks for a spot beside a wall it is to build; the wall 0x18 at
      // (215.5, 319.5) reaches z 320, the edge of the candidate (215,320): the original takes that cell, ours called
      // it blocked and sent the engineer to (215,317), round the far side of the walls.)
      // The cells are the ones the grid has the unit in (its span as of the last AdvanceCoords, skirt-wide for a unit:
      // Unit::UpdateCollision), not its collision box's own. Seen (probe mp1_13985): MP1 13983, army 5's three T3
      // engineers (uel0309) look for spots beside a ueb1301 site; the energy storage 0x5000e5 (ueb1105 at 344.5, 846.5,
      // box x 344..345, skirt from 343.25) is registered from cell 85 and its box touches the candidate (343,846): the
      // original refuses that cell and gives 0x50011d (342,846), 0x500097 (341,846) and 0x50010a (346,846); ours, with
      // the box's cells (from 86), took (343,846) and handed the others the next ones.
      if (!GridFindsInBox(sim, o, x0, z0, x1, z1)) return;
    } else {
      const MotionBlueprint* ob = o->motion.bp;
      float hx = (ob ? ob->sizeX : 1) * 0.5f, hz = (ob ? ob->sizeZ : 1) * 0.5f;
      if (o->position.x + hx < x0 || o->position.x - hx > x1 || o->position.z + hz < z0 || o->position.z - hz > z1)
        return;
    }
    blocked = true;
  });
  return !blocked;
}

// One PrepareMove candidate (air_extra.md 3.1): bounds, terrain for the modified footprint fp'
// (S x S, Land (+Water), the blueprint's MaxSlope / MinWaterDepth, MaxWaterDepth 0), structures,
// other units' o-grid reservations, blocking units.
bool CanLandAt(Sim& sim, Unit* u, int cx, int cz, const Vec3& w, int S, int caps) {
  if (!IsWithin(sim, u, w, static_cast<float>(S))) return false;
  const AirBp& b = *A(u).bp;
  NamedFootprint fp;
  fp.sizeX = fp.sizeZ = static_cast<uint8_t>(S);
  fp.caps = static_cast<uint8_t>(caps);
  fp.maxSlope = b.fpMaxSlope;
  fp.minWaterDepth = b.fpMinWaterDepth;
  fp.maxWaterDepth = 0;
  fp.flags = b.fpFlags;
  char key[96];
  std::snprintf(key, sizeof key, "__air_land_%d_%d_%a_%a", S, caps, fp.maxSlope, fp.minWaterDepth);
  fp.name = key;
  int c = sim.navigation().CellCaps(fp, cx, cz);  // (Grid(fp)->Caps without making the grid)
  if (u->layer == "Water") c &= ~kSub;
  c = FitsCaps(static_cast<uint8_t>(c), (fp.flags & 1) != 0, sim.navigation().AnyStructureIn(cx, cz, cx + S, cz + S, kOccLand),
               sim.navigation().AnyStructureIn(cx, cz, cx + S, cz + S, kOccWater));
  if (!c) return false;
  int r[4] = {cx, cz, cx + S, cz + S};
  if (!CanReserve(sim, u, r)) return false;
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  return NoBlockingUnits(sim, u, x0, z0, x0 + S, z0 + S);
}

// Unit::PrepareMove 0x62b780 for a flyer: the squared footprint, the exclusion rect (closed test on the candidate's
// 1x1 cell, 0x62b98c..0x62b9e7: it follows the CanFly branch, so a flyer is tested against it as a ground unit is),
// the ring step `spacing` or 2 x the footprint size (0x62b7c6..0x62b7fd)
namespace {
bool Excluded(const float* e, int cx, int cz) {
  return e && e[2] > e[0] && e[3] > e[1] && static_cast<float>(cx) <= e[2] && e[0] <= static_cast<float>(cx + 1) &&
         static_cast<float>(cz) <= e[3] && e[1] <= static_cast<float>(cz + 1);
}
}  // namespace
bool PrepareMove(Sim& sim, Unit* u, Vec3* pos, const float* excl = nullptr, int spacing = 0) {
  const AirBp& b = *A(u).bp;
  int S = std::max(b.footprintX, b.footprintZ);
  int caps = kLand;
  const TerrainMap* map = sim.map();
  if (InCategory(sim, u, "CANLANDONWATER") && Elevation(map, pos->x, pos->z) < WaterOrNone(map)) caps |= kWater;
  int cx = static_cast<int>(std::nearbyint(pos->x - S * 0.5f)), cz = static_cast<int>(std::nearbyint(pos->z - S * 0.5f));
  if (!Excluded(excl, cx, cz) && CanLandAt(sim, u, cx, cz, *pos, S, caps)) return true;
  int step = spacing > 0 ? spacing : 2 * S;
  int tested = 0;
  for (int r = 1;; ++r) {
    bool found = false;
    Vec3 best;
    float bestD = kInf;
    for (int i = -r; i <= r; ++i) {
      int jStep = (i == -r || i == r) ? 1 : 2 * r;
      for (int j = -r; j <= r; j += jStep) {
        ++tested;
        int x = cx + step * i, z = cz + step * j;
        Vec3 w{x + S * 0.5f, 0.0f, z + S * 0.5f};
        if (!Excluded(excl, x, z) && CanLandAt(sim, u, x, z, w, S, caps)) {
          float dx = w.x - u->position.x, dy = w.y - u->position.y, dz = w.z - u->position.z;
          float d = (dx * dx + dy * dy) + dz * dz;
          if (d < bestD) {
            bestD = d;
            best = w;
            found = true;
          }
        }
      }
    }
    if (found) {
      *pos = best;
      return true;
    }
    if (tested > 899) return false;
  }
}

// Unit::PrepareMove 0x62b780 for a ground unit: its own footprint (not squared), the exclusion
// rect tested against the candidate's 1x1 cell, the unit's own o-grid reservation (Unit::ogridRect).
namespace {
bool GroundCanReserve(Sim& sim, Unit* u, const int* r) {
  bool own = !RectEmpty(u->ogridRect);
  if (own) FillRes(sim, u->ogridRect, 0);
  bool blocked = AnyRes(sim, r);
  if (own) FillRes(sim, u->ogridRect, 1);
  return !blocked;
}
// MOHO64_DEBUG_PREPMOVE=<unit id> (or -2: every unit): every spot tested and why it was refused
bool PrepDbgOn(const Unit* u) {
  static const long v = getenv("MOHO64_DEBUG_PREPMOVE") ? atol(getenv("MOHO64_DEBUG_PREPMOVE")) : -1;
  return v == -2 || v == static_cast<long>(u->id);
}
bool GroundSpotOkImpl(Sim& sim, Unit* u, const NamedFootprint& fp, int cx, int cz, const Vec3& w, const float* excl,
                      const char** why) {
  if (excl && excl[2] > excl[0] && excl[3] > excl[1] && cx <= excl[2] && excl[0] <= cx + 1 && cz <= excl[3] &&
      excl[1] <= cz + 1) {  // closed: a cell touching the rect is out
    *why = "excluded";
    return false;
  }
  int S = std::max(fp.sizeX, fp.sizeZ);
  if (!IsWithin(sim, u, w, static_cast<float>(S))) {
    *why = "outside";
    return false;
  }
  int c = sim.navigation().CellCaps(fp, cx, cz);  // (Grid(fp)->Caps without making the grid)
  if (u->layer == "Water") c &= ~kSub;
  c = sim.navigation().FootprintFits(static_cast<uint8_t>(c), fp, cx, cz);
  if (!c) {
    *why = "caps";
    return false;
  }
  int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
  if (!GroundCanReserve(sim, u, r)) {
    *why = "reserved";
    return false;
  }
  // (COORDS_CanMoveAt 0x720f70 squares the footprint: x0 + max(SizeX, SizeZ) on both axes, 0x721016..0x721076)
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  if (!NoBlockingUnits(sim, u, x0, z0, x0 + static_cast<float>(S), z0 + static_cast<float>(S))) {
    *why = "unit";
    return false;
  }
  *why = "ok";
  return true;
}
bool GroundSpotOk(Sim& sim, Unit* u, const NamedFootprint& fp, int cx, int cz, const Vec3& w, const float* excl) {
  const char* why = "";
  const bool ok = GroundSpotOkImpl(sim, u, fp, cx, cz, w, excl, &why);
  if (PrepDbgOn(u))
    Logf(LogLevel::Info, "prepmove %u unit %u spot (%d,%d) %s", sim.tick(), u->id, cx, cz, why);
  return ok;
}
}  // namespace

bool GroundPrepareMoveImpl(Sim& sim, Unit* u, Vec3* pos, const float excl[4], int spacing) {
  const NamedFootprint& fp = UnitFootprint(u);  // (Unit::PrepareMove 0x62b7c1: Entity::GetFootprint)
  int cx = static_cast<int>(std::nearbyint(pos->x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(pos->z - fp.sizeZ * 0.5f));
  if (PrepDbgOn(u))
    Logf(LogLevel::Info, "prepmove %u unit %u from %.3f %.3f at %.3f %.3f excl %.2f %.2f %.2f %.2f spacing %d", sim.tick(),
         u->id, pos->x, pos->z, u->position.x, u->position.z, excl ? excl[0] : 0.0f, excl ? excl[1] : 0.0f,
         excl ? excl[2] : 0.0f, excl ? excl[3] : 0.0f, spacing);
  if (GroundSpotOk(sim, u, fp, cx, cz, *pos, excl)) return true;
  int step = spacing > 0 ? spacing : 2 * std::max(fp.sizeX, fp.sizeZ);
  int tested = 0;
  for (int r = 1;; ++r) {
    bool found = false;
    Vec3 best;
    float bestD = kInf;
    for (int i = -r; i <= r; ++i) {
      int jStep = (i == -r || i == r) ? 1 : 2 * r;
      for (int j = -r; j <= r; j += jStep) {
        ++tested;
        int x = cx + step * i, z = cz + step * j;
        Vec3 w{x + fp.sizeX * 0.5f, 0.0f, z + fp.sizeZ * 0.5f};
        if (GroundSpotOk(sim, u, fp, x, z, w, excl)) {
          float dx = w.x - u->position.x, dy = w.y - u->position.y, dz = w.z - u->position.z;
          float d = (dx * dx + dy * dy) + dz * dz;
          if (d < bestD) {
            bestD = d;
            best = w;
            found = true;
          }
        }
      }
    }
    if (found) {
      *pos = best;
      return true;
    }
    if (tested > 899) return false;
  }
}
void GroundFreeRectImpl(Sim& sim, Unit* u) {
  if (!RectEmpty(u->ogridRect)) FillRes(sim, u->ogridRect, 0);
  std::fill(u->ogridRect, u->ogridRect + 4, 0);
}
void GroundReserveRectImpl(Sim& sim, Unit* u, const int r[4]) {
  GroundFreeRectImpl(sim, u);
  std::copy(r, r + 4, u->ogridRect);
  FillRes(sim, r, 1);
}

// CUnitMotion::SetTarget 0x6b85e0 (layer 0 keeps the landing layer)
void SetTarget(Sim& sim, Unit* u, Vec3 p, const Vec3& dir, int layer) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  a.stopped = false;
  Vec3 orig = p;
  if (const TerrainMap* map = sim.map()) {
    // 0x6b8603: into the map (0 .. the heightfield's size - 1) when the army uses the whole map, else into the
    // playable rect; each axis max(rect min, p) then min(rect max, that) as the SSE compares take them
    int r[4] = {0, 0, map->width(), map->height()};
    if (!Sim::UseWholeMap(u->army)) std::copy(sim.playableRect, sim.playableRect + 4, r);
    float v = p.x > static_cast<float>(r[0]) ? p.x : static_cast<float>(r[0]);
    p.x = static_cast<float>(r[2]) > v ? v : static_cast<float>(r[2]);
    v = p.z > static_cast<float>(r[1]) ? p.z : static_cast<float>(r[1]);
    p.z = static_cast<float>(r[3]) > v ? v : static_cast<float>(r[3]);
  }
  a.target = p;
  if (layer != 0) a.landLayer = layer;
  if (u->commands.empty()) a.idleTick = sim.tick();
  SetState(u, US("MovingUp"), false);
  SetState(u, US("MovingDown"), false);
  Vec3 d = dir;
  if (!IsZeroBits(d) && NormalizeInPlace(d) != 0.0) {
    a.facing = d;
  } else {
    float hx = orig.x - u->position.x, hz = orig.z - u->position.z;
    float dist = std::sqrt(hx * hx + hz * hz);
    if (dist > b.startTurnDistance) {  // 0x6b881c: times 1 / distance (a division differs in the last bit)
      const float inv = 1.0f / dist;
      a.facing = {inv * hx, inv * 0.0f, hz * inv};
    }
  }
  {  // MOHO64_DEBUG_SETTARGET=<unit id>: every target set, the facing it leaves, and who set it (image offsets)
    static const long dbgUnit = getenv("MOHO64_DEBUG_SETTARGET") ? atol(getenv("MOHO64_DEBUG_SETTARGET")) : -1;
    if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit) {
      void* fr[6];
      const int n = CaptureFrames(fr, 6);
      std::string s;
      for (int i = 1; i < n; ++i) {
        char buf[32];
        snprintf(buf, sizeof buf, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
        s += buf;
      }
      Logf(LogLevel::Info, "settarget %u unit %u to %.3f %.3f dir %.4f %.4f facing %.4f %.4f from%s", sim.tick(), u->id, p.x,
           p.z, dir.x, dir.z, a.facing.x, a.facing.z, s.c_str());
    }
  }
  FreeLandRect(sim, u);
}

// CUnitMotion::Stop 0x6b8460
void Stop(Sim& sim, Unit* u, const Vec3* pos) {
  AirMotion& a = A(u);
  a.stopped = true;
  {
    static const long dbgUnit = getenv("MOHO64_DEBUG_AIRSTOP") ? atol(getenv("MOHO64_DEBUG_AIRSTOP")) : -1;
    if (dbgUnit >= 0 && static_cast<long>(u->id) == dbgUnit) {  // who stops it (image offsets for llvm-symbolizer)
      void* fr[10];
      const int n = CaptureFrames(fr, 10);
      std::string s;
      for (int i = 1; i < n; ++i) {
        char b[32];
        snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
        s += b;
      }
      Logf(LogLevel::Info, "airstop %u unit %u from%s", sim.tick(), u->id, s.c_str());
    }
  }
  // 0x6b846f: only a unit in category AIR (u+0x68b, set from the blueprint in the Unit ctor 0x6a6aae) is told to stay
  // airborne. A flyer without it keeps its landing layer: seen, bench's Cybran build drones (ura0001: POD, no AIR)
  // record landing layer 0 after the IssueClearCommands that follows their CreateUnit(.., 'Air').
  if (a.bp->airCategory && !State(u, US("TransportUnloading")) && !State(u, US("TransportLoading")) && !State(u, US("Ferrying")))
    a.landLayer = kAir;
  a.target = pos ? *pos : u->position;
  (void)sim;
}

// ---------------------------------------------------------------------------------------------
// Orientation and control

struct Ctrl {
  Vec3 force, torque;
};

// MOHO64_DEBUG_BODY=<unit id>: the control's and the rotation step's values as bits. Set for the unit being stepped
// and read by its callees: per thread (multicore step 1), as each worker steps its own unit.
thread_local bool g_bodyDebug = false;
uint32_t FBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
void BodyDebugVec(const char* name, const float* p, int n) {
  char buf[160];
  int o = std::snprintf(buf, sizeof buf, "ctl %s", name);
  for (int i = 0; i < n && o < static_cast<int>(sizeof buf) - 10; ++i) o += std::snprintf(buf + o, sizeof buf - o, " %08x", FBits(p[i]));
  Logf(LogLevel::Info, "%s", buf);
}

// CUnitMotion::CalcWingedLift 0x6bc950
float WingedLift(const AirMotion& a, float unitY, float desiredY, float upY) {
  float base = GetElevationWanted(a, unitY);
  float y = (upY - 0.5f) * a.bp->liftFactor;
  if (y > 0.0f) return (desiredY > y) ? y : desiredY;
  float half = base * 0.5f;
  if (half > a.height) return half - a.height;
  return y;
}

// CUnitMotion::CalcWingedOrientation 0x6bd7b0
void WingedOrientation(Unit* u, const QuatW& tq, const Vec3& desired, const Vec3& desiredDir, const Vec3& facing,
                       const Vec3& f, Axes& axes, Vec3& dv, float& kTurn) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  Vec3 right = MultQuadVec(Q90(), f);
  float upY = 1.0f - (tq.z * tq.z + tq.x * tq.x) * 2.0f;
  float hlen = std::sqrt(desired.z * desired.z + desired.x * desired.x);
  float ms = MaxSpeed(u);
  float speed = (hlen > ms) ? ms : hlen;
  bool close = b.startTurnDistance > speed;
  bool guard = State(u, US("Guarding"));
  int cs = a.combatState;
  Vec3 dir;
  if (close && !guard && cs == 0) {
    dir = facing;
  } else {
    dir = desiredDir;
    dv = {f.x * speed, f.y * speed, f.z * speed};
    if (cs == 0 || cs == 1 || cs == 2) {
      float dot = (dir.y * f.y + dir.z * f.z) + dir.x * f.x;
      float fac = (dot > 0.5f) ? dot : 0.5f;
      if (guard && close) {
        float r = speed / b.startTurnDistance;
        float r2 = (r > 0.5f) ? r : 0.5f;
        fac = (r2 > fac) ? fac : r2;
      }
      dv = {dv.x * fac, dv.y * fac, dv.z * fac};
    }
    dv.y = WingedLift(a, u->position.y, desired.y, upY);
  }
  float h = std::sqrt(dir.z * dir.z + dir.x * dir.x);
  Vec3 aim{};
  if (h > 0.0f) {
    float inv = 1.0f / h;
    aim = {inv * dir.x, inv * 0.0f, dir.z * inv};
  }
  float side = (right.z * dir.z + right.y * dir.y) + right.x * dir.x;
  float sign = (0.0f > side) ? -1.0f : 1.0f;
  float a1 = dmath::Atan2(aim.x, aim.z);
  float diff = static_cast<float>(static_cast<double>(a1) - dmath::Atan2d(f.x, f.z));  // fpatan wide, fsub at PC_24
  float d;
  if (diff > 3.14159274f) {
    d = diff - 6.28318548f;
  } else {
    d = diff;
    if (-3.14159274f > d) d = d + 6.28318548f;
  }
  float maxStep = ((cs == 3) ? b.combatTurnSpeed : b.turnSpeed) * 0.1f;
  float step = (maxStep > d) ? d : maxStep;
  if (-maxStep > step) step = -maxStep;
  float half = (step * 10.0f) * 0.5f;
  float s = dmath::Sin(half), c = dmath::Cos(half);
  QuatW rot{c, s * 0.0f, s, s * 0.0f};
  Vec3 t = MultQuadVec(rot, f);
  Vec3 nf{t.x, dir.y, t.z};
  NormalizeInPlace(nf);
  float bankScale = speed / b.startTurnDistance;
  float cap = close ? 0.5f : 1.0f;
  if (bankScale > cap) bankScale = cap;
  float d2 = (aim.y * f.y + aim.z * f.z) + aim.x * f.x;
  float bf = b.bankFactor;
  if (cs == 2) {
    bf = b.bankFactor * 10.0f;
    float p = d2 * d2;
    p = p * p;
    d2 = p * p;
  }
  float bankFrac = 1.0f - ((d2 > 0.0f) ? d2 : 0.0f);
  float altF = State(u, US("MovingDown")) ? a.height / b.elevation : 1.0f;
  float bankAmt = (((altF * bankFrac) * bf) * bankScale) * sign;
  float k = ((nf.z + nf.x) * 0.0f + nf.y) * bankScale;
  Vec3 proj{aim.x * k, aim.y * k, aim.z * k};
  Vec3 r2v = MultQuadVec(Q90(), nf);
  Vec3 up;
  up.y = (r2v.y * bankAmt + 1.0f) - proj.y;
  up.z = r2v.z * bankAmt - proj.z;
  up.x = bankAmt * r2v.x - proj.x;
  float n = std::sqrt((up.z * up.z + up.y * up.y) + up.x * up.x);
  if (n > 0.0f) {
    float inv = 1.0f / n;
    up = {inv * up.x, up.y * inv, up.z * inv};
  } else {
    up = {};
  }
  axes.Y = up;
  axes.Z = nf;
  if (cs == 3) kTurn = b.tightTurnMultiplier * bankFrac + kTurn;
  else if (cs == 1 || cs == 2) kTurn = kTurn + bankFrac;
}

// CUnitMotion::CalcHoverOrientation 0x6be480
void HoverOrientation(Unit* u, const Vec3& facing, Axes& axes) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  const PhysBody& pb = a.body;
  Vec3 acc{pb.v.x - a.prevVel.x, pb.v.y - a.prevVel.y, pb.v.z - a.prevVel.z};
  if (!b.bankForward) {
    const QuatW& q = pb.q;
    Vec3 F{(q.w * q.y + q.z * q.x) * 2.0f, (q.z * q.y - q.w * q.x) * 2.0f, 1.0f - (q.y * q.y + q.x * q.x) * 2.0f};
    float n2 = (F.z * F.z + F.y * F.y) + F.x * F.x;
    if (n2 > 0.0f) {
      float t = ((F.z * acc.z + F.y * acc.y) + F.x * acc.x) / n2;
      acc = {acc.x - t * F.x, acc.y - F.y * t, acc.z - F.z * t};
    }
  }
  float h = a.height / b.elevation;
  if (h > 1.0f) h = 1.0f;
  float kb = b.bankFactor * h;
  axes.Y = {acc.x * kb - 0.0f * 0.1f, acc.y * kb - kGravityY * 0.1f, acc.z * kb - 0.0f * 0.1f};
  axes.Z = facing;
}

// RandRange of the circling parameters: lo + r * (hi - lo) * 2^-32 on the x87 (precision control 24)
float RandRange(Sim& sim, float lo, float hi) {
  uint32_t r = sim.NextUInt32();
  return dmath::MulU32(r, hi - lo) * 2.3283064365386963e-10f + lo;
}

// CUnitMotion::CalcCirclingOrientation 0x6bdee0
void CirclingOrientation(Sim& sim, Unit* u, Axes& axes, Vec3& dv, const AiTarget& target) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  Vec3 pos = u->position;
  uint32_t tick = sim.tick();
  if (tick > a.combatTimer) {
    if (b.circlingDirChange) {
      uint32_t r = sim.NextUInt32();
      a.circleDir = static_cast<int>((static_cast<uint64_t>(r) * 100) >> 32) > 50;
    }
    float e = b.circlingElevationChangeRatio * b.attackElevation;
    a.circleElev = RandRange(sim, -0.0f - e, e);
    a.circleRatio = RandRange(sim, b.circlingRadiusChangeMinRatio, b.circlingRadiusChangeMaxRatio);
    int hiT = EngineFloor((b.circlingFlightChangeFrequency * 2.0f) * 10.0f);
    int loT = EngineFloor(b.circlingFlightChangeFrequency * 10.0f);
    uint32_t r = sim.NextUInt32();
    a.combatTimer = static_cast<uint32_t>((static_cast<uint64_t>(r) * static_cast<uint32_t>(hiT - loT)) >> 32) +
                    static_cast<uint32_t>(loT) + tick;  }
  float cx = a.target.x, cz = a.target.z;
  float R = b.startTurnDistance * a.circleRatio;
  Entity* focus = u->focusId ? sim.FindEntity(u->focusId) : nullptr;
  if (focus && (State(u, US("Building")) || State(u, US("Repairing")))) {
    const Vec3& fp = focus->shapeHeld ? focus->shapePos : focus->position;  // as the beat began (see TargetPosNow)
    cx = fp.x;
    cz = fp.z;
  } else if (combat::HasTarget(sim, target)) {
    Vec3 tp = TargetPosNow(sim, target);
    cx = tp.x;
    cz = tp.z;
    UnitWeapon* w = nullptr;
    for (UnitWeapon* x : u->weapons)
      if (x->bp && WeaponCanAttackTarget(sim, x, target)) {
        w = x;
        break;
      }
    if (w) {
      float range = w->ovMaxRadius >= 0 ? w->ovMaxRadius : w->bp->maxRadius;
      R = range * a.circleRatio;
      Entity* te = combat::TargetEntity(sim, target);
      if (te && te->kind == Entity::Kind::Unit && static_cast<Unit*>(te)->layer == "Air")
        R = b.circlingRadiusVsAirMult * R;
    }
  }
  Vec3 toC{cx - pos.x, 0, cz - pos.z};
  if (9.99999997e-07f > (toC.z * toC.z + toC.x * toC.x)) toC = {1, 0, 0};
  NormalizeInPlace(toC);
  Vec3 t = MultQuadVec(a.circleDir ? Qm90() : Q90(), toC);
  float ms = b.minAirspeed;
  Vec3 off{(ms * t.x + pos.x) - cx, (t.y * ms + pos.y) - pos.y, (t.z * ms + pos.z) - cz};
  SetLength(off, R);
  Vec3 goal{off.x + cx, off.y + pos.y, off.z + cz};
  int ix = static_cast<int>(std::nearbyint(goal.x)), iz = static_cast<int>(std::nearbyint(goal.z));
  float hgt = SampleAt(sim.map(), ix, iz, b.flyInWater);
  dv = {goal.x - pos.x, ((b.attackElevation + a.circleElev) + hgt) - pos.y, goal.z - pos.z};
  float len = std::sqrt((dv.z * dv.z + dv.y * dv.y) + dv.x * dv.x);
  SetLength(dv, (b.maxAirspeed > len) ? len : b.maxAirspeed);
  HoverOrientation(u, toC, axes);
}

// CUnitMotion::CalcAirMovementDampingFactor 0x6bca10
void RecordAir(Sim& sim, Unit* u, const Vec3& d) {  // labhook 0.6 'A' record (CalcAirMovementDampingFactor entry)
  const AirMotion& a = A(u);
  unsigned char r[56];
  auto put = [&](int o, uint32_t v) { r[o] = v & 0xff; r[o + 1] = (v >> 8) & 0xff; r[o + 2] = (v >> 16) & 0xff; r[o + 3] = v >> 24; };
  auto putf = [&](int o, float f) { uint32_t v; std::memcpy(&v, &f, 4); put(o, v); };
  r[0] = 'A';
  r[1] = a.fullSpeed ? 1 : 0;
  r[2] = r[3] = 0;
  put(4, sim.RecorderTick());
  put(8, u->id);
  putf(12, d.x); putf(16, d.y); putf(20, d.z);
  putf(24, MaxSpeed(u));
  putf(28, a.target.x); putf(32, a.target.y); putf(36, a.target.z);
  putf(40, a.curTerrain); putf(44, a.elevOffset); putf(48, a.height);
  r[52] = static_cast<unsigned char>(a.landLayer);
  r[53] = static_cast<unsigned char>(a.vertEvent);
  r[54] = static_cast<unsigned char>(a.horzEvent);
  r[55] = static_cast<unsigned char>(a.carrierEvent);
  sim.RecorderRaw(r, 56);
}

float DampingFactor(Sim& sim, Unit* u, const Vec3& d) {
  if (sim.Recording()) RecordAir(sim, u, d);
  const AirBp& b = *A(u).bp;
  if (b.targetChaser) return 1.0f;
  float ms = MaxSpeed(u);
  float len = std::sqrt((d.x * d.x + d.y * d.y) + d.z * d.z);
  float c = (len > ms) ? ms : len;
  c = (c > 1.0f) ? c : 1.0f;
  if (ms > c) {
    float r = ms / c;
    return (r > b.kMoveDamping) ? b.kMoveDamping : r;
  }
  return b.kMove;
}

// CUnitMotion::ComputeAirControl 0x6be6b0
Ctrl ComputeAirControl(Sim& sim, Unit* u, const QuatW& tq, const Vec3& desired, const Vec3& desiredDir,
                       const Vec3& facing, const Vec3& fwdH, const AiTarget& target) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  PhysBody& pb = a.body;
  // 1 / Unit::CalcTransportLoadFactor (vt+0x24, 0x6a8b30). The FAF exe patches that function to "fld1; ret", so
  // the factor is 1 whatever the cargo (docs/deviations.md)
  float invL = 1.0f / 1.0f;
  float kTurn = b.kTurn * invL, kRoll = b.kRoll * invL, kLift = b.kLift * invL;
  Vec3 dv = desired;
  Axes axes;
  int mode;  // 0 winged, 1 circling, 2 hover
  if (b.winged && a.carrierEvent != 2) {
    mode = 0;
  } else if (State(u, US("Guarding")) && !(State(u, US("Moving")) || State(u, US("Ferrying")) || State(u, US("Attacking")) ||
                                      State(u, US("Building")) || State(u, US("Repairing")))) {
    mode = 0;
  } else if (b.hoverOverAttack) {
    mode = 2;
  } else if (u->focusId && sim.FindEntity(u->focusId) &&
             (State(u, US("Building")) || State(u, US("Repairing")) || State(u, US("Reclaiming")) || State(u, US("Capturing")))) {
    mode = 1;
  } else if (combat::HasTarget(sim, target)) {
    mode = 1;
  } else {
    mode = 2;
  }
  if (mode == 0) {
    WingedOrientation(u, tq, desired, desiredDir, facing, fwdH, axes, dv, kTurn);
  } else if (mode == 1) {
    CirclingOrientation(sim, u, axes, dv, target);
    kTurn = b.circlingTurnMult * kTurn;
  } else {
    HoverOrientation(u, facing, axes);
    a.combatTimer = 0;
  }
  if (g_bodyDebug) BodyDebugVec("rawaxes", &axes.X.x, 9);
  OrthoNormalize(axes);
  QuatW Qd = AxesToQuat(axes);
  const QuatW q = pb.q;
  float cx = -0.0f - q.x, cy = -0.0f - q.y, cz = -0.0f - q.z;
  QuatW P;
  P.w = ((Qd.w * q.w - Qd.x * cx) - Qd.y * cy) - Qd.z * cz;
  P.x = ((cx * Qd.w + Qd.x * q.w) + Qd.z * cy) - Qd.y * cz;
  P.y = ((cy * Qd.w + Qd.y * q.w) + Qd.x * cz) - Qd.z * cx;
  P.z = ((cz * Qd.w + Qd.z * q.w) + Qd.y * cx) - Qd.x * cy;
  if (0.0f > P.w) {
    P.w = -0.0f - P.w;
    P.x = -0.0f - P.x;
    P.y = -0.0f - P.y;
    P.z = -0.0f - P.z;
  }
  Vec3 nv{-0.0f - pb.v.x, -0.0f - pb.v.y, -0.0f - pb.v.z};
  Vec3 axis;
  float angle;
  QuatToAxisAngle(P, &axis, &angle);
  Vec3 err{axis.x * angle, axis.y * angle, axis.z * angle};
  Vec3 wb = MultQuadVec(QuatW{q.w, cx, cy, cz}, pb.L);
  Vec3 H{-0.0f - pb.invI[0] * wb.x, -0.0f - pb.invI[1] * wb.y, -0.0f - pb.invI[2] * wb.z};
  float dampF = DampingFactor(sim, u, desired);
  Vec3 F;
  F.x = dv.x * b.kMove + nv.x * dampF;
  F.y = b.kLiftDamping * nv.y + dv.y * kLift;
  F.z = dv.z * b.kMove + nv.z * dampF;
  if (a.carrierEvent == 2) {  // 0x6beb89: landing on a carrier, within 40 of the focus entity
    if (const Entity* fe = u->focusId ? sim.FindEntity(u->focusId) : nullptr) {
      // both COMMITTED: the unit's from vt+0x14 (IUnit::GetPosition 0x6a49b0, unit+0xb4), the carrier's at
      // entity+0xac / +0xb4 (0x6bebc7..0x6bebdf), so a carrier that has already moved in this beat counts where the
      // beat began with it
      const Vec3& up = u->shapeHeld ? u->shapePos : u->position;
      const Vec3& fp = fe->shapeHeld ? fe->shapePos : fe->position;
      float dz = up.z - fp.z, dx = up.x - fp.x;
      if (std::sqrt(dz * dz + dx * dx) < 40.0f) {
        F.x = (nv.x * 2.0f + dv.x) * b.kMove;
        F.y = (nv.y * 1.25f + dv.y) * kLift;
        F.z = (nv.z * 2.0f + dv.z) * b.kMove;
      }
    }
  }
  Vec3 T;
  T.x = H.x * b.kTurnDamping + err.x * kTurn;
  T.y = H.y * b.kTurnDamping + err.y * kTurn;
  T.z = b.kRollDamping * H.z + err.z * kRoll;
  {
    static const long dbgId = getenv("MOHO64_DEBUG_AIR") ? atol(getenv("MOHO64_DEBUG_AIR")) : -1;
    if (dbgId >= 0 && static_cast<long>(u->id) == dbgId)
      Logf(LogLevel::Info, "airdbg %u mode %d pos %.4f %.4f %.4f v %.4f %.4f %.4f d %.4f %.4f %.4f dv %.4f %.4f %.4f "
           "dampF %.4f F %.4f %.4f %.4f tgt %.3f %.3f %.3f landL %d vert %d elev %.3f h %.3f cev %d",
           sim.tick(), mode, u->position.x, u->position.y, u->position.z, pb.v.x, pb.v.y, pb.v.z, desired.x,
           desired.y, desired.z, dv.x, dv.y, dv.z, dampF, F.x, F.y, F.z, a.target.x, a.target.y, a.target.z,
           a.landLayer, a.vertEvent, a.elevOffset, a.height, a.carrierEvent);
  }
  Ctrl c;
  float m = pb.mass;
  c.force = {(F.x - 0.0f) * m, (F.y - kGravityY) * m, (F.z - 0.0f) * m};
  c.torque = MultQuadVec(q, Vec3{T.x / pb.invI[0], T.y / pb.invI[1], T.z / pb.invI[2]});
  a.ctrlForce = c.force;  // m+0xfc (0x6bedf4..0x6bee10)
  a.torque108 = T;        // m+0x108 (0x6bee1e..0x6bee3c)
  a.spinFromDeath = false;
  if (g_bodyDebug) {
    const float k[5] = {kTurn, kRoll, b.kTurnDamping, b.kRollDamping, static_cast<float>(mode)};
    BodyDebugVec("k", k, 5);
    BodyDebugVec("v", &pb.v.x, 3);
    BodyDebugVec("prevVel", &a.prevVel.x, 3);
    BodyDebugVec("facing", &facing.x, 3);
    BodyDebugVec("axes", &axes.X.x, 9);
    BodyDebugVec("Qd", &Qd.w, 4);
    BodyDebugVec("q", &q.w, 4);
    BodyDebugVec("P", &P.w, 4);
    BodyDebugVec("axis", &axis.x, 3);
    BodyDebugVec("angle", &angle, 1);
    BodyDebugVec("L", &pb.L.x, 3);
    BodyDebugVec("wb", &wb.x, 3);
    BodyDebugVec("H", &H.x, 3);
    BodyDebugVec("T", &T.x, 3);
    BodyDebugVec("torque", &c.torque.x, 3);
  }
  return c;
}

// CUnitMotion::CalcDesiredTargetElevation 0x6bcb90
float DesiredTargetElevation(Sim& sim, Unit* u, const AiTarget& tgt, const Vec3& v) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  Vec3 P{u->position.x + v.x, u->position.y + v.y, u->position.z + v.z};
  int ix = static_cast<int>(std::nearbyint(P.x)), iz = static_cast<int>(std::nearbyint(P.z));
  float h = SampleAt(sim.map(), ix, iz, b.flyInWater);
  Entity* E = combat::TargetEntity(sim, tgt);
  if (E && E->kind == Entity::Kind::Unit && static_cast<Unit*>(E)->layer == "Air") {
    Unit* tu = static_cast<Unit*>(E);
    // 0x6bcc99..0x6bccf2: the flying height of the target's blueprint (bp+0x2dc) above the ground there is asked of
    // the entity the CAiTarget itself holds (+0x4, Entity vt+0x10 = IsUnit). Held through a recon blip, that entity
    // is the blip, no unit: the target's point (GetTargetPosGun 0x5e2a90, flag clear) gives the height instead.
    // Seen: MP3 2624, army 0's interceptor 0x2a ordered onto the radar blip of army 1's bomber: the original's
    // attack-run velocity points at the bomber's true height (y -0.112 of 10), ours at the ground + 14 (y -0.807).
    const bool unitHeld = tu->motion.air && !(tgt.viaBlip || tgt.fakeId);
    float tgtY = unitHeld ? h + tu->motion.air->bp->elevation : TargetPosNow(sim, tgt).y;
    float want = b.elevation * 0.5f + h;
    return std::max(want, tgtY) - u->position.y;
  }
  if (a.combatState == 1) return (h + b.attackElevation) - u->position.y;
  return (h + b.elevation) - u->position.y;
}

// "position != previous position" of ANOTHER entity (entity+0xac against +0xc8): both are the transforms the
// beat began with, whether or not that entity has already taken its step in this beat
bool MovedThisTick(const Entity* e) {
  if (e->kind != Entity::Kind::Unit) return false;
  const Unit* t = static_cast<const Unit*>(e);
  return !IsZeroBits(t->motion.velCommitted);
}

// CUnitMotion::ComputeAirCombatTactics 0x6bcdb0
void CombatTactics(Sim& sim, Unit* u, const AiTarget& tgt, Vec3& out, const Vec3& fwd) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  uint32_t now = sim.tick();
  float speed = u->motion.speedMult * b.maxAirspeed;
  Vec3 pos = u->position;
  Vec3 tp = TargetPosNow(sim, tgt);
  Vec3 diff{tp.x - pos.x, tp.y - pos.y, tp.z - pos.z};
  // the squares are summed (x^2 + z^2) + y^2 here, for the distance and again for the direction (0x6bce66,
  // 0x6bcf02: inline code, not the vector helpers); the direction is the difference times 1 / length. Summed in
  // another order the direction times the distance misses the difference by a last bit or two, which is a last
  // bit in the desired velocity and, beats later, the other side of a comparison
  float d3 = std::sqrt((diff.x * diff.x + diff.z * diff.z) + diff.y * diff.y);
  float d2 = std::sqrt(diff.z * diff.z + diff.x * diff.x);
  Vec3 dir3{};
  if (d3 > 0.0f) {
    const float inv = 1.0f / d3;
    dir3 = {inv * diff.x, diff.y * inv, diff.z * inv};
  }
  float trig = u->motion.breakOffTriggerMult * b.breakOffTrigger;
  float brk = u->motion.breakOffDistanceMult * b.breakOffDistance;
  int s = a.combatState;
  Entity* E = combat::TargetEntity(sim, tgt);
  bool eAir = E && E->kind == Entity::Kind::Unit && static_cast<Unit*>(E)->layer == "Air";
  auto output = [&]() {
    a.combatState = s;
    auto elev = [&](const Vec3& v) { return DesiredTargetElevation(sim, u, tgt, v); };
    switch (s) {
      case 1:
      case 2: {
        SetState(u, US("MakingAttackRun"), true);
        if (E && tgt.mobile) {
          float t = 1.0f;
          if (b.predictAheadForBombDrop > 0 && !eAir) t = b.predictAheadForBombDrop;
          Vec3 p = E->kind == Entity::Kind::Unit ? PredictAhead(sim, static_cast<Unit*>(E), t, true) : Vec3{};
          out = {p.x - pos.x, p.y - pos.y, p.z - pos.z};
        } else {
          out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        }
        const Vec3 raw = out;
        out.y = elev(out);
        const Vec3 lifted = out;
        if (s == 2 && eAir && MovedThisTick(E)) SetLength(out, std::max(d3, b.minAirspeed));
        else SetLength(out, speed);
        {
          static const long dbgId = getenv("MOHO64_DEBUG_TACTICS") ? atol(getenv("MOHO64_DEBUG_TACTICS")) : -1;
          if (dbgId >= 0 && static_cast<long>(u->id) == dbgId && E && E->kind == Entity::Kind::Unit) {
            const Unit* tu = static_cast<const Unit*>(E);
            const PhysBody* tb = tu->motion.air ? &tu->motion.air->body : nullptr;
            Logf(LogLevel::Info, "tactics %u out: state %d raw %.9g %.9g %.9g y %.9g len %.9g -> %.9g %.9g %.9g | target held "
                 "%d pos %.9g %.9g %.9g held %.9g %.9g %.9g vel now %.9g %.9g %.9g committed %.9g %.9g %.9g q %.9g %.9g "
                 "%.9g %.9g L %.9g %.9g %.9g invI %.9g %.9g %.9g",
                 now, s, raw.x, raw.y, raw.z, lifted.y, (s == 2 && eAir && MovedThisTick(E)) ? std::max(d3, b.minAirspeed) : speed,
                 out.x, out.y, out.z, static_cast<int>(tu->shapeHeld), tu->position.x, tu->position.y, tu->position.z,
                 tu->shapePos.x, tu->shapePos.y, tu->shapePos.z, tu->motion.lastMove.x, tu->motion.lastMove.y,
                 tu->motion.lastMove.z, tu->motion.velCommitted.x, tu->motion.velCommitted.y, tu->motion.velCommitted.z,
                 tb ? tb->q.w : 0, tb ? tb->q.x : 0, tb ? tb->q.y : 0, tb ? tb->q.z : 0, tb ? tb->L.x : 0, tb ? tb->L.y : 0,
                 tb ? tb->L.z : 0, tb ? tb->invI[0] : 0, tb ? tb->invI[1] : 0, tb ? tb->invI[2] : 0);
          }
        }
        break;
      }
      case 3:
      case 4:
        ++a.combatCounter;
        out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        out.y = elev(out);
        {
          static const long dbgId = getenv("MOHO64_DEBUG_TACTICS") ? atol(getenv("MOHO64_DEBUG_TACTICS")) : -1;
          if (dbgId >= 0 && static_cast<long>(u->id) == dbgId)
            Logf(LogLevel::Info, "tactics %u out: state %d diff %.9g %.9g %.9g dir3 %.9g %.9g %.9g d3 %.9g lifted %.9g %.9g %.9g to %.9g",
                 now, s, diff.x, diff.y, diff.z, dir3.x, dir3.y, dir3.z, d3, out.x, out.y, out.z, b.minAirspeed);
        }
        SetLength(out, b.minAirspeed);
        break;
      case 5:
        ++a.combatCounter;
        out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        out.y = elev(out);
        SetLength(out, speed);
        break;
      case 6:
        SetState(u, US("MakingAttackRun"), true);
        a.combatCounter = 0;
        out = {fwd.x * speed, fwd.y * speed, fwd.z * speed};
        out.y = elev(out);
        break;
      case 7: {
        a.combatCounter = 0;
        const TerrainMap* map = sim.map();
        Vec3 c{map ? map->width() * 0.5f : 0, 0, map ? map->height() * 0.5f : 0};
        out = {c.x - pos.x, c.y - pos.y, c.z - pos.z};
        out.y = elev(out);
        SetLength(out, speed);
        break;
      }
      default:
        break;
    }
  };
  if (s == 7 && !IsWithin(sim, u, pos, 5.0f)) return output();
  Vec3 h = Normalized(Vec3{dir3.x, 0, dir3.z});
  float dotF = (h.x * fwd.x + h.y * fwd.y) + h.z * fwd.z;
  bool facing = (s == 3) ? dotF > 0.0f : dotF > 0.866f;
  if (eAir && !IsWithin(sim, u, pos, 0.0f)) {
    s = 7;
    return output();
  }
  bool breakOff = (s == 1 && trig > d3) || (b.breakOffIfNearNewTarget && s == 0 && brk > d2) ||
                  (s == 2 && dotF < 0.0f) ||
                  (a.combatCounter > EngineFloor(b.sustainedTurnThreshold * 10.0f));
  {
    // MOHO64_DEBUG_TACTICS=<unit id>: what the attack-run state machine sees each beat
    static const long dbgId = getenv("MOHO64_DEBUG_TACTICS") ? atol(getenv("MOHO64_DEBUG_TACTICS")) : -1;
    if (dbgId >= 0 && static_cast<long>(u->id) == dbgId)
      Logf(LogLevel::Info, "tactics %u unit %u state %d target %u bone %d tp %.9g %.9g %.9g pos %.9g %.9g %.9g d3 %.9g d2 %.9g "
           "trig %.9g brk %.9g dotF %.9g counter %d timer %u break %d",
           now, u->id, s, E ? E->id : 0u, tgt.aimBone, tp.x, tp.y, tp.z, pos.x, pos.y, pos.z, d3, d2, trig, brk, dotF,
           a.combatCounter, a.combatTimer, static_cast<int>(breakOff));
  }
  if (!breakOff) {
    if (a.combatTimer >= now) {
      if (!facing || s == 6) return output();
    }
    if (facing) {
      s = 1;
      if (eAir && MovedThisTick(E)) {
        // (the target's committed orientation: as the beat began, also when it has turned already)
        QuatW q = ToW(E->shapeHeld ? E->shapeOri : E->orientation);
        Vec3 Fe{2 * (q.w * q.y + q.z * q.x), 2 * (q.z * q.y - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
        if ((Fe.x * fwd.x + Fe.z * fwd.z) + Fe.y * fwd.y > 0.0f) s = 2;  // (0x6bd35b..0x6bd387: x, z, then y)
      }
      return output();
    }
    // (0x6bd3c0..0x6bd3fa: summed x, z, then y; `!(0 > dot)`)
    if (s == 2 && !(0.0f > (dir3.x * fwd.x + fwd.z * dir3.z) + fwd.y * dir3.y)) return output();
    uint32_t r1 = sim.NextUInt32();
    {
      static const bool dbgDraw = getenv("MOHO64_DEBUG_TACDRAW") != nullptr;  // who draws a new attack-run state
      if (dbgDraw)
        Logf(LogLevel::Info, "tacdraw %u unit %u (made %llu) target %u", now, u->id,
             static_cast<unsigned long long>(u->entSeq), E ? E->id : 0u);
    }
    s = 3 + static_cast<int>((static_cast<uint64_t>(r1) * 3) >> 32);
    int lo = EngineFloor(b.randomMinChangeCombatStateTime * 10.0f);
    int hi = EngineFloor(b.randomMaxChangeCombatStateTime * 10.0f);
    uint32_t r2 = sim.NextUInt32();
    a.combatTimer = now + static_cast<uint32_t>(lo) +
                    static_cast<uint32_t>((static_cast<uint64_t>(r2) * static_cast<uint32_t>(hi - lo)) >> 32);
    return output();
  }
  s = 6;
  float x = brk / speed * 10.0f;
  int n = EngineCeil(x);
  int m = static_cast<int>(static_cast<float>(n) * b.randomBreakOffDistanceMult);
  uint32_t r = sim.NextUInt32();
  a.combatTimer = now + static_cast<uint32_t>(n) +
                  static_cast<uint32_t>((static_cast<uint64_t>(r) * static_cast<uint32_t>(m - n)) >> 32);
  output();
}

// --------------------------------------------------------------------------------------------
// The tick

struct Transform {
  QuatW q;
  Vec3 pos;
};

void BodySetTransform(PhysBody& b, const Transform& T) {
  b.q = T.q;
  Vec3 r = MultQuadVec(T.q, b.off);
  b.com = {r.x + T.pos.x, r.y + T.pos.y, r.z + T.pos.z};
}
Transform BodyTransform(const PhysBody& b) {
  Transform T;
  T.q = b.q;
  Vec3 r = MultQuadVec(b.q, b.off);
  T.pos = {b.com.x - r.x, b.com.y - r.y, b.com.z - r.z};
  return T;
}

// SPhysBody linear step (inline in CalcMoveAir, 0x6977c0 / 0x697b00)
void BodyLinear(PhysBody& b, const Vec3& F, float dt) {
  float k = dt / b.mass;
  Vec3 vo = b.v;
  const Vec3 g{0, kGravityY, 0};
  float half = dt * 0.5f;
  b.v.x = ((k * F.x) + g.x * dt) + vo.x;
  b.v.y = ((k * F.y) + g.y * dt) + vo.y;
  b.v.z = ((k * F.z) + g.z * dt) + vo.z;
  b.com.x = ((vo.x + b.v.x) * half) + b.com.x;
  b.com.y = ((vo.y + b.v.y) * half) + b.com.y;
  b.com.z = ((vo.z + b.v.z) * half) + b.com.z;
}

// 0x6978d0: angular step with a world torque
void BodyAngular(PhysBody& b, const Vec3& T, float dt) {
  Vec3 Lo = b.L;
  const QuatW q0 = b.q;
  b.L.x = T.x * dt + Lo.x;
  b.L.y = T.y * dt + Lo.y;
  b.L.z = T.z * dt + Lo.z;
  float h = dt * 0.5f;
  Vec3 av{(b.L.x + Lo.x) * h, (b.L.y + Lo.y) * h, (b.L.z + Lo.z) * h};
  QuatW conj{b.q.w, -0.0f - b.q.x, -0.0f - b.q.y, -0.0f - b.q.z};
  Vec3 al = MultQuadVec(conj, av);
  Vec3 d{b.invI[0] * al.x, b.invI[1] * al.y, b.invI[2] * al.z};
  QuatW dq = VecToQuat(d);
  b.q = QMulBody(b.q, dq);
  const QuatW qm = b.q;
  NormalizeQuat(b.q);
  if (g_bodyDebug) {
    auto bits = [](float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      return v;
    };
    Logf(LogLevel::Info,
         "body dt %08x T %08x %08x %08x Lo %08x %08x %08x q0 %08x %08x %08x %08x invI %08x %08x %08x av %08x %08x %08x al %08x "
         "%08x %08x d %08x %08x %08x dq %08x %08x %08x %08x qm %08x %08x %08x %08x q %08x %08x %08x %08x",
         bits(dt), bits(T.x), bits(T.y), bits(T.z), bits(Lo.x), bits(Lo.y), bits(Lo.z), bits(q0.w), bits(q0.x), bits(q0.y),
         bits(q0.z), bits(b.invI[0]), bits(b.invI[1]), bits(b.invI[2]), bits(av.x), bits(av.y), bits(av.z), bits(al.x),
         bits(al.y), bits(al.z), bits(d.x), bits(d.y), bits(d.z), bits(dq.w), bits(dq.x), bits(dq.y), bits(dq.z), bits(qm.w),
         bits(qm.x), bits(qm.y), bits(qm.z), bits(b.q.w), bits(b.q.x), bits(b.q.y), bits(b.q.z));
  }
}

// CUnitMotion::CalcMoveAir 0x6bee50
void CalcMoveAir(Sim& sim, Unit* u, Transform& T) {
  AirMotion& a = A(u);
  static const long dbgBody = getenv("MOHO64_DEBUG_BODY") ? atol(getenv("MOHO64_DEBUG_BODY")) : -1;
  g_bodyDebug = dbgBody >= 0 && static_cast<long>(u->id) == dbgBody;
  if (g_bodyDebug) Logf(LogLevel::Info, "body tick %u", sim.tick());
  const AirBp& b = *a.bp;
  PhysBody& body = a.body;
  const TerrainMap* map = sim.map();
  float maxSpeed = MaxSpeed(u);
  Axes ax = VAxes3(T.q);
  Vec3 Z = ax.Z;
  float lenH = static_cast<float>(std::sqrt(static_cast<double>(Z.z * Z.z + Z.x * Z.x)));
  Vec3 fwdH{};
  if (lenH > 0.0f) {
    float inv = 1.0f / lenH;
    fwdH = {Z.x * inv, inv * 0.0f, Z.z * inv};
  }
  const int layer0 = LayerBits(u->layer);
  // Unit::GetVelocity 0x6a9c90 (0x6bef88): the last beat's displacement only in the Air layer; a transport
  // hovering over its pickup point is in the Land layer and reports the motion's own vector, which the flight
  // code leaves at rest: its horizontal event is 'Stopped' one beat after it got there
  Vec3 vel = layer0 == kAir ? u->motion.lastMove : u->motion.vel;
  vel.y = 0;
  float hspeed = std::sqrt(vel.z * vel.z + vel.x * vel.x);
  BodySetTransform(body, T);
  float surface = Elevation(map, T.pos.x, T.pos.z);
  if (!b.flyInWater && map && map->hasWater && surface < map->waterElevation) surface = map->waterElevation;
  Vec3 d{a.target.x - T.pos.x, 0, a.target.z - T.pos.z};
  float distH = static_cast<float>(std::sqrt(static_cast<double>(d.x * d.x + d.z * d.z)));
  float speed = a.fullSpeed ? maxSpeed : (maxSpeed < distH ? maxSpeed : distH);
  SetLength(d, speed);
  bool landing = false;
  Ctrl ctrl{};

  int layer = LayerBits(u->layer);
  if (u->dead && (layer == kAir || ShouldHover(u))) {
    SetLayerBits(sim, u, kAir);
    SetMotionState(sim, u, 2);
    float r[3];
    for (int i = 0; i < 3; ++i) {
      float ai = std::max(0.25f, std::min(body.invI[i], 4.0f));
      uint32_t x = sim.NextUInt32();
      r[i] = dmath::MulU32(x, ai - (-0.0f - ai)) * 2.3283064365386963e-10f + (-0.0f - ai);  // x87 at PC_24
    }
    a.torque108 = MultQuadVec(body.q, Vec3{r[0] / body.invI[0], r[1] / body.invI[1], r[2] / body.invI[2]});
    a.spinFromDeath = true;
  } else {
    Vec3 facing;
    if (distH > b.startTurnDistance && a.carrierEvent != 2) {
      facing = d;
      a.elevOffset = GetElevationWanted(a, u->position.y);
      SetState(u, US("MovingUp"), false);
      SetState(u, US("MovingDown"), false);
      if (State(u, US("CannotFindPlaceToLand"))) {
        SetState(u, US("CannotFindPlaceToLand"), false);
        UpdateSpeedThrough(sim, u);
      }
    } else {
      landing = a.landLayer != 0 && a.landLayer != kAir;
      int n = EngineFloor(b.autoLandTime * 10.0f);
      if (!landing && a.idleTick > 0 && n > 0 &&
          static_cast<int>(sim.tick()) > static_cast<int>(a.idleTick) + n) {
        if (a.landHeight != kInf) {
          if (State(u, US("CannotFindPlaceToLand"))) {
            SetState(u, US("CannotFindPlaceToLand"), false);
            UpdateSpeedThrough(sim, u);
          }
          landing = true;
          a.landLayer = kLand;
        } else {
          Vec3 spot = a.target;
          bool ok = PrepareMove(sim, u, &spot);
          if (!ok) {
            a.idleTick = sim.tick();
            SetState(u, US("CannotFindPlaceToLand"), true);
            UpdateSpeedThrough(sim, u);
          } else {
            a.target = spot;  // PrepareMove works on the target itself: a spot from its rings has y = 0
            if (State(u, US("CannotFindPlaceToLand"))) {
              SetState(u, US("CannotFindPlaceToLand"), false);
              UpdateSpeedThrough(sim, u);
            }
            int S = std::max(b.footprintX, b.footprintZ);
            int x0 = static_cast<int>(std::nearbyint(a.target.x - b.footprintX * 0.5f));
            int z0 = static_cast<int>(std::nearbyint(a.target.z - b.footprintZ * 0.5f));
            (void)S;
            int r[4] = {x0, z0, x0 + b.footprintX, z0 + b.footprintZ};
            ReserveLandRect(sim, u, r);
            landing = true;
            a.landLayer = Elevation(map, a.target.x, a.target.z) > WaterOrNone(map) ? kLand : kWater;
          }
        }
      }
      // desired facing: the formation vector first
      Vec3 fv = FormationVector(sim, u);
      if (g_bodyDebug) {
        BodyDebugVec("formationVector", &fv.x, 3);
        BodyDebugVec("storedFacing", &a.facing.x, 3);
        BodyDebugVec("fwdH", &fwdH.x, 3);
      }
      if (!IsZeroBits(fv)) {
        a.facing = fv;
        facing = fv;
      } else if (!IsZeroBits(a.facing)) {
        facing = Normalized(a.facing);
      } else {
        facing = fwdH;
      }
      // target elevation offset
      if (landing) {
        SetState(u, US("MovingDown"), true);
        if (a.landHeight != kInf) a.elevOffset = a.landHeight - surface;
        else if (ShouldHover(u) || a.vertEvent == 4) a.elevOffset = b.transportHoverHeight;
        else if (distH < 0.5f || a.vertEvent == 1) a.elevOffset = 0;
        else a.elevOffset = GetElevationWanted(a, u->position.y) * 0.5f;
      } else {
        a.elevOffset = GetElevationWanted(a, u->position.y);
        SetState(u, US("MovingUp"), false);
      }
    }

    // terrain look-ahead, terrain height smoothing, vertical error
    float look = u->motion.speedMult * b.maxAirspeed * 5.0f;
    if (distH < look) look = distH;
    float maxT = LookAheadForMaxTerrain(sim, map, T.pos, look * 1.0f, b.flyInWater);
    float climb = maxT - T.pos.y;
    if (climb < 0.0f) climb = 0.0f;
    if (b.liftFactor < climb && 1.0f < look) {
      look = look * 0.5f;
      float c2 = static_cast<float>(LookAheadForMaxTerrain(sim, map, T.pos, look * 1.0f, b.flyInWater) * 1.5) - T.pos.y;
      if (c2 < 0.0f) c2 = 0.0f;
      float f = (look - c2) / look;
      if (0.2f >= f) f = 0.2f;
      // 0x6bf94c: the square first, then each component times it (d.x * f * f rounds twice on d.x: a last bit
      // off, which grows over the flight)
      const float f2 = f * f;
      d.x = d.x * f2;
      d.z = d.z * f2;
    }
    float step = b.liftFactor * 0.1f;
    if (maxT < a.curTerrain && !landing) step = step * 0.5f;
    float cur = a.curTerrain + step;
    if (maxT < cur) cur = maxT;
    if (cur < a.curTerrain - step) cur = a.curTerrain - step;
    a.curTerrain = cur;
    a.height = T.pos.y - surface;
    d.y = (a.curTerrain + a.elevOffset) - T.pos.y;
    if (landing && d.y < 0.0f) {
      if (b.transportation) {
        if (-3.0f < d.y) d.y = -3.0f;
      } else {
        float h = d.y * 0.5f;
        d.y = (h <= -0.25f) ? h : -0.25f;
      }
    }

    // touchdown / take-off
    if (a.combatState == 0) {
      if (landing && (a.elevOffset == 0.0f || a.landHeight != kInf || ShouldHover(u))) {
        if (distH < 0.5f && (a.height - a.elevOffset < 0.1f || LayerBits(u->layer) == a.landLayer)) {
          FreeLandRectOnTouchdown(sim, u);
          SetLayerBits(sim, u, a.landLayer);
          SetState(u, US("MovingUp"), false);
          SetState(u, US("MovingDown"), false);
          if (ShouldHover(u)) {
            SetVertEvent(sim, u, 4);
          } else {
            SetVertEvent(sim, u, 1);
            a.target = u->position;
            a.prevVel = body.v;
            body.v = {};
            body.L = {};
            return;  // landed: the transform stays as it is
          }
        }
      } else {
        if (a.vertEvent == 1 || a.vertEvent == 4) SetState(u, US("MovingUp"), true);
        float e = a.elevOffset;
        if (0.0f < e && a.height < e * 0.5f) {
          float vs = std::sqrt((body.v.x * body.v.x + body.v.y * body.v.y) + body.v.z * body.v.z);
          if (vs < maxSpeed * 0.08f) {
            float f = a.height / e;
            if (1.0f < f) f = 1.0f;
            d.x = d.x * f;
            d.z = d.z * f;
          }
        }
      }
    }

    // combat tactics and control
    SetState(u, US("MakingAttackRun"), false);
    AiTarget target = u->desiredTarget;
    bool has = combat::HasTarget(sim, target);
    if (has && b.winged) {
      bool can = false;
      for (UnitWeapon* w : u->weapons)
        if (w->bp && WeaponCanAttackTarget(sim, w, target)) {
          can = true;
          break;
        }
      if (can) CombatTactics(sim, u, target, d, fwdH);
    }
    {
      static const long dbgTac = getenv("MOHO64_DEBUG_TACTICS") ? atol(getenv("MOHO64_DEBUG_TACTICS")) : -1;
      if (dbgTac >= 0 && static_cast<long>(u->id) == dbgTac && target.type != 0) {
        const Entity* te = target.type == 1 && target.entityId ? sim.FindEntity(target.entityId) : nullptr;
        bool can = false;
        for (UnitWeapon* w : u->weapons)
          if (w->bp && WeaponCanAttackTarget(sim, w, target)) can = true;
        Logf(LogLevel::Info, "tactics %u unit %u wants type %d entity %d%s%s%s: has %d, a weapon can attack it %d",
             sim.tick(), u->id, target.type, te ? static_cast<int>(te->id) : -1, te && te->dead ? " (dead)" : "",
             te && te->destroyQueued ? " (destroy queued)" : "", target.viaBlip ? " via its blip" : "", has ? 1 : 0,
             can ? 1 : 0);
      }
    }
    if (!has) {
      a.combatCounter = 0;
      a.combatState = 0;
    }
    Vec3 dir = Normalized(d);
    ctrl = ComputeAirControl(sim, u, T.q, d, dir, facing, fwdH, target);

    // events
    if (State(u, US("MovingDown"))) {
      SetVertEvent(sim, u, 3);
    } else if (State(u, US("MovingUp"))) {
      SetVertEvent(sim, u, 2);
    } else if (a.vertEvent != 4) {
      SetVertEvent(sim, u, 0);
    }
    int horz;
    if (distH > b.startTurnDistance || a.fullSpeed) horz = hspeed > maxSpeed * 0.08f ? 1 : 0;
    else if (a.combatState != 0) horz = 0;
    else horz = maxSpeed * 0.005f > hspeed ? 3 : 2;
    {
      static const long dbgH = getenv("MOHO64_DEBUG_AIR") ? atol(getenv("MOHO64_DEBUG_AIR")) : -1;
      if (dbgH >= 0 && static_cast<long>(u->id) == dbgH)
        Logf(LogLevel::Info, "airhorz %u unit %u horz %d hspeed %.5f distH %.4f lastMove %.5f %.5f %.5f layer %s", sim.tick(),
             u->id, horz, hspeed, distH, u->motion.lastMove.x, u->motion.lastMove.y, u->motion.lastMove.z, u->layer.c_str());
    }
    SetHorzEvent(sim, u, horz);
  }

  // integration
  a.prevVel = body.v;
  BodyLinear(body, ctrl.force, 0.1f);
  BodyAngular(body, ctrl.torque, 0.1f);
  if (a.vertEvent != 4) SetLayerBits(sim, u, kAir);
  T = BodyTransform(body);
}

// --------------------------------------------------------------------------------------------
// Falling (CUnitMotion::CalcMoveBallistic 0x6c0290). The step is a segment against the terrain (STIMap::
// Intersection 0x476f30, combat::TerrainSegmentHit): the unit comes down ON the point it returns and the step's
// scale is the hit distance over the segment's length (0x6c0896). On a map with water a unit that is not amphibious
// asks STIMap::SurfaceIntersection 0x577bc0 (0x6c07fc): a terrain point under the water, or none, gives way to the
// water plane (combat::WaterPlaneSegmentHit), and nothing else is tried. Seen: MP1 2695, army 2's interceptor
// 0x200023, shot down, comes down on the water at x 111.887993 in the original; our march put it at 111.888000, and
// army 1's interceptor 0x10001f, flying its attack run at the wreck, took a direction a last bit off (2696; its
// position 2846). Before the terrain, 0x577bc0 asks whether the segment's start (O + D near, near 0 > -inf at
// 0x577be3) is under the water: then that start is the hit, at distance 0 (combat::SegmentStartUnderWater). There
// is no march: with no hit the unit takes the whole step, and the layer comes from where it started (0x6c0937..
// 0x6c098d: the elevation 0x44fb90 at the transform's position, which is not yet moved): amphibious, on or under the
// terrain is Seabed under the water, else Land; otherwise Water when the start is not above the water, Land when it
// is not above the terrain. Seen: MP1 7268, army 3's spy plane 0x300090, shot down past the map's east edge (x 1044
// on the 1024 map), where the line test finds nothing (the ray is clipped to the map, 0x4776e0) and the clamped
// edge's elevation is above its start: Land in the original, and it lies there; ours kept it falling. With a hit
// the layer is the hit's height against the water (0x6c08b7), not the terrain's there.

void CalcMoveBallistic(Sim& sim, Unit* u, Transform& T, float* scale) {
  AirMotion& a = A(u);
  PhysBody& body = a.body;
  const TerrainMap* map = sim.map();
  Vec3 v = u->motion.lastMove;
  v = {v.x * a.lastScale, v.y * a.lastScale, v.z * a.lastScale};
  const float c = 0.0100000007f;
  Vec3 v2{0.0f * c + v.x, v.y + kGravityY * c, v.z + 0.0f * c};
  Vec3 P0 = T.pos, P1{P0.x + v2.x, P0.y + v2.y, P0.z + v2.z};
  *scale = 1.0f;
  {
    static const bool dbg = getenv("MOHO64_DEBUG_BALLISTIC") != nullptr;
    if (dbg)
      Logf(LogLevel::Info, "ballistic %u unit %u dead %d from death %d torque108 %.9g %.9g %.9g", sim.tick(), u->id,
           u->dead ? 1 : 0, a.spinFromDeath ? 1 : 0, a.torque108.x, a.torque108.y, a.torque108.z);
  }
  if (!IsZeroBits(a.torque108)) {  // (0x6c03be: also a torque the flight left there)
    BodyLinear(body, Vec3{}, 0.1f);
    BodyAngular(body, a.torque108, 0.1f);
    T.q = body.q;
  }
  float water = WaterOrNone(map);
  bool amph = u->motion.bp && u->motion.bp->motionType == kMotionAmphibious;
  // segment against the water's surface (start under it), the terrain and the water plane
  bool hit = false;
  float tHit = 1;
  Vec3 hitP{};
  const bool waterPlane = !amph && map && map->hasWater;
  {
    float d = 0;
    Vec3 h{};
    bool found = waterPlane && combat::SegmentStartUnderWater(P0, P1, water, &d, &h);
    if (!found) {
      const bool th = combat::TerrainSegmentHit(map, P0, P1, &d, &h);
      found = (th && (!waterPlane || h.y >= water)) || (waterPlane && combat::WaterPlaneSegmentHit(P0, P1, water, &d, &h));
    }
    if (found) {
      const Vec3 hv{(P1.x - P0.x) * 0.5f, (P1.y - P0.y) * 0.5f, (P1.z - P0.z) * 0.5f};
      const float ext = std::sqrt(hv.y * hv.y + hv.z * hv.z + hv.x * hv.x);
      hit = true;
      hitP = h;
      tHit = d / (ext * 2.0f);
    }
  }
  int oldLayer = LayerBits(u->layer);
  if (hit) {
    P1 = hitP;
    *scale = tHit;
    if (water < hitP.y) SetLayerBits(sim, u, kLand);  // (0x6c08b7..0x6c08dd)
    else if (amph) SetLayerBits(sim, u, kSeabed);
    else SetLayerBits(sim, u, kWater);
  } else {
    const float elev = Elevation(map, P0.x, P0.z);  // (0x6c0854, at the transform's position)
    if (amph) {
      if (elev >= P0.y) SetLayerBits(sim, u, water > elev ? kSeabed : kLand);
    } else if (water >= P0.y) {
      SetLayerBits(sim, u, kWater);
    } else if (elev >= P0.y) {
      SetLayerBits(sim, u, kLand);
    }
  }
  if (LayerBits(u->layer) != oldLayer) {
    if (u->dead) {
      lua_State* L = sim.L();
      lua_pushstring(L, u->layer == "Land" ? "Terrain" : "Water");
      if (sim.collectImpacts)  // (a host's list, render REQUESTS 36; reads only: where it lands, the fall's speed)
        RecordImpact(sim, 2, u, u->layer == "Land" ? "Terrain" : "Water", nullptr, P1,
                             Vec3{v2.x * 10.0f, v2.y * 10.0f, v2.z * 10.0f});
      sim.CallMethod(L, u, "OnImpact", 1);
      SetMotionState(sim, u, 3);
    } else {
      SetMotionState(sim, u, 0);
    }
  }
  T.pos = P1;
  if (0.001f > *scale) *scale = 0.001f;
}

}  // namespace

// --------------------------------------------------------------------------------------------

const AirBp& GetAirBp(lua_State* L, const BlueprintInfo& bp) {
  static const SimCache<std::unordered_map<const BlueprintInfo*, AirBp>> cacheSlot;
  auto& cache = cacheSlot.For(*Sim::From(L));
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  AirBp b;
  Sim* sim = Sim::From(L);
  int top = lua_gettop(L);
  sim->blueprints().PushTable(L, bp);
  int t = lua_gettop(L);
  auto num = [&](int tbl, const char* k, float def) {
    lua_pushstring(L, k);
    lua_rawget(L, tbl);
    float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
  };
  auto boo = [&](int tbl, const char* k, bool def) {
    lua_pushstring(L, k);
    lua_rawget(L, tbl);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
  };
  if (lua_istable(L, t)) {
    b.sizeX = num(t, "SizeX", 1);
    b.sizeY = num(t, "SizeY", 1);
    b.sizeZ = num(t, "SizeZ", 1);
    b.averageDensity = num(t, "AverageDensity", 0.49f);
    b.inertia[0] = num(t, "InertiaTensorX", 0);
    b.inertia[1] = num(t, "InertiaTensorY", 0);
    b.inertia[2] = num(t, "InertiaTensorZ", 0);
    b.collisionOffset[0] = num(t, "CollisionOffsetX", 0);
    b.collisionOffset[1] = num(t, "CollisionOffsetY", 0);
    b.collisionOffset[2] = num(t, "CollisionOffsetZ", 0);
    lua_pushstring(L, "Air");
    lua_rawget(L, t);
    int a = lua_gettop(L);
    if (lua_istable(L, a)) {
      b.canFly = boo(a, "CanFly", false);
      b.winged = boo(a, "Winged", false);
      b.flyInWater = boo(a, "FlyInWater", false);
      b.autoLandTime = num(a, "AutoLandTime", 0);
      b.maxAirspeed = num(a, "MaxAirspeed", 0);
      b.minAirspeed = num(a, "MinAirspeed", 0);
      b.turnSpeed = num(a, "TurnSpeed", 1);
      b.combatTurnSpeed = num(a, "CombatTurnSpeed", 1);
      b.startTurnDistance = num(a, "StartTurnDistance", 0);
      b.tightTurnMultiplier = num(a, "TightTurnMultiplier", 1);
      b.sustainedTurnThreshold = num(a, "SustainedTurnThreshold", 10);
      b.liftFactor = num(a, "LiftFactor", 5);
      b.bankFactor = num(a, "BankFactor", 0.5f);
      b.bankForward = boo(a, "BankForward", false);
      b.engageDistance = num(a, "EngageDistance", 0);
      b.breakOffTrigger = num(a, "BreakOffTrigger", 0);
      b.breakOffDistance = num(a, "BreakOffDistance", 0);
      b.breakOffIfNearNewTarget = boo(a, "BreakOffIfNearNewTarget", false);
      b.kMove = num(a, "KMove", 1);
      b.kMoveDamping = num(a, "KMoveDamping", 1);
      b.kLift = num(a, "KLift", 1);
      b.kLiftDamping = num(a, "KLiftDamping", 1);
      b.kTurn = num(a, "KTurn", 3);
      b.kTurnDamping = num(a, "KTurnDamping", 3);
      b.kRoll = num(a, "KRoll", 3);
      b.kRollDamping = num(a, "KRollDamping", 3);
      b.circlingTurnMult = num(a, "CirclingTurnMult", 3);
      b.circlingRadiusChangeMinRatio = num(a, "CirclingRadiusChangeMinRatio", 0.6f);
      b.circlingRadiusChangeMaxRatio = num(a, "CirclingRadiusChangeMaxRatio", 0.9f);
      b.circlingRadiusVsAirMult = num(a, "CirclingRadiusVsAirMult", 1);
      b.circlingElevationChangeRatio = num(a, "CirclingElevationChangeRatio", 0.25f);
      b.circlingFlightChangeFrequency = num(a, "CirclingFlightChangeFrequency", 2);
      b.circlingDirChange = boo(a, "CirclingDirChange", true);
      b.hoverOverAttack = boo(a, "HoverOverAttack", false);
      b.randomBreakOffDistanceMult = num(a, "RandomBreakOffDistanceMult", 1.5f);
      b.randomMinChangeCombatStateTime = num(a, "RandomMinChangeCombatStateTime", 3);
      b.randomMaxChangeCombatStateTime = num(a, "RandomMaxChangeCombatStateTime", 6);
      b.transportHoverHeight = num(a, "TransportHoverHeight", 0);
      b.predictAheadForBombDrop = num(a, "PredictAheadForBombDrop", 0);
    }
    lua_pop(L, 1);
    lua_pushstring(L, "Physics");
    lua_rawget(L, t);
    int p = lua_gettop(L);
    if (lua_istable(L, p)) {
      b.elevation = num(p, "Elevation", 0);
      b.attackElevation = num(p, "AttackElevation", 0);
      b.maxSpeed = num(p, "MaxSpeed", 0);
    }
    lua_pop(L, 1);
  }
  lua_settop(L, top);
  if (bp.hasFootprint) {
    b.footprintX = std::max<int>(1, bp.footprint.sizeX);
    b.footprintZ = std::max<int>(1, bp.footprint.sizeZ);
    b.fpMaxSlope = bp.footprint.maxSlope;
    b.fpMinWaterDepth = bp.footprint.minWaterDepth;
    b.fpFlags = bp.footprint.flags;
  }
  b.transportation = BpInCategory(*sim, &bp, "TRANSPORTATION");
  b.targetChaser = BpInCategory(*sim, &bp, "TARGETCHASER");
  b.experimental = BpInCategory(*sim, &bp, "EXPERIMENTAL");
  b.airCategory = BpInCategory(*sim, &bp, "AIR");
  return cache.emplace(&bp, b).first->second;
}

float AirSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z, float y) {
  const AirBp& b = GetAirBp(L, bp);
  if (!b.canFly) return y;
  return AirLayerSpawnHeight(sim, bp, L, x, z);
}

// IUnit::CalcSpawnElevation 0x541540 for the Air or Orbit layer (al & 0x30, 0x5415d6): 0x541a30, the map's elevation
// at the point (0x44fb90) or the water where that is higher, plus the blueprint's Physics.Elevation (edi+0x30); for
// any unit that starts there, whatever its motion. Seen: MP1 15588, army 6's Tempest 0x600049 makes its external
// factory 0x6000c7 (uas0401ef, a copy of zxa0002: MotionType None, BuildOnLayerCaps all) with CreateUnitHPR at the
// ExternalFactoryPoint bone (y 18.8887, over water): the original's stands at 17.5, the water, until its attach;
// ours kept the bone's y.
float AirLayerSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z) {
  const AirBp& b = GetAirBp(L, bp);
  const TerrainMap* map = sim.map();
  float h = Elevation(map, x, z);
  if (map && map->hasWater && map->waterElevation > h) h = map->waterElevation;
  return h + b.elevation;
}

void AirInit(Sim& sim, Unit* u) {
  lua_State* L = sim.L();
  auto a = std::make_shared<AirMotion>();
  a->bp = &GetAirBp(L, *u->blueprint);
  const AirBp& b = *a->bp;
  a->landHeight = kInf;
  a->elevationAttr = b.elevation;
  a->target = u->position;
  QuatW q = ToW(u->orientation);
  Axes ax = VAxes3(q);
  a->facing = ax.Z;
  const TerrainMap* map = sim.map();
  float s = Elevation(map, u->position.x, u->position.z);
  if (map && map->hasWater) s = std::max(s, map->waterElevation);
  a->curTerrain = s;
  PhysBody& pb = a->body;
  pb.mass = ((b.averageDensity * b.sizeZ) * b.sizeY) * b.sizeX;
  pb.invI[0] = 1.0f / (b.inertia[0] * pb.mass);
  pb.invI[1] = 1.0f / (b.inertia[1] * pb.mass);
  pb.invI[2] = 1.0f / (b.inertia[2] * pb.mass);
  pb.off = {b.collisionOffset[0], b.sizeY * 0.5f + b.collisionOffset[1], b.collisionOffset[2]};
  u->motion.air = a;
  BodySetTransform(pb, Transform{q, u->position});
}

void AirWarp(Sim& sim, Unit* u) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  BodySetTransform(a.body, Transform{ToW(u->orientation), u->position});
  const TerrainMap* map = sim.map();
  float s = Elevation(map, u->position.x, u->position.z);
  if (map && map->hasWater) s = std::max(s, map->waterElevation);
  a.curTerrain = s;
  SetTarget(sim, u, u->position, Vec3{}, 0);
  a.fullSpeed = u->motion.airSpeedThrough;
}

void AirSpeedThroughEvent(Sim& sim, Unit* u) { UpdateSpeedThrough(sim, u); }
void AirSetSpeedThroughGoal(Unit* u, bool v) {
  u->motion.airSpeedThrough = v;
  if (u->motion.air) A(u).fullSpeed = v;
}

void AirSetCarrierEvent(Unit* u, int e) {
  if (u->motion.air) A(u).carrierEvent = e;
}
void AirSetImmediateVelocity(Sim& sim, Unit* u, Vec3 vel, Quat q) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  u->orientation = q;
  BodySetTransform(a.body, Transform{ToW(q), u->position});
  a.body.v = vel;
  (void)sim;
}

void AirSetGoal(Sim& sim, Unit* u, Vec3 goal, uint32_t tick, int layer, bool landingSpot) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  // the goal cell's centre (SNavGoal of the command position; CellToWorld)
  const AirBp& b = *a.bp;
  a.pendingLayer = layer;
  a.pendingFacing = {};
  // COORDS_ToWorldPos 0x50b0f0: a goal cell's world position has the height of the ground there, or of the
  // water above it unless the footprint may stand on the seabed (OccupancyCaps bit 2): the height that came
  // with the order is not used
  const TerrainMap* map = sim.map();
  const bool seabed = u->blueprint && u->blueprint->hasFootprint && (u->blueprint->footprint.caps & 2);
  auto cellHeight = [&](float x, float z) {
    float h = Elevation(map, x, z);
    if (!seabed && map && map->hasWater && map->waterElevation > h) h = map->waterElevation;
    return h;
  };
  if (layer == kLand && b.canFly && landingSpot) {  // a landing move: a free spot (NewMoveTask ctor step 4)
    int cx = static_cast<int>(std::nearbyint(goal.x - b.footprintX * 0.5f));
    int cz = static_cast<int>(std::nearbyint(goal.z - b.footprintZ * 0.5f));
    goal = {cx + b.footprintX * 0.5f, 0.0f, cz + b.footprintZ * 0.5f};
    goal.y = cellHeight(goal.x, goal.z);
    PrepareMove(sim, u, &goal);
    // ... and the ctor reserves the footprint's rect at the spot found (0x618613..0x618631: 0x50b010, ReserveOgridRect
    // 0x6ab6f0, +0x92 = 1) until the move task ends, as for a factory's rally Move (commands.cpp): the ctor's
    // PrepareMove branch (0x6184c5..) is the same for both. So a second transport sent to the same place finds the
    // first one's landing spot taken and goes to the ring two footprints out. Seen: MP1 8638, army 7's transport
    // 0x700003 (xsa0104) starts its unload at (292.52, 621.39): the original lands at (298.5, 627.5), as 0x700019's
    // landing spot (291.5, 622.5) holds the cell; ours, holding nothing, took (292.5, 621.5). And 8685: both get one
    // unload point, the original puts them on two spots of the ring.
    const NamedFootprint& fp = UnitFootprint(u);
    const int rx = static_cast<int>(std::nearbyint(goal.x - fp.sizeX * 0.5f));
    const int rz = static_cast<int>(std::nearbyint(goal.z - fp.sizeZ * 0.5f));
    const int r[4] = {rx, rz, rx + fp.sizeX, rz + fp.sizeZ};
    GroundReserveRectImpl(sim, u, r);
    u->moveTaskReserved = true;
  }
  int cx = static_cast<int>(std::nearbyint(goal.x - b.footprintX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(goal.z - b.footprintZ * 0.5f));
  a.pending = true;
  a.pendingTick = tick;
  a.pendingGoal = {cx + b.footprintX * 0.5f, 0.0f, cz + b.footprintZ * 0.5f};
  a.pendingGoal.y = cellHeight(a.pendingGoal.x, a.pendingGoal.z);
  a.destEntity = 0;  // (a destination is cleared by the next SetGoal)
  u->motion.hasGoal = true;
  u->motion.arrived = false;
  (void)sim;
}

// CAiNavigatorAir::SetDestUnit 0x5a4a70 (air_nav.md 4.5): the motion flies at the entity's position itself (no
// goal cell), the target layer stays, the navigator steers; its own goal is not touched.
void AirSetDestEntity(Sim& sim, Unit* u, const Entity* e) {
  if (!u->motion.air || !e) return;
  AirMotion& a = A(u);
  a.pending = false;
  a.steering = true;
  a.destEntity = EntityRef(e);
  SetTarget(sim, u, e->position, Vec3{}, 0);
  u->motion.hasGoal = true;
  u->motion.arrived = false;
}

// Unit::KillCleanup 0x6a8790 deletes the unit's navigator (unit+0x54c = null, 0x6a8804..0x6a8832): from then on
// whoever asks it for its status gets nothing. UpdateCurrentTargetFromFormation 0x5a4d80 returns without a new
// target when the leader has no navigator (0x5a4ddd..0x5a4de5) or it does not steer (HasGoodPath, 0x5a4de7..0x5a4df2),
// so the escorts of a killed aircraft keep the slot they last took. Seen: MP3 10543, army 1's five interceptors
// guarding the Corsair 0x10012f, killed at 10542: the original's targets stay at their 10542 slots; ours took the
// slots of the falling wreck, which still counted as steering.
void AirNavigatorGone(Unit* u) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  a.steering = false;
  a.pending = false;
  a.destEntity = 0;
}

void AirSetLandHeight(Unit* u, float h) {
  if (u->motion.air) A(u).landHeight = h;
}

void AirSetFacing(Unit* u, Vec3 dir) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  a.facing = dir;
  if (a.pending) a.pendingFacing = dir;
}

// SetElevationRect 0x477e10 -> UpdateBounds(x0-1, z0-1, x1, z1): the samples [x0, x1] x [z0, z1] changed
// (Sim::FlattenMapRect); every tier cell whose sample block meets the rectangle is recomputed.
void AirTerrainChanged(const Sim& sim, const TerrainMap* map, int x0, int z0, int x1, int z1) {
  auto& c = TierCache(sim);
  auto it = c.find(map);
  if (it == c.end()) return;
  Tiers& t = it->second;
  for (int k = 1; k <= static_cast<int>(t.hi.size()); ++k) {
    int s = 1 << k, tw = t.tw[k - 1], th = t.th[k - 1];
    // cell i covers samples [i*s, i*s + s]
    int i0 = std::max(0, (x0 - 1 - s) / s), i1 = std::min(tw - 1, std::max(0, x1 + 1) / s);
    int j0 = std::max(0, (z0 - 1 - s) / s), j1 = std::min(th - 1, std::max(0, z1 + 1) / s);
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) t.hi[k - 1][static_cast<size_t>(j) * tw + i] = TierCell(t, map, k, i, j);
  }
}

void AirSetTargetNow(Sim& sim, Unit* u, Vec3 p, int layer) {
  if (!u->motion.air) return;
  SetTarget(sim, u, p, Vec3{}, layer);
}
void AirSetTargetFacing(Sim& sim, Unit* u, Vec3 p, Vec3 dir, int layer) {
  if (!u->motion.air) return;
  SetTarget(sim, u, p, dir, layer);
}

bool GroundPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float excl[4], int spacing) {
  return GroundPrepareMoveImpl(sim, u, pos, excl, spacing);
}
void GroundReserveRect(Sim& sim, Unit* u, const int r[4]) { GroundReserveRectImpl(sim, u, r); }
void GroundFreeRect(Sim& sim, Unit* u) { GroundFreeRectImpl(sim, u); }
bool GroundCanReserveRect(Sim& sim, Unit* u, const int r[4]) { return GroundCanReserve(sim, u, r); }
bool GroundCellFits(Sim& sim, Unit* u, int cx, int cz) {  // as GroundSpotOkImpl's "caps" test
  const NamedFootprint& fp = UnitFootprint(u);
  int c = sim.navigation().CellCaps(fp, cx, cz);  // (Grid(fp)->Caps without making the grid)
  if (u->layer == "Water") c &= ~kSub;
  c = sim.navigation().FootprintFits(static_cast<uint8_t>(c), fp, cx, cz);
  return c != 0;
}

bool AirPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float* excl, int spacing) {
  if (!u->motion.air) return false;
  return PrepareMove(sim, u, pos, excl, spacing);
}

void AirStopHere(Sim& sim, Unit* u) {
  if (u->motion.air) Stop(sim, u, nullptr);
}

// The navigator's SetGoal 0x5a4c60: SetMotionTarget 0x5a4a90 = CUnitMotion::SetTarget at once, status 2. Ours takes
// a goal up in the unit's motion step of the beat it was given in (a.pending, AirMotionTick).
void TakePendingGoal(Sim& sim, Unit* u) {
  AirMotion& a = A(u);
  a.pending = false;
  a.goal = a.pendingGoal;
  a.curTarget = a.goal;
  a.steering = true;
  SetTarget(sim, u, a.goal, a.pendingFacing, a.pendingLayer ? a.pendingLayer : kAir);
  a.inFormation = false;  // SetGoal 0x5a4c60: in a form formation unless the navigator ignores it
  if (!(u->navIgnoreFormation || (u->navigator && u->navigator->ignoreFormation))) {
    Formation* f = GetFormation(sim, u);
    a.inFormation = f && FormationIsForm(*f);
  }
}

void AirTakeGoalNow(Sim& sim, Unit* u) {
  if (u->motion.air && A(u).pending && u->fractionComplete >= 1.0f) TakePendingGoal(sim, u);
}

void AirAbort(Sim& sim, Unit* u) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  // A goal given earlier in this beat and aborted before the motion step: in the original that SetGoal has
  // already set the motion's target, and with it the facing toward it (SetTarget 0x6b85e0: no direction given and
  // farther than StartTurnDistance), which Stop 0x6b8460 keeps; the brake that follows turns the unit to it. Seen:
  // a Soul Ripper whose attack order ends because its target died (Experimental2 586): the attack task's last
  // Update (state 1 on the weapon's report "target lost") gives the goal again, then the task ends and aborts.
  if (a.pending && sim.tick() >= a.pendingTick && u->fractionComplete >= 1.0f) TakePendingGoal(sim, u);
  a.pending = false;
  a.destEntity = 0;
  Vec3 p = PredictAhead(sim, u, 1.0f);
  Stop(sim, u, &p);
  a.steering = false;
  a.fullSpeed = false;
  u->motion.airSpeedThrough = false;
  u->motion.hasGoal = false;
}

Vec3 PredictAhead(Sim& sim, Unit* u, float t, bool other) {
  (void)sim;
  // asked of ANOTHER unit inside the motion pass: its position and its last displacement are those the beat
  // began with (the transform is committed at the beat's end), its body's turn rate is the one of now
  Vec3 p = other && u->shapeHeld ? u->shapePos : u->position;
  // every unit with a motion has the body (unit+0x4b0, unit+0x1fc): a walking or driving unit is carried on
  // along its velocity too, its body just never turns (seen: bombers leading a commander who starts to walk)
  if (!u->motion.air && !(u->motion.bp && u->motion.bp->mobile())) return p;
  Vec3 w{};
  if (u->motion.air) {
    const PhysBody& pb = A(u).body;
    // world angular velocity 0x697e70: R * (invI (.) R^T L), through the axes of the orientation (VAxes3), each
    // sum in the original's order: the prediction below turns ten times by it, and a last bit here is a last
    // bit in an attacker's desired velocity
    const Axes ax = VAxes3(pb.q);
    const Vec3& L = pb.L;
    const float la = (L.x * ax.X.x + L.y * ax.X.y) + L.z * ax.X.z;
    const float lb = (L.z * ax.Y.z + L.y * ax.Y.y) + L.x * ax.Y.x;
    const float lc = (L.z * ax.Z.z + L.y * ax.Z.y) + L.x * ax.Z.x;
    const float wa = pb.invI[0] * la, wb = pb.invI[1] * lb, wc = pb.invI[2] * lc;
    w = {(ax.Z.x * wc + ax.Y.x * wb) + wa * ax.X.x, (ax.Z.y * wc + ax.Y.y * wb) + wa * ax.X.y,
         (ax.Z.z * wc + ax.Y.z * wb) + wa * ax.X.z};
  }
  // Unit::GetVelocity 0x6a9c90: the last beat's displacement only while the unit is in the Air layer; in any
  // other layer it is the motion's own velocity vector (motion+0x38), which the flight code does not use: a
  // transport coming down on its landing spot (layer Land) is "not moving" for this
  Vec3 v = u->layer == "Air" ? (other ? u->motion.velCommitted : u->motion.lastMove) : u->motion.vel;
  float ang = w.y * 0.1f;
  float s = dmath::Sin(ang * 0.5f), c = dmath::Cos(ang * 0.5f);
  QuatW q{c, 0.0f * s, 1.0f * s, 0.0f * s};  // 0x570750: the axis (0, 1, 0) times the sine
  float n = t * 10.0f;
  while (n > 0.0f) {
    v = MultQuadVec(q, v);
    Vec3 st = n >= 1.0f ? v : Vec3{v.x * n, v.y * n, v.z * n};
    p.x += st.x;
    p.z += st.z;
    n -= 1.0f;
  }
  return p;
}

// CAiNavigatorAir::Execute 0x5a50d0 -> UpdateCurrentTargetFromFormation 0x5a4d80: follow the formation slot while
// the leader steers. The navigator runs in the army stage, before the command stage gives goals and before the
// motion: it reads the slot (u+0x59c), the leader (u+0x584) and the formation (u+0x580) as the unit's
// UpdateInfoCache 0x6a9810 wrote them in its motion turn of the beat before, and a goal given in this beat's command
// stage (SetGoal 0x5a4c60 sets the motion's target at once) overrides the slot until the next beat. Seen in MP1's
// oracle: army 6's interceptor 0x60001b in a transport's guard formation has, at 1938, the slot ours wrote at 1937,
// and at 1937 the guard's re-goal (884, 25.16, 507), the slot only from 1938. The leader's status is the one of the
// army stage too, after its own Execute (creation order): at 2213 the transport it guards is stopped by FAF's
// AirTransportUnit.OnMotionHorzEventChange ('Stopped' -> navigator:AbortMove, in the transport's motion), and the
// original's interceptor still follows its slot in that beat and stops following only at 2214. So it runs in the
// navigators' stage (LandNavTickUnits), after AirNavArrival, not in the motion pass.
void AirFormationFollow(Sim& sim, Unit* u) {
  if (!u->motion.air || u->fractionComplete < 1.0f) return;
  AirMotion& a = A(u);
  if (!(a.steering && a.inFormation) || u->navIgnoreFormation || (u->navigator && u->navigator->ignoreFormation) ||
      State(u, US("Ferrying")))
    return;
  Formation* f = GetFormation(sim, u);
  Entity* le = u->formLeader ? sim.FindEntity(u->formLeader) : nullptr;
  Unit* L = le && le->kind == Entity::Kind::Unit ? static_cast<Unit*>(le) : nullptr;
  if (f && L && L != u) {
    bool good = L->motion.air ? A(L).steering : LandNavStatus(L) == 2;
    if (good) {
      a.curTarget = u->formSlot;
      SetTarget(sim, u, u->formSlot, Vec3{}, 0);
    }
  } else {
    a.inFormation = false;
    a.curTarget = a.goal;
    SetTarget(sim, u, a.goal, Vec3{}, 0);
  }
}

void AirMotionTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  {
    static const long dbgE = getenv("MOHO64_DEBUG_AIR") ? atol(getenv("MOHO64_DEBUG_AIR")) : -1;
    if (dbgE >= 0 && static_cast<long>(u->id) == dbgE)
      Logf(LogLevel::Info, "airenter %u unit %u lastMove %.5f %.5f %.5f layer %s", sim.tick(), u->id, m.lastMove.x,
           m.lastMove.y, m.lastMove.z, u->layer.c_str());
  }
  if (m.airLayerPending) {  // let go by a transport this beat (DetachUnitFrom): the Air layer from its motion step
    m.airLayerPending = false;
    SetLayerBits(sim, u, kAir);
  }
  if (!m.air) AirInit(sim, u);
  AirMotion& a = A(u);
  if (u->fractionComplete < 1.0f) return;
  // the navigator's SetGoal (steered from this beat's motion on; the formation slot of AirFormationFollow, taken
  // before, gives way to it)
  if (a.pending && sim.tick() >= a.pendingTick) TakePendingGoal(sim, u);
  Transform T{ToW(u->orientation), u->position};
  Vec3 start = u->position;
  bool idle = u->commands.empty();
  if (idle) {
    if (a.idleTick == 0) a.idleTick = sim.tick();
  } else {
    a.idleTick = 0;
  }
  float scale = 1.0f;
  bool move = true;
  switch (a.motionState) {
    case 2:
      CalcMoveBallistic(sim, u, T, &scale);
      break;
    case 3:
      move = false;
      break;
    default:
      if (!u->immobile && !u->stunned && a.bp->canFly) {
        CalcMoveAir(sim, u, T);
      } else {
        SetHorzEvent(sim, u, 3);
        move = false;
      }
      break;
  }
  if (move) {
    NormalizeQuat(T.q);
    u->orientation = FromW(T.q);
    u->position = T.pos;
    a.lastScale = 1.0f / scale;
  }
  m.lastMove = {(u->position.x - start.x) * a.lastScale, (u->position.y - start.y) * a.lastScale,
                (u->position.z - start.z) * a.lastScale};
  if (!move) m.lastMove = {};
}

namespace {
int l_SetElevation(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (u && u->motion.air) u->motion.air->elevationAttr = static_cast<float>(luaL_checknumber(L, 2));
  else if (u) u->motion.elevationAttr = static_cast<float>(luaL_checknumber(L, 2));  // (submarines: the dive depth)
  return 0;
}
int l_RevertElevation(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (u && u->motion.air) u->motion.air->elevationAttr = u->motion.air->bp->elevation;
  else if (u && u->motion.bp) u->motion.elevationAttr = u->motion.bp->elevation;
  return 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Fuel (fuel.md)

namespace {
struct PlatformBp {
  float mult = 1, repair = 20, energy = 2, mass = 0.5f;
};
const PlatformBp& GetPlatformBp(Sim& sim, const BlueprintInfo& bp) {
  static const SimCache<std::map<const BlueprintInfo*, PlatformBp>> cacheSlot;
  auto& cache = cacheSlot.For(sim);
  auto it = cache.find(&bp);
  if (it != cache.end()) return it->second;
  PlatformBp r;
  lua_State* L = sim.L();
  int top = lua_gettop(L);
  sim.blueprints().PushTable(L, bp);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "AI");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      auto num = [&](const char* k, float def) {
        lua_pushstring(L, k);
        lua_rawget(L, -2);
        float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
        lua_pop(L, 1);
        return v;
      };
      r.mult = num("RefuelingMultiplier", 1);
      r.repair = num("RefuelingRepairAmount", 20);
      r.energy = num("RepairConsumeEnergy", 2);
      r.mass = num("RepairConsumeMass", 0.5f);
    }
  }
  lua_settop(L, top);
  return cache.emplace(&bp, r).first->second;
}
void ClearRepairRequest(Unit* u) {
  UnitMotion& m = u->motion;
  if (m.repairRequest) m.repairRequest->live = false;
  m.repairRequest.reset();
  m.refuelFlag = false;
}
void FuelCallback(Sim& sim, Unit* u, const char* name) { sim.CallMethod(sim.L(), u, name, 0); }
}  // namespace

Unit* StagingPlatformOf(Sim& sim, const Unit* u) {
  if (!u->transportedBy) return nullptr;
  Entity* e = sim.FindEntity(u->transportedBy);
  if (!e || e->kind != Entity::Kind::Unit || e->dead) return nullptr;
  Unit* p = static_cast<Unit*>(e);
  return p->transport && p->transport->isAirStaging ? p : nullptr;
}

void FuelTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (u->dead || !m.bp || !(m.fuelUseTime > 0)) return;
  const float old = u->fuelRatio;
  const int ve = m.air ? A(u).vertEvent : 1;
  const bool grounded = ve == 1 || ve == 4;
  float nv;
  if (!grounded) {  // burn
    if (m.refuelFlag) ClearRepairRequest(u);
    nv = old - 1.0f / (m.fuelUseTime * 10.0f);
    if (!(nv > 0)) nv = 0;
    if (nv == 0 && old > 0) FuelCallback(sim, u, "OnRunOutOfFuel");
  } else {  // refuel
    Unit* P = StagingPlatformOf(sim, u);
    float rate = (m.fuelRecharge / m.fuelUseTime) * 0.1f;
    const bool damaged = u->health < u->maxHealth;
    if (!P) {
      rate = rate * 0.1f;
    } else {
      const PlatformBp& pb = GetPlatformBp(sim, *P->blueprint);
      rate = pb.mult * rate;
      if (!m.refuelFlag && old < 1.0f) {
        m.refuelFlag = true;
        FuelCallback(sim, u, "OnStartRefueling");
      }
      if (damaged && u->army) {
        if (!m.repairRequest) {
          m.refuelFlag = true;
          m.repairReq[kEnergy] = pb.energy;
          m.repairReq[kMass] = pb.mass;
          m.repairRequest = u->army->econ.NewRequest();
          m.repairRequest->requested[kEnergy] = pb.energy;
          m.repairRequest->requested[kMass] = pb.mass;
        } else if (m.repairRequest->granted[kEnergy] >= m.repairReq[kEnergy] &&
                   m.repairRequest->granted[kMass] >= m.repairReq[kMass]) {
          float got[2];
          float all[2] = {m.repairRequest->granted[0], m.repairRequest->granted[1]};
          m.repairRequest->Take(all, got);
          EntityAdjustHealth(sim.L(), u, P, pb.repair * 0.1f);
        }
      }
    }
    // (the damaged flag is the one taken before this beat's repair, 0x6b9a77 -> [esp+0x13], tested again at
    // 0x6b9c5d: the beat that heals the unit to full keeps the request, the next one drops it. Seen: MP1 8158, army
    // 1's fighter 0x8e (uaa0303) on a staging platform; its 30 E repair request still counts in the share-out of 8158
    // in the original; ours dropped it a beat early.)
    if (m.refuelFlag && old > 0.99f && !damaged) ClearRepairRequest(u);
    nv = rate + old;
    if (nv > 1.0f) nv = 1.0f;
    if (old == 0 && nv > 0) FuelCallback(sim, u, "OnGotFuel");
  }
  u->fuelRatio = nv;
}

void AirNotifyAttached(Sim& sim, Unit* u) {
  if (!u->motion.air) return;
  SetMotionState(sim, u, 1);
  SetHorzEvent(sim, u, 3);
  SetVertEvent(sim, u, 1);
}
void AirNotifyDetached(Sim& sim, Unit* u) {
  if (!u->motion.air) return;
  SetMotionState(sim, u, 0);
}
// CUnitMotion::MotionTick 0x6b9d10, motion state Attached with the parent link gone: the state becomes None for
// what can fly (Ballistic otherwise), the layer Air, and the transform stays where it is for this beat.
void AirParentGone(Sim& sim, Unit* u) {
  if (!u->motion.air) AirInit(sim, u);
  SetMotionState(sim, u, A(u).bp->canFly ? 0 : 2);
  SetLayerBits(sim, u, kAir);
  u->motion.lastMove = {};
}

// CAiNavigatorAir::Execute 0x5a50d0, the arrival (AtTarget + goal cell): it runs in the navigators' stage of each army
// (CArmyImpl::OnTick, brain+0xa8, beat_order.md 1.1), at the BEAT'S START, with the position the last motion left; so
// the Succeeded that wakes the move task re-links the command thread in the navigators' order of that beat (land and
// air navigators of an army together, by creation), not in the motion stage of the beat before. Seen: Skirmish 1383,
// Chloe's transport 0x600018 reaches its pickup point (its landing move ends) in the same beat as its cargo 0x600017
// ends its walk to the transport: the original's cargo thread still comes first in 1383, finds the transport not yet
// at its pickup (TransportAtPickupPosition comes in the transport's turn) and starts its beam-up in 1384; ours had
// re-linked the transport in the motion of 1382, so the transport went first and the cargo beamed up in 1383.
//
// Both re-aims at a destination entity (UpdateCurrentTargetFromDestinationEntity 0x5a4ea0: SetTarget(the entity's
// committed place +0xac, no direction, layer kept)) are Execute's own and run here too, then Execute returns (no
// formation following): a TARGETCHASER whose destination moved in the last beat (+0xac against +0xc8, 0x5a5149), and
// anybody at its target with a destination (0x5a5291..0x5a529b: no arrival). Not later in the beat's motion: a
// command that aborts the move in this beat's command stage (AbortMove -> Stop at the point one second ahead, which
// keeps the facing) comes after the re-aim, and the facing the re-aim gave is the one the hover turns to. Seen: MP3
// 9291, army 0's 0xb8 on a patrol's attack of 0x100170, which died at 9290: at its target with 0x100170 as its
// destination, the original's Execute aimed at the dead aircraft's place (facing (-0.2047, -0.9788)) before the attack
// task ended; ours re-aimed only in the motion step, after the abort had dropped the destination, and kept the attack
// run's facing; at 9296, inside StartTurnDistance, the hover turned to it (quaternion 0.0184 against 0.0174).
bool AirNavArrival(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!m.air) return false;
  AirMotion& a = A(u);
  if (a.steering && !a.pending) {
    // a TARGETCHASER whose destination entity is mobile and moved in the last beat takes its place as the motion's
    // target again; everybody else keeps the target SetDestUnit gave. A CZAR chases; a gunship does not.
    if (a.destEntity) {
      const Entity* de = sim.FindEntity(a.destEntity);
      if (de && !de->destroyQueued && de->kind == Entity::Kind::Unit) {
        const Unit* du = static_cast<const Unit*>(de);
        const Vec3& now = du->position;  // (nothing has moved yet in this beat: the committed transforms)
        const Vec3& before = du->lastPosition;
        const bool moved = !vm::SameBits(now, before);  // (0x5a5142..0x5a5156: memcmp 0x4f0a50 of +0xac / +0xc8)
        if (moved && du->motion.bp && du->motion.bp->mobile() && BpInCategory(sim, u->blueprint, "TARGETCHASER")) {
          SetTarget(sim, u, now, Vec3{}, 0);
          return true;
        }
      }
    }
    float dx = a.target.x - u->position.x, dz = a.target.z - u->position.z;
    float d = std::sqrt(dx * dx + dz * dz);
    float r = 0.25f;
    if (a.fullSpeed) {
      float v = a.bp->maxAirspeed * m.speedMult;
      r = a.bp->winged ? std::max(0.25f, v) : std::max(0.25f, 0.25f * v);
    }
    bool layerOk = a.landLayer == 0 || LayerBits(u->layer) == a.landLayer;
    bool at = layerOk && (d <= r || a.vertEvent == 4 || (a.vertEvent == 1 && a.landHeight != kInf));
    if (at && a.inFormation) {  // a follower only arrives when its slot (the motion target) is in the goal cell
      const AirBp& ab = *a.bp;
      auto cell = [&](float v, int s) { return static_cast<int>(std::nearbyint(v - s * 0.5f)); };
      at = cell(a.target.x, ab.footprintX) == cell(a.goal.x, ab.footprintX) &&
           cell(a.target.z, ab.footprintZ) == cell(a.goal.z, ab.footprintZ);
    }
    if (at && a.destEntity) {
      // a destination entity: no arrival, the target is taken from the entity again. So within its arrival radius - a
      // second of flight for a fighter at speed - an aircraft follows its destination beat by beat, chaser or not.
      // (Seen: the flight record's target of a patrolling fighter that attacks a passing aircraft is that aircraft's
      // place of the beat before, every beat.)
      const Entity* de = sim.FindEntity(a.destEntity);
      if (de && !de->destroyQueued) {
        SetTarget(sim, u, de->position, Vec3{}, 0);
        return true;
      }
    }
    if (at) {
      a.steering = false;
      m.hasGoal = false;
      m.arrived = true;
      // Succeeded wakes the move task (re-linked at the stage's new end); a refuel's own goal has no listener
      if (!RefuelGoalWakesNobody(u)) sim.ResumeCommandThread(u);
      // MOHO64_DEBUG_AIRARRIVE=1: every arrival, with what the unit was doing
      static const bool dbgArrive = getenv("MOHO64_DEBUG_AIRARRIVE") != nullptr;
      if (dbgArrive)
        Logf(LogLevel::Info, "airarrive %u unit %#x d %.4f r %.4f vert %d landLayer %d layer %s queue %zu head %d task %d",
             sim.tick(), u->id, d, r, a.vertEvent, a.landLayer, u->layer.c_str(), u->commands.size(),
             u->commands.empty() ? -1 : static_cast<int>(u->commands.front()->type), u->task ? 1 : 0);
    }
  }
  return false;
}

namespace {
Unit* FuelUnit(lua_State* L, int nargs) {
  if (lua_gettop(L) != nargs) luaL_error(L, "%s\n  expected %d args, but got %d", "fuel", nargs, lua_gettop(L));
  Unit* u = CheckObject<Unit>(L, 1);
  if (!u->motion.bp || u->motion.bp->motionType == kMotionNone) luaL_error(L, "Unit has not motion object");
  return u;
}
int l_GetFuelRatio(lua_State* L) {
  lua_pushnumber(L, FuelUnit(L, 1)->fuelRatio);
  return 1;
}
int l_SetFuelRatio(lua_State* L) {
  Unit* u = FuelUnit(L, 2);
  if (lua_type(L, 2) != LUA_TNUMBER) luaL_typerror(L, 2, "number");
  u->fuelRatio = static_cast<float>(lua_tonumber(L, 2));
  return 0;
}
int l_GetFuelUseTime(lua_State* L) {
  lua_pushnumber(L, FuelUnit(L, 1)->motion.fuelUseTime);
  return 1;
}
int l_SetFuelUseTime(lua_State* L) {
  Unit* u = FuelUnit(L, 2);
  if (lua_type(L, 2) != LUA_TNUMBER) luaL_typerror(L, 2, "number");
  u->motion.fuelUseTime = static_cast<float>(lua_tonumber(L, 2));
  return 0;
}
}  // namespace

void RegisterAirBindings(lua_State* L) {
  SetMethod(L, "Unit", "GetFuelRatio", l_GetFuelRatio);
  SetMethod(L, "Unit", "SetFuelRatio", l_SetFuelRatio);
  SetMethod(L, "Unit", "GetFuelUseTime", l_GetFuelUseTime);
  SetMethod(L, "Unit", "SetFuelUseTime", l_SetFuelUseTime);
  SetMethod(L, "Unit", "SetElevation", l_SetElevation);
  SetMethod(L, "Unit", "RevertElevation", l_RevertElevation);
}

void AirForget(Sim& sim) { sim.airState.reset(); }

}  // namespace moho
