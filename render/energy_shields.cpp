#include "energy_shields.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "lighting.h"

namespace render {
namespace {

constexpr int kMostLit = 32;  // bubbles whose light on what is inside them each pixel adds up (the nearest)
constexpr int kMostHits = 8;  // a bubble's hits at once (the newest)

// Two passes over each bubble's dome (its own mesh, at the place the frame has it):
//  InsidePS, over the shell nearest the eye, before anything see-through: the picture behind it (Post::CopyBehind),
//    taken from a little to the side (the bending), with the bubbles' light on what is inside them.
//  ShellPS, over both sides of the shell, blended over that: the plain bubble's look (mesh_renderer.cpp, kind 3: its
//    faction's tint, brighter toward the rim, its pattern of the albedo's red times green at two sizes drifting
//    against each other), and the rings of its hits, its pattern flaring, its weakening.
// The way out from the bubble's centre is taken from the sphere itself (the domes are spheres), not from the mesh.
const char kShader[] = R"hlsl(
cbuffer Shields : register(b1) {
  row_major float4x4 fromScreen;  // the picture's place (-1..1, y up) and depth -> the world (viewProj's inverse)
  float4 frameInfo;                // x: bubbles in `lit`; y: how much they bend what is behind; z: how much light they
                                   // throw on what is inside; w: how strong their rings are
  float4 lit[64];                  // the bubbles whose light falls inside them: (centre, radius), (tint, strength)
  float4 shellLook;                // [litshell] x: -; y: its hex lines' strength; z: its glow where it meets the ground and
                                   // what stands in it (the bubbles with the lit shell); w: -
};
cbuffer Bubble : register(b2) {
  float4 row0, row1, row2;  // the dome's model -> world
  float4 sphere;            // its centre and radius, world units
  float4 tintHealth;        // its faction's tint (as the plain bubble has it); w: how much of the shield is left (0..1)
  float4 info;              // x: its hits; y: 1 = the eye is inside it; z: a number of its own (its stutter); w: [litshell]
                            // the lit shell: its hex's steps to an icosahedron's edge, or -1 with its texture's pattern;
                            // 0: the plain shell
  float4 hits[8];           // xyz: the way from its centre to where it was struck; w: how many seconds ago
};
Texture2D albedoMap : register(t0);  // the dome's texture (its faction's pattern)
Texture2D behind : register(t1);     // the picture as it was before the bubbles (InsidePS only)
#if SAMPLES > 1
Texture2DMS<float> sceneDepth : register(t2);
#define SCENE_DEPTH(at, s) sceneDepth.Load(at, s)
#else
Texture2D<float> sceneDepth : register(t2);
#define SCENE_DEPTH(at, s) sceneDepth.Load(int3(at, 0))
#endif

struct VIn {
  float3 position : POSITION;
  float4 uv : TEXCOORD0;
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
};

VOut VS(VIn i) {
  VOut o;
  float4 p = float4(i.position, 1);
  o.world = float3(dot(row0, p), dot(row1, p), dot(row2, p));
  o.position = mul(viewProj, float4(o.world, 1));
  o.uv = i.uv.xy;
  return o;
}

// A hit's rings: three crests running out over the shell from where it struck, the first the strongest, at kSpeed
// radians a second (the same time across a small bubble as a large one), fading over kLife seconds (the game's
// flash lasts two) and as they spread.
static const float kSpeed = 1.5;
static const float kLife = 1.9;
static const float kSpacing = 0.17;  // radians between a hit's crests
static const float kWidth = 0.035;   // a crest's width, radians

// At a point of the shell (n: its way out from the centre): how high the rings stand there (h), which way the shell
// leans along itself there (lean: away from a hit where a crest rises toward the point, unit-less), and how much
// the place where a hit just struck glows (flare).
void Ripples(float3 n, out float h, out float3 lean, out float flare) {
  h = 0;
  lean = 0;
  flare = 0;
  int count = (int)(info.x + 0.5);
  for (int k = 0; k < count; ++k) {
    float3 d = hits[k].xyz;
    float age = hits[k].w;
    float c = clamp(dot(n, d), -1, 1);
    float a = acos(c);  // radians over the shell from where it struck
    float life = saturate(1 - age / kLife);
    float s = life * life / (1 + 2.0 * a);
    float x = age * kSpeed - a;  // how far the front has run past this point
    float wave = 0, slope = 0;
    [unroll] for (int j = 0; j < 3; ++j) {
      float y = (x - j * kSpacing) / kWidth;
      float g = exp(-y * y) * (j == 0 ? 1.0 : j == 1 ? 0.55 : 0.3);
      wave += g;
      slope += g * y;  // (the crest's rise toward the hit side and fall beyond it)
    }
    h += wave * s;
    float3 away = n * c - d;  // along the shell, away from where it struck
    float along = length(away);
    if (along > 1e-4) lean += away * (slope * s / along);
    float since = saturate(1 - age / 0.9);
    flare += exp(-a * a * 12) * since * since;
  }
}

// [litshell] (Chris, 2026-10-28: the hex shield should take the world's light, not be the original's look made fancier; a
// finer hex.) A hex of cells over the whole sphere, as a geodesic dome is made: an icosahedron's twenty faces (kIcoA..C,
// made with the shader: IcosahedronBlock), each with a triangular lattice of `f` steps to an edge, laid out onto the sphere;
// the cells round its points are hexagons (twelve pentagons where its corners are), and the points along an edge are
// both faces' alike, so the hex runs on across it.
// For a place p on the sphere: its cell (a number of its own), the cell's middle on the sphere, and how far it is from the
// cell's border (0 on it, 1 at its middle).
void HexCell(float3 p, float f, out float cell, out float3 middle, out float border) {
  // (its face: the one whose middle is nearest)
  int face = 0;
  float best = -2;
  [loop] for (int k = 0; k < 20; ++k) {
    float d = dot(p, normalize(kIcoA[k] + kIcoB[k] + kIcoC[k]));
    if (d > best) best = d, face = k;
  }
  float3 A = kIcoA[face], B = kIcoB[face], C = kIcoC[face];
  // onto the face's plane, from the centre; there in the face's own steps (u along AB, v along AC), f to an edge
  float3 nF = normalize(cross(B - A, C - A));
  float3 q = p * (dot(nF, A) / max(dot(nF, p), 1e-4)) - A;
  float3 e1 = B - A, e2 = C - A;
  float a11 = dot(e1, e1), a12 = dot(e1, e2), a22 = dot(e2, e2), b1 = dot(q, e1), b2 = dot(q, e2);
  float det = a11 * a22 - a12 * a12;
  float2 uv = float2(a22 * b1 - a12 * b2, a11 * b2 - a12 * b1) / det * f;
  // the triangular lattice in a plane of its own (a step 1 long): its points' cells are regular hexagons
  static const float kHalfRoot3 = 0.8660254;
  float2 at = float2(uv.x + uv.y * 0.5, uv.y * kHalfRoot3);
  float2 base = floor(uv);
  float2 nearest = base;
  float d1 = 1e9;
  [unroll] for (int s = 0; s < 4; ++s) {
    float2 g = base + float2(s % 2, s / 2);
    float2 c = float2(g.x + g.y * 0.5, g.y * kHalfRoot3);
    float d = dot(at - c, at - c);
    if (d < d1) d1 = d, nearest = g;
  }
  float2 off = at - float2(nearest.x + nearest.y * 0.5, nearest.y * kHalfRoot3);
  float most = max(abs(off.x), max(abs(dot(off, float2(0.5, kHalfRoot3))), abs(dot(off, float2(-0.5, kHalfRoot3)))));
  border = saturate((0.5 - most) * 2);
  middle = normalize(A + (nearest.x * e1 + nearest.y * e2) / f);
  cell = dot(floor(middle * 977 + 0.5), float3(1, 57, 3361));  // (the same for a point both faces share)
}

// The shell, taking the world's light: a clear shield of energy, like glass in that it mirrors the sky and the sun and
// catches the light of what burns and shines near it (shaded where the ground or the clouds shade it, and seen through the
// air as all else); its hex of thin lines in its faction's tint, which glow on their own (more toward its rim, by night
// more than by day, each cell's a little in its own time), and its cells filled with light as a hit's rings pass over them.
float4 LitShell(VOut i, float3 n, float3 v) {
  float3 N = dot(n, v) >= 0 ? n : -n;  // (the side the eye sees)
  float nDotV = saturate(dot(N, v));
  float fresnel = 0.04 + 0.96 * pow(1 - nDotV, 5);
  float dark = falling.w > 0 ? falling.w : 1;
  float3 tint = tintHealth.rgb;
  float health = saturate(tintHealth.w), weak = 1 - health;
  // its hex: the cell a place is in, and how near its border
  float lines, own = 0;
  float3 cellN = n;
  [branch] if (info.w > 0.5) {
    float cell, border;
    float3 middle;
    HexCell(n, info.w, cell, middle, border);
    float aa = max(fwidth(border), 1e-4);
    lines = (1 - smoothstep(0.06, 0.06 + aa * 1.5, border)) * saturate(1.6 - aa * 3);  // (where the cells are a few pixels across, faded)
    cellN = middle;
    own = frac(sin(cell * 12.9898 + info.z) * 43758.5453);
  } else {
    lines = saturate(albedoMap.Sample(samplerLinear, i.uv * 3 + float2(0.01, 0.03) * time).r *
                     albedoMap.Sample(samplerLinear, i.uv * 7 - float2(0.02, 0.013) * time).g * 2.5);
  }
  lines *= shellLook.y;
  // a hit's rings and flare: over whole cells (a cell lights up as a crest crosses it)
  float h, flare;
  float3 lean;
  Ripples(cellN, h, lean, flare);
  float rim = pow(1 - nDotV, 3);
  float pulse = 0.65 + 0.35 * sin(time * (0.9 + own * 1.7) + own * 6.2831853);
  // (weakening, its hex's: its cells go out one by one, the weaker it is the more (a third left dark at a quarter of its
  // shield, half at none), and below half its shield those still lit flicker, each in its own time)
  float on = 1;
  [branch] if (info.w > 0.5) {
    on = 1 - smoothstep(-0.05, 0.05, weak * 0.7 - 0.2 - own);
    on *= 1 - 0.85 * saturate((CloudNoise(float3(time * 9, own * 31, info.z)) - 0.62) * 4) * smoothstep(0.5, 0.8, weak);
    lines *= lerp(0.25, 1, on);
  }
  // (rain and snow on it: a drop that meets it sparks a cell for a moment, on its upper side; flakes faintly)
  float spark = 0;
  [branch] if (falling.x + falling.y > 0.01 && info.w > 0.5) {
    float when = frac(time * (1.4 + own * 0.8) + own * 17.3);
    spark = (falling.x + 0.3 * falling.y) * saturate(1 - when / 0.1) * saturate(cellN.y * 2 + 0.3);
  }
  float night = dark * lerp(0.35, 1, dark);
  float3 glow = tint * (lines * ((0.12 + 0.9 * rim) * pulse * night + h * 2.2 * frameInfo.w + flare * 2.5) * on + lines * weak * weak * weak * 0.7 * on * tint +
                        (1 - lines) * ((h * 0.55 * frameInfo.w + flare * 0.9) * on ) * tint + spark * 4 * (tint * 0.6 + 0.4));
  glow *= 1 - 0.5 * nDotV * nDotV;  // (less where seen face on, where the units inside are looked at)
  // the world's light on it
  float shadow = SunShadow(i.world, N) * CloudShadow(i.world);
  float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 R = reflect(-v, N);
  // (the computed sky, which follows the hour; the map's own cube is a picture of its day, bright at night too)
  float3 sky = skyZenith.w > 0.5 ? ComputedSky(R) : ToLinear(environmentSky.SampleLevel(samplerLinear, R, 1).rgb) * modern.y;
  float3 H = normalize(v + sunDirection);
  float nDotH = saturate(dot(N, H)), nDotL = saturate(dot(N, sunDirection));
  float glintF = 0.04 + 0.96 * pow(1 - saturate(dot(v, H)), 5);
  float3 glint = sun * shadow * nDotL * glintF * (pow(nDotH, 4000) * 90 + pow(nDotH, 160) * 0.08);
  float3 small = SmallLights(i.world, N, v, tint * (0.02 + 0.2 * lines), float3(0.04, 0.04, 0.04), 0.0016, 0.1, nDotV + 1e-4);
  // (its lines a little lit, as a thing: the sunlit side brighter than the shaded)
  float3 lit = lines * tint * (sun * nDotL * shadow * 0.06 + SkyAmbient(N) * 0.08);
  // (its faction's colour toward its rim, as the plain bubble's: the shield reads as its side's from afar)
  float edge = pow(1 - nDotV, 2.5);
  float alpha = saturate(0.012 + fresnel * 0.45 + edge * 0.22 + lines * (0.08 + 0.25 * rim) + (h * 0.12 + flare * 0.15) * on + spark * 0.3) *
                lerp(0.6, 1, dark);
  float3 color = sky * fresnel + glint + small + lit + glow + tint * ((0.03 + 0.9 * edge) * night * (0.06 + 0.22 * edge) / 0.28);
  // (with its texture's pattern: all of it stutters below two fifths, as the plain bubble's)
  float stutter = info.w > 0.5 ? 0 : saturate((CloudNoise(float3(time * 6, info.z, 0.5)) - 0.55) * 5) * saturate((0.4 - health) * 5);
  color *= 1 - 0.6 * stutter;
  // (seen through the air: what of it comes through)
  float3 through = ThroughAirTo(float3(1, 1, 1), i.world) - ThroughAirTo(float3(0, 0, 0), i.world);
  return float4(color * through, alpha * dot(through, 1.0 / 3));
}

// The picture behind the shell nearest the eye, bent, and lit by the bubbles it lies in.
float4 InsidePS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float3 n = normalize(i.world - sphere.xyz);
  float3 v = normalize(cameraPosition - i.world);
  // (from outside, its near side only; from inside, all of it: a ray meets it once)
  if (info.y < 0.5 && dot(n, v) < 0) discard;
  // where in the pixel the scene lies behind the shell (a nearer thing has the larger depth)
  int2 pixel = int2(i.position.xy);
  coverage = 0;
  for (int sm = 0; sm < SAMPLES; ++sm)
    if (SCENE_DEPTH(pixel, sm) <= i.position.z) coverage |= 1u << sm;
  if (coverage == 0) discard;
  float facing = abs(dot(n, v));
  // (the bending: none face on, most a little inside the rim, falling to none at the very edge, where the picture
  // outside the bubble goes on unbent)
  float rim = (1 - facing) * (1 - facing) * saturate(facing * 6);
  float h, flare;
  float3 lean;
  Ripples(n, h, lean, flare);
  // a slow shimmer toward the rim: a drifting noise that leans the shell a little (none face on, where the units
  // inside are looked at)
  float3 q = n * 2.5 + float3(0.11, 0.07, -0.05) * time;
  float3 shimmer = float3(CloudNoise(q), CloudNoise(q + 19.7), CloudNoise(q + 37.3)) - 0.5;
  shimmer -= n * dot(shimmer, n);
  // what is behind is taken from toward the bubble's middle (it is drawn out toward the rim, as through a lens), and
  // along each ring as it passes; in world units at the shell, as a share of the bubble's size
  float3 across = n - v * dot(n, v);
  float3 moved = (-across * (0.1 * rim) + shimmer * (0.03 * rim)) * frameInfo.y + lean * (0.05 * frameInfo.w);
  float4 to = mul(viewProj, float4(i.world + moved * sphere.w, 1));
  float2 straight = i.position.xy * viewport.zw;
  float2 uv = to.w > 1e-4 ? float2(to.x / to.w * 0.5 + 0.5, 0.5 - to.y / to.w * 0.5) : straight;
  // (never what stands in front of the shell, nor from outside the picture)
  int2 at = min(int2(saturate(uv) * viewport.xy), int2(viewport.xy) - 1);
  float depth = SCENE_DEPTH(at, 0);
  if (any(uv < 0) || any(uv > 1) || depth > i.position.z) {
    uv = straight;
    depth = SCENE_DEPTH(pixel, 0);
  }
  // (read at a pixel's middle: read between pixels, what is inside would be blurred wherever it is moved by a fraction
  // of a pixel; this way it is moved by whole pixels and stays as sharp as it was)
  float3 seen = behind.Load(int3(min(int2(uv * viewport.xy), int2(viewport.xy) - 1), 0)).rgb;
  // the bubbles' light on what is inside them: faint in the middle, stronger toward the shell, ending at it
  float3 light = 0;  // (the tints of the bubbles the place lies in, each by how strongly)
  float strength = 0;
  [branch] if (depth > 0 && frameInfo.z > 0) {
    float4 p = mul(fromScreen, float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1));
    float3 P = p.xyz / p.w;
    int count = (int)(frameInfo.x + 0.5);
    for (int b = 0; b < count; ++b) {
      float4 ball = lit[b * 2], t = lit[b * 2 + 1];
      float r = length(P - ball.xyz) / ball.w;
      if (r >= 1) continue;
      float toward = smoothstep(0.5, 1.0, r);
      float w = t.w * (0.05 + 0.95 * toward * toward) * (1 - smoothstep(0.94, 1.0, r));
      light += t.rgb * w;
      strength += w;
    }
  }
  // (its colour over what is there, keeping that's light and dark, and a little brighter: as a light it would only
  // brighten in its colour what is lit already, and the ground's own colours turn a blue light green; by night, when
  // little is lit, it adds a little of its own)
  const float3 luma = float3(0.2126, 0.7152, 0.0722);
  float amount = saturate(strength * 0.22 * frameInfo.z);
  float3 hue = light / max(dot(light, luma), 1e-4);
  float night = skyZenith.w > 0.5 ? skyHorizon.w : 0;
  // [litshell] where the shell meets the ground and what stands in it, a line of its light
  float3 contact = 0;
  [branch] if (shellLook.z > 0 && abs(info.w) > 0.5 && depth > 0) {
    float4 q = mul(fromScreen, float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1));
    float off = abs(length(q.xyz / q.w - sphere.xyz) - sphere.w);
    float dim = falling.w > 0 ? falling.w : 1;
    contact = tintHealth.rgb * (exp(-off / (0.15 + 0.008 * sphere.w)) * shellLook.z * 0.5 * dim);
  }
  return float4(lerp(seen, dot(seen, luma) * hue, amount) * (1 + 0.3 * amount) + light * (0.01 * night * frameInfo.z) + contact, 1);
}

