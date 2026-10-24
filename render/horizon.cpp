#include "horizon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lighting.h"
#include "ocean.h"
#include "weather.h"

namespace render {
namespace {

// What every part's shader starts with (after the view's and the lighting's blocks): its numbers, the noise, a hash
// for the floes' cells, the light of the air.
const char kCommon[] = R"hlsl(
cbuffer Horizon : register(b1) {
  float4 hzArea;    // x, y the map's width and height; z how far out the world beyond goes (world units); w the water's level
  float4 hzIce;     // x how much ice (0..1); y where the floes begin, z where they close into pack ice (world units out); w the floes' size
  float4 hzDrift;   // xy how far the ice has drifted (world units); zw the fog banks
  float4 hzFog;     // x how thick the banks are (0: none); y how high their middles stand; z where they begin (world units out); w how much fog (0..1)
  float4 hzWind;    // xy the wind's way (x, z); z 1: keep off the playable map (the game's camera); w the clock (seconds)
  float4 hzRight;   // the camera's right x tan(half the view's width)
  float4 hzUp;      // its up x tan(half the view's height)
  float4 hzAhead;   // xyz its forward; w its near distance
  float4 hzGround;  // x the ground's height unit; y how much of the clouds' shadow is carried over the far sea; z the banks' slow change
};
Texture2D hzNoise : register(t0);  // tiling noise, two channels (0..1)

// How far outside the map a place is (world units), from its x and z (view.h's OutsideMap).
float HzOutside(float2 xz) { return length(max(max(-xz, xz - hzArea.xy), 0)); }
// Numbers 0..1 for a cell, the same on every card (whole-number arithmetic only).
uint HzHash(int2 c, uint salt) {
  uint h = (asuint(c.x) * 0x8da6b343u) ^ (asuint(c.y) * 0xd8163841u) ^ (salt * 0xcb1ab31fu);
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}
float HzRand(int2 c, uint salt) { return float(HzHash(c, salt) >> 8) * (1.0 / 16777216.0); }
float2 HzRand2(int2 c, uint salt) {
  uint h = HzHash(c, salt);
  return float2(float(h & 0xffffu), float(h >> 16)) * (1.0 / 65536.0);
}
float2 HzNoise(float2 uv) { return hzNoise.SampleLevel(samplerLinear, uv, 0).rg; }
// Henyey-Greenstein, as the light in the air has it (air.cpp): how much light goes off at an angle whose cosine is c.
float HzPhase(float c, float g) {
  float g2 = g * g;
  return (1 - g2) / (4 * 3.14159265 * pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5));
}
// The light thick air (a fog bank, a veil of rain or snow) at `p` sends toward the eye along `ray`, as the light in
// the air has it (air.cpp's Gather): the sun's (or the moon's), through the clouds' shadow, mostly forward (`g`), and
// some of it scattered on and on inside (it is thick: from above a bank is lit white, not only toward the sun); the
// sky's from round about, half as the light in the air takes it (mostly from the sky above) and half the colour the
// haze has (view.h's AirColor: so that a bank in thick air is never darker than the air round it).
float3 HzLitAir(float3 p, float3 ray, float g, float sunShare, float skyShare) {
  float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 sky = lerp(SkyAmbient(float3(0, 1, 0)), SkyAmbient(float3(0, -1, 0)), 0.25) * modern.y * 0.35 + AirColor() * 0.6;
  float scattered = 3.14159265 * HzPhase(dot(ray, sunDirection), g) + 0.15 * saturate(sunDirection.y * 3 + 0.1);
  return sun * (scattered * sunShare * CloudShadow(p)) + sky * skyShare;
}
// Where a line (from o along d, in x and z) runs through a rectangle lo..hi: from x to y (x >= y: not at all).
float2 HzSpan(float2 o, float2 d, float2 lo, float2 hi) {
  float2 dd = float2(abs(d.x) > 1e-7 ? d.x : 1e-7, abs(d.y) > 1e-7 ? d.y : 1e-7);
  float2 a = (lo - o) / dd, b = (hi - o) / dd;
  float2 n = min(a, b), f = max(a, b);
  return float2(max(n.x, n.y), min(f.x, f.y));
}
// The line of sight through a pixel of the scene (the camera's own, without a temporal upscaler's shift: within a pixel).
float3 HzRay(float2 pixel) {
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return normalize(hzAhead.xyz + hzRight.xyz * ndc.x + hzUp.xyz * ndc.y);
}
)hlsl";

// The scene's depth, for the parts that read it (the fog banks and the showers).
const char kDepth[] = R"hlsl(
#if MSAA
Texture2DMS<float> hzDepth : register(t2);
#else
Texture2D<float> hzDepth : register(t2);
#endif
// How far along `ray` sample `s` of the pixel meets what was drawn (the sky: very far).
float HzSceneAlong(int2 pixel, uint s, float3 ray) {
#if MSAA
  float depth = hzDepth.Load(pixel, s);
#else
  float depth = hzDepth.Load(int3(pixel, 0));
#endif
  return depth > 0 ? hzAhead.w / depth / max(dot(ray, hzAhead.xyz), 1e-4) : 1e30;
}
// The game's camera never has the playable map hidden: what lies before it there is left out. 1: what the line of
// sight ends at (as far as `farthest` along `ray`) lies beyond the map's edge (or is the sky); 0 on the map.
float HzKeep(float3 ray, float farthest) {
  if (hzWind.z < 0.5 || farthest > 1e29) return 1;
  float3 end = cameraPosition + ray * farthest;
  return smoothstep(0, 20, HzOutside(end.xz));
}
)hlsl";

// The icebergs: a few lumps of ice far out, each a ring of fourteen sides in five rings (the sunken base, the
// waterline, half way up, the top's rim, a rounded shoulder above it) closed above and below; its corners pushed in
// and out by its own numbers, its top flat (a tabular berg) or raised to a peak. Its faces lit half as flat (ice
// breaks in planes) and half as the rounded lump it is, each a shade of its own. Lit as the ground is (ModernLight:
// snow lies on what faces up).
const char kBergs[] = R"hlsl(
struct Berg {
  float4 at;     // x, its waterline's height now, z; its radius
  float4 shape;  // its height over the water; how far its top rises in a peak; its turn; a number of its own
};
cbuffer Bergs : register(b2) {
  Berg bergs[16];
};
static const uint kSides = 14;
static const uint kRings = 5;
static const uint kBergVertices = (2 * (kRings - 1) + 2) * kSides * 3;

