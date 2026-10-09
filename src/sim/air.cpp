// Aircraft flight model (see air.h for what the original does). The arithmetic follows the
// original's operation order (float32, with the x87 parts evaluated in double), because the
// trajectory is a feedback loop: a last-bit difference grows. Function names and addresses refer
// to the FA exe; the specs in engine-ref/specs/air_*.md give the details.
#include "core/dmath.h"
#include "sim/air.h"
#include "sim/transport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#include "core/log.h"
#include "sim/commands.h"
#include "sim/economy.h"
#include "sim/combat.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace combat {
bool HasTarget(Sim& sim, const AiTarget& t);
Vec3 TargetPos(Sim& sim, const AiTarget& t, bool centre);
Entity* TargetEntity(Sim& sim, const AiTarget& t);
}  // namespace combat

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int kLand = 1, kSeabed = 2, kSub = 4, kWater = 8, kAir = 0x10;
constexpr float kGravityY = -4.9f;

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

// 0x452af0: normalise in place (zero if the length is <= 1e-6); returns the length
double NormalizeInPlace(Vec3& v) {
  float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
  double n = std::sqrt(static_cast<double>(s));
  if (n > static_cast<double>(9.99999997e-07f)) {
    float inv = 1.0f / static_cast<float>(n);
    v.x *= inv;
    v.y *= inv;
    v.z *= inv;
  } else {
    v = {};
  }
  return n;
}

// func_NormalizeQuatInPlace 0x4edaa0
void NormalizeQuat(QuatW& q) {
  float s = ((q.w * q.w + q.x * q.x) + q.y * q.y) + q.z * q.z;
  double n = std::sqrt(static_cast<double>(s));
  if (n > static_cast<double>(9.99999997e-07f)) {
    float inv = 1.0f / static_cast<float>(n);
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
  double s = static_cast<double>(L) / std::sqrt(static_cast<double>(n2));
  v.x = static_cast<float>(v.x * s);
  v.y = static_cast<float>(s * v.y);
  v.z = static_cast<float>(s * v.z);
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
    double inv = 1.0 / std::sqrt(static_cast<double>(s2));
    *axis = {static_cast<float>(q.x * inv), static_cast<float>(inv * q.y), static_cast<float>(inv * q.z)};
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
  double a = map->HeightAt(ix, iz);
  a = (static_cast<double>(map->HeightAt(ix, iz + 1)) - a) * fz + a;
  double b = map->HeightAt(ix + 1, iz);
  b = (static_cast<double>(map->HeightAt(ix + 1, iz + 1)) - b) * fz + b;
  return static_cast<float>((b - a) * fx + a);
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
std::map<const TerrainMap*, Tiers>& TierCache() {
  static std::map<const TerrainMap*, Tiers> c;
  return c;
}
int BitLen(unsigned v) {
  int n = 0;
  while (v) {
    ++n;
    v >>= 1;
  }
  return n;
}
const Tiers& GetTiers(const TerrainMap* map) {
  auto& c = TierCache();
  auto it = c.find(map);
  if (it != c.end()) return it->second;
  Tiers& t = c[map];
  t.map = map;
  int w = map->width(), h = map->height();
  t.W = w + 1;
  t.H = h + 1;
  int n = BitLen(static_cast<unsigned>(std::max(w, h) - 1));
  auto raw = [&](int x, int z) {
    x = std::clamp(x, 0, t.W - 1);
    z = std::clamp(z, 0, t.H - 1);
    return static_cast<uint32_t>(std::lround(map->HeightAt(x, z) * 128.0f));
  };
  for (int k = 1; k <= n; ++k) {
    int tw = std::max(1, w >> k), th = std::max(1, h >> k);
    t.tw.push_back(tw);
    t.th.push_back(th);
    std::vector<uint32_t> v(static_cast<size_t>(tw) * th, 0);
    int s = 1 << k;
    for (int j = 0; j < th; ++j)
      for (int i = 0; i < tw; ++i) {
        uint32_t m = 0;
        int x0 = i * s, z0 = j * s;
        int x1 = std::min(x0 + s, t.W - 1), z1 = std::min(z0 + s, t.H - 1);
        if (k == 1 || t.hi.empty()) {
          for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) m = std::max(m, raw(x, z));
        } else {
          const auto& p = t.hi.back();
          int pw = t.tw[k - 2], ph = t.th[k - 2];
          for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx) {
              int ci = std::min(2 * i + dx, pw - 1), cj = std::min(2 * j + dz, ph - 1);
              m = std::max(m, p[static_cast<size_t>(cj) * pw + ci]);
            }
        }
        v[static_cast<size_t>(j) * tw + i] = m;
      }
    t.hi.push_back(std::move(v));
  }
  return t;
}

