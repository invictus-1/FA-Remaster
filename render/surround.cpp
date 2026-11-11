#include "surround.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "beyond_land.h"
#include "json.h"
#include "lighting.h"
#include "png.h"
#include "terrain.h"

namespace render {
namespace {

// The surround's ground: placed by its points, its slopes and shadows from its heights (a texture, the map's own
// inside the map's square), coloured from the map's ground, lit as the ground is, dimmed and lost with the distance as
// all that lies beyond the edge.
const char kShader[] = R"hlsl(
cbuffer Surround : register(b1) {
  float4 surGrid;     // x, y the first sample's place (world x, z); z samples per world unit; w 1 / the samples across
  float4 surLevels;   // x the world units of a height's 1; y the water's level; z the highest ground; w from where the ground counts as high
  float4 surLook;     // x the shadows' softness; y 1 = the map's colours were gathered (palette); z over how far outside the
                      // edge the map's own colours go over into these (0: not at all)
#ifdef SURROUND_DETAIL
  float4 surPictures; // x the masks' samples per world unit, y 1 / its samples across; z, w the forest's (z 0: none)
  float4 surKinds;    // which of the map's layers the masks' red, green, blue and alpha are the shares of
  float4 surMore;     // x the layer of what the four leave of 1; y the layer laid under the forest (-1: none); z how far from
                      // the eye the layers' normals are read; w 1 = the look with eight strata (the map's second mask counts)
  float4 surMap;      // x, y 1 / the map's size; z, w the map's size
  float4 surCanopy;   // rgb the far forest's canopy (as stored); w how much of it is drawn
  float4 surTrees;    // x, y from how far past the edge to how far the scenery's single trees thin out to none (y 0: none
                      // stand); z the map's specular (as the ground's roughness counts it)
  float4 surSight;    // x, y from how far from the eye to how far the scenery's trees are left out (y 0: none are)
  float4 surPictures2; // x the second masks' samples per world unit, y 1 / its samples across (x 0: none)
  float4 surKinds2;   // which of the map's layers the second masks' red, green, blue and alpha are the shares of (-1: none)
  float4 albedoTile[10];  // as the ground's (terrain.cpp): x a layer's repeats per world unit; y 1 = it has its third map
  float4 normalTile[9];   // x its normal's repeats per world unit; y 1 = +y is up in its texture
  float4 upperOf[9];      // the map's upper layer where each layer shows, on the whole (rgb as stored, a its share)
#endif
};
Texture2D<float> surHeights : register(t0);  // its heights, the north row first, with smaller levels
#ifdef SURROUND_DETAIL
// (the detail look: the map's layers where the ground's own shader has them, Terrain::BindLayers; t26 and up are the
// lighting's)
Texture2D surMasks : register(t1);           // which layer lies where: four layers' shares (surKinds), the rest a fifth's
Texture2D maskLow : register(t2);            // the map's own masks (read on past its edge, mirrored)
Texture2D maskHigh : register(t3);
Texture2D<float> surForest : register(t4);   // 1 closed forest
Texture2D surMasks2 : register(t5);          // more layers' shares (surKinds2), where the surround brings them
Texture2D albedoMaps[10] : register(t6);
Texture2D normalMaps[9] : register(t16);
Texture2DArray materialMaps : register(t25);  // per layer: r creases' shading, g roughness, b height
#else
Texture2D palette : register(t1);            // 4 x 1: the map's ground low by the water, level, high, steep (as stored)
Texture2D mapColours : register(t2);         // Terrain::GroundColours: over the map at half its size, as stored
#endif

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float lit : TEXCOORD1;  // the sun's light (1) or a hill's shadow (0)
};

float SurHeight(float2 xz, float level) {
  return surHeights.SampleLevel(samplerLinearClamp, ((xz - surGrid.xy) * surGrid.z + 0.5) * surGrid.w, level) * surLevels.x;
}

// The sun's light at a point: steps toward the sun, each farther than the one before, the ground's height there
// against the line's; how near the line comes to the ground for its distance softens the shadow's edge.
float SurShadow(float3 p) {
  float3 L = sunDirection;
  if (L.y < 0.02) return 1;  // (the sun at the horizon or under it: its light is gone anyway)
  p.y += 0.8;
  float lit = 1, t = 3;
  [loop] for (int k = 0; k < 22; ++k) {
    float3 q = p + L * t;
    if (q.y > surLevels.z) break;  // (above the highest ground: nothing farther off is in the way)
    lit = min(lit, (q.y - SurHeight(q.xz, 0)) / (t * surLook.x));
    if (lit <= 0) break;
    t *= 1.3;
  }
  return smoothstep(0, 1, saturate(lit));
}

VOut VS(float3 position : POSITION) {
  VOut o;
  o.world = position;
  o.position = mul(viewProj, float4(position, 1));
  o.lit = SurShadow(position);
  return o;
}

#ifdef SURROUND_DETAIL
// A layer's normal map as read (terrain.cpp, LayerNormal): a two-channel one gets z from x and y; `up`: +y toward the top
// of its texture, turned to the stock layers' way.
float4 LayerNormal(float4 raw, float up) {
  float4 n = raw * 2 - 1;
  if (raw.b < 0.002) n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
  if (up > 0.5) n.y = -n.y;
  return n;
}

// The detail look: the map's own layers, laid by the surround's masks as the ground's shader lays the map's (terrain.cpp,
// Ground: the same pictures at the same repeats, so that a layer's picture runs on over the edge), the map's own masks
// by the edge.
float4 PS(VOut i) : SV_Target {
  float2 xz = i.world.xz;
  // its slope: from the heights about the pixel, as far apart as the pixel is wide (not finer than the samples)
  float2 across = fwidth(xz);
  float pixel = max(across.x, across.y);
  float wide = clamp(pixel, 1 / surGrid.z, 64 / surGrid.z);
  float level = log2(wide * surGrid.z);
  float3 base = normalize(float3(SurHeight(xz - float2(wide, 0), level) - SurHeight(xz + float2(wide, 0), level), 2 * wide,
                                 SurHeight(xz - float2(0, wide), level) - SurHeight(xz + float2(0, wide), level)));
  float outside = OutsideMap(i.world);
  // (1 at the map's edge: the map's own ground; how far out it goes over into the surround's is uneven along the edge,
  // four tenths of the way to all of it, so that the hand-over is no band parallel to the edge)
  float own = surLook.z > 0 ? 1 - smoothstep(0, surLook.z * (0.4 + 0.6 * CloudNoise(float3(xz * 0.045, 9.1))), outside) : 0;
  // (the layers read with the place's own steps, as the ground's: a layer is skipped where it is not there at all)
  float2 stepX = ddx(xz) * exp2(temporal.x), stepY = ddy(xz) * exp2(temporal.x);
  float away = distance(i.world, cameraPosition);
  float fine = max(1 - smoothstep(0.6, 1.0, away / surMore.z), own);  // (the layers' normals: near the eye, and at the edge)
  float2 place = xz - surGrid.xy;
  float4 m = surMasks.Sample(samplerLinearClamp, (place * surPictures.x + 0.5) * surPictures.y);
  // (the forest's picture read a little off its place, by a slow noise: a wood's side that is a straight line in the
  // picture wanders by some eight world units)
  float forest = 0;
  if (surPictures.z > 0) {
    float2 off = (float2(CloudNoise(float3(xz * 0.035, 1.3)), CloudNoise(float3(xz * 0.035, 5.7))) - 0.5) * 16;
    forest = surForest.Sample(samplerLinearClamp, ((place + off) * surPictures.z + 0.5) * surPictures.w);
  }

  // each layer's share here: the masks' four (and the second masks' up to four more) and what they leave; under the
  // forest its floor, over the layers below it
  float share[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  float taken = m.r + m.g + m.b + m.a;
  [branch] if (surPictures2.x > 0) {
    float4 m2 = surMasks2.SampleGrad(samplerLinearClamp, (place * surPictures2.x + 0.5) * surPictures2.y, ddx(xz) * surPictures2.x * surPictures2.y,
                                     ddy(xz) * surPictures2.x * surPictures2.y);
    if (surKinds2.x >= 0) { share[(int)surKinds2.x] += m2.r; taken += m2.r; }
    if (surKinds2.y >= 0) { share[(int)surKinds2.y] += m2.g; taken += m2.g; }
    if (surKinds2.z >= 0) { share[(int)surKinds2.z] += m2.b; taken += m2.b; }
    if (surKinds2.w >= 0) { share[(int)surKinds2.w] += m2.a; taken += m2.a; }
  }
  share[(int)surMore.x] += saturate(1 - taken);
  share[(int)surKinds.x] += m.r;
  share[(int)surKinds.y] += m.g;
  share[(int)surKinds.z] += m.b;
  share[(int)surKinds.w] += m.a;
  int floorLayer = (int)surMore.y;
  if (floorLayer > 0) {
    float f = smoothstep(0.2, 0.7, forest) * 0.92, under = 0;
    for (int k = 0; k < 9; ++k)
      if (k < floorLayer) {
        under += share[k];
        share[k] *= 1 - f;
      }
    share[floorLayer] += f * under;
  }
  // by the map's edge: the map's own shares there (its masks read on past the edge, mirrored; each stratum laid over
  // what is under it), going over into the surround's
  float4 rawLow = 0, rawHigh = 0;
  [branch] if (own > 0) {
    float2 uv = MirrorUv(xz * surMap.xy);
    rawLow = maskLow.SampleGrad(samplerLinearClamp, uv, stepX * surMap.xy, stepY * surMap.xy);
    rawHigh = maskHigh.SampleGrad(samplerLinearClamp, uv, stepX * surMap.xy, stepY * surMap.xy) * surMore.w;
    float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);
    float laid[9] = {1, low.x, low.y, low.z, low.w, high.x, high.y, high.z, high.w};
    float left = 1;
    for (int k = 8; k >= 0; --k) {
      float s = laid[k] * left;
      left -= s;
      share[k] = lerp(share[k], s, own);
    }
  }
  // the shares as the ground's shader has them: how much of what lies under it each stratum covers (`c`), and the value
  // its normal is laid by (`raw`: the mask as stored, half and more where the stratum shows at all)
  float c[9], raw[9];
  float above = 0, most = 0;
  for (int j = 8; j >= 1; --j) {
    c[j] = saturate(share[j] / max(1 - above, 1e-4));
    above += share[j];
    raw[j] = c[j] > 0.002 ? 0.5 + 0.5 * c[j] : 0;
    most = max(most, c[j]);
  }
  c[0] = raw[0] = 1;
  [branch] if (own > 0) {
    float stored[9] = {1, rawLow.x, rawLow.y, rawLow.z, rawLow.w, rawHigh.x, rawHigh.y, rawHigh.z, rawHigh.w};
    for (int j = 1; j < 9; ++j) raw[j] = lerp(raw[j], stored[j], own);
  }
  bool bare = most < 0.995;  // (the lower layer shows at all: not where a stratum covers everything)

  // a layer with its third map does not fade evenly into what lies under it: its high places come through first, and
  // it brings its own roughness and crease shading (`matter`: creases, roughness, how much of the pixel is such a layer's)
  #define MATERIAL(n) materialMaps.SampleGrad(samplerAniso, float3(xz * albedoTile[n].x, n), stepX * albedoTile[n].x, stepY * albedoTile[n].x).rgb
  float3 matter = 0;
  [branch] if (bare && albedoTile[0].y > 0.5) {
    float3 mm = MATERIAL(0);
    matter = float3(mm.r, mm.g, 1);
  }
  #define RICH(n) \
    [branch] if (albedoTile[n].y > 0.5 && raw[n] > 0.002) { \
      float3 mm = MATERIAL(n); \
      c[n] = saturate((mm.b + c[n] * 1.35 - 1) / 0.35); \
      raw[n] = saturate((mm.b + raw[n] * 1.35 - 1) / 0.35); \
      matter = lerp(matter, float3(mm.r, mm.g, 1), c[n]); \
    } else { \
      matter = lerp(matter, 0, c[n]); \
    }
  RICH(1) RICH(2) RICH(3) RICH(4) RICH(5) RICH(6) RICH(7) RICH(8)
  #define ALBEDO(n) albedoMaps[n].SampleGrad(samplerAniso, xz * albedoTile[n].x, stepX * albedoTile[n].x, stepY * albedoTile[n].x)
  #define OVER(value, read, part) [branch] if ((part) > 0) value = lerp(value, read, part)
  float4 albedo = 0;
  [branch] if (bare) albedo = ALBEDO(0);
  OVER(albedo, ALBEDO(1), c[1]);
  OVER(albedo, ALBEDO(2), c[2]);
  OVER(albedo, ALBEDO(3), c[3]);
  OVER(albedo, ALBEDO(4), c[4]);
  OVER(albedo, ALBEDO(5), c[5]);
  OVER(albedo, ALBEDO(6), c[6]);
  OVER(albedo, ALBEDO(7), c[7]);
  OVER(albedo, ALBEDO(8), c[8]);
  // the upper layer: the map's one picture does not reach here; its mean where each of these layers shows on the map,
  // and by the edge the picture itself (mirrored)
  float4 upper = upperOf[0];
  for (int u = 1; u < 9; ++u) upper = lerp(upper, upperOf[u], c[u]);
  [branch] if (own > 0) {
    float2 inMap = MirrorUv(xz * surMap.xy) * surMap.zw;
    upper = lerp(upper, albedoMaps[9].SampleGrad(samplerAniso, inMap * albedoTile[9].x, stepX * albedoTile[9].x, stepY * albedoTile[9].x), own);
  }
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);