float3 BergCorner(Berg b, uint side, uint ring) {
  side %= kSides;
  uint salt = uint(b.shape.w);
  bool peaked = b.shape.y > 0.2;
  float lift = HzRand(int2(side, ring), salt + 5u) - 0.5;
  float scale, y;
  if (ring == 0) { scale = 1.25; y = -0.75; }
  else if (ring == 1) { scale = 1.0; y = 0.0; }
  else if (ring == 2) { scale = peaked ? 0.82 : 0.97; y = 0.45 + 0.16 * lift; }
  else if (ring == 3) { scale = peaked ? 0.55 : 0.92; y = 0.85 + 0.1 * lift + (peaked ? b.shape.y * 0.35 * (lift + 0.5) : 0.0); }
  else { scale = peaked ? 0.3 : 0.8; y = 0.97 + 0.05 * lift + (peaked ? b.shape.y * 0.7 * (lift + 0.5) : 0.0); }
  // (the sides a little wavy round it, more on the large swings than from corner to corner)
  float wave = sin(float(side) / float(kSides) * 6.2831853 * 2 + float(salt % 17u)) * 0.12;
  float turn = (float(side) + 0.3 * (HzRand(int2(side, 9), salt) - 0.5)) / float(kSides) * 6.2831853 + b.shape.z;
  float radius = b.at.w * scale * (0.86 + 0.28 * HzRand(int2(side, ring), salt) + wave);
  return float3(b.at.x + cos(turn) * radius, b.at.y + y * b.shape.x, b.at.z + sin(turn) * radius);
}
float3 BergEnd(Berg b, bool top) {
  return top ? float3(b.at.x, b.at.y + b.shape.x * (1 + b.shape.y), b.at.z) : float3(b.at.x, b.at.y - b.shape.x * 0.9, b.at.z);
}
// The three corners of one of a berg's triangles.
void BergTriangle(Berg b, uint tri, out float3 p0, out float3 p1, out float3 p2) {
  if (tri < 2 * (kRings - 1) * kSides) {  // the sides, between ring `band` and the one above it
    uint band = tri / (2 * kSides), r = tri % (2 * kSides), side = r / 2;
    p0 = BergCorner(b, side, band);
    if (r % 2 == 0) {
      p1 = BergCorner(b, side + 1, band);
      p2 = BergCorner(b, side + 1, band + 1);
    } else {
      p1 = BergCorner(b, side + 1, band + 1);
      p2 = BergCorner(b, side, band + 1);
    }
  } else if (tri < (2 * (kRings - 1) + 1) * kSides) {  // the top
    uint side = tri - 2 * (kRings - 1) * kSides;
    p0 = BergCorner(b, side, kRings - 1);
    p1 = BergCorner(b, side + 1, kRings - 1);
    p2 = BergEnd(b, true);
  } else {  // the bottom
    uint side = tri - (2 * (kRings - 1) + 1) * kSides;
    p0 = BergCorner(b, side, 0);
    p1 = BergEnd(b, false);
    p2 = BergCorner(b, side + 1, 0);
  }
}

struct BergOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  nointerpolation float3 normal : TEXCOORD1;  // its face's
  nointerpolation float2 seed : TEXCOORD2;    // the berg's number, the face's shade
  float3 round : TEXCOORD3;                   // which way the rounded lump faces there
};
BergOut VS(uint id : SV_VertexID) {
  Berg b = bergs[id / kBergVertices];
  uint v = id % kBergVertices;
  float3 p[3];
  BergTriangle(b, v / 3, p[0], p[1], p[2]);
  BergOut o;
  o.world = p[v % 3];
  o.position = mul(viewProj, float4(o.world, 1));
  float3 n = normalize(cross(p[1] - p[0], p[2] - p[0]));
  float3 middle = (p[0] + p[1] + p[2]) / 3;
  float3 centre = float3(b.at.x, b.at.y + b.shape.x * 0.3, b.at.z);
  o.normal = dot(n, middle - centre) < 0 ? -n : n;  // (outward)
  float3 away = o.world - centre;
  o.round = away / float3(b.at.w * b.at.w, b.shape.x * b.shape.x * 0.5, b.at.w * b.at.w);  // (as an ellipsoid's)
  o.seed = float2(b.shape.w, HzRand(int2(v / 3, 3), uint(b.shape.w)));
  return o;
}
float4 PS(BergOut i) : SV_Target {
  float3 N = normalize(normalize(i.normal) + normalize(i.round) * 0.8);
  // glacier ice: blue-white, in faint bands, each face a shade of its own; under the water brighter (the water's own
  // colour takes it, water.cpp)
  float3 albedo = float3(0.42, 0.54, 0.61) * (0.88 + 0.08 * sin(i.world.y * 2.7 + i.seed.x) + 0.1 * i.seed.y);
  if (i.world.y < hzArea.w) albedo = float3(0.46, 0.64, 0.68);
  float3 c = ModernLight(albedo, 0, 0.36, 1, 0, N, i.world, SunShadow(i.world, N));
  return float4(BeyondScenery(c, i.world), 1);
}
)hlsl";