// The shell itself, both sides, laid over by its alpha (premultiplied: its rings and flares add their light).
float4 ShellPS(VOut i) : SV_Target {
  float3 n = normalize(i.world - sphere.xyz);
  float3 v = normalize(cameraPosition - i.world);
  [branch] if (abs(info.w) > 0.5) return LitShell(i, n, v);  // [litshell]
  float rim = pow(1 - abs(dot(n, v)), 2.5);
  float pattern = saturate(albedoMap.Sample(samplerLinear, i.uv * 3 + float2(0.01, 0.03) * time).r *
                           albedoMap.Sample(samplerLinear, i.uv * 7 - float2(0.02, 0.013) * time).g * 2.5);
  // (by night much less of its own light, and less of it in the way, as the plain bubble)
  float dark = falling.w > 0 ? falling.w : 1;
  float3 tint = tintHealth.rgb * (dark * lerp(0.35, 1, dark));
  float alpha = saturate(0.05 + 0.45 * rim + 0.18 * pattern) * lerp(0.55, 1, dark);
  float3 plain = tint * (0.8 + 2.0 * rim + 1.2 * pattern);
  float h, flare;
  float3 lean;
  Ripples(n, h, lean, flare);
  // weakening: its pattern brighter the less is left; below two fifths it stutters, dimming now and then
  float health = saturate(tintHealth.w);
  float weak = 1 - health;
  float stutter = saturate((CloudNoise(float3(time * 6, info.z, 0.5)) - 0.55) * 5) * saturate((0.4 - health) * 5);
  // (a ring and a flare light the pattern's lines most, in the tint made deeper: a white band would read as mist)
  float3 deep = tint * tintHealth.rgb;
  float3 glow = deep * (h * (1.8 * frameInfo.w) * (0.5 + 3.0 * pattern) + flare * (0.15 + 4.0 * pattern) +
                        pattern * (1.2 * weak * weak * weak));
  // (half as much of it where the shell is seen face on: that is where the units inside are looked at)
  float faceOn = abs(dot(n, v));
  glow *= 1 - 0.5 * faceOn * faceOn;
  return float4((plain * alpha + glow) * (1 - 0.6 * stutter), alpha);
}
)hlsl";

}  // namespace

