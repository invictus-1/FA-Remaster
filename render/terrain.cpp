#include "terrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "json.h"
#include "lighting.h"
#include "ray_scene.h"
#include "remasters.h"

namespace render {
namespace {

constexpr int kPatchQuads = 16;             // quads along one side of the grid mesh
constexpr int kLeafCells = 2 * kPatchQuads; // a level-0 node: 2 x 2 patches of one-cell quads
constexpr int kMaxLevels = 12;
constexpr int kMaxPatches = 1024;           // per draw call (the size of the shader's array)

// The blending and lighting below follow what the original game's terrain shaders compute (the looks it
// calls TTerrain and TTerrainXP), written anew:
//  - colour: the lower layer, then each stratum blended in by its mask channel (mask * 2 - 1, clamped),
//    then the upper (macro) layer by its own alpha;
//  - normal: the layers' normal maps blended by the raw mask values, then tilted from "straight up"
//    to the ground's own normal, which the map file stores as a texture;
//  - light: sun * max(0, N.L) + ambience (+ specular), scaled by the map's lighting multiplier, with the
//    "shadow fill" colour filling what the light leaves; then the water ramp tints by water depth.
//  - the look it calls TTerrainGlow (a campaign map's glowing ground) is TTerrain with one change:
//    the second stratum's picture drifts in a slow small circle, and its alpha says how much that
//    stratum glows instead of how dull it is.
const char kShader[] = R"hlsl(
cbuffer TerrainConstants : register(b1) {
  float4 mapSize;        // width, height, 1 / width, 1 / height
  float4 params;         // x: world height of the largest height value, y: 1 for the XP look, z: 1 for the glowing look,
                         // w: 1 = the ground goes on beyond the map's edge (the map mirrored)
  float4 specularColor;
  float4 albedoTile[10]; // x: repeats per world unit; y: 1 = the layer has its third map (materialMaps); [parallax] z: its depth
  float4 normalTile[9];
  float4 morph[12];      // per level: x: distance where sliding starts, y: 1 / (length of the slide)
  // deep tracks (deep_tracks.h, TrackFieldConstants): the field's levels, and per layer what kind of ground it is
  float4 tracksLevel[3];
  float4 tracksAtlas[3];
  float4 tracksInfo;     // x, y: 1 / the atlas's size; z: 1 = deep tracks are read
  float4 kindA[10];      // per layer, shares: snow, mud, sand, grass
  float4 kindB[10];      // dirt, hard ground (rock, concrete)
  float4 mudInfo;        // deep tracks' mud (MudInRuts): x 1 = the map's mud is drawn in the ruts; y its repeats per world unit
#ifdef GROUND_PARALLAX
  float4 parallax;       // [parallax] x 1 = on; y the distance from the eye it is full within; z 1 / the length it fades out over;
                         // w how much the relief shades itself from the sun (albedoTile[n].z: each layer's depth)
#endif
};
cbuffer Patches : register(b2) {
  float4 patches[1024];  // x, z of the patch's corner, size of a quad, level
};

Texture2D<float> heightMap : register(t0);
Texture2D normalMap : register(t1);
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
Texture2D waterMap : register(t4);
Texture2D waterRamp : register(t5);
Texture2D albedoMaps[10] : register(t6);
Texture2D normalMaps[9] : register(t16);
Texture2DArray materialMaps : register(t25);  // per layer: r creases' shading, g roughness, b height

float HeightAt(int2 c) {
  c = clamp(c, int2(0, 0), int2(mapSize.xy));
  return heightMap.Load(int3(c, 0)) * params.x;
}
float HeightBilinear(float2 p) {
  p = MirrorUv(p * mapSize.zw) * mapSize.xy;  // (beyond the map: the map mirrored; on it: as it is)
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  float a = HeightAt(c), b = HeightAt(c + int2(1, 0)), d = HeightAt(c + int2(0, 1)), e = HeightAt(c + int2(1, 1));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 grid : POSITION, uint instance : SV_InstanceID) {
  float4 patch = patches[instance];
  float2 p0 = patch.xy + grid * patch.z;
  if (params.w < 0.5) p0 = min(p0, mapSize.xy);
  float h0 = HeightBilinear(p0);
  // slide the odd vertices onto their even neighbours as this level's range runs out
  float4 m = morph[int(patch.w)];
  float k = saturate((distance(float3(p0.x, h0, p0.y), cameraPosition) - m.x) * m.y);
  float2 odd = frac(grid * 0.5) * 2.0;
  float2 p = patch.xy + (grid - odd * k) * patch.z;
  if (params.w < 0.5) p = min(p, mapSize.xy);
  VOut o;
  o.world = float3(p.x, HeightBilinear(p), p.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

// A layer's normal map as read: x, y, z from red, green, blue (from 0..1 to -1..1). A two-channel one (BC5,
// whose blue reads 0, which no three-channel normal map has: its z would point down) gets z from x and y.
// `up`: a remastered material's normal (two channels, +y toward the top of its texture): its y turned to the stock
// layers' way
float4 LayerNormal(float4 raw, float up) {
  float4 n = raw * 2 - 1;
  if (raw.b < 0.002) n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
  if (up > 0.5) n.y = -n.y;
  return n;
}

#ifdef SURFACE_DEEP_TRACKS
// [mud ruts] The map's mud material (Terrain::mudRuts; the ground's pass binds them, kMudSlot): albedo, normal (two
// channels, +y up), and creases, roughness, height as the layers' third maps.
Texture2D mudAlbedo : register(t41);
Texture2D mudNormal : register(t42);
Texture2D mudMaterial : register(t43);
// Its three maps at a place, read twice and joined so that its repeats do not show: at its own size, and a third
// larger, turned and shifted; which of the two shows goes by patches of a slow noise, their edges by the mud's own
// height (its ridges come through first). `n`: its normal's x and z (the layers' way: along world x and z).
void MudRead(float2 xz, float2 stepX, float2 stepY, out float3 albedo, out float3 arh, out float2 n, out float lod) {
  float s = mudInfo.y;
  const float2x2 turn = float2x2(0.8253, -0.5646, 0.5646, 0.8253);  // (34 degrees)
  float2 uvA = xz * s, uvB = mul(turn, xz) * (s * 0.77) + float2(0.37, 0.71);
  float2 aX = stepX * s, aY = stepY * s, bX = mul(turn, stepX) * (s * 0.77), bY = mul(turn, stepY) * (s * 0.77);
  float3 albedoA = mudAlbedo.SampleGrad(samplerAniso, uvA, aX, aY).rgb, albedoB = mudAlbedo.SampleGrad(samplerAniso, uvB, bX, bY).rgb;
  float3 arhA = mudMaterial.SampleGrad(samplerAniso, uvA, aX, aY).rgb, arhB = mudMaterial.SampleGrad(samplerAniso, uvB, bX, bY).rgb;
  float2 nA = LayerNormal(mudNormal.SampleGrad(samplerAniso, uvA, aX, aY), 1).xy, nB = LayerNormal(mudNormal.SampleGrad(samplerAniso, uvB, bX, bY), 1).xy;
  nB = mul(nB, turn);  // (the turned reading's slopes turned back to the world's axes)
  float b = saturate((CloudNoise(float3(xz * (s * 0.35), 4.2)) - 0.5) * 3 + 0.5);
  float ha = arhA.b + (1 - b) * 1.5, hb = arhB.b + b * 1.5, top = max(ha, hb) - 0.3;
  float wa = max(ha - top, 0), wb = max(hb - top, 0), k = wb / max(wa + wb, 1e-4);
  albedo = lerp(albedoA, albedoB, k);
  arh = lerp(arhA, arhB, k);
  n = lerp(nA, nB, k);
  uint w, h, levels;
  mudNormal.GetDimensions(0, w, h, levels);
  lod = log2(max(max(length(aX), length(aY)) * float(w), 1));
}
// The mud in the ruts: where the ground is mud by its kind (`kind`, the layers' given share of it), in the troughs
// (`t`) and less on their berms (`q`), the map's mud in place of the pressed ground's smooth average, with its own
// small shapes, creases and roughness: when dry a quarter of it, in the hollows of its picture (dark dry mud, matt),
// when wet all of it (glossy; the ruts' standing water lies on it as before). Its albedo as the visuals session made
// it (wet mud: the soil's x 0.55 in linear light; the weather wets it further as all ground, lighting.h).
// Put in once DeepTracksOn has done all its own sums, so that where there is none they are done exactly as before:
// as if before its darkening by kind (`dark`, which `darkened` is the ground after), its wear (`worn`), the shade of
// its walls (`open`) and its relief (`tilt`), which it is given here as the pressed ground was there. Its roughness is
// kept through the weather's (lighting.h: deepGloss).
void MudInRuts(float2 xz, float2 stepX, float2 stepY, float3 base, float kind, float t, float q, float wet, float2 tilt, float3 dark,
               float3 darkened, float worn, float open, inout float3 albedo, inout float3 N, inout float roughness, inout float creases) {
  float want = kind * saturate(t * 1.6 + q * 0.5) * lerp(0.25, 1, wet);
  if (want < 0.01) return;
  float3 colour, arh;
  float2 n;
  float lod;
  MudRead(xz, stepX, stepY, colour, arh, n, lod);
  float share = saturate((want * 1.35 - arh.b) / 0.35);  // (its hollows first)
  if (share <= 0) return;
  float3 a = lerp(darkened, colour * dark, share);
  albedo = lerp(a, dot(a, float3(0.3, 0.6, 0.1)) * float3(1.25, 0.95, 0.68), worn * 0.8);  // (worn as DeepTracksOn wears)
  // (dry: matt; wet: its own, a little rougher than the scan says: at its full gloss its hollows glint in specks)
  float rough = lerp(0.7 + 0.5 * arh.g, lerp(arh.g, 0.6, 0.3), wet);
  rough = sqrt(rough * rough + 0.1 * saturate(lod / 4));  // (its small shapes a pixel takes in: rougher, no sparkle)
  roughness = lerp(roughness, rough, share);
  deepGloss = float2(rough, share);
  creases = lerp(creases, arh.r * open, share);
  // its normal as the layers' are put on the ground's (Ground below), with the trough's relief
  float3 detail = float3(n.x, sqrt(saturate(1 - dot(n, n))), n.y);
  float3 h = normalize(base + float3(0, 1, 0));
  float3 axisX = float3(1 - 2 * h.x * h.x, 2 * h.x * h.y, -2 * h.x * h.z);
  float3 axisZ = float3(-2 * h.z * h.x, 2 * h.z * h.y, 1 - 2 * h.z * h.z);
  float3 M = float3(dot(detail, axisX), dot(detail, base), dot(detail, axisZ));
  N = normalize(lerp(N, normalize(M - float3(tilt.x, 0, tilt.y)), share));
}
// Deep tracks (deep_tracks.h): a level of the field read at a place (r how much is pressed down, g pushed up),
// one bilinear reading; a wrapping level by the place's texel within its width (its copied edges make the
// reading across the wrap right), the map's level clamped.
float2 TrackTap(int level, float2 xz) {
  float4 L = tracksLevel[level], A = tracksAtlas[level];
  float2 s = xz * L.z;
  float2 c = A.z > 0.5 ? s - L.w * floor(s / L.w) : clamp(s, 0.5, L.w - 0.5);
  return tracksMap.SampleLevel(samplerLinearClamp, (A.xy + c) * tracksInfo.xy, 0).rg;
}
// How far inside a level's window a place is, in its texels (negative: outside).
float TrackInside(int level, float2 xz) {
  float4 L = tracksLevel[level];
  float2 s = xz * L.z - L.xy;
  return L.w > 0 ? min(min(s.x, s.y), min(L.w - s.x, L.w - s.y)) : -1;
}
// What the amounts come to: how deep the trough is (0..1, more passes deeper but never past the ground's
// most), and the berm beside it (none where it is pressed down).
float2 TrackShape(float2 pressed) {
  float t = 1 - exp(-1.5 * pressed.x);
  return float2(t, (1 - exp(-1.5 * pressed.y)) * (1 - t));
}
// A level's trough and berm round a place, and how they slope (per world unit: x, z of the trough's; of the
// berm's), from the readings a texel to either side.
void TrackLevel(int level, float2 xz, out float2 shape, out float4 slope) {
  float e = tracksAtlas[level].w;
  float2 a = TrackShape(TrackTap(level, xz + float2(e, 0))), b = TrackShape(TrackTap(level, xz - float2(e, 0)));
  float2 c = TrackShape(TrackTap(level, xz + float2(0, e))), d = TrackShape(TrackTap(level, xz - float2(0, e)));
  shape = (a + b + c + d) * 0.25;
  slope = float4(a.x - b.x, c.x - d.x, a.y - b.y, c.y - d.y) / (2 * e);
}
// The layers' average colour here (each layer's smallest level, blended as the layers are; the upper layer as it is
// here): what the ground pressed smooth comes to, its small shapes pressed out of it.
float3 LayersAverage(float4 low, float4 high, float4 upper) {
  #define AVERAGE(n) albedoMaps[n].SampleLevel(samplerLinear, float2(0.5, 0.5), 16).rgb
  float3 a = AVERAGE(0);
  a = lerp(a, AVERAGE(1), low.x);
  a = lerp(a, AVERAGE(2), low.y);
  a = lerp(a, AVERAGE(3), low.z);
  a = lerp(a, AVERAGE(4), low.w);
  a = lerp(a, AVERAGE(5), high.x);
  a = lerp(a, AVERAGE(6), high.y);
  a = lerp(a, AVERAGE(7), high.z);
  a = lerp(a, AVERAGE(8), high.w);
  #undef AVERAGE
  return lerp(a, upper.rgb, upper.a);
}
// The ground where it is pressed, by what kind of ground it is (its layers' kinds blended as their colours are):
//   snow (a layer of it): deep troughs, a berm of pushed snow, the bottom packed, greyer, bluer and smoother;
//   mud: dark wet ruts, sheen, water standing in them when the ground is wet (dirt turns to mud in the wet);
//   sand: soft ruts, the berm beside them lighter (dry, loose sand pushed up);
//   grass: pressed flat and crushed, darker and browner (the ground cover lies down along them, grass.cpp), the earth
//   showing where many pass;
//   dirt: compacted, darker, smooth, the loose earth pushed up beside; hard ground (rock, concrete): only scuffed.
// Down in a trough the ground is pressed smooth: its colour goes toward the layers' average and its own small
// shapes (the layers' normals) toward the ground's (`base`), so that a track reads as a smooth band in a pebbly or
// grassy ground; the trough's walls keep part of the sky from its bottom (`creases`). Relief by the normal; it shows
// where the level's texels are not much smaller than a pixel and fades beyond (from far, only the map-wide level's
// darkening of the way trodden). The snow that lies by the weather is pressed in ModernLight (lighting.h), from what
// this leaves in deepTrack.
void DeepTracksOn(float2 xz, float2 stepX, float2 stepY, float4 low, float4 high, float4 upper, float3 base, inout float3 albedo, inout float3 N,
                  inout float roughness, inout float creases) {
  float pixel = max(length(stepX), length(stepY));  // (world units a pixel spans here)
  float in0 = TrackInside(0, xz), in1 = TrackInside(1, xz);
  float w0 = saturate((in0 - 8) / 96) * saturate((2.5 * tracksAtlas[0].w - pixel) / (1.5 * tracksAtlas[0].w));
  float w1 = saturate((in1 - 8) / 96) * saturate((2.5 * tracksAtlas[1].w - pixel) / (1.5 * tracksAtlas[1].w));
  float2 c0 = 0, c1 = 0;
  [branch] if (w0 > 0) c0 = TrackTap(0, xz);
  [branch] if (w0 < 1 && w1 > 0) c1 = TrackTap(1, xz);
  float2 c2 = TrackTap(2, xz);
  [branch] if (tracksInfo.w > 0.5 && tracksInfo.w < 1.5) {  // (a view for checking them: MOHO64_DEBUG_TRACKS=2)
    albedo = float3(saturate(lerp(c1.x, c0.x, w0)), saturate(lerp(c1.y, c0.y, w0)), w0 > 0 ? 0.6 : w1 > 0 ? 0.3 : 0) + float3(c2.x, 0, 0);
    return;
  }
  if (max(max(c0.x + c0.y, c1.x + c1.y), c2.x) < 0.004) return;  // (nothing pressed here: most of the ground)
  float2 shape = 0, s1 = 0;
  float4 slope = 0, g1 = 0;
  [branch] if (w0 > 0) TrackLevel(0, xz, shape, slope);
  [branch] if (w0 < 1 && w1 > 0) TrackLevel(1, xz, s1, g1);
  shape = lerp(s1, shape, w0);
  slope = lerp(g1, slope, w0);
  float raw = lerp(c1.x, c0.x, w0);              // (how often it has been pressed: heavy traffic)
  float relief = w0 + (1 - w0) * w1;             // (how much of the relief shows)
  float far = TrackShape(c2).x * 0.6;            // (the map's level: the way trodden, without relief)
  float t = lerp(far, shape.x, relief), q = shape.y * relief;
  // the kind of ground
  float4 kA = kindA[0], kB = kindB[0];
  #define KIND(n, w) { kA = lerp(kA, kindA[n], w); kB = lerp(kB, kindB[n], w); }
  KIND(1, low.x) KIND(2, low.y) KIND(3, low.z) KIND(4, low.w) KIND(5, high.x) KIND(6, high.y) KIND(7, high.z) KIND(8, high.w)
  #undef KIND
  float wet = saturate(weather.x);
  float snow = kA.x, mud = kA.y + kB.x * wet * 0.8, sand = kA.z, grass = kA.w, dirt = kB.x * (1 - wet * 0.8), hard = kB.y;
  float sum = max(snow + mud + sand + grass + dirt + hard, 1e-3);
  snow /= sum, mud /= sum, sand /= sum, grass /= sum, dirt /= sum, hard /= sum;
  // per kind: how deep a full trough is (world units: somewhat deeper than true to the units' size, so that it reads
  // from the game's height), how high its berm against that, how much of the ground's own small shapes and colour
  // the pressing smooths out, the colour down in it and on the berm, and how much its walls hide of the sky
  float depth = snow * 0.07 + mud * 0.05 + sand * 0.04 + grass * 0.03 + dirt * 0.03 + hard * 0.003;
  float berm = snow * 0.7 + mud * 0.6 + sand * 0.9 + grass * 0.25 + dirt * 0.45;
  float smooth = snow * 0.6 + mud * 0.8 + sand * 0.7 + grass * 0.35 + dirt * 0.75 + hard * 0.1;
  float3 down = snow * float3(0.86, 0.89, 0.95) + mud * float3(0.5, 0.47, 0.44) + sand * float3(0.86, 0.84, 0.8) + grass * float3(0.68, 0.64, 0.46) +
                dirt * float3(0.7, 0.67, 0.64) + hard * 0.93;
  float3 up = snow * 1.05 + mud * float3(0.86, 0.84, 0.82) + sand * 1.1 + grass * float3(0.92, 0.9, 0.82) + dirt * 1.08 + hard * 1.0;
  float shade = snow * 0.5 + mud * 0.45 + sand * 0.4 + grass * 0.35 + dirt * 0.35;
  // pressed smooth: its colour and small shapes toward the average and the ground's own slope
  float flat = smooth * saturate(t * 1.4) * relief;
  albedo = lerp(albedo, LayersAverage(low, high, upper), flat * 0.85);
  N = normalize(lerp(N, base, flat * 0.9));
  creases = lerp(creases, 1, flat * 0.8);
  // the trough and berm's relief, the colours down in it and up on the berm
  float2 g = depth * (-slope.xy + berm * slope.zw) * relief;
  N = normalize(N - float3(g.x, 0, g.y));
  float3 dark = lerp(1, down, t) * lerp(1, up, q);
  albedo *= dark;
  float3 darkened = albedo;  // [mud ruts] (for MudInRuts)
  // grass worn through to the earth where many have passed
  float worn = smoothstep(1.5, 5.0, raw) * relief * grass;
  float grey = dot(albedo, float3(0.3, 0.6, 0.1));
  albedo = lerp(albedo, grey * float3(1.25, 0.95, 0.68), worn * 0.8);
  // packed snow, flattened grass and wet mud smoother; water standing in the ruts of wet mud (taken as a puddle,
  // lighting.h)
  roughness = lerp(roughness, 0.45, t * snow);
  roughness = lerp(roughness, 0.6, t * grass * (1 - worn));
  roughness = lerp(roughness, lerp(0.5, 0.25, wet), t * mud);
  deepWater = smoothstep(0.4, 0.8, t) * saturate(mud * 1.5) * saturate(wet * 1.5 - 0.2);
  float open = 1 - shade * t * relief;
  creases *= open;
  // [mud ruts] where the ground is mud by its kind, the map's own mud in place of the pressed ground's smooth average
  // (MudInRuts: last, so that without it the sums above are as they were)
  [branch] if (mudInfo.x > 0.5 && kA.y > 0.01)
    MudInRuts(xz, stepX, stepY, base, saturate(kA.y), t, q, wet, g, dark, darkened, worn, open, albedo, N, roughness, creases);
  deepTrack = float4(t, q, relief, 1);
  deepSlope = slope * relief;
  if (tracksInfo.w > 1.5) albedo = float3(t, q, relief * 0.5);  // (MOHO64_DEBUG_TRACKS=3)
}
#endif

#ifdef GROUND_PARALLAX
// [parallax] The ground's relief close by (World::groundParallax; the modern look). A remastered layer's third map holds
// how high each point of its picture stands (its blue: the scan's relief stretched to 0..1, 0.5 its middle), and the
// picture was taken from straight above. Seen at a slant, a stone should hide what lies just behind it and a hollow
// sink away from the eye: the place whose picture a pixel shows is found by following the eye's ray down through that
// relief, and every layer's picture is read there (parallax occlusion mapping: even steps down the ray, then the
// crossing between the last two). The ground's own surface, its geometry, which the decals, the tracks' marks and the
// plants lie on, is the relief's middle: on the whole they stay where they lie on it.
//
// The relief of the one or two layers that show most at a pixel, `a` under `b`: each about its middle by its depth
// (world units from its lowest to its highest), b's share by its height as the layers meet (RICH, in Ground); and as
// the second of them gives way to a third (`pair` toward 0), the strongest one's alone, so that where they change places
// nothing jumps.
struct Relief {
  float tileA, tileB;    // repeats per world unit
  float deepA, deepB;    // depths, as faded
  float sliceA, sliceB;  // their layers of materialMaps
  float maskB;           // b's mask
  float richB;           // 1: b's share goes by its height
  float pair;            // how much the two show together (0: the strongest alone)
  float bestB;           // 1: b is the strongest
  float2 gx, gy;         // the place's steps (world units a pixel)
};
float ReliefAt(Relief r, float2 p) {
  float za = 0, zb = 0, hb = 0.5;
  [branch] if (r.pair > 0 || r.bestB < 0.5)
    za = (materialMaps.SampleGrad(samplerLinear, float3(p * r.tileA, r.sliceA), r.gx * r.tileA, r.gy * r.tileA).b - 0.5) * r.deepA;
  [branch] if (r.pair > 0 || r.bestB > 0.5) {
    hb = materialMaps.SampleGrad(samplerLinear, float3(p * r.tileB, r.sliceB), r.gx * r.tileB, r.gy * r.tileB).b;
    zb = (hb - 0.5) * r.deepB;
  }
  float share = r.richB > 0.5 ? saturate((hb + r.maskB * 1.35 - 1) / 0.35) : r.maskB;
  return lerp(r.bestB > 0.5 ? zb : za, lerp(za, zb, share), r.pair);
}
// Where the layers' pictures are read for the pixel at `world` (on the ground's surface, `base` its normal; `low`, `high`
// the layers' masks), and how much of the sun's light the relief's own high places leave it (`sunSeen`, parallax.w).
// Full within parallax.y of the eye, fading out over 1 / parallax.z beyond (past that Ground does not call it: from the
// game's usual height it costs nothing); nothing where the whole relief would cross under about a pixel.
float2 GroundParallax(float3 world, float3 base, float4 low, float4 high, float2 stepX, float2 stepY, out float sunSeen) {
  sunSeen = 1;
  // each layer's share of the pixel as the layers are laid over each other (before their heights push them), the two
  // that show most, and how far the second stands above the third
  float masks[9] = {1, low.x, low.y, low.z, low.w, high.x, high.y, high.z, high.w};
  float left = 1, best = 0, second = 0, third = 0, bestMask = 1, secondMask = 0;
  int a = 0, b = 0;
  [unroll] for (int n = 8; n >= 0; --n) {
    float w = masks[n] * left;
    left *= 1 - masks[n];
    if (w > best) {
      third = second;
      second = best, secondMask = bestMask, b = a;
      best = w, bestMask = masks[n], a = n;
    } else if (w > second) {
      third = second;
      second = w, secondMask = masks[n], b = n;
    } else {
      third = max(third, w);
    }
  }
  int lo = min(a, b), up = max(a, b);
  float pair = lo == up ? 0 : saturate((second - third) * 8);
  bool bestUp = a == up && lo != up;
  float deepLo = pair > 0 || !bestUp ? albedoTile[lo].z : 0, deepUp = pair > 0 || bestUp ? albedoTile[up].z : 0;
  float deep = max(deepLo, deepUp);
  if (deep <= 0) return world.xz;
  // the eye's ray: its way over the ground per unit of depth (in the ground's plane, the pictures' coordinates being
  // the world's x and z), and how many pixels it crosses the whole relief over
  float3 V = world - cameraPosition;
  float dist = length(V);
  V /= max(dist, 1e-4);
  float facing = max(-dot(V, base), 0.1);
  float2 along = (V.xz + base.xz * facing) / facing;
  float pixel = max(max(length(stepX), length(stepY)), 1e-6);
  float across = deep * length(along) / pixel;
  float fade = saturate((parallax.y - dist) * parallax.z + 1) * saturate(across - 0.5);
  [branch] if (fade <= 0) return world.xz;
  Relief r;
  r.tileA = albedoTile[lo].x, r.tileB = albedoTile[up].x;
  r.deepA = deepLo * fade, r.deepB = deepUp * fade;
  r.sliceA = lo, r.sliceB = up;
  r.maskB = a == up ? bestMask : secondMask;
  r.richB = albedoTile[up].y;
  r.pair = pair, r.bestB = bestUp ? 1 : 0;
  r.gx = stepX * 2, r.gy = stepY * 2;  // (the relief a level less sharp than the pictures: steadier, and no thin walls)
  // down the ray from where it enters the relief's top to where it leaves its bottom: the first step below the
  // relief (a step for about every two pixels the ray crosses, 4 to 24), then the crossing between it and the step
  // before, halved three times and taken between the last two by their heights over the relief (so that no slices
  // show where the ray runs along a steep side)
  float half = 0.5 * deep * fade;
  float2 from = world.xz - along * half, way = along * (2 * half);
  int steps = int(clamp(ceil(across * fade * 0.5), 4, 24));
  float t = 0, gap = half - ReliefAt(r, from), lastT = 0, lastGap = gap;
  [loop] for (int k = 1; k <= steps; ++k) {
    t = float(k) / steps;
    gap = half * (1 - 2 * t) - ReliefAt(r, from + way * t);
    if (gap <= 0) break;
    lastT = t, lastGap = gap;
  }
  [loop] for (int h = 0; h < 3 && gap < 0; ++h) {
    float middle = 0.5 * (lastT + t), g = half * (1 - 2 * middle) - ReliefAt(r, from + way * middle);
    if (g > 0) lastT = middle, lastGap = g;
    else t = middle, gap = g;
  }
  float hit = gap < 0 ? lerp(lastT, t, lastGap / max(lastGap - gap, 1e-6)) : t;
  float2 at = from + way * hit;
  // the sun's light there: up the sun's ray through the relief to its top, shaded by what stands over that ray (soft
  // by how far over, against the whole depth: so that it fades out with the relief)
  [branch] if (parallax.w > 0 && sunDirection.y > 0) {
    float rise = dot(sunDirection, base);
    [branch] if (rise > 0.02) {
      float2 toSun = (sunDirection.xz - base.xz * rise) / max(rise, 0.15);
      float z = half * (1 - 2 * hit), room = half - z, most = 0;
      int sunSteps = int(clamp(ceil(room * length(toSun) / pixel * 0.5), 2, 6));  // (a step for every two pixels)
      [loop] for (int j = 1; j <= sunSteps; ++j) {
        float climb = room * j / sunSteps;
        most = max(most, ReliefAt(r, at + toSun * climb) - (z + climb));
      }
      sunSeen = lerp(1, saturate(1 - most / (0.25 * deep)), parallax.w);
    }
  }
  return at;
}
#endif

float4 Ground(VOut i) {
  float2 xz = i.world.xz;
  float2 uv = MirrorUv(xz * mapSize.zw);
  // (in a mirrored copy of the map the stored slopes point the other way)
  float2 mirrored = 1 - 2 * abs(fmod(floor(xz * mapSize.zw), 2));
  float xp = params.y;
  float4 rawLow = maskLow.Sample(samplerLinearClamp, uv);
  float4 rawHigh = maskHigh.Sample(samplerLinearClamp, uv) * xp;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);

  // (read with the place's own steps, so that a layer can be skipped where it is not there at all:
  // inside such a branch the sampler cannot work the steps out itself; the temporal upscaler's bias, view.h,
  // as steps that much smaller)
  float2 stepX = ddx(xz) * exp2(temporal.x), stepY = ddy(xz) * exp2(temporal.x);
#ifdef GROUND_PARALLAX
  // [parallax] where the layers' pictures (and the deep tracks' field) are read: along the eye's ray into their relief,
  // close by (GroundParallax); with the same steps, so that their sharpness is the surface's
  float2 lxz = xz;
  float sunSeen = 1;
  [branch] if (look > 0.5 && parallax.x > 0.5 && distance(i.world, cameraPosition) < parallax.y + 1 / parallax.z) {
    float4 under = normalMap.SampleLevel(samplerLinearClamp, uv, 0);
    float3 surface;
    surface.xz = (under.ag * 2 - 1) * mirrored;
    surface.y = sqrt(saturate(1 - dot(surface.xz, surface.xz)));
    lxz = GroundParallax(i.world, surface, low, high, stepX, stepY, sunSeen);
  }
  #define LXZ lxz
  #define TRACKS_XZ lxz
#else
  #define LXZ xz
  #define TRACKS_XZ i.world.xz
#endif
  #define ALBEDO(n) albedoMaps[n].SampleGrad(samplerAniso, LXZ * albedoTile[n].x, stepX * albedoTile[n].x, stepY * albedoTile[n].x)
  #define NORMAL(n) LayerNormal(normalMaps[n].SampleGrad(samplerAniso, LXZ * normalTile[n].x, stepX * normalTile[n].x, stepY * normalTile[n].x), normalTile[n].y)
  // a layer laid over what is there only where it is there: a share of nothing changes nothing, and a
  // map's layers each cover a part of it only (most pixels need two or three of the ten)
  #define OVER(value, read, share) [branch] if ((share) > 0) value = lerp(value, read, share)
  // A remastered map's layers (modern look): where a layer has its third map, it does not fade
  // evenly into what lies under it: its high places come through first (how much of it shows is
  // its mask pushed by its height), and it brings its own roughness and crease shading.
  // `matter`: x creases' shading, y roughness, z how much of the pixel is such a layer's at all.
  float3 matter = 0;
  [branch] if (look > 0.5 && albedoTile[0].y > 0.5) {
    float3 m = materialMaps.SampleGrad(samplerAniso, float3(LXZ * albedoTile[0].x, 0), stepX * albedoTile[0].x, stepY * albedoTile[0].x).rgb;
    matter = float3(m.r, m.g, 1);
  }
  #define RICH(n, mask, raw) \
    [branch] if (look > 0.5 && albedoTile[n].y > 0.5 && raw > 0.002) { \
      float3 m = materialMaps.SampleGrad(samplerAniso, float3(LXZ * albedoTile[n].x, n), stepX * albedoTile[n].x, stepY * albedoTile[n].x).rgb; \
      mask = saturate((m.b + mask * 1.35 - 1) / 0.35); \
      raw = saturate((m.b + raw * 1.35 - 1) / 0.35); \
      matter = lerp(matter, float3(m.r, m.g, 1), mask); \
    } else { \
      matter = lerp(matter, 0, mask); \
    }
  RICH(1, low.x, rawLow.x)
  RICH(2, low.y, rawLow.y)
  RICH(3, low.z, rawLow.z)
  RICH(4, low.w, rawLow.w)
  RICH(5, high.x, rawHigh.x)
  RICH(6, high.y, rawHigh.y)
  RICH(7, high.z, rawHigh.z)
  RICH(8, high.w, rawHigh.w)
  float4 albedo = ALBEDO(0);
  OVER(albedo, ALBEDO(1), low.x);
  float4 second = ALBEDO(2);
  float3 glowing = 0;
  if (params.z > 0.5) {
    // the glowing look: the second stratum's picture goes round a circle of a hundredth of itself,
    // once in fifty seconds; how much it glows is its alpha, read with the circle's two parts
    // swapped (so the glow wanders against the picture), and it counts as fully shiny
    float2 drift;
    sincos(time * 0.125, drift.x, drift.y);
    drift *= 0.01;
    second = albedoMaps[2].Sample(samplerAniso, xz * albedoTile[2].x + drift);
    glowing = second.rgb * (albedoMaps[2].Sample(samplerAniso, xz * albedoTile[2].x + drift.yx).a * low.y);
    second.a = 0;
  }
  albedo = lerp(albedo, second, low.y);
  OVER(albedo, ALBEDO(3), low.z);
  OVER(albedo, ALBEDO(4), low.w);
  OVER(albedo, ALBEDO(5), high.x);
  OVER(albedo, ALBEDO(6), high.y);
  OVER(albedo, ALBEDO(7), high.z);
  OVER(albedo, ALBEDO(8), high.w);
  float4 upper = ALBEDO(9);
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);

  float4 n = NORMAL(0);
  OVER(n, NORMAL(1), rawLow.x);
  OVER(n, NORMAL(2), rawLow.y);
  OVER(n, NORMAL(3), rawLow.z);
  OVER(n, NORMAL(4), rawLow.w);
  OVER(n, NORMAL(5), rawHigh.x);
  OVER(n, NORMAL(6), rawHigh.y);
  OVER(n, NORMAL(7), rawHigh.z);
  OVER(n, NORMAL(8), rawHigh.w);
  n.xyz = normalize(n.xyz);
  // the layers' normal, y up
  float3 detail = float3(n.x, sqrt(saturate(1 - dot(n.xy, n.xy))), n.y);
  // the ground's normal: x in the texture's alpha, z in its green
  float4 stored = normalMap.Sample(samplerLinearClamp, uv);
  float3 base;
  base.xz = (stored.ag * 2 - 1) * mirrored;
  base.y = sqrt(saturate(1 - dot(base.xz, base.xz)));
  // the rotation that takes "up" to the ground's normal, applied to the layers' normal
  float3 h = normalize(base + float3(0, 1, 0));
  float3 axisX = float3(1 - 2 * h.x * h.x, 2 * h.x * h.y, -2 * h.x * h.z);
  float3 axisZ = float3(-2 * h.z * h.x, 2 * h.z * h.y, 1 - 2 * h.z * h.z);
  float3 N = float3(dot(detail, axisX), dot(detail, base), dot(detail, axisZ));

  // under water: tinted toward the ramp's colour by depth
  float depth = waterMap.Sample(samplerLinearClamp, uv).g;
  float4 ramp = waterRamp.Sample(samplerLinearClamp, float2(depth, 0.5));
  float shadow = SunShadow(i.world, base);
  // how shiny the ground is: the plain look keeps it in the albedo's alpha inverted, the XP look as it is
  float gloss = lerp(1 - albedo.a, albedo.a, xp);

  if (look > 0.5) {
    // the modern look: the ground is a rough, non-metal surface
    float roughness = 1 - 0.5 * gloss * saturate(specularColor.x * 2);
    // (a remastered map's layers say their own; the upper, map-wide layer has none)
    roughness = lerp(roughness, matter.y, matter.z);
    float creases = lerp(1, matter.x, matter.z);
#ifdef MODERN_WATER
    [branch] if (shore.x > 0.5) {
      // the beach the waves have just wetted: darker and shinier in a band over the water's level, its edge
      // uneven
      float uneven = (CloudNoise(float3(i.world.xz * 0.7, 2.5)) - 0.5) * shore.z * 0.8;
      float over = i.world.y - shore.y;
      float wet = saturate(1 - (over + uneven) / shore.z) * saturate(over * 20 + 1);  // (not under the water: its own colours)
      albedo.rgb *= lerp(1, 0.6, wet);
      roughness = lerp(roughness, 0.22, wet * 0.85);
    }
#endif
#ifdef SURFACE_DEEP_TRACKS
    [branch] if (tracksInfo.z > 0.5) {  // deep tracks: the ground pressed where units have gone
      float3 pressed = albedo.rgb;
      DeepTracksOn(TRACKS_XZ, stepX, stepY, low, high, upper, base, pressed, N, roughness, creases);
      albedo.rgb = pressed;
    }
#endif
    // (ground that glows gives its own light, by day and by night)
#ifdef GROUND_PARALLAX
    // [parallax] the relief's own high places' shadow; none where deep tracks have pressed the ground smooth
    shadow *= lerp(sunSeen, 1, saturate(deepTrack.x * 1.4) * deepTrack.z);
#endif
    float3 lit = ModernLight(ToLinear(albedo.rgb), 0, roughness, creases, ToLinear(glowing) * 2.5, N, i.world, shadow);
    // the under-water tint is mixed the way the original mixes it (in screen colours, relative to
    // full sunlight), so that shallows keep the map's water colours
#ifdef MODERN_WATER
    return float4(lit, 1);  // (the modern water colours what is under it itself, by how deep it is)
#endif
    float full = max(lightMultiplier * modern.x * (skyZenith.w > 0.5 ? skyGlow.w * lerp(1, 0.45, skyHorizon.w) : 1), 0.001);  // (a computed sky: as bright as the day is)
    float3 screen = lerp(pow(max(lit / full, 0), 1 / 2.2), ramp.rgb, ramp.a);
    return float4(ToLinear(screen) * full, 1);
  }

  float3 sun = sunDirection;
  float3 toPixel = normalize(i.world - cameraPosition);
  float sunDotN = dot(sun, N);
  float3 diffuse = sunColor * saturate(sunDotN) * shadow + sunAmbience;
  // the plain look: one specular value
  float specularPlain = pow(saturate(dot(sun - 2 * sunDotN * N, toPixel)), 80) * specularColor.x * gloss;
  float3 lightPlain = diffuse + specularPlain;
  lightPlain = lightMultiplier * lightPlain + shadowFill * (1 - lightPlain);
  float3 colorPlain = lightPlain * albedo.rgb;
  // the XP look: coloured specular added to the albedo
  float3 specularXp = pow(saturate(dot(reflect(toPixel, N), sun)), 80) * gloss * specularColor.a * specularColor.rgb;
  float3 lightXp = lightMultiplier * diffuse + shadowFill * (1 - diffuse);
  float3 colorXp = lightXp * (albedo.rgb + specularXp);
  // (the glowing look: the original marks the pixel to be spread as glow; here it is added)
  float3 color = lerp(colorPlain, colorXp, xp) + glowing * 0.6;
  // its glow, in the picture's alpha, for the original's bloom (render 28): TTerrain 0.01 plus its
  // specular times SpecularColor.w, the XP look 0.01, the glowing look its glow; under the water none
  // (the original's water lays alpha 0 under its plane)
  float glowAlpha = 0.01 + lerp(pow(saturate(dot(sun - 2 * sunDotN * N, toPixel)), 80) * gloss * specularColor.w, 0, xp) +
                    max(glowing.r, max(glowing.g, glowing.b));
  return float4(lerp(color, ramp.rgb, ramp.a), ramp.a > 0.001 ? 0 : saturate(glowAlpha));
}

float4 PS(VOut i) : SV_Target {
  float4 c = Ground(i);
  return float4(BeyondMap(c.rgb, i.world), c.a);
}
)hlsl";

// The ground's colour for the ray tracing scene (Terrain::GroundColours): the shader's blend of the layers, each
// by its texture's average (its smallest level), the upper one read at a level whose texels are about the
// picture's.
const char kGroundColours[] = R"hlsl(
cbuffer TerrainConstants : register(b1) {
  float4 mapSize;
  float4 params;
  float4 specularColor;
  float4 albedoTile[10];
};
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
Texture2D albedoMaps[10] : register(t6);
RWTexture2D<float4> colours : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  colours.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 xz = (id.xy + 0.5) / float2(w, h) * mapSize.xy;
  float2 uv = xz * mapSize.zw;
  float4 rawLow = maskLow.SampleLevel(samplerLinearClamp, uv, 0);
  float4 rawHigh = maskHigh.SampleLevel(samplerLinearClamp, uv, 0) * params.y;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);
  #define AVERAGE(n) albedoMaps[n].SampleLevel(samplerLinear, float2(0.5, 0.5), 16)
  // (and how grassy each layer is, by how green its average is against its red and blue: a meadow's grass, not
  // dirt, rock or sand; the ground cover reads it in the alpha)
  #define GRASSY(c) saturate((c.g - max(c.r, c.b)) / max(c.g, 0.05) * 14)
  float4 albedo = AVERAGE(0);
  float grassy = GRASSY(albedo);
  #define LAYER(n, w) { float4 a = AVERAGE(n); albedo = lerp(albedo, a, w); grassy = lerp(grassy, GRASSY(a), w); }
  LAYER(1, low.x);
  LAYER(2, low.y);
  LAYER(3, low.z);
  LAYER(4, low.w);
  LAYER(5, high.x);
  LAYER(6, high.y);
  LAYER(7, high.z);
  LAYER(8, high.w);
  uint tw, th, levels;
  albedoMaps[9].GetDimensions(0, tw, th, levels);
  float across = tw * albedoTile[9].x * mapSize.x / w;  // (the upper layer's texels over one of the picture's)
  float4 upper = albedoMaps[9].SampleLevel(samplerLinear, xz * albedoTile[9].x, max(log2(max(across, 1)), 0));
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);
  colours[id.xy] = float4(albedo.rgb, grassy);
}
)hlsl";