// The floes, on the water's surface over the sea beyond the edge (four strips round the map, out to where the world
// beyond ends), tested against the scene's depth (land beyond hides them). Two sizes of floes, each a cell of a
// Voronoi pattern drifting with the wind (its corners rounded, its outline bent a little by the noise): a cell is
// ice when its own number is under how much of the sea is ice where its middle lies (few near the edge, more
// farther out, pack ice toward the horizon), and its floe fills the more of it the more ice there is. Seen from far
// (cells of a few pixels) they go over into their share of the sea. And the clouds' shadows over the far sea.
const char kFloes[] = R"hlsl(
struct FloeOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};
FloeOut VS(uint id : SV_VertexID) {
  uint strip = id / 6, corner = id % 6;
  float2 uv = corner == 0 ? float2(0, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1) : corner == 3 ? float2(0, 0) : corner == 4 ? float2(1, 1) : float2(0, 1);
  float R = hzArea.z, W = hzArea.x, H = hzArea.y;
  float2 lo = strip == 0 ? float2(-R, -R) : strip == 1 ? float2(-R, H) : strip == 2 ? float2(-R, 0) : float2(W, 0);
  float2 hi = strip == 0 ? float2(W + R, 0) : strip == 1 ? float2(W + R, H + R) : strip == 2 ? float2(0, H) : float2(W + R, H);
  FloeOut o;
  float2 xz = lerp(lo, hi, uv);
  o.world = float3(xz.x, hzArea.w + 0.03, xz.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

// How much of the sea is ice that far out (world units).
float IceCover(float outside) {
  float t = saturate((outside - hzIce.y) / max(hzIce.z - hzIce.y, 1));
  return (0.55 + 0.42 * hzIce.x) * pow(t, 0.6);
}
// The ice lies in patches and bands drawn out along the wind, with open water between (0..1 at a place, drifting
// with the ice: `xz` taken back by its drift).
float IcePatches(float2 xz) {
  float2 along = float2(dot(xz, hzWind.xy), dot(xz, float2(-hzWind.y, hzWind.x)));
  float n = HzNoise(along * float2(1.0 / 900, 1.0 / 330) + float2(0.61, 0.17)).r * 0.7 + HzNoise(along * float2(1.0 / 260, 1.0 / 120) + float2(0.29, 0.83)).g * 0.3;
  return smoothstep(0.3, 0.68, n);
}
// Whether the floes of two neighbouring cells are frozen together into one (the same answer asked from either side):
// floes of several cells, of many sizes and shapes, not a honeycomb.
bool FloesJoin(int2 a, int2 b, uint salt, float chance) {
  int2 apart = abs(a - b);
  return HzRand(a + b, salt * 7u + uint(apart.x + 3 * apart.y)) < chance;
}
// One size of floes (cells `size` world units across) at a place: how much of it is ice (0..1); its floe's cell,
// middle (world x, z) and how far inside its edge the place is (world units). `share`: of the ice that far out, how
// much this size takes; `grain`: how much of the sea one pixel covers. Each cell of a Voronoi pattern holds a floe
// or open water; its edge is the soft least (corners rounded) of the distances to the borders with its neighbours,
// leaving out those it is frozen together with.
float FloesOf(float2 xz, float size, uint salt, float share, float grain, out int2 cell, out float2 middle, out float inside) {
  float2 q = (xz - hzDrift.xy) / size;
  q += (HzNoise(q * 0.043 + float(salt) * 0.37) - 0.5) * 0.6;  // (outlines not ruled: bent a little)
  int2 base = int2(floor(q));
  float2 f = q - float2(base);
  float best = 1e9;
  float2 nearest = 0;
  int2 at = base;
  [unroll] for (int j = -1; j <= 1; ++j)
    [unroll] for (int i = -1; i <= 1; ++i) {
      int2 c = base + int2(i, j);
      float2 p = float2(i, j) + 0.15 + 0.7 * HzRand2(c, salt) - f;
      float d = dot(p, p);
      if (d < best) {
        best = d;
        nearest = p;
        at = c;
      }
    }
  cell = at;
  middle = (q + nearest) * size + hzDrift.xy;
  float cover = saturate(IceCover(HzOutside(middle)) * share * lerp(0.3, 1.3, IcePatches(middle - hzDrift.xy)));
  float grow = saturate((cover - HzRand(cell, salt + 11u)) * 6);  // (one whose turn has just come is small, and grows)
  float join = lerp(0.12, 0.62, cover);
  float sum = 0;
  [unroll] for (int j2 = -1; j2 <= 1; ++j2)
    [unroll] for (int i2 = -1; i2 <= 1; ++i2) {
      int2 c = base + int2(i2, j2);
      float2 p = float2(i2, j2) + 0.15 + 0.7 * HzRand2(c, salt) - f;
      float2 dd = p - nearest;
      float l2 = dot(dd, dd);
      if (l2 > 1e-5 && !(HzRand(c, salt + 11u) < cover && FloesJoin(at, c, salt, join))) sum += exp(-dot(0.5 * (nearest + p), dd) * rsqrt(l2) / 0.06);
    }
  float border = sum > 0 ? -0.06 * log(sum) : 0.5;
  // (the open water between them, in cells; each floe a size of its own, the more alike the closer they lie)
  float gap = lerp(0.36, 0.035, cover) * lerp(0.45 + 1.1 * HzRand(cell, salt + 17u), 1, cover * cover);
  inside = (border - gap - 0.3 * (1 - grow)) * size;
  float a = grow > 0 ? smoothstep(-1, 1, inside / max(0.25, grain * 1.5)) : 0;
  // too small to be seen one by one (a floe under about four pixels across, or the cells under five): their share of
  // the sea, so that from far the scattered small floes are never a salting of white points
  float g = lerp(0.36, 0.035, cover);
  float mean = cover * saturate(1 - 2.2 * g) * saturate(1 - 2.2 * g);
  float across = (1 - 2 * gap) * size / max(grain, 1e-4);  // (a floe's width in pixels, about)
  return lerp(mean, a, saturate(size / max(grain * 5, 1e-4) - 0.4) * saturate((across - 2.5) / 3));
}

float4 PS(FloeOut i) : SV_Target {
#if RIDE
  // [whitecaps] (on the lifted waves, World::oceanLift: the floes read where the line of sight meets the lifted surface, as
  // the place on the still water it rests on: they ride the waves; the sheet's depth stays the still water's)
  float2 xz = OceanRideRest(cameraPosition, normalize(i.world - cameraPosition)).xz;
#else
  float2 xz = i.world.xz;
#endif
  float outside = HzOutside(xz);
  float grain = max(length(ddx(xz)), length(ddy(xz)));
  float ice = 0, shade = 0.5, edge = 1;
  float3 N = float3(0, 1, 0);
  [branch] if (hzIce.x > 0 && outside > hzIce.y * 0.5) {
    int2 c1, c2;
    float2 m1, m2;
    float in1, in2 = 0;
    float a1 = FloesOf(xz, hzIce.w, 1u, 1, grain, c1, m1, in1);
    float a2 = 0;
    c2 = c1;
    m2 = m1;
    // the small floes and broken ice between the large ones (where there is room)
    [branch] if (a1 < 0.98) a2 = FloesOf(xz, hzIce.w * 0.31, 2u, 0.8, grain, c2, m2, in2);
    ice = a1 + a2 * (1 - a1);
    bool large = a1 >= a2 * (1 - a1);
    int2 cell = large ? c1 : c2;
    float2 middle = large ? m1 : m2;
    shade = HzRand(cell, 23u);
    edge = saturate((large ? in1 : in2) / 0.3 + 0.2);
    // rocking on the swell as it runs under them, each a little its own way
    float phase = dot(middle, float2(0.86, 0.5)) * 0.27 - hzWind.w * 1.1 + shade * 1.3;
    float2 tilt = float2(0.86, 0.5) * (0.05 * cos(phase)) + (HzRand2(cell, 29u) - 0.5) * 0.04;
    N = normalize(float3(-tilt.x, 1, -tilt.y));
    ice *= smoothstep(hzIce.y * 0.5, hzIce.y, outside);  // (never in the playable map: none over its first units)
  }
  // the clouds' shadows over the far sea: where the sea's own shading goes over into the haze (lighting.h, BeyondFar)
  // their shadows on it are lost with it; carried on there, fading toward the far end, where the haze has the air's
  float dark = 0;
  [branch] if (hzGround.y > 0) {
    float t = saturate(outside / hzArea.z);
    dark = hzGround.y * (1 - CloudShadow(i.world)) * t * t * (3 - 2 * t) * (1 - smoothstep(0.7, 1.0, t));
  }
  if (ice <= 0.002 && dark <= 0.002) discard;
  float3 color = 0;
  [branch] if (ice > 0.002) {
    // sea ice, blue-grey, wet and darker at its edge; the lying snow on it in patches, more on some floes than others
    float3 albedo = float3(0.34, 0.40, 0.44) * (0.85 + 0.3 * shade);
    albedo = lerp(float3(0.17, 0.23, 0.27), albedo, edge);
    surfaceSnowShare = lerp(0.75, 1.0, shade) * edge;
    color = ModernLight(albedo, 0, 0.55, 1, 0, N, i.world, SunShadow(i.world, N));
    // (each floe a shade of its own under the snow too, and the snow on it drifted unevenly)
    color *= lerp(0.88, 1.04, shade) * (0.94 + 0.12 * HzNoise(xz * 0.031 + shade).g);
    color = BeyondScenery(color, i.world);
  }
  return float4(color * ice, 1 - (1 - ice) * (1 - dark));
}
)hlsl";

// A full-screen pass: the fog banks lying on the far sea, marched along each line of sight through the layer they lie
// in, beyond the map's edge only (not where the line goes over the map), as far as the scene's depth.
const char kBanks[] = R"hlsl(
Texture2D<float> hzHeights : register(t3);  // the ground's heights (Terrain::HeightTexture), in units of hzGround.x

// How thick the fog is at a place (share of light lost per world unit).
float BankAt(float3 p) {
  float outside = HzOutside(p.xz);
  float from = smoothstep(hzFog.z, hzFog.z + 60, outside);
  if (from <= 0) return 0;
  // (drawn out along the wind: long low banks, not heaps)
  float2 q = p.xz + hzDrift.zw;
  float2 along = float2(dot(q, hzWind.xy), dot(q, float2(-hzWind.y, hzWind.x)));
  float field = HzNoise(along * float2(1.0 / 1500, 1.0 / 520) + float2(0.13, 0.71)).r * 0.6 +
                HzNoise(along * float2(1.0 / 420, 1.0 / 170) + float2(hzGround.z, 0.37)).g * 0.4;
  float far = saturate(outside / hzArea.z);
  float bar = lerp(0.62, 0.5, hzFog.w) - 0.08 * far;  // (more fog: more of the sea under banks, never all; more toward the horizon)
  float banks = smoothstep(bar - 0.1, bar + 0.38, field) * from * (0.6 + 0.4 * far);
  // only over the sea (the mist lies on low land)
  float2 m = MirrorUv(p.xz / hzArea.xy) * hzArea.xy;
  float ground = hzHeights.SampleLevel(samplerLinearClamp, (m + 0.5) / (hzArea.xy + 1), 0) * hzGround.x;
  banks *= saturate((hzArea.w - ground) * 0.3);
  float top = hzFog.y * (0.4 + 0.8 * banks);  // (the thicker, the higher)
  return hzFog.x * banks * (1 - smoothstep(top * 0.3, top, p.y - hzArea.w));
}

struct BankOut {
  float4 position : SV_Position;
};
BankOut VS(uint id : SV_VertexID) {
  BankOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.position = float4(p * 2 - 1, 0, 1);
  return o;
}
float4 PS(BankOut i, out uint coverage : SV_Coverage) : SV_Target {
  coverage = 0;
  int2 pixel = int2(i.position.xy);
  float3 o = cameraPosition, d = HzRay(i.position.xy);
  // the layer they lie in
  float w0 = hzArea.w, w1 = hzArea.w + hzFog.y * 1.25;
  float ta = 0, tb = 1e30;
  if (abs(d.y) > 1e-6) {
    float a = (w0 - o.y) / d.y, b = (w1 - o.y) / d.y;
    ta = max(min(a, b), 0);
    tb = max(a, b);
  } else if (o.y < w0 || o.y > w1) {
    discard;
  }
  // within the square the world beyond fills
  float2 square = HzSpan(o.xz, d.xz, -hzArea.zz, hzArea.xy + hzArea.zz);
  ta = max(ta, square.x);
  tb = min(tb, square.y);
  float along[SAMPLES];
  float farthest = 0;
  for (uint s = 0; s < SAMPLES; ++s) {
    along[s] = HzSceneAlong(pixel, s, d);
    farthest = max(farthest, along[s]);
  }
  tb = min(tb, farthest);
  if (tb <= ta) discard;
  // the parts of the way before the map and after it
  float2 map = HzSpan(o.xz, d.xz, float2(0, 0), hzArea.xy);
  float a0 = ta, a1 = tb, b0 = 0, b1 = 0;
  if (map.x < map.y) {
    a1 = min(tb, map.x);
    b0 = max(ta, map.y);
    b1 = tb;
  }
  float la = max(a1 - a0, 0), lb = max(b1 - b0, 0);
  if (la + lb <= 0) discard;
  const int kSteps = 12;
  float dt = (la + lb) / kSteps;
  float through = 1, weight = 0, entered = 1e30;
  float3 middle = 0;
  for (int k = 0; k < kSteps; ++k) {
    float u = (k + 0.5) * dt;
    float t = u < la ? a0 + u : b0 + (u - la);
    float3 p = o + d * t;
    float density = BankAt(p);
    if (density > 0) {
      float a = 1 - exp(-density * dt);
      middle += p * (through * a);
      weight += through * a;
      through *= 1 - a;
      entered = min(entered, t - dt * 0.5);
    }
  }
  if (weight < 0.002) discard;
  for (uint s2 = 0; s2 < SAMPLES; ++s2)
    if (along[s2] > entered) coverage |= 1u << s2;
  if (coverage == 0) discard;
  // lit where most of what is seen of it lies, seen through the air, lost in the far haze with the sea under it
  float3 p = middle / weight;
  float3 c = HzLitAir(p, d, 0.55, 1, 1);
  c = BeyondScenery(ThroughAirTo(c, p), p);
  float a = (1 - through) * HzKeep(d, farthest) * (1 - smoothstep(0.9, 1, saturate(HzOutside(p.xz) / hzArea.z)));
  return float4(c * a, a);
}
)hlsl";

