// The lighting every scene shader shares, as HLSL source placed after kViewShaderBlock:
//  - the sun's shadow (cascaded shadow maps),
//  - the original game's light formula,
//  - the modern one: physically based (metalness / roughness), sun plus sky light from the map's
//    sky cube or from the computed sky (sky.h), plus the small lights near the surface (lights.h), in linear colour with values
//    beyond 1 kept for the last pass to map to the screen.
// A shader computes its surface (colour, normal, material) and asks one of the two for the result,
// by the `look` value of the View block.
#pragma once

namespace render {

inline const char kLightingShaderBlock[] = R"hlsl(
// How much of the lying snow a kind of surface takes (a shader may say before this block: units
// take less than the ground, so that they can still be told apart).
#ifndef SURFACE_SNOW
#define SURFACE_SNOW 1.0
#endif
// Whether water gathers on this kind of surface when it is wet (the ground says 1; a unit's roof does not).
#ifndef SURFACE_PUDDLES
#define SURFACE_PUDDLES 0.0
#endif
// The share of that this very surface holds (a unit's own snow, unit_wear.h: a shader sets it before ModernLight; 1:
// as the weather lays it), and the place its patches are laid by (a unit's own snow: its place on the model at rest,
// so that they go with it as it drives and turns; else the world's).
static float surfaceSnowShare = 1;
static bool surfaceSnowOwn = false;
static float3 surfaceSnowPlace = float3(0, 0, 0);
// [unit dirt] How wet this very surface is by the weather: the weather's wetness; on units (a shader that says
// SURFACE_OWN_WET), times the share of it the surface takes: 1, or 0 for a unit with its own (unit_wear.h,
// World::unitDirt: its own laid on before ModernLight). Elsewhere the lines below are as they were, word for word.
#ifdef SURFACE_OWN_WET
static float surfaceWetShare = 1;
#define SURFACE_WET (weather.x * surfaceWetShare)
#else
#define SURFACE_WET weather.x
#endif
#ifdef SURFACE_TRACKS
Texture2D tracksMap : register(t26);  // (the ground only) how much of the lying snow is trodden away at a place
#endif
// [snow ground] The lying snow's colour, its roughness, how much of its small shapes it smooths, and the colour of packed
// snow. The ground's snow pipeline (terrain.cpp, GROUND_SNOW) gives its own, from the map's snow materials and what lies
// round the place, before this block; everywhere else these are the constants they always were.
#ifndef SNOW_COLOUR
#define SNOW_COLOUR float3(0.50, 0.53, 0.58)
#define SNOW_ROUGH 0.7
#define SNOW_UP float3(0, 1, 0)
#define SNOW_FLAT(lying, fallen) 0.5 * lying * saturate(fallen * 2 - 0.8)
#define SNOW_PACKED float3(0.36, 0.40, 0.48)
#endif
Texture2DArray<float> lampShadowMaps : register(t27);
Texture2DArray<float> shadowMaps : register(t28);
TextureCube environmentSky : register(t29);
#ifndef SMALL_LIGHTS_DECLARED  // (ray_scene.h's block declares it too, when it comes first)
#define SMALL_LIGHTS_DECLARED 1
StructuredBuffer<float4> lightData : register(t1, space1);  // the small lights and their grid (lights.h)
#endif

// Soft shadows (the view's softShadows): the sun is not a point, so a shadow's edge is the sharper the
// nearer what casts it. First what casts it is looked for round the place (taps nearer the sun than
// it), and how far it stands from the place; the edge is then blurred by that much (taps compared, as
// the sharp shadow's are, spread over a disc), turned from place to place so that the taps make no
// pattern. `s`: the place in cascade `i` (x, y in the map, z its depth); `texel` that cascade's texel in
// world units, `step` in the map's.
static const float2 kShadowDisc[16] = {
  float2(-0.942, -0.399), float2(0.946, -0.769), float2(-0.094, -0.929), float2(0.345, 0.294),
  float2(-0.916, 0.458), float2(-0.815, -0.879), float2(-0.383, 0.276), float2(0.975, 0.756),
  float2(0.443, -0.975), float2(0.537, -0.474), float2(-0.265, -0.419), float2(0.792, 0.191),
  float2(-0.242, 0.997), float2(-0.814, 0.914), float2(0.200, 0.786), float2(0.144, -0.141)};
float SoftSunShadow(float3 s, int i, float texel, float step) {
  float perDepth = 1 / length(shadowMatrix[i][2].xyz);  // world units one of the map's depth spans
  float turn = frac(sin(dot(s.xy * 4096, float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
  float2x2 r = float2x2(cos(turn), -sin(turn), sin(turn), cos(turn));
  float search = max(softShadows.z / texel, 1.5) * step;
  float sum = 0, n = 0;
  [unroll] for (int k = 0; k < 16; ++k) {
    float d = shadowMaps.SampleLevel(samplerPointClamp, float3(s.xy + mul(r, kShadowDisc[k]) * search, i), 0);
    if (d > s.z) {  // (reversed depth: nearer the sun is greater)
      sum += d;
      n += 1;
    }
  }
  if (n == 0) return 1;
  float gap = (sum / n - s.z) * perDepth;
  float spread = clamp(gap * softShadows.y, texel * 1.2, max(softShadows.w, texel * 1.2)) / texel * step;
  float lit = 0;
  [unroll] for (int k = 0; k < 16; ++k) lit += shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy + mul(r, kShadowDisc[k]) * spread, i), s.z);
  return lit / 16;
}

// 1 in sunlight, 0 in shadow, by the shadow maps alone. `normal` is the surface's (the lookup is moved a little
// along it, which keeps a lit surface from shadowing itself).
float SunShadowMaps(float3 world, float3 normal) {
  if (shadowsOn < 0.5) return 1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float texel = cascadeTexel[i];
    float3 p = world + normal * (texel * 1.5) + sunDirection * (texel * 1.0);
    float3 s = mul(shadowMatrix[i], float4(p, 1)).xyz;
    if (all(s.xy > 0.02) && all(s.xy < 0.98) && s.z > 0 && s.z < 1) {
      float lit = 0;
      uint mapWidth, mapHeight, mapLayers;  // (the maps' size is a setting)
      shadowMaps.GetDimensions(mapWidth, mapHeight, mapLayers);
      float step = 1.0 / float(mapWidth);
      [branch] if (softShadows.x > 0.5) return SoftSunShadow(s, i, texel, step);
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
          lit += shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy + float2(x, y) * step, i), s.z);
      return lit / 9.0;
    }
  }
  return 1;
}

