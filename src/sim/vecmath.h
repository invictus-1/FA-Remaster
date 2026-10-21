// Vector and quaternion helpers for the combat code (y up; heading 0 faces +z; quaternions
// stored x, y, z, w as in sim/entity.h).
#pragma once
#include "core/dmath.h"
#include <cmath>
#include <cstring>
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
// The original's vector tests go through 0x4f0a50, a memcmp of the 12 bytes: a -0.0 part is not 0.0 there, and a
// NaN part equals the same NaN. Its "did it move" (the position, u+0xb4, against the one before, u+0xd0) and "is it
// zero" (against the zero vector 0xf3d21c) tests are these.
inline bool SameBits(const Vec3& a, const Vec3& b) { return std::memcmp(&a, &b, sizeof(Vec3)) == 0; }
inline bool BitsZero(const Vec3& v) { return SameBits(v, Vec3{0.0f, 0.0f, 0.0f}); }
// The same for a quaternion, the exe's 16-byte memcmp 0x4f0b40 (the order of the parts does not matter to it)
inline bool SameBits(const Quat& a, const Quat& b) { return std::memcmp(&a, &b, sizeof(Quat)) == 0; }
inline bool BitsZero4(float a, float b, float c, float d) {
  const float v[4] = {a, b, c, d};
  const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  return std::memcmp(v, z, sizeof v) == 0;
}
inline bool BitsZero(const Quat& q) { return BitsZero4(q.x, q.y, q.z, q.w); }
inline Vec3 NaNVec() {
  float n = std::numeric_limits<float>::quiet_NaN();
  return {n, n, n};
}