// The showers: each a column leaning with the wind from the sea up to the cloud base, drawn over the rectangle its box
// takes on the screen; along each line of sight the way through it (a circle in the layer it leans by) is summed: its
// density highest in its middle, ragged at its edge, thinning into the cloud base, in streaks that fall (rain: fine
// and fast; snow: soft and slow).
const char kShowers[] = R"hlsl(
struct Shower {
  float4 at;    // x, z where it meets the sea; its radius; how strong (0..1)
  float4 look;  // its top (world y); how far it leans per unit of height; 1 = snow; a number of its own
};
cbuffer Showers : register(b2) {
  Shower showers[16];
};
struct ShowerOut {
  float4 position : SV_Position;
  nointerpolation uint index : TEXCOORD0;
};
ShowerOut VS(uint id : SV_VertexID) {
  uint n = id / 6, corner = id % 6;
  Shower s = showers[n];
  float w = hzArea.w, top = s.look.x;
  float2 lean = -hzWind.xy * (s.look.y * (top - w));  // (its top upwind of its foot)
  float2 lo = min(s.at.xy, s.at.xy + lean) - s.at.z, hi = max(s.at.xy, s.at.xy + lean) + s.at.z;
  float2 least = float2(1e9, 1e9), most = float2(-1e9, -1e9);
  bool behind = false;
  for (uint c = 0; c < 8; ++c) {
    float3 p = float3((c & 1u) != 0 ? hi.x : lo.x, (c & 2u) != 0 ? top : w, (c & 4u) != 0 ? hi.y : lo.y);
    float4 h = mul(viewProj, float4(p, 1));
    if (h.w < 1e-3) {
      behind = true;
    } else {
      least = min(least, h.xy / h.w);
      most = max(most, h.xy / h.w);
    }
  }
  if (behind) {
    least = float2(-1, -1);
    most = float2(1, 1);
  }
  least = clamp(least, -1, 1);
  most = clamp(most, -1, 1);
  float2 uv = corner == 0 ? float2(0, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1) : corner == 3 ? float2(0, 0) : corner == 4 ? float2(1, 1) : float2(0, 1);
  ShowerOut o;
  o.position = float4(lerp(least, most, uv), 0, 1);
  o.index = n;
  return o;
}
float4 PS(ShowerOut i, out uint coverage : SV_Coverage) : SV_Target {
  coverage = 0;
  Shower s = showers[i.index];
  int2 pixel = int2(i.position.xy);
  float3 o = cameraPosition, d = HzRay(i.position.xy);
  float w = hzArea.w, top = s.look.x, r = s.at.z;
  // in the frame where the column stands upright (each height moved back by its lean), the way through it is a
  // circle's chord
  float2 lean = -hzWind.xy * s.look.y;
  float2 o2 = o.xz - lean * (o.y - w) - s.at.xy, d2 = d.xz - lean * d.y;
  float A = dot(d2, d2), B = dot(o2, d2), C = dot(o2, o2) - r * r;
  float disc = B * B - A * C;
  if (A < 1e-10 || disc <= 0) discard;
  float root = sqrt(disc);
  float t0 = (-B - root) / A, t1 = (-B + root) / A;
  if (abs(d.y) > 1e-6) {
    float a = (w - o.y) / d.y, b = (top - o.y) / d.y;
    t0 = max(t0, min(a, b));
    t1 = min(t1, max(a, b));
  } else if (o.y < w || o.y > top) {
    discard;
  }
  t0 = max(t0, 0);
  float along[SAMPLES];
  float farthest = 0;
  for (uint s0 = 0; s0 < SAMPLES; ++s0) {
    along[s0] = HzSceneAlong(pixel, s0, d);
    farthest = max(farthest, along[s0]);
  }
  t1 = min(t1, farthest);
  if (t1 <= t0) discard;
  // how far across the column the line passes, as the eye sees it (-1..1): the streaks run along it (looking down
  // the column there is no across: the streaks fade)
  float across = (o2.x * d2.y - o2.y * d2.x) * rsqrt(A) / r;
  float streaky = saturate(sqrt(A) * 2.5 - 0.5);
  bool snowy = s.look.z > 0.5;
  float fall = snowy ? 1.5 : 7.0;  // (how fast the streaks come down, world units a second)
  float depth = 0;
  const int kSteps = 5;
  float dt = (t1 - t0) / kSteps;
  for (int k = 0; k < kSteps; ++k) {
    float3 p = o + d * (t0 + (k + 0.5) * dt);
    float2 rel = p.xz - lean * (p.y - w) - s.at.xy;
    float h = saturate((p.y - w) / max(top - w, 1));
    // (its outline ragged: the noise read over a disc round its middle, each height a little moved)
    float ragged = HzNoise(normalize(rel + 1e-4) * 0.11 + float2(s.look.w * 0.37 + h * 0.21, h * 0.13 + hzWind.w * 0.0015)).r;
    float rho = length(rel) / (r * (0.55 + 0.9 * ragged));
    float radial = saturate(1 - rho * rho);
    float streak = snowy ? 0.6 + 0.8 * HzNoise(float2(across * 1.3 + s.look.w, (p.y + hzWind.w * fall) * 0.01)).g
                         : 0.45 + 1.1 * smoothstep(0.3, 0.75, HzNoise(float2(across * 3.7 + s.look.w * 3, (p.y + hzWind.w * fall) * 0.005)).g);
    streak = lerp(snowy ? 1.0 : 0.85, streak, streaky);
    // (rain thickest toward the sea, where all of it has come down; both thinning into the cloud base)
    depth += radial * radial * (1 - smoothstep(0.55, 1.0, h)) * (snowy ? 1.0 : 1.25 - 0.5 * h) * streak * dt;
  }
  // (a shower is weather on the horizon: seen from high above, looking down on it, it gives way to its cloud)
  float a = (1 - exp(-depth * (snowy ? 0.05 : 0.042))) * s.at.w * HzKeep(d, farthest) * (1 - smoothstep(0.45, 0.85, -d.y));
  if (a <= 0.002) discard;
  for (uint s2 = 0; s2 < SAMPLES; ++s2)
    if (along[s2] > t0) coverage |= 1u << s2;
  if (coverage == 0) discard;
  float3 p = o + d * ((t0 + t1) * 0.5);
  // rain: grey, the sky's light through it, the sun little under its cloud; snow: whiter, sent more to all sides
  float3 c = snowy ? HzLitAir(p, d, 0.3, 0.8, 1.1) : HzLitAir(p, d, 0.5, 0.25, 0.5);
  c = BeyondScenery(ThroughAirTo(c, p), p);
  return float4(c * a, a);
}
)hlsl";