// Smoke's shadow (smoke.h; World::litSmoke, smokeShadows): what of the sun's light comes through the smoke on
// its way to `world`. The map holds, for each line toward the sun, the smoke's optical depth and its first two
// moments in height; the smoke on that line is taken as spread normally about its mean height, and what of it is
// higher (nearer the sun) than the place shadows it: the ground under a column takes all of it, a puff on the
// column's sunny side little, an aircraft above it none. Fades out toward the map's edge. `scale`: what the smoke's
// optical depth is multiplied by besides (lit smoke takes less of its own shadow: light scattered on and on inside it).
Texture2D smokeShadowMap : register(t40);
float SmokeShadowAt(float3 world, float scale) {
  float3 s = mul(smokeMatrix, float4(world, 1)).xyz;
  float edge = min(min(s.x, 1 - s.x), min(s.y, 1 - s.y));
  if (edge <= 0) return 1;
  float3 m = smokeShadowMap.SampleLevel(samplerLinearClamp, s.xy, 0).rgb;
  if (m.x < 1e-3) return 1;
  float mean = m.y / m.x;
  float spread = sqrt(max(m.z / m.x - mean * mean, 0) + smokeShadow.y * smokeShadow.y);
  float ahead = 1 / (1 + exp(-1.7 * (mean - s.z) / spread));  // (the normal distribution's share above, nearly)
  return lerp(1, exp(-m.x * ahead * smokeShadow.z * scale), saturate(edge * 12));
}
float SmokeShadow(float3 world) {
  [branch] if (smokeShadow.x < 0.5) return 1;
  return SmokeShadowAt(world, 1);
}
// The sun's shadow on a place: the shadow maps', and the smoke's where it casts one.
float SunShadow(float3 world, float3 normal) { return SunShadowMaps(world, normal) * SmokeShadow(world); }

// [shadowmask] The sun's shadow worked out once a visible pixel of the ground (sun_mask.h, World::shadowMask): the shadow maps'
// share of it, as SunShadowMaps works it out for the ground the camera sees there, or below 0 where the mask leaves it to
// the surface (no ground there, or the depth not smooth round the pixel: a crest, a crease, the ground's edge, where the
// scene's samples of a pixel may lie on other ground than its middle). Kept as two halves to a word, a row of the picture
// after another (the ground's texture registers are all taken): a shader that reads it says SUN_MASK_BUFFER before this block
// and has the shader buffer at slot 0 for it. `words`: a row's words, `rows` the rows (out of them: -1).
#ifdef SUN_MASK_BUFFER
ByteAddressBuffer sunMaskRows : register(t0, space1);
float SunMaskAt(uint2 p, uint words, uint rows) {
  [branch] if ((p.x >> 1) >= words || p.y >= rows) return -1;
  uint w = sunMaskRows.Load((p.y * words + (p.x >> 1)) * 4);
  return f16tof32((p.x & 1) ? (w >> 16) : (w & 0xffff));
}
#endif
// The ground's shader: the mask where it serves (the view's shadowMask.x on), else the ground's own SunShadow, as without it.
// `position`: the pixel shader's SV_Position.
float SunShadowSeen(float3 world, float3 normal, float4 position) {
#ifdef SUN_MASK_BUFFER
  [branch] if (shadowMask.x > 0.5) {
    float m = SunMaskAt(uint2(position.xy), uint(shadowMask.z), uint(shadowMask.w));
    [branch] if (m >= 0) return m * SmokeShadow(world);
    [branch] if (shadowMask.x > 1.5) return 0.5;  // (checks, SunMask::check: where it is left to the ground, half)
  }
#endif
  return SunShadow(world, normal);
}

// Which of the sun's shadow maps a place is looked up in (0 the nearest and sharpest; -1: none).
int SunCascade(float3 world) {
  if (shadowsOn < 0.5) return -1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float3 s = mul(shadowMatrix[i], float4(world, 1)).xyz;
    if (all(s.xy > 0.02) && all(s.xy < 0.98) && s.z > 0 && s.z < 1) return i;
  }
  return -1;
}

// The original's light on a surface: sun by N.L and shadow, plus ambience, scaled by the map's
// multiplier, with the "shadow fill" colour filling what the light leaves.
float3 OriginalLight(float sunDotN, float shadow) {
  float3 light = sunColor * saturate(sunDotN) * shadow + sunAmbience;
  return lightMultiplier * light + (1 - light) * shadowFill;
}

float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }

// How much sky a rough surface reflects, without a lookup table (after Karis, "Physically Based
// Shading on Mobile").
float3 EnvironmentBrdf(float3 f0, float roughness, float nDotV) {
  const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
  const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
  float4 r = roughness * c0 + c1;
  float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
  float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + ab.y;
}

// How much of a lamp's beam reaches a place: 1 where nothing stands between them, by the lamp's
// own shadow map (lights.h). The lookup is moved a little along the surface's normal and toward
// the lamp, which keeps a lit surface from shadowing itself.
float LampShadow(float3 world, float3 N, uint map) {
  uint at = uint(lightGridSize.w) + map * 4;
  float4 p = float4(world + N * 0.1, 1);
  float4 s = float4(dot(lightData[at], p), dot(lightData[at + 1], p), dot(lightData[at + 2], p), dot(lightData[at + 3], p));
  if (s.w <= 0.02) return 1;
  float2 uv = s.xy / s.w;
  if (any(uv < 0.01) || any(uv > 0.99)) return 1;
  float depth = s.z / max(s.w - (0.12 + 0.015 * s.w), 0.01);
  float step = 1.0 / 512.0;
  float lit = 0;
  for (int y = 0; y <= 1; ++y)
    for (int x = 0; x <= 1; ++x)
      lit += lampShadowMaps.SampleCmpLevelZero(samplerShadow, float3(uv + (float2(x, y) - 0.5) * (1.5 * step), map), depth);
  return lit * 0.25;
}

