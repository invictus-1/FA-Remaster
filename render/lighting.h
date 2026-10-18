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
#ifdef SURFACE_TRACKS
Texture2D tracksMap : register(t26);  // (the ground only) how much of the lying snow is trodden away at a place
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

// The world beyond the map's edge: ground or water there, a shade dimmer just outside (the
// playable ground ends here) and lost in the distance farther out: it goes over into what the
// sky's pass shows in that direction, so the land has no rim. On the map: the colour as it is.
float3 BeyondFar(float3 color, float3 world, float outside, float pixel) {
  float3 d = normalize(world - cameraPosition);
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
  float t = saturate(outside * beyond.z);
  // without the haze (beyond.w < 0): the land stays clear and is lost only in the last tenth of
  // the way, where it meets the horizon
  if (beyond.w < 0) t = saturate((t - 0.9) * 10);
  // just outside: dimmer and with a third of its colour gone, behind a thin dark line along the
  // edge that keeps its width in the picture at any zoom: where the playable ground ends must be
  // plain from all the way up
  float near = saturate(outside / 6);
  color = lerp(color, dot(color, float3(0.3, 0.6, 0.1)), 0.35 * near) * lerp(1, abs(beyond.w), near);
  if (pixel > 0) color *= 1 - 0.7 * saturate(1 - outside / max(pixel * 1.6, 1e-4));
  return lerp(color, back, t * t * (3 - 2 * t));
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
  // the weather on the surface: wet is darker and shinier; snow lies on what faces up
  [branch] if (weather.x + weather.y > 0) {
    float upward = saturate(N.y * 1.5 - 0.2);
    albedo *= 1 - 0.25 * weather.x;
    // puddles: where the ground is level, in patches that grow as it gets wetter: still water, which
    // mirrors the sky
    float puddle = SURFACE_PUDDLES * smoothstep(0.90, 0.97, N.y) * smoothstep(1.24 - 0.45 * weather.x, 1.34 - 0.45 * weather.x, CloudNoise(float3(world.xz * 0.45, 3.7)) + CloudNoise(float3(world.xz * 1.7, 8.1)) * 0.35);
    if (puddle > 0) {
      N = normalize(lerp(N, float3(0, 1, 0), puddle));
      albedo *= 1 - 0.45 * puddle;
      roughness = lerp(roughness, 0.07, puddle);
    }
    roughness = lerp(roughness, 0.6, weather.x * (0.35 + 0.5 * upward));
    // snow builds up: it lies first where the surface is level, in patches; the patches close as
    // more falls, steeper ground comes last and the steepest stays bare. Where it lies thick it
    // hides the small shapes under it.
    float fallen = weather.y * SURFACE_SNOW;
    float patch = CloudNoise(world * 0.9) * 0.55 + CloudNoise(float3(world.xz * 0.21, 5.3)) * 0.45;
    float takes = patch * 0.75 + saturate((N.y - 0.35) * 1.6) * 0.25;  // how readily it lies here
    float bar = 1.02 - fallen * 1.15;
    float lying = smoothstep(bar, bar + 0.2, takes) * smoothstep(0.25, 0.5, N.y);
#ifdef SURFACE_TRACKS
    // where units have stood and driven the snow is trodden away, down to a dark wet ground
    [branch] if (tracksArea.z > 0.5 && lying > 0) {
      // (four readings half a pixel of the picture apart: one alone shows the picture's pixels as steps
      // along every track that runs aslant)
      float2 at = saturate(world.xz * tracksArea.xy), half = 0.5 / 2048.0;
      float trodden = (tracksMap.SampleLevel(samplerLinearClamp, at + float2(half.x, half.y), 0).r + tracksMap.SampleLevel(samplerLinearClamp, at + float2(-half.x, half.y), 0).r +
                       tracksMap.SampleLevel(samplerLinearClamp, at + float2(half.x, -half.y), 0).r + tracksMap.SampleLevel(samplerLinearClamp, at - float2(half.x, half.y), 0).r) * 0.25;
      albedo *= 1 - 0.15 * trodden * lying;
      lying *= 1 - 0.78 * trodden;  // (trodden snow, not bare earth: some of it stays)
    }
#endif
    N = normalize(lerp(N, float3(0, 1, 0), 0.5 * lying * saturate(fallen * 2 - 0.8)));
    albedo = lerp(albedo, float3(0.50, 0.53, 0.58), lying);
    metallic *= 1 - lying;
    roughness = lerp(roughness, 0.7, lying);
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
