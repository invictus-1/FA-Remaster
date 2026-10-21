// [plantlod] The ground cover's plants by their size on screen (plant_lod.h).
#include "plant_lod.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace render {

// With plantsFar the plants' shaders (grass.cpp's, after its declarations) take this: the tiles, where the fields kept
// over the square round what the camera looks at count, a spot's placing (the same as the cards' without it, by a cell's
// whole-number place in the world), its size on screen, and how much of each of its plants is drawn.
const char kPlantLodBlock[] = R"hlsl(
// (plant_lod.h, PlantTiles) per tile: its first cell's x and z (whole cells), its row of the models' flags (-1: none)
StructuredBuffer<float4> plantTiles : register(t0, space1);
// per spot of a tile with a row of flags: bit m set where the models' pass drew its plant m (it had room for it)
Texture2D<uint> modelFlags : register(t14);
// beyond the square the fields are kept over: what clears the plants there as the near ones are cleared (the remastered
// pads, what stands on the ground), over the tiles in view (GroundCover::PlaceFar; farField says where)
Texture2D<float> farCleared : register(t21);
// Where a place lies in the square the fields are kept over (pressed flat, the pads, the blasts' waves), and how much
// they count there: 1 well inside, nothing outside it or when they are not kept this frame (the camera far)
float2 PlantFieldUv(float2 at) { return (at - area.xy) / (area.z * area.w); }
float PlantFieldWeight(float2 at) {
  float2 uv = PlantFieldUv(at);
  return lodInfo[2].x > 0.5 ? saturate(min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y)) * 20) : 0;
}
struct PlantSpot {
  float2 xz;
  uint h, group, cell, count;
  float grown, bare;
};
// A cell's numbers: the cover's 16 (cells, cellModels), or past them a model with no card of its own (extraCells: its
// model as cellModels', its size and more as cells')
float4 CellSize(uint c) { return c < 16 ? cells[c * 3 + 1] : extraCells[(c - 16) * 3 + 1]; }
float4 CellMore(uint c) { return c < 16 ? cells[c * 3 + 2] : extraCells[(c - 16) * 3 + 2]; }
float4 CellModel(uint c) { return c < 16 ? cellModels[c] : extraCells[(c - 16) * 3]; }
// The spot of the cell `cellId` (whole cells of area.z), as the cards are placed without plantsFar: false where nothing
// grows there (off the map, a pad, bare ground, not yet grown in, or the table's chance says none).
bool PlantSpotAt(int2 cellId, out PlantSpot s) {
  s = (PlantSpot)0;
  float2 cellAt = float2(cellId) * area.z;
  uint h = PlantHash(uint(cellId.x) * 83492791u ^ PlantHash(uint(cellId.y) * 2971215073u));
  float2 xz = cellAt + float2(PlantRand(h), PlantRand(PlantHash(h + 1))) * area.z;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return false;
  [branch] if (info.y > 0.5 && padCover.SampleLevel(samplerLinearClamp, PlantFieldUv(xz), 0) * PlantFieldWeight(xz) > PlantRand(PlantHash(h + 12))) return false;
  // (beyond it, the far field: none where a pad lies or something stands, as near)
  [branch] if (farField.w > 0.5) {
    float2 fuv = (xz - farField.xy) * farField.z;
    if (all(fuv >= 0) && all(fuv <= 1) && farCleared.SampleLevel(samplerLinearClamp, fuv, 0) * (1 - PlantFieldWeight(xz)) > PlantRand(PlantHash(h + 16)))
      return false;
  }
  float bare = 0;
  [branch] if (shown.y > 0.5) {
    bare = bareGround.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
    if (bare > PlantRand(PlantHash(h + 13))) return false;
  }
  const float grown = CoverGrown(shown.x, PlantRand(PlantHash(h + 14)));
  if (grown <= 0) return false;
  float4 a = densityA.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float4 b = densityB.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float k = info.x;
  float pc = a.g * k, pf = a.b * k * PlantPatch(xz), ps = a.a * k, pn = b.r * k;
  float r = PlantRand(PlantHash(h + 2));
  uint group, cell;
  [branch] if (lodInfo[0].w >= 0) {
    // (for pictures, PlantLodTune::onlyCell: a quarter of the spots that cell, wherever the table lets anything grow)
    if (r > 0.25 || a.r + a.g + a.b + a.a + b.r < 0.01) return false;
    group = uint(lodInfo[3].w);
    cell = uint(lodInfo[0].w);
  } else {
    if (r < pc) {
      float q = PlantRand(PlantHash(h + 3));
      group = q < b.b ? 2 : (PlantRand(PlantHash(h + 4)) < b.g ? 1 : 0);
    } else if (r < pc + pf) group = 3;
    else if (r < pc + pf + ps) group = 4;
    else if (r < pc + pf + ps + pn) group = 5;
    else return false;
    float4 g = groups[group];
    if (g.y < 0.5) return false;
    cell = OrderAt(uint(g.x) + PlantHash(h + 5) % uint(g.y));
    // a model with no card takes its share of a group's spots (extraRules: the visuals session's "place", or the ferns')
    [loop] for (uint e = 0; e < uint(lodInfo[2].w); ++e)
      if (uint(extraRules[e].x) == group && PlantRand(PlantHash(h + 40 + e)) < extraRules[e].y) {
        cell = 16 + e;
        break;
      }
  }
  float4 more = CellMore(cell);
  s.count = uint(more.y) + PlantHash(h + 6) % uint(max(more.z - more.y + 1, 1));
  s.xz = xz;
  s.h = h;
  s.group = group;
  s.cell = cell;
  s.grown = grown;
  s.bare = bare;
  return true;
}
// Its size on screen: pixels across its card (the card's width at the cell's own height) at the spot's distance
float PlantPixels(PlantSpot s) {
  float4 size = CellSize(s.cell), more = CellMore(s.cell);
  float away = length(float3(s.xz.x, PlantGround(s.xz), s.xz.y) - cameraPosition);
  return size.x * more.w * lodInfo[0].z / max(away, 0.01);
}
// How much of a plant of the spot is drawn, by its size on screen (`px`): the spot gone below a size of its own in the
// cut's band, shrinking into the ground as it nears it; a clump's plants after the first (`member`) shrinking away as it
// gets small while the first grows, the clump's share of the ground kept. 0: none.
float PlantLodScale(PlantSpot s, float px, uint member) {
  float cutAt = lerp(lodInfo[1].z, lodInfo[1].w, PlantRand(PlantHash(s.h + 15)));
  float keep = saturate((px / cutAt - 1) / 0.35);
  float collapse = saturate((lodInfo[1].x - px) / max(lodInfo[1].x - lodInfo[1].y, 1e-3));
  return keep * (member == 0 ? sqrt(1 + float(s.count - 1) * collapse) : sqrt(1 - collapse));
}
// The level the models draw a plant of the spot at (0 the finest .. 3), or -1: its card (no model for the card, too
// small on screen for the model's levels, or no models). Below the last of its levels' sizes a kind with a card ends on
// it; one without (or with a fourth level) keeps its last level down to the cut. Each plant at its own sizes within the
// band round the levels' (no ring where they change). `hc`: the plant's own number (grass.cpp: PlantHash(h * 31 + plant
// * 977 + 7)).
int PlantModelLevel(PlantSpot s, float px, uint hc) {
  float4 m = CellModel(s.cell);
  if (m.x < 0 || lodInfo[2].y < 0.5 || (s.group <= 1 && s.cell < 16)) return -1;
  float4 kind = modelKinds[uint(m.x)];
  float p = px * (1 + lodInfo[2].z * (PlantRand(PlantHash(hc + 21)) - 0.5));
  int level = p >= m.y ? 0 : p >= m.z ? 1 : p >= m.w ? 2 : 3;
  int last = int(kind.x) - 1;
  return level <= last ? level : kind.y > 0.5 ? -1 : last;
}
)hlsl";