#if RAYS
// --- The small lights' shadows by rays (World::rayLightShadows; view.h's rayLights; ray_scene.h's scene) ---
// A place's small lights are those of its square, as SmallLights has them; what stands between it and a light is
// found by a ray toward that light. So that many lights cost about as much as a few, a place casts at most K rays
// (rayLights.y, 1..4; grass one, the water two, leaves none: rayLightsAtMost, which their shaders set, as many layers
// of them lie over each other or the shadows show little there), the lights for them chosen as resampled importance
// sampling chooses (after ReSTIR DI, Bitterli et al. 2020, "Spatiotemporal reservoir resampling for real-time ray
// tracing with dynamic direct lighting"; here every light of the square is a candidate, and its share,
// the brightness of its unshadowed light at the place, is known exactly): the strongest lights that each carry at
// least 1/K of what is left get a ray of their own; the rest are chosen by K - c evenly spaced picks over the running
// sum of their shares, from one offset for the place (stratified, so each is chosen at most once, with the chance
// picks x its share / their sum), and what the ray of a chosen one finds is weighted by 1 / that chance. The light is
// then the unshadowed light less what the rays find shadowed: where no ray finds a shadow it is the light without
// rays exactly, and a place with K lights or fewer is shadowed exactly. The offset is the place's own, fixed to the
// world and the same every frame, and leaves let light through by their mean (RayLightSeen), so a still picture stands
// still and in a moving one the grain moves with the world (nothing blended over frames, no upscaler needed).

// One light of a square at a place, as SmallLights' loop works it out: its place and radius, colour, fourth row
// (lights.h: its shadow map's number, or -1; -2 and where the model it rides on stands), the way toward it, how
// the place faces it, and its fade in two parts: what a shadow takes (all of a light to all sides; a beam within
// its cone) and what none does (a beam's glow round its lamp). Both 0 where it does not reach.
struct SmallLight {
  float4 where;
  float4 tint;
  float4 more;
  float3 l;
  float nl;
  float lit;
  float glow;
  bool beam;
};
SmallLight SmallLightAt(uint light, float3 world, float3 N) {
  SmallLight L;
  L.where = lightData[uint(lightGridSize.z) + light * 4];
  float3 toLight = L.where.xyz - world;
  float far2 = dot(toLight, toLight) / (L.where.w * L.where.w);
  L.tint = 0;
  L.more = float4(-1, 0, 0, 0);
  L.beam = false;
  L.l = 0;
  L.nl = 0;
  L.lit = 0;
  L.glow = 0;
  [branch] if (far2 >= 1) return L;  // (the rest only for a light that reaches)
  L.tint = lightData[uint(lightGridSize.z) + light * 4 + 1];
  float4 beam = lightData[uint(lightGridSize.z) + light * 4 + 2];
  L.more = lightData[uint(lightGridSize.z) + light * 4 + 3];
  L.beam = beam.w > -1.5;
  L.l = normalize(toLight);
  L.nl = saturate(dot(N, L.l));
  float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
  L.lit = fade;
  if (L.beam) {
    float off = sqrt(saturate((1 - dot(-L.l, beam.xyz)) / max(1 - beam.w, 1e-4)));
    float inside = 1 - smoothstep(0.7, 1.0, off);
    L.lit = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * inside;
    L.glow = fade * L.tint.w * saturate(1 - far2 * 25);
    L.nl = saturate(dot(N, L.l) + 0.6 * inside);
  }
  return L;
}
// What a light's ray is chosen by: the brightness of what of its light a shadow can take there (none for a lamp
// with its own shadow map, which keeps it). ([nightlight] A light to all sides by its steady part, its second row's w,
// lights.h: a build arc's by its mean, not its quick flicker, so that the rays' picks round a building site do not change
// every frame with the arcs and the shadowed light there no longer flickers; the picks' weights stay the same shares.)
float SmallLightShare(SmallLight L) {
  return L.more.x > -0.5 ? 0 : L.lit * L.nl * dot(L.tint.rgb, float3(0.3, 0.6, 0.1)) * (L.beam ? 1 : L.tint.w);
}

// How wide, in pixels at least, the squares of the world are that a place's picks take their offset from
// (SmallLightsTraced): one place's offset holds for the square round it.
static const float kRayLightCell = 1.5;
// The most rays a place casts, as the surface's shader may lower it (below rayLights.y; set before ModernLight or
// SmallLights is called; 0: none, the small lights as without rays).
static uint rayLightsAtMost = 4;
// How far over the straight way to a light the ground must rise to shadow it (world units): the rays' ground is
// every fourth height of the drawn one, which a lamp low over the ground would otherwise shadow with itself.
static const float kRayGroundRise = 0.35;
// How much of a small light reaches the place p (1 all, 0 none), by rays toward it. First what stands on the ground
// and is solid (units, scenery: mask 4), by the front of its triangles only (a ray that starts inside the coarser
// level the rays see of the place's own model is not stopped by it), as opaque (the card's own search, no candidates
// for the shader), up to the light's own ball (rayLights.z of its radius, a beam's rayLights.w: the wreck a fire burns
// in does not shadow its flash). A light that rides on a model (a unit's lamp: `more`, its fourth row, says where the
// model stands) is not shadowed by that model: its ray takes the nearest hit, and where that is the model, goes on
// past it (three tries). Then, along the way raised by kRayGroundRise at both ends, the ground (mask 8, the front of
// its triangles) and leaves (mask 16): they let light through as the other rays' chance does, but as that chance's
// mean (each leaf passed keeps 1 - its coverage), so a canopy's shadow has no grain. The water never shadows.
float RayLightSeen(float3 p, float3 N, SmallLight L) {
  float3 o = p + N * 0.04;
  float3 to = L.where.xyz - o;
  float far = length(to);
  float ball = L.where.w * (L.beam ? rayLights.w : rayLights.z);
  float end = far - ball;
  if (end <= 0.03) return 1;
  RayDesc ray;
  ray.Origin = o;
  ray.Direction = to / far;
  ray.TMin = 0.02;
  ray.TMax = end;
  [branch] if (L.more.x < -1.5) {  // (a lamp on a model: the nearest hit, and on past the model it rides on)
    for (uint tries = 0; tries < 3; ++tries) {
      RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
      q.TraceRayInline(scene, RAY_FLAG_NONE, 0x04, ray);
      q.Proceed();
      if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) break;
      // (the instance's place: its matrix's translation, the 4 x 3 form's last row; gfx_check.cpp checks it)
      if (any(abs(q.CommittedObjectToWorld4x3()[3] - L.more.yzw) > 0.01)) return 0;
      ray.TMin = q.CommittedRayT() + 0.01;
      if (ray.TMin >= ray.TMax) break;
    }
  } else {
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x04, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return 0;
  }
  float3 a = o + float3(0, kRayGroundRise, 0), b = L.where.xyz + float3(0, kRayGroundRise, 0);
  float along = length(b - a);
  RayDesc raised;
  raised.Origin = a;
  raised.Direction = (b - a) / max(along, 1e-4);
  raised.TMin = 0;
  raised.TMax = max(along - ball, 0.0);
  // (the ground's triangles come to the loop too, to take their fronts alone; a ray meets few of them)
  RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> g;
  g.TraceRayInline(scene, RAY_FLAG_NONE, 0x18, raised);
  float through = 1;
  while (g.Proceed()) {
    float cover = RayInstance(g.CandidateInstanceID()).a;
    bool take;
    if (cover < 1.5) {  // leaves
      through *= 1 - saturate(cover);
      take = through < 0.02;  // (as good as shut)
    } else {
      take = g.CandidateTriangleFrontFace();
    }
    if (take) g.CommitNonOpaqueTriangleHit();
  }
  return g.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0 : through;
}

