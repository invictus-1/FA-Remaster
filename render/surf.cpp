#include "surf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "lighting.h"
#include "models.h"
#include "scmap.h"

namespace render {
namespace {

float Smooth(float from, float to, float x) {
  const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// A float as a half float's bits (rounded to the nearest), for the field's texture.
uint16_t Half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t e = static_cast<int32_t>((x >> 23) & 0xffu) - 127 + 15;
  uint32_t m = x & 0x7fffffu;
  if (e <= 0) {  // (too small for a normal half: a subnormal one, or 0)
    if (e < -10) return static_cast<uint16_t>(sign);
    m |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - e);
    uint32_t h = m >> shift;
    if ((m >> (shift - 1)) & 1u) ++h;
    return static_cast<uint16_t>(sign | h);
  }
  if (e >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  uint32_t h = sign | (static_cast<uint32_t>(e) << 10) | (m >> 13);
  if (m & 0x1000u) ++h;  // (a carry into the exponent is still the right number)
  return static_cast<uint16_t>(h);
}

// The squared distance from each of `n` places along a line to the nearest site, where `f` holds 0 at a site and a
// large number elsewhere (or, along the second axis, the squared distances found along the first): the lower envelope
// of the parabolas rooted at each place (after Felzenszwalb and Huttenlocher's "Distance Transforms of Sampled
// Functions", 2012, written here from the paper's idea). `arg`: which place the nearest is; `v`, `z`: room for n and
// n + 1.
void Envelope(const double* f, int n, double* d, int* arg, int* v, double* z) {
  int k = 0;
  v[0] = 0;
  z[0] = -1e30;
  z[1] = 1e30;
  for (int q = 1; q < n; ++q) {
    double s;
    for (;;) {  // (the parabolas this one hides are let go of; the first's z is -1e30, so it is never passed)
      const int p = v[k];
      s = ((f[q] + static_cast<double>(q) * q) - (f[p] + static_cast<double>(p) * p)) / (2.0 * q - 2.0 * p);
      if (s > z[k]) break;
      --k;
    }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = 1e30;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    const double off = static_cast<double>(q - v[k]);
    d[q] = off * off + f[v[k]];
    arg[q] = v[k];
  }
}

// For every cell of a w x h grid, the squared distance (in cells) to the nearest cell where `site` is true, and that
// cell's index (-1 when there is none).
void Nearest(const std::vector<uint8_t>& site, int w, int h, std::vector<float>* dist2, std::vector<int>* nearest) {
  const int most = std::max(w, h);
  std::vector<double> f(static_cast<size_t>(most)), d(static_cast<size_t>(most)), z(static_cast<size_t>(most) + 1);
  std::vector<int> arg(static_cast<size_t>(most)), v(static_cast<size_t>(most));
  std::vector<double> rowD(static_cast<size_t>(w) * h);
  std::vector<int> rowArg(static_cast<size_t>(w) * h);
  constexpr double kNone = 1e12;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) f[static_cast<size_t>(x)] = site[static_cast<size_t>(y) * w + x] ? 0 : kNone;
    Envelope(f.data(), w, d.data(), arg.data(), v.data(), z.data());
    for (int x = 0; x < w; ++x) {
      rowD[static_cast<size_t>(y) * w + x] = d[static_cast<size_t>(x)];
      rowArg[static_cast<size_t>(y) * w + x] = arg[static_cast<size_t>(x)];
    }
  }
  dist2->assign(static_cast<size_t>(w) * h, 0);
  nearest->assign(static_cast<size_t>(w) * h, -1);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[static_cast<size_t>(y)] = rowD[static_cast<size_t>(y) * w + x];
    Envelope(f.data(), h, d.data(), arg.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) {
      const size_t at = static_cast<size_t>(y) * w + x;
      const int ny = arg[static_cast<size_t>(y)];
      const bool none = d[static_cast<size_t>(y)] >= kNone * 0.5;
      (*dist2)[at] = none ? 1e12f : static_cast<float>(d[static_cast<size_t>(y)]);
      (*nearest)[at] = none ? -1 : ny * w + rowArg[static_cast<size_t>(ny) * w + x];
    }
  }
}

// The mean of `in` over a square of (2r + 1) cells a side round each cell, counting only what lies on the grid (by a
// table of sums).
std::vector<float> BoxMean(const std::vector<float>& in, int w, int h, int r) {
  std::vector<double> sum(static_cast<size_t>(w + 1) * (h + 1), 0.0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      sum[static_cast<size_t>(y + 1) * (w + 1) + x + 1] = in[static_cast<size_t>(y) * w + x] + sum[static_cast<size_t>(y) * (w + 1) + x + 1] +
                                                          sum[static_cast<size_t>(y + 1) * (w + 1) + x] - sum[static_cast<size_t>(y) * (w + 1) + x];
  std::vector<float> out(in.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int x0 = std::max(x - r, 0), x1 = std::min(x + r + 1, w), y0 = std::max(y - r, 0), y1 = std::min(y + r + 1, h);
      const double s = sum[static_cast<size_t>(y1) * (w + 1) + x1] - sum[static_cast<size_t>(y0) * (w + 1) + x1] - sum[static_cast<size_t>(y1) * (w + 1) + x0] +
                       sum[static_cast<size_t>(y0) * (w + 1) + x0];
      out[static_cast<size_t>(y) * w + x] = static_cast<float>(s / static_cast<double>((x1 - x0) * (y1 - y0)));
    }
  return out;
}

// The spray: each puff a card facing the camera, its place from where it was thrown and its age (spray thrown up and
// slowed by the air, falling back and gone into the water; mist drifting off with the wind, rising a little), growing
// as it spreads; white water's colour, lit as the lit smoke is (the sun softly through it on its sunny side, sent on
// forward more than back: spray brightens before the sun; the sky from round about; with lit smoke on the small lights
// and the smoke's own shadow), seen through the air; worked out at its corners (a puff is small and soft). A pixel only
// shapes it (drops, broken; mist soft) and tests it against the scene's depth, fading where it meets something solid.
const char kSprayShader[] = R"hlsl(
cbuffer Spray : register(b1) {
  float4 right;    // xyz: the camera's right; w: the camera's near distance (the scene's depth is that over the distance)
  float4 upward;   // xyz: the camera's up; w: the water's level
  float4 wind;     // xy: the wind near the ground (world units a second, along x and z); z: 1 = lit as smoke is; w: the first row
  float4 fade;     // x: from how far off the eye a puff thins out (world units); y: 1 / over how far; w: 1 = a check (each puff red)
};
// three rows a puff: (where it was thrown, its age), (as it was thrown: velocity, its life), (its size at first, at the
// last, how much shows, its kind + its own number: 0 spray, 1 mist, 2 a ship's bow spray)
StructuredBuffer<float4> sprayRows : register(t0, space1);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

// The small lights at a place in the spray (lights.h; as the lit smoke's): only how near each light is counts; a beam
// within its cone, through its lamp's shadow.
float3 SpraySmallLights(float3 p) {
  float3 sum = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), count = min(uint(square.y), 48u);
      for (uint s = 0; s < count; ++s) {
        uint at = first + s;
        uint which = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + which * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 colour = lightData[uint(lightGridSize.z) + which * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + which * 4 + 2];
        if (beam.w > -1.5) {
          float3 l = normalize(toLight);
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float reaches = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + which * 4 + 3].x;
          [branch] if (map > -0.5 && reaches > 0) reaches *= LampShadow(p, l, uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * colour.w * saturate(1 - far2 * 25);
        }
        sum += colour.rgb * fade;
      }
    }
  }
  return sum;
}

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD1;      // -1..1 across the card
  float3 colour : TEXCOORD2;  // as the eye sees it here: lit, through the air (linear)
  float4 shade : TEXCOORD3;   // x: how much shows (0..1); y: over how many world units it fades where it meets something; z: its seed
  nointerpolation float kind : TEXCOORD6;
};