// The models' placing (GroundCover::PrepareFar): a thread a spot of the tiles that have a row of flags (their first cells
// in the tiles' buffer, PlantTiles::modelRows). Each plant of a spot whose card has a model, large enough on screen for
// one of its levels, is put in that kind's level's list (its foot, its ground, its height; its own number, its cell) as
// far as there is room, and its bit set in the spot's flags, so that the cards leave it out; what does not fit stays a
// card.
const char kPlantModelPlace[] = R"hlsl(
RWTexture2D<uint> placeCounters : register(u0);    // per kind and level: how many were put in its list
RWTexture2D<float4> placeInstances : register(u1);  // the lists: two texels a plant, 2048 plants a row, 4 rows a list
RWTexture2D<uint> placeFlags : register(u2);       // per spot (x) of a model tile (y): what the models drew
cbuffer ModelPlace : register(b2) {
  float4 room[8];      // per kind * 4 + level: how many its list has room for this frame
  float4 placeInfo;    // x: the model tiles
};
[numthreads(64, 1, 1)] void ClearCS(uint3 id : SV_DispatchThreadID) { placeCounters[int2(id.x, 0)] = 0; }
[numthreads(64, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint slot = id.x / 256u, within = id.x % 256u;
  if (slot >= uint(placeInfo.x)) return;
  float4 tile = plantTiles[slot];
  uint bits = 0;
  PlantSpot s;
  [branch] if (PlantSpotAt(int2(tile.xy) + int2(within % 16u, within / 16u), s) && CellModel(s.cell).x >= 0) {
    float px = PlantPixels(s);
    float4 size = CellSize(s.cell), more = CellMore(s.cell);
    uint kind = uint(CellModel(s.cell).x);
    for (uint plant = 0; plant < s.count && plant < 6u; ++plant) {
      uint hc = PlantHash(s.h * 31 + plant * 977 + 7);
      int level = PlantModelLevel(s, px, hc);
      if (level < 0) continue;
      float scale = PlantLodScale(s, px, plant);
      if (scale <= 0.01) continue;
      // (its foot, as the cards': grass.cpp)
      float2 foot = s.xz;
      if (plant > 0) {
        float angle = PlantRand(hc) * 6.2832, reach = size.x * more.w * 0.35 * sqrt(PlantRand(PlantHash(hc + 1)));
        foot += float2(cos(angle), sin(angle)) * reach;
      }
      float ground = PlantGround(foot);
      if (ground < mapInfo.w + 0.08) continue;
      float height = lerp(size.y, size.z, PlantRand(PlantHash(hc + 2))) * more.w * scale * s.grown * (1 - 0.5 * s.bare);
      uint bucket = kind * 4 + uint(level), at;
      InterlockedAdd(placeCounters[int2(bucket, 0)], 1u, at);
      if (at >= uint(room[bucket / 4][bucket % 4])) continue;
      int2 texel = int2((at % 2048u) * 2, bucket * 4 + at / 2048u);
      placeInstances[texel] = float4(foot.x, ground, foot.y, height);
      placeInstances[texel + int2(1, 0)] = float4(asfloat(hc), float(s.cell), float(plant), 0);
      bits |= 1u << plant;
    }
  }
  placeFlags[int2(within, slot)] = bits;
}
)hlsl";

// The models (GroundCover::Draw): each level of each kind drawn for its list (an instance a plant, as many as there was
// room for; those past the count put nowhere), placed as its card's plant (its foot, height and turn), swaying in the same
// wind, pressed, laid over and buried as the cards are, lit as the visuals session made it: its own normals rounded over
// the plant and its normal map, the same on both sides (not turned to face the eye: its back faces carry the outward
// normal), its own colour brought toward the ground's brightness as its card's is.
const char kPlantModelShader[] = R"hlsl(
Texture2D<float4> modelInstances : register(t15);
Texture2D<uint> modelCounters : register(t16);
Texture2D modelAlbedo : register(t17);   // sRGB values; alpha its coverage
Texture2D modelNormals : register(t18);  // FAF's layout: x in green, y in alpha (as the models', mesh_renderer.cpp)
Texture2D modelSpec : register(t19);     // LabPBR: r metal, g roughness, a how much light comes through
Texture2D modelAo : register(t20);       // r: ambient occlusion
// lodInfo[3]: x this draw's list (kind * 4 + level), y its mesh's height (model units), z 1 = it has its material (modelSpec)

struct MIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance : SV_InstanceID;
};
struct MOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float3 normal : TEXCOORD2;
  float3 tangent : TEXCOORD3;
  float3 binormal : TEXCOORD4;
  float4 info : TEXCOORD5;  // x: how high up the plant (0 its foot, 1 its top); y: the snow's top here (world y); z: its
                            // snow dusting; w: 1 = drawn
  nointerpolation float3 tint : TEXCOORD6;  // its colour times this (its card's: the ground's brightness, tones)