// Where the plants grow (Terrain::CoverDensity): the cover table's rows blended by the layers' masks as the
// shader blends the layers, then more or fewer near trees.
const char kCoverDensity[] = R"hlsl(
cbuffer CoverConstants : register(b1) {
  float4 mapSize;
  float4 params;
  float4 coverA[10];  // per layer: blades, clumps, flowers, dry stalks
  float4 coverB[10];  // per layer: ferns, dry cells' share, weeds' share, 1 where near trees counts
  float4 nearTrees;   // near a tree: blades, clumps, flowers, stalks times these
  float4 nearFern;    // x: and this many more ferns
};
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
Texture2D<float> trees : register(t4);
RWTexture2D<float4> outA : register(u0);
RWTexture2D<float4> outB : register(u1);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  outA.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 uv = (id.xy + 0.5) / float2(w, h);
  float4 rawLow = maskLow.SampleLevel(samplerLinearClamp, uv, 0);
  float4 rawHigh = maskHigh.SampleLevel(samplerLinearClamp, uv, 0) * params.y;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);
  float4 a = coverA[0], b = coverB[0];
  #define LAYER(n, w) { a = lerp(a, coverA[n], w); b = lerp(b, coverB[n], w); }
  LAYER(1, low.x);
  LAYER(2, low.y);
  LAYER(3, low.z);
  LAYER(4, low.w);
  LAYER(5, high.x);
  LAYER(6, high.y);
  LAYER(7, high.z);
  LAYER(8, high.w);
  float near = trees.SampleLevel(samplerLinearClamp, uv, 0) * b.w;
  a *= lerp(1, nearTrees, near);
  b.x += nearFern.x * near;
  outA[id.xy] = saturate(a);
  outB[id.xy] = float4(saturate(b.xyz), 0);
}
)hlsl";