static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

VOut VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (no puff: behind the eye)
  uint at = uint(wind.w) + instance * 3;
  float4 a = sprayRows[at], b = sprayRows[at + 1], c = sprayRows[at + 2];
  float age = a.w, life = b.w;
  if (age < 0 || age >= life) return o;
  float kind = floor(c.w), seed = frac(c.w) * 97;
  bool mist = kind > 0.5 && kind < 1.5;
  float t = age / life;
  // where it is: thrown and slowed by the air; spray falls back (and is gone into the water), carried a little by the
  // wind; mist drifts with the wind and rises a little
  float drag = mist ? 2.0 : 0.8;
  float3 p = a.xyz + b.xyz * ((1 - exp(-drag * age)) / drag);
  p.xz += wind.xy * (age * (mist ? 1.0 : 0.35));
  p.y += mist ? 0.15 * age : -3.2 * age * age;
  float size = lerp(c.x, c.y, (1 - exp(-3 * t)) / (1 - exp(-3.0)));
  float underwater = upward.w - (p.y - size * 0.3);  // (how far its lower part has gone into the water)
  p.y = max(p.y, upward.w + size * 0.3);
  // spray drawn out along the way it flies (across the line of sight), as water thrown up is seen; mist turned a
  // little each, so the cards do not all show the same face
  float3 toEye = normalize(cameraPosition - p);
  float3 moving = b.xyz * exp(-drag * age) + (mist ? float3(wind.x, 0.15, wind.y) : float3(wind.x * 0.35, -6.4 * age, wind.y * 0.35));
  float3 across = moving - toEye * dot(moving, toEye);
  float fast = length(across);
  float3 along = fast > 1e-3 ? across / fast : upward.xyz;
  float3 side = normalize(cross(along, toEye) + 1e-5);
  float stretch = mist ? 1 : 1 + min(fast * 0.3, 2.2);
  float turn = mist ? seed * 0.37 + age * (frac(seed * 0.13) - 0.5) : 0;
  float2 corner = kCorners[vertex];
  float2 tc = float2(corner.x * cos(turn) - corner.y * sin(turn), corner.x * sin(turn) + corner.y * cos(turn));
  float3 out_ = (mist ? right.xyz : side) * tc.x + (mist ? upward.xyz : along) * tc.y;
  float3 w = p + ((mist ? right.xyz : side) * tc.x + (mist ? upward.xyz : along) * (tc.y * stretch)) * size;
  o.position = mul(viewProj, float4(w, 1));
  o.uv = corner;
  float far = length(p - cameraPosition);
  o.shade.x = c.z * smoothstep(0, mist ? 0.3 : 0.05, age) * pow(1 - t, mist ? 1.2 : 1.6) * (1 - saturate((far - fade.x) * fade.y)) *
              saturate(1 - underwater / max(size * 0.5, 0.05));
  o.shade.y = size * 0.6;
  o.shade.z = seed;
  o.kind = kind;
  if (o.shade.x <= 0) return o;
  float sun = SunShadow(p, float3(0, 1, 0)) * CloudShadow(p);
  float3 small = 0;
  [branch] if (wind.z > 0.5) {
    [branch] if (smokeShadow.w > 0.5 && smokeShadow.x < 0.5) sun *= SmokeShadowAt(p + sunDirection * (size * 0.6), 0.6);
    small = SpraySmallLights(p);
  }
  // white water's colour; lit at each corner (a ball's way: its sunny side brighter across the card), the sun let
  // through and sent on forward, the sky from round about
  float3 albedo = mist ? float3(0.70, 0.75, 0.79) : float3(0.80, 0.84, 0.86);
  float3 toward = normalize(cameraPosition - p);
  float3 n = normalize(out_ * 0.53 + toward * 0.66);
  float sunSide = saturate((dot(n, sunDirection) + 0.8) / 1.8);
  float g = mist ? 0.6 : 0.5, mu = dot(-sunDirection, toward);
  float phase = lerp(1, (1 - g * g) / pow(max(1 + g * g - 2 * g * mu, 1e-4), 1.5), 0.45);
  float3 skyDiffuse = skyZenith.w > 0.5 ? SkyAmbient(n) : ToLinear(environmentSky.SampleLevel(samplerLinear, n, 8).rgb);
  float3 light = ToLinear(sunColor) * (lightMultiplier * modern.x) * (sunSide * phase * lerp(0.35, 1, sun)) +
                 (skyDiffuse + ToLinear(shadowFill) * 0.5) * modern.y + small;
  o.colour = ThroughAirTo(albedo * light, w);
  return o;
}

