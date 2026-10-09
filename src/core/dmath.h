// Deterministic math: the same results on every platform and compiler.
//
// The C library's sin/cos/atan2 differ in the last bits between Windows and Linux (checked
// 2026-10-08: the same moho64 run gave quaternions 1 ulp apart). The sim must give the same
// result everywhere (replays, multiplayer), so the sim uses these instead: only +, -, *, / and
// sqrt, which IEEE 754 rounds identically everywhere, evaluated in double and rounded to float.
#pragma once
#include <cmath>

namespace moho::dmath {

// sin and cos of x (radians), |error| < 1e-15 for |x| < 1e6.
inline void SinCos(double x, double* s, double* c) {
  constexpr double kPiHalf1 = 1.57079632673412561417e+00;  // pi/2 split for Cody-Waite reduction
  constexpr double kPiHalf2 = 6.07710050650619224932e-11;
  constexpr double kPiHalf3 = 2.02226624879595063154e-21;
  constexpr double kTwoOverPi = 6.36619772367581382433e-01;
  double k = std::nearbyint(x * kTwoOverPi);
  double r = ((x - k * kPiHalf1) - k * kPiHalf2) - k * kPiHalf3;
  double r2 = r * r;
  // Taylor/minimax to degree 15/16 on |r| <= pi/4
  double sp = r * (1.0 + r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 +
              r2 * (-1.0 / 39916800 + r2 * (1.0 / 6227020800.0 + r2 * (-1.0 / 1307674368000.0))))))));
  double cp = 1.0 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800 +
              r2 * (1.0 / 479001600 + r2 * (-1.0 / 87178291200.0 + r2 * (1.0 / 20922789888000.0))))))));
  long q = static_cast<long>(k) & 3;
  switch (q) {
    case 0: *s = sp; *c = cp; break;
    case 1: *s = cp; *c = -sp; break;
    case 2: *s = -sp; *c = -cp; break;
    default: *s = -cp; *c = sp; break;
  }
}
inline float Sin(float x) { double s, c; SinCos(x, &s, &c); return static_cast<float>(s); }
inline float Cos(float x) { double s, c; SinCos(x, &s, &c); return static_cast<float>(c); }

// atan of x in [-1, 1] (|error| < 2e-16) by argument halving and a series.
inline double AtanUnit(double x) {
  // atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))): twice brings |x| below 0.2
  double y = x / (1.0 + std::sqrt(1.0 + x * x));
  y = y / (1.0 + std::sqrt(1.0 + y * y));
  double y2 = y * y, term = y, sum = y;
  for (int n = 3; n <= 27; n += 2) {
    term *= -y2;
    sum += term / n;
  }
  return 4.0 * sum;
}
inline double Atan2d(double y, double x) {
  constexpr double kPi = 3.14159265358979311600e+00;
  if (x == 0.0 && y == 0.0) return 0.0;
  double ax = std::fabs(x), ay = std::fabs(y);
  double a = ay <= ax ? AtanUnit(ay / ax) : kPi / 2 - AtanUnit(ax / ay);
  if (x < 0) a = kPi - a;
  return y < 0 ? -a : a;
}
inline float Atan2(float y, float x) { return static_cast<float>(Atan2d(y, x)); }
inline float Acos(float x) {
  double d = x < -1 ? -1.0 : (x > 1 ? 1.0 : x);
  return static_cast<float>(Atan2d(std::sqrt((1.0 - d) * (1.0 + d)), d));
}

inline float Asin(float x) {
  double d = x < -1 ? -1.0 : (x > 1 ? 1.0 : x);
  return static_cast<float>(Atan2d(d, std::sqrt((1.0 - d) * (1.0 + d))));
}
inline float Atan(float x) { return static_cast<float>(Atan2d(x, 1.0)); }
// natural log (x > 0): x = m * 2^e with m in [sqrt(1/2), sqrt(2)), then 2*atanh((m-1)/(m+1))
inline double Logd(double x) {
  if (!(x > 0)) return x == 0 ? -HUGE_VAL : NAN;
  int e;
  double m = std::frexp(x, &e);  // [0.5, 1)
  if (m < 0.70710678118654752440) {
    m *= 2;
    --e;
  }
  double t = (m - 1) / (m + 1), t2 = t * t, term = t, sum = t;
  for (int n = 3; n <= 41; n += 2) {
    term *= t2;
    sum += term / n;
  }
  return 2 * sum + e * 0.69314718055994530942;
}
inline float Log(float x) { return static_cast<float>(Logd(x)); }

}  // namespace moho::dmath
