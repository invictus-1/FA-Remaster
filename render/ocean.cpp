#include "ocean.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "scmap.h"
#include "view.h"

namespace render {
namespace {

constexpr float kPi = 3.14159265f;
// The squares' sizes (world units): the long waves (and the swell), the middle, the short. Not multiples of each other,
// so that their repeats never line up.
constexpr float kCascadeLength[Ocean::kCascades] = {180.0f, 41.0f, 9.7f};
// A square holds the waves from its 8th harmonic of the next smaller square's length on down to that of its own; the
// smallest all the way to the shortest it can hold (its length / 128: some 0.08 world units).
constexpr float kBandHarmonic = 8;
// Gravity in world units (seconds): the deep sea's waves run at sqrt(g lambda / 2 pi), as the swell's trains before
// (water.cpp, Swell: 0.55 sqrt(lambda), g about 1.9). A world unit is some 5 m by it, and the waves' pace the game's.
constexpr float kGravity = 1.9f;
// Every wave's speed is rounded so that the whole sea repeats after this long (seconds): the phases stay exact over a
// long game (omega t never grows past what a float holds well), and nobody watches the same sea come round again.
constexpr double kRepeat = 512;

// The wind sea's peak's length and its significant height (world units) by how rough the sea is (0 still .. 1 a storm),
// and the swell's. The heights stay low (the units must read: about knee-high on a small unit in a storm); the waves are
// as steep as a real sea's (a sea the wind has raised fully some 1/40 of a wave's length high, a storm's young sea 1/22),
// so they are short: a storm's some two ships long.
float SeaPeakLength(float rough) { return 3.2f + 2.4f * rough; }
float SeaHeight(float rough) { return 0.05f + 0.20f * rough; }
constexpr float kSwellLength = 30;
float SwellHeight(float rough) { return 0.03f + 0.04f * rough; }
constexpr float kSwellTurn = 0.61f;  // the swell's way, turned from the wind's (radians: some 35 degrees)
// How choppy: the share of the sideways shift (sharper crests, wider troughs; 1 the full shift Tessendorf's sea has).
float Choppiness(float rough) { return 0.75f + 0.35f * rough; }
// Where the surface folds or nearly (its Jacobian lower than its mean, 1, by this many of its standard deviations) it
// foams: only the very steepest crests of a gentle sea (well under one in a thousand of its places), a storm's many (some
// one in ten).
float FoamDeviations(float rough) { return 3.4f - 1.8f * rough; }
// [whitecaps] How long foam stays fresh after its crest has broken (seconds to fall to 1/e): bright and solid while the crest
// breaks and just after, then the residue, greyer and broken.
constexpr float kFreshLife = 0.45f;
// [whitecaps] The foam's drift told to the water's shader within this (world units): its pattern drifts with the field.
constexpr double kDriftRepeat = 4096;

// The JONSWAP wind sea's spectrum over the waves' angular frequency w (alpha 1): a peak at wp, sharpened (gamma 3.3).
// The same formula is the shader's (kSimShader, Jonswap): keep the two alike.
double Jonswap(double w, double wp) {
  if (w <= 1e-6) return 0;
  const double sigma = w <= wp ? 0.07 : 0.09;
  const double r = std::exp(-(w - wp) * (w - wp) / (2 * sigma * sigma * wp * wp));
  const double q = wp / w;
  return kGravity * kGravity / std::pow(w, 5) * std::exp(-1.25 * q * q * q * q) * std::pow(3.3, r);
}
// The swell's: narrow round its own frequency (as the shader's SwellShape).
double SwellShape(double w, double ws) {
  const double d = (w - ws) / (0.1 * ws);
  return std::exp(-0.5 * d * d);
}
// The integral of a spectrum over the frequencies the squares hold (alpha 1).
template <class F>
double Integral(F f, double w0, double w1) {
  const int n = 800;
  double sum = 0;
  const double l0 = std::log(w0), l1 = std::log(w1);
  for (int i = 0; i < n; ++i) {
    const double w = std::exp(l0 + (l1 - l0) * (i + 0.5) / n);
    sum += f(w) * w * (l1 - l0) / n;  // (d w = w d ln w)
  }
  return sum;
}

// The shaders' common part: the constants, the sea's spectrum and the transform's stages.
const char kSimCommon[] = R"hlsl(
cbuffer OceanSim : register(b1) {
  float4 band;     // x the square's length; y the shortest wave number it holds (from); z the first it no longer holds; w which square
  float4 sea;      // xy the wind's way; z the wind sea's peak frequency; w its amplitude (alpha)
  float4 swell;    // xy the swell's way; z its frequency; w its amplitude
  float4 clock;    // x the clock (within the repeat); y g; z how choppy; w the least step of frequency (2 pi / the repeat)
  float4 foamNew;  // the foam's field: xy its first cell's corner; z a cell's size; w cells a side
  float4 foamOld;  // the field read: xy its first cell's corner; z 1 / its side; w 1 = none (it starts empty)
  float4 foamMove; // xy how far the foam drifts this frame (world units); z the share of it kept this frame; w the frame's seconds
  float4 foamMake; // x the Jacobian it foams below; y how fast; z the water's level; w the ground's height unit
  float4 lengths;  // x, y, z 1 / each square's length; w the map's width
  float4 mapInfo;  // x, y the map's width and height
  float4 foamMore; // [whitecaps] xy the field's shift since the last frame in whole cells (foamOld.w 0); z the share of the fresh foam kept this frame
};
static const float kPi = 3.14159265;
SamplerState samplerLinearClamp : register(s1);  // (the fixed samplers, gfx.h)
SamplerState samplerLinear : register(s3);

uint OceanHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
// A complex number of the standard normal distribution for the wave at (n, m) of square c (Box and Muller), the same each
// frame. (The processor's OceanGauss is the same: keep the two alike.)
float2 OceanGauss(uint n, uint m, uint c) {
  uint h1 = OceanHash(n * 0x8da6b343u ^ m * 0xd8163841u ^ (c + 1u) * 0xcb1ab31fu);
  uint h2 = OceanHash(h1 ^ 0x68e31da4u);
  float u1 = (float(h1 >> 8) + 1.0) / 16777217.0;
  float u2 = float(h2 >> 8) / 16777216.0;
  float r = sqrt(-2 * log(u1));
  return r * float2(cos(6.2831853 * u2), sin(6.2831853 * u2));
}
float Jonswap(float w, float wp) {
  float sigma = w <= wp ? 0.07 : 0.09;
  float r = exp(-(w - wp) * (w - wp) / (2 * sigma * sigma * wp * wp));
  float q = wp / w;
  float q2 = q * q;
  float w2 = w * w;
  return clock.y * clock.y / (w2 * w2 * w) * exp(-1.25 * q2 * q2) * pow(3.3, r);
}
// Donelan and Banner's spreading of the waves round the wind's way: narrow at the peak, wider for the shorter and the
// longer waves; its integral over the angle is 1.
float Spreading(float theta, float w, float wp) {
  float x = w / wp;
  float beta = x < 0.95 ? 2.61 * pow(x, 1.3) : x < 1.6 ? 2.28 * pow(x, -1.3) : pow(10.0, -0.4 + 0.8393 * exp(-0.567 * log(x * x)));
  float ch = cosh(beta * theta);
  return beta / (2 * tanh(beta * kPi)) / (ch * ch);
}
float SwellShape(float w, float ws) {
  float d = (w - ws) / (0.1 * ws);
  return exp(-0.5 * d * d);
}
float Angle(float2 k, float2 way) { return atan2(k.x * way.y - k.y * way.x, dot(k, way)); }
// The wave at index (n, m) of this square: its wave vector, its frequency and how high it is (the spectrum over the square's
// cell of wave numbers: the root of a quarter of its variance, so that the sea's variance is the spectrum's integral)
void Wave(uint n, uint m, out float2 k, out float w, out float amplitude) {
  int2 q = int2(int(n) - (n >= 128 ? 256 : 0), int(m) - (m >= 128 ? 256 : 0));
  float dk = 2 * kPi / band.x;
  k = float2(q) * dk;
  float kl = length(k);
  w = 0;
  amplitude = 0;
  if (n == 128 || m == 128 || kl < band.y || kl >= band.z || kl <= 0) return;
  w = sqrt(clock.y * kl);
  // (S(k) = S(w) D(theta) (dw / dk) / k, dw / dk = g / (2 w))
  float toK = clock.y / (2 * w) / kl;
  float s = sea.w * Jonswap(w, sea.z) * Spreading(Angle(k, sea.xy), w, sea.z);
  // the swell: long-crested, its waves all near its own way (cos^48 of the angle from it, its integral 0.362: normalised)
  float t = Angle(k, swell.xy);
  float ct = cos(t);
  float ss = abs(t) < kPi * 0.5 ? swell.w * SwellShape(w, swell.z) * pow(ct * ct, 24) * 2.764 : 0;
  amplitude = sqrt(max(s + ss, 0) * toK * dk * dk * 0.25);
  w = max(floor(w / clock.w + 0.5), 1) * clock.w;  // (rounded to the repeat: kRepeat)
}
float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// The inverse transform of 256 complex numbers, four at once, in the group's shared memory (radix 2, decimation in time:
// the numbers loaded in bit-reversed order, the result in order). Each thread works out one number of each stage.
groupshared float2 fftBuf[2][4][256];
void Stages(uint i) {
  [unroll] for (uint s = 0; s < 8; ++s) {
    GroupMemoryBarrierWithGroupSync();
    uint src = s & 1;
    uint span = 1u << s;
    uint j = i & (2 * span - 1);
    uint low = j & (span - 1);
    uint at = i - j + low;  // the first of the pair
    float angle = kPi * float(low) / float(span);
    float2 tw = float2(cos(angle), sin(angle));
    float sign = j < span ? 1.0 : -1.0;
    [unroll] for (uint f = 0; f < 4; ++f) {
      float2 a = fftBuf[src][f][at], b = fftBuf[src][f][at + span];
      fftBuf[1 - src][f][i] = a + sign * CMul(tw, b);
    }
  }
  GroupMemoryBarrierWithGroupSync();
}
)hlsl";

// The rows: each wave's state now (its two halves, the wave and its opposite, turned on by their frequency), and from it
// the four sums the surface needs, packed two real fields to a complex one (each field is real, so its transform is
// Hermitian and two fit in one): (shift x, height), (shift z, slope x), (slope z, d shift x / dx), (d shift z / dz,
// d shift x / dz). Then the transform along each row.
const char kRows[] = R"hlsl(
RWTexture2D<float4> rowOut0 : register(u0);
RWTexture2D<float4> rowOut1 : register(u1);
[numthreads(256, 1, 1)] void Rows(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
  uint i = t.x, m = g.x;
  uint n = reversebits(i) >> 24;
  float2 k, kOpp;
  float w, a, wOpp, aOpp;
  Wave(n, m, k, w, a);
  Wave((256 - n) & 255, (256 - m) & 255, kOpp, wOpp, aOpp);
  float2 h0 = OceanGauss(n, m, uint(band.w)) * a;
  float2 h0Opp = OceanGauss((256 - n) & 255, (256 - m) & 255, uint(band.w)) * aOpp;
  float phase = w * clock.x;
  float2 e = float2(cos(phase), sin(phase));
  // h(k, t) = h0(k) e^(i w t) + conj(h0(-k)) e^(-i w t)
  float2 h = CMul(h0, e) + CMul(float2(h0Opp.x, -h0Opp.y), float2(e.x, -e.y));
  float kl = max(length(k), 1e-6);
  float2 ih = float2(-h.y, h.x);  // i h
  float2 dx = ih * (k.x / kl), dz = ih * (k.y / kl);  // the sideways shift: i k / |k| h (crests sharpened)
  float2 sx = ih * k.x, sz = ih * k.y;                // the slopes: i k h
  float2 dxx = -h * (k.x * k.x / kl), dzz = -h * (k.y * k.y / kl), dxz = -h * (k.x * k.y / kl);
  // (A + i B for two real fields A, B)
  fftBuf[0][0][i] = dx + float2(-h.y, h.x);
  fftBuf[0][1][i] = dz + float2(-sx.y, sx.x);
  fftBuf[0][2][i] = sz + float2(-dxx.y, dxx.x);
  fftBuf[0][3][i] = dzz + float2(-dxz.y, dxz.x);
  Stages(i);
  rowOut0[uint2(i, m)] = float4(fftBuf[0][0][i], fftBuf[0][1][i]);
  rowOut1[uint2(i, m)] = float4(fftBuf[0][2][i], fftBuf[0][3][i]);
}
)hlsl";