struct CoverConstants {
  float mapSize[4], params[4];
  float coverA[10][4], coverB[10][4];
  float nearTrees[4], nearFern[4];
};

struct TerrainConstants {
  float mapSize[4];
  float params[4];
  float specularColor[4];
  float albedoTile[10][4];
  float normalTile[9][4];
  float morph[12][4];
  TrackFieldConstants tracks;  // (deep tracks)
  float kindA[10][4], kindB[10][4];
  float mudInfo[4];  // [mud ruts]
  float parallax[4];  // [parallax] (read only by the ground's parallax pipeline)
};

// The ground's shader as the scene draws it. [parallax] `parallax`: with the relief close by (GROUND_PARALLAX), the
// ground's parallax pipeline's; without it the source is as it always was.
std::string GroundSource(bool parallax) {
  // (the modern look has its own water: water.cpp)
  // (deep tracks: what the ground's shader read of them, for the lying snow and the puddles in ModernLight)
  return std::string(parallax ? "#define GROUND_PARALLAX 1\n" : "") + "#define MODERN_WATER 1\n" +
         "#define SURFACE_PUDDLES 1.0\n#define SURFACE_TRACKS 1\n" +
         "#define SURFACE_DEEP_TRACKS 1\nstatic float4 deepTrack = 0;\nstatic float4 deepSlope = 0;\nstatic float deepWater = 0;\n" +
         "static float2 deepGloss = 0;\n" +  // [mud ruts] (the mud's roughness and share, MudInRuts)
         kViewShaderBlock + RayShaders() + kLightingShaderBlock + kShader;
}

