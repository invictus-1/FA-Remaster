// The lighting every scene shader shares, as HLSL source placed after kViewShaderBlock:
//  - the sun's shadow (cascaded shadow maps),
//  - the original game's light formula,
//  - the modern one: physically based (metalness / roughness), sun plus sky light from the map's
//    sky cube, in linear colour with values beyond 1 kept for the last pass to map to the screen.
// A shader computes its surface (colour, normal, material) and asks one of the two for the result,
// by the `look` value of the View block.
#pragma once

namespace render {

inline const char kLightingShaderBlock[] = R"hlsl(
Texture2DArray<float> shadowMaps : register(t28);
TextureCube environmentSky : register(t29);

// 1 in sunlight, 0 in shadow. `normal` is the surface's (the lookup is moved a little along it, which
// keeps a lit surface from shadowing itself).
float SunShadow(float3 world, float3 normal) {
  if (shadowsOn < 0.5) return 1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float texel = cascadeTexel[i];
    float3 p = world + normal * (texel * 1.5) + sunDirection * (texel * 1.0);
    float3 s = mul(shadowMatrix[i], float4(p, 1)).xyz;
    if (all(s.xy > 0.02) && all(s.xy < 0.98) && s.z > 0 && s.z < 1) {
      float lit = 0;
      float step = 1.0 / 2048.0;
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
          lit += shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy + float2(x, y) * step, i), s.z);
      return lit / 9.0;
    }
  }
  return 1;
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

// The modern light on a surface. `albedo` is linear; `occlusion` dims the sky's light (1: open).
// Returns linear colour, unbounded.
float3 ModernLight(float3 albedo, float metallic, float roughness, float occlusion, float3 emission, float3 N, float3 world, float shadow) {
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
  float3 sunRadiance = ToLinear(sunColor) * (lightMultiplier * modern.x);
  // (the sun's highlight is held back by the same share as the mirrored sky, below: at full strength
  // dark paint of middling roughness shows more sheen than colour)
  float3 color = (diffuseColor / 3.14159265 + D * Vis * F * modern.w) * sunRadiance * (nDotL * shadow) * 3.14159265;

  // the sky: its blurriest level lights the surface from all sides, a sharper level is mirrored
  float3 R = reflect(-V, N);
  float levels = 8;
  float3 skyDiffuse = ToLinear(environmentSky.SampleLevel(samplerLinear, N, levels).rgb);
  float3 skySpecular = ToLinear(environmentSky.SampleLevel(samplerLinear, R, roughness * levels).rgb);
  float3 fill = ToLinear(shadowFill) * 0.5;
  // (the mirrored sky is held back: the game's sky pictures have no values beyond white and no
  // occlusion of their own, and at full strength every metal turns into a pale mirror of the clouds)
  color += (diffuseColor * (skyDiffuse + fill) + skySpecular * EnvironmentBrdf(f0, roughness, nDotV) * modern.w) * (modern.y * occlusion);
  return color + emission;
}
)hlsl";

}  // namespace render