  // the layers' normals, near the eye: laid by the masks as stored, then tilted from "straight up" to the ground's slope
  float3 N = base;
  [branch] if (fine > 0) {
    #define NORMAL(n) LayerNormal(normalMaps[n].SampleGrad(samplerAniso, xz * normalTile[n].x, stepX * normalTile[n].x, stepY * normalTile[n].x), normalTile[n].y)
    float4 n = float4(0, 0, 1, 0);
    [branch] if (bare) n = NORMAL(0);
    OVER(n, NORMAL(1), raw[1]);
    OVER(n, NORMAL(2), raw[2]);
    OVER(n, NORMAL(3), raw[3]);
    OVER(n, NORMAL(4), raw[4]);
    OVER(n, NORMAL(5), raw[5]);
    OVER(n, NORMAL(6), raw[6]);
    OVER(n, NORMAL(7), raw[7]);
    OVER(n, NORMAL(8), raw[8]);
    n.xyz = normalize(n.xyz);
    float3 detail = float3(n.x, sqrt(saturate(1 - dot(n.xy, n.xy))), n.y);
    float3 h = normalize(base + float3(0, 1, 0));
    float3 axisX = float3(1 - 2 * h.x * h.x, 2 * h.x * h.y, -2 * h.x * h.z);
    float3 axisZ = float3(-2 * h.z * h.x, 2 * h.z * h.y, 1 - 2 * h.z * h.z);
    N = normalize(lerp(base, float3(dot(detail, axisX), dot(detail, base), dot(detail, axisZ)), fine));
  }
  // a rough, non-metal surface, as the map's ground; a slope a little darker in its creases (as the block-out's), not
  // at the edge (the map's ground has none of that)
  float gloss = lerp(1 - albedo.a, albedo.a, surMore.w);
  float roughness = 1 - 0.5 * gloss * saturate(surTrees.z * 2);
  roughness = lerp(roughness, matter.y, matter.z);
  float creases = lerp(1, matter.x, matter.z) * lerp(0.75 + 0.25 * saturate(base.y), 1, own);
  float lit = i.lit;
  // the beach the waves have just wetted, as the map's shores have it (terrain.cpp): darker and shinier in a band over
  // the water's level, its edge uneven, and on under the water as the bed's soak
  [branch] if (shore.x > 0.5) {
    float uneven = (CloudNoise(float3(xz * 0.7, 2.5)) - 0.5) * shore.z * 0.8;
    float over = i.world.y - shore.y;
    float wet = saturate(1 - (over + uneven) / shore.z) * saturate(over * 20 + 1);
    float wetGloss = wet;
    if (shore.x > 1.5) {
      wet = saturate(1 - (over + uneven) / shore.z) * (1 - smoothstep(0, 0.8, -over));
      wetGloss = wet * saturate(over * 20 + 1);
    }
    albedo.rgb *= lerp(1, 0.6, wet);
    roughness = lerp(roughness, 0.22, wetGloss * 0.85);
  }
  // the forest: where the scenery's single trees stand, their shade on the ground under them; where none stand, the
  // forest as one canopy, a dark green cover with the crowns' grain (some five world units a crown, each crown's sunny
  // side lighter, larger patches lighter and darker), the grain let go of as a crown gets smaller than a pixel
  [branch] if (forest > 0.004) {
    // (the single trees: thinning out with the way from the map's edge, and left out far from the eye)
    float single = surTrees.y > 0 ? 1 - smoothstep(surTrees.x, surTrees.y, outside) : 0;
    if (surSight.y > 0) single *= 1 - smoothstep(surSight.x, surSight.y, away);
    float keep = saturate(1.7 - pixel * 0.5);
    float2 p = xz * 0.21;
    float crown = CloudNoise(float3(p, 3.3));
    float sunny = saturate(0.5 + (crown - CloudNoise(float3(p + sunDirection.xz * 0.35, 3.3))) * 2.4);
    float patches = CloudNoise(float3(xz * 0.043, 7.9));
    crown = lerp(0.5, crown, keep);
    sunny = lerp(0.5, sunny, keep);
    float cover = smoothstep(0.15, 0.75, forest + (crown - 0.5) * 0.5 * keep) * surCanopy.w;
    float canopy = cover * (1 - single * single);
    // (the crowns: the noise's high places, with the forest's floor showing between them as it does between the map's own
    // trees from above; half and half once a crown is smaller than a pixel)
    float crowns = canopy * lerp(0.5, smoothstep(0.36, 0.62, crown), keep);
    float3 leaf = surCanopy.rgb * (1 + 0.5 * (patches - 0.5));
    albedo.rgb = lerp(albedo.rgb, leaf, crowns * 0.9);
    roughness = lerp(roughness, 0.9, crowns);
    creases = lerp(creases, 0.8, crowns);
    N = normalize(lerp(N, base, crowns));
    lit *= lerp(1, 0.5 + 0.8 * sunny, crowns);
    // (between the crowns, and under the scenery's single trees: their shade on the floor, in patches)
    lit *= 1 - 0.42 * cover * (1 - crowns) * (0.6 + 0.8 * crown);
    creases *= 1 - 0.15 * cover * (1 - crowns);
  }
  float3 color = ModernLight(ToLinear(albedo.rgb), 0, roughness, creases, 0, N, i.world, lit);
  return float4(BeyondMap(color, i.world), 1);
}
#else
float4 PS(VOut i) : SV_Target {
  // its slope: from the heights about the pixel, as far apart as the pixel is wide (not finer than the samples)
  float2 across = fwidth(i.world.xz);
  float wide = clamp(max(across.x, across.y), 1 / surGrid.z, 64 / surGrid.z);
  float level = log2(wide * surGrid.z);
  float3 N = normalize(float3(SurHeight(i.world.xz - float2(wide, 0), level) - SurHeight(i.world.xz + float2(wide, 0), level), 2 * wide,
                              SurHeight(i.world.xz - float2(0, wide), level) - SurHeight(i.world.xz + float2(0, wide), level)));
  // its colours: the map's own ground where it is low by the water, level, high and steep, a little uneven
  float3 shore = float3(0.62, 0.58, 0.47), plain = float3(0.36, 0.38, 0.26), high = float3(0.42, 0.40, 0.34), steep = float3(0.45, 0.44, 0.42);
  if (surLook.y > 0.5) {
    shore = palette.Load(int3(0, 0, 0)).rgb;
    plain = palette.Load(int3(1, 0, 0)).rgb;
    high = palette.Load(int3(2, 0, 0)).rgb;
    steep = palette.Load(int3(3, 0, 0)).rgb;
  }
  float vary = CloudNoise(float3(i.world.xz * 0.006, 1.7)) * 0.45 + CloudNoise(float3(i.world.xz * 0.031, 4.1)) * 0.35 +
               CloudNoise(float3(i.world.xz * 0.13, 6.3)) * 0.2;
  float over = i.world.y - surLevels.y;  // (over the water)
  float3 c = lerp(plain, high, smoothstep(surLevels.w - 6, surLevels.w + 6, i.world.y + (vary - 0.5) * 14));
  c = lerp(shore, c, smoothstep(0.8, 3.2, over + (vary - 0.5) * 1.2));
  c = lerp(c, steep, smoothstep(0.90, 0.80, N.y + (vary - 0.5) * 0.06));
  c *= 0.9 + 0.2 * vary;
  // by the map's edge: the map's own colours there (read on past the edge, mirrored), going over into its own
  float own = surLook.z > 0 ? 1 - smoothstep(0, surLook.z, OutsideMap(i.world)) : 0;
  [branch] if (own > 0) c = lerp(c, mapColours.SampleLevel(samplerLinearClamp, MirrorUv(i.world.xz / beyond.xy), 0).rgb, own);
  float3 lit = ModernLight(ToLinear(c), 0, 0.92, 0.75 + 0.25 * saturate(N.y), 0, N, i.world, i.lit);
  return float4(BeyondMap(lit, i.world), 1);
}
#endif
)hlsl";