float Meets(VOut i, float soft, out uint coverage, out float nearest) {
  coverage = 0;
  nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(soft, 1e-3));
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float r2 = dot(i.uv, i.uv);
  float body = (1 - r2) * (1 - r2);
  coverage = 0;
  if (r2 >= 1 || body * i.shade.x < 0.004) discard;  // (outside it, or too faint to show: before the scene's depth is read)
  float behind;
  float meets = Meets(i, i.shade.y, coverage, behind);
  if (fade.w > 0.5) return float4(fade.w > 1.5 ? i.colour : float3(1, 0, 0), fade.w > 1.5 ? i.shade.x : 1);  // (MOHO64_DEBUG_SURF=3: every puff red, where it is drawn; 4: its colour, by how much shows)
  if (meets <= 0) discard;
  float mist = i.kind > 0.5 && i.kind < 1.5 ? 1 : 0;
  // spray dense in its middle and broken into drops toward its edge; mist soft and billowing
  float n = CloudNoise(float3(i.uv * lerp(4.2, 1.8, mist) + i.shade.z, i.shade.z)) * 0.6 +
            CloudNoise(float3(i.uv * lerp(9.0, 3.9, mist) - i.shade.z, 2.3)) * 0.4;
  float drops = saturate((sqrt(body) - (1 - n) * 0.7) * 2.5);
  float alpha = saturate(lerp(drops * 1.3, body * (0.4 + 0.9 * n), mist) * i.shade.x) * meets;
  // (a ship's bow spray thinner where its hull, or anything solid, lies close behind it: the ship stays readable)
  if (i.kind > 1.5) alpha *= lerp(0.3, 1, saturate(behind / 2.5));
  if (alpha <= 0.004) discard;
  return float4(i.colour, alpha);
}
)hlsl";

}  // namespace

Surf::~Surf() {
  if (worker_.joinable()) worker_.join();  // [groundfix]
  if (pipeline_) device_->Destroy(pipeline_);
  if (rows_) device_->Destroy(rows_);
  if (field_) device_->Destroy(field_);
}

void Surf::SetMap(const MapData* map) {
  if (worker_.joinable()) worker_.join();  // [groundfix] (the last map's field, if still being worked out: let go of)
  work_.reset();
  fade_ = 1;
  fieldWaited = 0;
  fieldMainMs = 0;
  map_ = map;
  tried_ = false;  // (worked out again the first frame it is on)
  if (field_) device_->Destroy(field_);
  field_ = {};
  spots_.clear();
  puffs_.clear();
  ships_.clear();
  bursts = 0;
  const char* debug = std::getenv("MOHO64_DEBUG_SURF");
  debug_ = debug ? std::max(1, atoi(debug)) : 0;
}

namespace {

// The map's smooth offset (the shader's SurfOffset).
float MapOffset(float x, float z) {
  return 0.55f * std::sin(x * 0.0211f + z * 0.0137f) + 0.35f * std::sin(-x * 0.0173f + z * 0.0291f + 1.7f) +
         0.20f * std::sin(x * 0.047f - z * 0.031f + 4.1f);
}

// [coast] The shore's stretches, as the shader has them (surf.h: SurfHash, SurfStretchAt, SurfStretchTiming): keep the two
// alike.
constexpr float kStretch = 16, kStretchEnds = 0.3f;
uint32_t StretchHash(int32_t cx, int32_t cz, uint32_t salt) {
  uint32_t h = (static_cast<uint32_t>(cx) * 0x8da6b343u) ^ (static_cast<uint32_t>(cz) * 0xd8163841u) ^ (salt * 0xcb1ab31fu);
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return h;
}
float StretchHash01(int32_t cx, int32_t cz, uint32_t salt) { return static_cast<float>(StretchHash(cx, cz, salt) >> 8) * (1.0f / 16777216.0f); }
struct StretchPlace {
  int32_t cx = 0, cz = 0;
  float sx = 0, sz = 0;
};
StretchPlace StretchAt(float x, float z) {
  StretchPlace s;
  const float qx = x / kStretch - 0.5f, qz = z / kStretch - 0.5f;
  s.cx = static_cast<int32_t>(std::floor(qx));
  s.cz = static_cast<int32_t>(std::floor(qz));
  s.sx = Smooth(0.5f - kStretchEnds, 0.5f + kStretchEnds, qx - std::floor(qx));
  s.sz = Smooth(0.5f - kStretchEnds, 0.5f + kStretchEnds, qz - std::floor(qz));
  return s;
}
float StretchTiming(int32_t cx, int32_t cz) {
  return MapOffset((static_cast<float>(cx) + 0.5f) * kStretch, (static_cast<float>(cz) + 0.5f) * kStretch) + (StretchHash01(cx, cz, 1u) - 0.5f) * 0.6f;
}
float Blend(const StretchPlace& s, float a, float b, float c, float d) {
  const float ab = a + (b - a) * s.sx, cd = c + (d - c) * s.sx;
  return ab + (cd - ab) * s.sz;
}

}  // namespace

float Surf::Offset(float x, float z) {
  // [coast] the stretch's timing (was the map's smooth offset at the place: the arrival ran along the coast)
  const StretchPlace s = StretchAt(x, z);
  return Blend(s, StretchTiming(s.cx, s.cz), StretchTiming(s.cx + 1, s.cz), StretchTiming(s.cx, s.cz + 1), StretchTiming(s.cx + 1, s.cz + 1));
}

float Surf::Strength(float n, float x, float z) {
  const float s = 0.5f + 0.5f * std::sin(n * 0.897f + x * 0.013f + z * 0.009f);
  const float t = 0.5f + 0.5f * std::sin(n * 2.31f - x * 0.029f + z * 0.041f + 2.0f);
  // [coast] times the stretch's own size of wave n
  const StretchPlace p = StretchAt(x, z);
  const uint32_t salt = static_cast<uint32_t>(static_cast<int32_t>(n)) * 2u + 7u;
  const float own = 0.6f + 0.8f * Blend(p, StretchHash01(p.cx, p.cz, salt), StretchHash01(p.cx + 1, p.cz, salt), StretchHash01(p.cx, p.cz + 1, salt),
                                        StretchHash01(p.cx + 1, p.cz + 1, salt));
  return (0.3f + 0.7f * (0.65f * s * s + 0.35f * t)) * own;
}

