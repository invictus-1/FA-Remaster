#include "air.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lighting.h"
#include "lights.h"
#include "sunlight.h"  // [sunlight]

namespace render {
namespace {

constexpr int kAirLayersPast = 10;  // (airfix) the summed volume's layers past its cells: view.h's kAirLayers

// What both shaders are told (b1): the camera's axes for the cells' rays, the volume's size and range,
// last frame's view for the blending, and the strengths.
struct AirConstants {
  float right[4];      // xyz: the camera's right x tan(half the view's width)
  float up[4];         // xyz: up x tan(half the view's height); w: where in each cell's depth this frame samples (0..1)
  float ahead[4];      // xyz: forward; w: how much of last frame's a cell keeps (0: none)
  float cells[4];      // cells across, down, deep; w (airfix) the places each cell takes this frame
  float range[4];      // x the nearest distance, y log(farthest / nearest), z the air's own thickness, w the lamps' light in it
  Mat4 lastViewProj;   // last frame's
  float lastEye[4];    // xyz: last frame's eye; w: last frame's log(farthest / nearest)
  float light[4];      // x the sun's forward scattering, y the lamps', z the sun's light in the air, w the sky's
  float lastRange[4];  // x last frame's nearest distance; y the height the haze and dust thin above; z 1 / their scale height; w the share of a lamp's glow to all sides the air takes
  float points[4];     // x the other small lights' (flashes, glows) light in the air
  float ground[4];     // (airfix) x the ground's height unit (0: no heights, nothing left out); y, z the map's width and height; w the water's level (-1e9: none)
};

const char kShader[] = R"hlsl(
cbuffer Air : register(b1) {
  float4 airRight;
  float4 airUp;
  float4 airAhead;
  float4 airCells;
  float4 airRange;
  row_major float4x4 lastViewProj;
  float4 lastEye;
  float4 airLight;
  float4 lastRange;
  float4 airPoints;
  float4 airGround;
};

// Henyey-Greenstein: how much of the light goes off at an angle whose cosine is `c` (g > 0: forward).
float AirPhase(float c, float g) {
  float g2 = g * g;
  return (1 - g2) / (4 * 3.14159265 * pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5));
}
float CellDistance(float t) { return airRange.x * exp(t * airRange.y); }  // (t: 0 .. 1 along the volume's depth)
float3 CellRay(float2 uv) { return normalize(airAhead.xyz + airRight.xyz * (uv.x * 2 - 1) + airUp.xyz * (1 - uv.y * 2)); }
// [weather] How wide a cell is `t` world units from the eye (the mist's banks are read for so wide a place: view.h, MistBanks).
float CellWidth(float t) { return t * 2 * length(airRight.xyz) / max(airCells.x, 1); }

// (airfix) The ground: there is no air under it. A column of cells goes on past where it meets the ground (or the water),
// and the mist is thickest low down: what the cells there gathered under the ground was laid over the ground itself as
// rings of fog round the eye, one to a cell. So the cell a column meets the ground in gathers over its stretch above the
// ground only, Sum ends that cell's sum where the column meets the ground and says where that is (in the summed volume's
// last layer) for AirAt (view.h), which takes the cell a place lies in only as far as the place. The cells past it are
// read only by what is seen past the place the column's middle meets the ground (the far sea and land when the camera
// looks along them, the land behind a ridge): they gather the air just over the ground under them.
Texture2D<float> airHeights : register(t7);  // the ground's heights (Terrain::HeightTexture)
// The ground's height under a place: the map's heights (past its edge mirrored, as the world beyond lays them), or the
// water's level where that is higher (as sunlight.cpp's ShaftGround).
float AirGround(float2 xz) {
  float2 m = MirrorUv(xz / airGround.yz) * airGround.yz;
  return max(airHeights.SampleLevel(samplerLinearClamp, (m + 0.5) / (airGround.yz + 1), 0) * airGround.x, airGround.w);
}
// How far along `ray` the stretch from `t0` to `t1` lies above the ground: `t1` when its far end does (a ridge it dips
// under between them is not looked for), `t0` when its near end lies under it already, else where it meets the ground.
// (airgrid) Halved down seven times, then taken where the line through the last two places' heights over the ground
// crosses it: exact over the water and flat ground, and nearly so elsewhere. The halving alone put it on steps of a
// 128th of the cell (the middle of the last stretch), which from a column to the next jumped by a step now and then: rings
// round the point under the eye, closer and closer outward, that the volume's columns took up as a grid of squares
// within squares (Chris's "Weird shapes on map", 2026-10-21: from far up over the sea at night in snow and fog).
float AirOverGround(float3 ray, float t0, float t1) {
  if (airGround.x <= 0) return t1;
  float3 far = cameraPosition + ray * t1;
  float overHi = far.y - AirGround(far.xz);
  if (overHi >= 0) return t1;
  float3 near = cameraPosition + ray * t0;
  float overLo = near.y - AirGround(near.xz);
  if (overLo < 0) return t0;
  float lo = t0, hi = t1;
  [unroll] for (int i = 0; i < 7; ++i) {
    float t = (lo + hi) * 0.5;
    float3 p = cameraPosition + ray * t;
    float over = p.y - AirGround(p.xz);
    if (over >= 0) lo = t, overLo = over;
    else hi = t, overHi = over;
  }
  return lerp(lo, hi, saturate(overLo / max(overLo - overHi, 1e-6)));
}

// How thick the air is at a place (the share of light lost per world unit): its own dust and the haze of
// the weather, thinning with height above the ground, and the mist on low ground in its drifting banks (as
// view.h's MistLost has it).
float Thickness(float3 p, out float dust) {
  float thins = exp(-max(p.y - lastRange.y, 0) * lastRange.z);
  dust = airRange.z * thins;
  float t = (airRange.z + weather.z) * thins;
  [branch] if (mist.x > 0) {
    float banks = MistBanks(p.xz, CellWidth(length(p - cameraPosition)));  // ([weather] as wide as a cell there)
    t += mist.x * exp(-max((p.y - mist.y) * mist.z, -1)) * banks;
  }
  return t;
}

// The sun's shadow at a place in the air (one reading of the nearest map that holds it).
float SunInAir(float3 p) {
  if (shadowsOn < 0.5) return 1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float3 s = mul(shadowMatrix[i], float4(p, 1)).xyz;
    if (all(s.xy > 0.0) && all(s.xy < 1.0) && s.z > 0 && s.z < 1) return shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy, i), s.z);
  }
  return 1;
}