// STIMap::LookAheadForMaxTerrain 0x62d620
float LookAheadForMaxTerrain(const TerrainMap* map, const Vec3& pos, float radius, bool terrainOnly) {
  if (!map) return 0;
  if (1.0f > radius) {
    float h = Elevation(map, pos.x, pos.z);
    if (!terrainOnly && map->hasWater && map->waterElevation > h) h = map->waterElevation;
    return h;
  }
  const Tiers& t = GetTiers(map);
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
void SetState(Unit* u, const char* s, bool on) {
  if (on) u->unitStates.insert(s);
  else u->unitStates.erase(s);
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
}
void SetMotionState(Sim& sim, Unit* u, int s) {
  AirMotion& a = A(u);
  if (a.motionState == s) return;
  int old = a.motionState;
  a.motionState = s;
  Callback2(sim, u, "OnMotionStateChange", kStateNames[s], kStateNames[old]);
}

bool InCategory(Sim& sim, Unit* u, const char* cat) { return BpInCategory(sim, u->blueprint, cat); }

// Unit::UpdateInfoCache: the speed cap (u+0x594)
float MaxSpeed(Unit* u) {
  const AirBp& b = *A(u).bp;
  float ms = b.maxAirspeed * u->motion.speedMult / 1.0f;
  if (u->motion.speedCap > 0) ms = std::min(ms, u->motion.speedCap * u->motion.speedMult);
  return ms;
}

// CUnitMotion::GetElevation 0x6bc8e0
float GetElevationWanted(const AirMotion& a) { return a.elevationAttr + a.randElev; }

// CUnitMotion::ShouldHoverInsteadOfLand 0x6bc820
bool ShouldHover(Unit* u) {
  const AirBp& b = *A(u).bp;
  return b.transportHoverHeight > 0 && (State(u, "TransportLoading") || TransportHasCargo(u));
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

// Unit::UpdateSpeedThroughStatus 0x6ac940
void UpdateSpeedThrough(Unit* u) {
  AirMotion& a = A(u);
  bool v;
  if (State(u, "Refueling")) v = false;
  else if (State(u, "ForceSpeedThrough") || State(u, "CannotFindPlaceToLand")) v = true;
  else if (State(u, "Guarding") && !State(u, "Ferrying") && !a.bp->experimental) v = true;
  else {
    const UnitCommand* cur = u->commands.empty() ? nullptr : u->commands[0].get();
    const UnitCommand* next = u->commands.size() > 1 ? u->commands[1].get() : nullptr;
    v = cur && next && IsMoveLikeType(cur->type) && IsMoveLikeType(next->type);
  }
  a.fullSpeed = v;
}

// --------------------------------------------------------------------------------------------
// Landing spots (Unit::PrepareMove for flyers; predicates of air_extra.md 3)

struct Reservations {
  int w = 0, h = 0;
  std::vector<uint8_t> bits;
};
std::map<const Sim*, Reservations>& ResMap() {
  static std::map<const Sim*, Reservations> m;
  return m;
}
Reservations& Res(Sim& sim) {
  Reservations& r = ResMap()[&sim];
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
void FreeLandRect(Sim& sim, Unit* u) {
  AirMotion& a = A(u);
  if (!RectEmpty(a.landRect)) FillRes(sim, a.landRect, 0);
  std::fill(a.landRect, a.landRect + 4, 0);
}
void ReserveLandRect(Sim& sim, Unit* u, const int* r) {
  FreeLandRect(sim, u);
  AirMotion& a = A(u);
  std::copy(r, r + 4, a.landRect);
  FillRes(sim, r, 1);
}
bool CanReserve(Sim& sim, Unit* u, const int* r) {
  AirMotion& a = A(u);
  bool own = !RectEmpty(a.landRect);
  if (own) FillRes(sim, a.landRect, 0);
  bool blocked = AnyRes(sim, r);
  if (own) FillRes(sim, a.landRect, 1);
  return !blocked;
}

bool IsWithin(Sim& sim, const Vec3& p, float m) {
  const TerrainMap* map = sim.map();
  if (!map) return true;
  // the playable rect = the whole map (ScenarioInfo areas not modelled)
  int W = map->width() + 1, H = map->height() + 1;
  return 0 <= p.x - m && 0 <= p.z - m && p.x + m < W - 1 && p.z + m < H - 1;
}

// COORDS_CanMoveAt 0x720f70 with strict = 1 (air_extra.md 3.6): no live, completed, non-air unit
// other than u whose box touches [x0, x1] x [z0, z1]; carried units never block. A transport (u has
// a transport object) ignores moving non-transport units, and all non-transport units while it is
// TransportLoading; other transports always block (strict).
bool NoBlockingUnits(Sim& sim, Unit* u, float x0, float z0, float x1, float z1) {
  bool blocked = false;
  const bool selfTransport = u->transport != nullptr;
  const bool loading = selfTransport && State(u, "TransportLoading");
  sim.ForUnitsInRect(x0 - 8, z0 - 8, x1 + 8, z1 + 8, [&](Unit* o) {
    if (blocked || o == u || o->dead || o->destroyQueued || o->fractionComplete < 1.0f) return;
    if (o->layer == "Air") return;
    if (u->layer == "Sub" && o->layer != "Sub") return;
    if (!o->transport || InCategory(sim, o, "PODSTAGINGPLATFORM")) {
      const Vec3& v = o->motion.lastMove;
      bool moving = v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
      if (selfTransport && (moving || loading)) return;
    }
    if (o->parentId || State(o, "Attached")) return;
    const MotionBlueprint* ob = o->motion.bp;
    float hx = (ob ? ob->sizeX : 1) * 0.5f, hz = (ob ? ob->sizeZ : 1) * 0.5f;
    if (o->position.x + hx < x0 || o->position.x - hx > x1 || o->position.z + hz < z0 || o->position.z - hz > z1)
      return;
    blocked = true;
  });
  return !blocked;
}

// One PrepareMove candidate (air_extra.md 3.1): bounds, terrain for the modified footprint fp'
// (S x S, Land (+Water), the blueprint's MaxSlope / MinWaterDepth, MaxWaterDepth 0), structures,
// other units' o-grid reservations, blocking units.
bool CanLandAt(Sim& sim, Unit* u, int cx, int cz, const Vec3& w, int S, int caps) {
  if (!IsWithin(sim, w, static_cast<float>(S))) return false;
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
  const PathGrid* g = sim.navigation().Grid(fp);
  int c = g ? g->Caps(cx, cz) : 0;
  if (u->layer == "Water") c &= ~kSub;
  if ((c & 3) && !(fp.flags & 1) && sim.navigation().AnyStructureIn(cx, cz, cx + S, cz + S)) c &= ~3;
  if (!c) return false;
  int r[4] = {cx, cz, cx + S, cz + S};
  if (!CanReserve(sim, u, r)) return false;
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  return NoBlockingUnits(sim, u, x0, z0, x0 + S, z0 + S);
}

// Unit::PrepareMove 0x62b780 (spacing 0, no exclusion rect)
bool PrepareMove(Sim& sim, Unit* u, Vec3* pos) {
  const AirBp& b = *A(u).bp;
  int S = std::max(b.footprintX, b.footprintZ);
  int caps = kLand;
  const TerrainMap* map = sim.map();
  if (InCategory(sim, u, "CANLANDONWATER") && Elevation(map, pos->x, pos->z) < WaterOrNone(map)) caps |= kWater;
  int cx = static_cast<int>(std::nearbyint(pos->x - S * 0.5f)), cz = static_cast<int>(std::nearbyint(pos->z - S * 0.5f));
  if (CanLandAt(sim, u, cx, cz, *pos, S, caps)) return true;
  int step = 2 * S;
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
        if (CanLandAt(sim, u, x, z, w, S, caps)) {
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
bool GroundSpotOk(Sim& sim, Unit* u, const NamedFootprint& fp, int cx, int cz, const Vec3& w, const float* excl) {
  if (excl && excl[2] > excl[0] && excl[3] > excl[1] && cx < excl[2] && excl[0] < cx + 1 && cz < excl[3] &&
      excl[1] < cz + 1)
    return false;
  int S = std::max(fp.sizeX, fp.sizeZ);
  if (!IsWithin(sim, w, static_cast<float>(S))) return false;
  const PathGrid* g = sim.navigation().Grid(fp);
  int c = g ? g->Caps(cx, cz) : 0;
  if (u->layer == "Water") c &= ~kSub;
  if ((c & 3) && !(fp.flags & 1) && sim.navigation().AnyStructureIn(cx, cz, cx + fp.sizeX, cz + fp.sizeZ)) c &= ~3;
  if (!c) return false;
  int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
  if (!GroundCanReserve(sim, u, r)) return false;
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  return NoBlockingUnits(sim, u, x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ);
}
}  // namespace

bool GroundPrepareMoveImpl(Sim& sim, Unit* u, Vec3* pos, const float excl[4]) {
  const NamedFootprint& fp = u->motion.bp->footprint;
  int cx = static_cast<int>(std::nearbyint(pos->x - fp.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(pos->z - fp.sizeZ * 0.5f));
  if (GroundSpotOk(sim, u, fp, cx, cz, *pos, excl)) return true;
  int step = 2 * std::max(fp.sizeX, fp.sizeZ);
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
    p.x = std::min(std::max(p.x, 0.0f), static_cast<float>(map->width()));
    p.z = std::min(std::max(p.z, 0.0f), static_cast<float>(map->height()));
  }
  a.target = p;
  if (layer != 0) a.landLayer = layer;
  if (u->commands.empty()) a.idleTick = sim.tick();
  SetState(u, "MovingUp", false);
  SetState(u, "MovingDown", false);
  Vec3 d = dir;
  if (!IsZeroBits(d) && NormalizeInPlace(d) != 0.0) {
    a.facing = d;
  } else {
    float hx = orig.x - u->position.x, hz = orig.z - u->position.z;
    float dist = std::sqrt(hx * hx + hz * hz);
    if (dist > b.startTurnDistance) a.facing = {hx / dist, 0, hz / dist};
  }
  FreeLandRect(sim, u);
}

// CUnitMotion::Stop 0x6b8460
void Stop(Sim& sim, Unit* u, const Vec3* pos) {
  AirMotion& a = A(u);
  a.stopped = true;
  if (!State(u, "TransportUnloading") && !State(u, "TransportLoading") && !State(u, "Ferrying")) a.landLayer = kAir;
  a.target = pos ? *pos : u->position;
  (void)sim;
}

// ---------------------------------------------------------------------------------------------
// Orientation and control

struct Ctrl {
  Vec3 force, torque;
};

// CUnitMotion::CalcWingedLift 0x6bc950
float WingedLift(const AirMotion& a, float desiredY, float upY) {
  float base = a.elevationAttr + a.randElev;
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
  bool guard = State(u, "Guarding");
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
    dv.y = WingedLift(a, desired.y, upY);
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
  double diff = static_cast<double>(a1) - dmath::Atan2d(f.x, f.z);
  float d;
  if (diff > static_cast<double>(3.14159274f)) {
    d = static_cast<float>(diff) - 6.28318548f;
  } else {
    d = static_cast<float>(diff);
    if (-3.14159274f > d) d = d + 6.28318548f;
  }
  float maxStep = ((cs == 3) ? b.combatTurnSpeed : b.turnSpeed) * 0.1f;
  float step = (maxStep > d) ? d : maxStep;
  if (-maxStep > step) step = -maxStep;
  float half = static_cast<float>(static_cast<double>(step) * 10.0 * 0.5);
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
  float altF = State(u, "MovingDown") ? a.height / b.elevation : 1.0f;
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

// RandRange of the circling parameters: lo + r * (hi - lo) * 2^-32 in double, one rounding
float RandRange(Sim& sim, float lo, float hi) {
  uint32_t r = sim.NextUInt32();
  return static_cast<float>(static_cast<double>(lo) +
                            (static_cast<double>(r) * (static_cast<double>(hi) - static_cast<double>(lo))) *
                                2.3283064365386963e-10);
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
                    static_cast<uint32_t>(loT) + tick;
  }
  float cx = a.target.x, cz = a.target.z;
  float R = b.startTurnDistance * a.circleRatio;
  Entity* focus = u->focusId ? sim.FindEntity(u->focusId) : nullptr;
  if (focus && (State(u, "Building") || State(u, "Repairing"))) {
    cx = focus->position.x;
    cz = focus->position.z;
  } else if (combat::HasTarget(sim, target)) {
    Vec3 tp = combat::TargetPos(sim, target, false);
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
float DampingFactor(Unit* u, const Vec3& d) {
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
  float invL = 1.0f / 1.0f;
  float kTurn = b.kTurn * invL, kRoll = b.kRoll * invL, kLift = b.kLift * invL;
  Vec3 dv = desired;
  Axes axes;
  int mode;  // 0 winged, 1 circling, 2 hover
  if (b.winged && a.carrierEvent != 2) {
    mode = 0;
  } else if (State(u, "Guarding") && !(State(u, "Moving") || State(u, "Ferrying") || State(u, "Attacking") ||
                                      State(u, "Building") || State(u, "Repairing"))) {
    mode = 0;
  } else if (b.hoverOverAttack) {
    mode = 2;
  } else if (u->focusId && sim.FindEntity(u->focusId) &&
             (State(u, "Building") || State(u, "Repairing") || State(u, "Reclaiming") || State(u, "Capturing"))) {
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
  float dampF = DampingFactor(u, desired);
  Vec3 F;
  F.x = dv.x * b.kMove + nv.x * dampF;
  F.y = b.kLiftDamping * nv.y + dv.y * kLift;
  F.z = dv.z * b.kMove + nv.z * dampF;
  Vec3 T;
  T.x = H.x * b.kTurnDamping + err.x * kTurn;
  T.y = H.y * b.kTurnDamping + err.y * kTurn;
  T.z = b.kRollDamping * H.z + err.z * kRoll;
  Ctrl c;
  float m = pb.mass;
  c.force = {(F.x - 0.0f) * m, (F.y - kGravityY) * m, (F.z - 0.0f) * m};
  c.torque = MultQuadVec(q, Vec3{T.x / pb.invI[0], T.y / pb.invI[1], T.z / pb.invI[2]});
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
    float tgtY = tu->motion.air ? h + tu->motion.air->bp->elevation : combat::TargetPos(sim, tgt, false).y;
    float want = b.elevation * 0.5f + h;
    return std::max(want, tgtY) - u->position.y;
  }
  if (a.combatState == 1) return (h + b.attackElevation) - u->position.y;
  return (h + b.elevation) - u->position.y;
}

bool MovedThisTick(const Entity* e) {
  if (e->kind != Entity::Kind::Unit) return false;
  const Unit* t = static_cast<const Unit*>(e);
  return !IsZeroBits(t->motion.lastMove);
}

// CUnitMotion::ComputeAirCombatTactics 0x6bcdb0
void CombatTactics(Sim& sim, Unit* u, const AiTarget& tgt, Vec3& out, const Vec3& fwd) {
  AirMotion& a = A(u);
  const AirBp& b = *a.bp;
  uint32_t now = sim.tick();
  float speed = u->motion.speedMult * b.maxAirspeed;
  Vec3 pos = u->position;
  Vec3 tp = combat::TargetPos(sim, tgt, false);
  Vec3 diff{tp.x - pos.x, tp.y - pos.y, tp.z - pos.z};
  float d3 = std::sqrt((diff.x * diff.x + diff.y * diff.y) + diff.z * diff.z);
  float d2 = std::sqrt(diff.x * diff.x + diff.z * diff.z);
  Vec3 dir3 = Normalized(diff);
  float trig = 1.0f * b.breakOffTrigger;
  float brk = 1.0f * b.breakOffDistance;
  int s = a.combatState;
  Entity* E = combat::TargetEntity(sim, tgt);
  bool eAir = E && E->kind == Entity::Kind::Unit && static_cast<Unit*>(E)->layer == "Air";
  auto output = [&]() {
    a.combatState = s;
    auto elev = [&](const Vec3& v) { return DesiredTargetElevation(sim, u, tgt, v); };
    switch (s) {
      case 1:
      case 2: {
        SetState(u, "MakingAttackRun", true);
        if (E && tgt.mobile) {
          float t = 1.0f;
          if (b.predictAheadForBombDrop > 0 && !eAir) t = b.predictAheadForBombDrop;
          Vec3 p = E->kind == Entity::Kind::Unit ? PredictAhead(sim, static_cast<Unit*>(E), t) : Vec3{};
          out = {p.x - pos.x, p.y - pos.y, p.z - pos.z};
        } else {
          out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        }
        out.y = elev(out);
        if (s == 2 && eAir && MovedThisTick(E)) SetLength(out, std::max(d3, b.minAirspeed));
        else SetLength(out, speed);
        break;
      }
      case 3:
      case 4:
        ++a.combatCounter;
        out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        out.y = elev(out);
        SetLength(out, b.minAirspeed);
        break;
      case 5:
        ++a.combatCounter;
        out = {dir3.x * d3, dir3.y * d3, dir3.z * d3};
        out.y = elev(out);
        SetLength(out, speed);
        break;
      case 6:
        SetState(u, "MakingAttackRun", true);
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
  if (s == 7 && !IsWithin(sim, pos, 5.0f)) return output();
  Vec3 h = Normalized(Vec3{dir3.x, 0, dir3.z});
  float dotF = (h.x * fwd.x + h.y * fwd.y) + h.z * fwd.z;
  bool facing = (s == 3) ? dotF > 0.0f : dotF > 0.866f;
  if (eAir && !IsWithin(sim, pos, 0.0f)) {
    s = 7;
    return output();
  }
  bool breakOff = (s == 1 && trig > d3) || (b.breakOffIfNearNewTarget && s == 0 && brk > d2) ||
                  (s == 2 && dotF < 0.0f) ||
                  (a.combatCounter > EngineFloor(b.sustainedTurnThreshold * 10.0f));
  if (!breakOff) {
    if (a.combatTimer >= now) {
      if (!facing || s == 6) return output();
    }
    if (facing) {
      s = 1;
      if (eAir && MovedThisTick(E)) {
        QuatW q = ToW(E->orientation);
        Vec3 Fe{2 * (q.w * q.y + q.z * q.x), 2 * (q.z * q.y - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
        if ((Fe.x * fwd.x + Fe.y * fwd.y) + Fe.z * fwd.z > 0.0f) s = 2;
      }
      return output();
    }
    if (s == 2 && (dir3.x * fwd.x + dir3.y * fwd.y) + dir3.z * fwd.z >= 0.0f) return output();
    uint32_t r1 = sim.NextUInt32();
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
  NormalizeQuat(b.q);
}

// CUnitMotion::CalcMoveAir 0x6bee50
void CalcMoveAir(Sim& sim, Unit* u, Transform& T) {
  AirMotion& a = A(u);
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
  Vec3 vel = u->motion.lastMove;
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
      r[i] = static_cast<float>(static_cast<double>(x) * (static_cast<double>(ai) - static_cast<double>(-ai)) *
                                    2.3283064365386963e-10 +
                                static_cast<double>(-ai));
    }
    a.spinTorque = MultQuadVec(body.q, Vec3{r[0] / body.invI[0], r[1] / body.invI[1], r[2] / body.invI[2]});
  } else {
    Vec3 facing;
    if (distH > b.startTurnDistance && a.carrierEvent != 2) {
      facing = d;
      a.elevOffset = GetElevationWanted(a);
      SetState(u, "MovingUp", false);
      SetState(u, "MovingDown", false);
      if (State(u, "CannotFindPlaceToLand")) {
        SetState(u, "CannotFindPlaceToLand", false);
        UpdateSpeedThrough(u);
      }
    } else {
      landing = a.landLayer != 0 && a.landLayer != kAir;
      int n = EngineFloor(b.autoLandTime * 10.0f);
      if (!landing && a.idleTick > 0 && n > 0 &&
          static_cast<int>(sim.tick()) > static_cast<int>(a.idleTick) + n) {
        if (a.landHeight != kInf) {
          if (State(u, "CannotFindPlaceToLand")) {
            SetState(u, "CannotFindPlaceToLand", false);
            UpdateSpeedThrough(u);
          }
          landing = true;
          a.landLayer = kLand;
        } else {
          Vec3 spot = a.target;
          bool ok = PrepareMove(sim, u, &spot);
          if (!ok) {
            a.idleTick = sim.tick();
            SetState(u, "CannotFindPlaceToLand", true);
            UpdateSpeedThrough(u);
          } else {
            a.target.x = spot.x;
            a.target.z = spot.z;
            if (State(u, "CannotFindPlaceToLand")) {
              SetState(u, "CannotFindPlaceToLand", false);
              UpdateSpeedThrough(u);
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
      // desired facing
      if (!IsZeroBits(a.facing)) facing = Normalized(a.facing);
      else facing = fwdH;
      // target elevation offset
      if (landing) {
        SetState(u, "MovingDown", true);
        if (a.landHeight != kInf) a.elevOffset = a.landHeight - surface;
        else if (ShouldHover(u) || a.vertEvent == 4) a.elevOffset = b.transportHoverHeight;
        else if (distH < 0.5f || a.vertEvent == 1) a.elevOffset = 0;
        else a.elevOffset = GetElevationWanted(a) * 0.5f;
      } else {
        a.elevOffset = GetElevationWanted(a);
        SetState(u, "MovingUp", false);
      }
    }

    // terrain look-ahead, terrain height smoothing, vertical error
    float look = u->motion.speedMult * b.maxAirspeed * 5.0f;
    if (distH < look) look = distH;
    float maxT = LookAheadForMaxTerrain(map, T.pos, look * 1.0f, b.flyInWater);
    float climb = maxT - T.pos.y;
    if (climb < 0.0f) climb = 0.0f;
    if (b.liftFactor < climb && 1.0f < look) {
      look = look * 0.5f;
      float c2 = static_cast<float>(LookAheadForMaxTerrain(map, T.pos, look * 1.0f, b.flyInWater) * 1.5) - T.pos.y;
      if (c2 < 0.0f) c2 = 0.0f;
      float f = (look - c2) / look;
      if (0.2f >= f) f = 0.2f;
      d.x = d.x * f * f;
      d.z = d.z * f * f;
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
          FreeLandRect(sim, u);
          SetLayerBits(sim, u, a.landLayer);
          SetState(u, "MovingUp", false);
          SetState(u, "MovingDown", false);
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
        if (a.vertEvent == 1 || a.vertEvent == 4) SetState(u, "MovingUp", true);
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
    SetState(u, "MakingAttackRun", false);
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
    if (!has) {
      a.combatCounter = 0;
      a.combatState = 0;
    }
    Vec3 dir = Normalized(d);
    ctrl = ComputeAirControl(sim, u, T.q, d, dir, facing, fwdH, target);

    // events
    if (State(u, "MovingDown")) {
      SetVertEvent(sim, u, 3);
    } else if (State(u, "MovingUp")) {
      SetVertEvent(sim, u, 2);
    } else if (a.vertEvent != 4) {
      SetVertEvent(sim, u, 0);
    }
    int horz;
    if (distH > b.startTurnDistance || a.fullSpeed) horz = hspeed > maxSpeed * 0.08f ? 1 : 0;
    else if (a.combatState != 0) horz = 0;
    else horz = maxSpeed * 0.005f > hspeed ? 3 : 2;
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
// Falling (CUnitMotion::CalcMoveBallistic 0x6c0290; the terrain test is approximated by a fine
// march along the segment against the bilinear surface - TODO: the original's triangle walk)

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
  if (!IsZeroBits(a.spinTorque)) {
    BodyLinear(body, Vec3{}, 0.1f);
    BodyAngular(body, a.spinTorque, 0.1f);
    T.q = body.q;
  }
  float water = WaterOrNone(map);
  bool amph = u->motion.bp && u->motion.bp->motionType == kMotionAmphibious;
  // segment against the terrain (and the water plane)
  bool hit = false;
  float tHit = 1;
  const int N = 16;
  float prevDiff = P0.y - (amph || !map || !map->hasWater ? Elevation(map, P0.x, P0.z)
                                                          : std::max(Elevation(map, P0.x, P0.z), water));
  for (int i = 1; i <= N && !hit; ++i) {
    float t = static_cast<float>(i) / N;
    Vec3 p{P0.x + v2.x * t, P0.y + v2.y * t, P0.z + v2.z * t};
    float s = Elevation(map, p.x, p.z);
    if (!amph && map && map->hasWater) s = std::max(s, water);
    float dfy = p.y - s;
    if (dfy <= 0) {
      float f = (prevDiff > 0 && prevDiff != dfy) ? prevDiff / (prevDiff - dfy) : 0;
      tHit = (static_cast<float>(i - 1) + f) / N;
      hit = true;
    }
    prevDiff = dfy;
  }
  int oldLayer = LayerBits(u->layer);
  if (hit) {
    P1 = {P0.x + v2.x * tHit, P0.y + v2.y * tHit, P0.z + v2.z * tHit};
    *scale = tHit;
    float terr = Elevation(map, P1.x, P1.z);
    if (water < terr) SetLayerBits(sim, u, kLand);
    else if (amph) SetLayerBits(sim, u, kSeabed);
    else SetLayerBits(sim, u, kWater);
  }
  if (LayerBits(u->layer) != oldLayer) {
    if (u->dead) {
      lua_State* L = sim.L();
      lua_pushstring(L, u->layer == "Land" ? "Terrain" : "Water");
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
  static std::unordered_map<const BlueprintInfo*, AirBp> cache;
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
  return cache.emplace(&bp, b).first->second;
}

float AirSpawnHeight(const Sim& sim, const BlueprintInfo& bp, lua_State* L, float x, float z, float y) {
  const AirBp& b = GetAirBp(L, bp);
  if (!b.canFly) return y;
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
}

void AirSetGoal(Sim& sim, Unit* u, Vec3 goal, uint32_t tick, int layer, bool landingSpot) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  // the goal cell's centre (SNavGoal of the command position; CellToWorld)
  const AirBp& b = *a.bp;
  a.pendingLayer = layer;
  a.pendingFacing = {};
  if (layer == kLand && b.canFly && landingSpot) {  // a landing move: a free spot (NewMoveTask ctor step 4)
    int cx = static_cast<int>(std::nearbyint(goal.x - b.footprintX * 0.5f));
    int cz = static_cast<int>(std::nearbyint(goal.z - b.footprintZ * 0.5f));
    goal = {cx + b.footprintX * 0.5f, goal.y, cz + b.footprintZ * 0.5f};
    PrepareMove(sim, u, &goal);
  }
  int cx = static_cast<int>(std::nearbyint(goal.x - b.footprintX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(goal.z - b.footprintZ * 0.5f));
  a.pending = true;
  a.pendingTick = tick;
  a.pendingGoal = {cx + b.footprintX * 0.5f, goal.y, cz + b.footprintZ * 0.5f};
  u->motion.hasGoal = true;
  u->motion.arrived = false;
  (void)sim;
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

void AirSetTargetNow(Sim& sim, Unit* u, Vec3 p, int layer) {
  if (!u->motion.air) return;
  SetTarget(sim, u, p, Vec3{}, layer);
}

bool GroundPrepareMove(Sim& sim, Unit* u, Vec3* pos, const float excl[4]) { return GroundPrepareMoveImpl(sim, u, pos, excl); }
void GroundReserveRect(Sim& sim, Unit* u, const int r[4]) { GroundReserveRectImpl(sim, u, r); }
void GroundFreeRect(Sim& sim, Unit* u) { GroundFreeRectImpl(sim, u); }

bool AirPrepareMove(Sim& sim, Unit* u, Vec3* pos) {
  if (!u->motion.air) return false;
  return PrepareMove(sim, u, pos);
}

void AirAbort(Sim& sim, Unit* u) {
  if (!u->motion.air) return;
  AirMotion& a = A(u);
  a.pending = false;
  Vec3 p = PredictAhead(sim, u, 1.0f);
  Stop(sim, u, &p);
  a.steering = false;
  a.fullSpeed = false;
  u->motion.hasGoal = false;
}

Vec3 PredictAhead(Sim& sim, Unit* u, float t) {
  (void)sim;
  Vec3 p = u->position;
  if (!u->motion.air) return p;
  const PhysBody& pb = A(u).body;
  // world angular velocity: R * (invI (.) R^T L)
  QuatW conj{pb.q.w, -pb.q.x, -pb.q.y, -pb.q.z};
  Vec3 lb = MultQuadVec(conj, pb.L);
  Vec3 wb{lb.x * pb.invI[0], lb.y * pb.invI[1], lb.z * pb.invI[2]};
  Vec3 w = MultQuadVec(pb.q, wb);
  Vec3 v = u->motion.lastMove;
  float ang = w.y * 0.1f;
  float s = dmath::Sin(ang * 0.5f), c = dmath::Cos(ang * 0.5f);
  QuatW q{c, 0, s, 0};
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

void AirMotionTick(Sim& sim, Unit* u) {
  UnitMotion& m = u->motion;
  if (!m.air) AirInit(sim, u);
  AirMotion& a = A(u);
  if (u->fractionComplete < 1.0f) return;
  // the navigator's SetGoal (the command task that started this tick hands it over after motion)
  if (a.pending && sim.tick() >= a.pendingTick) {
    a.pending = false;
    a.goal = a.pendingGoal;
    a.steering = true;
    SetTarget(sim, u, a.goal, a.pendingFacing, a.pendingLayer ? a.pendingLayer : kAir);
  }
  UpdateSpeedThrough(u);
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
  // CAiNavigatorAir::Execute: arrival (AtTarget + goal cell)
  if (a.steering && !a.pending) {
    float dx = a.target.x - u->position.x, dz = a.target.z - u->position.z;
    float d = std::sqrt(dx * dx + dz * dz);
    float r = 0.25f;
    if (a.fullSpeed) {
      float v = a.bp->maxAirspeed * m.speedMult;
      r = a.bp->winged ? std::max(0.25f, v) : std::max(0.25f, 0.25f * v);
    }
    bool layerOk = a.landLayer == 0 || LayerBits(u->layer) == a.landLayer;
    bool at = layerOk && (d <= r || a.vertEvent == 4 || (a.vertEvent == 1 && a.landHeight != kInf));
    if (at) {
      a.steering = false;
      m.hasGoal = false;
      m.arrived = true;
    }
  }
}

namespace {
int l_SetElevation(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (u && u->motion.air) u->motion.air->elevationAttr = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_RevertElevation(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  if (u && u->motion.air) u->motion.air->elevationAttr = u->motion.air->bp->elevation;
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
  static std::map<const BlueprintInfo*, PlatformBp> cache;
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
    if (m.refuelFlag && old > 0.99f && !(u->health < u->maxHealth)) ClearRepairRequest(u);
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

}  // namespace moho