// Where the kinds of ground lie (Terrain::GroundKinds): the layers' kinds blended by their masks as the shader
// blends the layers.
const char kGroundKinds[] = R"hlsl(
cbuffer KindConstants : register(b1) {
  float4 mapSize;
  float4 params;
  float4 kindA[10];  // per layer: snow, mud, sand, grass
  float4 kindB[10];  // dirt, hard
};
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
RWTexture2D<float4> kinds : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  kinds.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 uv = (id.xy + 0.5) / float2(w, h);
  float4 low = saturate(maskLow.SampleLevel(samplerLinearClamp, uv, 0) * 2 - 1);
  float4 high = saturate(maskHigh.SampleLevel(samplerLinearClamp, uv, 0) * params.y * 2 - 1);
  float4 a = kindA[0], b = kindB[0];
  #define LAYER(n, w) { a = lerp(a, kindA[n], w); b = lerp(b, kindB[n], w); }
  LAYER(1, low.x);
  LAYER(2, low.y);
  LAYER(3, low.z);
  LAYER(4, low.w);
  LAYER(5, high.x);
  LAYER(6, high.y);
  LAYER(7, high.z);
  LAYER(8, high.w);
  kinds[id.xy] = saturate(float4(a.z, a.w, a.x, b.y));  // sand, grass, snow, hard
}
)hlsl";