// The sky's light from all sides (half from above, half from the ground under it).
float3 SkyInAir() {
  [branch] if (skyZenith.w > 0.5) return (SkyAmbient(float3(0, 1, 0)) + SkyAmbient(float3(0, -1, 0))) * 0.5;
  return (ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, 1, 0), 8).rgb) + ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, -1, 0), 8).rgb)) * 0.5;
}

// The small lights at a place in the air, toward the eye along `ray` (as lighting.h's SmallLights
// for a surface: the grid square under the place, the beams' cones and shadows): the lamps' beams, and
// apart from them the lights to all sides (flashes of explosions, glows).
float3 LampsInAir(float3 p, float3 ray, out float3 points) {
  float3 sum = 0;
  points = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), n = min(uint(square.y), 48u);
      for (uint s = 0; s < n; ++s) {
        uint at = first + s;
        uint light = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + light * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float3 l = normalize(toLight);
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 tint = lightData[uint(lightGridSize.z) + light * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + light * 4 + 2];
        if (beam.w < -1.5) {  // a light to all sides ([nightlight] tint.w: how much of it the air shows, lights.h)
          points += tint.rgb * (tint.w * fade * 3.14159265 * AirPhase(dot(l, ray), airLight.y));
          continue;
        }
        {  // a beam: its cone, its shadow; its glow to all sides near the lamp
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + light * 4 + 3].x;
          float reaches = inside;
          [branch] if (map > -0.5 && inside > 0) reaches *= LampShadow(p, float3(0, 0, 0), uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * tint.w * saturate(1 - far2 * 25) * lastRange.w;
        }
        sum += tint.rgb * (fade * 3.14159265 * AirPhase(dot(l, ray), airLight.y));
      }
    }
  }
  return sum;
}

