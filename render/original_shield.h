// [ogshield] The original game's shields, each faction's bubble as its own shaders draw it (medium detail): the sums the
// game makes over its own textures, read from the game and written anew here. Shared by the original look's shields
// (mesh_renderer.cpp) and the modern look's energy shields (energy_shields.cpp), which start from this look and add the
// world's light to it (Chris, 2026-10-28: the modern shields an evolution of the base game's, in its colours).
//
// OriginalShieldLook(whose, uv, N, T, B, view, s, health, computedSky, ...): whose 0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim;
// `uv` the sphere's coordinates (the game's shields read their first set in all four places); N the sphere's way out, T
// and B its surface's ways (the mesh's tangent and binormal); `view` from the eye to the place; `s` seconds; `health`
// how much of the shield is left (1: full; the game tints a weakened one toward red); `computedSky`: the Aeon mirrors the
// computed sky (which follows the hour) instead of the map's cube. Returns the colour as the game has it (on a screen's
// scale, not light's) and its alpha (for blending over what is behind).
#pragma once

namespace render {

inline const char kOriginalShieldBlock[] = R"hlsl(
float3 OriginalShieldNormal(Texture2D map, float2 uv, float3 N, float3 T, float3 B) {  // (its normal map: green, alpha)
  float3 t = 2 * map.Sample(samplerAniso, uv).gaa - 1;
  t.z = sqrt(saturate(1 - t.x * t.x - t.y * t.y));
  return normalize(t.x * B + t.y * T + t.z * N);
}
float4 OriginalShieldLook(int whose, float2 uv, float3 N, float3 T, float3 B, float3 view, float s, float health, bool computedSky,
                          Texture2D albedoTex, Texture2D normalsTex, Texture2D specularTex, Texture2D secondaryTex, TextureCube environment) {
  static const float kPi = 3.14159265;
  if (whose == 1) {  // Aeon: the sky mirrored in a rippled surface, laced by two drifting noises
    float4 albedo = albedoTex.Sample(samplerAniso, uv);
    float3 spec = specularTex.Sample(samplerAniso, uv * 12 + float2(0, 0.032) * s).rgb;
    float3 spec2 = specularTex.Sample(samplerAniso, uv * 8 + float2(0.012, -0.032) * s).rgb;
    float3 n = OriginalShieldNormal(normalsTex, (uv * 3 + float2(0, 0.0012) * s) * 4, N, T, B);
    float3 R = reflect(view, n);
    float phong = saturate(dot(R, -view)) * 0.6;
    float3 env = computedSky ? pow(saturate(ComputedSky(R)), 1 / 2.2) : environment.Sample(samplerLinear, R).rgb;
    float3 c = (phong + env - albedo.ggg) * (spec.rrr * lerp(0.6, 1.3, sin(frac(0.015 * s) * kPi))) *
               (spec2.rrr * lerp(2.0, 2.2, sin(frac(0.0045 * s) * kPi)));
    c = (c * n * 0.65 + c) * env * albedo.a;
    c = lerp(lerp(float3(0.7, 0.3, 0.3), c, 0.9), c, health);
    return float4(max(c, 0), saturate(0.707 * dot(env, 0.25) + albedo.b * 0.5));
  }
  if (whose == 2) {  // Cybran: a dark violet with its noises' bright threads
    float4 albedo = albedoTex.Sample(samplerAniso, uv + float2(-0.01, 0) * s);
    float4 albedo2 = albedoTex.Sample(samplerAniso, uv + float2(-0.002, 0) * s);
    float3 spec = specularTex.Sample(samplerAniso, uv * 2 + float2(0, 0.0012) * s).rgb;
    float3 spec2 = specularTex.Sample(samplerAniso, uv + float2(0.001, -0.0015) * s).rgb;
    float3 a = albedo2.b * spec2.g * 3, b = spec2.g * albedo.a, d = (albedo2.g - spec2.b) * spec.b * albedo.a;
    float3 c = float3(0.05, 0, 0.3) + d - a * b;
    float3 weak = lerp(float3(0.2, 0, 0), c, 0.5);
    weak = lerp(c, (weak - c) + (d + weak), sin(frac(0.06 * s) * kPi));
    c = lerp(weak, c, health);
    c += (albedo.r + albedo2.r) * 0.1 - (1 - albedo.a);
    float sum = c.r + c.g + c.b;
    c = sum < 0.1 ? float3(0.15, 0.15, 0.3) : sum < 0.2 ? spec.bbb : c;
    c += (albedo.r + albedo2.r) * float3(0, 0, 0.3);
    return float4(max(c, 0), saturate(0.17 + (albedo.r + albedo2.r) * 0.2));
  }
  if (whose == 3) {  // Seraphim: pale blue veins drifting through a rippled surface, clearer toward its top
    float2 at = uv * 11 + float2(-0.005, -0.045) * s;
    float4 pixel = normalsTex.Sample(samplerAniso, at);
    float3 n = OriginalShieldNormal(normalsTex, at, N, T, B);
    float4 address = normalsTex.Sample(samplerAniso, uv + float2(0.003, -0.0045) * s);
    float3 spec = specularTex.Sample(samplerAniso, uv * 5 + float2(-0.00153, -0.0159) * s + address.rb * 0.1).rgb;
    float dp = abs(cos(n.y));
    float channel = 0.453 - clamp(1 - dp, 0, 0.453);
    float t = abs(N.y);
    float m = t < 0.753 ? 1 : 1 - 0.7 * (t - 0.753) / (1 - 0.753);
    float alpha = m * (abs(dot(view, n)) * 0.3 + channel) * 1.75;
    return float4(float3(0.425, 0.76274, 1.0) * dp * dp * spec, saturate(alpha));
  }
  // UEF: blue, its see-through changing in patches by three noises against each other
  float4 mask = albedoTex.Sample(samplerAniso, uv);
  float4 albedo = albedoTex.Sample(samplerAniso, uv * 3 + float2(0.0003, 0.005) * s);
  float3 n = 2 * secondaryTex.Sample(samplerAniso, uv * 32 + float2(-0.001, -0.005) * s).gaa - 1;
  n.z = sqrt(saturate(1 - n.x * n.x - n.y * n.y));
  float3 spec = specularTex.Sample(samplerAniso, uv * 6 + float2(-0.0003, -0.0008) * s).rgb;
  float3 c = dot(albedo.rgr, n) + float3(0, 0, 0.25);
  float w = sin(frac(0.01 * s) * kPi);
  float alpha;
  if (spec.g <= albedo.r) alpha = spec.b >= albedo.g ? (c.b >= n.b ? 0.12 : lerp(0.05, 0, w)) : (n.b >= albedo.r ? 0.2 : lerp(0.01, 0.1, w));
  else alpha = spec.r >= albedo.r ? (spec.b >= albedo.r ? 0.025 : 0.1) : (spec.g >= albedo.g ? 0.02 : lerp(0.37, 0.46, w));
  c += float3(0, 0, 0.15);
  float3 weak = lerp(float3(0.5, 0, 0), c, 0.5);
  weak = lerp(c, weak + c, sin(frac(0.06 * s) * kPi));
  c = lerp(weak, c, health) + mask.b * 0.95;
  return float4(max(c, 0), saturate(alpha * mask.a));
}
)hlsl";

}  // namespace render