// The columns: the transform along each column, then the surface's numbers at each sample: its slopes and their squares,
// the sideways shift and the height (and the slopes' product), the shift's derivatives.
const char kColumns[] = R"hlsl(
Texture2D rowIn0 : register(t0);
Texture2D rowIn1 : register(t1);
RWTexture2D<float4> outSlope : register(u0);
RWTexture2D<float4> outDisp : register(u1);
RWTexture2D<float4> outFold : register(u2);
[numthreads(256, 1, 1)] void Columns(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
  uint i = t.x, n = g.x;
  uint m = reversebits(i) >> 24;
  float4 a = rowIn0.Load(int3(n, m, 0)), b = rowIn1.Load(int3(n, m, 0));
  fftBuf[0][0][i] = a.xy;
  fftBuf[0][1][i] = a.zw;
  fftBuf[0][2][i] = b.xy;
  fftBuf[0][3][i] = b.zw;
  Stages(i);
  float2 p0 = fftBuf[0][0][i], p1 = fftBuf[0][1][i], p2 = fftBuf[0][2][i], p3 = fftBuf[0][3][i];
  float dx = p0.x, h = p0.y, dz = p1.x, sx = p1.y, sz = p2.x, dxx = p2.y, dzz = p3.x, dxz = p3.y;
  float chop = clock.z;
  uint2 at = uint2(n, i);
  outSlope[at] = float4(sx, sz, sx * sx, sz * sz);
  outDisp[at] = float4(dx * chop, h, dz * chop, sx * sz);
  outFold[at] = float4(dxx * chop, dzz * chop, dxz * chop, 0);
}
)hlsl";