// (both shaders' input and output: last frame's cells and this frame's for Gather, the gathered cells and
// the sums for Sum)
Texture3D airSource : register(t0);
RWTexture3D<float4> airTarget : register(u0);

#ifdef SUNLIGHT
// (sky) The hazes that lie on the ground are far thinner than a cell is deep once the eye is high: the light shafts' band
// (thinning to 1/e over 2 world units), the low haze under the clouds (over 40), the mist over its floor, in cells some
// 300 units deep at the strategic view. One place a frame anywhere in the cell mostly missed them and now and then, as the
// golden ratio's steps came near the cell's far end, fell into them: that frame's light was many times the cell's, and
// the frames after kept it, so the whole view brightened at once and sank back (the strategic view pulsing every 13 to 34
// frames, the far part of a nearer view flickering). So over the cell's stretch above the ground each haze is taken whole
// (its mean along the stretch, worked out exactly for a height over its floor that changes evenly along the way), and the
// place whose light the cell takes is drawn by how the hazes lie along the stretch: one of them by its share of the whole,
// then a place along the stretch by where that one is thick; the light there counts for each haze by its share of the
// thickness there. On average the cell's light is what it was; it no longer jumps.
// A haze whose thickness goes as exp(-k o), o going evenly from o0 to o1 along a stretch: its mean over the stretch ...
float SpreadMean(float k, float o0, float o1) {
  float a = abs(k * (o1 - o0));
  return exp(-k * min(o0, o1)) * (a < 1e-3 ? 1 - a * 0.5 : (1 - exp(-a)) / a);
}
// ... and a place along it (0 at o0 .. 1 at o1) drawn by where it is thick, from `u` (0..1).
float SpreadPlace(float k, float o0, float o1, float u) {
  float c = k * (o1 - o0), a = abs(c);
  if (a < 1e-3) return u;
  float v = -log(1 - u * (1 - exp(-a))) / a;  // (from its thick end)
  return c > 0 ? v : 1 - v;
}
// The cell's light and thickness (as Gather's sample) from its stretch above the ground, `t0` to `t1` along `ray`;
// `meets`: the stretch ends where the column meets the ground; `u`: this frame's place (0..1).
float4 GatherSpread(float3 ray, float t0, float t1, bool meets, float u) {
  float3 p0 = cameraPosition + ray * t0, p1 = cameraPosition + ray * t1;
  float over0 = max(p0.y - ShaftGround(p0.xz), 0), over1 = meets ? 0 : max(p1.y - ShaftGround(p1.xz), 0);
  // the hazes: the air's own (dust and the weather's, thinning above the floor), the mist, the band, the low haze; each
  // how thick at its floor (c), how fast it thins (k), and its height over its floor at either end (o0, o1)
  float mistFloor = -1 / max(mist.z, 1e-6);
  float4 c = float4(airRange.z + weather.z, mist.x > 0 ? mist.x * MistBanks(p1.xz, CellWidth(t1)) : 0,  // ([weather] a cell's width)
                    shaftBand.z * lerp(shaftMore.x, 1, shaftWoods.SampleLevel(samplerLinearClamp, MirrorUv(p1.xz / shaftGround.yz), 0)), shaftBand.w);
  float4 k = float4(lastRange.z, mist.x > 0 ? mist.z : 0, 3 / max(shaftBand.x, 1e-3), 1 / max(shaftLow.x, 1));
  float4 o0 = float4(max(p0.y - lastRange.y, 0), max(p0.y - mist.y, mistFloor), over0, over0);
  float4 o1 = float4(max(p1.y - lastRange.y, 0), max(p1.y - mist.y, mistFloor), over1, over1);
  float4 mean = c * float4(SpreadMean(k.x, o0.x, o1.x), SpreadMean(k.y, o0.y, o1.y), SpreadMean(k.z, o0.z, o1.z), SpreadMean(k.w, o0.w, o1.w));
  float whole = mean.x + mean.y + mean.z + mean.w;
  float thick = mean.x + mean.y + mean.z + mean.w * 0.5;  // (the low haze loses half as much as it scatters)
  if (whole <= 0) return float4(0, 0, 0, thick);
  // the haze the place is drawn from, and where along the stretch
  float4 upTo = float4(mean.x, mean.x + mean.y, mean.x + mean.y + mean.z, whole) / whole;
  int which = u < upTo.x ? 0 : u < upTo.y ? 1 : u < upTo.z ? 2 : 3;
  float from = which == 0 ? 0 : upTo[which - 1];
  float s = SpreadPlace(k[which], o0[which], o1[which], saturate((u - from) / max(upTo[which] - from, 1e-6)));
  float3 p = cameraPosition + ray * lerp(t0, t1, s);
  float3 q = float3(p.x, max(p.y, AirGround(p.xz) + 1), p.z);  // (its light a unit over the ground at least: not in its own shadow)
  // each haze's share of the thickness there, as the means have them
  float4 here = c * exp(-k * lerp(o0, o1, s));
  float4 share = here / max(here.x + here.y + here.z + here.w, 1e-30);
  float over = lerp(over0, over1, s), away = length(p - cameraPosition);
  float near = share.x + share.y + share.z, thin = near + share.w * 0.5, dust = share.x * airRange.z / max(c.x, 1e-30);
  float3 falls = ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265) * SunInAir(q) * CloudShadow(q) * SmokeShadow(q);
  float cs = dot(sunDirection, ray);
  float3 sun = falls * (AirPhase(cs, airLight.x) * airLight.z * near * (1 - ShaftShare(over, away)) + AirPhase(cs, shaftLow.y) * airLight.z * share.w * (1 - LowShare(away)));
  float3 points;
  float3 beams = LampsInAir(q, ray, points);
  float3 scattered = sun + SkyInAir() * (modern.y * airLight.w) * thin + beams * (thin + dust * max(airRange.w - 1, 0)) +
                     points * (thin + dust * max(airPoints.x - 1, 0));
  return float4(scattered * whole, thick);
}
#endif