// The vertices of one iceberg (kBergs' kBergVertices: 5 rings of 14 sides, 4 bands of sides, a top and a bottom).
constexpr uint32_t kBergVertices = (2 * (5 - 1) + 2) * 14 * 3;

// A tiling noise of two channels (256 x 256, RG8): five sizes of smooth value noise added, the same every time.
std::vector<uint8_t> MakeNoise() {
  constexpr int kSize = 256;
  std::vector<uint8_t> out(static_cast<size_t>(kSize) * kSize * 2);
  for (int channel = 0; channel < 2; ++channel) {
    std::vector<float> sum(static_cast<size_t>(kSize) * kSize, 0.0f);
    float total = 0, amplitude = 0.5f;
    for (int octave = 0; octave < 5; ++octave) {
      const int cells = 4 << octave;
      const uint32_t seed = 0x51ed27u + static_cast<uint32_t>(channel) * 977u + static_cast<uint32_t>(octave) * 131u;
      auto lattice = [&](int x, int y) {
        x = ((x % cells) + cells) % cells;
        y = ((y % cells) + cells) % cells;
        return WeatherDraw(seed, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
      };
      for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
          const float fx = static_cast<float>(x) * static_cast<float>(cells) / kSize, fy = static_cast<float>(y) * static_cast<float>(cells) / kSize;
          const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
          float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
          tx = tx * tx * (3 - 2 * tx);
          ty = ty * ty * (3 - 2 * ty);
          const float a = lattice(ix, iy), b = lattice(ix + 1, iy), c = lattice(ix, iy + 1), d = lattice(ix + 1, iy + 1);
          sum[static_cast<size_t>(y) * kSize + x] += amplitude * ((a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty);
        }
      total += amplitude;
      amplitude *= 0.5f;
    }
    for (size_t i = 0; i < sum.size(); ++i) out[i * 2 + static_cast<size_t>(channel)] = static_cast<uint8_t>(std::clamp(sum[i] / total, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  return out;
}

float Smooth(float from, float to, float x) {
  const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// How far outside the map's rectangle a place is (world units).
float Outside(float x, float z, float w, float h) {
  const float dx = std::max({-x, x - w, 0.0f}), dz = std::max({-z, z - h, 0.0f});
  return std::sqrt(dx * dx + dz * dz);
}

// A place beyond the map's edge as the map has it there (the ground goes on mirrored, terrain.cpp's MirrorUv).
float Mirror(float v, float size) {
  v = std::fabs(v);
  v = std::fmod(v, 2 * size);
  return v > size ? 2 * size - v : v;
}

}  // namespace

bool Horizon::Make() {
  if (tried_) return made_;
  tried_ = true;
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "#define MSAA 0\n") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                           kViewShaderBlock + kLightingShaderBlock + kCommon;
  // [whitecaps] (the floes' variant that rides the lifted waves: made the first time it is wanted, DrawOnWater)
  floeRideSource_ = std::string("#define MSAA ") + (gSceneSamples > 1 ? "1" : "0") + "\n#define SAMPLES " + std::to_string(gSceneSamples) + "\n#define RIDE 1\n" +
                    kViewShaderBlock + kOceanRideShaderBlock + kLightingShaderBlock + kCommon + kFloes;
  std::string why;
  auto make = [&](const char* name, const std::string& source, bool depth, bool opaque, gfx::Pipeline* out) {
    gfx::PipelineDesc pd;
    pd.name = name;
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTarget = depth;
    pd.depthTest = depth;
    pd.depthWrite = depth && opaque;
    pd.blend = opaque ? gfx::Blend::Opaque : gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    *out = device_->CreatePipeline(pd, &why);
    if (!*out) fprintf(stderr, "render: the weather on the horizon: %s cannot be made (%s)\n", name, why.c_str());
    return static_cast<bool>(*out);
  };
  const std::string bergs = head + kBergs, floes = head + kFloes, banks = head + kDepth + kBanks, curtains = head + kDepth + kShowers;
  made_ = make("horizon: icebergs", bergs, true, true, &bergPipeline_) && make("horizon: floes", floes, true, false, &floePipeline_) &&
          make("horizon: fog banks", banks, false, false, &bankPipeline_) && make("horizon: showers", curtains, false, false, &showerPipeline_);
  if (made_) {
    const std::vector<uint8_t> noise = MakeNoise();
    gfx::TextureDesc td;
    td.width = td.height = 256;
    td.format = gfx::Format::RG8;
    td.data = noise.data();
    td.dataSize = noise.size();
    noise_ = device_->CreateTexture(td);
    made_ = static_cast<bool>(noise_);
  }
  return made_;
}

bool Horizon::Begin(const MapData& map, float reach, const Weather& weather, float time, const Camera& camera, int width, int height) {
  iceNow = fogNow = 0;
  showersNow = bergsNow = 0;
  onWater_ = inAir_ = false;
  showerCount_ = bergCount_ = 0;
  if (reach <= 0 || map.width <= 0 || map.height <= 0 || !Make()) return false;
  const float W = static_cast<float>(map.width), H = static_cast<float>(map.height), R = reach;
  const bool sea = map.water.present;
  const float level = sea ? map.water.elevation : 0.0f;
  // the wind: the way the clouds drift (sky.cpp: their field moves by -(5, 2.2) world units a second)
  const float windX = -5.0f / std::sqrt(5.0f * 5.0f + 2.2f * 2.2f), windZ = -2.2f / std::sqrt(5.0f * 5.0f + 2.2f * 2.2f);
  // (the same for every player of a game: from the map and the weather's seed)
  const uint32_t seed = weather.seed ^ (static_cast<uint32_t>(map.width) * 73856093u) ^ (static_cast<uint32_t>(map.height) * 19349663u);
  auto rise = [](float from, float to, float h) { return Smooth(from, to, h); };

  // winter ice: with the lying snow; pack ice nearer as more lies
  const float iceAmount = sea && ice ? std::clamp(weather.snowLying, 0.0f, 1.0f) : 0.0f;
  iceNow = iceAmount;
  // fog banks: some always far out; more at dawn and after damp nights (the mist's own reckoning, World::mistNow),
  // in fog, and in the cold
  const float cold = std::max(weather.snowLying, weather.cold ? 0.6f : 0.0f);
  const float dawn = rise(4, 5.5f, weather.hour) * (1 - rise(8, 10.5f, weather.hour));
  const float fogAmount = sea && fog ? std::clamp(0.15f + 0.5f * weather.mist + 0.25f * dawn + 0.3f * cold + 0.35f * weather.fog + 0.1f * weather.wet, 0.0f, 1.0f) : 0.0f;
  fogNow = fogAmount;
  // showers: in rain and snow, more and stronger the harder it falls, a few under heavy cloud
  const float showerAmount = showers ? std::max({weather.rain, 0.85f * weather.snow, 0.5f * std::clamp((weather.cloudCover - 0.55f) * 1.6f, 0.0f, 1.0f)}) : 0.0f;
  const bool snowy = weather.snow > weather.rain;

  Constants& k = k_;
  k = {};
  k.area[0] = W;
  k.area[1] = H;
  k.area[2] = R;
  k.area[3] = level;
  k.ice[0] = iceAmount;
  k.ice[1] = std::max(6.0f, 0.012f * R);
  k.ice[2] = R * (0.9f - 0.52f * iceAmount);
  k.ice[3] = std::max(floeSize, 1.0f);
  k.drift[0] = windX * iceDrift * time;
  k.drift[1] = windZ * iceDrift * time;
  k.drift[2] = -windX * 0.5f * time;  // (the banks' noise is read at place + drift: it moves against it)
  k.drift[3] = -windZ * 0.5f * time;
  k.fog[0] = fogAmount > 0 ? (0.025f + 0.08f * fogAmount) * fogThickness : 0.0f;
  k.fog[1] = fogHeight * (1 + 0.4f * weather.fog);
  k.fog[2] = std::max(20.0f, 0.04f * R);
  k.fog[3] = fogAmount;
  k.wind[0] = windX;
  k.wind[1] = windZ;
  k.wind[2] = camera.cinematic ? 0.0f : 1.0f;
  k.wind[3] = time;
  {
    Vec3 right, up, back;
    camera.Axes(&right, &up, &back);
    const float t = std::tan(camera.fovY * 0.5f), aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
    const float r4[4] = {right.x * t * aspect, right.y * t * aspect, right.z * t * aspect, 0}, u4[4] = {up.x * t, up.y * t, up.z * t, 0};
    const float a4[4] = {-back.x, -back.y, -back.z, camera.zNear};
    std::memcpy(k.right, r4, sizeof r4);
    std::memcpy(k.up, u4, sizeof u4);
    std::memcpy(k.ahead, a4, sizeof a4);
  }
  // (the clouds' shadows carried: a pattern only where the cover is broken; under an even overcast, as in rain, the
  // far haze is already the overcast's and darkening the far water alone would only set it apart from the land)
  const float cover = std::clamp(weather.cloudCover, 0.0f, 1.0f);
  k.ground[1] = shadows ? shadowCarry * 4 * cover * (1 - cover) : 0.0f;
  k.ground[2] = std::fmod(time * 0.0007f, 1.0f);
  onWater_ = sea && (iceAmount > 0 || k.ground[1] > 0);

  // the showers: each from a place of its own drawn by the seed, passing with the wind round a square that holds the
  // world beyond (coming back on the far side), seen only well out from the edge and fading toward the far end
  const int wanted = static_cast<int>(std::lround(static_cast<float>(kMostShowers) * std::clamp(showerAmount, 0.0f, 1.0f)));
  const float side = W + 2 * R, depth = H + 2 * R;
  for (int i = 0; i < wanted && i < kMostShowers; ++i) {
    const uint32_t n = static_cast<uint32_t>(i);
    float x = -R + WeatherDraw(seed, 401, n) * side + windX * showerSpeed * (0.7f + 0.6f * WeatherDraw(seed, 405, n)) * time;
    float z = -R + WeatherDraw(seed, 402, n) * depth + windZ * showerSpeed * (0.7f + 0.6f * WeatherDraw(seed, 405, n)) * time;
    x = -R + std::fmod(std::fmod(x + R, side) + side, side);
    z = -R + std::fmod(std::fmod(z + R, depth) + depth, depth);
    const float out = Outside(x, z, W, H);
    const float shows = Smooth(0.28f * R, 0.42f * R, out) * (1 - Smooth(0.82f * R, 0.97f * R, out));
    const float strength = (0.55f + 0.45f * WeatherDraw(seed, 403, n)) * std::min(1.0f, showerAmount * 1.4f) * shows;
    if (strength < 0.01f) continue;
    Shower& s = showers_[showerCount_++];
    s.at[0] = x;
    s.at[1] = z;
    s.at[2] = (snowy ? 30.0f : 22.0f) + 30.0f * WeatherDraw(seed, 404, n);
    s.at[3] = strength;
    s.look[0] = std::max(level + 60.0f, 150.0f);  // (to the clouds' underside, 150 over the ground's zero: clouds.h)
    s.look[1] = (snowy ? 0.4f : 0.22f) + 0.18f * WeatherDraw(seed, 406, n);
    s.look[2] = snowy ? 1.0f : 0.0f;
    s.look[3] = WeatherDraw(seed, 407, n) * 37.0f;
  }
  showersNow = showerCount_;

  // the icebergs: placed once for the map and the seed where the sea is deep enough (as the map has it there,
  // mirrored), well out from the edge; they rise as the snow comes, rock a little and drift to and fro
  if (placedFor_[0] != W || placedFor_[1] != H || placedFor_[2] != level || placedFor_[3] != static_cast<float>(seed & 0xffffff)) {
    placedFor_[0] = W;
    placedFor_[1] = H;
    placedFor_[2] = level;
    placedFor_[3] = static_cast<float>(seed & 0xffffff);
    placedCount_ = 0;
    for (int i = 0; sea && i < kMostBergs; ++i) {
      for (uint32_t tries = 0; tries < 40; ++tries) {
        const uint32_t n = static_cast<uint32_t>(i) * 64u + tries;
        const float x = -R + WeatherDraw(seed, 501, n) * (W + 2 * R), z = -R + WeatherDraw(seed, 502, n) * (H + 2 * R);
        const float out = Outside(x, z, W, H);
        if (out < 0.16f * R || out > 0.62f * R) continue;
        const float radius = 2.5f + 4.5f * WeatherDraw(seed, 503, n) * WeatherDraw(seed, 504, n);
        bool deep = true;
        for (int a = 0; a < 5 && deep; ++a) {  // (its middle and round it: in deep water)
          const float px = x + (a == 1 ? 2.0f : a == 2 ? -2.0f : 0.0f) * radius, pz = z + (a == 3 ? 2.0f : a == 4 ? -2.0f : 0.0f) * radius;
          deep = map.Height(Mirror(px, W), Mirror(pz, H)) < level - 4;
        }
        if (!deep) continue;
        Berg& b = placed_[placedCount_++];
        const bool tabular = WeatherDraw(seed, 505, n) < 0.6f;
        b.at[0] = x;
        b.at[1] = level;
        b.at[2] = z;
        b.at[3] = radius;
        b.shape[0] = radius * (tabular ? 0.35f + 0.25f * WeatherDraw(seed, 506, n) : 0.7f + 0.4f * WeatherDraw(seed, 506, n));
        b.shape[1] = tabular ? 0.04f : 0.25f + 0.35f * WeatherDraw(seed, 507, n);
        b.shape[2] = WeatherDraw(seed, 508, n) * 6.2831853f;
        b.shape[3] = static_cast<float>(1 + (WeatherDraw(seed, 509, n) * 60000.0f));
        break;
      }
    }
  }
  for (int i = 0; i < placedCount_ && iceAmount > 0; ++i) {
    const float from = 0.25f + 0.035f * static_cast<float>(i);
    const float grown = std::clamp((iceAmount - from) / 0.12f, 0.0f, 1.0f);
    if (grown <= 0) continue;
    Berg b = placed_[i];
    const float phase = b.shape[3] * 0.013f;
    const float drift = 5.0f * std::sin(time * 0.0015f + phase);
    b.at[0] += windX * drift;
    b.at[2] += windZ * drift;
    b.at[1] = level + 0.07f * std::sin(time * 0.6f + phase) - (1 - grown) * b.shape[0];  // (bobbing; rising as the ice comes)
    bergs_[bergCount_++] = b;
  }
  bergsNow = bergCount_;
  if (debug_ && (showerCount_ != toldShowers_ || bergCount_ != toldBergs_)) {  // (MOHO64_DEBUG_HORIZON: where they are, when that changes)
    toldShowers_ = showerCount_;
    toldBergs_ = bergCount_;
    fprintf(stderr, "render: horizon: ice %.2f, fog %.2f (density %.3f), %d showers, %d icebergs\n", iceAmount, fogAmount, k.fog[0], showerCount_, bergCount_);
    for (int i = 0; i < showerCount_; ++i)
      fprintf(stderr, "render: horizon:   shower at %.0f, %.0f, radius %.0f, strength %.2f%s\n", showers_[i].at[0], showers_[i].at[1], showers_[i].at[2], showers_[i].at[3],
              showers_[i].look[2] > 0.5f ? " (snow)" : "");
    for (int i = 0; i < bergCount_; ++i)
      fprintf(stderr, "render: horizon:   iceberg at %.0f, %.0f, radius %.1f, height %.1f\n", bergs_[i].at[0], bergs_[i].at[2], bergs_[i].at[3], bergs_[i].shape[0]);
  }
  inAir_ = k.fog[0] > 0 || showerCount_ > 0;
  return onWater_ || inAir_ || bergCount_ > 0;
}

void Horizon::DrawSolid(const View& view) {
  if (!made_ || bergCount_ == 0) return;
  device_->SetPipeline(bergPipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k_, sizeof k_);
  device_->SetConstants(2, bergs_, sizeof bergs_);
  device_->Draw(static_cast<uint32_t>(bergCount_) * kBergVertices);
}

void Horizon::DrawOnWater(const View& view) {
  if (!made_ || !onWater_) return;
  // [whitecaps] on the lifted waves (ride, World::oceanLift): the floes' variant that rides them (made the first time)
  if (ride && !floeRideTried_) {
    floeRideTried_ = true;
    gfx::PipelineDesc pd;
    pd.name = "horizon: floes, riding the waves";
    pd.vertexSource = pd.pixelSource = floeRideSource_;
    pd.cull = gfx::Cull::None;
    pd.depthTarget = pd.depthTest = true;
    pd.depthWrite = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    std::string why;
    floeRide_ = device_->CreatePipeline(pd, &why);
    if (!floeRide_) fprintf(stderr, "render: the weather on the horizon: the floes cannot ride the waves (%s)\n", why.c_str());
  }
  const bool riding = ride && floeRide_;
  device_->SetPipeline(riding ? floeRide_ : floePipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k_, sizeof k_);
  device_->SetTexture(0, noise_);
  if (riding) {
    device_->SetConstants(3, ride->RideConstants(), ride->RideConstantsSize());
    ride->BindRide();
  }
  device_->Draw(24);
  if (riding) ride->UnbindRide();
}

void Horizon::DrawInAir(const View& view, gfx::Texture sceneDepth, gfx::Texture heights, float heightUnit) {
  if (!made_ || !inAir_) return;
  k_.ground[0] = heightUnit;
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, noise_);
  device_->SetTexture(2, sceneDepth);
  device_->SetTexture(3, heights);
  if (k_.fog[0] > 0 && heights) {
    device_->SetPipeline(bankPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &k_, sizeof k_);
    device_->Draw(3);
  }
  if (showerCount_ > 0) {
    device_->SetPipeline(showerPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &k_, sizeof k_);
    device_->SetConstants(2, showers_, sizeof showers_);
    device_->Draw(static_cast<uint32_t>(showerCount_) * 6);
  }
  device_->SetTexture(2, {});  // (the scene's depth is drawn into again)
}

bool Horizon::Set(const std::string& list, std::string* why) {
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string item = list.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      if (why) *why = "'" + item + "' is not name=value";
      return false;
    }
    const std::string name = item.substr(0, eq);
    const float value = static_cast<float>(std::atof(item.c_str() + eq + 1));
    if (name == "floe") floeSize = value;
    else if (name == "drift") iceDrift = value;
    else if (name == "fogheight") fogHeight = value;
    else if (name == "fog") fogThickness = value;
    else if (name == "showerspeed") showerSpeed = value;
    else if (name == "shadow") shadowCarry = value;
    else {
      if (why) *why = "no such number '" + name + "' (floe, drift, fogheight, fog, showerspeed, shadow)";
      return false;
    }
  }
  return true;
}

}  // namespace render