// [litshell] An icosahedron's twenty faces (their corners on the unit sphere, each face's turned outward), for the shader.
std::string IcosahedronBlock() {
  const float g = 1.6180339887f;
  std::vector<Vec3> v;
  for (float a : {-1.0f, 1.0f})
    for (float b : {-g, g}) v.insert(v.end(), {Vec3{0, a, b}, Vec3{a, b, 0}, Vec3{b, 0, a}});
  std::string A, B, C;
  int faces = 0;
  for (size_t i = 0; i < v.size(); ++i)
    for (size_t j = i + 1; j < v.size(); ++j)
      for (size_t k = j + 1; k < v.size(); ++k) {
        auto edge = [](Vec3 p, Vec3 q) { return std::fabs(Length(p - q) - 2) < 1e-3f; };
        if (!edge(v[i], v[j]) || !edge(v[j], v[k]) || !edge(v[i], v[k])) continue;
        Vec3 a = Normalize(v[i]), b = Normalize(v[j]), c = Normalize(v[k]);
        if (Dot(Cross(b - a, c - a), a + b + c) < 0) std::swap(b, c);
        auto put = [](std::string* s, Vec3 p) {
          char text[96];
          std::snprintf(text, sizeof text, "%sfloat3(%.7f, %.7f, %.7f)", s->empty() ? "" : ", ", p.x, p.y, p.z);
          *s += text;
        };
        put(&A, a), put(&B, b), put(&C, c);
        ++faces;
      }
  if (faces != 20) std::fprintf(stderr, "render: energy shields: the icosahedron has %d faces\n", faces);
  return "static const float3 kIcoA[20] = {" + A + "};\nstatic const float3 kIcoB[20] = {" + B + "};\nstatic const float3 kIcoC[20] = {" + C + "};\n";
}