#ifdef PLANTS
  nointerpolation float4 burnt : TEXCOORD7;
#endif
#ifdef MOTION
  float4 here : TEXCOORD9;
  float4 then : TEXCOORD10;
#endif
};

float3 ModelTurn(float3 v, float c, float s) { return float3(v.x * c - v.z * s, v.y, v.x * s + v.z * c); }

MOut MVS(MIn v) {
  MOut o = (MOut)0;
  o.position = float4(0, 0, -1, 1);
#ifdef MOTION
  o.here = o.then = float4(0, 0, 0, 1);
#endif
  uint bucket = uint(lodInfo[3].x);
  if (v.instance >= modelCounters.Load(int3(bucket, 0, 0))) return o;
  int2 texel = int2((v.instance % 2048u) * 2, bucket * 4 + v.instance / 2048u);
  float4 r0 = modelInstances.Load(int3(texel, 0)), r1 = modelInstances.Load(int3(texel + int2(1, 0), 0));
  float2 foot = r0.xz;
  float ground = r0.y, height = r0.w;
  uint hc = asuint(r1.x), cell = uint(r1.y);
  float4 more = CellMore(cell);
  float meshHeight = max(lodInfo[3].y, 1e-3);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, PlantFieldUv(foot), 0) * PlantFieldWeight(foot);
#ifdef PLANTS
  o.burnt = PlantsBurnLook(foot, plantsK[0], plantsK[1], PlantRand(PlantHash(hc + 9)));
  height *= 1 - 0.8 * o.burnt.z;
#endif
  if (height <= 0.001) return o;
  float scale = height / meshHeight;
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  // under lying snow (as the cards): buried when no taller than it, else what stands out of it, bowed a little
  float deep = 0, snowDust = 0;
  [branch] if (weather.y > 0 && coverSnow.x > 0) {
    deep = CoverSnowDepth(foot, ground, n, mapInfo.xy, snowDust);
    if (deep > 0 && height - deep < CoverSnowOut(height)) return o;
  }
  // turned its own way (as its card's star: PlantRand(hc + 3)), a little off upright
  float turn = PlantRand(PlantHash(hc + 3)) * 6.2832, c = cos(turn), s = sin(turn);
  float2 tilt = (float2(PlantRand(PlantHash(hc + 4)), PlantRand(PlantHash(hc + 5))) - 0.5) * 0.3;
  float3 local = ModelTurn(v.position, c, s) * scale;
  float above = saturate(v.position.y / meshHeight);
  local.xz += tilt * local.y;
  // the trees' wind, the more toward the top (as the cards: the card's sway by `more.x`)
  float beat = dot(foot, float2(0.11, 0.07));
  float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
  float3 p = float3(foot.x, ground, foot.y) + local;
#ifdef MOTION
  float swingB = sin(motionWhen.x * 1.3 + beat) * 0.6 + sin(motionWhen.x * 2.9 + beat * 1.7) * 0.4;
  float gustB = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (motionWhen.x * 7)) * 0.02, motionWhen.x * 0.07)));
  float3 pB = p;
  pB.xz += float2(0.915, 0.403) * (swingB * gustB * 0.35 * more.x) * above * above * height;
#endif
  p.xz += float2(0.915, 0.403) * (swing * gust * 0.35 * more.x) * above * above * height;
#ifdef PLANTS
  {  // pressed by what drove over it: laid over the way it was pushed, rising again (as the cards)
    float4 pressedBy = PlantsPressAt(foot, plantsK[0], plantsK[1]);
    [branch] if (pressedBy.z > 0.01) {
      float down = pressedBy.z * exp(-(pressedBy.w / pressedBy.z) / 6.0);
      float2 way = pressedBy.xy / pressedBy.z;
      float layOver = min(down * 1.3, 1.3);
      way = length(way) > 0.05 ? normalize(way) : float2(c, s);
      p.y -= local.y * (1 - cos(layOver));
      p.xz += way * (sin(layOver) * above * local.y);
#ifdef MOTION
      float downB = pressedBy.z * exp(-max(pressedBy.w / pressedBy.z - motionWhen.y, 0) / 6.0);
      float layOverB = min(downB * 1.3, 1.3);
      pB.y -= local.y * (1 - cos(layOverB));
      pB.xz += way * (sin(layOverB) * above * local.y);
#endif
    }
  }
#endif
#ifdef BLASTS
  {  // a blast's wave: laid over away from it (as the cards)
    float2 blown = blastField.SampleLevel(samplerLinearClamp, PlantFieldUv(foot), 0) * PlantFieldWeight(foot);
    float over = length(blown), lay = min(over, 1.35);
    if (lay > 1e-3) {
      p.y -= local.y * (1 - cos(lay));
      p.xz += blown / over * (sin(lay) * above * local.y);
#ifdef MOTION
      pB.y -= local.y * (1 - cos(lay));
      pB.xz += blown / over * (sin(lay) * above * local.y);
#endif
    }
  }
#endif
  [branch] if (deep > 0) {  // (bowed a little its own way by the snow on it)
    float lay = 0.45 * snowDust, a = PlantRand(PlantHash(hc + 3)) * 6.2832;
    float3 bowed = float3(0, -local.y * (1 - cos(lay)), 0) + float3(cos(a), 0, sin(a)) * (sin(lay) * above * local.y);
    p += bowed;
#ifdef MOTION
    pB += bowed;
#endif
  }
  o.position = mul(viewProj, float4(p, 1));
  o.world = p;
  o.uv = v.uv;
  o.normal = ModelTurn(v.normal, c, s);
  o.tangent = ModelTurn(v.tangent, c, s);
  o.binormal = ModelTurn(v.binormal, c, s);
  o.info = float4(above, ground + deep, snowDust, 1);
#ifdef MOTION
  o.here = mul(motionNow, float4(p, 1));
  o.then = mul(motionBefore, float4(pB, 1));