// SmallLights with the lights' shadows by rays (above): the `n` lights of the square's list from `first`.
float3 SmallLightsTraced(float3 world, float3 N, float3 V, float3 diffuseColor, float3 f0, float a2, float k, float nDotV, uint first, uint n) {
  // the picks' offset: the same every frame for the same place, and fixed to the world, not to the screen, so that when
  // the camera moves what grain there is moves with the ground and the models and does not crawl over them (an offset
  // by the pixel did: docs/render-lighting.md). The place's square on the plane its surface faces most, the squares a
  // power of two of world units wide by the place's distance (at least kRayLightCell pixels on the screen, also where
  // the surface is seen aslant), its number made into Jimenez's interleaved gradient noise.
  float4 clip = mul(viewProj, float4(world, 1));
  float3 an = abs(N);
  const bool up = an.y >= an.x && an.y >= an.z, side = !up && an.x >= an.z;
  float2 onPlane = up ? world.xz : side ? world.zy : world.xy;
  float aslant = max(abs(up ? V.y : side ? V.x : V.z), 0.3);
  float pixelWide = 2 * max(clip.w, 1e-3) * viewport.w / length(viewProj[1].xyz);  // (a pixel's width in world units there)
  float2 sq = floor(onPlane / exp2(ceil(log2(kRayLightCell * pixelWide / aslant))));
  sq -= 1024 * floor(sq / 1024);  // (kept small, for the noise's precision)
  float offset = frac(52.9829189 * frac(dot(sq, float2(0.06711056, 0.00583715))));
  const uint most = uint(rayLights.y);
  const uint K = clamp(most <= 4 ? min(most, rayLightsAtMost) : most, 1u, 4u);
  const bool everyOne = n <= K;  // (as many lights as rays, or fewer: each its own, nothing to choose)
  // first the shares: their sum, and the four largest (in order) with their places in the list
  float total = 0;
  float4 topW = 0;
  uint4 topS = 0;
  [branch] if (most <= 4 && !everyOne) {
    for (uint s = 0; s < n; ++s) {
      uint at = first + s;
      float w = SmallLightShare(SmallLightAt(uint(lightData[at >> 2][at & 3]), world, N));
      total += w;
      [branch] if (w > topW.w) {
        if (w > topW.x) {
          topW = float4(w, topW.xyz);
          topS = uint4(s, topS.xyz);
        } else if (w > topW.y) {
          topW.yzw = float3(w, topW.yz);
          topS.yzw = uint3(s, topS.yz);
        } else if (w > topW.z) {
          topW.zw = float2(w, topW.z);
          topS.zw = uint2(s, topS.z);
        } else {
          topW.w = w;
          topS.w = s;
        }
      }
    }
  }
  // those that get a ray of their own, and the picks over the rest
  uint certain = 0;
  float rest = total;
  [unroll] for (uint j = 0; j < 4; ++j) {
    if (j < K && certain == j && topW[j] > 0 && topW[j] * float(K - j) >= rest) {
      rest -= topW[j];
      certain = j + 1;
    }
  }
  const float picks = float(K - certain);
  const float scale = rest > 0 ? picks / rest : 0;  // (a share's chance of a pick, per unit of share)
  // then the light, as SmallLights has it; what a shadow can take of it summed apart, and the lights chosen kept
  float3 small = 0, shadowable = 0;
  float3 c0 = 0, c1 = 0, c2 = 0, c3 = 0;  // (what each chosen light's ray stands for)
  uint l0 = 0, l1 = 0, l2 = 0, l3 = 0;    // (and which light it is)
  uint taken = 0;
  float sum = 0;
  for (uint s = 0; s < n; ++s) {
    uint at = first + s;
    uint light = uint(lightData[at >> 2][at & 3]);
    SmallLight L = SmallLightAt(light, world, N);
    if (L.lit + L.glow <= 0) continue;
    float3 h = normalize(V + L.l);
    float nh = saturate(dot(N, h));
    float dl = nh * nh * (a2 - 1) + 1;
    float3 fl = f0 + (1 - f0) * pow(1 - saturate(dot(V, h)), 5);
    float visl = 0.25 / ((L.nl * (1 - k) + k) * (nDotV * (1 - k) + k));
    float3 c = (diffuseColor / 3.14159265 + a2 / (3.14159265 * dl * dl) * visl * fl * modern.w) * L.tint.rgb * (L.nl * 3.14159265);
    [branch] if (L.more.x > -0.5) {  // a lamp with its own shadow map keeps it
      small += c * ((L.lit > 0 ? L.lit * LampShadow(world, N, uint(L.more.x)) : 0) + L.glow);
      continue;
    }
    small += c * L.glow;
    float3 shade = c * L.lit;
    [branch] if (most > 4) {  // (every light its own ray: exact, for comparing)
      if (L.lit * L.nl > 0) shade *= RayLightSeen(world, N, L);
      small += shade;
      continue;
    }
    shadowable += shade;
    float w = SmallLightShare(L);
    if (w <= 0) continue;
    bool chosen = everyOne || (certain > 0 && s == topS.x) || (certain > 1 && s == topS.y) || (certain > 2 && s == topS.z) || (certain > 3 && s == topS.w);
    float3 stands = shade;
    if (!chosen && picks > 0) {
      float before = sum * scale - offset;
      sum += w;
      if (ceil(sum * scale - offset) > ceil(before)) {  // (a pick falls within this light's part of the sum)
        chosen = true;
        stands = shade / (w * scale);
      }
    }
    if (chosen && taken < 4) {
      if (taken == 0) {
        c0 = stands;
        l0 = light;
      } else if (taken == 1) {
        c1 = stands;
        l1 = light;
      } else if (taken == 2) {
        c2 = stands;
        l2 = light;
      } else {
        c3 = stands;
        l3 = light;
      }
      ++taken;
    }
  }
  // the rays: what they find shadowed is taken off
  float3 shadowed = 0;
  for (uint t = 0; t < taken; ++t) {
    uint light = t == 0 ? l0 : t == 1 ? l1 : t == 2 ? l2 : l3;
    float3 stands = t == 0 ? c0 : t == 1 ? c1 : t == 2 ? c2 : c3;
    shadowed += stands * (1 - RayLightSeen(world, N, SmallLightAt(light, world, N)));
  }
  return small + max(shadowable - shadowed, 0);
}
#endif

