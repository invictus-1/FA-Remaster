// [grime] (unit_grime.h says what it does)
#include "unit_grime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "remasters.h"
#include "textures.h"

namespace render {

namespace {

// The sets in the array's order, two slices each (albedo, normal).
constexpr const char* kGrimeSets[5] = {"dust", "mud", "soot", "frost", "wetmud"};
constexpr int kStandIn = 256;  // the stand-ins' size (a side)

uint32_t Mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
// A number 0..1 at a corner of a lattice that repeats every `px` by `py` corners.
float Corner(int x, int y, int px, int py, uint32_t seed) {
  x = ((x % px) + px) % px;
  y = ((y % py) + py) % py;
  return static_cast<float>(Mix(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed * 83492791u) >> 8) / 16777216.0f;
}
// Smooth noise over the tile (u, v in 0..1), repeating at its edges: `px` by `py` cells across it.
float Value(float u, float v, int px, int py, uint32_t seed) {
  const float x = u * static_cast<float>(px), y = v * static_cast<float>(py);
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const float a = Corner(x0, y0, px, py, seed), b = Corner(x0 + 1, y0, px, py, seed);
  const float c = Corner(x0, y0 + 1, px, py, seed), d = Corner(x0 + 1, y0 + 1, px, py, seed);
  return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
// Several of them, each twice as fine and half as strong (0..1).
float Fbm(float u, float v, int px, int py, int octaves, uint32_t seed) {
  float sum = 0, weight = 0, w = 1;
  for (int o = 0; o < octaves; ++o) {
    sum += Value(u, v, px << o, py << o, seed + static_cast<uint32_t>(o) * 101u) * w;
    weight += w;
    w *= 0.5f;
  }
  return sum / weight;
}
// Cells round points scattered one in each of n x n squares (repeating): how far the nearest lies and the second nearest
// (in squares).
void Cells(float u, float v, int n, uint32_t seed, float* first, float* second) {
  const float x = u * static_cast<float>(n), y = v * static_cast<float>(n);
  const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
  float a = 9, b = 9;
  for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
      const float px = static_cast<float>(cx + i) + 0.15f + 0.7f * Corner(cx + i, cy + j, n, n, seed);
      const float py = static_cast<float>(cy + j) + 0.15f + 0.7f * Corner(cx + i, cy + j, n, n, seed + 7u);
      const float d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
      if (d < a) {
        b = a;
        a = d;
      } else if (d < b) {
        b = d;
      }
    }
  *first = a;
  *second = b;
}
float Smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
uint8_t Byte(float x) { return static_cast<uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255 + 0.5f); }

// One set drawn: its colour (as painted, sRGB), height and roughness at each texel of the tile.
void DrawSet(int set, std::vector<float>* rgbhr) {
  const int n = kStandIn;
  rgbhr->assign(static_cast<size_t>(n) * n * 5, 0);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / n, v = (static_cast<float>(y) + 0.5f) / n;
      float r = 0, g = 0, b = 0, h = 0, rough = 0.9f;
      if (set == 0) {  // dust: a fine pale coat, grainy, in drifts
        const float drift = Fbm(u, v, 4, 4, 4, 11), grain = Value(u, v, 64, 64, 12);
        h = std::clamp(drift * 0.8f + grain * 0.35f - 0.1f, 0.0f, 1.0f);
        const float t = 0.85f + 0.3f * drift;
        r = 0.46f * t, g = 0.40f * t, b = 0.31f * t;
        rough = 0.9f + 0.06f * grain;
      } else if (set == 1) {  // caked mud: plates of dried earth with cracks between them
        float a, s;
        Cells(u, v, 6, 21, &a, &s);
        const float crack = 1 - Smooth(0.0f, 0.07f, s - a);
        const float lump = Fbm(u, v, 6, 6, 4, 22);
        h = std::clamp(0.45f + 0.45f * lump - 0.55f * crack, 0.0f, 1.0f);
        const float t = (0.8f + 0.35f * lump) * (1 - 0.3f * crack);
        r = 0.37f * t, g = 0.29f * t, b = 0.21f * t;
        rough = 0.86f + 0.1f * crack;
      } else if (set == 2) {  // soot: dark, streaked downward (along v) and in blots
        const float streak = Fbm(u, v, 16, 3, 3, 31), blot = Fbm(u, v, 5, 5, 3, 32);
        h = std::clamp(blot * 0.7f + streak * 0.5f - 0.15f, 0.0f, 1.0f);
        const float t = 0.75f + 0.45f * streak;
        r = 0.080f * t, g = 0.072f * t, b = 0.066f * t;
        rough = 0.93f;
      } else if (set == 3) {  // frost: white-blue crystals, finer and sparkling where thin
        const float crystals = Fbm(u, v, 24, 24, 3, 41), sheet = Fbm(u, v, 5, 5, 4, 42);
        float a, s;
        Cells(u, v, 40, 43, &a, &s);
        h = std::clamp(sheet * 0.6f + crystals * 0.4f + (1 - Smooth(0.0f, 0.5f, a)) * 0.2f, 0.0f, 1.0f);
        const float t = 0.88f + 0.12f * crystals;
        r = 0.80f * t, g = 0.85f * t, b = 0.92f * t;
        rough = 0.32f + 0.35f * (1 - crystals);
      } else {  // wet mud: dark, smooth, glossy, in soft lumps
        const float lump = Fbm(u, v, 5, 5, 4, 51), fine = Value(u, v, 40, 40, 52);
        h = std::clamp(lump * 0.9f + fine * 0.15f, 0.0f, 1.0f);
        const float t = 0.85f + 0.3f * lump;
        r = 0.22f * t, g = 0.16f * t, b = 0.105f * t;
        rough = 0.16f + 0.18f * fine;
      }
      float* o = rgbhr->data() + (static_cast<size_t>(y) * n + x) * 5;
      o[0] = r, o[1] = g, o[2] = b, o[3] = h, o[4] = rough;
    }
}

}  // namespace