struct SurroundConstants {
  float grid[4], levels[4], look[4];
  // (the detail look's, after the block-out's)
  float pictures[4], kinds[4], more[4], map[4], canopy[4], trees[4], sight[4], pictures2[4], kinds2[4];
  float albedoTile[10][4], normalTile[9][4], upperOf[9][4];
};

// A square picture's smaller levels after it (`all`: the picture itself, `channels` bytes a pixel), each the mean of
// four of the one before; how many levels it has then.
int WithLevels(std::vector<uint8_t>* all, int size, int channels) {
  std::vector<uint8_t> last(*all);
  int levels = 1;
  while (size > 24 && levels < 8) {
    const int half = size / 2;
    std::vector<uint8_t> next(static_cast<size_t>(half) * static_cast<size_t>(half) * static_cast<size_t>(channels));
    for (int j = 0; j < half; ++j)
      for (int i = 0; i < half; ++i) {
        const uint8_t* r0 = last.data() + (static_cast<size_t>(2 * j) * static_cast<size_t>(size) + static_cast<size_t>(2 * i)) * static_cast<size_t>(channels);
        const uint8_t* r1 = r0 + static_cast<size_t>(size) * static_cast<size_t>(channels);
        uint8_t* to = next.data() + (static_cast<size_t>(j) * static_cast<size_t>(half) + static_cast<size_t>(i)) * static_cast<size_t>(channels);
        for (int k = 0; k < channels; ++k) to[k] = static_cast<uint8_t>((static_cast<uint32_t>(r0[k]) + r0[channels + k] + r1[k] + r1[channels + k] + 2) / 4);
      }
    all->insert(all->end(), next.begin(), next.end());
    last.swap(next);
    size = half;
    ++levels;
  }
  return levels;
}

