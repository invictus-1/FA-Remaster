#include "grass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "lighting.h"
#include "plants.h"  // [plants]
#include "ray_scene.h"

namespace render {
namespace {

// [cover] What the blades' and the plants' shaders share (after their constants): where the ground is bare, how much of a
// tuft or a plant is there as the cover comes in, and the meadow's colour for a blade.
const char kCoverShared[] = R"hlsl(
Texture2D<float> bareGround : register(t12);  // over the map: 1 where it is bare (GroundCover::bare, cover_bare.h)
// How much of a tuft or a plant is there (its height's share) by how much of the cover is (`shown`, 0..1) and a number of
// its own (0..1): nothing while `shown` is under its number, then growing to its full height as `shown` passes it. So the
// cover comes in one tuft and one plant at a time, each growing out of the ground: no sudden appearance.
float CoverGrown(float shown, float own) { return saturate((shown * 1.25 - own) * 4); }
// A blade's colour (linear) from the ground's under it: the ground's own where it is green or straw, but never the
// brown of bare earth (a blade at a trail's edge, on a patch of soil, would be a brown stick): the red and blue held to
// what a dry blade has, at the ground's brightness. Varied over the meadow in patches some units across (yellower,
// bluer, paler, darker), as a real meadow is never one colour.
float3 MeadowColour(float3 ground, float2 xz) {
  const float3 luma = float3(0.2126, 0.7152, 0.0722);
  float bright = dot(ground, luma);
  float3 c = float3(min(ground.r, ground.g * 1.0), ground.g, min(ground.b, ground.g * 0.6));
  c *= bright / max(dot(c, luma), 1e-4);
  float hue = CloudNoise(float3(xz * 0.31, 7.7)), pale = CloudNoise(float3(xz * 0.93 + 3.1, 2.3));
  c *= lerp(float3(1.08, 1.0, 0.80), float3(0.90, 1.0, 1.10), hue);
  // (fresh blades a little lighter and richer than the ground's mean, which mixes in the soil between them: else they
  // vanish into it)
  c = max(lerp(dot(c, luma), c, 1.25), 0);
  return c * (0.95 + 0.4 * pale);
}
// [leftover] Under lying snow (GroundCover::snowDepth): how deep the snow lies over a root, in world units, as the ground's
// light lays it (lighting.h: its patches, level ground first, by how much lies, the view's weather.y) and, with the snow
// ground's shelter read (World::snowGround; terrain.cpp's SnowAround), deeper in hollows and the wind's lee and thinner under
// the trees' crowns and on steep and windward ground; times coverSnow.x at the most. Its share of a full cover (0..1) in
// `share`. `ground`: the ground's normal there; `map`: the map's width and height.
cbuffer CoverSnow : register(b3) {
  float4 coverSnow;      // x: world units the snow lies at the most where all of it lies (0: the cover as without snow); y: 1 = the shelter is read;
                         // z: [plantlod] 1 = the ground lays the map's snow materials (their mottled margin: GroundCover::snowMaterials)
  float4 coverSnowWind;  // xy: the way the wind blows (unit length); z: the trees' crowns' weight, w: the drifts' (Terrain::snowTune y, w)
};
Texture2D snowShelter : register(t13);  // (Terrain::SnowShelter) g the sky it sees, b the trees' crowns over it, a how it lies
float CoverSnowDepth(float2 xz, float y, float3 ground, float2 map, out float share) {
  float lie = 1, less = 0;
  [branch] if (coverSnow.y > 0.5) {
    float4 s = snowShelter.SampleLevel(samplerLinearClamp, xz / map, 0);
    float crowns = s.b, lay = s.a * 2 - 1, slope = length(ground.xz);
    float lee = slope > 1e-3 ? dot(ground.xz / slope, coverSnowWind.xy) * saturate(slope * 5) : 0;
    lie = (1 + 0.4 * lay * coverSnowWind.w + 0.15 * lee) * (1 - 0.3 * crowns * coverSnowWind.z);
    less = 0.08 * crowns * coverSnowWind.z + 0.25 * saturate((0.85 - ground.y) * 4) + 0.1 * saturate(-lee) + 0.3 * saturate(-lay) * coverSnowWind.w;
  }
  float fallen = weather.y * lie;
  float patch = CloudNoise(float3(xz.x, y, xz.y) * 0.9) * 0.55 + CloudNoise(float3(xz * 0.21, 5.3)) * 0.45;
  float takes = patch * 0.75 + saturate((ground.y - 0.35) * 1.6) * 0.25 - less;
  float bar = 1.02 - fallen * 1.15;
  // (with the shelter, over the snow ground's wider margin)
  float lying = (coverSnow.y > 0.5 ? smoothstep(bar - 0.15, bar + 0.35, takes) : smoothstep(bar, bar + 0.2, takes)) * smoothstep(0.25, 0.5, ground.y);
  // [plantlod] with the map's snow materials laid the ground's margin is mottled (thinner snow, islands and holes) as the marks
  // and the map's low plants take it (decals.cpp's MarkSnowLying, under_snow.h): the cover green where the ground is bare
  [branch] if (coverSnow.z > 0.5) {
    float mottle = CloudNoise(float3(xz * 0.45, 1.3)) * 0.6 + CloudNoise(float3(xz * 1.3, 4.7)) * 0.4;
    lying = saturate(lying + (mottle - 0.5) * 3.2 * lying * (1 - lying));
  }
  share = saturate(lying * fallen);
  // (never quite even: a little deeper and shallower in patches a unit or so across, so that what stands out of it does so
  // in places, not as the same tips everywhere)
  return lying * fallen * coverSnow.x * (0.75 + 0.5 * CloudNoise(float3(xz * 0.8, 2.9)));
}
// What stands out of the snow at the least (world units of a blade or plant `tall`): less is under it (the snow is uneven
// round each stem; a tip a hair over it would read as a speck of dirt)
float CoverSnowOut(float tall) { return max(0.025, 0.3 * tall); }
#ifdef MOTION
// [leftover] Motion (motion.h; GroundCover::DrawMotion): each corner drawn again where the scene drew it, writing where the
// same corner was the frame before (the wind at that frame's time, what presses it as long ago less the frame's seconds;
// the blasts' push as now, their field of the frame before not being kept), both by the views without the jitter.
cbuffer CoverMotion : register(b2) {
  row_major float4x4 motionNow;     // this frame's view without the jitter
  row_major float4x4 motionBefore;  // the frame before's
  float4 motionWhen;                // x: the frame before's time; y: the seconds between the two
};
float2 CoverMoved(float4 here, float4 then) { return (then.xy / then.w - here.xy / here.w) * float2(0.5, -0.5); }
#endif
)hlsl";

// Fifteen corners a blade (three segments, the last a point): no vertex data; a blade's number gives its cell.
const char kShader[] = R"hlsl(
cbuffer Grass : register(b1) {
  float4 area;     // xy: the first cell's world x, z; z: a cell's size; w: cells along a side
  float4 blades;   // x: the tallest blade; y: its width at the foot; z: how far from the eye it thins out to nothing;
                   // w: world units of the heights' largest value
  float4 mapInfo;  // xy: the map's width and height; z: world units a pixel covers at distance 1; w: the water's level
  float4 table;    // x: 1 where the map's cover table says how many blades grow (densityA's red), else by how grassy;
                   // [tarmac] y: 1 = none where a remastered pad lies (padCover)
  float4 shown;    // [cover] x: how much of the cover is there (GroundCover::Prepare: the camera's distance, since it came
                   // in); y: 1 = the bare ground's picture is read (bareGround)
#ifdef PLANTS
  float4 plantsK[2];  // [plants] (plants.h) PlantLife::Constants
#endif
};
Texture2D<float> heightMap : register(t0);
Texture2D cover : register(t1);          // Terrain::GroundColours: rgb the ground's colour, a how grassy
Texture2D groundNormals : register(t2);  // x in alpha, z in green
Texture2D<float> trampled : register(t3);  // over the square: 1 where what stands there presses the grass flat
Texture2D densityA : register(t4);       // Terrain::CoverDensity's first: blades, clumps, flowers, dry stalks
#ifdef BLASTS
Texture2D<float2> blastField : register(t8);  // (blasts.h) over the square: which way and how far a blast's wave bends it
#endif
Texture2D<float> padCover : register(t11);  // [tarmac] over the square: where a remastered pad lies (GroundCover::cleared; table.y)

uint GrassHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float GrassRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }

float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * blades.w;
}

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 colour : TEXCOORD1;  // linear
  float3 normal : TEXCOORD2;  // ([cover] folded along its middle: each edge's turned a little to its own side)
  float up : TEXCOORD3;       // 0 at the foot, 1 at the tip
  float shadow : TEXCOORD4;
  float through : TEXCOORD5;  // the sun's light through the blade (World::leafLight; lighting.h, LeafThrough)
#ifdef PLANTS
  float3 ember : TEXCOORD6;   // [plants] a burnt blade's embers' light
#endif
#ifdef MOTION
  float4 here : TEXCOORD9;    // [leftover] (this frame's place and the frame before's, by the views without the jitter)
  float4 then : TEXCOORD10;
#endif
};

// (level along the blade: 0 the foot .. 3 the tip; side 0 / 1)
static const uint2 kCorner[15] = {uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1), uint2(1, 0), uint2(1, 1),
                                  uint2(2, 0), uint2(2, 0), uint2(1, 1), uint2(2, 1), uint2(2, 0), uint2(2, 1), uint2(3, 0)};
static const float kUp[4] = {0, 0.4, 0.75, 1};
static const float kWide[4] = {1, 0.75, 0.45, 0};

VOut VS(uint id : SV_VertexID) {
  VOut o;
  o.position = float4(0, 0, -1, 1);  // (no blade: behind the eye)
  o.world = o.colour = o.normal = 0;
  o.up = o.shadow = o.through = 0;
#ifdef PLANTS
  o.ember = 0;
#endif
#ifdef MOTION
  o.here = o.then = float4(0, 0, 0, 1);
#endif
  uint blade = id / 15, corner = id % 15;
  uint tuft = blade / 3, k = blade % 3;  // (three blades a cell, from close roots)
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(tuft % side, tuft / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));  // (the cell's whole-number place in the world: the pattern stays)
  uint h = GrassHash(uint(cellId.x) * 73856093u ^ GrassHash(uint(cellId.y) * 19349663u));
  float2 xz = cellAt + float2(GrassRand(h), GrassRand(GrassHash(h + 1))) * area.z;
  uint hb = GrassHash(h + 101 * (k + 1));  // (this blade of the tuft: its own root near the tuft's, its own shape)
  xz += (float2(GrassRand(hb), GrassRand(GrassHash(hb + 1))) - 0.5) * 0.07;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  float4 c = cover.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float ground = GroundAt(xz);
  if (ground < mapInfo.w + 0.08) return o;  // (none in or at the water)
  float3 root = float3(xz.x, ground, xz.y);
  float far = length(root - cameraPosition);
  // (fewer toward the square's edge, none at it: by the distance from what the camera looks at, its middle)
  float half = area.z * area.w * 0.5;
  float thin = saturate((1 - length(xz - (area.xy + half)) / half) * 3);
  const float grassy = table.x > 0.5 ? densityA.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0).r : c.a;
  // [cover] in tufts and gaps, not evenly strewn: the tufts gathered where a noise of the place (patches a unit or two
  // across) is high, fewer between them, as many over all
  float gathered = 0.3 + 1.4 * smoothstep(0.25, 0.75, CloudNoise(float3(xz * 0.7, 1.7)));
  if (GrassRand(GrassHash(h + 2)) > grassy * thin * gathered) return o;
  // [tarmac] none where a remastered pad lies, by its coverage (its worn rim thinned)
  [branch] if (table.y > 0.5 && padCover.SampleLevel(samplerLinearClamp, (xz - area.xy) / (area.z * area.w), 0) > GrassRand(GrassHash(h + 12))) return o;
  // [cover] none where the ground is bare (a stream, a trail), by how bare; lower where it thins out
  float bare = 0;
  [branch] if (shown.y > 0.5) {
    bare = bareGround.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
    if (bare > GrassRand(GrassHash(h + 13))) return o;
  }
  // [cover] as the cover comes in (the camera coming near, the map opened): this tuft growing when its turn comes
  const float grown = CoverGrown(shown.x, GrassRand(GrassHash(h + 14)));
  if (grown <= 0) return o;
  // (its height: its own, and [cover] taller and shorter grass in patches some units across)
  float height = blades.x * (0.7 + 0.3 * GrassRand(GrassHash(hb + 3))) * (0.55 + 0.45 * CloudNoise(float3(xz * 0.23, 4.1))) * saturate(thin * 2.5);
  height *= grown * (1 - 0.5 * bare);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, (xz - area.xy) / (area.z * area.w), 0);  // (pressed flat)
  float turn = GrassRand(GrassHash(hb + 4)) * 6.2832;
  float2 face = float2(cos(turn), sin(turn)), across = float2(-face.y, face.x);
  // its lean, and the wind the trees sway in (mesh_renderer.cpp: a slow swing and a quicker one, in gusts that
  // run across the land, harder in rain)
  float lean = 0.12 + 0.22 * GrassRand(GrassHash(hb + 5));
  float2 leanWay = float2(cos(turn * 1.7 + 1), sin(turn * 1.7 + 1));
  float beat = dot(xz, float2(0.11, 0.07));
  float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((xz - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
  float2 bend = leanWay * lean + float2(0.915, 0.403) * (swing * gust * 0.45);
#ifdef MOTION
  // [leftover] the frame before's: the wind at its time, the rest as now (and as below)
  float swingB = sin(motionWhen.x * 1.3 + beat) * 0.6 + sin(motionWhen.x * 2.9 + beat * 1.7) * 0.4;
  float gustB = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((xz - float2(0.915, 0.403) * (motionWhen.x * 7)) * 0.02, motionWhen.x * 0.07)));
  float2 bendB = leanWay * lean + float2(0.915, 0.403) * (swingB * gustB * 0.45);
  float heightB = height;
#endif
#ifdef PLANTS
  // [plants] pressed by what drove over it: laid over the way it was pushed (its height going to its lean, as in a blast's
  // wave), rising again over some seconds; burnt where the ground is (black, then brown, and short), its own share of a
  // half burnt place
  float4 pressedBy = PlantsPressAt(xz, plantsK[0], plantsK[1]);
  [branch] if (pressedBy.z > 0.01) {
    float down = pressedBy.z * exp(-(pressedBy.w / pressedBy.z) / 6.0) * (0.85 + 0.3 * GrassRand(GrassHash(hb + 8)));
    float2 way = pressedBy.xy / pressedBy.z;
    float layOver = min(down * 1.3, 1.3);
    if (layOver > 1e-3 && length(way) > 0.05) {
      height *= cos(layOver);
      bend += normalize(way) * tan(layOver);
    } else {
      height *= 1 - 0.9 * saturate(down);
    }
#ifdef MOTION
    {  // [leftover] (the frame before: pressed as long ago less the frame's seconds)
      float downB = pressedBy.z * exp(-max(pressedBy.w / pressedBy.z - motionWhen.y, 0) / 6.0) * (0.85 + 0.3 * GrassRand(GrassHash(hb + 8)));
      float layOverB = min(downB * 1.3, 1.3);
      if (layOverB > 1e-3 && length(way) > 0.05) {
        heightB *= cos(layOverB);
        bendB += normalize(way) * tan(layOverB);
      } else {
        heightB *= 1 - 0.9 * saturate(downB);
      }
    }
#endif
  }
  float4 burnt = PlantsBurnLook(xz, plantsK[0], plantsK[1], GrassRand(GrassHash(hb + 9)));
  height *= 1 - 0.8 * burnt.z;
#ifdef MOTION
  heightB *= 1 - 0.8 * burnt.z;