// [groundfix] On a thread of its own (StartField): from the copy of the map's heights in `work`, nothing of the device's
// nor of the Surf's; the texture's texels and the spray's places into `work`, `ok` set when there is a field.
void Surf::WorkOutField(FieldWork* work, int debug) {
  const MapData* map = work->map.get();
  const auto started = std::chrono::steady_clock::now();
  // a cell of one world unit (more on the largest maps: at most 1025 cells a side)
  const float width = static_cast<float>(map->width), height = static_cast<float>(map->height);
  const float cell = std::max(1.0f, std::max(width, height) / 1024.0f);
  const int w = static_cast<int>(std::ceil(width / cell)) + 1, h = static_cast<int>(std::ceil(height / cell)) + 1;
  const size_t count = static_cast<size_t>(w) * h;
  const float level = map->water.elevation;
  std::vector<float> ground(count);
  std::vector<uint8_t> wet(count), dry(count);
  for (int z = 0; z < h; ++z)
    for (int x = 0; x < w; ++x) {
      const size_t at = static_cast<size_t>(z) * w + x;
      ground[at] = map->Height(std::min(static_cast<float>(x) * cell, width), std::min(static_cast<float>(z) * cell, height));
      wet[at] = ground[at] < level ? 1 : 0;
      dry[at] = 1 - wet[at];
    }
  // the nearest dry cell to each wet one and the nearest wet cell to each dry one: both lie on the waterline
  std::vector<float> toDry, toWet;
  std::vector<int> nearDry, nearWet;
  Nearest(dry, w, h, &toDry, &nearDry);
  Nearest(wet, w, h, &toWet, &nearWet);
  // how steep the ground is (rise per world unit), and that over five cells round about
  std::vector<float> slope(count);
  auto g = [&](int x, int z) { return ground[static_cast<size_t>(std::clamp(z, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; };
  for (int z = 0; z < h; ++z)
    for (int x = 0; x < w; ++x) slope[static_cast<size_t>(z) * w + x] = std::hypot(g(x + 1, z) - g(x - 1, z), g(x, z + 1) - g(x, z - 1)) / (2 * cell);
  const std::vector<float> slopeWide = BoxMean(slope, w, h, 2);
  // how open the water is: the share of water a unit or more deep within 80 world units
  std::vector<float> deepWater(count);
  for (size_t i = 0; i < count; ++i) deepWater[i] = level - ground[i] >= 1.0f ? 1.0f : 0.0f;
  const std::vector<float> share = BoxMean(deepWater, w, h, std::max(1, static_cast<int>(80 / cell)));
  // the signed distance to the waterline (world units, + over the water): from the nearest cell of the other kind;
  // within a cell and a half of the line, where the heights tell it better, by the ground's height and its slope
  std::vector<float> distance(count), steep(count), open(count);
  for (size_t i = 0; i < count; ++i) {
    const bool isWet = wet[i] != 0;
    const float d2 = isWet ? toDry[i] : toWet[i];
    float d = d2 >= 1e11f ? 64.0f : (std::sqrt(d2) - 0.5f) * cell;
    if (d < 1.5f * cell) {
      const float local = (level - ground[i]) / std::max(slope[i], 0.02f);
      if (std::fabs(local) < 1.5f * cell && (local >= 0) == isWet) d = std::fabs(local);
    }
    distance[i] = std::clamp(isWet ? d : -d, -16.0f, 64.0f);
    // how steep the nearest shore is: the ground's slope where the nearest cell of the other kind lies (both sides of
    // the line), on a scale from a beach (a rise of 0.3 a unit or less) to rock (0.85 or more)
    const int n = isWet ? nearDry[i] : nearWet[i];
    steep[i] = n >= 0 ? Smooth(0.3f, 0.85f, slopeWide[static_cast<size_t>(n)]) : 0.0f;
    // how open the water before it is: from a share of about an eighth round about (ponds, inlets) to one of nearly half
    // (an open coast: the land takes the other half)
    open[i] = std::clamp((share[i] - 0.12f) / 0.33f, 0.0f, 1.0f);
  }
  // (the nearest cell's steepness changes at once between two shores: smoothed over a few cells)
  steep = BoxMean(steep, w, h, 2);
  // [coast] how exposed the water near the shores is to the sea's waves (it scales the openness, and with it all of the
  // surf): from each place, how far open water reaches in sixteen ways (its fetch, up to kFetch world units; past the map's
  // edge the sea is taken to go on), each way weighted by how near it is to the way the swell comes from (kSwellX, kSwellZ:
  // the way it runs, as the spray's wind blows, movement_dust.h) and a little from every way (the wind's own small waves).
  // A coast facing the swell or a headland gets the most, a bay less and a lee shore little; a pond nothing. On cells of
  // two (the rays stepping as much), smoothed over about ten world units; the land takes its nearest water's.
  {
    constexpr int kWays = 16, kStep = 2;
    constexpr float kFetch = 160, kSwellX = 0.915f, kSwellZ = 0.403f;
    float wayX[kWays], wayZ[kWays], weight[kWays], total = 0;
    for (int k = 0; k < kWays; ++k) {
      const float a = 6.2831853f * static_cast<float>(k) / kWays;
      wayX[k] = std::cos(a);
      wayZ[k] = std::sin(a);
      const float toward = std::max(0.0f, -(wayX[k] * kSwellX + wayZ[k] * kSwellZ) / std::hypot(kSwellX, kSwellZ));  // (back where it comes from)
      weight[k] = 0.25f + 0.75f * toward * toward;
      total += weight[k];
    }
    const int cw = (w + kStep - 1) / kStep, chh = (h + kStep - 1) / kStep;
    const float reach = kFetch / cell;  // (cells)
    std::vector<float> value(static_cast<size_t>(cw) * chh, 0.0f), worked(static_cast<size_t>(cw) * chh, 0.0f);
    for (int cz = 0; cz < chh; ++cz)
      for (int cx = 0; cx < cw; ++cx) {
        const int x = std::min(cx * kStep, w - 1), z = std::min(cz * kStep, h - 1);
        const size_t at = static_cast<size_t>(z) * w + x;
        if (!wet[at] || distance[at] > 48) continue;
        float sum = 0;
        for (int k = 0; k < kWays; ++k) {
          float fetch = reach;
          for (float t = kStep; t <= reach; t += kStep) {
            const int px = static_cast<int>(std::lround(static_cast<float>(x) + wayX[k] * t)), pz = static_cast<int>(std::lround(static_cast<float>(z) + wayZ[k] * t));
            if (px < 0 || pz < 0 || px >= w || pz >= h) break;  // (past the map's edge: the sea goes on)
            if (!wet[static_cast<size_t>(pz) * w + px]) {
              fetch = t;
              break;
            }
          }
          sum += weight[k] * fetch / reach;
        }
        value[static_cast<size_t>(cz) * cw + cx] = sum / total;
        worked[static_cast<size_t>(cz) * cw + cx] = 1;
      }
    const std::vector<float> valueMean = BoxMean(value, cw, chh, 2), workedMean = BoxMean(worked, cw, chh, 2);
    auto coarse = [&](int cx, int cz) {
      const size_t at = static_cast<size_t>(std::clamp(cz, 0, chh - 1)) * cw + std::clamp(cx, 0, cw - 1);
      return workedMean[at] > 0 ? valueMean[at] / workedMean[at] : 1.0f;
    };
    // (a coast facing the swell straight on comes to about 0.7, a lee shore 0.3, a pond under 0.1: from 0.1 none to 0.7 all)
    std::vector<float> exposed(count, 1.0f);
    for (int z = 0; z < h; ++z)
      for (int x = 0; x < w; ++x) {
        const size_t at = static_cast<size_t>(z) * w + x;
        if (!wet[at]) continue;
        const float fx = static_cast<float>(x) / kStep, fz = static_cast<float>(z) / kStep;
        const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
        const float ax = fx - static_cast<float>(ix), az = fz - static_cast<float>(iz);
        const float e = (coarse(ix, iz) * (1 - ax) + coarse(ix + 1, iz) * ax) * (1 - az) + (coarse(ix, iz + 1) * (1 - ax) + coarse(ix + 1, iz + 1) * ax) * az;
        exposed[at] = std::clamp((e - 0.1f) / 0.6f, 0.0f, 1.0f);
      }
    for (size_t i = 0; i < count; ++i) {
      if (!wet[i] && nearWet[i] >= 0) exposed[i] = exposed[static_cast<size_t>(nearWet[i])];
      open[i] *= exposed[i];
    }
  }
  // (out at sea the distance's lines of equal value have corners, where the nearest shore changes: the waves' crests
  // would follow them; rounded off by the mean over three cells, then eight, the farther out the more)
  {
    const std::vector<float> round3 = BoxMean(distance, w, h, std::max(1, static_cast<int>(3 / cell)));
    const std::vector<float> round8 = BoxMean(round3, w, h, std::max(1, static_cast<int>(8 / cell)));
    for (size_t i = 0; i < count; ++i) {
      const float d = distance[i];
      if (d <= 3) continue;
      const float a = Smooth(3, 8, d), b = Smooth(10, 20, d);
      distance[i] = d + (round3[i] - d) * a * (1 - b) + (round8[i] - d) * b;
    }
  }
  // the texture: distance, depth, steepness, openness
  std::vector<uint16_t> texels(count * 4);
  for (size_t i = 0; i < count; ++i) {
    texels[i * 4 + 0] = Half(distance[i]);
    texels[i * 4 + 1] = Half(std::clamp(level - ground[i], -16.0f, 64.0f));
    texels[i * 4 + 2] = Half(steep[i]);
    texels[i * 4 + 3] = Half(open[i]);
  }
  work->w = w;
  work->h = h;
  work->cell = cell;
  // the places on rocky, open shores that throw spray: wet cells on the waterline, the steepest in each square of three
  // world units
  std::vector<Spot>& spots = work->spots;
  const int bin = std::max(1, static_cast<int>(3 / cell));
  const int bw = (w + bin - 1) / bin, bh = (h + bin - 1) / bin;
  std::vector<int> best(static_cast<size_t>(bw) * bh, -1);
  std::vector<float> bestScore(static_cast<size_t>(bw) * bh, 0);
  for (int z = 1; z + 1 < h; ++z)
    for (int x = 1; x + 1 < w; ++x) {
      const size_t at = static_cast<size_t>(z) * w + x;
      if (!wet[at] || (wet[at - 1] && wet[at + 1] && wet[at - w] && wet[at + w])) continue;
      const float score = steep[at] * open[at];
      if (steep[at] < 0.45f || open[at] < 0.2f) continue;
      const size_t b = static_cast<size_t>(z / bin) * bw + x / bin;
      if (score > bestScore[b]) {
        bestScore[b] = score;
        best[b] = static_cast<int>(at);
      }
    }
  for (int at : best) {
    if (at < 0) continue;
    const int x = at % w, z = at / w;
    auto dist = [&](int i, int k) { return distance[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + std::clamp(i, 0, w - 1)]; };
    float gx = dist(x - 1, z) - dist(x + 1, z), gz = dist(x, z - 1) - dist(x, z + 1);  // (toward the land: where the distance falls)
    const float len = std::hypot(gx, gz);
    if (len < 1e-4f) continue;
    gx /= len;
    gz /= len;
    Spot s;
    const float d = distance[static_cast<size_t>(at)];
    s.x = static_cast<float>(x) * cell + gx * d;
    s.z = static_cast<float>(z) * cell + gz * d;
    s.toLandX = gx;
    s.toLandZ = gz;
    s.steep = steep[static_cast<size_t>(at)];
    s.open = open[static_cast<size_t>(at)];
    s.offset = Offset(s.x, s.z);  // [coast] (four stretches' timings blended: once here, not every frame)
    spots.push_back(s);
  }
  work->ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
  if (debug) {
    int shore = 0, rocky = 0, sheltered = 0;
    for (size_t i = 0; i < count; ++i)
      if (wet[i] && distance[i] < cell * 1.5f) {
        ++shore;
        if (steep[i] > 0.5f) ++rocky;
        if (open[i] < 0.2f) ++sheltered;
      }
    printf("render: surf: field %dx%d cells of %.1f units in %.0f ms; %d waterline cells, %d rocky, %d sheltered; %zu spray spots\n", w, h, cell,
           work->ms, shore, rocky, sheltered, spots.size());
  }
  // (MOHO64_DEBUG_SURF_FIELD=<file.ppm>: the field as a picture, north up: the water blue by its distance from the
  // shore (bands of four units), red the steepness, green the openness; the land dark)
  if (const char* path = std::getenv("MOHO64_DEBUG_SURF_FIELD")) {
    if (FILE* f = fopen(path, "wb")) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (size_t i = 0; i < count; ++i) {
        const float d = distance[i];
        const bool band = d > 0 && d < 30 && static_cast<int>(d / 4) % 2 == 0;
        fputc(static_cast<int>(steep[i] * 255), f);
        fputc(static_cast<int>(open[i] * (d > -6 ? 255 : 60)), f);
        fputc(d > 0 ? (band ? 255 : 150) : 0, f);
      }
      fclose(f);
    }
  }
  work->texels = std::move(texels);
  work->ok = true;
}

// [groundfix] (the first frame it is on, or the map's opening with it on: the heights and the water copied, then the rest
// on a thread of its own)
void Surf::StartField() {
  if (tried_) return;
  tried_ = true;
  if (!map_ || !map_->water.present || map_->width <= 0 || map_->height <= 0 || map_->heights.empty()) return;
  const auto started = std::chrono::steady_clock::now();
  level_ = map_->water.elevation;
  work_ = std::make_unique<FieldWork>();
  work_->map = std::make_shared<MapData>();
  work_->map->width = map_->width;
  work_->map->height = map_->height;
  work_->map->heightScale = map_->heightScale;
  work_->map->heights = map_->heights;
  work_->map->water = map_->water;
  workDone_ = false;
  worker_ = std::thread([this, work = work_.get(), debug = debug_] {
    WorkOutField(work, debug);
    workDone_ = true;
  });
  fieldMainMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// [groundfix] (Begin's, once its thread is done: the texture made of what it worked out, the spray's places taken over)
bool Surf::TakeField() {
  if (!worker_.joinable() || !workDone_) return false;
  const auto started = std::chrono::steady_clock::now();
  worker_.join();
  const std::unique_ptr<FieldWork> work = std::move(work_);
  if (!work || !work->ok) return false;
  gfx::TextureDesc td;
  td.width = work->w;
  td.height = work->h;
  td.format = gfx::Format::RGBA16F;
  td.data = work->texels.data();
  td.dataSize = work->texels.size() * sizeof(uint16_t);
  field_ = device_->CreateTexture(td);
  if (!field_) {
    fprintf(stderr, "render: the surf's field cannot be made\n");
    return false;
  }
  area_[0] = -0.5f * work->cell;
  area_[1] = -0.5f * work->cell;
  area_[2] = 1 / (static_cast<float>(work->w) * work->cell);
  area_[3] = 1 / (static_cast<float>(work->h) * work->cell);
  spots_ = std::move(work->spots);
  fieldMs = work->ms;
  const float took = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
  if (debug_)
    printf("render: surf: the field taken in after %d frames of the surf on; the frames' share of it %.2f ms (the heights copied %.2f, the "
           "texture made %.2f; on its thread %.0f ms)\n", fieldWaited, fieldMainMs + took, fieldMainMs, took, fieldMs);
  fieldMainMs += took;
  return true;
}

void Surf::Begin(float time, bool on, bool side, float rough, const float wind[2], Vec3 eye) {
  side_ = side;
  if (side) return;  // (a picture for something else: the game's frames' surf left as it is)
  on_ = on;
  ships = 0;
  if (!on) {
    lastTime_ = -1;
    return;
  }
  // [groundfix] the field on a thread of its own, the frames going on; taken in when it is done, the surf fading in over a
  // second from then (at once when it was there from the surf's first frame)
  if (!tried_) StartField();
  if (!field_ && TakeField()) fade_ = fieldWaited > 0 ? 0.0f : 1.0f;
  if (!field_ && worker_.joinable()) ++fieldWaited;
  if (lastTime_ >= 0 && time < lastTime_ - 1) puffs_.clear();  // (the clock went back: a run started over)
  const float dt = lastTime_ < 0 ? 0 : std::clamp(time - lastTime_, 0.0f, 0.5f);
  if (field_ && fade_ < 1) fade_ = std::min(1.0f, fade_ + std::max(dt, 1.0f / 120));
  lastTime_ = time;
  time_ = time;
  eye_ = eye;
  wind_[0] = wind[0];
  wind_[1] = wind[1];
  rough = std::clamp(rough, 0.0f, 1.0f);
  // (how rough the sea is follows the weather over some seconds: a storm builds the waves up, not at once)
  rough_ = rough_ < 0 ? rough : rough_ + (rough - rough_) * std::min(1.0f, dt / 8);
  // the waves come every 4.5 seconds on a still day, every 7 in a storm: the phase moved on by what passed (kept below
  // a thousand cycles, so that the card's float holds it finely; the waves' sets repeat after that many)
  phase_ += dt / (4.5f + 2.5f * rough_);
  if (phase_ >= 1000) {
    phase_ -= 1000;
    for (Spot& s : spots_) s.lastCrest = -1e9;
  }
}

void Surf::Constants(float area[4], float look[4], float look2[4], float look3[4]) const {
  std::memcpy(area, area_, sizeof area_);
  const float r = std::max(rough_, 0.0f);
  // the waves' height near the shore, their phase, their length out at sea and how high they run up the beach
  look[0] = (0.07f + 0.23f * r) * fade_;  // ([groundfix] fade_: 1 but while fading in)
  look[1] = static_cast<float>(phase_);
  look[2] = 6 + 4 * r;
  look[3] = std::min(0.06f + 0.3f * r, 0.42f) * fade_;
  look2[0] = r;
  look2[1] = breakDepth;
  look2[2] = 0.6f;
  look2[3] = strength * fade_;
  look3[0] = static_cast<float>(view);
  look3[1] = look3[2] = look3[3] = 0;
}

const Surf::Kind& Surf::KindOf(const std::string& blueprint) {
  auto known = kinds_.find(blueprint);
  if (known != kinds_.end()) return known->second;
  Kind& k = kinds_[blueprint];
  BpValue bp;
  if (readBlueprint && readBlueprint(blueprint, &bp)) {
    const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
    k.ship = motion == "RULEUMT_Water" || motion == "RULEUMT_SurfacingSub";
    k.maxSpeed = static_cast<float>(bp["Physics"]["MaxSpeed"].NumberOr(5));
  }
  return k;
}

float Surf::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) / 16777216.0f;
}

void Surf::Throw(const Puff& p) {
  puffs_.push_back(p);
  while (puffs_.size() > static_cast<size_t>(kMostPuffs)) puffs_.pop_front();  // (the oldest let go first)
}

void Surf::AddShip(uint32_t id, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation) {
  if (!on_ || side_ || !model || !model->ok || blueprint.empty() || sprayStrength <= 0) return;
  if (std::fabs(position.y - level_) > 0.6f) return;  // (not on the water: under it, or in the air)
  const Kind& kind = KindOf(blueprint);
  if (!kind.ship) return;
  Ship& s = ships_[id];
  const float dt = time_ - s.seen;
  // (first seen, after a gap, or moved farther than it can go: from here on)
  if (s.seen < 0 || dt < 0 || dt > 0.6f || Length(position - s.place) > std::max(kind.maxSpeed, 1.0f) * dt * 3 + 2) {
    s = Ship{};
    s.place = position;
    s.seen = time_;
    s.slam = Random();
    return;
  }
  if (dt < 1e-4f) return;  // (the same moment drawn again)
  s.velocity = s.velocity + ((position - s.place) * (1 / dt) - s.velocity) * std::min(1.0f, dt * 6);
  s.place = position;
  s.seen = time_;
  if (Length(position - eye_) > farthest) return;
  const float speed = std::hypot(s.velocity.x, s.velocity.z);
  const float share = std::clamp(speed / std::max(kind.maxSpeed, 0.5f), 0.0f, 1.2f);
  if (share < 0.25f) return;
  ++ships;
  const Vec3 lo = model->boundsMin * model->scale, hi = model->boundsMax * model->scale;
  const float beam = std::clamp(hi.x - lo.x, 0.4f, 6.0f), half = beam * 0.5f;
  Vec3 forward = Rotate(orientation, {0, 0, 1});
  forward.y = 0;
  forward = Length(forward) > 1e-4f ? Normalize(forward) : Vec3{0, 0, 1};
  const Vec3 side{forward.z, 0, -forward.x};
  const Vec3 bow = Vec3{position.x, level_ + 0.04f, position.z} + forward * (hi.z * 0.88f);
  const float rough = std::max(rough_, 0.0f);
  const float fast = Smooth(0.25f, 1.0f, share);
  // a sheet of spray off each side of the bow (a pair at a time), more the faster it goes and the rougher the sea
  s.carry += 26 * fast * fast * (0.55f + 0.9f * rough) * std::clamp(half, 0.5f, 1.5f) * sprayStrength * dt;
  int thrown = 0;
  auto sheet = [&](float amount, float out) {
    Puff p;
    p.at = bow + side * (out * half * (0.3f + 0.25f * Random())) - forward * (Random() * 0.5f * half);
    p.velocity = side * (out * (0.9f + 1.5f * fast) * (0.6f + 0.8f * Random())) + forward * (speed * 0.55f) +
                 Vec3{0, (0.7f + 1.6f * fast * (0.6f + 0.8f * rough)) * (0.6f + 0.6f * Random()) * amount, 0};
    p.born = time_ - Random() * dt;
    p.life = 0.55f + 0.5f * Random();
    p.size0 = std::clamp(0.09f * beam, 0.06f, 0.4f);
    p.size1 = std::clamp(0.22f * beam * (0.6f + 0.6f * fast), 0.16f, 1.1f) * (0.8f + 0.4f * Random());
    p.strength = 0.45f * (0.4f + 0.6f * fast) * std::min(sprayStrength, 1.5f) * std::min(amount, 1.6f);
    p.kind = 2;
    p.seed = Random();
    Throw(p);
  };
  for (; s.carry >= 1 && thrown < 6; ++thrown) {
    s.carry -= 1;
    sheet(1, -1);
    sheet(1, 1);
  }
  // in a rough sea the bow meets a wave now and then: a burst (and a little mist, off with the wind)
  if (rough > 0.3f) {
    const float before = s.slam;
    s.slam += dt / (1.7f + 0.8f * static_cast<float>(id % 7) / 7);
    if (std::floor(s.slam) != std::floor(before)) {
      const float amount = Smooth(0.3f, 1.0f, rough) * (0.6f + 0.4f * fast) * (0.6f + 0.8f * Random());
      for (int k = 0; k < 2 + static_cast<int>(5 * amount); ++k) {
        sheet(1.4f + amount, -1);
        sheet(1.4f + amount, 1);
      }
      Puff m;
      m.kind = 1;
      m.at = bow + Vec3{0, 0.3f, 0};
      m.velocity = forward * (speed * 0.4f) + Vec3{0, 0.6f, 0};
      m.born = time_;
      m.life = 1.8f + Random();
      m.size0 = 0.4f * beam;
      m.size1 = 1.2f * beam * (0.6f + 0.5f * amount);
      m.strength = 0.12f * amount * std::min(sprayStrength, 1.5f);
      m.seed = Random();
      Throw(m);
    }
  }
}

void Surf::Burst(Spot& spot, float amount) {
  ++bursts;
  const Vec3 toLand{spot.toLandX, 0, spot.toLandZ};
  const Vec3 along{spot.toLandZ, 0, -spot.toLandX};
  const Vec3 at{spot.x, level_ + 0.05f, spot.z};
  const float up = 2.0f + 5.0f * amount;
  // thrown up the rock face and off it in a fan, along a stretch of the shore
  const int n = 8 + static_cast<int>(22 * amount);
  for (int k = 0; k < n; ++k) {
    Puff p;
    const float a = Random() - 0.5f;
    p.at = at + along * (a * 2.6f) + toLand * (Random() * 0.35f - 0.2f);
    p.velocity = Vec3{0, up * (0.5f + 0.65f * Random()), 0} + toLand * (up * (0.05f + 0.3f * Random())) + along * (a * 1.6f) -
                 toLand * (Random() * 0.9f);
    p.born = time_ + Random() * 0.15f;
    p.life = 0.8f + 0.6f * Random();
    p.size0 = (0.1f + 0.1f * Random()) * (0.6f + amount);
    p.size1 = (0.35f + 0.35f * Random()) * (0.5f + amount);
    p.strength = 0.8f * (0.5f + 0.5f * amount) * std::min(sprayStrength, 1.5f);
    p.seed = Random();
    Throw(p);
  }
  // and a mist off it, drifting with the wind
  const int mists = 1 + static_cast<int>(4 * amount);
  for (int k = 0; k < mists; ++k) {
    Puff m;
    m.kind = 1;
    m.at = at + along * ((Random() - 0.5f) * 2.0f) + Vec3{0, up * (0.15f + 0.2f * Random()), 0};
    m.velocity = Vec3{0, up * 0.25f, 0} + toLand * (0.3f * up) + along * ((Random() - 0.5f) * 0.8f);
    m.born = time_ + 0.1f + Random() * 0.25f;
    m.life = 2.2f + 1.6f * Random();
    m.size0 = 0.5f * (0.6f + amount);
    m.size1 = (1.8f + 1.2f * Random()) * (0.5f + amount);
    m.strength = 0.22f * amount * std::min(sprayStrength, 1.5f);
    m.seed = Random();
    Throw(m);
  }
}

bool Surf::Init() {
  if (shaderTried_) return shaderTried_ > 0;
  shaderTried_ = -1;
  std::string error;
  const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                             kViewShaderBlock + kLightingShaderBlock + kSprayShader;
  gfx::PipelineDesc pd;
  pd.name = "surf: spray";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shader tests the scene's depth itself)
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, &error);
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  if (pipeline_) rows_ = device_->CreateBuffer(bd);
  if (!rows_) {
    fprintf(stderr, "render: the surf's spray cannot be drawn (%s)\n", error.empty() ? "no buffer" : error.c_str());
    return false;
  }
  shaderTried_ = 1;
  return true;
}