// The points along one axis of a part: from `a` to `b` (samples), `stride` apart, the last at `b`.
std::vector<int> Stops(int a, int b, int stride) {
  std::vector<int> out;
  for (int i = a; i < b; i += stride) out.push_back(i);
  out.push_back(b);
  return out;
}

// Where the parts begin and end along one axis: the map's two edges (samples `m0`, `m1`) and from each every `cell`
// samples, out to the surround's ends (0 and `last`).
std::vector<int> Cuts(int m0, int m1, int last, int cell) {
  std::vector<int> out;
  for (int i = m0; i > 0; i -= cell) out.push_back(i);
  out.push_back(0);
  for (int i = m0 + cell; i < m1; i += cell) out.push_back(i);
  for (int i = m1; i < last; i += cell) out.push_back(i);
  out.push_back(last);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

void Surround::Forget() {
  if (device_) {
    for (gfx::Buffer* b : {&vertices_, &indices_})
      if (*b) device_->Destroy(*b);
    if (texture_) device_->Destroy(texture_);
  }
  vertices_ = indices_ = {};
  if (device_) {  // (the detail look's)
    if (masksTexture_) device_->Destroy(masksTexture_);
    if (forestTexture_) device_->Destroy(forestTexture_);
    if (masks2Texture_) device_->Destroy(masks2Texture_);
  }
  masksTexture_ = forestTexture_ = masks2Texture_ = {};
  masks2_.clear();
  masks2Samples_ = 0;
  std::fill(kinds2_, kinds2_ + 4, -1);
  treesGiven_[0] = treesGiven_[1] = 0;
  masks_.clear();
  forest_.clear();
  masksSamples_ = forestSamples_ = 0;
  floorGiven_ = -1;
  decor_.clear();
  LetGoAsCame();
  upperAsked_ = detailNow_ = false;
  texture_ = palette_ = ground_ = {};
  heights_.clear();
  for (std::vector<float>& e : edges_) e.clear();
  parts_.clear();
  samples_ = drawn_ = 0;
  reach_ = 0;
  made_ = now_ = false;
}

bool Surround::Open(const MapData& map, const std::string& json, const ReadFn& read, std::string* why) {
  const auto started = std::chrono::steady_clock::now();
  Forget();
  JsonValue root;
  if (!ReadJson(json, &root, why)) return false;
  const JsonValue& part = root["near"];
  const std::string name = part["heights"].StringOr("");
  const int samples = static_cast<int>(part["samples"].NumberOr(0));
  const float step = static_cast<float>(part["units_per_sample"].NumberOr(0));
  const float unit = static_cast<float>(part["height_scale"].NumberOr(1.0 / 128));
  const JsonValue& origin = part["origin"];
  if (name.empty() || samples < 3 || step <= 0 || unit <= 0 || origin.items.size() != 2) {
    *why = "it names no heights, or not their samples, units_per_sample and origin";
    return false;
  }
  if (map.width <= 0 || map.height <= 0 || map.heights.empty()) {
    *why = "no map";
    return false;
  }
  const float ox = static_cast<float>(origin.items[0].NumberOr(0)), oz = static_cast<float>(origin.items[1].NumberOr(0));
  const float W = static_cast<float>(map.width), H = static_cast<float>(map.height);
  // the map's edges among its samples: they must lie on samples, with ground of it on every side
  const float fx0 = -ox / step, fz0 = -oz / step, fx1 = (W - ox) / step, fz1 = (H - oz) / step;
  const int mx0 = static_cast<int>(std::lround(fx0)), mz0 = static_cast<int>(std::lround(fz0));
  const int mx1 = static_cast<int>(std::lround(fx1)), mz1 = static_cast<int>(std::lround(fz1));
  auto whole = [](float f, int i) { return std::fabs(f - static_cast<float>(i)) < 1e-3f; };
  if (!whole(fx0, mx0) || !whole(fz0, mz0) || !whole(fx1, mx1) || !whole(fz1, mz1) || mx0 < 1 || mz0 < 1 || mx1 > samples - 2 || mz1 > samples - 2) {
    *why = "the map's edges do not lie on its samples with ground on every side";
    return false;
  }
  const std::optional<std::string> file = read(name);
  if (!file) {
    *why = "no " + name;
    return false;
  }
  int width = 0, height = 0;
  std::vector<uint16_t> raw;
  if (!ReadPngGrey16(*file, &width, &height, &raw, why)) {
    *why = name + ": " + *why;
    return false;
  }
  if (width != samples || height != samples) {
    *why = name + " is not " + std::to_string(samples) + " x " + std::to_string(samples);
    return false;
  }

  // how far its heights are from the map's own along the map's edge, as it came
  float apart = 0;
  int off = 0, along = 0;
  auto its = [&](int i, int j) { return static_cast<float>(raw[static_cast<size_t>(j) * static_cast<size_t>(samples) + static_cast<size_t>(i)]) * unit; };
  auto compare = [&](int i, int j) {
    const float d = std::fabs(its(i, j) - map.Height(ox + static_cast<float>(i) * step, oz + static_cast<float>(j) * step));
    apart = std::max(apart, d);
    off += d > 1.0f;
    ++along;
  };
  for (int i = mx0; i <= mx1; ++i) compare(i, mz0), compare(i, mz1);
  for (int j = mz0 + 1; j < mz1; ++j) compare(mx0, j), compare(mx1, j);

  if (!root["decor"].StringOr("").empty()) asCame_ = raw;  // (the detail look: the scenery's footing, JoinMoved)
  // joined to the map: the map's own heights inside its square, and over the first `seam` world units outside the edge
  // from the map's edge height over into its own
  float highest = 0, mapHighest = 0;
  for (uint16_t h : map.heights) mapHighest = std::max(mapHighest, static_cast<float>(h) * map.heightScale);
  const float join = std::max(seam, 0.0f);
  for (int j = 0; j < samples; ++j) {
    const float z = oz + static_cast<float>(j) * step;
    uint16_t* row = raw.data() + static_cast<size_t>(j) * static_cast<size_t>(samples);
    for (int i = 0; i < samples; ++i) {
      const float x = ox + static_cast<float>(i) * step;
      const float dx = std::max({-x, x - W, 0.0f}), dz = std::max({-z, z - H, 0.0f});
      const float out = std::sqrt(dx * dx + dz * dz);
      float h = static_cast<float>(row[i]) * unit;
      if (out < join || out <= 0) {
        const float edge = map.Height(std::clamp(x, 0.0f, W), std::clamp(z, 0.0f, H));
        float t = join > 0 ? std::clamp(out / join, 0.0f, 1.0f) : 0;
        t = t * t * (3 - 2 * t);
        h = edge + (h - edge) * t;
        row[i] = static_cast<uint16_t>(std::clamp(std::lround(h / unit), 0L, 65535L));
      }
      highest = std::max(highest, h);
    }
  }

  for (int i = 0; i <= map.width; ++i) {
    edges_[0].push_back(map.HeightAt(i, 0));
    edges_[1].push_back(map.HeightAt(i, map.height));
  }
  for (int j = 0; j <= map.height; ++j) {
    edges_[2].push_back(map.HeightAt(0, j));
    edges_[3].push_back(map.HeightAt(map.width, j));
  }
  heights_ = std::move(raw);
  samples_ = samples;
  step_ = step;
  unit_ = unit;
  origin_[0] = ox;
  origin_[1] = oz;
  mapSize_[0] = W;
  mapSize_[1] = H;
  reach_ = static_cast<float>(std::min({mx0, mz0, samples - 1 - mx1, samples - 1 - mz1})) * step;
  highest_ = highest;
  mapHighest_ = mapHighest;
  const JsonValue& level = root["water"]["level"];
  sea_ = level.kind == JsonValue::Number ? static_cast<float>(level.number) : map.water.present ? map.water.elevation : -1e9f;
  mapSea_ = map.water.present ? map.water.elevation : sea_;
  {  // the detail look's files, where the json names them: a file that cannot be used is said and left out (its ground
     // then the block-out, its forest none), the surround itself stays
    const JsonValue& masks = root["masks"];
    if (const std::string file = masks["file"].StringOr(""); !file.empty()) {
      static const char* const kNames[5] = {"r", "g", "b", "a", "rest"};
      int kinds[5];
      bool ok = true;
      for (int k = 0; k < 5; ++k) {
        kinds[k] = static_cast<int>(masks[kNames[k]].NumberOr(-1));
        ok = ok && kinds[k] >= 0 && kinds[k] <= 8;
      }
      std::string bad;
      int w = 0, h = 0;
      std::vector<uint8_t> pixels;
      const std::optional<std::string> bytes = ok ? read(file) : std::nullopt;
      if (!ok) bad = "its r, g, b, a and rest must each name one of the map's layers 0 to 8";
      else if (!bytes) bad = "no such file";
      else if (ReadPng8(*bytes, 4, &w, &h, &pixels, &bad) && (w != h || w < 2)) bad = "not square";
      if (bad.empty()) {
        masks_ = std::move(pixels);
        masksSamples_ = w;
        std::copy(kinds, kinds + 5, kinds_);
      } else {
        fprintf(stderr, "render: the surround's masks %s: %s (its ground is drawn as the block-out)\n", file.c_str(), bad.c_str());
      }
    }
    if (const std::string file = root["forest"].StringOr(""); !file.empty()) {
      std::string bad;
      int w = 0, h = 0;
      std::vector<uint8_t> pixels;
      const std::optional<std::string> bytes = read(file);
      if (!bytes) bad = "no such file";
      else if (ReadPng8(*bytes, 1, &w, &h, &pixels, &bad) && (w != h || w < 2)) bad = "not square";
      if (bad.empty()) {
        forest_ = std::move(pixels);
        forestSamples_ = w;
      } else {
        fprintf(stderr, "render: the surround's forest %s: %s (no forest is drawn)\n", file.c_str(), bad.c_str());
      }
    }
    // (the layer under the forest: beside the forest's name, or among the masks' numbers)
    const int floor = static_cast<int>(root["forest_floor"].NumberOr(masks["forest_floor"].NumberOr(-1)));
    floorGiven_ = floor >= 1 && floor <= 8 ? floor : -1;
    canopyColour_[0] = 0.165f, canopyColour_[1] = 0.155f, canopyColour_[2] = 0.095f;
    if (const JsonValue& colour = root["forest_colour"]; colour.items.size() == 3)
      for (int k = 0; k < 3; ++k) canopyColour_[k] = std::clamp(static_cast<float>(colour.items[static_cast<size_t>(k)].NumberOr(0.08)), 0.0f, 1.0f);
    decor_ = root["decor"].StringOr("");
    // the second masks, with the first: up to four more layers' shares (a picture of three channels or of four)
    const JsonValue& masks2 = root["masks2"];
    if (const std::string file = masks2["file"].StringOr(""); !file.empty() && !masks_.empty()) {
      static const char* const kNames[4] = {"r", "g", "b", "a"};
      int kinds[4];
      bool ok = true, any = false;
      for (int k = 0; k < 4; ++k) {
        kinds[k] = masks2[kNames[k]].kind == JsonValue::Number ? static_cast<int>(masks2[kNames[k]].number) : -1;
        ok = ok && kinds[k] >= -1 && kinds[k] <= 8;
        any = any || kinds[k] >= 0;
      }
      std::string bad;
      int w = 0, h = 0, channels = 3;
      std::vector<uint8_t> pixels;
      const std::optional<std::string> bytes = ok && any ? read(file) : std::nullopt;
      if (!ok || !any) bad = "its r, g, b and a must each name one of the map's layers 0 to 8 (or be left out), one of them at least";
      else if (!bytes) bad = "no such file";
      else if (!ReadPng8(*bytes, 3, &w, &h, &pixels, &bad)) {
        channels = 4;
        if (ReadPng8(*bytes, 4, &w, &h, &pixels, &bad)) bad.clear();
        else bad = "neither an 8-bit RGB nor an 8-bit RGBA PNG without interlace";
      }
      if (bad.empty() && (w != h || w < 2)) bad = "not square";
      if (bad.empty() && channels == 3 && kinds[3] >= 0) bad = "its a names a layer, but the picture has no alpha";
      if (bad.empty()) {
        masks2_.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i) {
          for (int k = 0; k < 3; ++k) masks2_[i * 4 + static_cast<size_t>(k)] = pixels[i * static_cast<size_t>(channels) + static_cast<size_t>(k)];
          masks2_[i * 4 + 3] = channels == 4 ? pixels[i * 4 + 3] : 0;
        }
        masks2Samples_ = w;
        std::copy(kinds, kinds + 4, kinds2_);
      } else {
        fprintf(stderr, "render: the surround's second masks %s: %s (its ground is drawn by the first alone)\n", file.c_str(), bad.c_str());
      }
    }
    // from where to where the scenery's trees thin out, where the json says
    if (const JsonValue& thin = root["decor_trees"]; thin.items.size() == 2) {
      treesGiven_[0] = static_cast<float>(thin.items[0].NumberOr(0));
      treesGiven_[1] = static_cast<float>(thin.items[1].NumberOr(0));
    }
    if (!masks_.empty())
      printf("render: the map's surround: its ground's masks %d x %d (the map's layers %d, %d, %d, %d, the rest %d)%s%s%s\n", masksSamples_, masksSamples_,
             kinds_[0], kinds_[1], kinds_[2], kinds_[3], kinds_[4],
             masks2_.empty() ? "" : (", its second masks (the layers " + std::to_string(kinds2_[0]) + ", " + std::to_string(kinds2_[1]) + ", " +
                                     std::to_string(kinds2_[2]) + ", " + std::to_string(kinds2_[3]) + "; -1: none)").c_str(),
             forest_.empty() ? "" : ", its forest", decor_.empty() ? "" : ", its scenery");
  }
  printf("render: the map's surround: %d x %d heights %.4g world units apart, %.0f past the map's edges; along the edge %d of %d are more "
         "than 1 from the map's own (the most %.2f), joined over %.0f world units (read in %.0f ms)\n",
         samples, samples, step, reach_, off, along, apart, join,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  return true;
}

float Surround::HeightAt(float x, float z) const {
  if (heights_.empty()) return 0;
  const float fx = std::clamp((x - origin_[0]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const float fz = std::clamp((z - origin_[1]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const int i = std::min(static_cast<int>(fx), samples_ - 2), j = std::min(static_cast<int>(fz), samples_ - 2);
  const float tx = fx - static_cast<float>(i), tz = fz - static_cast<float>(j);
  const uint16_t* r0 = heights_.data() + static_cast<size_t>(j) * static_cast<size_t>(samples_) + static_cast<size_t>(i);
  const uint16_t* r1 = r0 + samples_;
  const float a = static_cast<float>(r0[0]) + (static_cast<float>(r0[1]) - static_cast<float>(r0[0])) * tx;
  const float b = static_cast<float>(r1[0]) + (static_cast<float>(r1[1]) - static_cast<float>(r1[0])) * tx;
  return (a + (b - a) * tz) * unit_;
}

float Surround::JoinMoved(float x, float z) const {
  if (asCame_.size() != heights_.size() || heights_.empty()) return 0;
  const float fx = std::clamp((x - origin_[0]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const float fz = std::clamp((z - origin_[1]) / step_, 0.0f, static_cast<float>(samples_ - 1));
  const int i = std::min(static_cast<int>(fx), samples_ - 2), j = std::min(static_cast<int>(fz), samples_ - 2);
  const float tx = fx - static_cast<float>(i), tz = fz - static_cast<float>(j);
  auto moved = [&](int a, int b) {
    const size_t at = static_cast<size_t>(b) * static_cast<size_t>(samples_) + static_cast<size_t>(a);
    return (static_cast<float>(heights_[at]) - static_cast<float>(asCame_[at])) * unit_;
  };
  const float a = moved(i, j) + (moved(i + 1, j) - moved(i, j)) * tx, b = moved(i, j + 1) + (moved(i + 1, j + 1) - moved(i, j + 1)) * tx;
  return a + (b - a) * tz;
}

void Surround::Prepare(gfx::Device* device, bool on, Terrain& terrain, BeyondLand& colours) {
  now_ = false;
  if (!on || failed_ || heights_.empty()) return;
  device_ = device;
  if (!tried_) {
    tried_ = true;
    const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "the map's surround";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
    pd.stride = sizeof(float) * 3;
    pd.cull = gfx::Cull::None;  // (its skirts and the curtain along the map's edge are seen from either side)
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    std::string error;
    pipeline_ = device_->CreatePipeline(pd, &error);
    if (!pipeline_) {
      fprintf(stderr, "render: the map's surround cannot be drawn: %s\n", error.c_str());
      failed_ = true;
      return;
    }
  }
  if (!made_) {
    Make();
    made_ = true;
  }
  // (the detail look) with the map's ground's masks, unless the block-out is asked for: its pipeline made the first time
  // (where it cannot be, the block-out is drawn), the map's upper layer by its layers asked once
  terrain_ = &terrain;
  detailNow_ = false;
  if (!masks_.empty() && !blockOut && masksTexture_) {
    if (!detailTried_) {
      detailTried_ = true;
      const std::string source = std::string("#define SURROUND_DETAIL 1\n") + kViewShaderBlock + kLightingShaderBlock + kShader;
      gfx::PipelineDesc pd;
      pd.name = "the map's surround, in the map's layers";
      pd.vertexSource = pd.pixelSource = source;
      pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
      pd.stride = sizeof(float) * 3;
      pd.cull = gfx::Cull::None;
      pd.colorFormat = kSceneFormat;
      pd.samples = gSceneSamples;
      std::string error;
      detail_ = device_->CreatePipeline(pd, &error);
      if (!detail_) fprintf(stderr, "render: the map's surround cannot be drawn in the map's layers (the block-out is drawn): %s\n", error.c_str());
    }
    if (detail_ && !upperAsked_) {
      upperAsked_ = true;
      const auto started = std::chrono::steady_clock::now();
      const bool have = terrain.UpperByLayer(upperOf_);
      printf("render: the map's surround: the map's upper layer by its layers %s (%.0f ms)\n", have ? "read" : "not there",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }
    detailNow_ = static_cast<bool>(detail_);
  }
  if (!detailNow_) {
    palette_ = colours.Palette(device, terrain);
    ground_ = terrain.GroundColours();
  }
  now_ = !parts_.empty() && vertices_ && indices_ && texture_;
}

// What is drawn, made for the surround read (Open) the first time it is wanted: its heights as a texture with smaller
// levels, and its ground in parts of 64 samples, each at one fineness by how far from the map it lies, with a skirt
// round it; then the curtain along the map's four edges.
void Surround::Make() {
  const auto started = std::chrono::steady_clock::now();
  const int n = samples_;
  {  // the texture: each smaller level the mean of four of the one before
    std::vector<uint16_t> all(heights_);
    std::vector<uint16_t> last(heights_);
    int size = n, levels = 1;
    while (size > 24 && levels < 8) {
      const int half = size / 2;
      std::vector<uint16_t> next(static_cast<size_t>(half) * static_cast<size_t>(half));
      for (int j = 0; j < half; ++j)
        for (int i = 0; i < half; ++i) {
          const uint16_t* r0 = last.data() + static_cast<size_t>(2 * j) * static_cast<size_t>(size) + static_cast<size_t>(2 * i);
          const uint16_t* r1 = r0 + size;
          next[static_cast<size_t>(j) * static_cast<size_t>(half) + static_cast<size_t>(i)] =
              static_cast<uint16_t>((static_cast<uint32_t>(r0[0]) + r0[1] + r1[0] + r1[1] + 2) / 4);
        }
      all.insert(all.end(), next.begin(), next.end());
      last.swap(next);
      size = half;
      ++levels;
    }
    gfx::TextureDesc td;
    td.width = td.height = n;
    td.mips = levels;
    td.format = gfx::Format::R16;
    td.data = all.data();
    td.dataSize = all.size() * sizeof(uint16_t);
    texture_ = device_->CreateTexture(td);
    mips_ = levels;
  }
  // (the detail look) the masks and the forest as textures, with smaller levels
  if (!masks_.empty()) {
    std::vector<uint8_t> all(masks_);
    gfx::TextureDesc td;
    td.width = td.height = masksSamples_;
    td.mips = WithLevels(&all, masksSamples_, 4);
    td.format = gfx::Format::RGBA8;
    td.data = all.data();
    td.dataSize = all.size();
    masksTexture_ = device_->CreateTexture(td);
  }
  if (!masks2_.empty()) {
    std::vector<uint8_t> all(masks2_);
    gfx::TextureDesc td;
    td.width = td.height = masks2Samples_;
    td.mips = WithLevels(&all, masks2Samples_, 4);
    td.format = gfx::Format::RGBA8;
    td.data = all.data();
    td.dataSize = all.size();
    masks2Texture_ = device_->CreateTexture(td);
  }
  if (!forest_.empty()) {
    std::vector<uint8_t> all(forest_);
    gfx::TextureDesc td;
    td.width = td.height = forestSamples_;
    td.mips = WithLevels(&all, forestSamples_, 1);
    td.format = gfx::Format::R8;
    td.data = all.data();
    td.dataSize = all.size();
    forestTexture_ = device_->CreateTexture(td);
  }

  const float W = mapSize_[0], H = mapSize_[1];
  const int mx0 = static_cast<int>(std::lround(-origin_[0] / step_)), mz0 = static_cast<int>(std::lround(-origin_[1] / step_));
  const int mx1 = static_cast<int>(std::lround((W - origin_[0]) / step_)), mz1 = static_cast<int>(std::lround((H - origin_[1]) / step_));
  auto height = [&](int i, int j) { return static_cast<float>(heights_[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)]) * unit_; };
  std::vector<float> points;     // x, y, z
  std::vector<uint32_t> list;
  parts_.clear();
  auto point = [&](float x, float y, float z) {
    points.push_back(x);
    points.push_back(y);
    points.push_back(z);
    return static_cast<uint32_t>(points.size() / 3 - 1);
  };
  auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint32_t k : {a, c, b, b, c, d}) list.push_back(k);
  };

  const std::vector<int> cutsX = Cuts(mx0, mx1, n - 1, 64), cutsZ = Cuts(mz0, mz1, n - 1, 64);
  for (size_t cz = 0; cz + 1 < cutsZ.size(); ++cz)
    for (size_t cx = 0; cx + 1 < cutsX.size(); ++cx) {
      const int i0 = cutsX[cx], i1 = cutsX[cx + 1], j0 = cutsZ[cz], j1 = cutsZ[cz + 1];
      if (i0 >= mx0 && i1 <= mx1 && j0 >= mz0 && j1 <= mz1) continue;  // (the map itself)
      const float dx = static_cast<float>(std::max({mx0 - i1, i0 - mx1, 0})) * step_, dz = static_cast<float>(std::max({mz0 - j1, j0 - mz1, 0})) * step_;
      const float away = std::sqrt(dx * dx + dz * dz);
      if (away >= reach_) continue;  // (lost in the distance there, wholly)
      const int stride = away < 128 ? 1 : away < 384 ? 2 : away < 768 ? 4 : 8;
      const std::vector<int> xs = Stops(i0, i1, stride), zs = Stops(j0, j1, stride);
      const float drop = std::max(3.0f, 1.5f * static_cast<float>(stride) * step_);
      Part part;
      part.first = static_cast<uint32_t>(list.size());
      float low = 1e30f, high = -1e30f;
      const uint32_t base = static_cast<uint32_t>(points.size() / 3);
      const uint32_t nx = static_cast<uint32_t>(xs.size()), nz = static_cast<uint32_t>(zs.size());
      for (int j : zs)
        for (int i : xs) {
          const float y = height(i, j);
          low = std::min(low, y);
          high = std::max(high, y);
          point(origin_[0] + static_cast<float>(i) * step_, y, origin_[1] + static_cast<float>(j) * step_);
        }
      for (uint32_t b = 0; b + 1 < nz; ++b)
        for (uint32_t a = 0; a + 1 < nx; ++a)
          quad(base + b * nx + a, base + b * nx + a + 1, base + (b + 1) * nx + a, base + (b + 1) * nx + a + 1);
      // its skirt: along each of its four sides, the side's points again `drop` lower
      auto skirt = [&](uint32_t from, uint32_t by, uint32_t count) {
        uint32_t before = 0;
        for (uint32_t k = 0; k < count; ++k) {
          const uint32_t top = base + from + k * by;
          const uint32_t under = point(points[top * 3], points[top * 3 + 1] - drop, points[top * 3 + 2]);
          if (k) quad(top - by, top, before, under);
          before = under;
        }
      };
      skirt(0, 1, nx);
      skirt((nz - 1) * nx, 1, nx);
      skirt(0, nx, nz);
      skirt(nx - 1, nx, nz);
      part.count = static_cast<uint32_t>(list.size()) - part.first;
      part.lo = {origin_[0] + static_cast<float>(i0) * step_, low - drop, origin_[1] + static_cast<float>(j0) * step_};
      part.hi = {origin_[0] + static_cast<float>(i1) * step_, high, origin_[1] + static_cast<float>(j1) * step_};
      parts_.push_back(part);
    }

  // the curtain: along each of the map's edges, at every grid point of the map, from the higher of the map's own edge
  // and the surround's there down to under the lower (the map's ground is drawn finer along its edge than the
  // surround's, and coarser from afar: what would show between the two is closed)
  auto curtain = [&](float x0, float z0, float x1, float z1, const std::vector<float>& edge) {
    const int count = static_cast<int>(edge.size()) - 1;
    if (count < 1) return;
    Part part;
    part.first = static_cast<uint32_t>(list.size());
    float low = 1e30f, high = -1e30f;
    uint32_t topBefore = 0, underBefore = 0;
    for (int k = 0; k <= count; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(count);
      const float x = x0 + (x1 - x0) * t, z = z0 + (z1 - z0) * t;
      const float own = HeightAt(x, z), maps = edge[static_cast<size_t>(k)];
      const float up = std::max(own, maps) + 0.05f, down = std::min(own, maps) - 6.0f;
      const uint32_t top = point(x, up, z), under = point(x, down, z);
      if (k) quad(topBefore, top, underBefore, under);
      topBefore = top;
      underBefore = under;
      low = std::min(low, down);
      high = std::max(high, up);
    }
    part.count = static_cast<uint32_t>(list.size()) - part.first;
    part.lo = {std::min(x0, x1), low, std::min(z0, z1)};
    part.hi = {std::max(x0, x1), high, std::max(z0, z1)};
    parts_.push_back(part);
  };
  curtain(0, 0, W, 0, edges_[0]);
  curtain(0, H, W, H, edges_[1]);
  curtain(0, 0, 0, H, edges_[2]);
  curtain(W, 0, W, H, edges_[3]);

  if (!list.empty()) {
    gfx::BufferDesc vb;
    vb.stride = sizeof(float) * 3;
    vb.size = static_cast<uint32_t>(points.size() * sizeof(float));
    vb.data = points.data();
    vertices_ = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 4;
    ib.size = static_cast<uint32_t>(list.size() * sizeof(uint32_t));
    ib.data = list.data();
    indices_ = device_->CreateBuffer(ib);
  }
  printf("render: the map's surround: %zu triangles in %zu parts, %zu points (made in %.0f ms)\n", list.size() / 3, parts_.size(), points.size() / 3,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void Surround::Draw(const View& view) {
  drawn_ = 0;
  if (!now_) return;
  SurroundConstants c = {};
  c.grid[0] = origin_[0];
  c.grid[1] = origin_[1];
  c.grid[2] = 1 / step_;
  c.grid[3] = 1 / static_cast<float>(samples_);
  c.levels[0] = unit_ * 65535.0f;
  c.levels[1] = sea_;
  c.levels[2] = highest_ + 1;
  c.levels[3] = mapSea_ + (mapHighest_ - mapSea_) * 0.45f;  // (as the map's colours were gathered: BeyondLand's palette)
  c.look[0] = std::max(softness, 1e-3f);
  c.look[1] = palette_ ? 1.0f : 0.0f;
  c.look[2] = ground_ ? std::max(ownColours, 0.0f) : 0.0f;
  // (the detail look) the map's layers bound where the ground's shader has them, and what it says of each; else the
  // block-out, as before
  Terrain::LayerTiles tiles;
  bool detailed = detailNow_ && terrain_;
  if (detailed) {
    device_->SetPipeline(detail_);
    detailed = terrain_->BindLayers(&tiles);
  }
  if (detailed) {
    const float side = static_cast<float>(samples_ - 1) * step_;  // (the square all its pictures lie over)
    c.look[2] = std::max(ownColours, 0.0f);
    c.pictures[0] = static_cast<float>(masksSamples_ - 1) / side;
    c.pictures[1] = 1 / static_cast<float>(masksSamples_);
    c.pictures[2] = forestTexture_ ? static_cast<float>(forestSamples_ - 1) / side : 0.0f;
    c.pictures[3] = forestTexture_ ? 1 / static_cast<float>(forestSamples_) : 0.0f;
    for (int k = 0; k < 4; ++k) c.kinds[k] = static_cast<float>(kinds_[k]);
    c.more[0] = static_cast<float>(kinds_[4]);
    c.more[1] = static_cast<float>(forestTexture_ ? (floorGiven_ >= 0 ? floorGiven_ : forestFloor >= 1 && forestFloor <= 8 ? forestFloor : -1) : -1);
    c.more[2] = std::max(fineReach, 1.0f);
    c.more[3] = tiles.xp;
    c.map[0] = 1 / mapSize_[0];
    c.map[1] = 1 / mapSize_[1];
    c.map[2] = mapSize_[0];
    c.map[3] = mapSize_[1];
    std::copy(canopyColour_, canopyColour_ + 3, c.canopy);
    c.canopy[3] = std::clamp(canopy, 0.0f, 1.0f);
    c.trees[0] = treesFrom;
    c.trees[1] = treesTo > treesFrom ? treesTo : 0.0f;
    c.trees[2] = tiles.specular;
    c.sight[0] = sightFrom;
    c.sight[1] = c.trees[1] > 0 && sightTo > sightFrom ? sightTo : 0.0f;
    c.pictures2[0] = masks2Texture_ ? static_cast<float>(masks2Samples_ - 1) / side : 0.0f;
    c.pictures2[1] = masks2Texture_ ? 1 / static_cast<float>(masks2Samples_) : 0.0f;
    for (int k = 0; k < 4; ++k) c.kinds2[k] = static_cast<float>(kinds2_[k]);
    std::memcpy(c.albedoTile, tiles.albedoTile, sizeof c.albedoTile);
    std::memcpy(c.normalTile, tiles.normalTile, sizeof c.normalTile);
    std::memcpy(c.upperOf, upperOf_, sizeof c.upperOf);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &c, sizeof c);
    device_->SetTexture(0, texture_);
    device_->SetTexture(1, masksTexture_);
    device_->SetTexture(4, forestTexture_ ? forestTexture_ : texture_);
    device_->SetTexture(5, masks2Texture_ ? masks2Texture_ : masksTexture_);
  } else {
    device_->SetPipeline(pipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, &c, 12 * sizeof(float));
    device_->SetTexture(0, texture_);
    device_->SetTexture(1, palette_ ? palette_ : texture_);
    device_->SetTexture(2, ground_ ? ground_ : texture_);
  }
  device_->SetVertexBuffer(vertices_);
  device_->SetIndexBuffer(indices_);
  // the parts in view, those that follow one another in the list in one draw
  uint32_t first = 0, count = 0;
  for (const Part& p : parts_) {
    if (!view.BoxVisible(p.lo, p.hi)) continue;
    drawn_ += static_cast<int>(p.count / 3);
    if (count && first + count == p.first) {
      count += p.count;
      continue;
    }
    if (count) device_->DrawIndexed(count, first);
    first = p.first;
    count = p.count;
  }
  if (count) device_->DrawIndexed(count, first);
}

}  // namespace render