#else
  // its colour as its card's (grass.cpp, [cover]): its leaves' mean brought to its card's (modelTint), then the weeds' and
  // ferns' brightness brought toward the ground's (a model with no card: by its own mean, extraTones)
  float4 tone = cell < 16 ? tones[cell] : extraTones[cell - 16];
  o.tint = modelTint[uint(CellModel(cell).x)].rgb;
  [branch] if (tone.w != 0) {
    float3 under = ToLinear(cover.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0).rgb) * (0.9 + 0.2 * PlantRand(PlantHash(hc + 10)));
    if (tone.w > 0) o.tint *= lerp(1, clamp(under / max(tone.rgb, 1e-3), 0.25, 4), tone.w);
    else o.tint *= lerp(1, clamp(PlantLuma(under) / max(PlantLuma(tone.rgb), 1e-3), 0.5, 3), -tone.w);
  }
#endif
  return o;
}

// Its coverage (the albedo's alpha) kept as it is at full size in the smaller levels of its picture: a level's alpha is the
// mean of the finer ones', so leaves thinner than a texel there fall under the cut and a plant far away loses its leaves
// (the bush's level 2 all but gone at 20-30 pixels); scaled up by the level it is read at, as foliage is commonly kept.
float ModelCoverage(float2 uv) {
  float level = modelAlbedo.CalculateLevelOfDetail(samplerAniso, uv);
  return modelAlbedo.Sample(samplerAniso, uv).a * (1 + max(level, 0) * 0.25);
}

#ifdef MOTION
float2 MotionMPS(MOut i) : SV_Target {
  clip(i.info.w - 0.5);
  clip(ModelCoverage(i.uv) - 0.5);
  clip(i.world.y - i.info.y);
  return CoverMoved(i.here, i.then);
}
#endif

float4 MPS(MOut i) : SV_Target {
#if RAYS
  rayLightsAtMost = 1;
#endif
  [branch] if (weather.y > 0 && coverSnow.x > 0) surfaceSnowShare = 0;
  float4 albedo = modelAlbedo.Sample(samplerAniso, i.uv);
  albedo.a = ModelCoverage(i.uv);
  // its holes' edges a pixel soft, which the samples of a pixel turn into a smooth one (alpha to coverage)
  float cover = saturate((albedo.a - 0.5) / max(fwidth(albedo.a), 1e-4) + 0.5);
#ifdef PLANTS
  cover = saturate((albedo.a - 0.5 - 0.3 * saturate(i.burnt.x + i.burnt.y)) / max(fwidth(albedo.a), 1e-4) + 0.5);
#endif
  clip(cover - 0.02);
  clip(i.world.y - i.info.y);  // (under lying snow: what is under its top)
  // right at the eye (a cinematic camera down among the plants): faded out as the cards are
  float near = length(i.world - cameraPosition);
  [branch] if (near < 1.2) {
    float fade = smoothstep(0.4, 1.2, near);
#if PLANT_SAMPLES < 2
    if (fade <= frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))))) discard;
#else
    cover *= fade;
#endif
  }
  float2 nxy = modelNormals.Sample(samplerAniso, i.uv).ga * 2 - 1;
  float3 vertexNormal = normalize(i.normal);
  float3 N = normalize(nxy.x * normalize(i.binormal) + nxy.y * normalize(i.tangent) + sqrt(saturate(1 - dot(nxy, nxy))) * vertexNormal);
  // (without its material: rough, no metal, some light through, as the models' leaves without one)
  float4 spec = lodInfo[3].z > 0.5 ? modelSpec.Sample(samplerAniso, i.uv) : float4(0, 0.85, 0, 0.35);
  float occlusion = modelAo.Sample(samplerAniso, i.uv).r * lerp(0.7, 1, i.info.x);
  float3 base = ToLinear(albedo.rgb) * i.tint;
#ifdef PLANTS
  base = PlantsBurnColour(base, i.burnt);
#endif
  float3 toEye = normalize(cameraPosition - i.world);
  float shadow = SunShadow(i.world + vertexNormal * 0.02, vertexNormal);
  float3 through = LeafLight(base, LeafThrough(N, toEye), i.world, shadow, 0.7);
#ifdef PLANTS
  through = through * (1 - saturate(i.burnt.x + i.burnt.y)) + PlantsEmber(i.burnt.w, i.world.xz, time) * (falling.w > 0 ? falling.w : 1);
#endif
  [branch] if (i.info.z > 0) {  // (dusted with snow where its leaves face up, the more toward its top)
    float dust = i.info.z * saturate(N.y * 2.5 + 0.4) * smoothstep(0.1, 0.6, i.info.x) * 0.9;
    base = lerp(base, SNOW_COLOUR, dust);
    through *= 1 - dust;
  }
  return float4(ModernLight(base, spec.r, spec.g, occlusion, through, N, i.world, shadow), cover);
}
)hlsl";