[numthreads(8, 8, 1)] void Gather(uint3 id : SV_DispatchThreadID) {
  if (any(id >= uint3(airCells.xyz))) return;
  float2 uv = (id.xy + 0.5) / airCells.xy;
  float3 ray = CellRay(uv);
  // (airfix) this frame's place in the cell: in the cell the column meets the ground in, in its stretch above it; under the
  // ground (past it), just over the ground there
  float t0 = CellDistance(id.z / airCells.z), t1 = CellDistance((id.z + 1) / airCells.z);
  float clearTo = AirOverGround(ray, t0, t1);
  // (airfix) a weather cut's first frame (Air::Restart) takes several places, evenly spread over the cell: one alone, at the
  // same share of every cell's depth, is off the cells' mean all alike, and the frames after would keep that
  float4 now = 0;
  uint samples = max(uint(airCells.w), 1u);
  for (uint i = 0; i < samples; ++i) {
    float jitter = frac(airUp.w + float(i) / samples);
#ifdef SUNLIGHT
    [branch] if (clearTo > t0) {  // (sky) a stretch above the ground: the hazes on it taken whole (GatherSpread)
      now += GatherSpread(ray, t0, clearTo, clearTo < t1, jitter);
      continue;
    }
#endif
    float at = clearTo < t1 && clearTo > t0 ? t0 * exp(jitter * log(clearTo / t0)) : CellDistance((id.z + jitter) / airCells.z);
    float3 p = cameraPosition + ray * at;
    [branch] if (clearTo < t1) {  // (a unit over it: not in the ground's own shadow)
      float g = AirGround(p.xz);
      if (p.y < g) p.y = g + 1;
    }
    float dust;
    float thick = Thickness(p, dust);
    float3 scattered = 0;
#ifdef SUNLIGHT
    // [sunlight] (sunlight.h; built only for the light shafts) the low band's haze and the haze under the clouds' layer
    // thicken the air; the band's sunlight near the eye is the shafts' pass's (ShaftShare), worked out there finely; the
    // haze under the clouds (half as much lost as it scatters: thin, its light mostly forward) shows the clouds' shadow
    // as rays.
    float over = p.y - ShaftGround(p.xz);
    float band = ShaftHaze(p, over), under = LowHaze(over);
    float near = thick + band;
    thick = near + under * 0.5;
    [branch] if (thick > 0) {
      float3 falls = ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265) * SunInAir(p) * CloudShadow(p) * SmokeShadow(p);
      float c = dot(sunDirection, ray);
      float3 sun = falls * (AirPhase(c, airLight.x) * airLight.z * near * (1 - ShaftShare(over, length(p - cameraPosition))) + AirPhase(c, shaftLow.y) * airLight.z * under * (1 - LowShare(length(p - cameraPosition))));
      float3 points;
      float3 beams = LampsInAir(p, ray, points);
      scattered = sun + SkyInAir() * (modern.y * airLight.w) * thick + beams * (thick + dust * max(airRange.w - 1, 0)) +
                  points * (thick + dust * max(airPoints.x - 1, 0));
    }
#else
    [branch] if (thick > 0) {
      float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265) * AirPhase(dot(sunDirection, ray), airLight.x) * SunInAir(p) * CloudShadow(p) * SmokeShadow(p);
      // (the lamps' light strengthened on the clear air's dust alone: in mist and fog a beam shows by itself)
      float3 points;
      float3 beams = LampsInAir(p, ray, points);
      scattered = (sun * airLight.z + SkyInAir() * (modern.y * airLight.w)) * thick + beams * (thick + dust * max(airRange.w - 1, 0)) +
                  points * (thick + dust * max(airPoints.x - 1, 0));
    }