// The levels of detail, the means of the levels before (the moments average as they are: LEAN mapping). Near: each group
// of 16 x 16 samples to levels 0..3 (level 0 the samples themselves) and its one mean of level 4 into a small picture of
// its own (16 x 16: the groups'); far: the one group, from that small picture, to levels 4..8. (Slots u0..u4 of each of
// the three are its five levels written: the C++ side binds them so.)
const char kMips[] = R"hlsl(
Texture2D base0 : register(t0);
Texture2D base1 : register(t1);
Texture2D base2 : register(t2);
RWTexture2D<float4> l00 : register(u0);
RWTexture2D<float4> l01 : register(u1);
RWTexture2D<float4> l02 : register(u2);
RWTexture2D<float4> l03 : register(u3);
RWTexture2D<float4> l04 : register(u4);
RWTexture2D<float4> l10 : register(u5);
RWTexture2D<float4> l11 : register(u6);
RWTexture2D<float4> l12 : register(u7);
RWTexture2D<float4> l13 : register(u8);
RWTexture2D<float4> l14 : register(u9);
RWTexture2D<float4> l20 : register(u10);
RWTexture2D<float4> l21 : register(u11);
RWTexture2D<float4> l22 : register(u12);
RWTexture2D<float4> l23 : register(u13);
RWTexture2D<float4> l24 : register(u14);
groupshared float4 mipBuf[3][16][16];
void Put(uint k, uint level, uint2 at, float4 v) {
  if (k == 0) {
    if (level == 0) l00[at] = v; else if (level == 1) l01[at] = v; else if (level == 2) l02[at] = v; else if (level == 3) l03[at] = v; else l04[at] = v;
  } else if (k == 1) {
    if (level == 0) l10[at] = v; else if (level == 1) l11[at] = v; else if (level == 2) l12[at] = v; else if (level == 3) l13[at] = v; else l14[at] = v;
  } else {
    if (level == 0) l20[at] = v; else if (level == 1) l21[at] = v; else if (level == 2) l22[at] = v; else if (level == 3) l23[at] = v; else l24[at] = v;
  }
}
float4 Base(uint k, int2 at) { return k == 0 ? base0.Load(int3(at, 0)) : k == 1 ? base1.Load(int3(at, 0)) : base2.Load(int3(at, 0)); }
// (from `first`, the levels below it: 16 x 16 values in mipBuf, written at `corner` of level `first`)
void Reduce(uint2 t, uint2 corner, uint first) {
  [unroll] for (uint l = 1; l <= 4; ++l) {
    uint size = 16u >> l;
    float4 v[3];
    bool mine = t.x < size && t.y < size;
    [unroll] for (uint k = 0; k < 3; ++k) {
      v[k] = 0;
      if (mine)
        v[k] = 0.25 * (mipBuf[k][2 * t.y][2 * t.x] + mipBuf[k][2 * t.y][2 * t.x + 1] + mipBuf[k][2 * t.y + 1][2 * t.x] + mipBuf[k][2 * t.y + 1][2 * t.x + 1]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (mine) {
      [unroll] for (uint k = 0; k < 3; ++k) {
        mipBuf[k][t.y][t.x] = v[k];
        Put(k, first + l, (corner >> l) + t, v[k]);
      }
    }
    GroupMemoryBarrierWithGroupSync();
  }
}
[numthreads(16, 16, 1)] void MipsNear(uint3 id : SV_DispatchThreadID, uint3 t : SV_GroupThreadID, uint3 g : SV_GroupID) {
  [unroll] for (uint k = 0; k < 3; ++k) {
    float4 v = Base(k, int2(id.xy));
    Put(k, 0, id.xy, v);
    mipBuf[k][t.y][t.x] = v;
  }
  GroupMemoryBarrierWithGroupSync();
  Reduce(t.xy, g.xy * 16, 0);
}
[numthreads(16, 16, 1)] void MipsFar(uint3 t : SV_GroupThreadID) {
  [unroll] for (uint k = 0; k < 3; ++k) {
    float4 v = Base(k, int2(t.xy));  // (the near pass's means: level 4)
    Put(k, 0, t.xy, v);
    mipBuf[k][t.y][t.x] = v;
  }
  GroupMemoryBarrierWithGroupSync();
  Reduce(t.xy, uint2(0, 0), 0);
}
)hlsl";

// The foam's field: what was here a frame ago, a little less of it; and new foam where the surface folds now (the squares'
// shift derivatives added up: the Jacobian), none where the water is shallow. [whitecaps] Two numbers a cell: x how much
// foam lies there (it fades over seconds: the residue), y how fresh it is (it fades within a second of the crest's
// breaking). The field drifts downwind with the water in WHOLE cells (Ocean::Update keeps the drift's remainder, which the
// water's shader adds when it reads the field), so that it is copied cell for cell from frame to frame: read between its
// cells each frame, as it was, it blurred a little more every frame, and a storm's foam, living some seconds, spread into
// soft blobs some ten cells across. Only when the field's cell changes (the camera's distance) is it read between them.
const char kFoam[] = R"hlsl(
Texture2D<float2> foamIn : register(t0);
Texture2D fold0 : register(t1);
Texture2D fold1 : register(t2);
Texture2D fold2 : register(t3);
Texture2D<float> heights : register(t4);
RWTexture2D<float2> foamOut : register(u0);
float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heights.Load(int3(min(c, most), 0)), b = heights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heights.Load(int3(min(c + int2(0, 1), most), 0)), e = heights.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * foamMake.w;
}
[numthreads(8, 8, 1)] void Foam(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint(foamNew.w))) return;
  float2 world = foamNew.xy + (float2(id.xy) + 0.5) * foamNew.z;
  float2 old = 0;
  if (foamOld.w < 0.5) {  // (the same cells: the one it drifted from, whole)
    int2 c = int2(id.xy) + int2(foamMore.xy);
    if (all(c >= 0) && all(c < int(foamNew.w))) old = foamIn.Load(int3(c, 0));
  } else if (foamOld.w < 1.5) {  // (another cell size: read between the old field's cells, once)
    float2 at = (world - foamMove.xy - foamOld.xy) * foamOld.z;
    if (all(at >= 0) && all(at <= 1)) old = foamIn.SampleLevel(samplerLinearClamp, at, 0);
  }
  // (each square read at the level its texels are about a cell: finer folds than a cell are averaged in)
  float3 f = 0;
  float l0 = log2(max(foamNew.z * lengths.x * 256, 1)), l1 = log2(max(foamNew.z * lengths.y * 256, 1)), l2 = log2(max(foamNew.z * lengths.z * 256, 1));
  f += fold0.SampleLevel(samplerLinear, world * lengths.x + 0.5 / 256, l0).xyz;
  f += fold1.SampleLevel(samplerLinear, world * lengths.y + 0.5 / 256, l1).xyz;
  f += fold2.SampleLevel(samplerLinear, world * lengths.z + 0.5 / 256, l2).xyz;
  float J = (1 + f.x) * (1 + f.y) - f.z * f.z;
  float depth = foamMake.z - GroundAt(world);
  float make = saturate((foamMake.x - J) * foamMake.y) * saturate((depth - 0.3) / 1.5);
  // (a crest breaking for a third of a second lays a full cover, and keeps it fresh while it breaks)
  foamOut[id.xy] = float2(min(old.x * foamMove.z + make * foamMove.w * 3, 1.2), min(old.y * foamMore.z + make * foamMove.w * 6, 1));
}
)hlsl";