// The far cover (GroundCover::DrawFarCover): over the ground's pixels, where the plants are no longer drawn (smaller on
// screen than their cut, or the meadow's blades beyond their square), the share of the ground they would cover and their
// colour, from the cover's own table as the cards are placed by it: the ground's colour multiplied toward theirs by that
// share. The ground there has been lit; the plants' colour over the ground's says how much lighter or darker, and which
// hue, a lit plant is than the lit ground.
const char kFarCoverShader[] = R"hlsl(
cbuffer FarCover : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // xy: the map's width and height; z: the water's level; w: spots a cell where the table says 1
  float4 cutInfo;      // x: pixels a world unit at a distance of one; y, z: the cut's band (pixels); w: the plants' cell
  float4 bladeInfo;    // xy: the blades' square's middle; z: its half width; w: how much of the blades is shown (0: none drawn)
  float4 bladeLook;    // x: the blades' share of fully grassy ground; y: how much of it is laid (farCover); z: flowers' patches' size, w: their cover
  float4 groupLook[6]; // per group: rgb its colour over the ground's (w < 0: brought toward the ground's brightness by -w);
  float4 groupSize[6]; // per group: x its card's width (world units); y how much of the card it covers seen from above; z its plants a spot;
                       // w how much darker it is lit than its colour (the shade among its leaves)
  float4 farMore;      // x: 1 = the bare ground's picture is read; y: how much of the plants' (not the blades') is laid
  float4 farField;     // the far field (GroundCover::PlaceFar): its first x, z; 1 / its width; 1 = made this frame
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
Texture2D groundColours : register(t2);  // Terrain::GroundColours: rgb the ground's colour, a how grassy
Texture2D densityA : register(t4);
Texture2D densityB : register(t5);
Texture2D<float> bareGround : register(t12);
Texture2D<float> farCleared : register(t21);  // (where a pad lies or something stands: none there)
uint FarHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float FarRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
// the flowers' patches, as the cards' (grass.cpp, PlantPatch)
float FarPatch(float2 xz) {
  float2 g = xz / max(bladeLook.z, 0.1);
  float2 i = floor(g), f = g - i;
  f = f * f * (3 - 2 * f);
  float a = FarRand(FarHash(uint(int(i.x)) * 73856093u ^ FarHash(uint(int(i.y)) * 19349663u)));
  float b = FarRand(FarHash(uint(int(i.x) + 1) * 73856093u ^ FarHash(uint(int(i.y)) * 19349663u)));
  float c = FarRand(FarHash(uint(int(i.x)) * 73856093u ^ FarHash(uint(int(i.y) + 1) * 19349663u)));
  float d = FarRand(FarHash(uint(int(i.x) + 1) * 73856093u ^ FarHash(uint(int(i.y) + 1) * 19349663u)));
  float n = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
  float cover = max(bladeLook.w, 0.05);
  return saturate((n - (1 - cover)) / 0.08) / cover;
}
float4 VS(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1); }
float4 PS(float4 position : SV_Position) : SV_Target {
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) discard;
  float away = cameraBack.w / depth;
  float2 ndc = float2(position.x * viewport.z * 2 - 1, 1 - position.y * viewport.w * 2);
  float3 world = cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
  if (any(world.xz < 0) || any(world.xz > mapInfo.xy) || world.y < mapInfo.z + 0.08) discard;
  float dist = length(world - cameraPosition);
  float2 uv = world.xz / mapInfo.xy;
  float4 a = densityA.SampleLevel(samplerLinearClamp, uv, 0), b = densityB.SampleLevel(samplerLinearClamp, uv, 0);
  float bare = farMore.x > 0.5 ? bareGround.SampleLevel(samplerLinearClamp, uv, 0) : 0;
  float3 ground = ToLinear(groundColours.SampleLevel(samplerLinearClamp, uv, 0).rgb);
  float k = mapInfo.w, cell2 = cutInfo.w * cutInfo.w;
  // each group's chance a cell (as the cards are chosen: one group a spot, in this order)
  float pc = a.g * k, pf = a.b * k * FarPatch(world.xz), ps = a.a * k, pn = b.r * k;
  float pw = pc * b.b, pd = pc * (1 - b.b) * b.g, pg = pc * (1 - b.b) * (1 - b.g);
  float chance[6] = {pg, pd, pw, pf, ps, pn};
  float total = 0;
  float3 colour = 0;
  float luma = max(dot(ground, float3(0.2126, 0.7152, 0.0722)), 1e-4);
  [branch] if (farMore.y > 0) {
  [unroll] for (int g = 0; g < 6; ++g) {
    float px = groupSize[g].x * cutInfo.x / max(dist, 0.01);
    // (the share of its spots no longer drawn: each spot's cut is its own in the band, and it shrinks as it nears it)
    float gone = 1 - saturate((px / 1.2 - cutInfo.y) / max(cutInfo.z - cutInfo.y, 1e-3));
    float share = saturate(chance[g]) / cell2 * groupSize[g].z * groupSize[g].x * groupSize[g].x * groupSize[g].y * gone * farMore.y;
    // (as the cards are coloured: the meadow's clumps the ground's own, a little richer; the weeds' and ferns' brightness
    // brought toward the ground's; then as much darker as a plant is among its own leaves, groupSize's w)
    float3 c = groupLook[g].w < 0 ? groupLook[g].rgb * lerp(1, luma / max(dot(groupLook[g].rgb, float3(0.2126, 0.7152, 0.0722)), 1e-4), -groupLook[g].w)
                                  : lerp(groupLook[g].rgb, ground * float3(1.02, 1.1, 0.9), groupLook[g].w);
    c *= groupSize[g].w;
    colour += c * share;
    total += share;
  }
  }
  // the meadow's blades beyond what is drawn of them (their square, as far as it reaches; how much of them is shown)
  // (grass.cpp: fewer and lower toward the square's edge, by the distance from its middle)
  float thin = saturate((1 - length(world.xz - bladeInfo.xy) / max(bladeInfo.z, 1e-3)) * 3);
  float inSquare = thin * saturate(thin * 2.5) * bladeInfo.w;
  float grassy = a.r;
  float blades = bladeLook.x * grassy * (1 - inSquare);
  // (a blade: the meadow's colour, richer and a little lighter than the ground's mean, shaded down among the blades: about
  // the ground's brightness, greener)
  colour += ground * float3(1.0, 1.08, 0.88) * blades;
  total += blades;
  if (total < 1e-4) discard;
  float3 theirs = colour / total;
  float2 fuv = (world.xz - farField.xy) * farField.z;
  float cleared = farField.w > 0.5 && all(fuv >= 0) && all(fuv <= 1) ? farCleared.SampleLevel(samplerLinearClamp, fuv, 0) : 0;
  float covered = 1 - exp(-total * (1 - bare) * (1 - cleared));  // (they lie over each other)
  return float4(lerp(1, theirs / max(ground, 1e-3), covered * bladeLook.y), 1);
}
)hlsl";

// The far field's footprints (GroundCover::PlaceFar): what stands on the ground (a disc each: x, z, radius) laid into it,
// full within the disc and nothing a quarter of a world unit outside it (as the near cover's pressed flat, grass.cpp's
// kTrample), added over the pads already there.
const char kFarDiscShader[] = R"hlsl(
cbuffer FarDiscs : register(b1) {
  float4 area;        // the far field: its first x, z; a texel's size; texels a side
  float4 discs[256];  // x, z, radius
};
struct DOut {
  float4 position : SV_Position;
  float2 at : TEXCOORD0;
  nointerpolation float3 disc : TEXCOORD1;
};
static const float2 kDiscCorner[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
DOut VS(uint id : SV_VertexID) {
  float4 d = discs[id / 6];
  float2 xz = d.xy + kDiscCorner[id % 6] * (d.z + 0.25 + area.z);  // (a texel more: none smaller than a texel is lost)
  float wide = area.z * area.w;
  DOut o;
  o.position = float4((xz.x - area.x) / wide * 2 - 1, 1 - (xz.y - area.y) / wide * 2, 0, 1);
  o.at = xz;
  o.disc = d.xyz;
  return o;
}
float4 PS(DOut i) : SV_Target {
  float flat = saturate((i.disc.z + 0.25 + 0.5 * area.z - length(i.at - i.disc.xy)) / (0.25 + area.z));
  if (flat <= 0) discard;
  return float4(flat, flat, flat, 1);
}
)hlsl";