#endif
#endif
#ifdef BLASTS
  // a blast's wave: laid over away from it (as far as the field says, radians from upright), its height going to its
  // lean so that the blade keeps about its length
  float2 blown = blastField.SampleLevel(samplerLinearClamp, (xz - area.xy) / (area.z * area.w), 0);
  float over = length(blown), laid = min(over, 1.35);
  if (laid > 1e-3) {
    height *= cos(laid);
    bend += blown / over * tan(laid);
#ifdef MOTION
    heightB *= cos(laid);
    bendB += blown / over * tan(laid);
#endif
  }
#endif
  // [leftover] under lying snow: the blade's foot at the snow's top and only what stands out over it (none: buried, the
  // shorter first), bowed by the snow on it, dusted white toward its tip (its colour, below)
  float snowDust = 0;
  [branch] if (weather.y > 0 && coverSnow.x > 0) {
    float4 under = groundNormals.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
    float3 g;
    g.xz = under.ag * 2 - 1;
    g.y = sqrt(saturate(1 - dot(g.xz, g.xz)));
    float deep = CoverSnowDepth(xz, ground, g, mapInfo.xy, snowDust);
    if (deep > 0 && height - deep < CoverSnowOut(height)) return o;
    root.y += deep;
    height -= deep;
    bend += leanWay * (0.6 * snowDust);
#ifdef MOTION
    heightB = max(heightB - deep, 0);
    bendB += leanWay * (0.6 * snowDust);
#endif
  }
  uint2 at = kCorner[corner];
  float v = kUp[at.x];
  // (at least most of a pixel wide, or far blades would flicker in and out between pixels)
  float wide = max(blades.y, mapInfo.z * far * 0.8) * kWide[at.x];
  float3 p = root + float3(bend.x * v * v * height, v * height, bend.y * v * v * height);
  p.xz += across * ((at.y ? 0.5 : -0.5) * wide);
  o.position = mul(viewProj, float4(p, 1));
#ifdef MOTION
  float3 pB = root + float3(bendB.x * v * v * heightB, v * heightB, bendB.y * v * v * heightB);
  pB.xz += across * ((at.y ? 0.5 : -0.5) * wide);
  o.here = mul(motionNow, float4(p, 1));
  o.then = mul(motionBefore, float4(pB, 1));
#else
  o.world = p;
  o.up = v;
  // its colour: [cover] the meadow's from the ground's under it (MeadowColour: never bare earth's brown, varied in
  // patches), each blade a little its own, lighter toward the tip; a share of the blades drier toward the tip (late
  // summer), paler and yellower at about their own brightness, not a bright straw that stands out of the green
  float3 base = MeadowColour(ToLinear(c.rgb), xz) * (0.85 + 0.3 * GrassRand(GrassHash(hb + 6)));
  float dry = GrassRand(GrassHash(hb + 7)) < 0.14 ? 1 : 0;
  float3 straw = float3(1.0, 0.86, 0.5) * (dot(base, float3(0.2126, 0.7152, 0.0722)) * 1.35 / 0.864);
  o.colour = lerp(base * lerp(0.8, 1.35, v), straw, dry * v * 0.7);
  // lit as the ground under it, a little toward the blade's own face, [cover] folded along its middle (each edge turned a
  // little its own way: a blade is never flat, and so it shades across)
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  o.normal = normalize(n + float3(face.x, 0, face.y) * 0.3 + float3(across.x, 0, across.y) * (at.y ? 0.35 : -0.35));
  // [cover] the sun's shadow where this corner is (a little off the ground), not one for the whole blade: as the camera
  // zooms and the shadow maps change, a blade at a shadow's edge is not all lit, then all dark
  o.shadow = SunShadow(p + n * 0.02, n);
  // the sun's light through it (World::leafLight): the side the eye sees faces it; thinner, more toward the tip
  float3 toEye = normalize(cameraPosition - p), faceWay = float3(face.x, 0, face.y);
  o.through = leaves.x > 0.5 ? LeafThrough(dot(faceWay, toEye) < 0 ? -faceWay : faceWay, toEye) * (0.4 + 0.6 * v) : 0;
#ifdef PLANTS
  // [plants] burnt: its colour, no light through it, its embers glowing toward the tip for the first seconds
  o.colour = PlantsBurnColour(o.colour, burnt);
  o.through *= 1 - saturate(burnt.x + burnt.y);
  o.ember = PlantsEmber(burnt.w, xz, time) * (v * (falling.w > 0 ? falling.w : 1));
#endif
#ifdef BLASTS
  // (grass laid flat is paler: its blades lie open to the sky and show their backs, no longer shading each other; so
  // the wave shows as a pale ring running out over the grass)
  float open = sin(laid);
  o.colour *= 1 + 0.45 * open;
  o.up = lerp(o.up, 1, open);
#endif
  [branch] if (snowDust > 0) o.colour = lerp(o.colour, SNOW_COLOUR, snowDust * (0.3 + 0.55 * v));  // [leftover] (dusted, the more toward its tip)
#endif
  return o;
}

#ifdef MOTION
float2 MotionPS(VOut i) : SV_Target { return CoverMoved(i.here, i.then); }  // [leftover]
#endif

float4 PS(VOut i) : SV_Target {
  float occlusion = lerp(0.55, 1, i.up);  // (darker down among the blades: [cover] they shade each other near the ground)
#if RAYS
  rayLightsAtMost = 1;  // (one ray for the small lights' shadows; lighting.h, SmallLightsTraced)
#endif
  // [leftover] under lying snow the blade is dusted by its own measure (its colour, CoverSnowDepth), not whitened all over
  // where the light's snow would lie on what faces up as the ground does
  [branch] if (weather.y > 0 && coverSnow.x > 0) surfaceSnowShare = 0;
  float3 through = LeafLight(i.colour, i.through, i.world, i.shadow, 0.8);
#ifdef PLANTS
  through += i.ember;  // [plants]
#endif
  return float4(ModernLight(i.colour, 0, 0.85, occlusion, through, normalize(i.normal), i.world, i.shadow), 1);
}
)hlsl";

struct GrassConstants {
  float area[4], blades[4], mapInfo[4], table[4];
  float shown[4];       // [cover]
  float plantsK[2][4];  // [plants] (read by the shaders with them only)
};
// [leftover] (kCoverShared's CoverSnow, at register b3; CoverMotion at b2)
struct CoverSnowConstants {
  float snow[4], wind[4];
};
struct CoverMotionConstants {
  Mat4 now, before;
  float when[4];
};

// The plants (GroundCover::SetPlants): a spot a cell, 108 corners a spot. [cover] Up to six plants a spot (a clump's), each
// a star of three cards crossing at its foot (eighteen corners); or, for the grass's clumps, the spot one tuft of up to
// twelve real blades (nine corners each: a quad from the foot to the middle, then the tip).
const char kPlantDecls[] = R"hlsl(
cbuffer Plants : register(b1) {
  float4 area;      // xy: the first cell's world x, z; z: a cell's size; w: cells along a side
  float4 mapInfo;   // xy: the map's width and height; z: world units of the heights' largest value; w: the water's level
  float4 info;      // x: spots a cell where the table says 1; y: [tarmac] 1 = none where a remastered pad lies (padCover);
                    // z: flowers' patches' size; w: their cover
  float4 shown;     // [cover] x: how much of the cover is there (as the blades'); y: 1 = the bare ground's picture is read;
                    // z: world units a pixel covers at distance 1; w: a blade's width at its foot (the meadow's)
  float4 cells[48]; // per cell three: uv rectangle; frame, low, high height, height (metres); sway, clump's least, most,
                    // its world units a metre
  float4 order[4];  // the cells by group (16 indices)
  float4 groups[6]; // per group: where its cells start in `order`, how many
  float4 tones[16]; // per cell: rgb its mean colour (linear); w [coverdark] above 0: how far it takes the ground's colour,
                    // [cover] below 0: how far its brightness goes toward the ground's (0: the art's own)
#ifdef PLANTS
  float4 plantsK[2];  // [plants] (plants.h) PlantLife::Constants
#endif
#ifdef PLANT_LOD
#ifndef PLANTS
  float4 plantsKUnused[2];  // [plantlod] (their place kept: the constants are laid out alike with and without)
#endif
  float4 lodInfo[4];      // [plantlod] (plant_lod.h; GroundCover::FillPlantConstants says what each holds)
  float4 cellModels[16];  // [plantlod] per cell: x its model's kind (-1: none); y, z, w the pixels its levels 0, 1, 2 go down to
  float4 modelKinds[8];   // [plantlod] per kind: x its levels, y 1 = it ends on a card
  float4 extraCells[24];  // [plantlod] per kind with no card (cell 16 + its number): its model (as cellModels), size, more (as cells)
  float4 extraRules[8];   // [plantlod] the same: x the group whose spots it takes, y the share of them
  float4 extraTones[8];   // [plantlod] the same: as tones (its leaves' mean colour; w below 0: its brightness toward the ground's)
  float4 modelTint[8];    // [plantlod] per kind: rgb its colour times this (its leaves' mean brought to its card's); w its
                          // card's width times this (the model's width over the card's frame)
  float4 farField;        // [plantlod] the far field (farCleared): its first x, z; 1 / its width; 1 = made this frame
#endif
};
Texture2D<float> heightMap : register(t0);
Texture2D cover : register(t1);          // Terrain::GroundColours: rgb the ground's colour ([coverdark] the grass's clumps take it)
Texture2D groundNormals : register(t2);  // x in alpha, z in green
Texture2D<float> trampled : register(t3);
Texture2D densityA : register(t4);       // blades, clumps, flowers, dry stalks
Texture2D densityB : register(t5);       // ferns, dry cells' share, weeds' share
Texture2D atlas : register(t6);          // the plants' albedo (sRGB values; alpha their coverage, tested at a half)
Texture2D atlasNormals : register(t7);   // card space: x in alpha, y (up) in green (BC5 read as the game's DXT5)
#ifdef BLASTS
Texture2D<float2> blastField : register(t8);  // (blasts.h) over the square: which way and how far a blast's wave bends it
#endif
Texture2D<float> padCover : register(t11);  // [tarmac] (as the blades')

uint PlantHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float PlantRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
float PlantGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}
// flowers in loose patches: a smooth noise of the place, so many patches across (info.z apart), info.w of the ground
float PlantPatch(float2 xz) {
  float2 g = xz / max(info.z, 0.1);
  float2 i = floor(g), f = g - i;
  f = f * f * (3 - 2 * f);
  float a = PlantRand(PlantHash(uint(int(i.x)) * 73856093u ^ PlantHash(uint(int(i.y)) * 19349663u)));
  float b = PlantRand(PlantHash(uint(int(i.x) + 1) * 73856093u ^ PlantHash(uint(int(i.y)) * 19349663u)));
  float c = PlantRand(PlantHash(uint(int(i.x)) * 73856093u ^ PlantHash(uint(int(i.y) + 1) * 19349663u)));
  float d = PlantRand(PlantHash(uint(int(i.x) + 1) * 73856093u ^ PlantHash(uint(int(i.y) + 1) * 19349663u)));
  float n = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
  float cover = max(info.w, 0.05);
  return saturate((n - (1 - cover)) / 0.08) / cover;  // (the same flowers in all, gathered in the patches)
}
uint OrderAt(uint i) { return uint(order[i / 4][i % 4]); }
float PlantLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)hlsl";
// (the plants' shader goes on; with plantsFar plant_lod.h's kPlantLodBlock comes between)
const char kPlantShader[] = R"hlsl(
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float3 right : TEXCOORD2;
  float3 up : TEXCOORD3;
  float3 toward : TEXCOORD4;   // a card's face (one of its two sides: the pixel takes the one the eye sees); [cover] a
                               // tuft's blade: its lighting normal
  float3 ground : TEXCOORD5;   // the ground's normal under it
  float3 shade : TEXCOORD6;    // x: its sun shadow; y: how high up the plant (0 the foot, 1 its top); z: [cover] a tuft's
                               // blade: the sun's light through it (LeafThrough)
#ifdef PLANTS
  nointerpolation float4 burnt : TEXCOORD7;  // [plants] how it is burnt (PlantsBurnLook)
#endif
  nointerpolation float4 tint : TEXCOORD8;   // rgb: a card's colour times this ([coverdark] the ground's under a clump of
                                             // grass); [cover] a tuft's blade's own colour; w: 1 a tuft's blade (no picture)
                                             // ([leftover] under snow: 1 + its dusting a tuft's blade, minus its dusting a card)
#ifdef MOTION
  float4 here : TEXCOORD9;    // [leftover] (this frame's place and the frame before's, by the views without the jitter)
  float4 then : TEXCOORD10;
#endif
#ifdef PLANT_ROUND
  float3 mass : TEXCOORD11;   // [plantlod] a card's: the plant's normal as a mass, out from a point under its middle
#endif
};

static const float2 kCard[6] = {float2(0, 1), float2(1, 1), float2(0, 0), float2(0, 0), float2(1, 1), float2(1, 0)};
// [cover] a tuft's blade: level along it (0 the foot, 1 the middle, 2 the tip), side 0 / 1
static const uint2 kTuftCorner[9] = {uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1), uint2(1, 0), uint2(1, 1), uint2(2, 0)};
static const float kTuftUp[3] = {0, 0.55, 1};
static const float kTuftWide[3] = {1, 0.6, 0};

VOut VS(uint id : SV_VertexID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (no plant: behind the eye)
#ifdef MOTION
  o.here = o.then = float4(0, 0, 0, 1);
#endif
#ifdef PLANT_LOD
  // [plantlod] a spot of this frame's tiles (plant_lod.h): lodInfo[0].x corners a spot in this draw's tiles (from the tile
  // lodInfo[0].y on), 16 x 16 spots a tile; drawn as large as its size on screen says (PlantLodScale), or by the models
  uint perSpot = uint(lodInfo[0].x);
  uint spot = id / perSpot, local = id % perSpot;
  float4 tile = plantTiles[uint(lodInfo[0].y) + spot / 256u];
  uint within = spot % 256u;
  PlantSpot s;
  if (!PlantSpotAt(int2(tile.xy) + int2(within % 16u, within / 16u), s) || s.cell >= 16) return o;  // (a model with no card: its pass alone)
  uint h = s.h, group = s.group, cell = s.cell, count = s.count;
  float2 xz = s.xz;
  float bare = s.bare;
  const float grown = s.grown;
  float4 uvRect = cells[cell * 3], size = cells[cell * 3 + 1], more = cells[cell * 3 + 2];
  const bool tuft = group <= 1;
  uint plant = tuft ? 0 : local / 18;
  if (tuft ? local / 9 >= count * 2 : plant >= count) return o;
  const float px = PlantPixels(s);
  const float lodScale = PlantLodScale(s, px, tuft ? local / 18 : plant);
  if (lodScale <= 0.01) return o;
#else
  uint spot = id / 108, local = id % 108;
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(spot % side, spot / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));
  uint h = PlantHash(uint(cellId.x) * 83492791u ^ PlantHash(uint(cellId.y) * 2971215073u));
  float2 xz = cellAt + float2(PlantRand(h), PlantRand(PlantHash(h + 1))) * area.z;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  // [tarmac] none where a remastered pad lies, by its coverage
  [branch] if (info.y > 0.5 && padCover.SampleLevel(samplerLinearClamp, (xz - area.xy) / (area.z * area.w), 0) > PlantRand(PlantHash(h + 12))) return o;
  // [cover] none where the ground is bare (a stream, a trail), by how bare; lower where it thins out
  float bare = 0;
  [branch] if (shown.y > 0.5) {
    bare = bareGround.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
    if (bare > PlantRand(PlantHash(h + 13))) return o;
  }
  // [cover] as the cover comes in: this spot growing when its turn comes
  const float grown = CoverGrown(shown.x, PlantRand(PlantHash(h + 14)));
  if (grown <= 0) return o;
  float4 a = densityA.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float4 b = densityB.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  // which group grows here, if any: clumps, flowers (in patches), dry stalks, ferns, by the table
  float k = info.x;
  float pc = a.g * k, pf = a.b * k * PlantPatch(xz), ps = a.a * k, pn = b.r * k;
  float r = PlantRand(PlantHash(h + 2));
  uint group;
  if (r < pc) {
    float s = PlantRand(PlantHash(h + 3));
    group = s < b.b ? 2 : (PlantRand(PlantHash(h + 4)) < b.g ? 1 : 0);  // (weeds, else dry grass by the dry share, else grass)
  } else if (r < pc + pf) group = 3;
  else if (r < pc + pf + ps) group = 4;
  else if (r < pc + pf + ps + pn) group = 5;
  else return o;
  float4 g = groups[group];
  if (g.y < 0.5) return o;
  uint cell = OrderAt(uint(g.x) + PlantHash(h + 5) % uint(g.y));
  float4 uvRect = cells[cell * 3], size = cells[cell * 3 + 1], more = cells[cell * 3 + 2];
  uint count = uint(more.y) + PlantHash(h + 6) % uint(max(more.z - more.y + 1, 1));
  // [cover] the grass's clumps (groups 0 and 1) a tuft of blades: two blades for each plant of its clump; the rest stars
  const bool tuft = group <= 1;
  uint plant = tuft ? 0 : local / 18;
  if (tuft ? local / 9 >= count * 2 : plant >= count) return o;