struct SimConstants {
  float band[4], sea[4], swell[4], clock[4];
  float foamNew[4], foamOld[4], foamMove[4], foamMake[4];
  float lengths[4], mapInfo[4];
  float foamMore[4];  // [whitecaps]
};

float BandFrom(int c) { return c == 0 ? 1e-6f : 2 * kPi * kBandHarmonic / kCascadeLength[c]; }
float BandTo(int c) { return c == Ocean::kCascades - 1 ? 1e9f : 2 * kPi * kBandHarmonic / kCascadeLength[c + 1]; }

}  // namespace

Ocean::~Ocean() {
  for (int c = 0; c < kCascades; ++c) {
    for (gfx::Texture t : {rowOut_[c][0], rowOut_[c][1], base_[c][0], base_[c][1], base_[c][2], field_[c][0], field_[c][1], field_[c][2], top_[c][0],
                           top_[c][1], top_[c][2]})
      if (t) device_->Destroy(t);
  }
  for (gfx::Texture t : foamMap_)
    if (t) device_->Destroy(t);
}

void Ocean::SetMap(const MapData* map) {
  map_ = map;
  haveWater_ = map && map->water.present;
  level_ = haveWater_ ? map->water.elevation : 0;
  foamFresh_ = true;
}

bool Ocean::PreparePipelines() {
  if (pipesTried_) return foam_.id != 0;
  pipesTried_ = true;
  auto make = [&](const char* name, const char* entry, const char* body) {
    gfx::ComputeDesc cd;
    const std::string source = std::string(kSimCommon) + body;
    cd.source = source;
    cd.name = name;
    cd.entry = entry;
    return device_->CreateCompute(cd, &pipesError_);
  };
  rows_ = make("ocean: rows", "Rows", kRows);
  if (rows_) columns_ = make("ocean: columns", "Columns", kColumns);
  if (columns_) mipsNear_ = make("ocean: levels near", "MipsNear", kMips);
  if (mipsNear_) mipsFar_ = make("ocean: levels far", "MipsFar", kMips);
  if (mipsFar_) foam_ = make("ocean: foam", "Foam", kFoam);
  return foam_.id != 0;
}

bool Ocean::Ready() {
  if (tried_) return made_;
  tried_ = true;
  std::string error;
  if (!PreparePipelines()) error = pipesError_;
  if (foam_) {
    made_ = true;
    gfx::TargetDesc td;
    td.storage = true;
    td.width = td.height = kN;
    for (int c = 0; c < kCascades && made_; ++c) {
      td.format = gfx::Format::RGBA32F;
      td.mips = 1;
      for (int k = 0; k < 2; ++k) made_ = made_ && (rowOut_[c][k] = device_->CreateTarget(td));
      td.format = gfx::Format::RGBA16F;
      for (int k = 0; k < 3; ++k) made_ = made_ && (base_[c][k] = device_->CreateTarget(td));
      td.mips = 9;
      for (int k = 0; k < 3; ++k) made_ = made_ && (field_[c][k] = device_->CreateTarget(td));
      td.mips = 1;
      td.width = td.height = kN / 16;
      for (int k = 0; k < 3; ++k) made_ = made_ && (top_[c][k] = device_->CreateTarget(td));
      td.width = td.height = kN;
    }
    td.mips = 1;
    td.width = td.height = kFoamCells;
    td.format = gfx::Format::RG16F;  // [whitecaps] (how much, how fresh)
    for (int k = 0; k < 2 && made_; ++k) made_ = made_ && (foamMap_[k] = device_->CreateTarget(td));
    if (!made_) error = "its storage targets cannot be made";
  }
  if (!made_) fprintf(stderr, "render: the ocean cannot be used (%s)\n", error.c_str());
  return made_;
}

void Ocean::Begin(double time, bool on, bool side, float rough, const float wind[2]) {
  side_ = side;
  active_ = false;
  if (side) return;  // (a picture for something else: the field as it is)
  if (!on || !haveWater_) {
    on_ = false;
    foamFresh_ = true;
    lastTime_ = -1;
    return;
  }
  const float want = std::clamp(roughness >= 0 ? roughness : rough, 0.0f, 1.0f);
  const double dt = lastTime_ >= 0 ? std::clamp(time - lastTime_, 0.0, 1.0) : 0.0;
  if (!on_ || rough_ < 0) rough_ = want;
  else rough_ += (want - rough_) * static_cast<float>(1 - std::exp(-dt / 6.0));  // (the sea grows and calms over some seconds)
  on_ = true;
  time_ = time;
  const float l = std::sqrt(wind[0] * wind[0] + wind[1] * wind[1]);
  if (l > 1e-4f) {
    wind_[0] = wind[0] / l;
    wind_[1] = wind[1] / l;
  }
  Spectrum();
}

void Ocean::Spectrum() {
  const float r = rough_;
  hs_ = SeaHeight(r) * heightScale;
  peak_ = SeaPeakLength(r);
  wp_ = std::sqrt(kGravity * 2 * kPi / peak_);
  const double wp = std::sqrt(kGravity * 2 * kPi / peak_);
  const double ws = std::sqrt(kGravity * 2 * kPi / kSwellLength);
  // the frequencies the squares hold: from the longest wave of the largest square to the shortest of the smallest
  const double w0 = std::sqrt(kGravity * 2 * kPi / kCascadeLength[0]);
  const double w1 = std::sqrt(kGravity * 2 * kPi * 128 * 1.2 / kCascadeLength[kCascades - 1]);
  const double seaVar = (hs_ / 4.0) * (hs_ / 4.0), swellVar = (SwellHeight(r) * heightScale / 4.0) * (SwellHeight(r) * heightScale / 4.0);
  const double seaAlpha = seaVar / std::max(Integral([wp](double w) { return Jonswap(w, wp); }, w0, w1), 1e-12);
  const double swellAlpha = swellVar / std::max(Integral([ws](double w) { return SwellShape(w, ws); }, w0, w1), 1e-12);
  seaAlpha_ = seaAlpha;
  swellAlpha_ = swellAlpha;
  const float turn = kSwellTurn, c = std::cos(turn), s = std::sin(turn);
  const float sim[16] = {0, 0, 0, 0,
                         wind_[0], wind_[1], static_cast<float>(wp), static_cast<float>(seaAlpha),
                         wind_[0] * c - wind_[1] * s, wind_[0] * s + wind_[1] * c, static_cast<float>(ws), static_cast<float>(swellAlpha),
                         static_cast<float>(std::fmod(time_, kRepeat)), kGravity, Choppiness(r), static_cast<float>(2 * kPi / kRepeat)};
  std::memcpy(sim_, sim, sizeof sim);
}