struct KindConstants {
  float mapSize[4], params[4];
  float kindA[10][4], kindB[10][4];
};

// What kind of ground a layer is by its name ("/env/evergreen2/layers/evgrass005_albedo.dds", "sand_wet"): shares
// of snow, mud, sand, grass, dirt, hard ground. `kind`, where given (remap.json's "ground", the viewer's
// --ground-kind): one of those words, or two with a plus ("sand+mud": half each).
void GroundKindOf(const std::string& texture, const std::string& kind, float out[6]) {
  std::fill(out, out + 6, 0.0f);
  static const char* const kNames[6] = {"snow", "mud", "sand", "grass", "dirt", "hard"};
  if (!kind.empty()) {
    int n = 0;
    float given[6] = {};
    size_t at = 0;
    while (at <= kind.size()) {
      const size_t plus = kind.find('+', at);
      const std::string word = kind.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
      for (int k = 0; k < 6; ++k)
        if (word == kNames[k]) given[k] += 1, ++n;
      if (plus == std::string::npos) break;
      at = plus + 1;
    }
    if (n > 0) {
      for (int k = 0; k < 6; ++k) out[k] = given[k] / static_cast<float>(n);
      return;
    }
  }
  std::string name = texture;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  name = name.substr(name.rfind('/') + 1);
  auto has = [&](std::initializer_list<const char*> words) {
    for (const char* w : words)
      if (name.find(w) != std::string::npos) return true;
    return false;
  };
  enum { Snow, Mud, Sand, Grass, Dirt, Hard };
  if (has({"snow"})) out[Snow] = 1;
  else if (has({"ice"})) out[Snow] = 0.4f, out[Hard] = 0.6f;
  else if (has({"mud", "swamp", "bog", "marsh"})) out[Mud] = 1;
  else if (has({"sand", "beach", "dune", "desert"})) {
    if (has({"wet"})) out[Sand] = 0.5f, out[Mud] = 0.5f;  // (wet sand: firm, darker ruts that hold water)
    else out[Sand] = 1;
  } else if (has({"grass", "veg", "meadow", "moss", "lawn", "field"})) {
    if (has({"dry"})) out[Grass] = 0.8f, out[Dirt] = 0.2f;
    else out[Grass] = 1;
  } else if (has({"gravel", "pebble", "scree", "shingle"})) out[Dirt] = 0.3f, out[Hard] = 0.7f;
  else if (has({"rock", "cliff", "stone", "concrete", "tarmac", "crystal", "lava", "cracked", "ribbon", "metal", "plate", "macro"}))
    out[Hard] = 1;
  else out[Dirt] = 1;  // (dirt, soil, earth, a forest's floor, a path; and what the name does not tell)
}