// The far distance the world beyond the map's edge is lost in, toward `d` (unit length): what the
// sky's pass shows below the horizon that way. (beyondland: the land far off is lost in it too,
// beyond_land.cpp)
float3 BeyondBack(float3 d) {
  float3 back;
  if (skyZenith.w > 0.5) {  // (as sky.cpp shows the far distance under a computed sky)
    back = BeyondGrey(HorizonHaze(d)) * pow(max(skyGlow.w, 0.006), 0.43);
  } else {
    float3 picture = environmentSky.SampleLevel(samplerLinear, d, 0).rgb;
    back = BeyondGrey(look > 0.5 ? pow(max(picture, 0), 2.2) * (lightMultiplier * modern.x) : picture);
  }
  [branch] if (air.x > 0.5) {  // (light in the air: through all of the volume, as the sky's pass shows the far distance)
    float4 a = AirAt(cameraPosition + d * (air.y * exp(air.z) * 0.999));
    back = back * a.a + a.rgb;
  }
  [branch] if (beyondSea.x > 0.5) back = BeyondSea(d, back);  // (sky) from above, the far sea (view.h)
  return back;
}
// [weather] What the air between the eye and `world` adds to what is seen there (rgb) and what of that comes through (a),
// as ThroughAirTo lays them (view.h).
float4 AirOver(float3 world) {
  [branch] if (air.x > 0.5) return AirAt(world);
  float hazed = weather.z > 0 ? min(1 - exp(-length(world - cameraPosition) * weather.z), weather.w) : 0;
  float lost = MistLost(world);
  return float4(AirColor() * (hazed * (1 - lost)) + MistColor() * lost, (1 - hazed) * (1 - lost));
}
// The world beyond the map's edge: ground or water there, a shade dimmer just outside (the
// playable ground ends here) and lost in the distance farther out: it goes over into what the
// sky's pass shows in that direction, so the land has no rim. On the map: the colour as it is.
// [weather] The dimming and the line along the edge are the ground's, not the air's before it: what the air adds (fog, mist,
// the haze) is taken off first and laid again after, as on the map (they darkened the fog past the edge: seams down the
// strategic view at the map's sides, Chris's snapshot test, 2026-10-25).
float3 BeyondFar(float3 color, float3 world, float outside, float pixel) {
  float t = saturate(outside * beyond.z);
  float3 airAdds = 0;
  [branch] if (air.x > 0.5 || weather.z > 0 || mist.x > 0) {
    airAdds = AirOver(world).rgb;
    color = max(color - airAdds, 0);
  }
  // without the haze (beyond.w < 0): the land stays clear and is lost only in the last tenth of
  // the way, where it meets the horizon
  if (beyond.w < 0) t = saturate((t - 0.9) * 10);
  [branch] if (beyondLand.x > 0.5) {
    // (beyondland) the soft edge: past the edge the ground dims and loses some of its colour little by little,
    // over beyondLand.y world units, and a soft shadow lies along the edge itself, fading out over beyondLand.x
    // pixels at any zoom (`pixel`: how much of the way out one pixel is), so where the playable ground ends stays
    // plain from all the way up without a hard line
    float grow = smoothstep(0, beyondLand.y, outside);
    color = lerp(color, dot(color, float3(0.3, 0.6, 0.1)), beyondLand.w * grow) * lerp(1, abs(beyond.w), grow);
    // (sky) the shadow only from far up, while the dimming's band is too narrow in the picture to show the edge by itself;
    // close in, over a band many pixels wide, it read as a seam cut through the land and the sea
    float shadow = beyondLand.z * (1 - smoothstep(6, 24, beyondLand.y / max(pixel, 1e-5)));
    if (pixel > 0) color *= 1 - shadow * exp(-outside / max(pixel * beyondLand.x, 1e-4));
  } else {
    // just outside: dimmer and with a third of its colour gone, behind a thin dark line along the
    // edge that keeps its width in the picture at any zoom: where the playable ground ends must be
    // plain from all the way up
    float near = saturate(outside / 6);
    color = lerp(color, dot(color, float3(0.3, 0.6, 0.1)), 0.35 * near) * lerp(1, abs(beyond.w), near);
    if (pixel > 0) color *= 1 - 0.7 * saturate(1 - outside / max(pixel * 1.6, 1e-4));
  }
  color += airAdds;  // ([weather] the air before it again)
  [branch] if (beyondSea.x > 0.5 && t <= 0) return color;  // ((sky) the far sea is dearer: what it is lost in only where it is)
  return lerp(color, BeyondBack(normalize(world - cameraPosition)), t * t * (3 - 2 * t));
}
float3 BeyondMap(float3 color, float3 world) {
  float outside = OutsideMap(world);
  float pixel = fwidth(outside);  // (how much of the way out one pixel is, for the line along the edge)
  if (outside <= 0) return color;
  return BeyondFar(color, world, outside, max(pixel, 1e-5));
}
// The same for what stands on that ground (trees, rocks): dimmed and lost in the distance with
// it, without the line along the edge.
float3 BeyondScenery(float3 color, float3 world) {
  float outside = OutsideMap(world);
  if (outside <= 0) return color;
  return BeyondFar(color, world, outside, 0);
}