// ---------------------------------------------------------------------------------------------------------------------

void PlantTiles::SetHeights(const std::vector<uint16_t>& heights, int width, int height, float heightScale, float waterLevel) {
  levels_.clear();
  width_ = width;
  height_ = height;
  water_ = waterLevel;
  if (width <= 0 || height <= 0 || heights.size() < static_cast<size_t>(width + 1) * static_cast<size_t>(height + 1)) return;
  Level l0;
  l0.size = 4;
  l0.w = (width + 3) / 4;
  l0.h = (height + 3) / 4;
  l0.low.assign(static_cast<size_t>(l0.w) * static_cast<size_t>(l0.h), 1e30f);
  l0.high.assign(l0.low.size(), -1e30f);
  for (int z = 0; z <= height; ++z)
    for (int x = 0; x <= width; ++x) {
      const float y = heights[static_cast<size_t>(z) * static_cast<size_t>(width + 1) + static_cast<size_t>(x)] * heightScale;
      // (a point on a square's edge belongs to both)
      for (int dz = -1; dz <= 0; ++dz)
        for (int dx = -1; dx <= 0; ++dx) {
          const int sx = (x + dx * (x % 4 == 0 ? 1 : 0)) / 4, sz = (z + dz * (z % 4 == 0 ? 1 : 0)) / 4;
          if (sx < 0 || sz < 0 || sx >= l0.w || sz >= l0.h) continue;
          const size_t i = static_cast<size_t>(sz) * static_cast<size_t>(l0.w) + static_cast<size_t>(sx);
          l0.low[i] = std::min(l0.low[i], y);
          l0.high[i] = std::max(l0.high[i], y);
        }
    }
  levels_.push_back(std::move(l0));
  while (levels_.back().w > 1 || levels_.back().h > 1) {
    const Level& f = levels_.back();
    Level l;
    l.size = f.size * 2;
    l.w = (f.w + 1) / 2;
    l.h = (f.h + 1) / 2;
    l.low.assign(static_cast<size_t>(l.w) * static_cast<size_t>(l.h), 1e30f);
    l.high.assign(l.low.size(), -1e30f);
    for (int z = 0; z < f.h; ++z)
      for (int x = 0; x < f.w; ++x) {
        const size_t from = static_cast<size_t>(z) * static_cast<size_t>(f.w) + static_cast<size_t>(x);
        const size_t to = static_cast<size_t>(z / 2) * static_cast<size_t>(l.w) + static_cast<size_t>(x / 2);
        l.low[to] = std::min(l.low[to], f.low[from]);
        l.high[to] = std::max(l.high[to], f.high[from]);
      }
    levels_.push_back(std::move(l));
  }
}

void PlantTiles::Range(float x0, float z0, float x1, float z1, float* low, float* high) const {
  *low = 1e30f;
  *high = -1e30f;
  if (levels_.empty()) {
    *low = -1e4f;
    *high = 1e4f;
    return;
  }
  const float wide = std::max(x1 - x0, z1 - z0);
  size_t li = 0;
  while (li + 1 < levels_.size() && static_cast<float>(levels_[li].size) * 2 < wide) ++li;
  const Level& l = levels_[li];
  const int sx0 = std::clamp(static_cast<int>(std::floor(x0 / static_cast<float>(l.size))), 0, l.w - 1);
  const int sz0 = std::clamp(static_cast<int>(std::floor(z0 / static_cast<float>(l.size))), 0, l.h - 1);
  const int sx1 = std::clamp(static_cast<int>(std::floor(x1 / static_cast<float>(l.size))), 0, l.w - 1);
  const int sz1 = std::clamp(static_cast<int>(std::floor(z1 / static_cast<float>(l.size))), 0, l.h - 1);
  for (int z = sz0; z <= sz1; ++z)
    for (int x = sx0; x <= sx1; ++x) {
      const size_t i = static_cast<size_t>(z) * static_cast<size_t>(l.w) + static_cast<size_t>(x);
      *low = std::min(*low, l.low[i]);
      *high = std::max(*high, l.high[i]);
    }
}

void PlantTiles::Select(const View& view, float cell, const std::vector<CellSize>& cells, const PlantLodTune& tune, float tallest) {
  rows.clear();
  modelRows.clear();
  batches.clear();
  found_.clear();
  if (cell <= 0 || width_ <= 0 || cells.empty()) return;
  focal = static_cast<float>(view.height) / (2 * std::tan(view.fovY * 0.5f)) / std::max(view.lodScale, 1e-3f);
  cutScale = std::max(1.0f, static_cast<float>(view.height) / 1440.0f);
  float largest = 0;
  for (const CellSize& c : cells) largest = std::max(largest, c.frame);
  if (largest <= 0) return;
  // (no farther than its largest plant is at the least of the cut on screen)
  const float reach = largest * focal / std::max(tune.cutLow * cutScale, 0.5f);
  const float tileWide = cell * kSide;
  const int tilesX = static_cast<int>(std::ceil(static_cast<float>(width_) / tileWide));
  const int tilesZ = static_cast<int>(std::ceil(static_cast<float>(height_) / tileWide));
  int span = 1;
  while (span < std::max(tilesX, tilesZ)) span *= 2;
  Visit(view, 0, 0, span, tilesX, tilesZ, tileWide, cell, cells, tune, tallest, reach);
  // (at most so many, the nearest: past them the far cover alone)
  if (static_cast<int>(found_.size()) > kMost) {
    std::nth_element(found_.begin(), found_.begin() + kMost, found_.end(), [](const Found& a, const Found& b) { return a.dNear < b.dNear; });
    found_.resize(kMost);
  }
  // the tiles by how many corners their spots need, a draw each; the nearest with models given rows of flags
  std::sort(found_.begin(), found_.end(), [](const Found& a, const Found& b) { return a.corners != b.corners ? a.corners < b.corners : a.dNear < b.dNear; });
  rows.reserve(found_.size() * 4);
  for (const Found& f : found_) {
    float slot = -1;
    if (f.model && static_cast<int>(modelRows.size() / 4) < kMostModelTiles) {
      slot = static_cast<float>(modelRows.size() / 4);
      modelRows.insert(modelRows.end(), {static_cast<float>(f.x * kSide), static_cast<float>(f.z * kSide), 0, f.dNear});
    }
    rows.insert(rows.end(), {static_cast<float>(f.x * kSide), static_cast<float>(f.z * kSide), slot, 0});
    if (batches.empty() || batches.back().corners != f.corners) batches.push_back({f.corners, Count() - 1, 0});
    ++batches.back().count;
  }
}