// [mud ruts] A two-channel (BC5) normal map's mean x and y (0..1), from its 4 x 4 level (one block): a normal map's mean
// is near 0.5, 0.5 (its small shapes lean every way alike). False: not a BC5 file with that level.
bool NormalMapMean(const std::string& file, float* x, float* y) {
  if (file.size() < 128 || file.compare(0, 4, "DDS ") != 0) return false;
  auto word = [&](size_t at) {
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(static_cast<uint8_t>(file[at + static_cast<size_t>(k)])) << (8 * k);
    return v;
  };
  const uint32_t height = word(12), width = word(16), mips = std::max(word(28), 1u), fourCC = word(84);
  size_t at = 128;
  if (fourCC == 0x30315844u) {  // "DX10"
    if (file.size() < 148 || word(128) != 83) return false;
    at = 148;
  } else if (fourCC != 0x32495441u && fourCC != 0x55354342u) {  // "ATI2", "BC5U"
    return false;
  }
  uint32_t w = width, h = height, level = 0;
  while ((w > 4 || h > 4) && level + 1 < mips) {
    at += static_cast<size_t>(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    w = std::max(w / 2, 1u), h = std::max(h / 2, 1u), ++level;
  }
  if (w > 4 || h > 4 || at + 16 > file.size()) return false;
  float mean[2];
  for (int c = 0; c < 2; ++c) {  // (the block's two halves: red, then green, each as a BC4 block)
    const uint8_t* b = reinterpret_cast<const uint8_t*>(file.data() + at + 8 * static_cast<size_t>(c));
    float v[8];
    v[0] = b[0] / 255.0f, v[1] = b[1] / 255.0f;
    if (b[0] > b[1]) {
      for (int k = 1; k < 7; ++k) v[k + 1] = ((7 - k) * v[0] + k * v[1]) / 7;
    } else {
      for (int k = 1; k < 5; ++k) v[k + 1] = ((5 - k) * v[0] + k * v[1]) / 5;
      v[6] = 0, v[7] = 1;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
    float sum = 0;
    int n = 0;
    for (uint32_t p = 0; p < 16; ++p)
      if (p % 4 < w && p / 4 < h) sum += v[(bits >> (3 * p)) & 7], ++n;
    mean[c] = sum / static_cast<float>(std::max(n, 1));
  }
  *x = mean[0], *y = mean[1];
  return true;
}

// Is any point of the box within `radius` of `center`?
bool SphereTouchesBox(Vec3 center, float radius, Vec3 lo, Vec3 hi) {
  float dx = std::max({lo.x - center.x, 0.0f, center.x - hi.x});
  float dy = std::max({lo.y - center.y, 0.0f, center.y - hi.y});
  float dz = std::max({lo.z - center.z, 0.0f, center.z - hi.z});
  return dx * dx + dy * dy + dz * dz <= radius * radius;
}

}  // namespace

int Terrain::MaterialLayers() const {
  int n = 0;
  for (bool has : material_) n += has ? 1 : 0;
  return n;
}

gfx::Texture Terrain::GroundColours() {
  if (groundColours_ || !map_ || map_->width <= 0 || map_->height <= 0 || !maskLow_) return groundColours_;
  if (!groundColoursPass_) {
    const std::string source = std::string(kViewShaderBlock) + kGroundColours;
    gfx::ComputeDesc cd;
    cd.name = "terrain: the ground's colour for the rays";
    cd.source = source;
    std::string error;
    groundColoursPass_ = device_->CreateCompute(cd, &error);
    if (!groundColoursPass_) {
      fprintf(stderr, "render: %s\n", error.c_str());
      return {};
    }
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = std::max(map_->width / 2, 1);
  td.height = std::max(map_->height / 2, 1);
  td.format = gfx::Format::RGBA8;
  groundColours_ = device_->CreateTarget(td);
  if (!groundColours_) return {};
  const MapData& m = *map_;
  TerrainConstants c = {};
  c.mapSize[0] = static_cast<float>(m.width);
  c.mapSize[1] = static_cast<float>(m.height);
  c.mapSize[2] = 1.0f / static_cast<float>(m.width);
  c.mapSize[3] = 1.0f / static_cast<float>(m.height);
  c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
  for (int i = 0; i < 10; ++i) c.albedoTile[i][0] = albedoScale_[i] > 0 ? 1.0f / albedoScale_[i] : 0;
  device_->SetPipeline(groundColoursPass_);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(2, maskLow_);
  device_->SetTexture(3, maskHigh_);
  for (int i = 0; i < 10; ++i) device_->SetTexture(6 + i, albedo_[i]);
  device_->SetStorage(0, groundColours_);
  device_->Dispatch(static_cast<uint32_t>((td.width + 7) / 8), static_cast<uint32_t>((td.height + 7) / 8), 1);
  device_->SetStorage(0, {});
  for (int i = 0; i < 16; ++i) device_->SetTexture(i, {});
  return groundColours_;
}

gfx::Texture Terrain::GroundKinds() {
  if (kinds_ || !map_ || map_->width <= 0 || map_->height <= 0 || !maskLow_) return kinds_;
  if (!kindsPass_) {
    const std::string source = std::string(kViewShaderBlock) + kGroundKinds;
    gfx::ComputeDesc cd;
    cd.name = "terrain: the kinds of ground";
    cd.source = source;
    std::string error;
    kindsPass_ = device_->CreateCompute(cd, &error);
    if (!kindsPass_) {
      fprintf(stderr, "render: %s\n", error.c_str());
      return {};
    }
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = std::max(map_->width / 2, 1);
  td.height = std::max(map_->height / 2, 1);
  td.format = gfx::Format::RGBA8;
  kinds_ = device_->CreateTarget(td);
  if (!kinds_) return {};
  KindConstants c = {};
  c.mapSize[0] = static_cast<float>(map_->width);
  c.mapSize[1] = static_cast<float>(map_->height);
  c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
  for (int i = 0; i < 10; ++i) {
    std::copy(groundKind_[i], groundKind_[i] + 4, c.kindA[i]);
    std::copy(groundKind_[i] + 4, groundKind_[i] + 6, c.kindB[i]);
  }
  device_->SetPipeline(kindsPass_);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(2, maskLow_);
  device_->SetTexture(3, maskHigh_);
  device_->SetStorage(0, kinds_);
  device_->Dispatch(static_cast<uint32_t>((td.width + 7) / 8), static_cast<uint32_t>((td.height + 7) / 8), 1);
  device_->SetStorage(0, {});
  device_->SetTexture(2, {});
  device_->SetTexture(3, {});
  return kinds_;
}

// The visuals track's remastered ground (remasters.h, kRemasteredTerrain): where its remap.json lists this map and a
// layer's albedo, the layer takes the material's albedo, normal (+y up: normalUp_) and ARH (creases, roughness,
// height: its path into `arh`), at the map's own scale for the layer times scale_mul, or at "scale".
void Terrain::Remaster(const MapData& m, std::vector<std::string>* arh) {
  remasteredLayers_ = 0;
  seasonLook_ = SeasonLook{};  // [seasons] (read again below, where the map's entry has a block for its season)
  seasonLook_.season = season;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string folder = kRemasteredTerrain;
  auto file = textures_->ReadFile(folder + "/remap.json");
  if (!file) return;
  JsonValue remap;
  std::string why;
  if (!ReadJson(*file, &remap, &why)) {
    fprintf(stderr, "render: the remastered ground's remap.json cannot be read: %s\n", why.c_str());
    return;
  }
  const JsonValue& layers = remap["maps"][lower(mapPath)];
  if (layers.kind != JsonValue::Object) return;
  mapDecalsGiven_ = layers["decals"].StringOr(std::string());  // (which of the map's own decals are drawn, where it says)
  seasonLook_.Read(layers, season);  // [seasons] (nothing for summer, or where the entry has no block for the season)
  // the plants' card atlas and table, where the map's entry names one ([seasons] the season's, where it names its own)
  const std::string coverPath = seasonLook_.cover.empty() ? layers["cover"].StringOr(std::string()) : seasonLook_.cover;
  if (!coverPath.empty()) ReadCover(m, folder + coverPath);
  const char* tier = ultraGround ? "8k" : "4k";
  // [seasons] the tier a season's material is read at: the one asked for, or its 4k where its "tiers" lack that one
  auto seasonTier = [&](const std::string& name) -> const char* {
    const JsonValue& tiers = remap["materials"][name]["tiers"];
    if (tiers.kind != JsonValue::Array) return tier;
    bool wanted = false, four = false;
    for (const JsonValue& t : tiers.items) {
      wanted = wanted || t.StringOr(std::string()) == tier;
      four = four || t.StringOr(std::string()) == "4k";
    }
    return wanted || !four ? tier : "4k";
  };
  for (int i = 0; i < 10; ++i) {
    const JsonValue& entry = layers[lower(m.albedo[i].texture)];
    std::string name = entry["material"].StringOr(std::string());
    const char* tierOf = tier;
    if (const std::string own = seasonLook_.Material(name); !own.empty()) {  // [seasons] the season's material for this one
      if (remap["materials"][own]["dir"].StringOr(std::string()).empty()) {
        fprintf(stderr, "render: the %s material %s is not among remap.json's materials: %s kept\n", SeasonName(season), own.c_str(), name.c_str());
      } else {
        name = own;
        tierOf = seasonTier(own);
      }
    }
    const std::string dir = remap["materials"][name]["dir"].StringOr(std::string());
    if (name.empty() || dir.empty()) continue;
    groundGiven_[i] = entry["ground"].StringOr(groundGiven_[i]);  // (deep tracks: the layer's kind of ground, where it says)
    std::string base = folder + dir + "/" + name + "_";
    if (!textures_->ReadFile(base + "albedo_" + tierOf + ".dds")) {
      fprintf(stderr, "render: the remastered ground's %s has no %s set\n", name.c_str(), tierOf);
      // [seasons] a season's material: its 4k set, else summer's material as it is
      const std::string summer = entry["material"].StringOr(std::string());
      const std::string summerDir = remap["materials"][summer]["dir"].StringOr(std::string());
      if (name == summer || summerDir.empty()) continue;
      if (std::strcmp(tierOf, "4k") != 0 && textures_->ReadFile(base + "albedo_4k.dds")) {
        tierOf = "4k";
      } else {
        name = summer;
        tierOf = tier;
        base = folder + summerDir + "/" + name + "_";
        if (!textures_->ReadFile(base + "albedo_" + tier + ".dds")) continue;
      }
    }
    if (name != entry["material"].StringOr(std::string()))  // [seasons]
      printf("render: the ground's layer %d in its %s material %s (%s)\n", i, SeasonName(season), name.c_str(), tierOf);
    albedo_[i] = textures_->Get(base + "albedo_" + tierOf + ".dds", albedo_[i]);
    const double scale = entry["scale"].NumberOr(0), mul = entry["scale_mul"].NumberOr(1);
    albedoScale_[i] = scale > 0 ? static_cast<float>(scale) : m.albedo[i].scale * static_cast<float>(mul);
    const double metres = remap["materials"][name]["real_size_m"].NumberOr(0);  // [mud ruts]
    if (metres > 0) layerUnitsPerMetre_[i] = albedoScale_[i] / static_cast<float>(metres);
    if (i < 9) {
      normal_[i] = textures_->Get(base + "normal_" + tierOf + ".dds", normal_[i]);
      normalScale_[i] = albedoScale_[i];  // (the material's normal lies on its own albedo)
      normalUp_[i] = true;
      (*arh)[static_cast<size_t>(i)] = base + "arh_" + tierOf + ".dds";
    }
    ++remasteredLayers_;
  }
  if (remasteredLayers_) printf("render: %d of the map's ground layers remastered (%s)\n", remasteredLayers_, tier);
  {  // [mud ruts] deep tracks' mud: the material the map's entry names as "mud", else "<first word>_mud" by the first
     // word of its layers' materials ("setonsr_soil": setonsr_mud), where remap.json has one of that name
    std::string mud = layers["mud"].StringOr(std::string());
    const char* mudTier = tier;
    if (!seasonLook_.mud.empty() && !remap["materials"][seasonLook_.mud]["dir"].StringOr(std::string()).empty()) {  // [seasons] the season's own
      mud = seasonLook_.mud;
      mudTier = seasonTier(mud);
    }
    for (int i = 0; i < 10 && mud.empty(); ++i) {
      const std::string name = layers[lower(m.albedo[i].texture)]["material"].StringOr(std::string());
      const size_t cut = name.find('_');
      if (cut != std::string::npos && remap["materials"][name.substr(0, cut) + "_mud"].kind == JsonValue::Object) mud = name.substr(0, cut) + "_mud";
    }
    const JsonValue& material = remap["materials"][mud];
    const std::string dir = material["dir"].StringOr(std::string());
    if (!mud.empty() && !dir.empty()) {
      const std::string base = folder + dir + "/" + mud + "_";
      mudFiles_[0] = base + "albedo_" + mudTier + ".dds";
      mudFiles_[1] = base + "normal_" + mudTier + ".dds";
      mudFiles_[2] = base + "arh_" + mudTier + ".dds";
      mudMetres_ = static_cast<float>(material["real_size_m"].NumberOr(0));
    }
  }
}

// [mud ruts] Deep tracks' mud for this map, once the layers' kinds are known: none unless a layer is mud by its kind; tiled
// at the material's real size by the world units a metre of the layers that are mud (their scale over their material's
// real size; else the middle of all the remastered layers').
void Terrain::FindMud() {
  mudSize_ = 0;
  mudTried_ = false;
  for (gfx::Texture& t : mud_) t = {};
  if (mudFiles_[0].empty() || mudMetres_ <= 0) return;
  bool any = false;
  float sum = 0;
  int count = 0;
  std::vector<float> all;
  for (int i = 0; i < 10; ++i) {
    any = any || groundKind_[i][1] > 0;
    if (layerUnitsPerMetre_[i] <= 0) continue;
    all.push_back(layerUnitsPerMetre_[i]);
    if (groundKind_[i][1] > 0) sum += layerUnitsPerMetre_[i], ++count;
  }
  if (!any || all.empty()) return;
  std::sort(all.begin(), all.end());
  const float perMetre = count ? sum / static_cast<float>(count) : all[all.size() / 2];
  mudSize_ = mudMetres_ * perMetre;
  printf("render: deep tracks' mud in the ruts: %s, a repeat every %.2f world units (%.2f m)\n", mudFiles_[0].c_str(), mudSize_, mudMetres_);
}

void Terrain::ReadCover(const MapData& m, const std::string& path) {
  auto file = textures_->ReadFile(path);
  std::string why;
  if (!file || !ReadJson(*file, &cover_, &why)) {
    fprintf(stderr, "render: the ground cover's %s cannot be read%s%s\n", path.c_str(), why.empty() ? "" : ": ", why.c_str());
    return;
  }
  const JsonValue& density = cover_["density"];
  const JsonValue& table = density["layers"];
  if (table.kind != JsonValue::Object) {
    fprintf(stderr, "render: the ground cover's %s has no density table\n", path.c_str());
    return;
  }
  const double weedShare = density["clumps_from"]["weed"].NumberOr(0.3);
  const JsonValue& near = density["near_trees"];
  coverTreeRadius_ = static_cast<float>(near["radius_units"].NumberOr(6));
  coverNear_[0] = static_cast<float>(near["blades"].NumberOr(1));
  coverNear_[1] = static_cast<float>(near["clumps"].NumberOr(1));
  coverNear_[2] = static_cast<float>(near["flowers"].NumberOr(1));
  coverNear_[3] = static_cast<float>(near["stalks"].NumberOr(1));
  coverFern_ = static_cast<float>(near["fern_add"].NumberOr(0));
  int known = 0;
  for (int i = 0; i < 9; ++i) {  // (each layer by its name: "/maps/x/env/layers/<name>_albedo.dds")
    std::string name = m.albedo[i].texture;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    name = name.substr(name.rfind('/') + 1);
    const size_t tail = name.rfind("_albedo.dds");
    if (tail != std::string::npos) name = name.substr(0, tail);
    const JsonValue& row = table[name];
    if (row.kind != JsonValue::Object) continue;
    ++known;
    bool treesApply = true;
    for (const JsonValue& not_on : near["not_on"].items) treesApply = treesApply && not_on.StringOr(std::string()) != name;
    const double dry = row["dry_only"].kind == JsonValue::Bool && row["dry_only"].boolean ? 1.0 : row["dry_share"].NumberOr(0);
    const float a[4] = {static_cast<float>(row["blades"].NumberOr(0)), static_cast<float>(row["clumps"].NumberOr(0)),
                        static_cast<float>(row["flowers"].NumberOr(0)), static_cast<float>(row["stalks"].NumberOr(0))};
    const float b[4] = {static_cast<float>(row["fern"].NumberOr(0)), static_cast<float>(dry),
                        static_cast<float>(row["clumps_from"]["weed"].NumberOr(weedShare)), treesApply ? 1.0f : 0.0f};
    std::copy(a, a + 4, coverA_[i]);
    std::copy(b, b + 4, coverB_[i]);
  }
  hasCover_ = true;
  printf("render: the ground cover's plants for %d of the map's layers (%s)\n", known, path.c_str());
}

bool Terrain::CoverDensity(gfx::Texture trees, gfx::Texture* a, gfx::Texture* b) {
  if (!hasCover_ || !map_ || !maskLow_) return false;
  if (!coverA_tex_) {
    if (!coverPass_) {
      gfx::ComputeDesc cd;
      cd.name = "terrain: where the plants grow";
      const std::string source = std::string(kViewShaderBlock) + kCoverDensity;
      cd.source = source;
      std::string error;
      coverPass_ = device_->CreateCompute(cd, &error);
      if (!coverPass_) {
        fprintf(stderr, "render: %s\n", error.c_str());
        hasCover_ = false;
        return false;
      }
    }
    gfx::TargetDesc td;
    td.storage = true;
    td.width = std::max(map_->width / 2, 1);
    td.height = std::max(map_->height / 2, 1);
    td.format = gfx::Format::RGBA8;
    coverA_tex_ = device_->CreateTarget(td);
    coverB_tex_ = device_->CreateTarget(td);
    if (!coverA_tex_ || !coverB_tex_) return false;
    CoverConstants c = {};
    c.mapSize[0] = static_cast<float>(map_->width);
    c.mapSize[1] = static_cast<float>(map_->height);
    c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
    std::copy(&coverA_[0][0], &coverA_[0][0] + 40, &c.coverA[0][0]);
    std::copy(&coverB_[0][0], &coverB_[0][0] + 40, &c.coverB[0][0]);
    std::copy(coverNear_, coverNear_ + 4, c.nearTrees);
    c.nearFern[0] = coverFern_;
    device_->SetPipeline(coverPass_);
    device_->SetConstants(1, &c, sizeof c);
    device_->SetTexture(2, maskLow_);
    device_->SetTexture(3, maskHigh_);
    device_->SetTexture(4, trees ? trees : textures_->Black());
    device_->SetStorage(0, coverA_tex_);
    device_->SetStorage(1, coverB_tex_);
    device_->Dispatch(static_cast<uint32_t>((td.width + 7) / 8), static_cast<uint32_t>((td.height + 7) / 8), 1);
    device_->SetStorage(0, {});
    device_->SetStorage(1, {});
    for (int i = 2; i < 5; ++i) device_->SetTexture(i, {});
  }
  *a = coverA_tex_;
  *b = coverB_tex_;
  return true;
}

bool Terrain::Load(const MapData* map, std::string* error) {
  map_ = map;
  hasCover_ = false;  // (the map's cover read again by Remaster, its pictures made again when asked for)
  cover_ = JsonValue{};
  for (gfx::Texture* t : {&coverA_tex_, &coverB_tex_})
    if (*t) {
      device_->Destroy(*t);
      *t = {};
    }
  std::fill(&coverA_[0][0], &coverA_[0][0] + 40, 0.0f);
  std::fill(&coverB_[0][0], &coverB_[0][0] + 40, 0.0f);
  if (groundColours_) device_->Destroy(groundColours_);  // (GroundColours: made again for this map when asked for)
  groundColours_ = {};
  const int w = map->width, h = map->height;

  if (!pipeline_) {
    std::string source = GroundSource(false);
    gfx::PipelineDesc pd;
    pd.name = "terrain";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    // into a shadow map: the same vertices, depth only
    pd.name = "terrain, shadow";
    pd.pixelSource = {};
    pd.depthOnly = true;
    pd.samples = 1;
    pd.cull = gfx::Cull::None;
    shadowPipeline_ = device_->CreatePipeline(pd, error);
    if (!shadowPipeline_) return false;

    std::vector<float> vertices;
    for (int z = 0; z <= kPatchQuads; ++z)
      for (int x = 0; x <= kPatchQuads; ++x) {
        vertices.push_back(static_cast<float>(x));
        vertices.push_back(static_cast<float>(z));
      }
    std::vector<uint16_t> indices;
    const int row = kPatchQuads + 1;
    for (int z = 0; z < kPatchQuads; ++z)
      for (int x = 0; x < kPatchQuads; ++x) {
        uint16_t a = static_cast<uint16_t>(z * row + x), b = static_cast<uint16_t>(a + row);
        // counter-clockwise seen from above (x east, z south)
        for (uint16_t i : {a, b, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 1), b, static_cast<uint16_t>(b + 1)}) indices.push_back(i);
      }
    gfx::BufferDesc vb;
    vb.stride = 8;
    vb.size = static_cast<uint32_t>(vertices.size() * sizeof(float));
    vb.data = vertices.data();
    gridVertices_ = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 2;
    ib.size = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
    ib.data = indices.data();
    gridIndices_ = device_->CreateBuffer(ib);
    gridIndexCount_ = static_cast<uint32_t>(indices.size());
  }

  gfx::TextureDesc hd;
  hd.width = w + 1;
  hd.height = h + 1;
  hd.format = gfx::Format::R16;
  hd.data = map->heights.data();
  hd.dataSize = map->heights.size() * sizeof(uint16_t);
  heightTexture_ = device_->CreateTexture(hd);
  if (!heightTexture_) {
    if (error) *error = "cannot create the height texture";
    return false;
  }
  auto embedded = [&](const std::string& dds, const char* what, gfx::Texture fallback) {
    gfx::Texture t = dds.empty() ? gfx::Texture{} : textures_->FromDds(dds, what);
    return t ? t : fallback;
  };
  normalMap_ = embedded(map->normalMap, "the map's normal map", textures_->FlatNormal());
  maskLow_ = embedded(map->maskLow, "the map's first layer mask", textures_->Black());
  maskHigh_ = embedded(map->maskHigh, "the map's second layer mask", textures_->Black());
  waterMap_ = embedded(map->waterMap, "the map's water map", textures_->Black());
  waterRamp_ = map->water.present ? textures_->Get(map->water.ramp, textures_->Black()) : textures_->Black();
  for (int i = 0; i < 10; ++i) albedo_[i] = textures_->Get(map->albedo[i].texture, textures_->Black());
  for (int i = 0; i < 9; ++i) normal_[i] = textures_->Get(map->normal[i].texture, textures_->FlatNormal());
  for (int i = 0; i < 10; ++i) albedoScale_[i] = map->albedo[i].scale;
  for (int i = 0; i < 9; ++i) normalScale_[i] = map->normal[i].scale, normalUp_[i] = false;
  {  // a remastered map's third maps: beside each layer's `<name>_albedo.dds` a `<name>_arh.dds`
    std::vector<std::string> paths(9);
    for (size_t i = 0; i < 9; ++i) {
      std::string lower = map->albedo[i].texture;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t at = lower.rfind("_albedo.dds");
      if (at != std::string::npos) paths[i] = map->albedo[i].texture.substr(0, at) + "_arh.dds";
    }
    for (std::string& g : groundGiven_) g.clear();
    for (std::string& f : mudFiles_) f.clear();  // [mud ruts] (Remaster finds them again)
    mudMetres_ = 0;
    std::fill(layerUnitsPerMetre_, layerUnitsPerMetre_ + 10, 0.0f);
    mapDecalsGiven_.clear();
    Remaster(*map, &paths);  // (the visuals track's remastered ground, where it lists this map)
    std::vector<bool> present;
    materials_ = textures_->Layers(paths, &present);
    for (size_t i = 0; i < 9; ++i) material_[i] = materials_ && present[i];
    if (MaterialLayers()) printf("render: %d of the map's ground layers have a third map (creases, roughness, height)\n", MaterialLayers());
  }
  {  // deep tracks: what kind of ground each layer is (by remap.json's "ground", the viewer's --ground-kind, its name)
    for (int i = 0; i < 10; ++i) {
      std::string given = groundGiven_[i];
      std::string lower = map->albedo[i].texture;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      for (const auto& [part, kind] : groundKinds)
        if (!part.empty() && lower.find(part) != std::string::npos) given = kind;
      GroundKindOf(map->albedo[i].texture, given, groundKind_[i]);
    }
    if (kinds_) device_->Destroy(kinds_);  // (GroundKinds: made again for this map when asked for)
    kinds_ = {};
    FindMud();  // [mud ruts]
  }
  shaderKind_ = map->shader == "TTerrainXP" ? 1 : map->shader == "TTerrainGlow" ? 2 : 0;
  if (map->shader != "TTerrain" && map->shader != "TTerrainXP" && map->shader != "TTerrainGlow")
    fprintf(stderr, "render: the map asks for the terrain look \"%s\", which is not written yet; using TTerrain\n", map->shader.c_str());

  // the tree of nodes: per level, each node's lowest and highest ground (for culling and for distances)
  // (a node of the finest level holds the points [n * kLeafCells, (n + 1) * kLeafCells] each way;
  // each coarser node the four below it)
  levels_.clear();
  for (int k = 0; k < kMaxLevels; ++k) {
    Level level;
    level.nodeCells = kLeafCells << k;
    level.nx = (w + level.nodeCells - 1) / level.nodeCells;
    level.nz = (h + level.nodeCells - 1) / level.nodeCells;
    level.lo.assign(static_cast<size_t>(level.nx) * static_cast<size_t>(level.nz), std::numeric_limits<float>::max());
    level.hi.assign(level.lo.size(), std::numeric_limits<float>::lowest());
    if (k == 0) {
      for (int nz = 0; nz < level.nz; ++nz)
        for (int nx = 0; nx < level.nx; ++nx) {
          uint16_t lo = 65535, hi = 0;
          const int x1 = std::min(w, (nx + 1) * kLeafCells), z1 = std::min(h, (nz + 1) * kLeafCells);
          for (int z = nz * kLeafCells; z <= z1; ++z) {
            const uint16_t* p = map->heights.data() + static_cast<size_t>(z) * static_cast<size_t>(w + 1);
            for (int x = nx * kLeafCells; x <= x1; ++x) {
              lo = std::min(lo, p[x]);
              hi = std::max(hi, p[x]);
            }
          }
          size_t at = static_cast<size_t>(nz) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx);
          level.lo[at] = static_cast<float>(lo) * map->heightScale;
          level.hi[at] = static_cast<float>(hi) * map->heightScale;
        }
    } else {
      const Level& below = levels_.back();
      for (int nz = 0; nz < below.nz; ++nz)
        for (int nx = 0; nx < below.nx; ++nx) {
          size_t from = static_cast<size_t>(nz) * static_cast<size_t>(below.nx) + static_cast<size_t>(nx);
          size_t to = static_cast<size_t>(nz / 2) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx / 2);
          level.lo[to] = std::min(level.lo[to], below.lo[from]);
          level.hi[to] = std::max(level.hi[to], below.hi[from]);
        }
    }
    levels_.push_back(std::move(level));
    if (levels_.back().nx == 1 && levels_.back().nz == 1) break;
  }
  return true;
}