#endif
  // this plant of the spot: its own foot near the spot's, its own height and turn
  uint hc = PlantHash(h * 31 + plant * 977 + 7);
#ifdef PLANT_LOD
  // [plantlod] drawn by the models' pass where it had room for it (its flags; else the card)
  [branch] if (tile.z >= 0 && PlantModelLevel(s, px, hc) >= 0 && ((modelFlags.Load(int3(within, int(tile.z), 0)) >> plant) & 1u) != 0) return o;
#endif
  float2 foot = xz;
  if (plant > 0) {
    float angle = PlantRand(hc) * 6.2832, reach = size.x * more.w * 0.35 * sqrt(PlantRand(PlantHash(hc + 1)));
    foot += float2(cos(angle), sin(angle)) * reach;
  }
  float ground = PlantGround(foot);
  if (ground < mapInfo.w + 0.08) return o;  // (none in or at the water)
  float3 root = float3(foot.x, ground, foot.y);
#ifdef PLANT_LOD
  // [plantlod] as large as its size on screen says (a tuft's blades: less taller, wider), pressed flat where something
  // stands (where that is kept)
  float height = lerp(size.y, size.z, PlantRand(PlantHash(hc + 2))) * more.w * (tuft ? sqrt(lodScale) : lodScale);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, PlantFieldUv(foot), 0) * PlantFieldWeight(foot);
#else
  // fewer toward the square's edge, none at it; pressed flat where something stands
  float half = area.z * area.w * 0.5;
  float thin = saturate((1 - length(foot - (area.xy + half)) / half) * 3);
  float height = lerp(size.y, size.z, PlantRand(PlantHash(hc + 2))) * more.w * saturate(thin * 2.5);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, (foot - area.xy) / (area.z * area.w), 0);
#endif
  height *= grown * (1 - 0.5 * bare);  // [cover]
#ifdef PLANTS
  // [plants] burnt where the ground is (its own share of a half burnt place): short
  o.burnt = PlantsBurnLook(foot, plantsK[0], plantsK[1], PlantRand(PlantHash(hc + 9)));
  height *= 1 - 0.8 * o.burnt.z;
#endif
  if (height <= 0.001) return o;
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  o.ground = n;
  // the trees' wind (as the blades'), the more toward the top by the cell's sway
  float beat = dot(foot, float2(0.11, 0.07));
  float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
#ifdef MOTION
  // [leftover] the frame before's wind (and below, what pressed it then; the rest as now)
  float swingB = sin(motionWhen.x * 1.3 + beat) * 0.6 + sin(motionWhen.x * 2.9 + beat * 1.7) * 0.4;
  float gustB = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (motionWhen.x * 7)) * 0.02, motionWhen.x * 0.07)));
#endif
  float4 tone = tones[cell];
  // [leftover] under lying snow (CoverSnowDepth): how deep it lies over this plant's foot; a plant no taller is buried (the
  // shorter first: each cell's height from the cover's file), a taller one stands out of it, bowed and dusted
  float deep = 0, snowDust = 0;
  [branch] if (weather.y > 0 && coverSnow.x > 0) {
    deep = CoverSnowDepth(foot, ground, n, mapInfo.xy, snowDust);
    if (deep > 0 && height - deep < CoverSnowOut(height)) return o;
  }

  [branch] if (tuft) {
    // [cover] a blade of the tuft: its root near the tuft's middle, fanned out from it (each its own way round, leaning out
    // the more the longer), swaying as the meadow's blades, its colour the meadow's (the dry clumps: their own straw)
    uint blade = local / 9;
    uint2 at = kTuftCorner[local % 9];
    uint hb = PlantHash(h * 53 + blade * 7919 + 11);
    float turn = float(blade) * 2.39996 + PlantRand(hb) * 0.9;
    float2 fan = float2(cos(turn), sin(turn)), across = float2(-fan.y, fan.x);
    float2 rootAt = foot + fan * (size.x * more.w * 0.12 * sqrt(PlantRand(PlantHash(hb + 1))));
    float bh = height * (0.6 + 0.4 * PlantRand(PlantHash(hb + 2)));
    float2 bend = fan * (0.25 + 0.55 * PlantRand(PlantHash(hb + 3))) + float2(0.915, 0.403) * (swing * gust * 0.45 * more.x);
#ifdef MOTION
    float2 bendB = fan * (0.25 + 0.55 * PlantRand(PlantHash(hb + 3))) + float2(0.915, 0.403) * (swingB * gustB * 0.45 * more.x);  // [leftover]
    float bhB = bh;
#endif
#ifdef PLANTS
    {  // [plants] pressed by what drove over it: laid over the way it was pushed (as the meadow's blades)
      float4 pressedBy = PlantsPressAt(foot, plantsK[0], plantsK[1]);
      [branch] if (pressedBy.z > 0.01) {
        float down = pressedBy.z * exp(-(pressedBy.w / pressedBy.z) / 6.0) * (0.85 + 0.3 * PlantRand(PlantHash(hb + 8)));
        float2 way = pressedBy.xy / pressedBy.z;
        float layOver = min(down * 1.3, 1.3);
        if (layOver > 1e-3 && length(way) > 0.05) {
          bh *= cos(layOver);
          bend += normalize(way) * tan(layOver);
        } else {
          bh *= 1 - 0.9 * saturate(down);
        }
#ifdef MOTION
        {  // [leftover] (the frame before: pressed as long ago less the frame's seconds)
          float downB = pressedBy.z * exp(-max(pressedBy.w / pressedBy.z - motionWhen.y, 0) / 6.0) * (0.85 + 0.3 * PlantRand(PlantHash(hb + 8)));
          float layOverB = min(downB * 1.3, 1.3);
          if (layOverB > 1e-3 && length(way) > 0.05) {
            bhB *= cos(layOverB);
            bendB += normalize(way) * tan(layOverB);
          } else {
            bhB *= 1 - 0.9 * saturate(downB);
          }
        }
#endif
      }
    }
#endif
#ifdef BLASTS
    float laid = 0;
    {  // a blast's wave: laid over away from it (as the meadow's blades)
      float2 blown = blastField.SampleLevel(samplerLinearClamp, (foot - area.xy) / (area.z * area.w), 0);
#ifdef PLANT_LOD
      blown *= PlantFieldWeight(foot);  // [plantlod] (where the waves are kept)
#endif
      float over = length(blown);
      laid = min(over, 1.35);
      if (laid > 1e-3) {
        bh *= cos(laid);
        bend += blown / over * tan(laid);
#ifdef MOTION
        bhB *= cos(laid);
        bendB += blown / over * tan(laid);
#endif
      }
    }
#endif
    [branch] if (deep > 0) {  // [leftover] under lying snow: only what stands out of it, bowed outward by the snow on it
      if (bh - deep < CoverSnowOut(bh)) return o;
      bh -= deep;
      ground += deep;
      bend += fan * (0.5 * snowDust);
#ifdef MOTION
      bhB = max(bhB - deep, 0);
      bendB += fan * (0.5 * snowDust);
#endif
    }
    float v = kTuftUp[at.x];
    float3 p = float3(rootAt.x, ground, rootAt.y) + float3(bend.x * v * v * bh, v * bh, bend.y * v * v * bh);
    // (at least most of a pixel wide, or far blades would flicker in and out between pixels)
    float wide = max(shown.w * 0.75, shown.z * length(root - cameraPosition) * 0.8) * kTuftWide[at.x];
#ifdef PLANT_LOD
    wide *= lodScale;  // [plantlod] (the first pair wider as the tuft is drawn as fewer blades, the others narrower)
#endif
    p.xz += across * ((at.y ? 0.5 : -0.5) * wide);
    o.position = mul(viewProj, float4(p, 1));
#ifdef MOTION
    float3 pB = float3(rootAt.x, ground, rootAt.y) + float3(bendB.x * v * v * bhB, v * bhB, bendB.y * v * v * bhB);  // [leftover]
    pB.xz += across * ((at.y ? 0.5 : -0.5) * wide);
    o.here = mul(motionNow, float4(p, 1));
    o.then = mul(motionBefore, float4(pB, 1));
    o.tint.w = 1;  // (a tuft's blade: nothing cut away)
#else
    o.world = p;
    float3 face = float3(fan.x, 0, fan.y);
    o.toward = normalize(n + face * 0.3 + float3(across.x, 0, across.y) * (at.y ? 0.35 : -0.35));
    float3 toEye = normalize(cameraPosition - p);
    o.shade = float3(SunShadow(p + n * 0.02, n), v,
                     leaves.x > 0.5 ? LeafThrough(dot(face, toEye) < 0 ? -face : face, toEye) * (0.4 + 0.6 * v) : 0);
    float3 colour = group == 0 ? MeadowColour(ToLinear(cover.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0).rgb), foot)
                               : (any(tone.rgb > 0) ? tone.rgb : float3(0.30, 0.24, 0.13));
    colour *= (0.85 + 0.3 * PlantRand(PlantHash(hb + 6))) * lerp(0.8, 1.35, v);
#ifdef BLASTS
    // (grass laid flat is paler, as the meadow's)
    colour *= 1 + 0.45 * sin(laid);
#endif
    o.tint = float4(colour, 1);
    if (snowDust > 0) o.tint.w = 1 + snowDust;  // [leftover] (dusted toward its tip: the pixel's)
#endif
    return o;
  }

  // [cover] a card of the plant's star: three cards 60 degrees apart, upright, the plant turned its own way and leaning a
  // little its own way (a plant is never quite straight); the picture's foot at the plant's foot
  uint card = (local % 18) / 6, corner = local % 6;
  float frame = size.x * height / max(size.w, 0.001);  // (the card: the cell's frame, as tall as the plant is)
  float turn = PlantRand(PlantHash(hc + 3)) * 3.14159 + float(card) * 1.0472;
  float3 right = float3(cos(turn), 0, sin(turn));
  float3 toward = float3(-right.z, 0, right.x);  // (one side's face)
  float2 tilt = (float2(PlantRand(PlantHash(hc + 4)), PlantRand(PlantHash(hc + 5))) - 0.5) * 0.3;
  float3 up = normalize(float3(tilt.x, 1, tilt.y));
#ifdef PLANT_ROUND
  // [plantlod] fanned out from the foot: each card leaning out its own way (the three round the plant, 120 degrees apart),
  // the weeds' and ferns' far over as their leaves and fronds spread, the flowers' and stalks' a little; seen from above
  // the plant then shows its cards' faces, a tuft, not three lines
  {
    float2 leanBy = group == 2 ? float2(0.75, 0.3) : group == 5 ? float2(0.6, 0.3) : group == 3 ? float2(0.25, 0.2) : float2(0.1, 0.15);
    float lean = leanBy.x + leanBy.y * PlantRand(PlantHash(hc + 30 + card));
    up = normalize(up * cos(lean) + ((card & 1u) != 0 ? -toward : toward) * sin(lean));
    toward = normalize(cross(up, right));
  }
#endif
  float2 bend = float2(0.915, 0.403) * (swing * gust * 0.35 * more.x);
  float2 c = kCard[corner];
  [branch] if (deep > 0) c.y = min(c.y, 1 - deep / frame);  // [leftover] (under lying snow: the picture's foot at the snow's top)
  bool flip = (hc & 64u) != 0;
  float above = 1 - c.y;  // (0 the foot, 1 the card's top)
#ifdef PLANT_LOD
  // [plantlod] a card a model ends on: as wide as the model (modelTint's w), so that the plant keeps its size where it changes
  float cardWide = CellModel(cell).x >= 0 && lodInfo[2].y > 0.5 ? modelTint[uint(CellModel(cell).x)].w : 1;
  float3 p = root + right * ((c.x - 0.5) * frame * cardWide) + up * (above * frame);
#else
  float3 p = root + right * ((c.x - 0.5) * frame) + up * (above * frame);
#endif
#ifdef MOTION
  float3 pB = p;  // [leftover] (the frame before's: its wind then)
  pB.xz += float2(0.915, 0.403) * (swingB * gustB * 0.35 * more.x) * above * above * frame;
#endif
  p.xz += bend * above * above * frame;
#ifdef PLANTS
  {  // [plants] pressed by what drove over it: laid over the way it was pushed, rising again over some seconds
    float4 pressedBy = PlantsPressAt(foot, plantsK[0], plantsK[1]);
    [branch] if (pressedBy.z > 0.01) {
      float down = pressedBy.z * exp(-(pressedBy.w / pressedBy.z) / 6.0);
      float2 way = pressedBy.xy / pressedBy.z;
      float layOver = min(down * 1.3, 1.3);
      way = length(way) > 0.05 ? normalize(way) : toward.xz;
      p -= up * (above * frame * (1 - cos(layOver)));
      p.xz += way * (sin(layOver) * above * above * frame);
#ifdef MOTION
      {  // [leftover] (the frame before: pressed as long ago less the frame's seconds)
        float downB = pressedBy.z * exp(-max(pressedBy.w / pressedBy.z - motionWhen.y, 0) / 6.0);
        float layOverB = min(downB * 1.3, 1.3);
        pB -= up * (above * frame * (1 - cos(layOverB)));
        pB.xz += way * (sin(layOverB) * above * above * frame);
      }
#endif
    }
  }
#endif
#ifdef BLASTS
  {  // a blast's wave: laid over away from it, its top going over as far as it was high
    float2 blown = blastField.SampleLevel(samplerLinearClamp, (foot - area.xy) / (area.z * area.w), 0);
#ifdef PLANT_LOD
    blown *= PlantFieldWeight(foot);  // [plantlod] (where the waves are kept)
#endif
    float over = length(blown), lay = min(over, 1.35);
    if (lay > 1e-3) {
      p -= up * (above * frame * (1 - cos(lay)));
      p.xz += blown / over * (sin(lay) * above * above * frame);
#ifdef MOTION
      pB -= up * (above * frame * (1 - cos(lay)));
      pB.xz += blown / over * (sin(lay) * above * above * frame);
#endif
    }
  }
#endif
  [branch] if (deep > 0) {  // [leftover] under lying snow: what stands out of it bowed a little its own way by the snow on it
    float lay = 0.45 * snowDust, a = PlantRand(PlantHash(hc + 3)) * 6.2832;
    float3 bowed = -up * (above * frame * (1 - cos(lay))) + float3(cos(a), 0, sin(a)) * (sin(lay) * above * above * frame);
    p += bowed;
#ifdef MOTION
    pB += bowed;
#endif
  }
  o.position = mul(viewProj, float4(p, 1));
  o.world = p;
  o.uv = lerp(uvRect.xy, uvRect.zw, float2(flip ? 1 - c.x : c.x, c.y));
#ifdef MOTION
  o.here = mul(motionNow, float4(p, 1));  // [leftover] (a card: its holes cut in MotionPS, tint.w 0)
  o.then = mul(motionBefore, float4(pB, 1));
