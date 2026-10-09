// Vector and quaternion helpers for the combat code (y up; heading 0 faces +z; quaternions
// stored x, y, z, w as in sim/entity.h).
#pragma once
#include "core/dmath.h"
#include <cmath>
#include <limits>

#include "sim/entity.h"
#include "sim/skeleton.h"

namespace moho::vm {

inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len2(Vec3 a) { return Dot(a, a); }
inline float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline float Dist(Vec3 a, Vec3 b) { return Len(Sub(a, b)); }
inline float Dist2XZ(Vec3 a, Vec3 b) {
  float dx = a.x - b.x, dz = a.z - b.z;
  return dx * dx + dz * dz;
}
inline float DistXZ(Vec3 a, Vec3 b) { return std::sqrt(Dist2XZ(a, b)); }
inline Vec3 Norm(Vec3 a) {
  float l = Len(a);
  return l > 0 ? Mul(a, 1.0f / l) : a;
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return Add(a, Mul(Sub(b, a), t)); }
inline bool IsNaN(Vec3 a) { return std::isnan(a.x) || std::isnan(a.y) || std::isnan(a.z); }
inline Vec3 NaNVec() {
  float n = std::numeric_limits<float>::quiet_NaN();
  return {n, n, n};
}

inline Quat Conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat QMul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
inline Vec3 Forward(const Quat& q) {  // local +z in the world
  return {2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
}
inline Quat AxisAngle(Vec3 axis, float a) {
  float s = dmath::Sin(a * 0.5f);
  return {axis.x * s, axis.y * s, axis.z * s, dmath::Cos(a * 0.5f)};
}
inline Quat QNorm(Quat q) {
  float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l <= 0) return Quat{};
  return {q.x / l, q.y / l, q.z / l, q.w / l};
}
// Look along `dir` with the world up (COORDS_Orient): identity for a zero vector.
inline Quat Orient(Vec3 dir) {
  float l = Len(dir);
  if (l <= 0) return Quat{};
  Vec3 f = Mul(dir, 1.0f / l);
  float yaw = dmath::Atan2(f.x, f.z);
  float pitch = dmath::Asin(std::fmax(-1.0f, std::fmin(1.0f, f.y)));
  // rotation = yaw about +y, then pitch about local +x (negative pitch tips +z up)
  return QMul(AxisAngle({0, 1, 0}, yaw), AxisAngle({1, 0, 0}, -pitch));
}
// Shortest-arc rotation turning q's forward toward dir by at most maxAngle (radians).
inline Quat RotateToward(const Quat& q, Vec3 dir, float maxAngle) {
  Vec3 f = Forward(q);
  Vec3 d = Norm(dir);
  if (Len2(d) <= 0) return q;
  float c = std::fmax(-1.0f, std::fmin(1.0f, Dot(f, d)));
  float ang = dmath::Acos(c);
  if (ang <= 1e-6f) return q;
  Vec3 axis = Cross(f, d);
  if (Len2(axis) < 1e-12f) {  // opposite: any perpendicular axis
    axis = std::fabs(f.y) < 0.9f ? Cross(f, {0, 1, 0}) : Cross(f, {1, 0, 0});
  }
  axis = Norm(axis);
  if (maxAngle < 3.14159265f && ang > maxAngle) ang = maxAngle;
  return QNorm(QMul(AxisAngle(axis, ang), q));
}
inline Quat Nlerp(const Quat& a, Quat b, float t) {
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) b = {-b.x, -b.y, -b.z, -b.w};
  return QNorm({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}
// Rotation vector (axis * angle) to quaternion.
inline Quat FromRotVec(Vec3 r) {
  float a = Len(r);
  if (a <= 0) return Quat{};
  return AxisAngle(Mul(r, 1.0f / a), a);
}

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = 0.0174533f;
inline float WrapPi(float x) {
  float r = std::fmod(x, 2 * kPi);
  if (r < -kPi) r += 2 * kPi;
  else if (r > kPi) r -= 2 * kPi;
  return r;
}

}  // namespace moho::vm