// Where a box of the map (x0..x1, z0..z1) lies in the world for the copy of the map being gone
// through (tileX_, tileZ_: 0, 0 the map itself; an odd one is mirrored).
void Terrain::InWorld(float x0, float z0, float x1, float z1, float lo, float hi, Vec3* a, Vec3* b) const {
  const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
  const float ox = static_cast<float>(tileX_) * w, oz = static_cast<float>(tileZ_) * h;
  const bool mx = (tileX_ & 1) != 0, mz = (tileZ_ & 1) != 0;
  *a = {ox + (mx ? w - x1 : x0), lo, oz + (mz ? h - z1 : z0)};
  *b = {ox + (mx ? w - x0 : x1), hi, oz + (mz ? h - z0 : z1)};
}

void Terrain::AddPatch(const View& view, int level, float x, float z, float size, float lo, float hi) {
  const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
  if (x >= w || z >= h) return;
  Vec3 a, b;
  InWorld(x, z, std::min(x + size, w), std::min(z + size, h), lo, hi, &a, &b);
  if (!view.BoxVisible(a, b)) return;
  if (tileX_ || tileZ_) {
    // (a copy of the map: only as far out as the ground is drawn at all)
    const float dx = std::max({-b.x, a.x - w, 0.0f}), dz = std::max({-b.z, a.z - h, 0.0f});
    if (dx * dx + dz * dz > beyondReach * beyondReach) return;
    // (the patch whole, not cut at the copy's edge: what laps over is the same ground again)
    InWorld(x, z, x + size, z + size, lo, hi, &a, &b);
  }
  patches_.push_back({a.x, a.z, size / kPatchQuads, static_cast<float>(level)});
}

void Terrain::AddNode(const View& view, int level, int ix, int iz) {
  const Level& L = levels_[static_cast<size_t>(level)];
  const float half = static_cast<float>(L.nodeCells) * 0.5f;
  const float x = static_cast<float>(ix * L.nodeCells), z = static_cast<float>(iz * L.nodeCells);
  const size_t at = static_cast<size_t>(iz) * static_cast<size_t>(L.nx) + static_cast<size_t>(ix);
  for (int q = 0; q < 4; ++q) AddPatch(view, level, x + half * static_cast<float>(q & 1), z + half * static_cast<float>(q >> 1), half, L.lo[at], L.hi[at]);
}

// Chooses what to draw for one node. False: the node is beyond this level's range, the caller draws
// its area at its own, coarser level.
bool Terrain::Select(const View& view, int level, int ix, int iz) {
  const Level& L = levels_[static_cast<size_t>(level)];
  const size_t at = static_cast<size_t>(iz) * static_cast<size_t>(L.nx) + static_cast<size_t>(ix);
  const float x = static_cast<float>(ix * L.nodeCells), z = static_cast<float>(iz * L.nodeCells);
  Vec3 lo, hi;
  InWorld(x, z, std::min(x + static_cast<float>(L.nodeCells), static_cast<float>(map_->width)),
          std::min(z + static_cast<float>(L.nodeCells), static_cast<float>(map_->height)), L.lo[at], L.hi[at], &lo, &hi);
  if (!SphereTouchesBox(view.eye, ranges_[static_cast<size_t>(level)], lo, hi)) return false;
  if (!view.BoxVisible(lo, hi)) return true;
  if (level == 0 || !SphereTouchesBox(view.eye, ranges_[static_cast<size_t>(level) - 1], lo, hi)) {
    AddNode(view, level, ix, iz);
    return true;
  }
  const Level& C = levels_[static_cast<size_t>(level) - 1];
  for (int q = 0; q < 4; ++q) {
    int cx = ix * 2 + (q & 1), cz = iz * 2 + (q >> 1);
    if (cx >= C.nx || cz >= C.nz) continue;
    if (!Select(view, level - 1, cx, cz)) {
      size_t cat = static_cast<size_t>(cz) * static_cast<size_t>(C.nx) + static_cast<size_t>(cx);
      AddPatch(view, level, static_cast<float>(cx * C.nodeCells), static_cast<float>(cz * C.nodeCells), static_cast<float>(C.nodeCells),
               C.lo[cat], C.hi[cat]);
    }
  }
  return true;
}