#else
  o.right = flip ? -right : right;
  o.up = up;
  o.toward = toward;
  // [cover] the sun's shadow at this corner (a little off the ground), not one for the whole card: as the camera zooms and
  // the shadow maps change, a plant at a shadow's edge is not all lit, then all dark
  o.shade = float3(SunShadow(p + n * 0.03, n), above * frame / max(height, 0.001), 0);
#ifdef PLANT_ROUND
  // [plantlod] (out from a point under the plant's middle: its top faces the sky, its rim outward)
  o.mass = normalize(p - (root + float3(0, -0.35 * height, 0)) + n * (0.05 * height));
#endif
  // [coverdark] a clump of the meadow's grass takes the ground's colour under it, as the blades do (the card's own light
  // and dark kept, its mean brought to the ground's, a little varied): else, at the art's own colour, it stands out as a
  // dark blot on the ground it grows from. [cover] The weeds and ferns (dark leaves): their brightness brought part of the
  // way toward the ground's, their colour kept (at the art's own they are black holes in the trees' shade).
  o.tint = float4(1, 1, 1, 0);
  [branch] if (tone.w != 0) {
    float3 under = ToLinear(cover.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0).rgb) * (0.9 + 0.2 * PlantRand(PlantHash(hc + 10)));
    if (tone.w > 0) o.tint.rgb = lerp(1, clamp(under / max(tone.rgb, 1e-3), 0.25, 4), tone.w);
    else o.tint.rgb = lerp(1, clamp(PlantLuma(under) / max(PlantLuma(tone.rgb), 1e-3), 0.5, 3), -tone.w);
  }
  if (snowDust > 0) o.tint.w = -snowDust;  // [leftover] (dusted where its leaves face up: the pixel's)
#endif
  return o;
}

#ifdef MOTION
// [leftover] the motion: a card's holes cut as the scene cut them (its picture's coverage at a half; right at the eye
// dithered away), so that what shows through keeps its own motion
float2 MotionPS(VOut i) : SV_Target {
  [branch] if (i.tint.w < 0.5) {
    float a = atlas.Sample(samplerAniso, i.uv).a;
    clip(a - 0.5);
#ifdef PLANTS
    clip(a - 0.5 - 0.3 * saturate(i.burnt.x + i.burnt.y));
#endif
    float near = length(i.world - cameraPosition);
    [branch] if (near < 1.2) {
      if (smoothstep(0.4, 1.2, near) <= frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))))) discard;
    }
  }
  return CoverMoved(i.here, i.then);
}
#endif

float4 PS(VOut i) : SV_Target {
#if RAYS
  rayLightsAtMost = 1;  // (one ray for the small lights' shadows; lighting.h, SmallLightsTraced)
#endif
  // [leftover] under lying snow the cover is dusted by its own measure (CoverSnowDepth: the tint's w), not whitened all over
  // where the light's snow would lie on what faces up as the ground does
  [branch] if (weather.y > 0 && coverSnow.x > 0) surfaceSnowShare = 0;
  [branch] if (i.tint.w > 0.5) {
    // [cover] a tuft's blade: lit as the meadow's blades (darker down among them)
    float3 colour = i.tint.rgb;
#ifdef PLANTS
    colour = PlantsBurnColour(colour, i.burnt);  // [plants]
#endif
    float3 lightThrough = LeafLight(colour, i.shade.z, i.world, i.shade.x, 0.8);
#ifdef PLANTS
    lightThrough = lightThrough * (1 - saturate(i.burnt.x + i.burnt.y)) + PlantsEmber(i.burnt.w, i.world.xz, time) * (i.shade.y * (falling.w > 0 ? falling.w : 1));  // [plants]
#endif
    [branch] if (i.tint.w > 1) {  // [leftover] (dusted with snow toward its tip; less light through there)
      float dust = (i.tint.w - 1) * (0.3 + 0.55 * smoothstep(0.2, 1, i.shade.y));
      colour = lerp(colour, SNOW_COLOUR, dust);
      lightThrough *= 1 - dust;
    }
    return float4(ModernLight(colour, 0, 0.85, lerp(0.55, 1, i.shade.y), lightThrough, normalize(i.toward), i.world, i.shade.x), 1);
  }
  float4 c = atlas.Sample(samplerAniso, i.uv);
  clip(c.a - 0.5);
#ifdef PLANTS
  clip(c.a - 0.5 - 0.3 * saturate(i.burnt.x + i.burnt.y));  // [plants] (burnt: thinner)
#endif
  // [foliage] right at the eye (a cinematic camera down among the plants): faded out from 1.2 world units to nothing
  // under 0.4, as the models' foliage is (mesh_renderer.cpp, NearFade): with several samples a pixel by its coverage
  // among them (the alpha returned, alpha to coverage: PLANT_SAMPLES), else whole pixels cut, dithered
  float fade = 1;
  float near = length(i.world - cameraPosition);
  [branch] if (near < 1.2) {
    fade = smoothstep(0.4, 1.2, near);
#if PLANT_SAMPLES < 2
    if (fade <= frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))))) discard;
    fade = 1;
#endif
  }
  // [cover] the card's side the eye sees (a card stands in the world, seen from either side)
  float3 toEye = normalize(cameraPosition - i.world);
  float3 face = dot(i.toward, toEye) < 0 ? -i.toward : i.toward;
  float2 t = atlasNormals.Sample(samplerAniso, i.uv).ag * 2 - 1;
  float3 card = normalize(i.right * t.x + i.up * t.y + face * sqrt(saturate(1 - dot(t, t))));
  float3 n = normalize(lerp(i.ground, card, 0.5));  // (the card's own normals at half strength: the visuals track's advice)
#ifdef PLANT_ROUND
  n = normalize(lerp(normalize(i.mass), card, 0.35));  // [plantlod] (rounded over the plant as a mass, a little of the card's own)
#endif
  float occlusion = lerp(0.7, 1, saturate(i.shade.y));
  // (the sun's light through its leaves, World::leafLight: by the card's own way, the side the eye sees)
  float3 albedo = ToLinear(c.rgb) * i.tint.rgb;  // ([coverdark] i.tint)
#ifdef PLANTS
  albedo = PlantsBurnColour(albedo, i.burnt);  // [plants]
#endif
  float3 through = LeafLight(albedo, LeafThrough(card, toEye), i.world, i.shade.x, 0.7);
#ifdef PLANTS
  through = through * (1 - saturate(i.burnt.x + i.burnt.y)) + PlantsEmber(i.burnt.w, i.world.xz, time) * (falling.w > 0 ? falling.w : 1);  // [plants]
#endif
  [branch] if (i.tint.w < 0) {  // [leftover] (dusted with snow where its leaves face up, the more toward its top; less light through there)
    float dust = -i.tint.w * saturate(card.y * 2.5 + 0.4) * smoothstep(0.1, 0.6, i.shade.y) * 0.9;
    albedo = lerp(albedo, SNOW_COLOUR, dust);
    through *= 1 - dust;
  }
  return float4(ModernLight(albedo, 0, 0.85, occlusion, through, n, i.world, i.shade.x), fade);
}
)hlsl";


// Over the grass's square, how flat what stands there presses it: each texel the most of the discs round it (full
// within a disc, nothing a quarter of a world unit outside it). A thread for each texel.
const char kTrample[] = R"hlsl(
cbuffer Trample : register(b1) {
  float4 area;        // as the grass's: the first cell's x, z; a cell's size; cells a side
  float4 count;       // x: how many discs
  float4 tracksLevel[2], tracksAtlas[2], tracksInfo;  // (deep tracks: their two levels round the camera, deep_tracks.h)
  float4 discs[256];  // x, z, radius
};
RWTexture2D<float> trampled : register(u0);
Texture2D tracksField : register(t0);  // (deep tracks)
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  trampled.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 p = area.xy + (id.xy + 0.5) / float2(w, h) * (area.z * area.w);
  float flat = 0;
  for (uint i = 0; i < uint(count.x); ++i) flat = max(flat, saturate((discs[i].z + 0.25 - length(p - discs[i].xy)) / 0.25));
  // (deep tracks: the grass lies flat along them, by the finest of their levels round the camera that holds the place)
  [branch] if (tracksInfo.z > 0.5) {
    for (int l = 0; l < 2; ++l) {
      float4 L = tracksLevel[l], A = tracksAtlas[l];
      float2 s = p * L.z, inside = s - L.xy;
      if (any(inside < 2) || any(inside > L.w - 2)) continue;
      float2 c = s - L.w * floor(s / L.w);
      float pressed = tracksField.SampleLevel(samplerLinearClamp, (A.xy + c) * tracksInfo.xy, 0).r;
      flat = max(flat, smoothstep(0.15, 0.7, 1 - exp(-1.5 * pressed)) * 0.9);
      break;
    }
  }
  trampled[id.xy] = flat;
}
)hlsl";

struct TrampleConstants {
  float area[4], count[4];
  float tracksLevel[2][4], tracksAtlas[2][4], tracksInfo[4];  // (deep tracks)
  float discs[256][4];
};
constexpr int kTrampleSize = 256;

}  // namespace

// The plants' constants (kPlantDecls' Plants; [plantlod] out of the file's own namespace: grass.h names it)
struct PlantConstants {
  float area[4], mapInfo[4], info[4], shown[4];  // ([cover] shown)
  float cells[48][4];
  float order[4][4];
  float groups[6][4];
  float tones[16][4];   // [coverdark]
  float plantsK[2][4];  // [plants] (read by the shaders with them only)
  // [plantlod] (read by the shaders with plantsFar only: PLANT_LOD) lod[0]: x a spot's corners in this draw, y its first
  // tile, z pixels a world unit at a distance of one, w unused; lod[1]: the clump's collapse (pixels: all of it above x,
  // one plant below y), the cut's band (z, w); lod[2]: x 1 = the fields over the square are this frame's, y 1 = models,
  // z the models' band; lod[3]: x a model draw's list, y its mesh's height, z 1 = it has its material
  float lod[4][4];
  float cellModels[16][4];  // [plantlod] per cell: its model's kind (-1 none), the pixels its levels go down to
  float modelKinds[8][4];   // [plantlod] per kind: its levels, 1 = it ends on a card
  float extraCells[24][4];  // [plantlod] per kind with no card: its cell (16 + its number): as cellModels, cells' size and more
  float extraRules[8][4];   // [plantlod] the same: the group whose spots it takes, the share of them
  float extraTones[8][4];   // [plantlod] the same: as tones
  float modelTint[8][4];    // [plantlod] per kind: its colour times this; w its card's width times this
  float farField[4];        // [plantlod] the far field over the tiles (GroundCover::FarArea)
};

GroundCover::~GroundCover() {
  for (gfx::Pipeline p : {pipeline_, trample_, plantPipeline_, blastPipeline_, blastPlantPipeline_, reactingPipelines_[0], reactingPipelines_[1],
                          reactingPipelines_[2], reactingPipelines_[3]})
    if (p) device_->Destroy(p);
  for (gfx::Pipeline p : motionPipelines_)  // [leftover]
    if (p) device_->Destroy(p);
  for (gfx::Pipeline p : {farCover_, modelPlace_, modelClear_, farDiscPipeline_})  // [plantlod]
    if (p) device_->Destroy(p);
  if (farField_) device_->Destroy(farField_);  // [plantlod]
  for (gfx::Pipeline p : modelPipelines_)  // [plantlod]
    if (p) device_->Destroy(p);
  for (gfx::Buffer b : {tileBuffer_, modelTileBuffer_})  // [plantlod]
    if (b) device_->Destroy(b);
  if (trampled_) device_->Destroy(trampled_);
}

// [plantlod] What the plants' shaders are made with for the switches (none: as without them).
std::string GroundCover::PlantDefines() const {
  return std::string((plantVariant_ & 1) ? "#define PLANT_LOD 1\n" : "") + ((plantVariant_ & 2) ? "#define PLANT_ROUND 1\n" : "");
}

// The plants' shaders' text (for the switches, plantVariant_): plantSource_, blastPlantSource_ and the reacting ones'.
void GroundCover::BuildPlantSources() {
  const bool lod = (plantVariant_ & 1) != 0;
  const std::string reacting = std::string("#define PLANTS 1\n#define PLANTS_PRESS t9\n#define PLANTS_BURN t10\n");
  auto text = [&](const std::string& head, bool reacts) {
    return PlantDefines() + "#define PLANT_SAMPLES " + std::to_string(gSceneSamples) + "\n" + head + kViewShaderBlock + RayShaders() + kLightingShaderBlock +
           (reacts ? std::string(kPlantsShaderBlock) : std::string()) + kCoverShared + kPlantDecls + (lod ? kPlantLodBlock : "") + kPlantShader +
           (lod ? kPlantModelShader : "");  // ([plantlod] the models' drawing in the same text)
  };
  plantSource_ = text(std::string(), false);
  blastPlantSource_ = "#define BLASTS 1\n" + plantSource_;
  reactingSources_[1] = text(reacting, true);
  reactingSources_[3] = "#define BLASTS 1\n" + reactingSources_[1];
}

// [plantlod] The plants' pipelines made again when plantsFar or plantsRound changed (those of the blasts' waves and of
// plants that react only where they were made already; the motion's and the models' when next wanted).
void GroundCover::MakePlantVariant() {
  const int want = (plantsFar ? 1 : 0) | (plantsRound ? 2 : 0);
  if (want == plantVariant_ || !pipeline_) return;
  plantVariant_ = want;
  for (gfx::Pipeline* p : {&plantPipeline_, &blastPlantPipeline_, &reactingPipelines_[1], &reactingPipelines_[3]})
    if (*p) {
      device_->Destroy(*p);
      *p = {};
    }
  for (int k = 1; k < 8; k += 2) {
    if (motionPipelines_[k]) device_->Destroy(motionPipelines_[k]);
    motionPipelines_[k] = {};
    motionTried_[k] = false;
  }
  for (int k = 0; k < 8; ++k) {
    if (modelPipelines_[k]) device_->Destroy(modelPipelines_[k]);
    modelPipelines_[k] = {};
    modelTried_[k] = false;
  }
  for (gfx::Pipeline* p : {&modelPlace_, &modelClear_})
    if (*p) {
      device_->Destroy(*p);
      *p = {};
    }
  modelPlaceTried_ = false;
  BuildPlantSources();
  std::string error;
  gfx::PipelineDesc pd = blastDesc_;
  pd.alphaToCoverage = true;
  pd.name = "ground cover: plants";
  pd.vertexSource = pd.pixelSource = plantSource_;
  plantPipeline_ = device_->CreatePipeline(pd, &error);
  if (!plantPipeline_) fprintf(stderr, "render: the ground cover's plants cannot be drawn so (%s)\n", error.c_str());
  if (blastsMade_ > 0) {
    pd.name = "ground cover: plants, blasts' waves";
    pd.vertexSource = pd.pixelSource = blastPlantSource_;
    blastPlantPipeline_ = device_->CreatePipeline(pd, &error);
  }
  if (reactingMade_ > 0)
    for (int k : {1, 3}) {
      pd.name = k == 1 ? "ground cover: plants, reacting" : "ground cover: plants, reacting, blasts' waves";
      pd.vertexSource = pd.pixelSource = reactingSources_[k];
      reactingPipelines_[k] = device_->CreatePipeline(pd, &error);
    }
}