bool EnergyShields::Init() {
  std::string error;
  const std::string source = "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + kLightingShaderBlock + IcosahedronBlock() + kShader;
  gfx::PipelineDesc pd;
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
              {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)}};
  pd.stride = sizeof(ScmVertex);
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.cull = gfx::Cull::None;  // (the shader picks the side: the dome's winding is not relied on)
  pd.depthWrite = false;
  pd.alphaWrite = false;  // (the scene's alpha is the original look's glow; this is the modern look's)
  pd.name = "energy shields, what is inside";
  pd.pixelEntry = "InsidePS";
  pd.depthTest = false;  // (the shader tests the scene's depth itself, which it reads)
  pd.depthTarget = false;
  inside_ = device_->CreatePipeline(pd, &error);
  if (inside_) {
    pd.name = "energy shields, the shell";
    pd.pixelEntry = "ShellPS";
    pd.depthTest = true;
    pd.depthTarget = true;
    pd.blend = gfx::Blend::Premultiplied;
    shell_ = device_->CreatePipeline(pd, &error);
  }
  if (!inside_ || !shell_) {
    fprintf(stderr, "render: energy shields cannot be drawn (%s); the plain bubbles stay\n", error.c_str());
    return false;
  }
  return true;
}

bool EnergyShields::Prepare() {
  if (state_ == 0) state_ = Init() ? 1 : -1;
  return state_ > 0;
}