#endif
    now += float4(scattered, thick);
  }
  now /= samples;
  // what the cell held last frame, read where its middle was then
  [branch] if (airAhead.w > 0) {
    float3 middle = cameraPosition + ray * CellDistance((id.z + 0.5) / airCells.z);
    // (airfix) where the column meets the ground in it: the middle of its stretch above the ground, and the cell that holds
    // that last frame, not blended with the one before or after it (in motion they were, and arcs showed)
    bool snap = clearTo < t1 && clearTo > t0;
    if (snap) middle = cameraPosition + ray * sqrt(t0 * clearTo);
    float4 c = mul(lastViewProj, float4(middle, 1));
    if (c.w > 1e-3) {
      float2 lastUv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
      float lastT = log(max(length(middle - lastEye.xyz), lastRange.x) / lastRange.x) / lastEye.w;
      if (snap) lastT = (floor(lastT * airCells.z) + 0.5) / airCells.z;
      if (all(lastUv >= 0) && all(lastUv <= 1) && lastT <= 1) now = lerp(now, airSource.SampleLevel(samplerLinearClamp, float3(lastUv, lastT), 0), airAhead.w);
    }
  }
  airTarget[id] = now;
}

// Along each column from the eye outward: the light each cell throws toward the eye, dimmed by the air
// before it (what a cell of even thickness and light gives over its length, so a thick cell is not too bright).
// (airfix) The cell the column meets the ground in only as far as the ground, the next one on from there (what lies past is
// seen only past the place the column's middle meets the ground: the far sea and land when the camera looks along them, and
// by the next column's places as the volume is filtered). Into the layers past the cells (view.h, kAirLayers): where the
// column meets the ground (a share of the volume's depth by the log of the distance; 2: it never does; in two parts, the
// second what the first's half float loses, a thousand times over), and the column's sums at the places before it and just
// past it that AirAt reads near the ground (the cells' sums there taken by how the air is spread over them, AirShareOf).
[numthreads(8, 8, 1)] void Sum(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(airCells.xy))) return;
  float3 ray = CellRay((id.xy + 0.5) / airCells.xy);
  float3 light = 0;
  float through = 1, start = 0, ground = 2, meets = 0, lastStart = 0, growth = exp(airRange.y / airCells.z);
  float4 lastFrom = float4(0, 0, 0, 1), atGround = float4(0, 0, 0, 1);
  uint depth = uint(airCells.z), layer = depth + 1 + kAirPast;  // (the layer of the column's sum at the ground)
  int past = 0;  // (1: this cell is the one after the ground's)
  // where the column meets the ground: the first cell whose far end lies under it (four cells' ends read at a time: one
  // thread a column, each reading waiting on the last), then halved down in it (AirOverGround, as Gather has it)
  uint metCell = depth;
  float metAt = 0;
  [branch] if (airGround.x > 0) {
    for (uint s = 0; s < depth && metCell == depth; s += 4) {
      float4 under;
      [unroll] for (uint i = 0; i < 4; ++i) {
        float3 p = cameraPosition + ray * CellDistance(min(s + i + 1, depth) / airCells.z);
        under[i] = p.y < AirGround(p.xz) ? 1 : 0;
      }
      metCell = under.x > 0 ? s : under.y > 0 ? s + 1 : under.z > 0 ? s + 2 : under.w > 0 ? s + 3 : depth;
    }
    metCell = min(metCell, depth);
    if (metCell < depth) metAt = AirOverGround(ray, CellDistance(metCell / airCells.z), CellDistance((metCell + 1) / airCells.z));
  }
  for (uint s = 0; s < depth; ++s) {
    float stop = CellDistance((s + 1) / airCells.z);
    bool met = s == metCell;
    if (met) {
      stop = max(metAt, start);
      ground = log(max(stop, airRange.x) / airRange.x) / airRange.y;
    }
    float4 from = float4(light, through);
    float4 g = airSource.Load(int4(id.xy, s, 0));
    float thick = max(g.a, 1e-7);
    float cell = exp(-thick * (stop - start));
    light += through * (g.rgb / thick) * (1 - cell);
    through *= cell;
    float4 into = float4(light, through);
    airTarget[uint3(id.xy, s)] = into;
    float len = meets * (1 - 1 / growth) / kAirAhead;  // (a step of the ground's layers)
    [branch] if (past == 1) {  // the stretch from the ground on, its air alike all along
      for (int j = 1; j <= kAirPast; ++j)
        airTarget[uint3(id.xy, layer - j)] = AirBetween(atGround, into, saturate(j * len / max(stop - start, 1e-4)));
      past = 2;
    }
    [branch] if (met) {  // the places before the ground: in this cell, or the one before
      meets = stop;
      atGround = into;
      past = 1;
      len = meets * (1 - 1 / growth) / kAirAhead;
      float3 p = cameraPosition + ray * meets;
      float mistHere = mist.x > 0 ? mist.x * MistBanks(p.xz, CellWidth(meets)) : 0;  // ([weather] as AirAt reads them)
      float rest = weather.z + airRange.z;
      for (int j = 0; j <= kAirAhead; ++j) {
        float t = max(meets - j * len, lastStart);
        airTarget[uint3(id.xy, layer + j)] = t >= start ? AirBetween(from, into, AirShareOf(t, start, meets, cameraPosition.y, ray.y, mistHere, rest))
                                                        : AirBetween(lastFrom, from, AirShareOf(t, lastStart, start, cameraPosition.y, ray.y, mistHere, rest));
      }
    }
    lastFrom = from;
    lastStart = start;
    start = stop;
  }
  // (a column that never meets the ground, or meets it in the last cell: its layers its last sum)
  if (past < 2)
    for (int j = past == 1 ? 1 : -kAirAhead; j <= kAirPast; ++j) airTarget[uint3(id.xy, layer - j)] = float4(light, through);
  // (the first part cut to the ten bits of a half float's mantissa by its bits: a half float holds it exactly; Vulkan's
  // compiler took f16tof32(f32tof16(x)) for x, the second part was then 0 and the ground's distance had a half float's
  // precision there, in blocks and rings)
  float coarse = asfloat(asuint(ground) & 0xFFFFE000u);
  airTarget[uint3(id.xy, depth)] = float4(coarse, (ground - coarse) * 1024, 0, 1);
}
)hlsl";

}  // namespace