bool GroundCover::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + RayShaders() + kLightingShaderBlock + kCoverShared + kShader;  // (RayShaders: the small lights' shadows by rays)
  gfx::PipelineDesc pd;
  pd.name = "ground cover: grass";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  // [foliage] (the plants' cards fade right at the eye by their coverage among a pixel's samples: PLANT_SAMPLES, alpha
  // to coverage; elsewhere their alpha is 1, every sample, as before)
  // ([plantlod] the plants' text for the switches as they are now: BuildPlantSources; without them as before)
  plantVariant_ = (plantsFar ? 1 : 0) | (plantsRound ? 2 : 0);
  BuildPlantSources();
  const std::string plants = plantSource_;
  pd.name = "ground cover: plants";
  pd.vertexSource = pd.pixelSource = plants;
  pd.alphaToCoverage = true;  // [foliage]
  plantPipeline_ = device_->CreatePipeline(pd, error);
  pd.alphaToCoverage = false;  // [foliage]
  if (!plantPipeline_) return false;
  // (the same two laid over by the blasts' waves, shaders of their own so that without them nothing is drawn otherwise:
  // made by PrepareBlasts when they are first wanted)
  blastDesc_ = pd;
  blastSource_ = "#define BLASTS 1\n" + source;
  // [plants] (and the same two with plants that react, without and with the blasts' waves: made by PreparePlants when first
  // wanted; its textures at t9, t10)
  const std::string reacting = std::string("#define PLANTS 1\n#define PLANTS_PRESS t9\n#define PLANTS_BURN t10\n");
  reactingSources_[0] = reacting + kViewShaderBlock + RayShaders() + kLightingShaderBlock + kPlantsShaderBlock + kCoverShared + kShader;
  reactingSources_[2] = "#define BLASTS 1\n" + reactingSources_[0];
  bladeSource_ = source;  // [leftover] (for the motion's: DrawMotion)
  const std::string trample = std::string(kViewShaderBlock) + kTrample;
  gfx::ComputeDesc cd;
  cd.name = "ground cover: pressed flat";
  cd.source = trample;
  trample_ = device_->CreateCompute(cd, error);
  if (!trample_) return false;
  gfx::TargetDesc td;
  td.storage = true;
  td.width = td.height = kTrampleSize;
  td.format = gfx::Format::R8;
  trampled_ = device_->CreateTarget(td);
  return static_cast<bool>(trampled_);
}

bool GroundCover::PrepareBlasts() {
  if (blastsMade_ || !pipeline_) return blastsMade_ > 0;
  blastsMade_ = -1;
  std::string error;
  gfx::PipelineDesc pd = blastDesc_;
  pd.name = "ground cover: grass, blasts' waves";
  pd.vertexSource = pd.pixelSource = blastSource_;
  blastPipeline_ = device_->CreatePipeline(pd, &error);
  pd.name = "ground cover: plants, blasts' waves";
  pd.vertexSource = pd.pixelSource = blastPlantSource_;
  pd.alphaToCoverage = true;  // [foliage] (as plantPipeline_)
  if (blastPipeline_) blastPlantPipeline_ = device_->CreatePipeline(pd, &error);
  if (!blastPlantPipeline_) {
    fprintf(stderr, "render: the ground cover cannot bend in blasts' waves (%s)\n", error.c_str());
    return false;
  }
  blastsMade_ = 1;
  return true;
}

// [plants] The grass and the plants with plants that react (without, then with the blasts' waves), made the first time the
// world wants them.
bool GroundCover::PreparePlants() {
  if (reactingMade_ || !pipeline_) return reactingMade_ > 0;
  reactingMade_ = -1;
  std::string error;
  for (int k = 0; k < 4; ++k) {
    gfx::PipelineDesc pd = blastDesc_;
    pd.name = k == 0 ? "ground cover: grass, reacting" : k == 1 ? "ground cover: plants, reacting" : k == 2 ? "ground cover: grass, reacting, blasts' waves"
                                                                                                   : "ground cover: plants, reacting, blasts' waves";
    pd.vertexSource = pd.pixelSource = reactingSources_[k];
    pd.alphaToCoverage = (k & 1) != 0;  // (the plants' cards, as plantPipeline_)
    reactingPipelines_[k] = device_->CreatePipeline(pd, &error);
    if (!reactingPipelines_[k]) {
      fprintf(stderr, "render: the ground cover cannot react (%s)\n", error.c_str());
      return false;
    }
  }
  reactingMade_ = 1;
  return true;
}

void GroundCover::Prepare(const View& view, const Camera& camera, const std::vector<float>& discs) {
  prepared_ = false;
  farReady_ = modelsPlaced_ = farFieldReady_ = false;  // [plantlod] (PrepareFar's, this frame's)
  if (plantsFar) farDiscs_ = discs;                     // [plantlod] (the far field's footprints: PlaceFar)
  // (a cinematic camera, cinecam.h: by how high the eye is over the ground ahead, and the square ahead of the eye,
  // from a little behind it to some fifty units on, where a low eye sees the grass)
  const Vec3 eye = camera.Eye();
  const float away = camera.cinematic ? eye.y - camera.target.y : camera.distance;
  away_ = away;  // [leftover] (for the motion: motionFarthest)
  if (pipeline_) MakePlantVariant();  // [plantlod] (the plants' shaders for the switches as they are now)
  const bool farOnly = plantsFar && pipeline_ && (!trample_ || !trampled_ || away > farthest);  // [plantlod] (the plants alone)
  if (!farOnly && (!pipeline_ || !trample_ || !trampled_ || away > farthest)) return;
  // [cover] how much of the cover is there: none at `farthest`, all from `fadeFrom` of it on; and since it came in, over
  // `appearSeconds` (the view's clock: a frame's step counted at most a tenth of a second, so that a hitch is no jump; it
  // starts again after two seconds without it, or the clock turned back more than a second: a map opened, a new game;
  // a photo's frames, which hold the clock, leave it as it is)
  const float now = view.constants.time;
  float step = std::clamp(now - lastShown_, 0.0f, 0.1f);
  if (now > lastShown_ + 2 || now < lastShown_ - 1) step = cameIn_ = 0;
  cameIn_ = std::min(cameIn_ + (appearSeconds > 0 ? step / appearSeconds : 1.0f), 1.0f);
  lastShown_ = now;
  cameInShown_ = cameIn_ * cameIn_ * (3 - 2 * cameIn_);  // [plantlod] (the plants by plantsFar: from any distance)
  if (farOnly) return;  // [plantlod] (no blades, nothing pressed flat: the plants only, PrepareFar)
  const float byDistance = std::clamp((farthest - away) / std::max(farthest * (1 - fadeFrom), 1e-3f), 0.0f, 1.0f);
  shown_ = byDistance * cameIn_ * cameIn_ * (3 - 2 * cameIn_);
  // a square round what the camera looks at, as wide as it sees near there (no more than about 370 cells a side)
  const float reach = camera.cinematic ? 28.0f : std::clamp(camera.distance * 0.75f, 8.0f, 28.0f);
  const int side = static_cast<int>(std::ceil(2 * reach / cell));
  const float middleX = camera.cinematic ? eye.x - std::sin(camera.heading) * reach * 0.7f : camera.target.x;
  const float middleZ = camera.cinematic ? eye.z - std::cos(camera.heading) * reach * 0.7f : camera.target.z;
  area_[0] = std::floor((middleX - reach) / cell) * cell;
  area_[1] = std::floor((middleZ - reach) / cell) * cell;
  area_[2] = cell;
  area_[3] = static_cast<float>(side);
  auto k = std::make_unique<TrampleConstants>();
  std::copy(area_, area_ + 4, k->area);
  const float wide = cell * static_cast<float>(side);
  int n = 0;
  for (size_t i = 0; i + 2 < discs.size() && n < 256; i += 3) {
    const float x = discs[i], z = discs[i + 1], r = discs[i + 2];
    if (x + r < area_[0] || z + r < area_[1] || x - r > area_[0] + wide || z - r > area_[1] + wide) continue;
    k->discs[n][0] = x;
    k->discs[n][1] = z;
    k->discs[n][2] = r;
    ++n;
  }
  k->count[0] = static_cast<float>(n);
  if (tracks) {  // (deep tracks: their field's levels round the camera)
    std::copy(&tracksField.level[0][0], &tracksField.level[0][0] + 8, &k->tracksLevel[0][0]);
    std::copy(&tracksField.atlas[0][0], &tracksField.atlas[0][0] + 8, &k->tracksAtlas[0][0]);
    std::copy(tracksField.info, tracksField.info + 4, k->tracksInfo);
  }
  device_->SetPipeline(trample_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k.get(), static_cast<uint32_t>(sizeof(float) * 28 + sizeof(float) * 4 * std::max(n, 1)));
  device_->SetTexture(0, tracks);
  device_->SetStorage(0, trampled_);
  device_->Dispatch(kTrampleSize / 8, kTrampleSize / 8, 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});
  prepared_ = true;
}

bool GroundCover::SetPlants(const JsonValue& cover, TextureCache* textures, const std::string& folder) {
  plants_ = PlantData{};
  atlas_ = atlasNormals_ = {};
  const JsonValue& cells = cover["cells"];
  if (cells.kind != JsonValue::Object) return false;
  // the groups' cells (a cell's group, and for grass whether it is one of the dry cells)
  std::vector<std::string> dry;
  for (const JsonValue& d : cover["density"]["dry_cells"].items) dry.push_back(d.StringOr(std::string()));
  std::vector<int> byGroup[6];
  float tallest = 0;
  for (const auto& [name, c] : cells.members) {
    const int index = static_cast<int>(c["index"].NumberOr(-1));
    if (index < 0 || index >= 16 || c["uv"].items.size() != 4) continue;
    const std::string group = c["group"].StringOr(std::string());
    const int g = group == "clump" ? (std::find(dry.begin(), dry.end(), name) != dry.end() ? 1 : 0)
                : group == "weed" ? 2 : group == "flower" ? 3 : group == "stalks" ? 4 : group == "fern" ? 5 : -1;
    if (g < 0) continue;
    float(&cell)[3][4] = plants_.cells[index];
    for (int k = 0; k < 4; ++k) cell[0][k] = static_cast<float>(c["uv"].items[static_cast<size_t>(k)].NumberOr(0));
    const JsonValue& range = c["height_range_m"];
    const float height = static_cast<float>(c["height_m"].NumberOr(0));
    const float low = range.items.size() == 2 ? static_cast<float>(range.items[0].NumberOr(height)) : height;
    const float high = range.items.size() == 2 ? static_cast<float>(range.items[1].NumberOr(height)) : height;
    cell[1][0] = static_cast<float>(c["frame_m"].NumberOr(height));
    cell[1][1] = low;
    cell[1][2] = high;
    cell[1][3] = height;
    const JsonValue& clump = c["clump"];
    cell[2][0] = static_cast<float>(c["sway"].NumberOr(0.5));
    cell[2][1] = clump.items.size() == 2 ? static_cast<float>(clump.items[0].NumberOr(1)) : 1.0f;
    cell[2][2] = clump.items.size() == 2 ? static_cast<float>(clump.items[1].NumberOr(1)) : 1.0f;
    if (height <= 0 || cell[1][0] <= 0) continue;
    // [coverdark] each cell's mean colour, from the cover's stats (its sRGB, made linear as the shader makes the atlas's and
    // the ground's): the grass's clumps (not the dry ones) for bringing it to the ground's (1), [cover] the weeds and ferns
    // their brightness toward the ground's (-1), the dry clumps' tufts their colour; none without stats
    plants_.cover[index] = static_cast<float>(cover["stats"][name]["cover"].NumberOr(0.15));  // [plantlod] (the far cover's)
    const JsonValue& mean = cover["stats"][name]["albedo_srgb8"];
    if (mean.items.size() == 3) {
      for (int k = 0; k < 3; ++k) plants_.tones[index][k] = std::pow(static_cast<float>(mean.items[static_cast<size_t>(k)].NumberOr(0)) / 255.0f, 2.2f);
      plants_.tones[index][3] = g == 0 ? 1.0f : g == 2 || g == 5 ? -1.0f : 0.0f;
    }
    tallest = std::max(tallest, high);
    byGroup[g].push_back(index);
  }
  int at = 0;
  for (int g = 0; g < 6; ++g) {
    plants_.groups[g][0] = static_cast<float>(at);
    plants_.groups[g][1] = static_cast<float>(byGroup[g].size());
    for (int index : byGroup[g]) {
      plants_.order[at / 4][at % 4] = static_cast<float>(index);
      ++at;
    }
  }
  plants_.count = at;
  lastShown_ = -1e9f;  // [cover] (a map opened: the cover comes in again)
  if (!at || tallest <= 0) return false;
  plants_.tallest = tallest;  // (the tallest's real height: each cell's scale is worked out as it is drawn, Draw)
  const JsonValue& patches = cover["density"]["flowers_patches"];
  plants_.patchScale = static_cast<float>(patches["patch_scale_units"].NumberOr(12));
  plants_.patchCover = static_cast<float>(patches["patch_cover"].NumberOr(0.35));
  atlas_ = textures->Get(folder + cover["albedo"].StringOr(std::string()));
  atlasNormals_ = textures->Get(folder + cover["normal"].StringOr(std::string()), textures->FlatNormal());
  return static_cast<bool>(atlas_);
}