void Ocean::Update(const Camera& camera, const View& view, gfx::Texture heights, float heightUnit) {
  active_ = false;
  nodes_.clear();
  if (!on_ || side_ || !map_ || !heights || !Ready()) return;
  const int viewHeight = view.height;
  heights_ = heights;
  heightUnit_ = heightUnit;
  SimConstants k = {};
  std::memcpy(k.sea, sim_ + 4, sizeof(float) * 12);
  k.lengths[0] = 1 / kCascadeLength[0];
  k.lengths[1] = 1 / kCascadeLength[1];
  k.lengths[2] = 1 / kCascadeLength[2];
  k.lengths[3] = static_cast<float>(map_->width);
  k.mapInfo[0] = static_cast<float>(map_->width);
  k.mapInfo[1] = static_cast<float>(map_->height);
  // the field: per square the rows, the columns and the levels
  for (int c = 0; c < kCascades; ++c) {
    k.band[0] = kCascadeLength[c];
    k.band[1] = BandFrom(c);
    k.band[2] = BandTo(c);
    k.band[3] = static_cast<float>(c);
    device_->Mark("ocean: rows");
    device_->SetPipeline(rows_);
    device_->SetConstants(1, &k, sizeof k);
    device_->SetStorage(0, rowOut_[c][0]);
    device_->SetStorage(1, rowOut_[c][1]);
    device_->Dispatch(kN);
    device_->SetStorage(0, {});
    device_->SetStorage(1, {});
    device_->Mark("ocean: columns");
    device_->SetPipeline(columns_);
    device_->SetTexture(0, rowOut_[c][0]);
    device_->SetTexture(1, rowOut_[c][1]);
    for (int t = 0; t < 3; ++t) device_->SetStorage(t, base_[c][t]);
    device_->Dispatch(kN);
    device_->SetTexture(0, {});
    device_->SetTexture(1, {});
    device_->Mark("ocean: levels");
    device_->SetPipeline(mipsNear_);
    for (int t = 0; t < 3; ++t) {
      device_->SetTexture(t, base_[c][t]);
      for (int l = 0; l < 4; ++l) device_->SetStorage(t * 5 + l, field_[c][t], l);
      device_->SetStorage(t * 5 + 4, top_[c][t]);
    }
    device_->Dispatch(kN / 16, kN / 16);
    device_->Mark("ocean: levels far");
    device_->SetPipeline(mipsFar_);
    for (int t = 0; t < 3; ++t) {
      device_->SetTexture(t, top_[c][t]);
      for (int l = 0; l < 5; ++l) device_->SetStorage(t * 5 + l, field_[c][t], 4 + l);
    }
    device_->Dispatch(1);
    for (int slot = 0; slot < 15; ++slot) device_->SetStorage(slot, {});
    for (int t = 0; t < 3; ++t) device_->SetTexture(t, {});
  }
  // the foam's field: its cell by the camera's distance (about a pixel and a half where it looks, in steps of two), the
  // square round what the camera looks at in whole cells
  const float pixel = 2 * camera.distance * std::tan(camera.fovY * 0.5f) / static_cast<float>(std::max(viewHeight, 1));
  float cell = foamCell_ > 0 ? foamCell_ : 0.0625f;
  const float want = std::clamp(pixel * 1.5f, 0.04f, 2.0f);
  while (want > cell * 1.3f && cell < 2.0f) cell *= 2;
  while (want < cell * 0.55f && cell > 0.04f) cell *= 0.5f;
  const double dt = lastTime_ >= 0 ? std::clamp(time_ - lastTime_, 0.0, 0.25) : 0.0;
  lastTime_ = time_;
  const float r = rough_;
  const float drift = (0.12f + 0.25f * r) * static_cast<float>(dt);
  // [whitecaps] the field drifts with the water: its cells lie from the drift so far on, in whole cells round what the
  // camera looks at (so that it is copied cell for cell from frame to frame, kFoam)
  if (foamFresh_) driftX_ = driftZ_ = 0;
  driftX_ += wind_[0] * drift;
  driftZ_ += wind_[1] * drift;
  const int ox = static_cast<int>(std::floor((camera.target.x - driftX_) / cell)) - kFoamCells / 2,
            oz = static_cast<int>(std::floor((camera.target.z - driftZ_) / cell)) - kFoamCells / 2;
  const float x0 = static_cast<float>(ox * static_cast<double>(cell) + driftX_), z0 = static_cast<float>(oz * static_cast<double>(cell) + driftZ_),
              side = static_cast<float>(kFoamCells) * cell;
  const float life = 2.0f + 3.5f * r;  // (seconds to fall to 1/e; [whitecaps] longer, as its residue thins to lace: was 1.5 + 2 r)
  const bool sameCells = !foamFresh_ && cell == foamCell_;
  const float foamNew[4] = {x0, z0, cell, static_cast<float>(kFoamCells)};
  const float foamOld[4] = {foamArea_[0], foamArea_[1], foamArea_[2], foamFresh_ ? 2.0f : sameCells ? 0.0f : 1.0f};
  const float foamMove[4] = {wind_[0] * drift, wind_[1] * drift, static_cast<float>(std::exp(-dt / life)), static_cast<float>(dt)};
  const float foamMore[4] = {static_cast<float>(ox - foamX_), static_cast<float>(oz - foamZ_), static_cast<float>(std::exp(-dt / kFreshLife)), 0};
  // (the Jacobian's spread: about the choppiness times the root of the slopes' mean square, of the waves the field's cells
  // hold, longer than about two cells)
  const double kCut = kPi / (2 * cell);
  const double wCut = std::sqrt(kGravity * std::min(kCut, 2 * kPi * 128 / static_cast<double>(kCascadeLength[kCascades - 1])));
  const double w0 = std::sqrt(kGravity * 2 * kPi / kCascadeLength[0]);
  const double wp = wp_, ws = std::sqrt(kGravity * 2 * kPi / kSwellLength);
  const double mss = Integral([&](double w) { const double k = w * w / kGravity; return k * k * (seaAlpha_ * Jonswap(w, wp) + swellAlpha_ * SwellShape(w, ws)); }, w0,
                              std::max(wCut, w0 * 1.01));
  foamThreshold_ = static_cast<float>(1 - Choppiness(r) * std::sqrt(mss) * FoamDeviations(r));
  const float foamMake[4] = {foamThreshold_, 4.0f, level_, heightUnit};
  std::memcpy(k.foamNew, foamNew, sizeof foamNew);
  std::memcpy(k.foamOld, foamOld, sizeof foamOld);
  std::memcpy(k.foamMove, foamMove, sizeof foamMove);
  std::memcpy(k.foamMake, foamMake, sizeof foamMake);
  std::memcpy(k.foamMore, foamMore, sizeof foamMore);
  device_->Mark("ocean: foam");
  device_->SetPipeline(foam_);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, foamMap_[foamCur_]);
  for (int c = 0; c < kCascades; ++c) device_->SetTexture(1 + c, field_[c][2]);
  device_->SetTexture(4, heights);
  device_->SetStorage(0, foamMap_[1 - foamCur_]);
  device_->Dispatch(kFoamCells / 8, kFoamCells / 8);
  device_->SetStorage(0, {});
  for (int t = 0; t < 5; ++t) device_->SetTexture(t, {});
  foamCur_ = 1 - foamCur_;
  foamFresh_ = false;
  foamCell_ = cell;
  foamX_ = ox;
  foamZ_ = oz;
  foamArea_[0] = x0;
  foamArea_[1] = z0;
  foamArea_[2] = 1 / side;
  foamArea_[3] = cell;

  // what the water's shader is told
  for (int c = 0; c < kCascades; ++c) {
    constants_.cascade[c][0] = 1 / kCascadeLength[c];
    constants_.cascade[c][1] = kCascadeLength[c];
    constants_.cascade[c][2] = 1;
    constants_.cascade[c][3] = 0.5f / kN;
  }
  constants_.wind[0] = wind_[0];
  constants_.wind[1] = wind_[1];
  constants_.wind[2] = r;
  constants_.wind[3] = Choppiness(r);
  std::memcpy(constants_.foamArea, foamArea_, sizeof foamArea_);
  constants_.look[0] = 0.0025f;
  constants_.look[1] = glow;
  constants_.look[2] = foamStrength;
  constants_.look[3] = static_cast<float>(this->view);
  constants_.look2[0] = hs_;
  constants_.look2[1] = 0.04f * r * r;
  constants_.look2[2] = tilt;
  constants_.look2[3] = lift ? 1.0f : 0.0f;
  constants_.look3[0] = foamThreshold_;
  constants_.look3[1] = glint;
  // [whitecaps] how far the foam has drifted (its pattern drifts with it; within kDriftRepeat, so that it stays exact)
  constants_.look4[0] = static_cast<float>(std::fmod(driftX_, kDriftRepeat));
  constants_.look4[1] = static_cast<float>(std::fmod(driftZ_, kDriftRepeat));
  constants_.look4[2] = 0;
  constants_.look4[3] = 0;
  active_ = true;
  // [lift] the grid's patches for this view, and the long waves the processor keeps for what floats
  if (lift && MakeGrid()) {
    PlaceGrid(view, view.constants.beyond[2] > 0 ? 1 / view.constants.beyond[2] : 0.0f);
    Vec3 right, up, back;
    camera.Axes(&right, &up, &back);
    const bool alongX = std::fabs(back.x) > std::fabs(back.z);
    gridOrder_ = (alongX ? 4 : 0) | (back.x < 0 ? 2 : 0) | (back.z < 0 ? 1 : 0);  // (looking toward +x: the far side first)
  }
  if (lift) KeepBobWaves();
  for (int c = 0; c < kCascades; ++c) std::memcpy(ride_[c], constants_.cascade[c], sizeof ride_[c]);
  ride_[3][0] = static_cast<float>(map_->width);
  ride_[3][1] = static_cast<float>(map_->height);
  ride_[3][2] = heightUnit;
  ride_[3][3] = level_;
}