gfx::Texture MakeGrimeLayers(gfx::Device* device, TextureCache* textures) {
  // the visuals session's sets, where all ten files are there
  std::vector<std::string> paths;
  for (const char* set : kGrimeSets) {
    paths.push_back(std::string(kRemasteredUnitGrime) + "/" + set + "_albedo.dds");
    paths.push_back(std::string(kRemasteredUnitGrime) + "/" + set + "_normal.dds");
  }
  std::vector<bool> present;
  gfx::Texture made = textures->Layers(paths, &present);
  const int found = static_cast<int>(std::count(present.begin(), present.end(), true));
  if (made && found == static_cast<int>(paths.size())) {
    printf("render: unit grime: the shared layers from %s\n", kRemasteredUnitGrime);
    return made;
  }
  if (found > 0) printf("render: unit grime: %d of the %zu layers' files in %s: stand-ins until all are there\n", found, paths.size(), kRemasteredUnitGrime);
  // stand-ins: each set's colour and height, its normal from its height, with all their smaller sizes
  const int n = kStandIn;
  int levels = 1;
  while ((n >> levels) >= 1) ++levels;
  std::vector<uint8_t> data;
  for (int set = 0; set < 5; ++set) {
    std::vector<float> s;
    DrawSet(set, &s);
    const float relief = set == 1 ? 3.0f : set == 3 ? 3.0f : set == 2 ? 1.0f : 2.0f;  // (how steep its height's slopes)
    for (int slice = 0; slice < 2; ++slice) {
      std::vector<float> level(static_cast<size_t>(n) * n * 4);
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
          const float* t = s.data() + (static_cast<size_t>(y) * n + x) * 5;
          float* o = level.data() + (static_cast<size_t>(y) * n + x) * 4;
          if (slice == 0) {
            o[0] = t[0], o[1] = t[1], o[2] = t[2], o[3] = t[3];
          } else {
            auto at = [&](int i, int j) { return s[(static_cast<size_t>((j + n) % n) * n + static_cast<size_t>((i + n) % n)) * 5 + 3]; };
            // (the units' layout: green the slope along v, alpha along u, as MeshRenderer reads a unit's normal map)
            const float du = (at(x + 1, y) - at(x - 1, y)) * relief, dv = (at(x, y + 1) - at(x, y - 1)) * relief;
            const float len = std::sqrt(du * du + dv * dv + 1);
            o[0] = t[4];
            o[1] = -dv / len * 0.5f + 0.5f;
            o[2] = 1;
            o[3] = -du / len * 0.5f + 0.5f;
          }
        }
      // its sizes, each a box filter of the one before
      int w = n;
      for (int l = 0; l < levels; ++l) {
        for (float f : level) data.push_back(Byte(f));
        if (w == 1) break;
        const int h = w / 2;
        std::vector<float> next(static_cast<size_t>(h) * h * 4);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < h; ++x)
            for (int c = 0; c < 4; ++c)
              next[(static_cast<size_t>(y) * h + x) * 4 + c] =
                  0.25f * (level[(static_cast<size_t>(2 * y) * w + 2 * x) * 4 + c] + level[(static_cast<size_t>(2 * y) * w + 2 * x + 1) * 4 + c] +
                           level[(static_cast<size_t>(2 * y + 1) * w + 2 * x) * 4 + c] + level[(static_cast<size_t>(2 * y + 1) * w + 2 * x + 1) * 4 + c]);
        level.swap(next);
        w = h;
      }
    }
  }
  gfx::TextureDesc d;
  d.width = d.height = n;
  d.mips = levels;
  d.layers = 10;
  d.format = gfx::Format::RGBA8;
  d.data = data.data();
  d.dataSize = data.size();
  made = device->CreateTexture(d);
  printf("render: unit grime: stand-in layers made (%d a side, %zu KB)\n", n, data.size() / 1024);
  return made;
}