// The plants' constants (Draw's, and [plantlod] PrepareFar's for the models' placing): the square they lie over, the map,
// the cover's cells and how each is scaled, coloured and grouped; [plantlod] with plantsFar their sizes on screen.
// `pixelAngle`: world units a pixel covers at a distance of one. Returns the square's cells a side.
int GroundCover::FillPlantConstants(PlantConstants* p, float mapWidth, float mapHeight, float heightUnit, float waterLevel, float pixelAngle,
                                    bool reacting) const {
  const float reach = 0.5f * area_[2] * area_[3];
  const int plantSide = static_cast<int>(std::ceil(2 * reach / plantCell));
  p->area[0] = std::floor(area_[0] / plantCell) * plantCell;  // (the blades' square, by the plants' cells)
  p->area[1] = std::floor(area_[1] / plantCell) * plantCell;
  p->area[2] = plantCell;
  p->area[3] = static_cast<float>(plantSide);
  p->mapInfo[0] = mapWidth;
  p->mapInfo[1] = mapHeight;
  p->mapInfo[2] = heightUnit;
  p->mapInfo[3] = waterLevel;
  p->info[0] = plantDensity * plantCell * plantCell;
  p->info[1] = cleared ? 1.0f : 0.0f;  // [tarmac]
  p->info[2] = plants_.patchScale;
  p->info[3] = plants_.patchCover;
  p->shown[0] = plantsFar ? cameInShown_ : shown_;  // [cover] ([plantlod] with plantsFar not by the camera's distance)
  p->shown[1] = bare ? 1.0f : 0.0f;
  p->shown[2] = pixelAngle;
  p->shown[3] = bladeWidth;
  std::copy(&plants_.cells[0][0][0], &plants_.cells[0][0][0] + 16 * 3 * 4, &p->cells[0][0]);
  // each cell's scale: the low plants `plantBoost` times the factor that brings the tallest to `plantTallest`, but
  // none taller than that (the readability rule): the stalks and the tall flowers at the cap, the grass and the
  // small flowers bigger than their real proportion, so that they read from the game's camera (the visuals
  // track's advice)
  const float metre = plantTallest / std::max(plants_.tallest, 1e-3f);
  for (int i = 0; i < 16; ++i) p->cells[i * 3 + 2][3] = std::min(metre * plantBoost, plantTallest / std::max(plants_.cells[i][1][2], 1e-3f));
  std::copy(&plants_.order[0][0], &plants_.order[0][0] + 16, &p->order[0][0]);
  std::copy(&plants_.groups[0][0], &plants_.groups[0][0] + 24, &p->groups[0][0]);
  for (int i = 0; i < 16; ++i) {  // [coverdark] (how far the grass's clumps take the ground's colour; [cover] the leaves its brightness)
    std::copy(plants_.tones[i], plants_.tones[i] + 3, p->tones[i]);
    p->tones[i][3] = plants_.tones[i][3] * (plants_.tones[i][3] > 0 ? clumpsToGround : leavesToGround);
    if (plants_.tones[i][3] < 0 && leavesToGround > 0 && leavesBaked > 0) {
      // [cover] the leaves' art already carries `leavesBaked` of the lift: the screen as before, the art's mean c' = k c,
      // c' x f = c x lerp(1, g / c, w): f = (1 - w) / k + w g / c' = lerp(1, g / L, w') with w' = 1 - (1 - w) / k and
      // L = c' w' / w (the shader takes the leaves' tone only for its brightness)
      const float w = leavesToGround, wk = 1 - (1 - w) / leavesBaked;
      for (int k = 0; k < 3; ++k) p->tones[i][k] *= wk / w;
      p->tones[i][3] = -wk;
    }
  }
  if (reacting) std::copy(plants->Constants(), plants->Constants() + 8, &p->plantsK[0][0]);  // [plants]
  // [plantlod] (plantsFar's: kPlantLodBlock)
  if (plantsFar && tiles_) {
    p->lod[0][2] = tiles_->focal;
    p->lod[1][0] = plantLod.collapseHigh;
    p->lod[1][1] = std::min(plantLod.collapseLow, plantLod.collapseHigh - 1);
    p->lod[1][2] = plantLod.cutLow * tiles_->cutScale;
    p->lod[1][3] = std::max(plantLod.cutHigh, plantLod.cutLow) * tiles_->cutScale;
    p->lod[2][0] = prepared_ ? 1.0f : 0.0f;
    const bool models = ModelsOn();
    p->lod[2][1] = models ? 1.0f : 0.0f;
    p->lod[2][2] = plantLod.modelBand;
    for (int i = 0; i < 16; ++i) p->cellModels[i][0] = -1;
    int extra = 0;
    if (models)
      for (size_t kind = 0; kind < models_->kinds.size(); ++kind) {
        const PlantModels::Kind& m = models_->kinds[kind];
        p->modelKinds[kind][0] = static_cast<float>(m.levelCount);
        p->modelKinds[kind][1] = m.card >= 0 ? 1.0f : 0.0f;
        // its colour: its leaves' mean brought to its card's (the stats' means, linear), so that it reads as its card does
        // where it changes to it; a kind with no card, or no means: its own
        const bool means = m.mean[0] + m.mean[1] + m.mean[2] > 0;
        for (int c = 0; c < 3; ++c)
          p->modelTint[kind][c] = m.card >= 0 && means && plants_.tones[m.card][c] > 0
                                      ? std::clamp(plants_.tones[m.card][c] / std::max(m.mean[c], 1e-4f), 0.5f, 2.0f)
                                      : 1.0f;
        // (the card it ends on as wide as it: its width over the card's frame, the visuals session's ask)
        p->modelTint[kind][3] = m.card >= 0 && plants_.cells[m.card][1][0] > 0
                                    ? std::clamp(std::max(m.size[0], m.size[1]) / plants_.cells[m.card][1][0], 0.3f, 1.5f)
                                    : 1.0f;
        // the pixels its levels go down to: by its size on screen, or where its json gives distances, by them (its width
        // over each distance)
        float bounds[3];
        const float frame = m.card >= 0 ? FrameOf(m.card) : ModelFrame(m);
        for (int l = 0; l < 3; ++l) bounds[l] = m.swap[l] > 0 ? frame * tiles_->focal / m.swap[l] : plantLod.modelPixels[l];
        float* model = nullptr;
        if (m.card >= 0) {
          model = p->cellModels[m.card];
        } else if (extra < 8) {
          // a kind with no card: a cell of its own past the cover's (its size and more as the cells', its share of a
          // group's spots)
          model = p->extraCells[extra * 3];
          float* size = p->extraCells[extra * 3 + 1];
          float* more = p->extraCells[extra * 3 + 2];
          size[0] = std::max(m.size[0], m.size[1]);
          size[1] = m.size[2] * 0.8f;
          size[2] = m.size[2] * 1.2f;
          size[3] = m.size[2];
          more[0] = m.sway;
          more[1] = static_cast<float>(m.clump[0]);
          more[2] = static_cast<float>(m.clump[1]);
          more[3] = ModelScale(m);
          p->extraRules[extra][0] = static_cast<float>(m.placeGroup);
          p->extraRules[extra][1] = m.placeShare;
          // (its leaves' brightness brought toward the ground's as the weeds' and ferns' are, by its own mean)
          if (m.mean[0] + m.mean[1] + m.mean[2] > 0 && leavesToGround > 0) {
            std::copy(m.mean, m.mean + 3, p->extraTones[extra]);
            p->extraTones[extra][3] = -leavesToGround;
          }
          ++extra;
        }
        if (!model) continue;
        model[0] = static_cast<float>(kind);
        for (int l = 0; l < 3; ++l) model[1 + l] = bounds[l];
      }
    p->lod[2][3] = static_cast<float>(extra);
    // (the far field: where it lies, made this frame or not)
    p->farField[0] = farArea_[0];
    p->farField[1] = farArea_[1];
    p->farField[2] = 1.0f / std::max(farArea_[2] * farArea_[3], 1e-3f);
    p->farField[3] = farFieldReady_ ? 1.0f : 0.0f;
    // (for pictures, PlantLodTune::onlyCell: that cell and its group; -1 as the table says)
    p->lod[0][3] = -1;
    const int only = plantLod.onlyCell;
    if (only >= 0 && only < 16 && plants_.cells[only][1][0] > 0) {
      for (int g = 0; g < 6; ++g)
        for (int j = 0; j < static_cast<int>(plants_.groups[g][1]); ++j) {
          const int at = static_cast<int>(plants_.groups[g][0]) + j;
          if (static_cast<int>(plants_.order[at / 4][at % 4]) == only) p->lod[3][3] = static_cast<float>(g);
        }
      p->lod[0][3] = static_cast<float>(only);
    } else if (only >= 16 && only < 16 + extra) {
      p->lod[3][3] = p->extraRules[only - 16][0];
      p->lod[0][3] = static_cast<float>(only);
    }
  }
  return plantSide;
}

// [plantlod] A cell's card's width in world units at its own height (as Draw scales it), 0 where the cover has no such cell.
float GroundCover::FrameOf(int cell) const {
  if (cell < 0 || cell >= 16 || plants_.cells[cell][1][0] <= 0 || plants_.cells[cell][1][3] <= 0) return 0;
  const float metre = plantTallest / std::max(plants_.tallest, 1e-3f);
  const float scale = std::min(metre * plantBoost, plantTallest / std::max(plants_.cells[cell][1][2], 1e-3f));
  return plants_.cells[cell][1][0] * scale;
}

// [plantlod] A model with no card: world units a metre of it (as the cells are scaled, by its tallest, but to modelTallest:
// a bush stands over the ferns, as the map's small bushes do), and its width.
float GroundCover::ModelScale(const PlantModels::Kind& m) const {
  const float metre = plantTallest / std::max(plants_.tallest, 1e-3f);
  return std::min(metre * plantBoost, modelTallest / std::max(m.size[2] * 1.2f, 1e-3f));
}
float GroundCover::ModelFrame(const PlantModels::Kind& m) const { return std::max(m.size[0], m.size[1]) * ModelScale(m); }
bool GroundCover::ModelsOn() const { return plantModels && models_ && !models_->kinds.empty(); }

void GroundCover::Draw(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover,
                       gfx::Texture normals, float mapWidth, float mapHeight, float waterLevel, gfx::Texture densityA, gfx::Texture densityB,
                       gfx::Texture blasts) {
  drawnBlades = drawnPlants = 0;
  drawnTiles = drawnModels = 0;  // [plantlod]
  drawn_ = Drawn{};  // [leftover]
  if (!blastPlantPipeline_) blasts = {};  // (PrepareBlasts has not made them)
  const bool farPlants = plantsFar && farReady_;  // [plantlod] (the plants by their tiles, with or without the blades)
  if ((!prepared_ && !farPlants) || !heights || !cover || !normals || view.height <= 0) return;
  const bool reacting = plants && plants->Ready() && reactingMade_ > 0;  // [plants]
  const int with = blasts ? 2 : 0;                                       // [plants] (reactingPipelines_: the blasts' pair)
  const int side = static_cast<int>(area_[3]);
  const float reach = 0.5f * area_[2] * area_[3];
  GrassConstants k{};
  std::copy(area_, area_ + 4, k.area);
  k.blades[0] = bladeHeight;
  k.blades[1] = bladeWidth;
  k.blades[2] = reach * 1.4f;
  k.blades[3] = heightUnit;
  k.mapInfo[0] = mapWidth;
  k.mapInfo[1] = mapHeight;
  k.mapInfo[2] = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  k.mapInfo[3] = waterLevel;
  const bool table = densityA && densityB;
  k.table[0] = table ? 1.0f : 0.0f;
  k.table[1] = cleared ? 1.0f : 0.0f;  // [tarmac]
  device_->SetTexture(11, cleared ? cleared : trampled_);  // [tarmac] (read only with table.y)
  k.shown[0] = shown_;              // [cover]
  k.shown[1] = bare ? 1.0f : 0.0f;  // [cover]
  device_->SetTexture(12, bare ? bare : trampled_);  // [cover] (read only with shown.y)
  if (blasts) device_->SetTexture(8, blasts);
  if (reacting) {  // [plants]
    std::copy(plants->Constants(), plants->Constants() + 8, &k.plantsK[0][0]);
    device_->SetTexture(9, plants->Press());
    device_->SetTexture(10, plants->Burnt());
  }
  // [leftover] under lying snow (kCoverShared's CoverSnow; the snow ground's shelter at t13 where it is read)
  const bool snowing = view.constants.weather[1] > 0 && snowDepth > 0;
  const bool shelter = snowing && snowShelter;
  const float wind = std::sqrt(snowWind[0] * snowWind[0] + snowWind[1] * snowWind[1]);
  const CoverSnowConstants snow{{snowing ? snowDepth : 0.0f, shelter ? 1.0f : 0.0f, snowing && snowMaterials ? 1.0f : 0.0f, 0},  // ([plantlod] z)
                                {wind > 0 ? snowWind[0] / wind : 0.0f, wind > 0 ? snowWind[1] / wind : 0.0f, snowTune[1], snowTune[3]}};
  device_->SetTexture(13, shelter ? snowShelter : trampled_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(3, &snow, sizeof snow);  // [leftover]
  device_->SetTexture(0, heights);
  device_->SetTexture(1, cover);
  device_->SetTexture(2, normals);
  device_->SetTexture(3, trampled_);
  device_->SetTexture(4, table ? densityA : cover);
  if (prepared_) {  // [plantlod] (the blades: where Prepare laid out their square this frame; always without plantsFar)
    device_->SetPipeline(reacting ? reactingPipelines_[with] : blasts ? blastPipeline_ : pipeline_);
    device_->SetConstants(1, &k, sizeof k);
    drawnBlades = side * side * 3;
    device_->Draw(static_cast<uint32_t>(drawnBlades) * 15);
    // [leftover] (what was drawn with, for the motion: DrawMotion)
    drawn_.blades = true;
  }
  drawn_.reacting = reacting;
  drawn_.blasts = static_cast<bool>(blasts);
  drawn_.heights = heights;
  drawn_.cover = cover;
  drawn_.normals = normals;
  drawn_.densityA = table ? densityA : cover;
  drawn_.blastField = blasts;
  drawn_.cleared = cleared ? cleared : trampled_;
  drawn_.bare = bare ? bare : trampled_;
  drawn_.press = reacting ? plants->Press() : gfx::Texture{};
  drawn_.burnt = reacting ? plants->Burnt() : gfx::Texture{};
  drawn_.shelter = shelter ? snowShelter : trampled_;
  drawnConstants_[0].assign(reinterpret_cast<const float*>(&k), reinterpret_cast<const float*>(&k) + sizeof k / sizeof(float));
  std::memcpy(snowConstants_, &snow, sizeof snow);
  // the plants, where the map has them: their spots over the same square, a little coarser
  if (table && plantDensity > 0 && plantPipeline_ && atlas_ && plants_.count) {
    auto p = std::make_unique<PlantConstants>();
    const int plantSide = FillPlantConstants(p.get(), mapWidth, mapHeight, heightUnit, waterLevel, k.mapInfo[2], reacting);
    device_->SetPipeline(reacting ? reactingPipelines_[with + 1] : blasts ? blastPlantPipeline_ : plantPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, p.get(), sizeof(PlantConstants));
    device_->SetConstants(3, &snow, sizeof snow);  // [leftover]
    device_->SetTexture(5, densityB);
    device_->SetTexture(6, atlas_);
    device_->SetTexture(7, atlasNormals_);
    if (farPlants) {
      // [plantlod] the tiles (PrepareFar), a draw for each count of corners a spot; then the models
      drawn_.plants = true;
      drawn_.far = true;
      drawn_.densityB = densityB;
      DrawFarPlants(p.get(), reacting ? 2 + (blasts ? 1 : 0) : blasts ? 1 : 0, false);
      drawnConstants_[1].assign(reinterpret_cast<const float*>(p.get()), reinterpret_cast<const float*>(p.get()) + sizeof(PlantConstants) / sizeof(float));
    } else {
      drawnPlants = plantSide * plantSide;
      device_->Draw(static_cast<uint32_t>(drawnPlants) * 108);  // ([cover] up to six stars of three cards, or a tuft of twelve blades)
      drawn_.plants = true;  // [leftover] (for the motion)
      drawn_.densityB = densityB;
      drawnConstants_[1].assign(reinterpret_cast<const float*>(p.get()), reinterpret_cast<const float*>(p.get()) + sizeof(PlantConstants) / sizeof(float));
    }
  }
  for (int slot = 0; slot < (reacting ? 11 : 9); ++slot) device_->SetTexture(slot, {});
  device_->SetTexture(11, {});  // [tarmac]
  device_->SetTexture(12, {});  // [cover]
  device_->SetTexture(13, {});  // [leftover]
  if (farPlants)  // [plantlod]
    for (int slot = 14; slot <= 21; ++slot) device_->SetTexture(slot, {});
}

// [plantlod] The plants of this frame's tiles (Draw, and DrawMotion with `motion`; the plants' pipeline of the kind
// `variant` - 0 plain, 1 the blasts' waves, 2 reacting, 3 both - set by the caller for the cards, its constants `p`), and
// the models where there are any.
void GroundCover::DrawFarPlants(PlantConstants* p, int variant, bool motion) {
  if (!tiles_ || !tileBuffer_) return;
  if (!modelsPlaced_) p->lod[2][1] = 0;  // (no models this frame: the flags not written, every plant a card)
  device_->SetShaderBuffer(tileBuffer_, 0);
  device_->SetTexture(14, models_ ? models_->Flags() : trampled_);
  device_->SetTexture(21, farFieldReady_ ? farField_ : trampled_);  // (read only with farField.w)
  for (const PlantTiles::Batch& b : tiles_->batches) {
    p->lod[0][0] = static_cast<float>(b.corners);
    p->lod[0][1] = static_cast<float>(b.first);
    device_->SetConstants(1, p, sizeof(PlantConstants));
    device_->Draw(static_cast<uint32_t>(b.count) * 256u * static_cast<uint32_t>(b.corners));
    if (!motion) {
      drawnTiles += b.count;
      drawnPlants += b.count * 256;
    }
  }
  // the models: each kind's levels, as many as there was room for (those past the count put nowhere)
  if (!modelsPlaced_ || !models_) return;
  const gfx::Pipeline pipeline = ModelPipeline(variant + (motion ? 4 : 0));
  if (!pipeline) return;
  device_->SetPipeline(pipeline);
  device_->SetTexture(15, models_->Instances());
  device_->SetTexture(16, models_->Counters());
  for (size_t kind = 0; kind < models_->kinds.size(); ++kind) {
    const PlantModels::Kind& m = models_->kinds[kind];
    device_->SetTexture(17, m.albedo);
    device_->SetTexture(18, m.normal);
    device_->SetTexture(19, m.spec ? m.spec : m.albedo);
    device_->SetTexture(20, m.ao);
    for (int l = 0; l < m.levelCount && l < PlantModels::kLevels; ++l) {
      const int bucket = static_cast<int>(kind) * PlantModels::kLevels + l;
      const uint32_t room = room_[bucket];
      if (!room) continue;
      p->lod[3][0] = static_cast<float>(bucket);
      p->lod[3][1] = m.levels[l].height;
      p->lod[3][2] = m.spec ? 1.0f : 0.0f;
      device_->SetConstants(1, p, sizeof(PlantConstants));
      device_->SetVertexBuffer(m.levels[l].vertices);
      device_->SetIndexBuffer(m.levels[l].indices);
      if (modelArgsNow_) {  // [gpuscenery] (as many as its list holds: not counted here)
        device_->DrawIndexedIndirect(modelArgsBuffer_, static_cast<uint32_t>(bucket) * 32u);
        continue;
      }
      device_->DrawIndexed(m.levels[l].indexCount, 0, 0, room);
      if (!motion) drawnModels += static_cast<int>(room);
    }
  }
}

// [plantlod] The models' pipelines (made the first time each is wanted, from the plants' text for the switches): `k` 0
// plain, 1 the blasts' waves, 2 reacting, 3 both; + 4 the motion's.
gfx::Pipeline GroundCover::ModelPipeline(int k) {
  if (k < 0 || k >= 8) return {};
  if (!modelTried_[k]) {
    modelTried_[k] = true;
    const int kind = k & 3;
    const std::string& scene = kind == 0 ? plantSource_ : kind == 1 ? blastPlantSource_ : kind == 2 ? reactingSources_[1] : reactingSources_[3];
    const std::string source = (k >= 4 ? "#define MOTION 1\n" : "") + scene;
    gfx::PipelineDesc pd;
    pd.name = k >= 4 ? "ground cover: plant models, motion" : "ground cover: plant models";
    pd.vertexSource = pd.pixelSource = source;
    pd.vertexEntry = "MVS";
    pd.pixelEntry = k >= 4 ? "MotionMPS" : "MPS";
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0},
                {"NORMAL", 0, gfx::AttrType::Float3, 12},
                {"TANGENT", 0, gfx::AttrType::Float3, 24},
                {"BINORMAL", 0, gfx::AttrType::Float3, 36},
                {"TEXCOORD", 0, gfx::AttrType::Float2, 48}};
    pd.stride = 68;
    pd.cull = gfx::Cull::None;  // (seen from both sides, lit by the same normals: the visuals session's)
    if (k >= 4) {
      pd.colorFormat = gfx::Format::RG16F;
      pd.depthWrite = false;
      pd.depthBias = 16;
    } else {
      pd.colorFormat = kSceneFormat;
      pd.samples = gSceneSamples;
      pd.alphaToCoverage = true;
    }
    std::string error;
    modelPipelines_[k] = device_->CreatePipeline(pd, &error);
    if (!modelPipelines_[k]) fprintf(stderr, "render: the ground cover's 3D plants cannot be drawn (%s)\n", error.c_str());
  }
  return modelPipelines_[k];
}