void Surf::Prepare(const View& view) {
  drawn_ = 0;
  if (!on_ || side_) return;
  // ships not seen for a while are gone
  for (auto it = ships_.begin(); it != ships_.end();)
    it = time_ - it->second.seen > 2 || it->second.seen > time_ ? ships_.erase(it) : std::next(it);
  // the rocks: where a wave arrives now (its crest's number goes up), spray thrown by its size, the sea's roughness and
  // the rock's; only where the eye may see it
  const float rough = std::max(rough_, 0.0f);
  const float rougher = 0.15f + 0.85f * rough * rough;
  for (Spot& s : spots_) {
    const double crest = std::floor(phase_ + static_cast<double>(s.offset));
    if (crest <= s.lastCrest) continue;
    const bool first = s.lastCrest < -1e8;
    s.lastCrest = crest;
    if (first || sprayStrength <= 0) continue;
    const float far = std::hypot(s.x - eye_.x, s.z - eye_.z);
    if (far > farthest || !view.BoxVisible({s.x - 3, level_ - 1, s.z - 3}, {s.x + 3, level_ + 4, s.z + 3})) continue;
    const float amount = Strength(static_cast<float>(crest), s.x, s.z) * s.open * s.steep * rougher;
    if (amount * fade_ > 0.12f) Burst(s, std::min(amount * 1.2f, 1.2f) * fade_);  // ([groundfix] fade_: 1 but while fading in)
  }
  // what is over let go of (the longest life is under five seconds)
  while (!puffs_.empty() && time_ - puffs_.front().born > 5) puffs_.pop_front();
  if (puffs_.empty() || !Init()) return;
  rowData_.clear();
  const float drift = std::fabs(wind_[0]) + std::fabs(wind_[1]);
  for (const Puff& p : puffs_) {
    const float age = time_ - p.born;
    if (age >= p.life) continue;
    const float reach = p.size1 + Length(p.velocity) + drift * std::max(age, 0.0f);
    if (!view.BoxVisible(p.at - Vec3{reach, reach, reach}, p.at + Vec3{reach, reach + 2, reach})) continue;
    rowData_.insert(rowData_.end(), {p.at.x, p.at.y, p.at.z, age, p.velocity.x, p.velocity.y, p.velocity.z, p.life, p.size0, p.size1, p.strength,
                                     static_cast<float>(p.kind) + std::min(p.seed, 0.999f)});
    ++drawn_;
  }
  if (!rowData_.empty()) device_->UpdateBuffer(rows_, rowData_.data(), static_cast<uint32_t>(rowData_.size() * sizeof(float)));
  if (debug_ >= 2 && time_ - debugAt_ >= 1) {
    debugAt_ = time_;
    printf("render: surf at %.1f s: phase %.2f, rough %.2f, %zu puffs kept, %d drawn, %d bursts so far, %d ships throwing spray\n", time_, phase_,
           rough_, puffs_.size(), drawn_, bursts, ships);
  }
}

