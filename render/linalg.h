// Small vector and matrix helpers for the renderer.
//
// Conventions (the game's): the world is right-handed, x east, y up, z south; quaternions are stored
// x, y, z, w. Matrices are row-major in memory and multiply column vectors (v' = M * v), so the last
// column holds the translation; shaders declare them `row_major` and write mul(M, v).
// Depth is reversed: the near plane maps to 1, infinity to 0 (see gfx.h).
#pragma once
#include <cmath>

namespace render {

struct Vec2 {
  float x = 0, y = 0;
};
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;
};
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { return a = a - b; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
  float l = Length(a);
  return l > 0 ? a * (1.0f / l) : a;
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline Vec3 Rotate(const Quat& q, Vec3 v) {
  Vec3 u{q.x, q.y, q.z};
  Vec3 t = 2.0f * Cross(u, v);
  return v + q.w * t + Cross(u, t);
}
// Shortest-path normalised lerp: enough between two consecutive simulation ticks.
inline Quat Nlerp(const Quat& a, Quat b, float t) {
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
  Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
  float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l > 0) q = {q.x / l, q.y / l, q.z / l, q.w / l};
  return q;
}

struct Mat4 {
  float m[4][4];  // m[row][column]

  static Mat4 Identity() { return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; }
  static Mat4 Translation(Vec3 t) { return {{{1, 0, 0, t.x}, {0, 1, 0, t.y}, {0, 0, 1, t.z}, {0, 0, 0, 1}}}; }
  static Mat4 Scale(Vec3 s) { return {{{s.x, 0, 0, 0}, {0, s.y, 0, 0}, {0, 0, s.z, 0}, {0, 0, 0, 1}}}; }
  // Rotation q, then translation t, with a uniform scale s applied first.
  static Mat4 FromTRS(Vec3 t, const Quat& q, float s = 1) {
    float x2 = q.x * 2, y2 = q.y * 2, z2 = q.z * 2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    return {{{s * (1 - (yy + zz)), s * (xy - wz), s * (xz + wy), t.x},
             {s * (xy + wz), s * (1 - (xx + zz)), s * (yz - wx), t.y},
             {s * (xz - wy), s * (yz + wx), s * (1 - (xx + yy)), t.z},
             {0, 0, 0, 1}}};
  }
  // A camera at `eye` with the given unit axes (right, up, back = the direction it looks away from).
  static Mat4 View(Vec3 eye, Vec3 right, Vec3 up, Vec3 back) {
    return {{{right.x, right.y, right.z, -Dot(right, eye)},
             {up.x, up.y, up.z, -Dot(up, eye)},
             {back.x, back.y, back.z, -Dot(back, eye)},
             {0, 0, 0, 1}}};
  }
  // Right-handed perspective looking down -z, reversed depth, no far plane: depth = zNear / distance.
  static Mat4 Perspective(float fovY, float aspect, float zNear) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    return {{{f / aspect, 0, 0, 0}, {0, f, 0, 0}, {0, 0, 0, zNear}, {0, 0, -1, 0}}};
  }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}
inline Vec4 operator*(const Mat4& a, Vec4 v) {
  return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w,
          a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w,
          a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w,
          a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w};
}

// General inverse (cofactors). Returns the identity when the matrix cannot be inverted.
inline Mat4 Inverse(const Mat4& a) {
  const float* s = &a.m[0][0];
  float inv[16];
  inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] + s[9] * s[7] * s[14] + s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
  inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] - s[8] * s[7] * s[14] - s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
  inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] + s[8] * s[7] * s[13] + s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
  inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] - s[8] * s[6] * s[13] - s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
  inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] - s[9] * s[3] * s[14] - s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
  inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] + s[8] * s[3] * s[14] + s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
  inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] - s[8] * s[3] * s[13] - s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
  inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] + s[8] * s[2] * s[13] + s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
  inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] + s[5] * s[3] * s[14] + s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
  inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] - s[4] * s[3] * s[14] - s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
  inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] + s[4] * s[3] * s[13] + s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
  inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] - s[4] * s[2] * s[13] - s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
  inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] - s[5] * s[3] * s[10] - s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
  inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] + s[4] * s[3] * s[10] + s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
  inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] - s[4] * s[3] * s[9] - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
  inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9] + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];
  float det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
  if (det == 0) return Mat4::Identity();
  Mat4 r;
  float* d = &r.m[0][0];
  for (int i = 0; i < 16; ++i) d[i] = inv[i] / det;
  return r;
}

}  // namespace render