Air::~Air() {
  for (gfx::Texture t : {cells_[0], cells_[1], summed_})
    if (t) device_->Destroy(t);
}

bool Air::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
  gfx::ComputeDesc cd;
  cd.source = source;
  cd.name = "air: gather";
  cd.entry = "Gather";
  gather_ = device_->CreateCompute(cd, error);
  if (!gather_) return false;
  cd.name = "air: sum";
  cd.entry = "Sum";
  sum_ = device_->CreateCompute(cd, error);
  return static_cast<bool>(sum_);
}

bool Air::Size(int width, int height) {
  const int w = std::clamp(across, 16, 512);
  const int h = std::clamp(static_cast<int>(std::lround(static_cast<float>(w) * static_cast<float>(height) / static_cast<float>(std::max(width, 1)))), 8, 512);
  const int d = std::clamp(deep, 8, 256);
  if (w == width_ && h == height_ && d == depth_ && summed_) return true;
  for (gfx::Texture& t : cells_) {
    if (t) device_->Destroy(t);
    t = {};
  }
  if (summed_) device_->Destroy(summed_);
  gfx::TargetDesc td;
  td.storage = true;
  td.width = w;
  td.height = h;
  td.depth = d;
  td.format = gfx::Format::RGBA16F;
  cells_[0] = device_->CreateTarget(td);
  cells_[1] = device_->CreateTarget(td);
  td.depth = d + kAirLayersPast;  // (airfix: and the layers past the cells, view.h's kAirLayers)
  summed_ = device_->CreateTarget(td);
  width_ = w;
  height_ = h;
  depth_ = d;
  haveLast_ = false;
  return cells_[0] && cells_[1] && summed_;
}