void Surf::DrawSpray(const View& view, gfx::Texture sceneDepth, float zNear, bool lit) {
  if (drawn_ <= 0 || shaderTried_ <= 0) return;
  // the camera's sideways and upward directions, from the rows of the view-projection matrix (as the particles take them)
  const Mat4& m = view.constants.viewProj;
  const Vec3 r = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), up = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  const float k[16] = {r.x, r.y, r.z, zNear, up.x, up.y, up.z, level_, wind_[0], wind_[1], lit ? 1.0f : 0.0f, 0, farthest * 0.6f, 1 / (farthest * 0.4f), 0, debug_ >= 4 ? 2.0f : debug_ >= 3 ? 1.0f : 0.0f};
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k, sizeof k);
  device_->SetShaderBuffer(rows_);
  device_->SetTexture(3, sceneDepth);
  device_->Draw(6, 0, static_cast<uint32_t>(drawn_));
  device_->SetTexture(3, {});
}

// --- the viewer's test ---

bool SurfTest::Init(const std::string& spec, Vec3 at, float radius, const std::function<bool(const std::string&, BpValue*)>& readBlueprint,
                    const std::function<float(float, float)>& depth, float waterLevel, std::string* error) {
  level_ = waterLevel;
  uint32_t r = 977;
  auto next = [&r]() {
    r = r * 1664525u + 1013904223u;
    return static_cast<float>(r >> 8) / 16777216.0f;
  };
  // (a loop is used where the water is deep enough all round it)
  auto deepAround = [&](float x, float z, float loop) {
    for (int k = 0; k < 24; ++k) {
      const float a = static_cast<float>(k) * 6.2831853f / 24;
      if (depth(x + std::cos(a) * loop, z + std::sin(a) * loop) < 1.5f) return false;
    }
    return depth(x, z) >= 1.5f;
  };
  for (size_t from = 0; from < spec.size();) {
    const size_t comma = spec.find(',', from);
    const std::string item = spec.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
    from = comma == std::string::npos ? spec.size() : comma + 1;
    const size_t eq = item.find('=');
    std::string id = item.substr(0, eq);
    for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const int count = eq == std::string::npos ? 1 : atoi(item.c_str() + eq + 1);
    BpValue bp;
    if (id.empty() || count <= 0 || !readBlueprint || !readBlueprint(id, &bp)) {
      if (error) *error += (error->empty() ? "" : "; ") + id + " cannot be read";
      continue;
    }
    const float speed = static_cast<float>(bp["Physics"]["MaxSpeed"].NumberOr(5));
    blueprints_.push_back(id);
    int placed = 0;
    for (int i = 0; i < count; ++i) {
      Driver d;
      d.blueprint = static_cast<uint32_t>(blueprints_.size() - 1);
      d.radius = 9 + 7 * next();
      bool found = false;
      for (int tries = 0; tries < 200 && !found; ++tries) {
        const float a = next() * 6.2831853f, far = radius * std::sqrt(next());
        d.x = at.x + std::cos(a) * far;
        d.z = at.z + std::sin(a) * far;
        found = deepAround(d.x, d.z, d.radius);
      }
      if (!found) continue;
      d.speed = speed;
      d.angle = next() * 6.2831853f;
      d.turn = next() < 0.5f ? 1.0f : -1.0f;
      drivers_.push_back(d);
      ++placed;
    }
    char b[160];
    snprintf(b, sizeof b, "%s%d of %s (%.1f a second)", describe_.empty() ? "" : ", ", placed, id.c_str(), speed);
    describe_ += b;
  }
  frame_.blueprints = &blueprints_;
  if (drivers_.empty() && error && error->empty()) *error = "no deep water near enough for a loop";
  return !drivers_.empty();
}

const Frame& SurfTest::Update(double clock) {
  frame_.units.clear();
  for (size_t k = 0; k < drivers_.size(); ++k) {
    const Driver& d = drivers_[k];
    // (where it is on its loop follows from the clock alone)
    const float angle = d.angle + d.turn * d.speed / d.radius * static_cast<float>(clock);
    const Vec3 p{d.x + std::cos(angle) * d.radius, level_, d.z + std::sin(angle) * d.radius};
    const float heading = std::atan2(-std::sin(angle) * d.turn, std::cos(angle) * d.turn);  // (a model's front is its +z)
    FrameUnit u;
    u.id = static_cast<uint32_t>(5001 + k);
    u.blueprint = d.blueprint;
    u.position[0] = p.x, u.position[1] = p.y, u.position[2] = p.z;
    u.orientation[1] = std::sin(heading * 0.5f);
    u.orientation[3] = std::cos(heading * 0.5f);
    u.health = u.maxHealth = 1;
    u.layer = 8;
    frame_.units.push_back(u);
  }
  return frame_;
}

}  // namespace render