inline Quat Conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat QMul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
// parent x local as the transform compose 0x549c20 multiplies its quaternions (a pose bone's composite)
inline Quat ComposeQ(const Quat& p, const Quat& l) {
  return {((l.z * p.y + p.x * l.w) + l.x * p.w) - p.z * l.y, ((p.z * l.x + p.y * l.w) + l.y * p.w) - l.z * p.x,
          ((l.y * p.x + p.z * l.w) + l.z * p.w) - p.y * l.x, ((l.w * p.w - l.x * p.x) - l.y * p.y) - l.z * p.z};
}
// The exe's rotate-by-quaternion 0x452d40: through the quaternion's matrix (0x452fd0), in its order of operations
// (the manipulators use it: the last digit matters for an aim controller's angles)
inline Vec3 RotateM(const Quat& q, Vec3 v) {
  const float x2 = q.x * 2.0f, y2 = q.y * 2.0f, z2 = q.z * 2.0f;
  const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
  const float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
  const float m0 = 1.0f - (zz + yy), m1 = xy - wz, m2 = xz + wy;
  const float m3 = xy + wz, m4 = 1.0f - (zz + xx), m5 = yz - wx;
  const float m6 = xz - wy, m7 = yz + wx, m8 = 1.0f - (yy + xx);
  return {((0.0f + v.x * m0) + v.y * m1) + v.z * m2, ((0.0f + v.x * m3) + v.y * m4) + v.z * m5,
          ((0.0f + v.x * m6) + v.y * m7) + v.z * m8};
}
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
// The exe's normalise in place 0x452af0: the squares summed x, y, z, the root taken of the float sum, the parts
// multiplied by 1 / length; a length of 1e-6 or less gives the zero vector and returns 0. The root is compared as
// the x87 holds it, rounded to float at PC_24 (0x452b3e..0x452b51).
inline double NormalizeExe(Vec3& v) {
  const float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
  const float n = std::sqrt(s);
  if (n > 9.99999997e-07f) {
    const float inv = 1.0f / n;
    v.x *= inv;
    v.y *= inv;
    v.z *= inv;
    return n;
  }
  v = {};
  return 0.0;
}
// func_QuatFromVecRot 0x69aa50 (a shell's turn, homing or VelocityAlign): turn q so that its forward points along d,
// by at most maxAngle. The rotation is built from the half vector (0x44f880: h = normalise(f + d), w = f.h,
// xyz = f x h), not from an angle: a small turn comes out exact where an arccos of a dot product near 1 is off by a
// percent. 0x4eb740 clamps it (only when maxAngle / 2 is below pi / 2), and the product r * q is NOT normalised.
inline Quat TurnToward(const Quat& q, Vec3 d, float maxAngle) {
  Vec3 f{(q.w * q.y + q.z * q.x) * 2.0f, (q.z * q.y - q.w * q.x) * 2.0f, 1.0f - (q.y * q.y + q.x * q.x) * 2.0f};
  NormalizeExe(f);
  NormalizeExe(d);
  Vec3 h{d.x + f.x, f.y + d.y, f.z + d.z};
  Quat r;
  if (NormalizeExe(h) > 0.0) {
    r.x = h.z * f.y - f.z * h.y;
    r.y = f.z * h.x - h.z * f.x;
    r.z = h.y * f.x - f.y * h.x;
    r.w = (h.z * f.z + f.y * h.y) + h.x * f.x;
  } else {  // opposite: half a turn about the world's up
    r = {0.0f, 1.0f, 0.0f, 0.0f};
  }
  const float half = std::fabs(maxAngle * 0.5f);
  if (half < 1.57079637f) {
    float s = dmath::Sin(half);
    const float len2 = (r.z * r.z + r.x * r.x) + r.y * r.y;
    if (len2 > s * s) {
      if (0.0f > r.w) s = -0.0f - s;
      Vec3 n{r.x, r.y, r.z};
      NormalizeExe(n);
      r = {n.x * s, n.y * s, n.z * s, dmath::Cos(half)};
    }
  }
  return {((q.z * r.y + q.x * r.w) + q.w * r.x) - q.y * r.z, ((q.w * r.y + q.x * r.z) + q.y * r.w) - q.z * r.x,
          ((q.w * r.z + q.z * r.w) + q.y * r.x) - q.x * r.y, ((q.w * r.w - q.x * r.x) - q.y * r.y) - q.z * r.z};
}
inline Quat Nlerp(const Quat& a, Quat b, float t) {
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) b = {-b.x, -b.y, -b.z, -b.w};
  return QNorm({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}
// func_QuatLERP 0x4eba80, in its order of operations (a shell's orientation at its hit: MotionTick 0x69c7ec): a
// itself when every part is within 1e-6 of b's (0x4eded0); else t clamped to [0, 1], b negated when the dot product
// (summed w, x, y, z) is below 0, each part a x (1 - t) + t x b, then normalised by 0x4edaa0 (the squares summed
// w, x, y, z, the parts times 1 / length; a length of 1e-6 or less gives zero).
inline Quat NlerpExe(const Quat& a, Quat b, float t) {
  const float eps = 9.99999997e-07f;
  if (std::fabs(a.w - b.w) <= eps && std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
      std::fabs(a.z - b.z) <= eps)
    return a;
  if (!(1.0f > t)) t = 1.0f;
  if (0.0f > t) t = 0.0f;
  const float d = ((a.w * b.w + a.x * b.x) + a.y * b.y) + a.z * b.z;
  const float s = 1.0f - t;
  if (0.0f > d) b = {-0.0f - b.x, -0.0f - b.y, -0.0f - b.z, -0.0f - b.w};
  Quat r{a.x * s + t * b.x, a.y * s + t * b.y, a.z * s + t * b.z, a.w * s + t * b.w};
  const float len = std::sqrt(((r.w * r.w + r.x * r.x) + r.y * r.y) + r.z * r.z);
  if (!(len > 9.99999997e-07f)) return Quat{0.0f, 0.0f, 0.0f, 0.0f};
  const float inv = 1.0f / len;
  return {r.x * inv, r.y * inv, r.z * inv, r.w * inv};
}
// func_NormalizeQuatInPlace 0x4edaa0: the squares summed w, x, y, z, the root (x87 at PC_24, a float), then w and x
// times 1 / length and 1 / length times y and z (0x4edb09..0x4edb39); a length of 1e-6 or less gives zero.
inline Quat NormalizeQuatExe(const Quat& q) {
  const float len = std::sqrt(((q.w * q.w + q.x * q.x) + q.y * q.y) + q.z * q.z);
  if (!(len > 9.99999997e-07f)) return Quat{0.0f, 0.0f, 0.0f, 0.0f};
  const float inv = 1.0f / len;
  return {q.x * inv, inv * q.y, inv * q.z, q.w * inv};
}
// Rotation vector (axis * angle) to quaternion.
inline Quat FromRotVec(Vec3 r) {
  float a = Len(r);
  if (a <= 0) return Quat{};
  return AxisAngle(Mul(r, 1.0f / a), a);
}

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = 0.017453292f;  // 0xdff090
inline float WrapPi(float x) {  // 0x62fb50 (the remainder is taken with the double 2 pi, 0xe4f780)
  float r = static_cast<float>(std::fmod(static_cast<double>(x), 6.283185307179586));
  if (r < -3.14159265f) r += 6.28318531f;
  else if (r > 3.14159265f) r -= 6.28318531f;
  return r;
}

}  // namespace moho::vm