// The small lights (lights.h) on a surface: those of the square of ground the place is over.
// `diffuseColor` and `f0`: what the surface gives back of light from all sides and mirrored;
// `a2` and `k`: its roughness as the highlight's formulas take it; `nDotV`: how it faces the eye.
float3 SmallLights(float3 world, float3 N, float3 V, float3 diffuseColor, float3 f0, float a2, float k, float nDotV) {
  float3 small = 0;
  // the small lights of the square of ground this place is over
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (world.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), n = min(uint(square.y), 48u);
#if RAYS
      [branch] if (rayLights.x > 0.5 && rayLightsAtMost > 0) return SmallLightsTraced(world, N, V, diffuseColor, f0, a2, k, nDotV, first, n);  // (shadows by rays)
#endif
      for (uint s = 0; s < n; ++s) {
        uint at = first + s;
        uint light = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + light * 4];
        float3 toLight = where.xyz - world;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float3 l = normalize(toLight);
        float nl = saturate(dot(N, l));
        // full at the light, nothing at its radius
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 tint = lightData[uint(lightGridSize.z) + light * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + light * 4 + 2];
        if (beam.w > -1.5) {
          // a beam: how far off its middle this place is (0 in the middle, 1 at its edge); it reaches
          // farther than a light to all sides, and a share of it stays around the lamp
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          // (what stands in the beam shadows it; the lamp's glow to all sides is not shadowed)
          float map = lightData[uint(lightGridSize.z) + light * 4 + 3].x;
          float reaches = inside;
          [branch] if (map > -0.5 && inside > 0) reaches *= LampShadow(world, N, uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * tint.w * saturate(1 - far2 * 25);
          // (a lamp low on a unit shines along the ground, which by the angle alone would hardly be lit:
          // within the beam the ground takes light as if the air scattered some of it down)
          nl = saturate(dot(N, l) + 0.6 * inside);
        }
        float3 h = normalize(V + l);
        float nh = saturate(dot(N, h));
        float dl = nh * nh * (a2 - 1) + 1;
        float3 fl = f0 + (1 - f0) * pow(1 - saturate(dot(V, h)), 5);
        float visl = 0.25 / ((nl * (1 - k) + k) * (nDotV * (1 - k) + k));
        small += (diffuseColor / 3.14159265 + a2 / (3.14159265 * dl * dl) * visl * fl * modern.w) * tint.rgb *
                 (nl * fade * 3.14159265);
      }
    }
  }
  return small;
}

// Light through leaves (World::leafLight, the view's leaves; 0 when it is off): a leaf, a grass blade or a plant is
// thin, so of the sun's light that falls on its far side some comes through to the side the eye sees, deeper in
// colour (it has passed through the leaf: its colour taken twice over, kept as bright in its strongest channel),
// and more of it toward an eye that looks toward the sun through it (leaves scatter the light they let through
// forward). `N`: the way the side the eye sees faces; `thin`: how much it lets through (0..1). What it returns is
// added as light of its own (ModernLight's `emission`, so the air dims it as the rest).
float LeafThrough(float3 N, float3 toCamera) {
  float back = saturate(dot(-N, sunDirection));
  float forward = pow(saturate(dot(-toCamera, sunDirection)), 8);
  return back * (1 + 2 * forward);
}
float3 LeafLight(float3 albedo, float through, float3 world, float shadow, float thin) {
  [branch] if (leaves.x < 0.5 || through <= 0) return 0;
  float3 falls = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 deep = albedo * albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 0.02);
  return deep * falls * (through * shadow * CloudShadow(world) * thin * leaves.y);
}