void Ocean::BindRide() const {
  for (int c = 0; c < kCascades; ++c) device_->SetTexture(kOceanFirstSlot + 3 + c, field_[c][1]);
  device_->SetTexture(kOceanFirstSlot + 10, heights_);
}

void Ocean::UnbindRide() const {
  for (int c = 0; c < kCascades; ++c) device_->SetTexture(kOceanFirstSlot + 3 + c, {});
  device_->SetTexture(kOceanFirstSlot + 10, {});
}

// [lift] The grid ------------------------------------------------------------------------------------------------------

namespace {
constexpr float kLeaf = 4;            // the smallest patch's side (world units): cells of 0.125
constexpr float kRangeTimes = 3;      // a level's range: this many of its patches' sides from the eye
constexpr float kMorphFrom = 0.75f;   // its vertices begin to slide at this share of its range ...
constexpr float kMorphTo = 0.97f;     // ... and lie on the coarser grid from this share on
constexpr float kLiftReach = 0.5f;    // (the waves' height a patch's box is given, world units: for culling)
}  // namespace

bool Ocean::MakeGrid() {
  if (gridTried_) return gridVertices_ && gridIndices_;
  gridTried_ = true;
  const int n = kPatchCells, v = n + 1;
  std::vector<float> corners;
  corners.reserve(static_cast<size_t>(v * v * 2));
  for (int j = 0; j < v; ++j)
    for (int i = 0; i < v; ++i) {
      corners.push_back(static_cast<float>(i) / static_cast<float>(n));
      corners.push_back(static_cast<float>(j) / static_cast<float>(n));
    }
  // eight orderings of the patch's triangles, each from one side and corner to the other: drawn so, from the far side, a
  // nearer wave's face is drawn over a farther one's (the water tests no depth of its own)
  std::vector<uint16_t> indices;
  indices.reserve(static_cast<size_t>(8 * n * n * 6));
  for (int order = 0; order < 8; ++order) {
    const bool alongX = (order & 4) != 0, backX = (order & 2) != 0, backZ = (order & 1) != 0;
    for (int a = 0; a < n; ++a)
      for (int b = 0; b < n; ++b) {
        int i = alongX ? a : b, j = alongX ? b : a;
        if (backX) i = n - 1 - i;
        if (backZ) j = n - 1 - j;
        const uint16_t v00 = static_cast<uint16_t>(j * v + i), v10 = static_cast<uint16_t>(v00 + 1), v01 = static_cast<uint16_t>(v00 + v),
                       v11 = static_cast<uint16_t>(v01 + 1);
        for (uint16_t k : {v00, v01, v10, v10, v01, v11}) indices.push_back(k);
      }
  }
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Vertex;
  bd.stride = 8;
  bd.size = static_cast<uint32_t>(corners.size() * sizeof(float));
  bd.data = corners.data();
  gridVertices_ = device_->CreateBuffer(bd);
  bd.kind = gfx::BufferKind::Index;
  bd.stride = 2;
  bd.size = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
  bd.data = indices.data();
  gridIndices_ = device_->CreateBuffer(bd);
  if (!gridVertices_ || !gridIndices_) fprintf(stderr, "render: the ocean's grid cannot be made\n");
  return gridVertices_ && gridIndices_;
}

void Ocean::PlaceGrid(const View& view, float reach) {
  mapW_ = static_cast<float>(map_->width);
  mapH_ = static_cast<float>(map_->height);
  // the root: a square round the map's middle as large as the water reaches (past the map's edge as far as the world beyond)
  float size = kLeaf;
  int level = 0;
  while (size < std::max(mapW_, mapH_) + 2 * reach && level < 15) {
    size *= 2;
    ++level;
  }
  rootLevel_ = level;
  rootX_ = mapW_ * 0.5f - size * 0.5f;
  rootZ_ = mapH_ * 0.5f - size * 0.5f;
  for (int l = 0; l <= level; ++l) ranges_[l] = kLeaf * static_cast<float>(1 << l) * kRangeTimes;
  ranges_[level] = 1e9f;  // (the root takes in all there is)
  Select(rootX_, rootZ_, level, view, view.eye, reach);
  // the farthest first (the water tests no depth of its own: a nearer patch is drawn over a farther); at most kMostPatches,
  // the nearest kept
  std::sort(nodes_.begin(), nodes_.end(), [](const Node& a, const Node& b) { return a.away > b.away; });
  if (nodes_.size() > static_cast<size_t>(kMostPatches)) nodes_.erase(nodes_.begin(), nodes_.end() - kMostPatches);
  grid_.info[0] = static_cast<float>(kPatchCells);
  grid_.info[1] = 0;
  grid_.info[2] = heightUnit_;
  grid_.info[3] = 0;
  for (int l = 0; l < 16; ++l) {
    const float r = l <= level ? ranges_[l] : 1e9f;
    grid_.lods[l][0] = r * kMorphFrom;
    grid_.lods[l][1] = 1 / std::max(r * (kMorphTo - kMorphFrom), 1e-3f);
    grid_.lods[l][2] = grid_.lods[l][3] = 0;
  }
  for (size_t k = 0; k < nodes_.size(); ++k) {
    grid_.nodes[k][0] = nodes_[k].x;
    grid_.nodes[k][1] = nodes_[k].z;
    grid_.nodes[k][2] = nodes_[k].size;
    grid_.nodes[k][3] = nodes_[k].level;
  }
}