const char kGrimeShaderBlock[] = R"hlsl(
Texture2D grimeDamageAlbedo : register(t22);  // [grime] a unit's damage set (unit_grime.h): its colour as painted, alpha how hot,
Texture2D grimeDamageNormal : register(t23);  // and its normal (x in alpha, y in green)
Texture2DArray grimeLayers : register(t24);   // the shared tiling layers: per set its albedo and height, its normal and roughness
static bool grimeTaken = false;  // grime laid here by GrimeSurface: WearSurface leaves its own patches of it out
static float grimeSplash = -1;   // the masks' A (where weather dirt sits): DirtSurface's fresh mud by it; -1: no masks

// Which of its art a model's level has (kind5.z: its level, and a quarter for the masks, a half for the damage set).
bool GrimeHasMasks(float flags) { return frac(frac(flags) * 2) > 0.25; }
bool GrimeHasDamage(float flags) { return frac(flags) > 0.4; }

// How much of a layer lies here: `amount` how much the unit has gathered (0..1), `spot` where it gathers first (0..1), `height`
// the layer's own (its albedo's alpha: where it lies thick shows first, breaking its edge up).
float GrimeCover(float amount, float spot, float height) {
  float d = spot + (height - 0.5) * 0.35 - (1 - amount) * 1.05;
  return smoothstep(0, 0.16, d) * (0.55 + 0.4 * amount);
}

// A set laid over the surface where it covers (`strength`: at most how much of it): its colour (times `shade`), its roughness (`rough` from afar; its own close by,
// with its relief over the surface's). `grad`: the unit's UVs' slopes across the screen. Returns how much of it lies here.
float GrimeLay(float2 uv, float4 grad, float set, float scale, float amount, float spot, float strength, float near, float3 shade, float rough,
               float3 T, float3 Bn, float3 vN, inout float3 base, inout float metallic, inout float roughness, inout float3 N) {
  float4 a = grimeLayers.SampleGrad(samplerAniso, float3(uv * scale, set * 2), grad.xy * scale, grad.zw * scale);
  float cover = GrimeCover(amount, spot, a.a) * strength;
  [branch] if (cover > 0.002) {
    base = lerp(base, ToLinear(a.rgb) * shade, cover);
    metallic *= 1 - cover;
    [branch] if (near > 0.01) {
      float4 b = grimeLayers.SampleGrad(samplerAniso, float3(uv * scale, set * 2 + 1), grad.xy * scale, grad.zw * scale);
      float2 g = b.ga * 2 - 1;
      float3 own = normalize(g.x * Bn + g.y * T + sqrt(saturate(1 - dot(g, g))) * vN);
      N = normalize(N + (own - vN) * (cover * near));
      rough = lerp(rough, b.r, near);
    }
    roughness = lerp(roughness, rough, cover);
  }
  return cover;
}