void Air::Skip(View* view) {
  std::fill(std::begin(view->constants.air), std::end(view->constants.air), 0.0f);
  built_ = false;
}

void Air::Build(View* view, const Camera& camera, gfx::Texture sunShadows, gfx::Texture lampShadows, Lights* lights, bool on, float floor) {
  ViewConstants& c = view->constants;
  std::fill(std::begin(c.air), std::end(c.air), 0.0f);
  built_ = false;
  if (!on || !gather_ || !sum_) {
    haveLast_ = false;
    return;
  }
  if (!Size(view->width, view->height)) return;
  const float closest = std::max(nearest, 0.05f), farthestNow = std::max(farthest, closest * 2);
  AirConstants k = {};
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float t = std::tan(camera.fovY * 0.5f), aspect = static_cast<float>(view->width) / static_cast<float>(std::max(view->height, 1));
  const Vec3 r = right * (t * aspect), u = up * t, a = back * -1.0f;
  const float r4[4] = {r.x, r.y, r.z, 0}, u4[4] = {u.x, u.y, u.z, 0}, a4[4] = {a.x, a.y, a.z, 0};
  std::memcpy(k.right, r4, sizeof k.right);
  std::memcpy(k.up, u4, sizeof k.up);
  std::memcpy(k.ahead, a4, sizeof k.ahead);
  // this frame's place inside each cell's depth: the golden ratio's steps, which spread evenly over frames
  k.up[3] = std::fmod(static_cast<float>(frame_) * 0.6180339887f, 1.0f);
  k.ahead[3] = haveLast_ ? std::clamp(keep, 0.0f, 0.98f) : 0.0f;
  // (airfix) from a fresh start (a weather cut, Restart; the first frame): four places a cell, evenly spread, and the frames
  // after keep the mean of all since then until that is as much as `keep` keeps (else the first frame's one place, at the
  // same share of every cell's depth, lingered off the mean for a quarter of a second)
  if (haveLast_) k.ahead[3] = std::min(k.ahead[3], counted_ / (counted_ + 1));
  k.cells[3] = haveLast_ ? 1.0f : 4.0f;
  k.cells[0] = static_cast<float>(width_);
  k.cells[1] = static_cast<float>(height_);
  k.cells[2] = static_cast<float>(depth_);
  k.range[0] = closest;
  k.range[1] = std::log(farthestNow / closest);
  k.range[2] = std::max(dust, 0.0f);
  k.range[3] = std::max(lampsInAir, 0.0f);
  k.lastViewProj = lastViewProj_;
  const Vec3 eye = camera.Eye();
  k.lastEye[0] = lastEye_.x;
  k.lastEye[1] = lastEye_.y;
  k.lastEye[2] = lastEye_.z;
  k.lastEye[3] = lastFarthest_ > lastNearest_ && lastNearest_ > 0 ? std::log(lastFarthest_ / lastNearest_) : k.range[1];
  k.lastRange[0] = lastNearest_ > 0 ? lastNearest_ : closest;
  k.lastRange[1] = floor;
  k.lastRange[2] = 1 / std::max(thinsOver, 1.0f);
  k.lastRange[3] = std::max(lampGlowInAir, 0.0f);
  k.points[0] = std::max(pointsInAir, 0.0f);
  // (airfix) the ground, where the cells' air ends
  const bool ground = heights && heightUnit > 0 && mapWidth > 0 && mapHeight > 0;
  k.ground[0] = ground ? heightUnit : 0.0f;
  k.ground[1] = std::max(mapWidth, 1.0f);
  k.ground[2] = std::max(mapHeight, 1.0f);
  k.ground[3] = waterLevel;
  k.light[0] = std::clamp(sunForward, -0.95f, 0.95f);
  k.light[1] = std::clamp(lampForward, -0.95f, 0.95f);
  k.light[2] = std::max(sunInAir, 0.0f);
  k.light[3] = std::max(skyInAir, 0.0f);

  const gfx::Texture last = cells_[1 - now_], gathered = cells_[now_];
  // [sunlight] with the light shafts (sunlight.h): the gather built with their band and haze, made the first time
  gfx::Pipeline gather = gather_;
  bool withSunlight = false;
  if (sunlight && heights && woods) {
    if (!sunlightTried_) {
      sunlightTried_ = true;
      std::string why;
      const std::string source = std::string("#define SUNLIGHT 1\n#define SHAFT_HEIGHTS t5\n#define SHAFT_WOODS t6\n") + kViewShaderBlock + kLightingShaderBlock + kShaftBandShaderBlock + kShader;
      gfx::ComputeDesc cd;
      cd.source = source;
      cd.name = "air: gather, light shafts";
      cd.entry = "Gather";
      gatherSunlight_ = device_->CreateCompute(cd, &why);
      if (!gatherSunlight_) fprintf(stderr, "render: the light in the air cannot take the light shafts (%s)\n", why.c_str());
    }
    if (gatherSunlight_) gather = gatherSunlight_, withSunlight = true;
  }
  device_->SetPipeline(gather);
  if (withSunlight) device_->SetTexture(5, heights), device_->SetTexture(6, woods);
  device_->SetConstants(0, &view->constants, sizeof view->constants);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, last);
  device_->SetTexture(kShadowMapSlot, sunShadows);
  device_->SetTexture(kLampShadowSlot, lampShadows);
  device_->SetTexture(7, ground ? heights : gfx::Texture{});  // (airfix)
  if (lights) lights->Bind();
  device_->SetStorage(0, gathered);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8), static_cast<uint32_t>(depth_));
  device_->SetPipeline(sum_);
  device_->SetTexture(0, gathered);
  device_->SetStorage(0, summed_);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8), 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});
  device_->SetTexture(7, {});
  if (withSunlight) device_->SetTexture(5, {}), device_->SetTexture(6, {});

  lastViewProj_ = view->constants.viewProj;
  lastEye_ = eye;
  lastNearest_ = closest;
  lastFarthest_ = farthestNow;
  counted_ = haveLast_ ? std::min(counted_ + 1, 1000.0f) : k.cells[3];
  haveLast_ = true;
  now_ = 1 - now_;
  ++frame_;
  built_ = true;
  c.air[0] = 1;
  c.air[1] = closest;
  c.air[2] = k.range[1];
  c.air[3] = std::clamp(std::max(c.weather[3], c.mist[3]) + 0.1f, 0.0f, 1.0f);  // (the most the air may hide: a little more than the formulas allowed)
}

}  // namespace render