void Terrain::HeightsChanged(int x0, int z0, int x1, int z1) {
  if (!map_ || levels_.empty() || !heightTexture_) return;
  const int w = map_->width, h = map_->height;
  x0 = std::max(x0, 0), z0 = std::max(z0, 0), x1 = std::min(x1, w), z1 = std::min(z1, h);
  if (x0 > x1 || z0 > z1) return;
  // the height texture's rectangle, from the map's heights as they are now
  device_->UpdateTexture(heightTexture_, x0, z0, x1 - x0 + 1, z1 - z0 + 1,
                         map_->heights.data() + static_cast<size_t>(z0) * static_cast<size_t>(w + 1) + static_cast<size_t>(x0),
                         static_cast<uint32_t>((w + 1) * sizeof(uint16_t)));
  // the finest nodes those points are in (a point on a node's edge is in the nodes on both sides), anew
  Level& leaf = levels_[0];
  const int nx0 = std::max(0, (x0 - 1) / kLeafCells), nx1 = std::min(leaf.nx - 1, x1 / kLeafCells);
  const int nz0 = std::max(0, (z0 - 1) / kLeafCells), nz1 = std::min(leaf.nz - 1, z1 / kLeafCells);
  for (int nz = nz0; nz <= nz1; ++nz)
    for (int nx = nx0; nx <= nx1; ++nx) {
      uint16_t lo = 65535, hi = 0;
      const int px1 = std::min(w, (nx + 1) * kLeafCells), pz1 = std::min(h, (nz + 1) * kLeafCells);
      for (int z = nz * kLeafCells; z <= pz1; ++z) {
        const uint16_t* p = map_->heights.data() + static_cast<size_t>(z) * static_cast<size_t>(w + 1);
        for (int x = nx * kLeafCells; x <= px1; ++x) {
          lo = std::min(lo, p[x]);
          hi = std::max(hi, p[x]);
        }
      }
      const size_t at = static_cast<size_t>(nz) * static_cast<size_t>(leaf.nx) + static_cast<size_t>(nx);
      leaf.lo[at] = static_cast<float>(lo) * map_->heightScale;
      leaf.hi[at] = static_cast<float>(hi) * map_->heightScale;
    }
  // and the coarser nodes above them, each from the four below it
  int ax0 = nx0, ax1 = nx1, az0 = nz0, az1 = nz1;
  for (size_t k = 1; k < levels_.size(); ++k) {
    const Level& below = levels_[k - 1];
    Level& level = levels_[k];
    ax0 /= 2, ax1 /= 2, az0 /= 2, az1 /= 2;
    for (int nz = az0; nz <= std::min(az1, level.nz - 1); ++nz)
      for (int nx = ax0; nx <= std::min(ax1, level.nx - 1); ++nx) {
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (int cz = nz * 2; cz <= std::min(nz * 2 + 1, below.nz - 1); ++cz)
          for (int cx = nx * 2; cx <= std::min(nx * 2 + 1, below.nx - 1); ++cx) {
            const size_t from = static_cast<size_t>(cz) * static_cast<size_t>(below.nx) + static_cast<size_t>(cx);
            lo = std::min(lo, below.lo[from]);
            hi = std::max(hi, below.hi[from]);
          }
        const size_t to = static_cast<size_t>(nz) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx);
        level.lo[to] = lo;
        level.hi[to] = hi;
      }
  }
}

// [parallax] Layer i's relief: world units from its lowest to its highest point. A remastered layer's scan holds about 3 cm
// of relief, its height map stretched to 0..1 (the visuals session, 2026-10-19: 2 to 4 cm over scans of 1 to 3 m), which
// its world units a metre (remap.json's real_size_m over its repeat) turn into the world's; a layer whose real size is
// not given is taken as a 2 m scan over its repeat. At most a fiftieth of its repeat (a small scan stretched far, such
// as a third of a metre of sand over 8 world units, would otherwise stand a tenth of its width high: its relief is
// smaller than 3 cm) and a quarter of a world unit (the decals lie on the relief's middle). Half of it for grass (a grass
// scan's relief is mostly the blades lying over the turf, which this draws as solid ridges, nothing seen under them).
// Times parallaxDepth.
float Terrain::LayerDepth(int i) const {
  const float perMetre = layerUnitsPerMetre_[i] > 0 ? layerUnitsPerMetre_[i] : albedoScale_[i] / 2.0f;
  return std::min({0.03f * perMetre, 0.02f * albedoScale_[i], 0.25f}) * (1 - 0.5f * groundKind_[i][3]) * parallaxDepth;
}

// [parallax] The ground's pipeline with its relief close by (GROUND_PARALLAX), made the first time it is asked for. False:
// it cannot be made (the ground is drawn as without it).
bool Terrain::ParallaxPipeline() {
  if (parallaxPipeline_ || parallaxTried_) return static_cast<bool>(parallaxPipeline_);
  parallaxTried_ = true;
  const std::string source = GroundSource(true);
  gfx::PipelineDesc pd;
  pd.name = "terrain, parallax";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
  pd.stride = 8;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  std::string error;
  parallaxPipeline_ = device_->CreatePipeline(pd, &error);
  if (!parallaxPipeline_) {
    fprintf(stderr, "render: the ground's parallax cannot be drawn: %s\n", error.c_str());
    return false;
  }
  printf("render: the ground's parallax: the layers' relief (world units, lowest to highest):");
  for (int i = 0; i < 9; ++i)
    if (material_[i]) printf(" %d: %.3f", i, LayerDepth(i));
  printf("\n");
  return true;
}

void Terrain::Draw(const View& view, bool shadowPass) {
  if (!shadowPass) drawnPatches = drawnTriangles = 0;
  if (!map_ || levels_.empty()) return;

  // level k is used up to the distance where its quads (2^k cells) shrink to quadPixels on screen
  const float base = static_cast<float>(view.height) / (2 * std::tan(view.fovY * 0.5f) * std::max(1.0f, quadPixels));
  const int top = static_cast<int>(levels_.size()) - 1;
  ranges_.resize(levels_.size());
  for (int k = 0; k <= top; ++k) ranges_[static_cast<size_t>(k)] = k == top ? std::numeric_limits<float>::max() : base * static_cast<float>(1 << k);
  patches_.clear();
  const Level& T = levels_[static_cast<size_t>(top)];
  // the map, and (not into shadow maps) the eight copies of it round it that the ground beyond
  // its edge is made of
  const int round = beyondReach > 0 && !shadowPass ? 1 : 0;
  for (tileZ_ = -round; tileZ_ <= round; ++tileZ_)
    for (tileX_ = -round; tileX_ <= round; ++tileX_)
      for (int iz = 0; iz < T.nz; ++iz)
        for (int ix = 0; ix < T.nx; ++ix) Select(view, top, ix, iz);
  tileX_ = tileZ_ = 0;
  if (patches_.empty()) return;

  const MapData& m = *map_;
  TerrainConstants c = {};
  c.mapSize[0] = static_cast<float>(m.width);
  c.mapSize[1] = static_cast<float>(m.height);
  c.mapSize[2] = 1.0f / static_cast<float>(m.width);
  c.mapSize[3] = 1.0f / static_cast<float>(m.height);
  c.params[0] = 65535.0f * m.heightScale;
  c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
  c.params[2] = shaderKind_ == 2 ? 1.0f : 0.0f;
  c.params[3] = beyondReach > 0 ? 1.0f : 0.0f;
  c.specularColor[0] = m.lighting.specular.x;
  c.specularColor[1] = m.lighting.specular.y;
  c.specularColor[2] = m.lighting.specular.z;
  c.specularColor[3] = m.lighting.specular.w;
  for (int i = 0; i < 10; ++i) c.albedoTile[i][0] = albedoScale_[i] > 0 ? 1.0f / albedoScale_[i] : 0;
  for (int i = 0; i < 9; ++i) c.albedoTile[i][1] = richGround && material_[i] ? 1.0f : 0.0f;
  for (int i = 0; i < 9; ++i) c.normalTile[i][0] = normalScale_[i] > 0 ? 1.0f / normalScale_[i] : 0;
  for (int i = 0; i < 9; ++i) c.normalTile[i][1] = normalUp_[i] ? 1.0f : 0.0f;
  c.tracks = tracksField;  // (deep tracks)
  for (int i = 0; i < 10; ++i) {
    std::copy(groundKind_[i], groundKind_[i] + 4, c.kindA[i]);
    std::copy(groundKind_[i] + 4, groundKind_[i] + 6, c.kindB[i]);
  }
  // [mud ruts] deep tracks' mud, while deep tracks are drawn: its textures read the first time
  bool mud = false;
  if (!shadowPass && mudRuts && mudSize_ > 0 && tracksField.info[2] > 0.5f) {
    if (!mudTried_) {
      mudTried_ = true;
      for (int k = 0; k < 3; ++k) mud_[k] = textures_->Get(mudFiles_[k]);
      // (a normal map whose mean leans far one way has its channels wrong: drawn flat, the mud's small shapes left out)
      float mx = 0.5f, my = 0.5f;
      if (auto file = textures_->ReadFile(mudFiles_[1]))
        if (NormalMapMean(*file, &mx, &my) && (std::fabs(mx - 0.5f) > 0.2f || std::fabs(my - 0.5f) > 0.2f)) {
          fprintf(stderr, "render: deep tracks' mud: %s leans %.2f, %.2f on the whole (a normal map's channels wrong?): drawn without it\n",
                  mudFiles_[1].c_str(), mx * 2 - 1, my * 2 - 1);
          mud_[1] = textures_->FlatNormal();
        }
      if (!mud_[0] || !mud_[1] || !mud_[2]) fprintf(stderr, "render: deep tracks' mud cannot be read: the ruts are drawn without it\n");
    }
    mud = mud_[0] && mud_[1] && mud_[2];
  }
  c.mudInfo[0] = mud ? 1.0f : 0.0f;
  c.mudInfo[1] = mud ? 1 / mudSize_ : 0.0f;

  for (int k = 0; k <= top && k < 12; ++k) {
    if (k == top) {
      c.morph[k][0] = 1e30f;
      c.morph[k][1] = 0;
    } else {
      const float end = ranges_[static_cast<size_t>(k)] * 0.98f, start = ranges_[static_cast<size_t>(k)] * 0.7f;
      c.morph[k][0] = start;
      c.morph[k][1] = 1.0f / (end - start);
    }
  }

  // [parallax] the ground's relief close by: by its own pipeline (made the first time), each remastered layer's depth
  const bool relief = parallax && !shadowPass && materials_ && richGround && ParallaxPipeline();
  if (relief) {
    for (int i = 0; i < 9; ++i) c.albedoTile[i][2] = material_[i] ? LayerDepth(i) : 0.0f;
    c.parallax[0] = 1;
    c.parallax[1] = parallaxReach;
    c.parallax[2] = 1 / std::max(0.5f * parallaxReach, 1.0f);
    c.parallax[3] = parallaxShadows;
  }

  device_->SetPipeline(shadowPass ? shadowPipeline_ : relief ? parallaxPipeline_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, heightTexture_);
  if (!shadowPass) {
    device_->SetTexture(1, normalMap_);
    device_->SetTexture(2, maskLow_);
    device_->SetTexture(3, maskHigh_);
    device_->SetTexture(4, waterMap_);
    device_->SetTexture(5, waterRamp_);
    for (int i = 0; i < 10; ++i) device_->SetTexture(6 + i, albedo_[i]);
    for (int i = 0; i < 9; ++i) device_->SetTexture(16 + i, normal_[i]);
    if (materials_) device_->SetTexture(25, materials_);
    device_->SetTexture(26, tracksMap ? tracksMap : textures_->Black());
    if (mud)
      for (int k = 0; k < 3; ++k) device_->SetTexture(kMudSlot + k, mud_[k]);  // [mud ruts]
  }
  device_->SetVertexBuffer(gridVertices_);
  device_->SetIndexBuffer(gridIndices_);
  // the shader's array is always handed over whole, so the list is padded to full batches
  const size_t count = patches_.size();
  patches_.resize((count + kMaxPatches - 1) / kMaxPatches * kMaxPatches, Patch{});
  for (size_t at = 0; at < count; at += kMaxPatches) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kMaxPatches, count - at));
    device_->SetConstants(2, patches_.data() + at, kMaxPatches * static_cast<uint32_t>(sizeof(Patch)));
    device_->DrawIndexed(gridIndexCount_, 0, 0, n);
  }
  if (shadowPass) return;
  drawnPatches = static_cast<int>(count);
  drawnTriangles = drawnPatches * kPatchQuads * kPatchQuads * 2;
}

}  // namespace render