void EnergyShields::Begin() {
  bubbles_.clear();
  hits_.clear();
  owners_.clear();
}

void EnergyShields::AddBubble(const Model* model, const Mat4& world, bool hex) {
  if (!model || !model->ok || model->lods.empty()) return;
  Bubble b{};
  b.model = model;
  b.hex = hex;
  b.world = world;
  b.origin = {world.m[0][3], world.m[1][3], world.m[2][3]};
  // its sphere: the middle of the dome's bounds, half its width across (a dome cut off below is as wide)
  const Vec3 middle = (model->boundsMin + model->boundsMax) * 0.5f;
  const Vec4 c = world * Vec4{middle.x, middle.y, middle.z, 1};
  b.centre = {c.x, c.y, c.z};
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  b.radius = std::max(model->boundsMax.x - model->boundsMin.x, model->boundsMax.z - model->boundsMin.z) * 0.5f * scale;
  if (b.radius <= 0) return;
  bubbles_.push_back(b);
}

void EnergyShields::AddHit(Vec3 at, Vec3 toward, float age) {
  if (age < 0) return;
  hits_.push_back({at, toward, age});
}

void EnergyShields::AddOwner(Vec3 position, float shieldRatio) { owners_.push_back({position, shieldRatio}); }

void EnergyShields::Draw(const View& view, Post* post) {
  drawn = 0;
  if (bubbles_.empty() || !post) return;
  if (!Prepare()) return;
  const Vec3 eye{view.constants.cameraPosition[0], view.constants.cameraPosition[1], view.constants.cameraPosition[2]};
  order_.clear();
  for (Bubble& b : bubbles_) {
    const Vec3 r{b.radius, b.radius, b.radius};
    if (!view.BoxVisible(b.centre - r, b.centre + r)) continue;
    b.distance = Length(b.centre - eye);
    order_.push_back(&b);
  }
  if (order_.empty()) return;
  // each hit to the bubble its flash was made at (its centre: the nearest, within three quarters of its radius, since
  // a flash stays where it was made while a mobile shield drives on), the newest kept
  for (const Hit& h : hits_) {
    Bubble* best = nullptr;
    float nearest = 1e30f;
    for (Bubble* b : order_) {
      const float d = Length(b->origin - h.at);
      if (d < std::max(1.0f, b->radius * 0.75f) && d < nearest) nearest = d, best = b;
    }
    if (!best) continue;
    int slot = best->hits;
    if (slot >= kMostHits) {  // (full: in place of the oldest, if this one is newer)
      slot = 0;
      for (int k = 1; k < kMostHits; ++k)
        if (best->hit[k][3] > best->hit[slot][3]) slot = k;
      if (best->hit[slot][3] <= h.age) continue;
    } else {
      ++best->hits;
    }
    const Vec3 d = Normalize(h.toward);
    best->hit[slot][0] = d.x, best->hit[slot][1] = d.y, best->hit[slot][2] = d.z, best->hit[slot][3] = h.age;
  }
  // how much of each shield is left: its unit's, the one standing under its centre
  for (Bubble* b : order_) {
    const float reach = std::max(2.0f, b->radius * 0.15f);
    float nearest = reach;
    for (const Owner& o : owners_) {
      const float across = std::hypot(o.at.x - b->origin.x, o.at.z - b->origin.z);
      if (across < nearest && std::fabs(o.at.y - b->origin.y) < b->radius) {
        nearest = across;
        b->health = std::clamp(o.ratio, 0.0f, 1.0f);
      }
    }
  }
  // the farthest first: each laid over those behind it
  std::sort(order_.begin(), order_.end(), [](const Bubble* a, const Bubble* b) { return a->distance > b->distance; });

  // the frame's constants: the way back from the picture into the world, and the bubbles whose light falls inside
  // them (the nearest kMostLit)
  float frame[16 + 4 + kMostLit * 8 + 4] = {};
  const Mat4 back = Inverse(view.constants.viewProj);
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) frame[r * 4 + k] = back.m[r][k];
  int listed = 0;
  for (size_t k = order_.size(); k-- > 0 && listed < kMostLit;) {
    const Bubble& b = *order_[k];
    const Vec3 tint = b.model->lods[0].shieldTint;
    const float health = b.health >= 0 ? b.health : std::clamp(healthUnknown, 0.0f, 1.0f);
    float* o = frame + 20 + listed * 8;
    o[0] = b.centre.x, o[1] = b.centre.y, o[2] = b.centre.z, o[3] = b.radius;
    o[4] = tint.x, o[5] = tint.y, o[6] = tint.z, o[7] = 0.5f + 0.5f * health;  // (a weakened shield throws less)
    ++listed;
  }
  frame[16] = static_cast<float>(listed);
  frame[17] = std::max(bend, 0.0f);
  frame[18] = std::max(light, 0.0f);
  frame[19] = std::max(rings, 0.0f);
  float* look = frame + 20 + kMostLit * 8;  // [litshell]
  look[0] = litShell ? 1.0f : 0.0f, look[1] = std::max(hexLines, 0.0f), look[2] = std::max(contactGlow, 0.0f);

  auto bubbleConstants = [&](const Bubble& b, float* out) {
    for (int k = 0; k < 4; ++k) {
      out[k] = b.world.m[0][k];
      out[4 + k] = b.world.m[1][k];
      out[8 + k] = b.world.m[2][k];
    }
    out[12] = b.centre.x, out[13] = b.centre.y, out[14] = b.centre.z, out[15] = b.radius;
    const Vec3 tint = b.model->lods[0].shieldTint;
    out[16] = tint.x, out[17] = tint.y, out[18] = tint.z;
    out[19] = b.health >= 0 ? b.health : std::clamp(healthUnknown, 0.0f, 1.0f);
    out[20] = static_cast<float>(b.hits);
    out[21] = Length(eye - b.centre) < b.radius ? 1.0f : 0.0f;
    out[22] = std::fmod(std::fabs(b.origin.x * 0.37f + b.origin.z * 0.61f), 97.0f);
    // [litshell] (the UEF's, for now: Chris asked for it; the others keep their own look) its hex's cells hexSize apart: an
    // icosahedron's edge is 1.05 of its sphere's radius
    out[23] = !litShell || !b.hex ? 0.0f : hexSize > 0 ? std::clamp(std::round(1.0515f * b.radius / hexSize), 2.0f, 200.0f) : -1.0f;
    for (int k = 0; k < kMostHits; ++k)
      for (int j = 0; j < 4; ++j) out[24 + k * 4 + j] = k < b.hits ? b.hit[k][j] : 0.0f;
  };
  auto drawDome = [&](const Bubble& b) {
    const ModelLod& lod = b.model->lods[0];
    float constants[24 + kMostHits * 4];
    bubbleConstants(b, constants);
    device_->SetConstants(2, constants, sizeof constants);
    device_->SetTexture(0, lod.albedo);
    device_->SetVertexBuffer(lod.vertices);
    device_->SetIndexBuffer(lod.indices);
    device_->DrawIndexed(lod.indexCount);
  };

  // what is inside and behind each, bent and lit: from a copy of the picture as it is now (where none can be taken,
  // the shells alone)
  const bool inside = (bend > 0 || light > 0) && post->CopyBehind();
  if (inside) {
    post->BeginColorOnly();
    device_->SetPipeline(inside_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, frame, sizeof frame);
    device_->SetTexture(1, post->Behind());
    device_->SetTexture(2, post->Depth());
    for (const Bubble* b : order_) drawDome(*b);
    device_->SetTexture(1, {});
    device_->SetTexture(2, {});
  }
  // then the shells over it, tested against the scene's depth
  post->ContinueScene();
  device_->SetPipeline(shell_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, frame, sizeof frame);
  for (const Bubble* b : order_) drawDome(*b);
  drawn = static_cast<int>(order_.size());
}