void PlantTiles::Visit(const View& view, int tx, int tz, int span, int tilesX, int tilesZ, float tileWide, float cell,
                       const std::vector<CellSize>& cells, const PlantLodTune& tune, float tallest, float reach) {
  if (tx >= tilesX || tz >= tilesZ) return;
  const float x0 = static_cast<float>(tx) * tileWide, z0 = static_cast<float>(tz) * tileWide;
  const float x1 = std::min(static_cast<float>(std::min(tx + span, tilesX)) * tileWide, static_cast<float>(width_));
  const float z1 = std::min(static_cast<float>(std::min(tz + span, tilesZ)) * tileWide, static_cast<float>(height_));
  float low, high;
  Range(x0, z0, x1, z1, &low, &high);
  if (high < water_ + 0.08f) return;  // (all of it under the water: nothing grows there)
  const Vec3 lo{x0, low, z0}, hi{x1, high + tallest, z1};
  if (!view.BoxVisible(lo, hi)) return;
  const Vec3 e = view.eye;
  const Vec3 nearest{std::clamp(e.x, lo.x, hi.x), std::clamp(e.y, lo.y, hi.y), std::clamp(e.z, lo.z, hi.z)};
  const float dNear = std::max(Length(nearest - e), 0.01f);
  if (dNear > reach) return;
  if (span > 1) {
    const int half = span / 2;
    Visit(view, tx, tz, half, tilesX, tilesZ, tileWide, cell, cells, tune, tallest, reach);
    Visit(view, tx + half, tz, half, tilesX, tilesZ, tileWide, cell, cells, tune, tallest, reach);
    Visit(view, tx, tz + half, half, tilesX, tilesZ, tileWide, cell, cells, tune, tallest, reach);
    Visit(view, tx + half, tz + half, half, tilesX, tilesZ, tileWide, cell, cells, tune, tallest, reach);
    return;
  }
  // how many plants a spot needs here at most (its largest on screen at the tile's nearest point): all of a clump while
  // it is not yet drawn as one, else one; none below the cut. And whether a model may draw one here.
  int most = 0;
  bool model = false;
  for (const CellSize& c : cells) {
    if (c.frame <= 0) continue;
    const float px = c.frame * focal / dNear;
    if (px <= tune.cutLow * cutScale) continue;
    if (c.model && px * (1 + 0.5f * tune.modelBand) >= c.modelDownTo) model = true;
    if (c.cardless) continue;  // (never a card: no corners for it)
    most = std::max(most, px > tune.collapseLow ? c.clumpMost : 1);
  }
  if (most <= 0 && !model) return;
  found_.push_back({tx, tz, 18 * std::clamp(most, 1, 6), model, dNear});
}

// ---------------------------------------------------------------------------------------------------------------------

PlantModels::~PlantModels() {
  for (Kind& k : kinds)
    for (Level& l : k.levels) {
      if (l.vertices) device_->Destroy(l.vertices);
      if (l.indices) device_->Destroy(l.indices);
    }
  for (gfx::Texture t : {instances_, counters_, flags_})
    if (t) device_->Destroy(t);
}