// [plantlod] (grass.h)
void GroundCover::SetHeights(const std::vector<uint16_t>& heights, int width, int height, float heightScale, float waterLevel) {
  if (!tiles_) tiles_ = std::make_unique<PlantTiles>();
  tiles_->SetHeights(heights, width, height, heightScale, waterLevel);
}

// [plantlod] (grass.h) Kept for when the models are first wanted (their files read then: LoadPlantModels); and the most
// each group's spots may be of a cell, from the cover's table (for the models' room).
void GroundCover::SetPlantModels(const JsonValue& cover, TextureCache* textures, const std::string& folder) {
  if (models_) models_->Set(JsonValue{}, textures, folder, {});  // (the last map's let go)
  modelsCover_ = cover;
  modelsTextures_ = textures;
  modelsFolder_ = folder;
  modelsLoaded_ = false;
  float most[4] = {0, 0, 0, 0};  // clumps, flowers, stalks, ferns over the layers
  for (const auto& [layer, d] : cover["density"]["layers"].members) {
    most[0] = std::max(most[0], static_cast<float>(d["clumps"].NumberOr(0)));
    most[1] = std::max(most[1], static_cast<float>(d["flowers"].NumberOr(0)));
    most[2] = std::max(most[2], static_cast<float>(d["stalks"].NumberOr(0)));
    most[3] = std::max(most[3], static_cast<float>(d["fern"].NumberOr(0)));
  }
  const JsonValue& near = cover["density"]["near_trees"];
  most[0] *= std::max(1.0f, static_cast<float>(near["clumps"].NumberOr(1)));
  most[1] *= std::max(1.0f, static_cast<float>(near["flowers"].NumberOr(1))) / std::max(plants_.patchCover, 0.05f);  // (in their patches)
  most[2] *= std::max(1.0f, static_cast<float>(near["stalks"].NumberOr(1)));
  most[3] += static_cast<float>(near["fern_add"].NumberOr(0));
  const float byGroup[6] = {most[0], most[0], most[0], most[1], most[2], most[3]};
  std::copy(byGroup, byGroup + 6, densityMost_);
}

// [plantlod] The cover's models read, the first time they are wanted after a map opened (none read without plantModels).
void GroundCover::LoadPlantModels() {
  if (modelsLoaded_ || !plantModels || !modelsTextures_) return;
  modelsLoaded_ = true;
  if (!models_) models_ = std::make_unique<PlantModels>(device_);
  std::vector<std::string> names(16);
  for (const auto& [name, c] : modelsCover_["cells"].members) {
    const int index = static_cast<int>(c["index"].NumberOr(-1));
    if (index >= 0 && index < 16) names[static_cast<size_t>(index)] = name;
  }
  models_->Set(modelsCover_, modelsTextures_, modelsFolder_, names);
}

// [plantlod] The models' placing's two compute pipelines, made the first time wanted (PrepareFar, PrepareAhead).
// [gpuscenery] (indirectModels) Each kind's level's draw from its list's count (as many as fit its room), on the card: an
// indexed draw's five arguments, eight words a list (gfx.h, DrawIndexedIndirect).
static const char kModelArgsShader[] = R"hlsl(
Texture2D<uint> placedCounters : register(t0);
RWByteAddressBuffer modelArgs : register(u0, space1);
cbuffer ModelArgs : register(b2) {
  uint4 argIndices[8];  // per kind * 4 + level: its mesh's indices
  uint4 argRoom[8];     // and its list's room
};
[numthreads(32, 1, 1)] void ArgsCS(uint3 id : SV_DispatchThreadID) {
  uint b = id.x;
  uint n = min(placedCounters.Load(int3(b, 0, 0)), argRoom[b / 4][b % 4]);
  modelArgs.Store4(b * 32, uint4(argIndices[b / 4][b % 4], n, 0, 0));
  modelArgs.Store(b * 32 + 16, 0u);
}
)hlsl";

void GroundCover::MakeModelPlace() {
  if (indirectModels && !modelArgsTried_ && device_->IndirectDraws()) {  // [gpuscenery]
    modelArgsTried_ = true;
    std::string error;
    gfx::ComputeDesc cd;
    cd.name = "ground cover: plant models' draws";
    cd.source = kModelArgsShader;
    cd.entry = "ArgsCS";
    modelArgs_ = device_->CreateCompute(cd, &error);
    gfx::BufferDesc bd;
    bd.kind = gfx::BufferKind::Card;
    bd.size = PlantModels::kMostKinds * PlantModels::kLevels * 32;
    if (modelArgs_) modelArgsBuffer_ = device_->CreateBuffer(bd);
    if (!modelArgsBuffer_) fprintf(stderr, "render: the ground cover's 3D plants cannot be drawn indirectly (%s)\n", error.c_str());
  }
  if (modelPlaceTried_) return;
  {
    modelPlaceTried_ = true;
    const std::string source = std::string("#define PLANT_LOD 1\n") + kViewShaderBlock + kCoverShared + kPlantDecls + kPlantLodBlock + kPlantModelPlace;
    std::string error;
    gfx::ComputeDesc cd;
    cd.name = "ground cover: plant models placed";
    cd.source = source;
    cd.entry = "CS";
    modelPlace_ = device_->CreateCompute(cd, &error);
    cd.entry = "ClearCS";
    if (modelPlace_) modelClear_ = device_->CreateCompute(cd, &error);
    if (!modelClear_) fprintf(stderr, "render: the ground cover's 3D plants cannot be placed (%s)\n", error.c_str());
  }
}

// [plantlod] (grass.h)
void GroundCover::PrepareAhead(bool reacting) {
  if (!plantsFar || !plantModels || !pipeline_) return;
  MakePlantVariant();
  LoadPlantModels();
  if (!ModelsOn()) return;
  ModelPipeline(0);
  if (reacting) ModelPipeline(2);
  MakeModelPlace();
}

// [plantlod] (grass.h)
void GroundCover::PrepareFar(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover, float mapWidth,
                             float mapHeight, float waterLevel, gfx::Texture densityA, gfx::Texture densityB) {
  farReady_ = false;
  modelsPlaced_ = false;
  if (!plantsFar || !pipeline_ || !atlas_ || !plants_.count || !densityA || !densityB || plantDensity <= 0 || view.height <= 0 || !heights) return;
  MakePlantVariant();
  if (!plantPipeline_) return;
  if (!tiles_) tiles_ = std::make_unique<PlantTiles>();
  far_ = {cover, densityA, densityB, mapWidth, mapHeight, waterLevel};
  LoadPlantModels();
  // the cells' sizes (and the models', and the kinds with no card)
  const bool models = ModelsOn();
  std::vector<PlantTiles::CellSize> sizes(16);
  for (int i = 0; i < 16; ++i) {
    sizes[static_cast<size_t>(i)].frame = FrameOf(i);
    sizes[static_cast<size_t>(i)].clumpMost = std::clamp(static_cast<int>(plants_.cells[i][2][2]), 1, 6);
  }
  if (models)
    for (const PlantModels::Kind& m : models_->kinds) {
      PlantTiles::CellSize c;
      c.model = true;
      if (m.card >= 0) {
        c = sizes[static_cast<size_t>(m.card)];
        c.model = true;
        c.modelDownTo = m.swap[2] > 0 ? c.frame * tiles_->focal / m.swap[2] : plantLod.modelPixels[std::min(m.levelCount, 3) - 1];
        sizes[static_cast<size_t>(m.card)] = c;
      } else {
        c.frame = ModelFrame(m);
        c.clumpMost = m.clump[1];
        c.cardless = true;
        c.modelDownTo = 0;  // (down to the cut)
        sizes.push_back(c);
      }
    }
  tiles_->Select(view, plantCell, sizes, plantLod, plantTallest * 3);
  if (!tileBuffer_) {
    gfx::BufferDesc bd;
    bd.kind = gfx::BufferKind::Shader;
    bd.stride = 16;
    bd.dynamic = true;
    tileBuffer_ = device_->CreateBuffer(bd);
  }
  if (!tileBuffer_) return;
  if (tiles_->Count()) device_->UpdateBuffer(tileBuffer_, tiles_->rows.data(), static_cast<uint32_t>(tiles_->rows.size() * sizeof(float)));
  else tiles_->batches.clear();
  farReady_ = true;
  // the far field over the tiles (PlaceFar): a square round their bounds
  if (tiles_->Count()) {
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    const float tileWide = plantCell * PlantTiles::kSide;
    for (size_t r = 0; r + 3 < tiles_->rows.size(); r += 4) {
      x0 = std::min(x0, tiles_->rows[r] * plantCell);
      z0 = std::min(z0, tiles_->rows[r + 1] * plantCell);
      x1 = std::max(x1, tiles_->rows[r] * plantCell + tileWide);
      z1 = std::max(z1, tiles_->rows[r + 1] * plantCell + tileWide);
    }
    const float wide = std::max({x1 - x0, z1 - z0, 1.0f});
    farArea_[0] = 0.5f * (x0 + x1 - wide);
    farArea_[1] = 0.5f * (z0 + z1 - wide);
    farArea_[2] = wide / static_cast<float>(kFarField);
    farArea_[3] = static_cast<float>(kFarField);
  }
}

// [plantlod] (grass.h) The far field (where pads lie and what stands on the ground: the plants beyond the square are
// cleared by it as the near ones are), then the models' placing.
gfx::Texture GroundCover::FarField() {
  if (!farField_) {
    gfx::TargetDesc td;
    td.width = td.height = kFarField;
    td.format = gfx::Format::R8;
    farField_ = device_->CreateTarget(td);
  }
  return farField_;
}