// The modern light on a surface. `albedo` is linear; `occlusion` dims the sky's light (1: open).
// Returns linear colour, unbounded.
float3 ModernLight(float3 albedo, float metallic, float roughness, float occlusion, float3 emission, float3 N, float3 world, float shadow) {
  shadow *= CloudShadow(world);  // (a computed sky's clouds)
#ifdef GROUND_MATT
  // [leftover] (the ground's shaders: Terrain::mattGrazing) how much of the sun's highlight the ground loses here: how low the
  // sun stands over the ground's own slope (groundGraze, the ground's shader's), where it is dry (groundDry: the ground's own
  // wet, a shore's band; below the weather's)
  float matt = groundGraze * groundDry;
#endif
  // the weather on the surface: wet is darker and shinier; snow lies on what faces up
  [branch] if (weather.x + weather.y > 0) {
    float upward = saturate(N.y * 1.5 - 0.2);
    albedo *= 1 - 0.25 * SURFACE_WET;  // [unit dirt] (SURFACE_WET: weather.x but on units)
    // puddles: where the ground is level, in patches that grow as it gets wetter: still water, which
    // mirrors the sky (Chris, 2026-10-20: mirroring, but "not so many puddles that the entire map is a mirror": some 8 %
    // of level ground when soaked, a third of before, and none until it has rained a good while)
    float puddle = SURFACE_PUDDLES * smoothstep(0.90, 0.97, N.y) * smoothstep(1.30 - 0.35 * weather.x, 1.40 - 0.35 * weather.x, CloudNoise(float3(world.xz * 0.45, 3.7)) + CloudNoise(float3(world.xz * 1.7, 8.1)) * 0.35);
#ifdef SURFACE_DEEP_TRACKS
    puddle = max(puddle, deepWater);  // (deep tracks: water standing in the ruts of wet mud, as the ground's shader found)
#endif
    if (puddle > 0) {
      N = normalize(lerp(N, float3(0, 1, 0), puddle));
      albedo *= 1 - 0.45 * puddle;
      roughness = lerp(roughness, 0.07, puddle);
    }
    roughness = lerp(roughness, 0.6, SURFACE_WET * (0.35 + 0.5 * upward) * (1 - puddle));  // [unit dirt] (SURFACE_WET; not over still water: the puddles keep their mirror, Chris 2026-10-20)
#ifdef SURFACE_DEEP_TRACKS
    roughness = lerp(roughness, deepGloss.x, deepGloss.y * (1 - puddle));  // [mud ruts] (wet mud in the ruts keeps its own gloss, terrain.cpp; still water over it its own)
#endif
    // snow builds up: it lies first where the surface is level, in patches; the patches close as
    // more falls, steeper ground comes last and the steepest stays bare. Where it lies thick it
    // hides the small shapes under it.
    float fallen = weather.y * SURFACE_SNOW * surfaceSnowShare;
    // (winter water, winter_water.h: no snow lies under the water, nor on the strip just over it that the waves wash,
    // which is dark with wet)
    [branch] if (winterWater.x > 0) {
      float washed = saturate((world.y - winterWater.y + 0.03 - 0.12 * CloudNoise(float3(world.xz * 0.7, 3.3))) * 12);
      fallen *= washed;
      albedo *= 1 - 0.45 * (1 - washed) * saturate(1 + (world.y - winterWater.y) * 3);  // (not deeper down: the water colours it)
    }
    float3 snowAt = surfaceSnowOwn ? surfaceSnowPlace : world;
    float patch = CloudNoise(snowAt * 0.9) * 0.55 + CloudNoise(float3(snowAt.xz * 0.21, 5.3)) * 0.45;
    float takes = patch * 0.75 + saturate((N.y - 0.35) * 1.6) * 0.25;  // how readily it lies here
#ifdef GROUND_SNOW
    // [snow ground] (terrain.cpp) deeper on slopes away from the wind and in hollows, thinner and patchier under trees and
    // on steep and windward slopes
    fallen *= snowLie;
    takes -= snowLess;
#endif
    float bar = 1.02 - fallen * 1.15;
    float lying = smoothstep(bar, bar + 0.2, takes) * smoothstep(0.25, 0.5, N.y);
#ifdef GROUND_SNOW
    // [snow ground] where it thins: over a wider margin, mottled (thinner snow, islands and holes, so that a large bare patch
    // does not read as cut out), and close by broken up by the ground's and its own height (the hollows first, the ground's
    // stones showing through); its creases in place of the ground's
    [branch] if (snowEdge.x >= 0) {
      lying = smoothstep(bar - 0.15, bar + 0.35, takes) * smoothstep(0.25, 0.5, N.y);
      lying = saturate(lying + (snowMottle - 0.5) * 3.2 * lying * (1 - lying));
      // ([snowthin] with the map's thin snow's cover: as it builds up the snow lies where the cover is over what is still bare
      // of it (1 - how much lies), in the real scan's patches over the map's own ground, and whole once it all lies; without
      // it, broken up by the ground's and its own heights)
      float edged = snowCover >= 0 ? smoothstep(-0.08, 0.08, snowCover - (1 - lying) * 1.1) : saturate((snowEdge.x + lying * 1.6 - 1) / 0.6);
      lying = lerp(lying, edged, snowEdge.y);
    }
    occlusion = lerp(occlusion, snowMatter.x, lying * snowSurface.w);
#endif
#ifdef SURFACE_TRACKS
    // where units have stood and driven the snow is trodden away, down to a dark wet ground
    [branch] if (tracksArea.z > 0.5 && lying > 0) {
      // (four readings half a pixel of the picture apart: one alone shows the picture's pixels as steps
      // along every track that runs aslant)
      float2 at = saturate(world.xz * tracksArea.xy), half = 0.5 / 2048.0;
      float trodden = (tracksMap.SampleLevel(samplerLinearClamp, at + float2(half.x, half.y), 0).r + tracksMap.SampleLevel(samplerLinearClamp, at + float2(-half.x, half.y), 0).r +
                       tracksMap.SampleLevel(samplerLinearClamp, at + float2(half.x, -half.y), 0).r + tracksMap.SampleLevel(samplerLinearClamp, at - float2(half.x, half.y), 0).r) * 0.25;
#ifdef SURFACE_DEEP_TRACKS
      // (with deep tracks on, the trough is theirs - each tread's own print, its cleats packed into the snow: this coarse map
      // of where units went, half a world unit a texel, only packs the snow a little more round them. Taking it away as
      // well cleared a wide unit's whole tread band down to the brown ground, a soft band like dust: the Fatboy's check)
      if (deepTrack.w > 0.5) trodden *= 0.3;
#endif
      albedo *= 1 - 0.15 * trodden * lying;
      lying *= 1 - 0.78 * trodden;  // (trodden snow, not bare earth: some of it stays)
#ifdef GROUND_SNOW
      lying *= (1 - 0.45 * trodden) / (1 - 0.78 * trodden);  // [snow ground] (more of it stays, packed: below)
      snowTrodden = trodden;
#endif
    }
#endif
#ifdef SURFACE_DEEP_TRACKS
    // deep tracks (deep_tracks.h; the ground's shader read them: deepTrack x how deep the trough is, y the berm
    // beside it, z how much of their relief shows from here, w 1 = on; deepSlope their slopes): through lying snow
    // a trough goes down to packed, greyer, bluer snow (where little lies, to the dark wet ground), its walls hiding
    // part of the sky from its bottom, the snow pushed out heaped beside it. Its relief and its packed colour are the
    // snow's own: laid on after the snow has smoothed and covered what lies under it (below). (Off, the lines below are
    // left exactly as they were: the snow's colour a constant there, so that the compiler makes the same of them.)
    float2 trackTilt = 0;
    float packed = 0;
    [branch] if (deepTrack.w > 0.5 && lying > 0) {
      float thick = saturate(fallen * 1.6 - 0.2);
      albedo *= 1 - 0.3 * deepTrack.x * (1 - thick);
      lying = saturate(lying * (1 - deepTrack.x * (0.9 - 0.6 * thick)) + deepTrack.y * thick * 0.8);
      packed = deepTrack.x * thick * 0.85;
      occlusion *= 1 - 0.45 * deepTrack.x * deepTrack.z * lying;
      trackTilt = (-deepSlope.xy + 0.8 * deepSlope.zw) * (0.1 * lying * deepTrack.z);
    }
#endif
    N = normalize(lerp(N, SNOW_UP, SNOW_FLAT(lying, fallen)));
    albedo = lerp(albedo, SNOW_COLOUR, lying);
#ifdef SURFACE_DEEP_TRACKS
    // (deep tracks: the trough's relief in the snow, and the snow packed down in it)
#ifdef GROUND_SNOW
    // [snow ground] (the snow tracks' trodden snow packed too; packed snow its own material's flatter shapes, under the
    // trough's relief)
    packed = max(packed, snowTrodden * 0.85);
    N = normalize(lerp(N, snowPackedNormal, packed * lying * snowSurface.w));
#endif
    if (any(trackTilt != 0)) N = normalize(N - float3(trackTilt.x, 0, trackTilt.y));
    if (packed > 0) albedo = lerp(albedo, SNOW_PACKED, packed * lying);
#endif
    metallic *= 1 - lying;
    roughness = lerp(roughness, SNOW_ROUGH, lying);
#ifdef GROUND_SNOW
    // [snow ground] packed snow its own material's roughness; wet snow at the thaw and in rain darker, greyer and glossier
    // (the hollows first, terrain.cpp); the sky's light cut by what is round the place (its shelter)
    roughness = lerp(roughness, snowPacked.w, packed * lying * snowSurface.w);
    float slush = snowWet * lying;
    albedo = lerp(albedo, dot(albedo, float3(0.3, 0.6, 0.1)) * float3(0.93, 0.98, 1.06), 0.35 * slush) * (1 - 0.22 * slush);
    roughness = lerp(roughness, 0.28, slush);
    occlusion *= lerp(snowBareSky, snowSky, lying);
#endif
#ifdef GROUND_MATT
    matt *= (1 - saturate(SURFACE_WET * 2)) * (1 - puddle) * (1 - lying);  // [leftover] (wet ground, still water and snow keep their gloss)
#endif
  }
  [branch] if (inspect.x > 0.5) {
    // a view for checking art: one of the surface's own numbers as a picture, as the screen would
    // show a colour of that value (so 0.5 reads as middle grey)
    float3 value = inspect.x < 1.5 ? pow(max(albedo, 0), 1 / 2.2)
                 : inspect.x < 2.5 ? N * 0.5 + 0.5
                 : inspect.x < 3.5 ? roughness
                 : inspect.x < 4.5 ? metallic
                 : inspect.x < 5.5 ? occlusion
                 : shadow;
    if (inspect.x < 6.5) return pow(max(value, 0), 2.2) * (modern.x * lightMultiplier);
    if (inspect.x > 7.5 && inspect.x < 8.5) {
      // which shadow map each place uses: red the nearest, then green, blue, yellow; grey: none
      int map = SunCascade(world);
      float3 mark = map == 0 ? float3(1, 0.25, 0.2) : map == 1 ? float3(0.25, 0.9, 0.3) : map == 2 ? float3(0.3, 0.45, 1) : map == 3 ? float3(1, 0.9, 0.25) : float3(0.5, 0.5, 0.5);
      return mark * (0.25 + 0.75 * dot(pow(max(albedo, 0), 1 / 2.2), float3(0.3, 0.6, 0.1)) * shadow) * (modern.x * lightMultiplier);
    }
  }
  float3 V = normalize(cameraPosition - world);
  float3 L = sunDirection;
  float3 H = normalize(V + L);
  float nDotV = saturate(dot(N, V)) + 1e-4;
  float nDotL = saturate(dot(N, L));
  float nDotH = saturate(dot(N, H));
  float vDotH = saturate(dot(V, H));
  roughness = clamp(roughness, 0.06, 1);
  float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
  float3 diffuseColor = albedo * (1 - metallic);

  // the sun: GGX highlight, Smith visibility, Schlick fresnel, Lambert diffuse
  float a = roughness * roughness, a2 = a * a;
  float d = nDotH * nDotH * (a2 - 1) + 1;
  float D = a2 / (3.14159265 * d * d);
  float k = a * 0.5;
  float Vis = 0.25 / ((nDotL * (1 - k) + k) * (nDotV * (1 - k) + k));
  float3 F = f0 + (1 - f0) * pow(1 - vDotH, 5);
#ifdef GROUND_MATT
  // [leftover] dry ground is matt at grazing light: a scan's roughness (soil some 0.5) says how glossy its grains are seen from
  // above; with the sun skimming the ground, the light reaches only their tops and the clods' and blades' shade takes the
  // rest, so the forward highlight that would read as wet plastic at dusk and dawn fades out (to a tenth with the sun on the
  // horizon)
  [branch] if (matt > 0) F *= 1 - 0.9 * matt;
#endif
  float3 small = SmallLights(world, N, V, diffuseColor, f0, a2, k, nDotV);
  if (inspect.x > 6.5 && inspect.x < 7.5) return small;
  float3 sunRadiance = ToLinear(sunColor) * (lightMultiplier * modern.x);
  // (the sun's highlight is held back by the same share as the mirrored sky, below: at full strength
  // dark paint of middling roughness shows more sheen than colour)
  float3 color = (diffuseColor / 3.14159265 + D * Vis * F * modern.w) * sunRadiance * (nDotL * shadow) * 3.14159265;

  // the sky: its blurriest level lights the surface from all sides, a sharper level is mirrored
  float3 R = reflect(-V, N);
  float levels = 8;
  float3 skyDiffuse, skySpecular;
  [branch] if (skyZenith.w > 0.5) {  // the computed sky (sky.h)
    skyDiffuse = SkyAmbient(N);
    skySpecular = lerp(ComputedSky(R), SkyAmbient(R), saturate(roughness * 1.5));
  } else {
    skyDiffuse = ToLinear(environmentSky.SampleLevel(samplerLinear, N, levels).rgb);
    skySpecular = ToLinear(environmentSky.SampleLevel(samplerLinear, R, roughness * levels).rgb);
  }
  float3 fill = ToLinear(shadowFill) * 0.5;
#if RAYS
  [branch] if (rayEffects.x > 0.5 && roughness < 0.5) {
    // reflections by ray tracing (ray_scene.h): what the mirrored ray meets, lit as the probes light it (bounce.h)
    // or by the sky's fitted light, the sun by a ray toward it; the sky where it meets nothing. Rougher, the more
    // of the sky's reflection as before.
    uint seed = RayHash(asuint(world.x * 37.0) ^ RayHash(asuint(world.z * 53.0) + asuint(world.y)));
    RayHit hit;
    float3 traced = skySpecular;
    if (RayTrace(world + N * 0.05, R, 0.02, rayEffects.y, 0xff, seed, hit)) {
      float3 p = world + R * hit.t, hitAlbedo, n;
      RaySurfaceAt(hit, p, R, rayEffects.zw, hitAlbedo, n);
      traced = RayLitAt(p, n, hitAlbedo, seed, 8) / max(modern.y, 1e-4);  // (8 of the small lights at most: all would be dear)
    }
    skySpecular = lerp(traced, skySpecular, saturate(roughness * 2));
  }
#endif
  [branch] if (bounce.x > 0.5) {  // bounce light (bounce.h): the probes' light in place of the sky's fitted light, which stays outside their grid
    float inside;
    float3 gathered = BounceAt(world, N, inside);
    skyDiffuse = lerp(skyDiffuse, gathered / max(modern.y, 1e-4), inside);
  }
  if (inspect.x > 9.5 && inspect.x < 10.5) return (skyDiffuse + fill) * modern.y;  // (the view of it: on a white surface)
  // (the mirrored sky is held back: the game's sky pictures have no values beyond white and no
  // occlusion of their own, and at full strength every metal turns into a pale mirror of the clouds)
  color += (diffuseColor * (skyDiffuse + fill) + skySpecular * EnvironmentBrdf(f0, roughness, nDotV) * modern.w) * (modern.y * occlusion);
  // rain on what faces up: each small square of ground has a drop falling on it again and again,
  // and its ring runs outward and fades (near the camera only: from far they would only flicker)
  float toEye = length(cameraPosition - world);
  [branch] if (falling.x > 0 && N.y > 0.6 && toEye < 140) {
    float2 g = world.xz * 3.0;
    float2 cell = floor(g);
    float h = CloudHash(float3(cell, 1.7));
    float age = frac(time * (0.9 + h) + h * 7.0);
    float2 centre = cell + 0.2 + 0.6 * float2(CloudHash(float3(cell, 5.1)), CloudHash(float3(cell, 9.4)));
    float ring = abs(length(g - centre) - age * 0.45);
    float splash = saturate(1 - ring * 6) * (1 - age) * (1 - age) * step(h, falling.x * 0.45) * saturate(1 - toEye / 140);
    color += (skyZenith.rgb + skyHorizon.rgb) * (splash * 0.1);
  }
  return ThroughAirTo(color + small + emission, world);
}
)hlsl";

}  // namespace render
