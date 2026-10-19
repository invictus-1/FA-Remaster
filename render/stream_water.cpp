#include "stream_water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "lighting.h"
#include "ray_scene.h"

namespace render {
namespace {

// The streams' shaders (stream_water.h says what they draw). Both read the scene's depth themselves and say per sample
// of the pixel whether the stream lies over it there, as the sea's do (water.cpp): the water writes its finished colour,
// the white water is laid over it, premultiplied.
const char kShader[] = R"hlsl(
cbuffer StreamConstants : register(b1) {
  float4 row0, row1, row2;  // the prop's place: its world matrix's top three rows
  float4 cameraRight;       // w: tan(half the field of view)
  float4 cameraUp;          // w: aspect
  float4 cameraBack;        // w: near distance
  float4 mapInfo;           // x, y the map's width and height; z its height unit; w 1 for a white water ribbon
  float4 streamLook;        // x the deepest water, as a share of the half width; y how murky; z its speed, times; w a view for checking
};
Texture2D behind : register(t0);  // (kStreamBehindSlot)
#if SAMPLES > 1
Texture2DMS<float> sceneDepth : register(t1);  // (kStreamDepthSlot)
#define SCENE_DEPTH(at, s) sceneDepth.Load(at, s)
#else
Texture2D<float> sceneDepth : register(t1);
#define SCENE_DEPTH(at, s) sceneDepth.Load(int3(at, 0))
#endif
Texture2D<float> heightMap : register(t2);  // (kStreamHeightsSlot)
Texture2D ripplesMap : register(t3);        // (kStreamRipplesSlot)
Texture2D foamMap : register(t4);           // (kStreamFoamSlot)

static const float kWetBank = 0.2;  // how far past the water's edge the bank is wet (world units)
static const float kPeriod = 1.7;   // seconds a layer of the flow is carried before it starts again

struct VIn {
  float3 position : POSITION;
  float3 across : TEXCOORD0;  // toward the bank at u = 1
  float3 along : TEXCOORD1;   // downstream
  float4 shape : TEXCOORD2;   // u; how far downstream; the half width; the drop per unit downstream
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 across : TEXCOORD1;
  float3 along : TEXCOORD2;
  float4 shape : TEXCOORD3;  // x across from the middle (world units, toward u = 1); y how far downstream; z the half width; w the drop per unit
};

float3 Turned(float3 d) { return float3(dot(row0.xyz, d), dot(row1.xyz, d), dot(row2.xyz, d)); }

VOut VS(VIn v) {
  float3 across = Turned(v.across), along = Turned(v.along);
  float scale = max(length(across), 1e-6);  // (the prop's own scale: the direction was of unit length)
  across /= scale;
  along = normalize(along);
  float side = v.shape.x * 2 - 1;  // -1 the bank at u = 0, +1 the other
  float halfW = v.shape.z * scale;
  // the water's ribbon reaches a little past its banks, and a little up (banks rise), for the wet ground there
  float reach = mapInfo.w > 0.5 ? 0 : kWetBank;
  float3 world = float3(dot(row0.xyz, v.position) + row0.w, dot(row1.xyz, v.position) + row1.w, dot(row2.xyz, v.position) + row2.w);
  world += across * (side * reach) + float3(0, abs(side) * reach * 0.5, 0);
  VOut o;
  o.world = world;
  o.position = mul(viewProj, float4(world, 1));
  o.across = across;
  o.along = along;
  o.shape = float4(side * (halfW + reach), v.shape.y * scale, halfW, v.shape.w);
  return o;
}

// Where a pixel's depth lies in the world (as the sea's, water.cpp).
float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / max(depth, 1e-6);
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

// The map's ground's height (its heights between their points, as the ground cover reads them).
float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}

// Which of the pixel's samples the stream lies over: those where what the scene has is behind its surface, or is the
// ground a hair over it (between the ribbon's points the ground can bulge through it: the water covers that too). And
// the depth of the farthest of them.
uint Covered(float4 position, float3 world, float distance, out float depthBehind) {
  int2 pixel = int2(position.xy);
  uint coverage = 0;
  depthBehind = 1;
  [unroll] for (int s = 0; s < SAMPLES; ++s) {
    float d = SCENE_DEPTH(pixel, s);
    bool under = d <= position.z;  // (a nearer thing has the larger depth)
    if (!under) {
      float3 p = WorldAt(position.xy, d);
      under = p.y < world.y + 0.03 && distance - length(p - cameraPosition) < 0.2;
    }
    if (under) {
      coverage |= 1u << s;
      depthBehind = min(depthBehind, d);
    }
  }
  return coverage;
}

// The flow's two layers: each carried downstream at `speed` for a period, then starting again from where it began, the
// second half a period behind; `offsets` how far each is carried now (world units), `weight` the first's share (the
// second's the rest): each is gone at the moment it starts again.
void FlowLayers(float speed, out float2 offsets, out float weight) {
  float2 phase = frac(time / kPeriod + float2(0, 0.5));
  offsets = (phase - 0.5) * (kPeriod * speed);
  weight = 1 - abs(phase.x * 2 - 1);
}

// Foam's lace on running water: noise of two sizes, drawn out along the flow, the finer turned against the first;
// 0..1, mostly about a half. q: across, downstream (world units). A size finer than two pixels is left at its mean.
float StreamLace(float2 q, float grain) {
  float t = time * 0.35;
  float2 shown = saturate(1.5 - grain * float2(3.4, 8.5) * 2);
  float n = 0.5, m = 0.5;
  if (shown.x > 0) n = CloudNoise(float3(q * float2(3.4, 1.4), t));
  if (shown.y > 0) m = CloudNoise(float3(float2(q.x * 0.8 - q.y * 0.6, q.x * 0.6 + q.y * 0.8) * float2(8.5, 3.6) + 17.0, t * 1.6));
  return lerp(0.5, n, shown.x) * 0.62 + lerp(0.5, m, shown.y) * 0.38;
}

// The ripples' tilt (across, downstream) from the visuals track's ripple picture: its green the normal's lean down v, its
// alpha one minus its lean across u (stream_build.py); one picture every 2.5 world units, as the mesh lays it.
float2 RippleTilt(float2 q) {
  float4 t = ripplesMap.Sample(samplerAniso, q / 2.5);
  return float2(1 - 2 * t.a, 2 * t.g - 1);
}

// Whether something stands up out of the water at `p` (a place on its surface): what the picture shows there is in front
// of the surface, stands clear of the ground (a rock, a unit; not the bank, which is ground) and goes down into the water
// there. 0..1.
float StandsAt(float3 p) {
  float4 c = mul(viewProj, float4(p, 1));
  if (c.w <= 0) return 0;
  float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
  if (any(at < 0) || any(at > 1)) return 0;
  float there = SCENE_DEPTH(int2(at * viewport.xy), 0);
  if (there <= c.z / c.w) return 0;
  float3 q = WorldAt(at * viewport.xy, there);
  // (what the eye meets on the way to the place, just short of it: a rock's side going into the water there; not a leaf
  // hanging over it, which the eye meets well before)
  float stands = saturate((q.y - GroundAt(q.xz) - 0.05) * 20) * saturate(1.5 - length(q - p) / 0.12);
  [branch] if (stands > 0) {
    // (and is solid: a rock's face runs on smoothly from pixel to pixel; a tree's needles hanging over the water break
    // up into what lies behind them)
    int2 px = int2(at * viewport.xy);
    float3 right = WorldAt(float2(px + int2(2, 0)), SCENE_DEPTH(min(px + int2(2, 0), int2(viewport.xy) - 1), 0));
    float3 down = WorldAt(float2(px + int2(0, 2)), SCENE_DEPTH(min(px + int2(0, 2), int2(viewport.xy) - 1), 0));
    stands *= saturate(1.5 - max(length(right - q), length(down - q)) / (0.03 + length(q - cameraPosition) * 0.004));
  }
  [branch] if (stands > 0) {
    // (and it goes down into the water: what the picture shows straight under it, at the water's level, is itself; under
    // a branch hanging low over the water it is the water)
    float3 foot = float3(q.x, p.y, q.z);
    float4 f = mul(viewProj, float4(foot, 1));
    float2 under = float2(f.x / f.w * 0.5 + 0.5, 0.5 - f.y / f.w * 0.5);
    float below = SCENE_DEPTH(int2(saturate(under) * (viewport.xy - 1)), 0);
    if (f.w <= 0 || below <= f.z / f.w) return 0;
    stands *= saturate(1.5 - length(WorldAt(saturate(under) * (viewport.xy - 1), below).xz - foot.xz) / 0.25);
  }
  return stands;
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float3 toPixel = normalize(i.world - cameraPosition);
  float distance = length(i.world - cameraPosition);
  float depthBehind;
  coverage = Covered(i.position, i.world, distance, depthBehind);
  if (coverage == 0) discard;
  float across = i.shape.x, downstream = i.shape.y, halfW = max(i.shape.z, 0.05), slope = max(i.shape.w, 0);
  float3 acrossDir = normalize(i.across), alongDir = normalize(i.along);
  float grain = max(length(ddx(i.world)), length(ddy(i.world)));  // (the ground one pixel covers)
  float2 uv = i.position.xy * viewport.zw;
  float3 straight = behind.SampleLevel(samplerLinearClamp, uv, 0).rgb;

  // the water's edge, wavering along each bank and lapping a little; `inside`: how far in from it (below 0: the bank)
  float sideOf = across >= 0 ? 1 : -1;
  float waver = (CloudNoise(float3(downstream * 1.3, sideOf * 5.0, 0.5)) - 0.5) * min(0.16, halfW * 0.3) + sin(time * 1.7 + downstream * 2.3) * 0.006;
  float inside = halfW - abs(across) + waver;
  // the bank it has wetted: darker, a little glossy
  float wet = saturate(1 + inside / kWetBank);
  wet *= wet;
  float3 up = float3(0, 1, 0);
  float shadow = SunShadow(i.world, up) * CloudShadow(i.world);
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 skyLight = skyZenith.w > 0.5 ? SkyAmbient(up) : sunLight * 0.3;
  float3 sheenDir = reflect(toPixel, up);
  sheenDir.y = max(sheenDir.y, 0.02);
  float3 sky = skyZenith.w > 0.5 ? ComputedSky(normalize(sheenDir)) : ToLinear(environmentSky.SampleLevel(samplerLinear, sheenDir, 0).rgb) * (lightMultiplier * modern.x * 0.5);
  float3 wetBank = lerp(straight * lerp(1, 0.6, wet), ThroughAirTo(sky, i.world), wet * 0.06);
  if (inside <= 0) {
    if (wet < 0.002 || streamLook.w > 0.5) {
      coverage = 0;
      discard;
    }
    return float4(wetBank, 1);
  }

  // the bed's shape across: none at the banks, deepest in the middle; pools and shallows along it
  float x = saturate(abs(across) / halfW);
  float pools = 0.7 + 0.6 * CloudNoise(float3(downstream * 0.11, 3.7, 1.3));
  float bedDepth = streamLook.x * halfW * (1 - x * x) * pools * saturate(inside / 0.03);
  // its speed: faster where it drops
  float speed = streamLook.z * min(0.14 + 1.5 * sqrt(slope), 2.4);
  float2 offsets;
  float w;
  FlowLayers(speed, offsets, w);
  float2 q = float2(across, downstream);

  // the surface: as steep as the stream runs; the ripples carried downstream on it, rougher where it runs fast
  float2 tilt = RippleTilt(q - float2(0, offsets.x)) * w + RippleTilt(q - float2(0, offsets.y) + float2(0.93, 1.71)) * (1 - w);
  tilt *= (0.7 + 1.6 * saturate(slope * 3)) / max(sqrt(w * w + (1 - w) * (1 - w)), 0.5);
  // near the eye: a finer chop, drifting
  float fine = saturate(1.5 - grain * 8.0);
  [branch] if (fine > 0) {
    const float e = 0.05;
    float t = time * 0.9;
    float2 p = float2(q.x * 4.5, (q.y - time * 0.3 * streamLook.z) * 2.6);
    float h = CloudNoise(float3(p, t));
    float2 g = float2(CloudNoise(float3(p + float2(e, 0), t)) - h, CloudNoise(float3(p + float2(0, e), t)) - h) / e;
    tilt += g * float2(4.5, 2.6) * (fine * 0.02 * (1 + 2 * saturate(slope * 3)));
  }
  float3 N = up + alongDir * slope + acrossDir * tilt.x + alongDir * tilt.y;
  // rain: rings where the drops fall, and the whole surface roughened
  float rings = 0;
  [branch] if (falling.x > 0 && distance < 160) {
    float2 g = i.world.xz * 3.0;
    float2 cell = floor(g);
    float h = CloudHash(float3(cell, 3.1));
    float age = frac(time * (0.9 + h) + h * 9.0);
    float2 from = g - (cell + 0.2 + 0.6 * float2(CloudHash(float3(cell, 6.7)), CloudHash(float3(cell, 11.3))));
    float away = length(from);
    float ring = saturate(1 - abs(away - age * 0.5) * 7) * (1 - age) * step(h, 0.1 + 0.35 * falling.x) * saturate(1 - distance / 160);
    N += float3(from.x, 0, from.y) * (ring * 0.9 / max(away, 0.05));
    N += float3(CloudNoise(float3(i.world.xz * 12, time * 3)) - 0.5, 0, CloudNoise(float3(i.world.xz * 12 + 5, time * 3)) - 0.5) * (0.3 * falling.x);
    rings = ring;
  }
  N = normalize(N);

  // what stands in it: foam where the water meets it (looks round the place), streaks downstream of it (looks upstream)
  float meet = 0, wake = 0;
  [branch] if (grain < 0.06) {
    float3 sideways = acrossDir * 0.15, ahead = alongDir * 0.15;
    meet = max(max(StandsAt(i.world + sideways), StandsAt(i.world - sideways)), max(StandsAt(i.world + ahead), StandsAt(i.world - ahead)));
    wake = max(max(StandsAt(i.world - alongDir * 0.3) * 0.9, StandsAt(i.world - alongDir * 0.6) * 0.65), StandsAt(i.world - alongDir * 1.0) * 0.4);
    float near = saturate(1.5 - grain / 0.04);  // (gone where the looks would be finer than a pixel or two)
    meet *= near;
    wake *= near;
  }

  // what lies under it, bent a little by the ripples (not where that would bring in something standing in front)
  float2 bent = uv + N.xz * (0.03 * saturate(bedDepth * 6) / (1 + distance * 0.03));
  float depthBent = SCENE_DEPTH(int2(bent * viewport.xy), 0);
  if (depthBent > i.position.z || any(bent < 0) || any(bent > 1)) bent = uv;
  float3 under = behind.SampleLevel(samplerLinearClamp, bent, 0).rgb * 0.7;  // (a wet bed is darker)
  // the bright net of light on a shallow bed in sunlight
  float n1 = CloudNoise(float3(i.world.xz * 2.6 + time * 0.30, time * 0.25));
  float n2 = CloudNoise(float3(i.world.xz * 2.6 - time * 0.25 + 7, time * 0.20 + 3));
  float net = pow(saturate(1 - abs(n1 - n2) * 6), 4);
  under *= 1 + net * 0.8 * saturate(bedDepth * 10) * exp(-bedDepth * 4) * shadow * saturate(sunDirection.y * 3);

  // the light on it
  float3 light = skyLight + sunLight * (saturate(sunDirection.y) * 0.6 * shadow);
#if RAYS
  rayLightsAtMost = 2;  // (two rays for the small lights' shadows on the water, as the sea's)
#endif
  float3 lamps = SmallLights(i.world, N, -toPixel, float3(0.5, 0.5, 0.5), float3(0.04, 0.04, 0.04), 0.0016, 0.1, saturate(dot(N, -toPixel)) + 1e-4);

  // what the water takes from the bed and its own colour in its place (the sea's, water.cpp), by how much of it the eye
  // looks through: down to the bed and back, along a path bent toward the upright
  float sinIn = sqrt(saturate(1 - toPixel.y * toPixel.y));
  float cosOut = sqrt(saturate(1 - sinIn * sinIn / 1.77));
  // (a stream is not the sea: what it carries hides its bed sooner, and its own colour is soon the deep end of the
  // sea's; it takes less of the bed's red than so much sea would, being shallow; the light reaching the bed is dimmed on
  // the way down too. Rain and the wet after it stir it up: murkier, its colour going over to the silt's.)
  float through = bedDepth / max(cosOut, 0.2) * streamLook.y * (1 + 0.8 * weather.x);
  float3 kept = exp(-through * 0.5 * float3(1 / 3.0, 1 / 8.0, 1 / 10.0)) * exp(-bedDepth * streamLook.y * 0.25);
  float hidden = 1 - exp(-through / 4.0);
  float3 own = lerp(float3(0.027, 0.672, 0.604), float3(0.001, 0.019, 0.058), saturate(1 - exp(-through / 0.8)));
  own = lerp(own, float3(0.045, 0.04, 0.03), weather.x * 0.6);
  float3 color = lerp(under * kept, own * (light + lamps) * 0.6, hidden);
  float3 body = color;

  // foam: a lace carried downstream, white where it drops, where it meets what stands in it and behind that, and specks
  // everywhere
  float lace = StreamLace(q - float2(0, offsets.x), grain) * w + StreamLace(q - float2(0, offsets.y) + float2(3.1, 7.7), grain) * (1 - w);
  float rough = saturate((slope - 0.05) * 2.5);
  float much = saturate(0.05 + rough * 0.5 + wake * 0.3 + meet * 0.35);
  float foam = smoothstep(0.76 - much * 0.5, 0.88 - much * 0.42, lace) * (0.45 + 0.55 * much);
  color = lerp(color, (light + lamps) * 0.85, foam * 0.92);
  color += lamps * 0.35;
  color += light * (rings * 0.04);

  // the mirror: the sky, and what the picture shows that way
  float3 R = reflect(toPixel, N);
  R.y = max(R.y, 0.02);
  R = normalize(R);
  float3 mirror = skyZenith.w > 0.5 ? ComputedSky(R) : ToLinear(environmentSky.SampleLevel(samplerLinear, R, 0).rgb) * (lightMultiplier * modern.x * 0.5);
  {
    // (what the picture shows that way, found from a calmer surface than the ripples': one look a pixel off every small
    // ripple broke a plant's mirror image into specks)
    float3 calm = reflect(toPixel, normalize(lerp(N, normalize(up + alongDir * slope), 0.75)));
    calm.y = max(calm.y, 0.02);
    calm = normalize(calm);
    float3 p = i.world;
    float stride = 0.12 + distance * 0.004;
    for (int k = 0; k < 12; ++k) {
      p += calm * stride;
      stride *= 1.4;
      float4 c = mul(viewProj, float4(p, 1));
      if (c.w <= 0) break;
      float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
      if (any(at < 0) || any(at > 1)) break;
      float there = SCENE_DEPTH(int2(at * viewport.xy), 0);
      float here = c.z / c.w;
      // the picture has something nearer than this place on the way: what stands on the mirrored side of the water (a
      // tree on the far bank, its crown hanging over the water) is what is mirrored, even where the way passes far
      // behind it (the sky there broke the trees' image into bright specks); what stands on the eye's side of it (a
      // fern on the near bank) is looked past
      if (there > here && dot(WorldAt(at * viewport.xy, there).xz - i.world.xz, calm.xz) >= 0) {
        float gap = cameraBack.w / here - cameraBack.w / there;
        float2 edge = min(at, 1 - at);
        float seen = saturate(min(edge.x, edge.y) * 10) * (1 - k / 12.0) * (gap < stride * 2 + 0.5 ? 1.0 : 0.8);
        mirror = lerp(mirror, behind.SampleLevel(samplerLinearClamp, at, 0).rgb, seen);
        break;
      }
    }
  }
  float facing = saturate(dot(-toPixel, N));
  float fresnel = 0.02 + 0.98 * pow(1 - facing, 5);
  color = lerp(color, mirror, fresnel * (1 - foam));
  // [sunlight] (drifting cloud shadows) darker in a cloud's shadow, its mirror of the sky too, as the sea
  [branch] if (cloudDrift.x > 0.5) color *= 1 - cloudDrift.w * saturate((1 - CloudShadow(i.world)) / max(cloudDrift.y, 1e-3));
  // the sun's (or the moon's) glint on the ripples
  color += sunLight * (pow(saturate(dot(R, sunDirection)), 500) * 0.9 * shadow * (1 - foam));
  color = ThroughAirTo(color, i.world);
  [branch] if (streamLook.w > 0.5) {  // (a view for checking: its depth; or its speed, white water and what it meets)
    if (streamLook.w < 1.5) return float4(saturate(bedDepth * 2.5).xxx, 1);
    if (streamLook.w < 2.5) return float4(saturate(speed / 2.4), foam, max(meet, wake), 1);
    if (streamLook.w < 3.5) return float4(ThroughAirTo(body, i.world), 1);  // (3: the body alone)
    if (streamLook.w < 4.5) return float4(mirror, 1);                       // (4: the mirror alone)
    return float4(fresnel.xxx, 1);                                          // (5: how much it mirrors)
  }
  // at its edge the water thins out into the wet bank
  return float4(lerp(wetBank, color, smoothstep(0, min(0.12, halfW * 0.35), inside)), 1);
}

// The white water: foam carried fast downstream, its streaks from the ribbon's own picture (alpha), thinning toward its
// sides, lit as the stream is. Premultiplied.
float4 WhitePS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float3 toPixel = normalize(i.world - cameraPosition);
  float distance = length(i.world - cameraPosition);
  float depthBehind;
  coverage = Covered(i.position, i.world, distance, depthBehind);
  if (coverage == 0) discard;
  float across = i.shape.x, downstream = i.shape.y, halfW = max(i.shape.z, 0.05), slope = max(i.shape.w, 0);
  float grain = max(length(ddx(i.world)), length(ddy(i.world)));
  float speed = streamLook.z * min(0.3 + 2.2 * sqrt(slope), 3.2);
  float2 offsets;
  float w;
  FlowLayers(speed, offsets, w);
  float2 q = float2(across, downstream);
  float u = across / (2 * halfW) + 0.5;
  float streak = foamMap.Sample(samplerAniso, float2(u, (downstream - offsets.x) / 2.5)).a * w +
                 foamMap.Sample(samplerAniso, float2(u + 0.37, (downstream - offsets.y) / 2.5 + 0.61)).a * (1 - w);
  float lace = StreamLace(q * 1.6 - float2(0, offsets.x * 1.6), grain) * w + StreamLace(q * 1.6 - float2(0, offsets.y * 1.6) + float2(5.3, 2.9), grain) * (1 - w);
  float sides = saturate((halfW - abs(across)) / (halfW * 0.5));
  // (broken: bright streaks and the water between them, not a sheet)
  float a = smoothstep(0.3, 0.72, streak * 0.75 + lace * 0.55) * sides * sides * (0.5 + 0.5 * saturate(slope * 3));
  if (a <= 0.002) {
    coverage = 0;
    discard;
  }
  float3 up = float3(0, 1, 0);
  float shadow = SunShadow(i.world, up) * CloudShadow(i.world);
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 skyLight = skyZenith.w > 0.5 ? SkyAmbient(up) : sunLight * 0.3;
  float3 light = skyLight + sunLight * (saturate(sunDirection.y) * 0.6 * shadow);
#if RAYS
  rayLightsAtMost = 2;
#endif
  float3 lamps = SmallLights(i.world, up, -toPixel, float3(0.5, 0.5, 0.5), float3(0.04, 0.04, 0.04), 0.0016, 0.1, saturate(-toPixel.y) + 1e-4);
  float3 color = ThroughAirTo((light + lamps) * 0.85 * float3(0.9, 0.94, 0.95) + lamps * 0.35, i.world);
  if (streamLook.w > 0.5) a = 0;  // (the views for checking show the water alone)
  return float4(color * a, a);
}
)hlsl";