// A unit's battle grime, weather dirt and visible damage (unit_grime.h; its three rows from `at`) on its surface, before it
// is lit. `uv`, `grad`: its UVs and their slopes across the screen; `mask`: its secondary texture's reading (the masks where
// `flags`, its level's kind5.z, says it has them); `occlusion`: its crease shading; `p`, `n`: the place on the model at rest
// and its surface's way there (world units, its own frame); `world`: the place in the world; `teamMask`, `tint`: how much
// is team colour and the army's colour (as stored); `glowDim`: what glowing is multiplied by under the hour's light; `T`,
// `Bn`, `vN`: the surface's tangent, binormal and its own normal (the damage's and the layers' relief on them).
void GrimeSurface(StructuredBuffer<float4> rows, uint at, float2 uv, float4 grad, float4 mask, float flags, float occlusion, float3 p, float3 n,
                  float3 world, float teamMask, float3 tint, float glowDim, float3 T, float3 Bn, float3 vN, inout float3 base, inout float metallic,
                  inout float roughness, inout float3 emission, inout float3 N) {
  float4 g0 = rows[at], g1 = rows[at + 1], g2 = rows[at + 2];
  float high = saturate((p.y - g2.x) * g2.y);   // 0 its lowest point .. 1 its top
  float seed = g2.w * 37.3;
  float pixel = max(length(fwidth(p)), 1e-4);   // world units a pixel spans here
  float near = saturate(2.5 - pixel * 80);      // (the layers' relief and own roughness close by only)
  grad *= exp2(temporal.x);                     // (as Sample's bias: the temporal upscaler's)
  float keep = 1 - 0.35 * teamMask;             // (team colour must still read: grime takes less of it)
  // its masks: the art's, or made up from its height, its facing and its crease shading
  float G, B, A;
  [branch] if (GrimeHasMasks(flags)) {
    G = mask.g;
    B = mask.b;
    A = mask.a;
  } else {
    float lumpy = CloudNoise(p * 2.3 + seed);
    float low = 1 - high;
    G = saturate((low * low * 0.75 + saturate(-n.y) * 0.4 + (1 - occlusion) * 0.8) * (0.4 + 1.2 * lumpy));
    B = saturate((CloudNoise(p * 1.6 + seed * 1.7) * 0.65 + CloudNoise(p * 4.3 + 5.1) * 0.35 - 0.5) * 2.8 + 0.5);
    A = saturate(saturate(1 - high / 0.3) * (0.5 + lumpy) + saturate((n.y - 0.6) * 3) * smoothstep(0.5, 0.7, high) * 0.8);
  }
  float lowA = A * (1 - smoothstep(0.3, 0.45, high));  // (the weather's dirt low down: mud splash)
  float highA = A * smoothstep(0.3, 0.5, high);        // (and up high: frost in sheltered creases)
  // visible damage: shown where the damage order is above its health, over a soft edge; what was lost in the last seconds
  // glows where it is hot; below what it was mended from, patched
  float health = g1.z;
  [branch] if (health >= 0) {
    const float edge = 0.03;
    // (each level spread over -edge .. 1 + edge: whole, none shows; at none left, all of it)
    float at = lerp(-edge, 1 + edge, health), then = lerp(-edge, 1 + edge, g1.w), from = lerp(-edge, 1 + edge, g2.z);
    float show = smoothstep(at - edge, at + edge, B);
    float fresh = saturate(show - smoothstep(then - edge, then + edge, B));  // (shown now, not then)
    float mended = g2.z <= 1 ? smoothstep(from - edge, from + edge, B) * (1 - show) : 0;
    [branch] if (show + mended > 0.002) {
      float3 before = base;
      [branch] if (GrimeHasDamage(flags)) {
        float4 d = grimeDamageAlbedo.SampleGrad(samplerAniso, uv, grad.xy, grad.zw);
        float2 dn = grimeDamageNormal.SampleGrad(samplerAniso, uv, grad.xy, grad.zw).ga * 2 - 1;
        float3 dented = normalize(dn.x * Bn + dn.y * T + sqrt(saturate(1 - dot(dn, dn))) * vN);
        // (no team colour in it: the army's where paint is left, as a remastered unit's panels take it; none where burnt)
        float luma = dot(d.rgb, float3(0.299, 0.587, 0.114));
        float3 paint = lerp(ToLinear(d.rgb), ToLinear(tint * saturate(0.15 + luma * 1.3)), teamMask * saturate(luma * 4 - 0.2));
        base = lerp(base, paint, show);
        roughness = lerp(roughness, max(roughness, 0.6), show);
        metallic *= 1 - 0.5 * show;
        N = normalize(N + (dented - vN) * (show + 0.35 * mended));
        emission += float3(1.0, 0.32, 0.05) * (smoothstep(0.3, 0.9, d.a) * fresh * 1.5 * glowDim);
      } else {
        // (no damage set: scorched and scuffed paint, bare metal where it is torn, dents, hot spots)
        float f1 = CloudNoise(p * 6.0 + seed);
        float f2 = CloudNoise(p * 21.0 + seed * 3.1);
        float burnt = show * saturate(0.35 + (f1 - 0.5) * 1.6);
        base = lerp(base, float3(0.03, 0.026, 0.022), burnt * 0.8 * keep);
        roughness = lerp(roughness, 0.88, burnt * 0.7);
        metallic *= 1 - 0.6 * burnt;
        float bare = show * smoothstep(0.6, 0.66, f2) * saturate(2 - pixel * 40);
        base = lerp(base, float3(0.14, 0.14, 0.145), bare);
        metallic = lerp(metallic, 0.85, bare);
        roughness = lerp(roughness, 0.5, bare);
        emission += float3(1.0, 0.32, 0.05) * (fresh * smoothstep(0.66, 0.74, f2) * 2.5 * glowDim);
        // (the dents tilt the surface, as WearSurface's)
        float height = (f1 - 0.5) * 0.05 * (show + 0.35 * mended);
        float3 dpdx = ddx(world), dpdy = ddy(world);
        float hx = ddx(height), hy = ddy(height);
        float3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
        float det = dot(dpdx, r1);
        float3 bent = abs(det) * N - (det < 0 ? -1 : 1) * (hx * r1 + hy * r2);
        if (dot(bent, bent) > 1e-12) N = normalize(bent);
      }
      // mended: fresh paint over it, a little lighter and matt
      base = lerp(base, before * 1.12 + 0.004, mended * 0.6);
      roughness = lerp(roughness, 0.62, mended * 0.5);
    }
  }
  // battle grime and the weather's dirt, each a shared layer laid where its mask says it gathers first, the more the more
  // the unit has: soot (anywhere, cavities first), dust (cavities and tops), caked mud (low down; dark and glossy when
  // wet), mud splashed by the rain (low down, where weather dirt sits), frost (tops and the sheltered creases up high)
  float dust = g0.x, caked = g0.y, soot = g0.z, frost = g0.w, wet = g1.x, splash = g1.y;
  grimeTaken = dust + caked + soot > 0.002;
  float upward = saturate(n.y);
  [branch] if (soot > 0.002)
    GrimeLay(uv, grad, 2, 7, soot, saturate(0.2 + G * 0.8) * keep, 0.9, near, 1, 0.93, T, Bn, vN, base, metallic, roughness, N);
  [branch] if (dust > 0.002)
    GrimeLay(uv, grad, 0, 8, dust, saturate(G * 0.8 + saturate(n.y - 0.25) * 0.5) * keep, 0.7, near, lerp(1, 0.6, wet), lerp(0.9, 0.6, wet), T, Bn, vN,
             base, metallic, roughness, N);
  [branch] if (caked > 0.002)
    GrimeLay(uv, grad, 1, 10, caked, saturate(G * saturate(1.5 - high * 2.0) + lowA * 0.3), 0.95, near, lerp(1, 0.55, wet), lerp(0.88, 0.3, wet), T, Bn, vN,
             base, metallic, roughness, N);
  [branch] if (splash > 0.002)
    GrimeLay(uv, grad, 4, 8, splash, lowA * lerp(0.5, 1, 1 - upward), 0.95, near, lerp(1.7, 1, wet), lerp(0.85, 0.25, wet), T, Bn, vN, base, metallic,
             roughness, N);
  [branch] if (frost > 0.002)
    GrimeLay(uv, grad, 3, 10, frost * 0.85, saturate(highA * 0.8 + saturate((n.y - 0.6) * 2.5) * 0.7), 0.65, near, 1, 0.5, T, Bn, vN, base, metallic, roughness, N);
}
)hlsl";

}  // namespace render