namespace {

// One level's mesh from an .scm file (version 5, or 6 with 32-bit indices: models.cpp, LoadScm): its vertices as stored
// (ScmVertex, 68 bytes), its triangles, its height (model units).
bool ReadLevel(gfx::Device* device, const std::string& file, PlantModels::Level* level, std::string* why) {
  if (file.size() < 48 || file.compare(0, 4, "MODL") != 0) {
    *why = "not an .scm file";
    return false;
  }
  int32_t h[11];
  std::memcpy(h, file.data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  if (h[0] != 5 && h[0] != 6) {
    *why = "unsupported .scm version";
    return false;
  }
  const int64_t indexBytes = h[0] == 6 ? 4 : 2;
  constexpr int64_t kVertex = 68;
  if (vertexCount <= 0 || indexCount <= 0 || vertexOffset < 0 || indexOffset < 0 || vertexOffset + vertexCount * kVertex > static_cast<int64_t>(file.size()) ||
      indexOffset + indexCount * indexBytes > static_cast<int64_t>(file.size())) {
    *why = "the .scm file is cut short";
    return false;
  }
  float top = 0;
  for (int64_t i = 0; i < vertexCount; ++i) {
    float y;
    std::memcpy(&y, file.data() + vertexOffset + i * kVertex + 4, sizeof y);
    top = std::max(top, y);
  }
  gfx::BufferDesc vb;
  vb.stride = static_cast<uint32_t>(kVertex);
  vb.size = static_cast<uint32_t>(vertexCount * kVertex);
  vb.data = file.data() + vertexOffset;
  level->vertices = device->CreateBuffer(vb);
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = static_cast<uint32_t>(indexBytes);
  ib.size = static_cast<uint32_t>(indexCount * indexBytes);
  ib.data = file.data() + indexOffset;
  level->indices = device->CreateBuffer(ib);
  level->indexCount = static_cast<uint32_t>(indexCount);
  level->indexBytes = static_cast<uint32_t>(indexBytes);
  level->height = top > 0 ? top : 1;
  if (!level->vertices || !level->indices) {
    *why = "the device refused the mesh";
    return false;
  }
  return true;
}

}  // namespace

bool PlantModels::Set(const JsonValue& cover, TextureCache* textures, const std::string& folder, const std::vector<std::string>& cellNames) {
  for (Kind& k : kinds)
    for (Level& l : k.levels) {
      if (l.vertices) device_->Destroy(l.vertices);
      if (l.indices) device_->Destroy(l.indices);
    }
  kinds.clear();
  const JsonValue& list = cover["models"]["kinds"];
  if (list.kind != JsonValue::Object) return false;
  for (const auto& [name, k] : list.members) {
    if (static_cast<int>(kinds.size()) >= kMostKinds) break;
    Kind kind;
    kind.name = name;
    // (the card it ends on: its cell's name, or its index; null: none, it takes a share of a group's spots instead)
    const JsonValue& card = k["card"];
    if (card.kind == JsonValue::String) {
      const std::string c = card.StringOr(std::string());
      for (size_t i = 0; i < cellNames.size(); ++i)
        if (cellNames[i] == c) kind.card = static_cast<int>(i);
      if (kind.card < 0) {
        fprintf(stderr, "render: the cover's model %s names a card the cover has not (%s); left out\n", name.c_str(), c.c_str());
        continue;
      }
    } else if (card.kind == JsonValue::Number) {
      kind.card = static_cast<int>(card.NumberOr(-1));
      if (kind.card < 0 || kind.card >= 16) {
        fprintf(stderr, "render: the cover's model %s names no card of the cover; left out\n", name.c_str());
        continue;
      }
    }
    if (kind.card >= 0)
      for (const Kind& other : kinds)
        if (other.card == kind.card) {
          fprintf(stderr, "render: the cover's models %s and %s end on the same card; %s left out\n", other.name.c_str(), name.c_str(), name.c_str());
          kind.card = -2;
        }
    if (kind.card == -2) continue;
    const JsonValue& place = k["place"];
    if (place.kind == JsonValue::Object) {
      const std::string g = place["group"].StringOr(std::string("fern"));
      kind.placeGroup = g == "clump" || g == "grass" ? 0 : g == "dry" ? 1 : g == "weed" ? 2 : g == "flower" ? 3 : g == "stalks" ? 4 : 5;
      kind.placeShare = std::clamp(static_cast<float>(place["share"].NumberOr(0.3)), 0.0f, 1.0f);
    }
    kind.sway = static_cast<float>(k["sway"].NumberOr(0.6));
    if (k["clump"].items.size() == 2) {
      kind.clump[0] = std::max(1, static_cast<int>(k["clump"].items[0].NumberOr(1)));
      kind.clump[1] = std::clamp(static_cast<int>(k["clump"].items[1].NumberOr(1)), kind.clump[0], 6);
    }
    std::string why;
    bool ok = true;
    for (const JsonValue& lod : k["lods"].items) {
      if (kind.levelCount >= kLevels) break;
      const std::string path = folder + lod.StringOr(std::string());
      const std::optional<std::string> file = textures->ReadFile(path);
      if (!file || !ReadLevel(device_, *file, &kind.levels[kind.levelCount], &why)) {
        fprintf(stderr, "render: the cover's model %s: %s (%s); left out\n", name.c_str(), path.c_str(), file ? why.c_str() : "not found");
        ok = false;
        break;
      }
      ++kind.levelCount;
    }
    if (!ok || kind.levelCount == 0) continue;
    kind.albedo = textures->Get(folder + k["albedo"].StringOr(std::string()));
    kind.normal = textures->Get(folder + k["normal"].StringOr(std::string()), textures->FlatNormal());
    kind.spec = textures->Get(folder + k["spec"].StringOr(std::string()));
    kind.ao = textures->Get(folder + k["ao"].StringOr(std::string()), textures->White());
    if (!kind.albedo) {
      fprintf(stderr, "render: the cover's model %s has no albedo it can read; left out\n", name.c_str());
      continue;
    }
    const JsonValue& size = k["size_m"].items.empty() ? k["size"] : k["size_m"];
    for (size_t i = 0; i < 3 && i < size.items.size(); ++i) kind.size[i] = static_cast<float>(size.items[i].NumberOr(1));
    if (size.items.size() == 2) kind.size[2] = kind.size[1];  // ([w, h])
    const JsonValue& lin = k["albedo_lin"];
    const JsonValue& srgb = k["albedo_srgb8"];
    for (size_t i = 0; i < 3; ++i)
      kind.mean[i] = lin.items.size() == 3 ? static_cast<float>(lin.items[i].NumberOr(0))
                       : srgb.items.size() == 3 ? std::pow(static_cast<float>(srgb.items[i].NumberOr(0)) / 255.0f, 2.2f)
                                                : 0.0f;
    const JsonValue& swap = k["swap"];
    for (size_t i = 0; i < 3 && i < swap.items.size(); ++i) kind.swap[i] = static_cast<float>(swap.items[i].NumberOr(0));
    kinds.push_back(std::move(kind));
  }
  if (!kinds.empty()) {
    std::string names;
    for (const Kind& k : kinds)
      names += (names.empty() ? "" : ", ") + k.name + " (" + std::to_string(k.levelCount) + " levels, " +
               (k.card >= 0 ? "then its card " + std::to_string(k.card) : "no card: " + std::to_string(static_cast<int>(k.placeShare * 100 + 0.5f)) +
                                                                            "% of group " + std::to_string(k.placeGroup) + "'s spots") +
               ")";
    printf("render: the ground cover's 3D plants: %s\n", names.c_str());
  }
  return !kinds.empty();
}

int PlantModels::KindOfCell(int cell) const {
  for (size_t i = 0; i < kinds.size(); ++i)
    if (kinds[i].card == cell) return static_cast<int>(i);
  return -1;
}

gfx::Texture PlantModels::Instances() {
  if (!instances_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = 2 * kInstancesARow;
    td.height = kRowsABucket * kMostKinds * kLevels;
    td.format = gfx::Format::RGBA32F;
    instances_ = device_->CreateTarget(td);
  }
  return instances_;
}

gfx::Texture PlantModels::Counters() {
  if (!counters_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = 64;
    td.height = 1;
    td.format = gfx::Format::R32UI;
    counters_ = device_->CreateTarget(td);
  }
  return counters_;
}

gfx::Texture PlantModels::Flags() {
  if (!flags_) {
    gfx::TargetDesc td;
    td.storage = true;
    td.width = 256;
    td.height = PlantTiles::kMostModelTiles;
    td.format = gfx::Format::R32UI;
    flags_ = device_->CreateTarget(td);
  }
  return flags_;
}

}  // namespace render