struct StreamConstants {
  float row0[4], row1[4], row2[4];
  float cameraRight[4], cameraUp[4], cameraBack[4];
  float mapInfo[4];
  float look[4];
};

Vec3 Level(Vec3 v) {
  const float l = std::sqrt(v.x * v.x + v.z * v.z);
  return l > 1e-9f ? Vec3{v.x / l, 0, v.z / l} : Vec3{0, 0, 0};
}

}  // namespace

void StreamWater::Forget() {
  placed_.clear();
  for (auto& [model, mesh] : meshes_) {  // (made again for the next map's: a model of the last one may be gone)
    if (mesh.vertices) device_->Destroy(mesh.vertices);
    if (mesh.indices) device_->Destroy(mesh.indices);
  }
  meshes_.clear();
}

int StreamWater::Count(bool whiteWater) const {
  int n = 0;
  for (const Placed& p : placed_) n += p.mesh->whiteWater == whiteWater ? 1 : 0;
  return n;
}

void StreamWater::Add(const Model* model, const Mat4& world, uint32_t key) {
  const Mesh* mesh = MeshOf(model);
  if (mesh) placed_.push_back({mesh, world, key});
}

// A stream's ribbon as its shaders take it, from its finest level's mesh file: the vertices of each cross-section (those
// that share a v) give the half width there and its middle; the middles' heights the way it falls (downstream: from the
// higher end) and how steeply, over a world unit either side (the banks follow the ground's bumps).
const StreamWater::Mesh* StreamWater::MeshOf(const Model* model) {
  auto known = meshes_.find(model);
  if (known != meshes_.end()) return known->second.ok ? &known->second : nullptr;
  Mesh& mesh = meshes_[model];
  if (!model || model->lods.empty() || !model->lods[0].stream) return nullptr;
  const ModelLod& lod = model->lods[0];
  auto fail = [&](const char* why) -> const Mesh* {
    fprintf(stderr, "render: stream %s: %s (not drawn as water)\n", lod.streamMesh.c_str(), why);
    return nullptr;
  };
  auto file = lod.streamMesh.empty() ? std::nullopt : textures_->ReadFile(lod.streamMesh);
  if (!file) return fail("file not found");
  const std::string& f = *file;
  if (f.size() < 48 || f.compare(0, 4, "MODL") != 0) return fail("not an .scm file");
  int32_t h[11];
  std::memcpy(h, f.data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  if (h[0] != 5 && h[0] != 6) return fail("unsupported .scm version");
  const int64_t indexBytes = h[0] == 6 ? 4 : 2;
  if (vertexCount <= 0 || indexCount <= 0 || vertexOffset < 0 || indexOffset < 0 ||
      vertexOffset + vertexCount * static_cast<int64_t>(sizeof(ScmVertex)) > static_cast<int64_t>(f.size()) ||
      indexOffset + indexCount * indexBytes > static_cast<int64_t>(f.size()))
    return fail("the .scm file is cut short");
  std::vector<ScmVertex> in(static_cast<size_t>(vertexCount));
  std::memcpy(in.data(), f.data() + vertexOffset, in.size() * sizeof(ScmVertex));
  std::vector<uint32_t> indices(static_cast<size_t>(indexCount));
  for (size_t k = 0; k < indices.size(); ++k) {
    if (indexBytes == 4) {
      std::memcpy(&indices[k], f.data() + indexOffset + static_cast<int64_t>(k) * 4, 4);
    } else {
      uint16_t i16;
      std::memcpy(&i16, f.data() + indexOffset + static_cast<int64_t>(k) * 2, 2);
      indices[k] = i16;
    }
    if (indices[k] >= in.size()) return fail("an index past its vertices");
  }

  // the cross-sections, in order along it
  std::map<int64_t, std::vector<uint32_t>> byV;
  for (uint32_t k = 0; k < in.size(); ++k) byV[std::llround(static_cast<double>(in[k].uv0[1]) * 4096.0)].push_back(k);
  struct Section {
    double v;
    Vec3 middle, across;
    float halfW;
  };
  std::vector<Section> sections;
  std::vector<uint32_t> sectionOf(in.size(), 0);
  for (const auto& [key, list] : byV) {
    Section s{static_cast<double>(key) / 4096.0, {0, 0, 0}, {0, 0, 0}, 0};
    uint32_t lo = list[0], hi = list[0];
    for (uint32_t k : list) {
      s.middle = s.middle + Vec3{in[k].position[0], in[k].position[1], in[k].position[2]} * (1.0f / static_cast<float>(list.size()));
      if (in[k].uv0[0] < in[lo].uv0[0]) lo = k;
      if (in[k].uv0[0] > in[hi].uv0[0]) hi = k;
      sectionOf[k] = static_cast<uint32_t>(sections.size());
    }
    const Vec3 gap{in[hi].position[0] - in[lo].position[0], 0, in[hi].position[2] - in[lo].position[2]};
    s.halfW = 0.5f * std::sqrt(gap.x * gap.x + gap.z * gap.z);
    s.across = Level(gap);
    sections.push_back(s);
  }
  if (sections.size() < 2) return fail("not a ribbon: fewer than two cross-sections (vertices sharing a v)");
  // world units of the model along it per unit of v: the sections' spacing (the middle of what they give: the white
  // water's runs have gaps between them)
  std::vector<double> perV;
  for (size_t k = 1; k < sections.size(); ++k) {
    const Vec3 d = sections[k].middle - sections[k - 1].middle;
    const double dv = sections[k].v - sections[k - 1].v;
    if (dv > 1e-9) perV.push_back(std::sqrt(d.x * d.x + d.z * d.z) / dv);
  }
  std::nth_element(perV.begin(), perV.begin() + static_cast<std::ptrdiff_t>(perV.size() / 2), perV.end());
  const double unitsPerV = std::max(perV[perV.size() / 2], 1e-6);
  // downstream: from the higher end
  const float sign = sections.front().middle.y >= sections.back().middle.y ? 1.0f : -1.0f;
  auto heightAt = [&](double v) {
    if (v <= sections.front().v) return static_cast<double>(sections.front().middle.y);
    if (v >= sections.back().v) return static_cast<double>(sections.back().middle.y);
    const auto next = std::lower_bound(sections.begin(), sections.end(), v, [](const Section& s, double at) { return s.v < at; });
    const auto before = next - 1;
    const double t = (v - before->v) / std::max(next->v - before->v, 1e-9);
    return before->middle.y + (next->middle.y - before->middle.y) * t;
  };
  std::vector<float> drop(sections.size());
  const double reach = 1.0 / unitsPerV;  // (a world unit, in v)
  for (size_t k = 0; k < sections.size(); ++k) {
    const double a = std::max(sections[k].v - reach, sections.front().v), b = std::min(sections[k].v + reach, sections.back().v);
    drop[k] = b > a ? static_cast<float>(sign * (heightAt(a) - heightAt(b)) / ((b - a) * unitsPerV)) : 0.0f;
    drop[k] = std::max(drop[k], 0.0f);
  }

  std::vector<Vertex> out(in.size());
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (size_t k = 0; k < in.size(); ++k) {
    const size_t s = sectionOf[k];
    const Section& at = sections[s];
    // downstream, from the middles round this one
    const Section& a = sections[s > 0 ? s - 1 : s];
    const Section& b = sections[s + 1 < sections.size() ? s + 1 : s];
    Vec3 along = Level((b.middle - a.middle) * sign);
    if (along.x == 0 && along.z == 0) along = Level(Vec3{in[k].binormal[0], 0, in[k].binormal[2]} * sign);
    Vertex& v = out[k];
    std::memcpy(v.position, in[k].position, sizeof v.position);
    v.across[0] = at.across.x;
    v.across[1] = 0;
    v.across[2] = at.across.z;
    v.along[0] = along.x;
    v.along[1] = 0;
    v.along[2] = along.z;
    v.shape[0] = in[k].uv0[0];
    v.shape[1] = static_cast<float>(sign * at.v * unitsPerV);
    v.shape[2] = at.halfW;
    v.shape[3] = drop[s];
    const Vec3 p{in[k].position[0], in[k].position[1], in[k].position[2]};
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  // (the wet bank past its edges: kWetBank in the shaders, and as much again up)
  mesh.lo = lo - Vec3{0.3f, 0.1f, 0.3f};
  mesh.hi = hi + Vec3{0.3f, 0.3f, 0.3f};
  gfx::BufferDesc vb;
  vb.kind = gfx::BufferKind::Vertex;
  vb.stride = sizeof(Vertex);
  vb.size = static_cast<uint32_t>(out.size() * sizeof(Vertex));
  vb.data = out.data();
  mesh.vertices = device_->CreateBuffer(vb);
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = 4;
  ib.size = static_cast<uint32_t>(indices.size() * 4);
  ib.data = indices.data();
  mesh.indices = device_->CreateBuffer(ib);
  if (!mesh.vertices || !mesh.indices) return fail("its buffers cannot be made");
  mesh.indexCount = static_cast<uint32_t>(indices.size());
  mesh.whiteWater = lod.stream == 2;
  mesh.texture = mesh.whiteWater ? lod.albedo : lod.normals;
  mesh.ok = true;
  return &mesh;
}

bool StreamWater::Prepare() {
  if (tried_) return water_ && white_;
  tried_ = true;
  const std::string source = "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + RayShaders() + kLightingShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.name = "stream water";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(Vertex, position)},
              {"TEXCOORD", 0, gfx::AttrType::Float3, offsetof(Vertex, across)},
              {"TEXCOORD", 1, gfx::AttrType::Float3, offsetof(Vertex, along)},
              {"TEXCOORD", 2, gfx::AttrType::Float4, offsetof(Vertex, shape)}};
  pd.stride = sizeof(Vertex);
  pd.cull = gfx::Cull::Back;  // (the ribbons hold both faces: the one facing up is drawn)
  pd.depthTest = false;
  pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Opaque;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  std::string why;
  water_ = device_->CreatePipeline(pd, &why);
  if (water_) {
    pd.name = "stream water, white water";
    pd.pixelEntry = "WhitePS";
    pd.blend = gfx::Blend::Premultiplied;
    white_ = device_->CreatePipeline(pd, &why);
  }
  if (!water_ || !white_) fprintf(stderr, "render: streams cannot be drawn as water (%s)\n", why.c_str());
  return water_ && white_;
}

void StreamWater::Draw(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth, gfx::Texture heights,
                       float heightUnit, float mapWidth, float mapHeight, const std::function<bool(uint32_t)>& standing) {
  drawn = triangles = 0;
  if (placed_.empty() || !behind || !sceneDepth || !heights || !Prepare()) return;
  StreamConstants c = {};
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float axes[12] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                          up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                          back.x, back.y, back.z, camera.zNear};
  std::memcpy(c.cameraRight, axes, sizeof axes);
  c.mapInfo[0] = mapWidth;
  c.mapInfo[1] = mapHeight;
  c.mapInfo[2] = heightUnit;
  c.look[0] = depth;
  c.look[1] = murk;
  c.look[2] = flow;
  c.look[3] = static_cast<float>(this->view);
  device_->SetTexture(kStreamBehindSlot, behind);
  device_->SetTexture(kStreamDepthSlot, sceneDepth);
  device_->SetTexture(kStreamHeightsSlot, heights);
  for (int pass = 0; pass < 2; ++pass) {  // the water, then its white water over it
    const bool white = pass == 1;
    bool set = false;
    for (const Placed& p : placed_) {
      if (p.mesh->whiteWater != white || (standing && !standing(p.key))) continue;
      // (its box in the world, for whether the view sees it)
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (int corner = 0; corner < 8; ++corner) {
        const Vec3 m{(corner & 1) ? p.mesh->hi.x : p.mesh->lo.x, (corner & 2) ? p.mesh->hi.y : p.mesh->lo.y, (corner & 4) ? p.mesh->hi.z : p.mesh->lo.z};
        const Vec4 w4 = p.world * Vec4{m.x, m.y, m.z, 1};
        const Vec3 w{w4.x, w4.y, w4.z};
        lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
        hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
      }
      if (!view.BoxVisible(lo, hi)) continue;
      if (!set) {
        device_->SetPipeline(white ? white_ : water_);
        device_->SetConstants(0, &view.constants, sizeof view.constants);
        set = true;
      }
      for (int r = 0; r < 3; ++r) {
        float* row = r == 0 ? c.row0 : r == 1 ? c.row1 : c.row2;
        for (int k = 0; k < 4; ++k) row[k] = p.world.m[r][k];
      }
      c.mapInfo[3] = white ? 1.0f : 0.0f;
      device_->SetConstants(1, &c, sizeof c);
      device_->SetTexture(white ? kStreamFoamSlot : kStreamRipplesSlot, p.mesh->texture);
      device_->SetVertexBuffer(p.mesh->vertices);
      device_->SetIndexBuffer(p.mesh->indices);
      device_->DrawIndexed(p.mesh->indexCount);
      ++drawn;
      triangles += static_cast<int>(p.mesh->indexCount / 6);  // (both faces are listed; the one facing up is drawn)
    }
  }
  for (int slot : {kStreamBehindSlot, kStreamDepthSlot, kStreamHeightsSlot, kStreamRipplesSlot, kStreamFoamSlot}) device_->SetTexture(slot, {});
}

}  // namespace render