void EnergyShieldsTestFrame(int count, Vec3 around, double clock, float every, const std::function<float(float, float)>& ground,
                            Frame* frame, std::vector<std::string>* names) {
  // (the four T2 shield generators, as the game's blueprints have their shields: Defense.Shield's Mesh, ImpactMesh,
  // ShieldSize, ShieldVerticalOffset)
  struct Kind {
    const char* unit;
    const char* mesh;
    const char* flash;
    float size, up;
  };
  static const Kind kKinds[4] = {
      {"ueb4202", "/effects/entities/Shield01/Shield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 26, -3},
      {"uab4202", "/effects/entities/AeonShield01/AeonShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 20, -3},
      {"urb4202", "/effects/entities/CybranShield01/CybranShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01b_mesh", 18, -3},
      {"xsb4202", "/effects/entities/SeraphimShield01/SeraphimShield01_mesh", "/effects/entities/ShieldSection01/ShieldSection01_mesh", 28, -3}};
  static const float kRatios[4] = {1.0f, 0.75f, 0.5f, 0.25f};
  if (names->empty()) {  // (0: none; then each kind's unit, bubble and flash)
    names->push_back(std::string());
    for (const Kind& k : kKinds) names->insert(names->end(), {k.unit, k.mesh, k.flash});
  }
  frame->blueprints = names;
  frame->units.clear();
  frame->entities.clear();
  auto hash = [](uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 13;
    return static_cast<float>(h >> 8) / 16777216.0f;
  };
  every = std::max(every, 0.05f);
  const int across = (count + 1) / 2;
  for (int b = 0; b < count; ++b) {
    const Kind& kind = kKinds[b % 4];
    const int k = static_cast<int>(&kind - kKinds);
    // two rows, 22 apart: neighbours' bubbles overlap, as round a base
    const float x = around.x + (static_cast<float>(b % across) - (across - 1) * 0.5f) * 22, z = around.z + (b < across ? -11.0f : 11.0f);
    const Vec3 foot{x, ground(x, z), z};
    FrameUnit u;
    u.id = static_cast<uint32_t>(b + 1);
    u.blueprint = static_cast<uint32_t>(1 + k * 3);
    u.position[0] = foot.x, u.position[1] = foot.y, u.position[2] = foot.z;
    u.health = u.maxHealth = 1000;
    u.layer = 1;
    u.hasShield = true;
    u.shieldRatio = kRatios[(b / 4 + b) % 4];
    frame->units.push_back(u);
    const Vec3 centre = foot + Vec3{0, kind.up, 0};
    FrameEntity e;
    e.id = 0x20000000u + static_cast<uint32_t>(b);
    e.mesh = static_cast<uint32_t>(2 + k * 3);
    e.army = 0;
    e.position[0] = centre.x, e.position[1] = centre.y, e.position[2] = centre.z;
    e.scale[0] = e.scale[1] = e.scale[2] = kind.size;
    e.shield = true;
    frame->entities.push_back(e);
    // its hits: one every `every` seconds from its own start, each a flash for two seconds
    const double phase = hash(static_cast<uint32_t>(b), 7) * every;
    const long last = static_cast<long>(std::floor((clock - phase) / every));
    for (long n = std::max(0L, last - static_cast<long>(2 / every)); n <= last; ++n) {
      const double at = phase + static_cast<double>(n) * every;
      if (at > clock || clock - at >= 2) continue;
      const float heading = hash(static_cast<uint32_t>(b), static_cast<uint32_t>(n) * 2 + 1) * 6.2831853f;
      const float rise = 0.15f + hash(static_cast<uint32_t>(b), static_cast<uint32_t>(n) * 2 + 2) * 1.1f;
      const Vec3 d{std::sin(heading) * std::cos(rise), std::sin(rise), std::cos(heading) * std::cos(rise)};
      // (turned toward the hit: heading about y, then pitch about x, so that its +z is d)
      const float pitch = -std::asin(std::clamp(d.y, -1.0f, 1.0f));
      const Quat turn = Mul(Quat{0, std::sin(heading * 0.5f), 0, std::cos(heading * 0.5f)}, Quat{std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)});
      FrameEntity f;
      f.id = 0x30000000u + static_cast<uint32_t>(b) * 65536u + static_cast<uint32_t>(n & 0xffff);
      f.mesh = static_cast<uint32_t>(3 + k * 3);
      f.army = 0;
      f.position[0] = centre.x, f.position[1] = centre.y, f.position[2] = centre.z;
      f.orientation[0] = turn.x, f.orientation[1] = turn.y, f.orientation[2] = turn.z, f.orientation[3] = turn.w;
      f.scale[0] = f.scale[1] = f.scale[2] = kind.size;
      frame->entities.push_back(f);
    }
  }
}

}  // namespace render