// The quadtree walked (Strugar's CDLOD): a patch of `level` at (x, z) is drawn whole where all of it lies beyond the next
// finer level's range; else its quarters are looked at, each drawn at the finer level where it lies within that level's
// range, or else at this level's resolution (a quarter of the patch's cells: the quarter's own patch, its vertices all slid
// onto this level's grid). False: it lies beyond this level's range altogether (its parent draws its place).
void Ocean::Select(float x, float z, int level, const View& view, Vec3 eye, float reach) {
  struct Walk {
    static float Away(float x, float z, float size, Vec3 eye, float y) {
      const float dx = std::max({x - eye.x, 0.0f, eye.x - (x + size)}), dz = std::max({z - eye.z, 0.0f, eye.z - (z + size)});
      const float dy = eye.y - y;
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
  };
  const float size = kLeaf * static_cast<float>(1 << level);
  // (only over the water: the map and as far beyond it as the world beyond reaches)
  if (x > mapW_ + reach || z > mapH_ + reach || x + size < -reach || z + size < -reach) return;
  if (!view.BoxVisible({x, level_ - kLiftReach, z}, {x + size, level_ + kLiftReach, z + size})) return;
  const float away = Walk::Away(x, z, size, eye, level_);
  auto add = [&](float ax, float az, float s, int l) {
    nodes_.push_back({ax, az, s, static_cast<float>(l), Walk::Away(ax, az, s, eye, level_)});
  };
  if (level == 0 || away > ranges_[level - 1]) {
    add(x, z, size, level);
    return;
  }
  const float half = size * 0.5f;
  for (int c = 0; c < 4; ++c) {
    const float cx = x + ((c & 1) ? half : 0), cz = z + ((c & 2) ? half : 0);
    if (cx > mapW_ + reach || cz > mapH_ + reach || cx + half < -reach || cz + half < -reach) continue;
    if (!view.BoxVisible({cx, level_ - kLiftReach, cz}, {cx + half, level_ + kLiftReach, cz + half})) continue;
    if (Walk::Away(cx, cz, half, eye, level_) <= ranges_[level - 1]) Select(cx, cz, level - 1, view, eye, reach);
    else add(cx, cz, half, level - 1);  // (beyond the finer range: its vertices slide fully onto this level's grid)
  }
}

void Ocean::PlaceGridFor(const Camera& camera, const View& view) {  // [views]
  if (!Lifted()) return;
  nodes_.clear();
  PlaceGrid(view, view.constants.beyond[2] > 0 ? 1 / view.constants.beyond[2] : 0.0f);
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const bool alongX = std::fabs(back.x) > std::fabs(back.z);
  gridOrder_ = (alongX ? 4 : 0) | (back.x < 0 ? 2 : 0) | (back.z < 0 ? 1 : 0);
}

void Ocean::DrawGrid() {
  if (!Lifted()) return;
  const int count = static_cast<int>(nodes_.size());
  device_->SetConstants(3, &grid_, static_cast<uint32_t>(offsetof(GridConstants, nodes) + sizeof grid_.nodes[0] * count));
  device_->SetTexture(kOceanFirstSlot + 10, heights_);
  device_->SetVertexBuffer(gridVertices_);
  device_->SetIndexBuffer(gridIndices_);
  const uint32_t perPatch = kPatchCells * kPatchCells * 6;
  device_->DrawIndexed(perPatch, perPatch * static_cast<uint32_t>(gridOrder_), 0, static_cast<uint32_t>(count));
  device_->SetTexture(kOceanFirstSlot + 10, {});
}

// [lift] What floats -------------------------------------------------------------------------------------------------------

namespace {
// The card's wave at index (n, m) of square c, as the shader's Wave and OceanGauss work it out (keep them alike).
uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
void Gauss(uint32_t n, uint32_t m, uint32_t c, float* re, float* im) {
  const uint32_t h1 = Hash(n * 0x8da6b343u ^ m * 0xd8163841u ^ (c + 1u) * 0xcb1ab31fu);
  const uint32_t h2 = Hash(h1 ^ 0x68e31da4u);
  const float u1 = (static_cast<float>(h1 >> 8) + 1.0f) / 16777217.0f;
  const float u2 = static_cast<float>(h2 >> 8) / 16777216.0f;
  const float r = std::sqrt(-2 * std::log(u1));
  *re = r * std::cos(6.2831853f * u2);
  *im = r * std::sin(6.2831853f * u2);
}
float Spreading(float theta, float w, float wp) {
  const float x = w / wp;
  const float beta = x < 0.95f ? 2.61f * std::pow(x, 1.3f) : x < 1.6f ? 2.28f * std::pow(x, -1.3f) : std::pow(10.0f, -0.4f + 0.8393f * std::exp(-0.567f * std::log(x * x)));
  const float ch = std::cosh(beta * theta);
  return beta / (2 * std::tanh(beta * kPi)) / (ch * ch);
}
float Angle(float kx, float kz, float wx, float wz) { return std::atan2(kx * wz - kz * wx, kx * wx + kz * wz); }

// The amplitude of the card's wave of wave vector (kx, kz) in a square whose cell of wave numbers is dk, for the sea's
// numbers `sim` (Spectrum's); its frequency into *w (as the shader's Wave: keep them alike).
float BobAmplitude(const float* sim, float kx, float kz, float dk, float* w) {
  const float wp = sim[6], seaAlpha = sim[7], ws = sim[10], swellAlpha = sim[11];
  const float seaX = sim[4], seaZ = sim[5], swellX = sim[8], swellZ = sim[9];
  const float w0 = sim[15];
  const float kl = std::sqrt(kx * kx + kz * kz);
  *w = 0;
  if (kl <= 0) return 0.0f;
  const float wf = std::sqrt(kGravity * kl);
  const float toK = kGravity / (2 * wf) / kl;
  const float s = seaAlpha * static_cast<float>(Jonswap(wf, wp)) * Spreading(Angle(kx, kz, seaX, seaZ), wf, wp);
  const float t = Angle(kx, kz, swellX, swellZ), ct = std::cos(t);
  const float ss = std::fabs(t) < kPi * 0.5f ? swellAlpha * static_cast<float>(SwellShape(wf, ws)) * std::pow(ct * ct, 24.0f) * 2.764f : 0;
  *w = std::max(std::floor(wf / w0 + 0.5f), 1.0f) * w0;
  return std::sqrt(std::max(s + ss, 0.0f) * toK * dk * dk * 0.25f);
}
// [whitecaps] (the waves looked at for the set: the largest square's, and the middle one's longer than about a ship, 1.5
// world units; each square's a square of rows of wave numbers)
constexpr float kBobShortest = 2 * kPi / 1.5f;
int BobReach(int c) { return static_cast<int>(std::ceil(std::min(BandTo(c), kBobShortest) / (2 * kPi / kCascadeLength[c]))); }
// [whitecaps] Rows of candidates looked at a frame while the set is chosen again (Ocean::KeepBobWaves): all of them at once
// took 2.2 ms (a storm's sea easing to calm), 24 rows about 0.25 ms, the last frame's choosing about 0.4: a new set in about
// six frames.
constexpr int kBobRowsAFrame = 24;
}  // namespace

void Ocean::KeepBobWaves() {
  // [whitecaps] The set of waves is chosen again (the strongest) when the sea has changed much since: from the sea's numbers
  // as they were when the choosing began, a few rows of candidates a frame (it took 2.2 ms at once, a spike in the frame a
  // storm came up), the old set kept until the new one is whole. The first set (the waves just switched on, or the map
  // new) at once: there is none to keep.
  const bool changed = std::fabs(rough_ - keptRough_) > 0.08f || std::fabs(heightScale - keptHeight_) > 1e-3f ||
                       wind_[0] * keptWind_[0] + wind_[1] * keptWind_[1] < 0.985f;
  if ((bobWaves_.empty() || changed) && !pick_.running) {
    keptRough_ = rough_;
    keptHeight_ = heightScale;
    keptWind_[0] = wind_[0];
    keptWind_[1] = wind_[1];
    pick_.running = true;
    std::memcpy(pick_.sim, sim_, sizeof pick_.sim);
    pick_.found.clear();
    pick_.square = 0;
    pick_.row = -BobReach(0);
  }
  if (pick_.running) {
    int rows = bobWaves_.empty() ? 1 << 30 : kBobRowsAFrame;
    while (rows-- > 0 && pick_.square < 2) {
      const int c = pick_.square, reach = BobReach(c);
      const float dk = 2 * kPi / kCascadeLength[c];
      const float from = BandFrom(c), to = std::min(BandTo(c), kBobShortest);
      const int qm = pick_.row;
      for (int qn = -reach; qn <= reach; ++qn) {
        const float kx = static_cast<float>(qn) * dk, kz = static_cast<float>(qm) * dk, kl = std::sqrt(kx * kx + kz * kz);
        if (kl < from || kl >= to || kl <= 0) continue;
        float w;
        const float a = BobAmplitude(pick_.sim, kx, kz, dk, &w), b = BobAmplitude(pick_.sim, -kx, -kz, dk, &w);
        pick_.found.push_back({a * a + b * b, c, qn, qm});
      }
      if (++pick_.row > reach) {
        ++pick_.square;
        if (pick_.square < 2) pick_.row = -BobReach(pick_.square);
      }
    }
    if (pick_.square >= 2) {  // (whole: the strongest kept)
      pick_.running = false;
      std::vector<BobPick::Candidate>& found = pick_.found;
      const size_t keep = std::min(found.size(), static_cast<size_t>(kBobWaves));
      std::partial_sort(found.begin(), found.begin() + static_cast<long>(keep), found.end(),
                        [](const BobPick::Candidate& x, const BobPick::Candidate& y) { return x.energy > y.energy; });
      bobWaves_.clear();
      for (size_t i = 0; i < keep; ++i) {
        const BobPick::Candidate& f = found[i];
        const float dk = 2 * kPi / kCascadeLength[f.c];
        BobWave b{};
        b.kx = static_cast<float>(f.n) * dk;
        b.kz = static_cast<float>(f.m) * dk;
        b.square = f.c;
        const uint32_t n = static_cast<uint32_t>(f.n & 255), m = static_cast<uint32_t>(f.m & 255);
        const uint32_t no = (256 - n) & 255, mo = (256 - m) & 255;
        Gauss(n, m, static_cast<uint32_t>(f.c), &b.a0r, &b.a0i);
        Gauss(no, mo, static_cast<uint32_t>(f.c), &b.a1r, &b.a1i);
        b.a1i = -b.a1i;  // (conj)
        bobWaves_.push_back(b);
      }
      ampSim_[6] = -1;  // (their amplitudes worked out below)
    }
  }
  // the amplitudes as the sea is now ([whitecaps] worked out again only when its numbers have moved by more than one in a
  // hundred: as the sea eases toward a new roughness they move a little each frame, and the 1280 amplitudes took some
  // 0.2 ms a frame), and the waves turned on to the clock each frame
  auto moved = [](float x, float y) { return std::fabs(x - y) > 0.01f * std::max(std::fabs(x), std::fabs(y)); };
  if (moved(sim_[6], ampSim_[6]) || moved(sim_[7], ampSim_[7]) || moved(sim_[11], ampSim_[11]) || moved(sim_[4], ampSim_[4]) ||
      moved(sim_[5], ampSim_[5])) {
    std::memcpy(ampSim_, sim_, sizeof ampSim_);
    for (BobWave& b : bobWaves_) {
      const float dk = 2 * kPi / kCascadeLength[b.square];
      float wo;
      b.amp = BobAmplitude(sim_, b.kx, b.kz, dk, &b.w);
      b.ampOpp = BobAmplitude(sim_, -b.kx, -b.kz, dk, &wo);
    }
  }
  const float t = sim_[12];
  bobNow_.resize(bobWaves_.size());
  for (size_t i = 0; i < bobWaves_.size(); ++i) {
    const BobWave& b = bobWaves_[i];
    const float ph = b.w * t, ce = std::cos(ph), se = std::sin(ph);
    // h = h0 e^(i w t) + conj(h0(-k)) e^(-i w t)
    const float h0r = b.a0r * b.amp, h0i = b.a0i * b.amp, h1r = b.a1r * b.ampOpp, h1i = b.a1i * b.ampOpp;
    bobNow_[i] = {b.kx, b.kz, h0r * ce - h0i * se + h1r * ce + h1i * se, h0r * se + h0i * ce + h1i * ce - h1r * se};
  }
}

bool Ocean::BobAt(float x, float z, float size, Bob* out) const {
  *out = Bob{};
  if (!Riding() || !map_) return false;  // ([whitecaps] was Lifted(): never so where it was asked)
  // (as OceanLiftShare: none over shallow water; lakes are not told apart here)
  const float depth = level_ - map_->Height(std::clamp(x, 0.0f, mapW_), std::clamp(z, 0.0f, mapH_));
  const float share = std::clamp((depth - 0.6f) / 3.0f, 0.0f, 1.0f);
  if (share <= 0) return true;
  // the waves summed there, each the less the shorter it is against the hull (a Gaussian footprint of about a third of it)
  const float sigma2 = (size * 0.3f) * (size * 0.3f);
  float h = 0, sx = 0, sz = 0;
  for (const BobNow& b : bobNow_) {
    const float k2 = b.kx * b.kx + b.kz * b.kz;
    const float q = 1 + 0.5f * k2 * sigma2;
    const float weight = 1 / (q * q);
    const float ph = b.kx * x + b.kz * z, c = std::cos(ph), s = std::sin(ph);
    const float re = b.hr * c - b.hi * s, im = b.hr * s + b.hi * c;
    h += weight * re;
    sx -= weight * b.kx * im;
    sz -= weight * b.kz * im;
  }
  out->rise = h * share;
  out->slopeX = sx * share;
  out->slopeZ = sz * share;
  return true;
}


void Ocean::Bind() {
  for (int c = 0; c < kCascades; ++c) {
    device_->SetTexture(kOceanFirstSlot + c, field_[c][0]);
    device_->SetTexture(kOceanFirstSlot + 3 + c, field_[c][1]);
    device_->SetTexture(kOceanFirstSlot + 6 + c, field_[c][2]);
  }
  device_->SetTexture(kOceanFirstSlot + 9, foamMap_[foamCur_]);
}

void Ocean::Unbind() {
  for (int s = 0; s < 10; ++s) device_->SetTexture(kOceanFirstSlot + s, {});
}

}  // namespace render