void GroundCover::PlaceFar(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, float mapWidth, float mapHeight,
                           float waterLevel, bool padsDrawn) {
  farFieldReady_ = false;
  if (!plantsFar || !farReady_ || !tiles_) return;
  if (!farDiscsTried_) {
    farDiscsTried_ = true;
    const std::string source = std::string(kViewShaderBlock) + kFarDiscShader;
    gfx::PipelineDesc pd;
    pd.name = "ground cover: the far field's footprints";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.blend = gfx::Blend::Additive;
    pd.colorFormat = gfx::Format::R8;
    std::string error;
    farDiscPipeline_ = device_->CreatePipeline(pd, &error);
    if (!farDiscPipeline_) fprintf(stderr, "render: the ground cover's far field cannot be made (%s)\n", error.c_str());
  }
  if (plantLod.farClear > 0 && FarField() && farDiscPipeline_) {
    // what stands on the ground over the far field (Prepare's discs), 256 a draw, added over the pads the host drew
    const float wide = farArea_[2] * farArea_[3];
    struct {
      float area[4];
      float discs[256][4];
    } k = {};
    std::copy(farArea_, farArea_ + 4, k.area);
    std::vector<float> inside;
    for (size_t i = 0; i + 2 < farDiscs_.size(); i += 3) {
      const float x = farDiscs_[i], z = farDiscs_[i + 1], r = farDiscs_[i + 2] + 0.5f;
      if (x + r < farArea_[0] || z + r < farArea_[1] || x - r > farArea_[0] + wide || z - r > farArea_[1] + wide) continue;
      inside.insert(inside.end(), {farDiscs_[i], farDiscs_[i + 1], farDiscs_[i + 2], 0});
    }
    if (!padsDrawn || !inside.empty()) {
      gfx::PassDesc pass;
      pass.color = farField_;
      pass.clearColor = !padsDrawn;
      device_->BeginPass(pass);
      device_->SetPipeline(farDiscPipeline_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      for (size_t at = 0; at < inside.size(); at += 256 * 4) {
        const size_t n = std::min<size_t>(256, (inside.size() - at) / 4);
        std::copy(inside.begin() + static_cast<std::ptrdiff_t>(at), inside.begin() + static_cast<std::ptrdiff_t>(at + n * 4), &k.discs[0][0]);
        device_->SetConstants(1, &k, static_cast<uint32_t>(sizeof(float) * 4 * (1 + n)));
        device_->Draw(static_cast<uint32_t>(n * 6));
      }
    }
    farFieldReady_ = true;
  }
  // the models' placing: their room this frame (by the tiles they may be in, at most what the cover's table lets grow
  // there), then a thread a spot of those tiles
  const bool models = ModelsOn();
  const gfx::Texture densityA = far_.densityA, densityB = far_.densityB;
  std::fill(room_, room_ + PlantModels::kMostKinds * PlantModels::kLevels, 0u);
  if (!models || tiles_->modelRows.empty()) return;
  MakeModelPlace();
  if (!modelPlace_ || !modelClear_ || !models_->Instances() || !models_->Counters() || !models_->Flags()) return;
  {  // the room: per tile the levels its plants may be at (from its nearest point to its farthest), its spots by the chance
     // of the kind's group (and of the kind in it), and its most plants a spot
    const float k = plantDensity * plantCell * plantCell;
    const float diagonal = plantCell * PlantTiles::kSide * 1.5f + plantTallest * 3;
    double want[PlantModels::kMostKinds * PlantModels::kLevels] = {};
    for (size_t kind = 0; kind < models_->kinds.size(); ++kind) {
      const PlantModels::Kind& m = models_->kinds[kind];
      int group = m.placeGroup;
      float chance;
      if (m.card >= 0) {
        group = -1;
        for (int g = 0; g < 6 && group < 0; ++g)
          for (int j = 0; j < static_cast<int>(plants_.groups[g][1]); ++j)
            if (static_cast<int>(plants_.order[(static_cast<int>(plants_.groups[g][0]) + j) / 4][(static_cast<int>(plants_.groups[g][0]) + j) % 4]) == m.card) group = g;
        if (group < 0) continue;
        chance = std::min(1.0f, k * densityMost_[group]) / std::max(plants_.groups[group][1], 1.0f);
      } else {
        chance = std::min(1.0f, k * densityMost_[group]) * m.placeShare;
      }
      const float frame = m.card >= 0 ? FrameOf(m.card) : ModelFrame(m);
      float bounds[3];
      for (int l = 0; l < 3; ++l) bounds[l] = m.swap[l] > 0 ? frame * tiles_->focal / m.swap[l] : plantLod.modelPixels[l];
      const int last = std::min(m.levelCount, PlantModels::kLevels) - 1;
      auto levelAt = [&](float px) {
        const int level = px >= bounds[0] ? 0 : px >= bounds[1] ? 1 : px >= bounds[2] ? 2 : 3;
        return level <= last ? level : m.card >= 0 ? -1 : last;
      };
      const int clump = m.card >= 0 ? std::clamp(static_cast<int>(plants_.cells[m.card][2][2]), 1, 6) : m.clump[1];
      for (size_t r = 0; r + 3 < tiles_->modelRows.size(); r += 4) {
        const float dNear = tiles_->modelRows[r + 3];
        const float pxNear = frame * tiles_->focal / dNear * (1 + 0.5f * plantLod.modelBand);
        const float pxFar = frame * tiles_->focal / (dNear + diagonal) * (1 - 0.5f * plantLod.modelBand);
        const int from = levelAt(pxNear), to = levelAt(pxFar);
        if (from < 0) continue;
        for (int l = from; l <= (to < 0 ? last : to); ++l) want[kind * PlantModels::kLevels + static_cast<size_t>(l)] += 256.0 * chance * clump;
      }
    }
    for (int b = 0; b < PlantModels::kMostKinds * PlantModels::kLevels; ++b)
      if (want[b] > 0) room_[b] = static_cast<uint32_t>(std::min<double>(PlantModels::kMostInBucket, want[b] * 1.3 + 32));
  }
  // [gpuscenery] (drawn by the lists' own counts: each list that may have plants its whole room)
  modelArgsNow_ = indirectModels && modelArgs_ && modelArgsBuffer_ && device_->IndirectDraws();
  if (modelArgsNow_)
    for (uint32_t& r : room_)
      if (r) r = PlantModels::kMostInBucket;
  if (!modelTileBuffer_) {
    gfx::BufferDesc bd;
    bd.kind = gfx::BufferKind::Shader;
    bd.stride = 16;
    bd.dynamic = true;
    modelTileBuffer_ = device_->CreateBuffer(bd);
  }
  if (!modelTileBuffer_) return;
  device_->UpdateBuffer(modelTileBuffer_, tiles_->modelRows.data(), static_cast<uint32_t>(tiles_->modelRows.size() * sizeof(float)));
  auto p = std::make_unique<PlantConstants>();
  FillPlantConstants(p.get(), mapWidth, mapHeight, heightUnit, waterLevel, 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height), false);
  struct {
    float room[8][4];
    float info[4];
  } place = {};
  for (int b = 0; b < 32; ++b) place.room[b / 4][b % 4] = static_cast<float>(room_[b]);
  const int modelTiles = static_cast<int>(tiles_->modelRows.size() / 4);
  place.info[0] = static_cast<float>(modelTiles);
  device_->SetPipeline(modelClear_);
  device_->SetStorage(0, models_->Counters());
  device_->Dispatch(1, 1, 1);
  device_->SetPipeline(modelPlace_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, p.get(), sizeof(PlantConstants));
  device_->SetConstants(2, &place, sizeof place);
  device_->SetShaderBuffer(modelTileBuffer_, 0);
  device_->SetTexture(0, heights);
  device_->SetTexture(4, densityA);
  device_->SetTexture(5, densityB);
  device_->SetTexture(11, cleared ? cleared : trampled_);
  device_->SetTexture(12, bare ? bare : trampled_);
  device_->SetTexture(21, farFieldReady_ ? farField_ : trampled_);
  device_->SetStorage(0, models_->Counters());
  device_->SetStorage(1, models_->Instances());
  device_->SetStorage(2, models_->Flags());
  device_->Dispatch(static_cast<uint32_t>(modelTiles) * 4u, 1, 1);
  for (int s = 0; s < 3; ++s) device_->SetStorage(s, {});
  for (int t : {0, 4, 5, 11, 12, 21}) device_->SetTexture(t, {});
  if (modelArgsNow_) {  // [gpuscenery] each list's draw from its count
    struct {
      uint32_t indices[8][4];
      uint32_t room[8][4];
    } args = {};
    for (size_t kind = 0; kind < models_->kinds.size() && kind < static_cast<size_t>(PlantModels::kMostKinds); ++kind)
      for (int l = 0; l < models_->kinds[kind].levelCount && l < PlantModels::kLevels; ++l) {
        const size_t b = kind * PlantModels::kLevels + static_cast<size_t>(l);
        args.indices[b / 4][b % 4] = models_->kinds[kind].levels[l].indexCount;
        args.room[b / 4][b % 4] = room_[b];
      }
    device_->SetPipeline(modelArgs_);
    device_->SetConstants(2, &args, sizeof args);
    device_->SetTexture(0, models_->Counters());
    device_->SetShaderBuffer(modelArgsBuffer_, 0);
    device_->Dispatch(1, 1, 1);
    device_->SetTexture(0, {});
  }
  modelsPlaced_ = true;
}

// [plantlod] The far cover's constants (kFarCoverShader's FarCover).
namespace {
struct FarCoverConstants {
  float cameraRight[4], cameraUp[4], cameraBack[4];
  float mapInfo[4], cutInfo[4], bladeInfo[4], bladeLook[4];
  float groupLook[6][4], groupSize[6][4];
  float farMore[4];
  float farField[4];
};
}  // namespace

// [plantlod] (grass.h)
void GroundCover::DrawFarCover(const View& view, const Camera& camera, gfx::Texture sceneDepth) {
  if (!FarCoverWanted() || !sceneDepth || !far_.densityA || !far_.densityB || !far_.cover || !tiles_) return;
  if (!farCoverTried_) {
    farCoverTried_ = 1;
    const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kLightingShaderBlock + kFarCoverShader;
    gfx::PipelineDesc pd;
    pd.name = "ground cover: far cover";
    pd.vertexSource = pd.pixelSource = source;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.blend = gfx::Blend::Multiply;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    std::string error;
    farCover_ = device_->CreatePipeline(pd, &error);
    if (!farCover_) fprintf(stderr, "render: the ground cover's far cover cannot be drawn (%s)\n", error.c_str());
  }
  if (!farCover_) return;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  FarCoverConstants c = {};
  c.cameraRight[0] = right.x, c.cameraRight[1] = right.y, c.cameraRight[2] = right.z, c.cameraRight[3] = std::tan(camera.fovY * 0.5f);
  c.cameraUp[0] = up.x, c.cameraUp[1] = up.y, c.cameraUp[2] = up.z;
  c.cameraUp[3] = static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1));
  c.cameraBack[0] = back.x, c.cameraBack[1] = back.y, c.cameraBack[2] = back.z, c.cameraBack[3] = camera.zNear;
  c.mapInfo[0] = far_.mapWidth;
  c.mapInfo[1] = far_.mapHeight;
  c.mapInfo[2] = far_.waterLevel;
  c.mapInfo[3] = plantDensity * plantCell * plantCell;
  c.cutInfo[0] = tiles_->focal;
  c.cutInfo[1] = plantLod.cutLow * tiles_->cutScale;
  c.cutInfo[2] = std::max(plantLod.cutHigh, plantLod.cutLow) * tiles_->cutScale;
  c.cutInfo[3] = plantCell;
  const float half = 0.5f * area_[2] * area_[3];
  c.bladeInfo[0] = area_[0] + half;
  c.bladeInfo[1] = area_[1] + half;
  c.bladeInfo[2] = half;
  c.bladeInfo[3] = prepared_ ? shown_ : 0.0f;
  c.bladeLook[0] = farBlades;
  c.bladeLook[1] = plantLod.farCover;
  c.bladeLook[2] = plants_.patchScale;
  c.bladeLook[3] = plants_.patchCover;
  // per group: its colour (the mean of its cells' art, linear, a little darker for the shade among its leaves) and how it
  // takes the ground's (the meadow's clumps: the ground's own; the weeds and ferns: their brightness toward it), its
  // card's width, how much of it covers the ground seen from above, its plants a spot
  // (how much of its cards' frames a plant covers seen from above: its picture's own share, the cover's stats, times what
  // its cards show from above - measured 2026-10-21 on a field of ferns at 1440p: 0.6 upright, 1.1 fanned out)
  const float shows = plantsRound ? 1.1f : 0.6f;
  for (int g = 0; g < 6; ++g) {
    const int first = static_cast<int>(plants_.groups[g][0]), n = static_cast<int>(plants_.groups[g][1]);
    float colour[3] = {0, 0, 0}, frame = 0, plantsASpot = 0, covers = 0;
    int coloured = 0;
    for (int j = 0; j < n; ++j) {
      const int cell = static_cast<int>(plants_.order[(first + j) / 4][(first + j) % 4]);
      frame += FrameOf(cell);
      covers += plants_.cover[cell] * (g <= 1 ? 0.6f : shows);  // (the grass's tufts are blades, not cards)
      plantsASpot += 0.5f * (plants_.cells[cell][2][1] + plants_.cells[cell][2][2]);
      if (plants_.tones[cell][0] + plants_.tones[cell][1] + plants_.tones[cell][2] > 0) {
        for (int k = 0; k < 3; ++k) colour[k] += plants_.tones[cell][k];
        ++coloured;
      }
    }
    if (n <= 0) continue;
    for (int k = 0; k < 3; ++k) c.groupLook[g][k] = coloured ? colour[k] / static_cast<float>(coloured) : 0.1f;
    c.groupLook[g][3] = g == 0 ? 1.0f : (g == 2 || g == 5) ? -leavesToGround : 0.0f;
    c.groupSize[g][0] = frame / static_cast<float>(n);
    c.groupSize[g][1] = covers / static_cast<float>(n);
    c.groupSize[g][2] = plantsASpot / static_cast<float>(n);
    c.groupSize[g][3] = farShade;
  }
  c.farMore[0] = bare ? 1.0f : 0.0f;
  c.farMore[1] = plantLod.farPlants;
  c.farField[0] = farArea_[0];
  c.farField[1] = farArea_[1];
  c.farField[2] = 1.0f / std::max(farArea_[2] * farArea_[3], 1e-3f);
  c.farField[3] = farFieldReady_ ? 1.0f : 0.0f;
  device_->SetPipeline(farCover_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(1, sceneDepth);
  device_->SetTexture(2, far_.cover);
  device_->SetTexture(4, far_.densityA);
  device_->SetTexture(5, far_.densityB);
  device_->SetTexture(12, bare ? bare : trampled_);
  device_->SetTexture(21, farFieldReady_ ? farField_ : trampled_);
  device_->Draw(3);
  for (int t : {1, 2, 4, 5, 12, 21}) device_->SetTexture(t, {});
}

// [leftover] The motion of what Draw drew this frame (grass.h): the same squares of blades and plants, by the scene's own
// shaders with MOTION (kCoverShared's CoverMotion at b2), into the motion over the scene's depth.
void GroundCover::DrawMotion(const View& view, gfx::Texture motion, gfx::Texture depth, const Mat4& now, const Mat4& before, float timeBefore) {
  if (!motion || !depth || !drawn_.blades || drawnConstants_[0].empty() || away_ > motionFarthest) return;
  auto pipelineFor = [&](int k) -> gfx::Pipeline {
    if (!motionTried_[k]) {
      motionTried_[k] = true;
      const bool ofPlants = (k & 1) != 0, withBlasts = (k & 2) != 0, reacts = (k & 4) != 0;
      const std::string& scene = ofPlants ? (reacts ? reactingSources_[withBlasts ? 3 : 1] : withBlasts ? blastPlantSource_ : plantSource_)
                                          : (reacts ? reactingSources_[withBlasts ? 2 : 0] : withBlasts ? blastSource_ : bladeSource_);
      const std::string source = "#define MOTION 1\n" + scene;
      gfx::PipelineDesc pd;
      pd.name = ofPlants ? "ground cover: plants, motion" : "ground cover: grass, motion";
      pd.vertexSource = pd.pixelSource = source;
      pd.pixelEntry = "MotionPS";
      pd.cull = gfx::Cull::None;
      pd.colorFormat = gfx::Format::RG16F;
      pd.depthWrite = false;
      pd.depthBias = 16;  // (as the models' motion: the same placement compiled in another shader may land a hair farther)
      std::string error;
      motionPipelines_[k] = device_->CreatePipeline(pd, &error);
      if (!motionPipelines_[k]) fprintf(stderr, "render: the ground cover's motion cannot be drawn (%s)\n", error.c_str());
    }
    return motionPipelines_[k];
  };
  const int kind = (drawn_.blasts ? 2 : 0) + (drawn_.reacting ? 4 : 0);
  const gfx::Pipeline blades = pipelineFor(kind), plantCards = drawn_.plants ? pipelineFor(kind + 1) : gfx::Pipeline{};
  if (!blades && !plantCards) return;
  gfx::PassDesc pass;
  pass.color = motion;
  pass.depth = depth;
  pass.clearColor = pass.clearDepth = false;
  device_->BeginPass(pass);
  const CoverMotionConstants m{now, before, {timeBefore, view.constants.time - timeBefore, 0, 0}};
  device_->SetTexture(0, drawn_.heights);
  device_->SetTexture(1, drawn_.cover);
  device_->SetTexture(2, drawn_.normals);
  device_->SetTexture(3, trampled_);
  device_->SetTexture(4, drawn_.densityA);
  if (drawn_.blasts) device_->SetTexture(8, drawn_.blastField);
  if (drawn_.reacting) {
    device_->SetTexture(9, drawn_.press);
    device_->SetTexture(10, drawn_.burnt);
  }
  device_->SetTexture(11, drawn_.cleared);
  device_->SetTexture(12, drawn_.bare);
  device_->SetTexture(13, drawn_.shelter);
  if (blades) {
    device_->SetPipeline(blades);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, drawnConstants_[0].data(), static_cast<uint32_t>(drawnConstants_[0].size() * sizeof(float)));
    device_->SetConstants(2, &m, sizeof m);
    device_->SetConstants(3, snowConstants_, sizeof snowConstants_);
    device_->Draw(static_cast<uint32_t>(drawnBlades) * 15);
  }
  if (plantCards && !drawnConstants_[1].empty()) {
    device_->SetPipeline(plantCards);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, drawnConstants_[1].data(), static_cast<uint32_t>(drawnConstants_[1].size() * sizeof(float)));
    device_->SetConstants(2, &m, sizeof m);
    device_->SetConstants(3, snowConstants_, sizeof snowConstants_);
    device_->SetTexture(5, drawn_.densityB);
    device_->SetTexture(6, atlas_);
    device_->SetTexture(7, atlasNormals_);
    if (drawn_.far) {  // [plantlod] (the tiles' plants and the models, as Draw drew them)
      PlantConstants p;
      std::memcpy(&p, drawnConstants_[1].data(), std::min(sizeof p, drawnConstants_[1].size() * sizeof(float)));
      DrawFarPlants(&p, (drawn_.blasts ? 1 : 0) + (drawn_.reacting ? 2 : 0), true);
    } else {
      device_->Draw(static_cast<uint32_t>(drawnPlants) * 108);
    }
  }
  for (int slot = 0; slot <= 13; ++slot) device_->SetTexture(slot, {});
  if (drawn_.far)  // [plantlod]
    for (int slot = 14; slot <= 21; ++slot) device_->SetTexture(slot, {});
}

}  // namespace render
