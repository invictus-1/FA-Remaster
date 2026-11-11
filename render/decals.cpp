#include "decals.h"
#include "marks.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/vfs.h"
#include "effect_files.h"
#include "lighting.h"
#include "ocean.h"
#include "ray_scene.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 1024;  // per draw call (the size of the shader's array)

// Each decal is drawn as a box (its far faces, so that it also works with the camera inside it).
// The pixel shader finds the ground's place under the pixel from the scene's depth, turns it into
// the decal's own frame, and gives up where that is outside the footprint.
const char kShaderHead[] = R"hlsl(
struct Instance {
  float4 centre;   // xyz; w: half the box's height (below 0: one the game laid, which fades out on steep ground, [steep])
  float4 shape;    // x, y: half the footprint along its own x and z; z, w: cos and sin of its heading
  float4 options;  // x: how much of it is there (fading), y: a light's age (0..1 of its time; a plain mark: how much of the lying snow covers it),
                   // z: 1 = a light (texture x ramp, added), 2 a normal decal, 3 a mark, 4 a "Water Albedo" decal
                   // where the ground is above the water, 5 the same on the water where the ground is under it;
                   // w: the modern look's brightness of a light; a picture's 1 when it has its own relief (t5)
};
cbuffer DecalCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // x, y: 1 / the map's size; z: 1 = the map has water; w: the water's level
  float4 groundInfo;   // [steep] x, y: 1 / the ground's heights' size (t7); z: their unit (0: none given)
#ifdef DECAL_SNOW_GROUND
  float4 snowWind;     // [decalsnow] xy the way the wind blows (unit length); zw 1 / the map's size
  float4 snowTune;     // [decalsnow] Terrain::snowTune
  float4 snowMore;     // [decalsnow] x 1 = the snow's materials are drawn (the ground's wider, mottled margin)
#endif
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
Texture2D decalTexture : register(t0);
Texture2D rampTexture : register(t2);
Texture2D waterMap : register(t3);
Texture2D waterRamp : register(t4);
Texture2D<float> groundHeights : register(t7);  // [steep] the ground's heights (Terrain::HeightTexture)
Texture2D reliefTexture : register(t5);  // a remastered picture's relief: x in alpha, y in green (BC5 read as the game's DXT5), y up the picture
#ifdef DECAL_SNOW_GROUND
Texture2D snowShelter : register(t6);  // [decalsnow] the snow ground's shelter (Terrain::SnowShelter): g sky, b crowns, a how it lies
#endif
#ifndef RIDE
// [churn] The shell holes under the marks (Decals::holes): deep tracks' field as the ground's shader reads it (terrain.cpp:
// its three levels, the third number a texel the holes' height in world units) and the kinds of ground. (Not for the
// pictures that ride the water: there are no holes under it, and their pipeline has the waves' constants here.)
cbuffer DecalHoles : register(b3) {
  float4 holeLevel[3];  // per level: x, z of its window's first texel; 1 / its texel's size; its texels across
  float4 holeAtlas[3];  // per level: the atlas texel its data begins at; 1 = it wraps round; its texel's size
  float4 holeInfo;      // x, y: 1 / the atlas's size; z: above 0.5 = there is a field to read
};
Texture2D holeField : register(t8);
Texture2D holeKinds : register(t9);  // Terrain::GroundKinds: sand, grass, snow (a layer of it), hard ground
float HoleTap(int level, float2 xz) {
  float4 L = holeLevel[level], A = holeAtlas[level];
  float2 s = xz * L.z;
  float2 c = A.z > 0.5 ? s - L.w * floor(s / L.w) : clamp(s, 0.5, L.w - 0.5);
  return holeField.SampleLevel(samplerLinearClamp, (A.xy + c) * holeInfo.xy, 0).b;
}
float HoleInside(int level, float2 xz) {
  float4 L = holeLevel[level];
  float2 s = xz * L.z - L.xy;
  return L.w > 0 ? min(min(s.x, s.y), min(L.w - s.x, L.w - s.y)) : -1;
}
// How much of a flat mark the game laid is left at a place: all of it on untouched ground, on a hole's lip and in a
// scuff; none where the ground shows a hole a fifth of a world unit deep or more (by the finest level that holds the
// place; as deep as the ground there holds it: a snow layer all of it, sand 0.7, dirt 0.6 and more as it gets wet,
// grass 0.55, rock none, and all of it under lying snow, as the ground's shader takes it).
float MarkOverHoles(float2 xz, float2 perMap) {
  [branch] if (holeInfo.z < 0.5) return 1;
  float w0 = saturate((HoleInside(0, xz) - 8) / 96), w1 = saturate((HoleInside(1, xz) - 8) / 96);
  float hole = HoleTap(2, xz);
  [branch] if (w1 > 0) hole = lerp(hole, HoleTap(1, xz), w1);
  [branch] if (w0 > 0) hole = lerp(hole, HoleTap(0, xz), w0);
  [branch] if (hole > -0.03) return 1;
  // (the map's size from the field's own level over the map, where the camera's block gives none: a map without water
  // has mapInfo.xy 0, and the kinds were read at one corner of the map there)
  float2 per = perMap.x > 0 ? perMap : holeLevel[2].z / max(holeLevel[2].w, 1);
  float4 k = holeKinds.SampleLevel(samplerLinearClamp, xz * per, 0);
  float rest = saturate(1 - k.x - k.y - k.z - k.w);
  float soft = max(k.z + k.x * 0.7 + k.y * 0.55 + rest * lerp(0.6, 0.88, saturate(weather.x)), saturate(weather.y * 2));
  return 1 - smoothstep(0.03, 0.2, -hole * soft);
}
#endif
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

struct VOut {
  float4 position : SV_Position;
  nointerpolation float4 centre : TEXCOORD0;
  nointerpolation float4 shape : TEXCOORD1;
  nointerpolation float4 options : TEXCOORD2;
};

VOut VS(float3 corner : POSITION, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
  float2 flat = corner.xz * n.shape.xy;
  float3 world = n.centre.xyz + float3(flat.x * n.shape.z + flat.y * n.shape.w, corner.y * abs(n.centre.w), -flat.x * n.shape.w + flat.y * n.shape.z);
  VOut o;
  o.position = mul(viewProj, float4(world, 1));
  o.centre = n.centre;
  o.shape = n.shape;
  o.options = n.options;
  return o;
}

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / depth;
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

#if defined(SETTLED_LIGHT) || defined(SETTLED_PLACE)
// [settled] (the settled scars' picture lit over the ground, Decals::settleScars: one triangle over the whole picture)
VOut VSSettled(uint id : SV_VertexID) {
  VOut o;
  o.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1);
  o.centre = 0;
  o.shape = 0;
  o.options = 0;
  return o;
}
#endif

)hlsl";

}  // namespace

// ([padsnow] the decals' shader is the head above, this, and the tail below, in that order: the pads' takes this too)
const char kMarkSnowLying[] = R"hlsl(
// How much snow lies on a mark's place (0..1), `fallen` the snow over it (the weather's, times the share on the mark),
// `ground` the ground's own slope there: as ModernLight lays it on the ground, level ground first, in patches. [decalsnow]
// With the snow ground (DECAL_SNOW_GROUND) as the ground's snow pipeline lays it: deeper in the lee and hollows, thinner
// and patchier under trees, on steep, windward and scoured ground (terrain.cpp's SnowAround, its snowLie and snowLess: the
// same numbers), and with the snow's materials over the ground's wider, mottled margin (lighting.h; the break-up by the
// layers' heights close by is the ground's alone).
float MarkSnowLying(float3 world, float3 ground, float fallen) {
  float patch = CloudNoise(world * 0.9) * 0.55 + CloudNoise(float3(world.xz * 0.21, 5.3)) * 0.45;
  float takes = patch * 0.75 + saturate((ground.y - 0.35) * 1.6) * 0.25;
#ifdef DECAL_SNOW_GROUND
  float4 shelter = snowShelter.SampleLevel(samplerLinearClamp, saturate(world.xz * snowWind.zw), 0);
  float crowns = shelter.b, lay = shelter.a * 2 - 1;
  float slope = length(ground.xz);
  float lee = slope > 1e-3 ? dot(ground.xz / slope, snowWind.xy) * saturate(slope * 5) : 0;  // (1: facing away from the wind)
  fallen *= (1 + 0.4 * lay * snowTune.w + 0.15 * lee) * (1 - 0.3 * crowns * snowTune.y);
  takes -= 0.08 * crowns * snowTune.y + 0.25 * saturate((0.85 - ground.y) * 4) + 0.1 * saturate(-lee) + 0.3 * saturate(-lay) * snowTune.w;
  float bar = 1.02 - fallen * 1.15;
  [branch] if (snowMore.x > 0.5) {
    float lying = smoothstep(bar - 0.15, bar + 0.35, takes) * smoothstep(0.25, 0.5, ground.y);
    float mottle = CloudNoise(float3(world.xz * 0.45, 1.3)) * 0.6 + CloudNoise(float3(world.xz * 1.3, 4.7)) * 0.4;
    return saturate(lying + (mottle - 0.5) * 3.2 * lying * (1 - lying));
  }
#else
  float bar = 1.02 - fallen * 1.15;
#endif
  return smoothstep(bar, bar + 0.2, takes) * smoothstep(0.25, 0.5, ground.y);
}
// [snowfix] How deep a normal decal lies under the snow (0..1; options.y: -1 - the share of the lying snow on it): in the modern
// look, as the snow fills it. Its relief is then read from a softer level of its picture and laid back toward the ground's
// own slope (MarkSnowLying), so that the map's craters and trenches read through deep snow as soft hollows, not sharp rings.
float NormalDecalBuried(float4 options) {
  return look > 0.5 && weather.y > 0 ? saturate(weather.y * (-1 - options.y) * 1.5 - 0.2) : 0;
}
)hlsl";

namespace {

const char kShaderTail[] = R"hlsl(
#ifdef MODERN_RELIEF
// [scars] A normal decal with Decals::modernRelief (the modern look): the ground's own slope (from how its place changes
// from pixel to pixel) tilted by the decal's, and the ground multiplied by how much more or less of the modern light the
// tilted ground takes than the flat: the sun through its and the clouds' shadows, the sky's light from all sides and the
// map's fill (ModernLight's diffuse part). With the sun down the sky's share keeps the change near 1; flat parts change
// nothing. Under lying snow (options.y: -1 - the share of it that lies on the mark, as a picture's) the relief softens
// as the snow fills it. options.w: 0 the game's (how much applies in red), 1 the visuals track's effects' own (two
// channels, y up the picture), 2 a plain normal map (x in alpha, y in green; no share in red): both at full strength.
float4 NormalDecalModern(float4 c, float4 options, float4 shape, float3 world) {
  float3 ground = normalize(cross(ddy(world), ddx(world)));
  if (ground.y < 0) ground = -ground;
  float2 tilt = c.ag * 2 - 1;
  float3 inDecal = float3(tilt.x, sqrt(saturate(1 - dot(tilt, tilt))), tilt.y);
  float strength = c.r;
  if (options.w > 0.5) {
    if (options.w < 1.5) inDecal.z = -tilt.y;
    strength = 1;
  }
  float3 turned = float3(inDecal.x * shape.z + inDecal.z * shape.w, inDecal.y, -inDecal.x * shape.w + inDecal.z * shape.z);
  float3 tilted = normalize(ground + turned - float3(0, 1, 0));
  // (where the snow lies on it - thick, it fills the relief but for a hint of it)
  // ([snowfix] filled all but a tenth, its relief read softer: PS)
  float buried = NormalDecalBuried(options);
  [branch] if (buried > 0) tilted = normalize(lerp(tilted, ground, MarkSnowLying(world, ground, weather.y * (-1 - options.y)) * buried * 0.9));
  float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x * SunShadow(world, ground) * CloudShadow(world));
  float3 fill = ToLinear(shadowFill) * 0.5;
  float3 skyFlat, skyTilted;
  [branch] if (skyZenith.w > 0.5) {  // the computed sky (sky.h)
    skyFlat = SkyAmbient(ground);
    skyTilted = SkyAmbient(tilted);
  } else {
    skyFlat = ToLinear(environmentSky.SampleLevel(samplerLinear, ground, 8).rgb);
    skyTilted = ToLinear(environmentSky.SampleLevel(samplerLinear, tilted, 8).rgb);
  }
  float3 before = sun * saturate(dot(sunDirection, ground)) + (skyFlat + fill) * modern.y;
  float3 after = sun * saturate(dot(sunDirection, tilted)) + (skyTilted + fill) * modern.y;
  float3 change = clamp(after / max(before, 1e-5), 0.35, 2.5);
  return float4(lerp(float3(1, 1, 1), change, saturate(strength * options.x)), 1);
}
#endif

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  if (depth <= 0) discard;  // nothing drawn here
  float3 world = WorldAt(i.position.xy, depth);
#ifdef SETTLED_PLACE
  // [settled] (Decals::settleScars) What the place does to a picture the game laid there, the same for every settled scar
  // over this pixel: how much of it the slope leaves ([steep] below: the same lines) and how much the shell holes
  // ([churn], MarkOverHoles). Worked out once a pixel for the frame, where each scar's box read the ground's heights and
  // the holes' field anew (some ten reads a layer).
  {
    float place = 1;
    [branch] if (look > 0.5) {
      [branch] if (groundInfo.z > 0) {
        float2 at = (world.xz + 0.5) * groundInfo.xy;
        float2 rise = float2(groundHeights.SampleLevel(samplerLinearClamp, at + float2(groundInfo.x, 0), 0)
                             - groundHeights.SampleLevel(samplerLinearClamp, at - float2(groundInfo.x, 0), 0),
                             groundHeights.SampleLevel(samplerLinearClamp, at + float2(0, groundInfo.y), 0)
                             - groundHeights.SampleLevel(samplerLinearClamp, at - float2(0, groundInfo.y), 0)) * (groundInfo.z * 0.5);
        place = smoothstep(0.45, 0.7, rsqrt(1 + dot(rise, rise)));
      }
      [branch] if (holeInfo.z > 0.5) place *= MarkOverHoles(world.xz, mapInfo.xy);
    }
    return float4(place, 0, 0, 1);
  }
#endif
#ifdef SETTLED_LIGHT
  // [settled] (Decals::settleScars) The settled scars' picture lit over the ground, in place of each scar lit by itself:
  // at t0 the scars laid over each other for this frame, pixel for pixel of the scene (their colours as linear light
  // times how much of each is there, and how much of them all is there: SETTLED_LAY 1 below), at t5 their relief's lean in
  // the world (x, z, as 0..1) and their share of the lying snow, laid over each other the same way (SETTLED_LAY 2). From
  // here on it is one mark: lit as the ground it lies on, tilted by its relief, blended by how much is there.
  float3 N = normalize(cross(ddy(world), ddx(world)));
  if (N.y < 0) N = -N;
  float4 c = decalTexture.Load(int3(i.position.xy, 0));
  if (c.a <= 0.003) discard;
  c.rgb /= c.a;
  float4 more = reliefTexture.Load(int3(i.position.xy, 0));
  more.xyz /= max(more.a, 0.003);
  float2 lean = more.xy * 2 - 1;
  N = normalize(N + float3(lean.x, sqrt(saturate(1 - dot(lean, lean))), lean.y) - float3(0, 1, 0));
  i.options.y = more.z;
#else
  // [steep] a picture the game laid (a scorch, a scar, a print: centre.w below 0) is thrown straight down, so on a steep face it
  // was drawn out down the face and cut off with a hard line where its box ends. In the modern look it now fades out as
  // the ground steepens (from 45 degrees, gone at 63; nothing walks there) and toward its box's top and bottom. The
  // map's own pictures stay as they are: its cliffs' faces are pictures thrown down on purpose. (The slope is read from
  // the ground's heights, which run smoothly; the drawn ground's own, from the depth, is one value a triangle, and the
  // fade then ended on a triangle's edge, a straight line.)
  float steepFade = 1;
#ifdef SETTLED_LAY
  // [settled] (a settled scar laid into the scars' picture: what the place itself does to every one of them - the slope,
  // the holes - is read from the picture made of it once for this frame, SETTLED_PLACE above, at t2; the scar's own part,
  // how far the ground is above or below it, here)
  float holeFade = 1;
  if (i.centre.w < 0 && look > 0.5)
    steepFade = rampTexture.Load(int3(i.position.xy, 0)).r * (groundInfo.z > 0 ? smoothstep(1, 0.7, abs(world.y - i.centre.y) / -i.centre.w) : 1);
#else
  [branch] if (i.centre.w < 0 && look > 0.5 && groundInfo.z > 0) {
    float2 at = (world.xz + 0.5) * groundInfo.xy;
    float2 rise = float2(groundHeights.SampleLevel(samplerLinearClamp, at + float2(groundInfo.x, 0), 0)
                         - groundHeights.SampleLevel(samplerLinearClamp, at - float2(groundInfo.x, 0), 0),
                         groundHeights.SampleLevel(samplerLinearClamp, at + float2(0, groundInfo.y), 0)
                         - groundHeights.SampleLevel(samplerLinearClamp, at - float2(0, groundInfo.y), 0)) * (groundInfo.z * 0.5);
    steepFade = smoothstep(0.45, 0.7, rsqrt(1 + dot(rise, rise))) * smoothstep(1, 0.7, abs(world.y - i.centre.y) / -i.centre.w);
  }
  // [churn] a picture the game laid gives way where the ground shows a shell hole (MarkOverHoles; its glow, the light of
  // hot earth, does not)
  float holeFade = 1;
#ifndef RIDE
  [branch] if (i.centre.w < 0 && look > 0.5 && holeInfo.z > 0.5) holeFade = MarkOverHoles(world.xz, mapInfo.xy);
#endif
#endif
  i.centre.w = abs(i.centre.w);
  [branch] if (i.options.z > 5.5) {
    // hot metal (hot_metal.h): a fresh scorch's or crater's middle glowing on the ground, its light added, through the
    // air as the ground's own (options: x its age, y how long it glows, w its own number)
    float3 d = world - i.centre.xyz;
    if (abs(d.y) > i.centre.w) discard;
    float2 local = float2(d.x * i.shape.z - d.z * i.shape.w, d.x * i.shape.w + d.z * i.shape.z) / i.shape.xy;
    if (any(abs(local) > 1)) discard;
    float4 c = decalTexture.Sample(samplerAniso, local * 0.5 + 0.5);
    float3 glow = HotGroundGlow(local, c.a, i.options.x, i.options.y, i.options.w, i.options.z > 6.5) *  // ([softscorch] light 7)
                  (kHotMetalStrength * pow(falling.w > 0 ? falling.w : 1, 0.6));
    glow *= steepFade;  // [steep]
    if (max(glow.r, glow.g) <= 0) discard;  // (nothing glows there: nothing written)
    return float4(ThroughAirTo(glow, world) - ThroughAirTo(0, world), 1);
  }
  // a "Water Albedo" decal: the original raises the ground under it to the water's surface
  // (TDecalsWaterAlbedo). Before the water: only where the ground is above it; after the water: only
  // where the ground is under it, on the surface the line of sight meets.
  if (i.options.z > 3.5) {
    bool under = mapInfo.z > 0.5 && world.y < mapInfo.w;
    if (under != (i.options.z > 4.5)) discard;
    if (under) {
      float3 ray = world - cameraPosition;
#if RIDE
      world = OceanRideRest(cameraPosition, normalize(ray));  // [whitecaps] (on the lifted waves: where it rests on the still water)
#else
      world = cameraPosition + ray * ((mapInfo.w - cameraPosition.y) / min(ray.y, -1e-4));
#endif
    }
  }
  float3 d = world - i.centre.xyz;
  if (abs(d.y) > i.centre.w) discard;
  // into the decal's own frame (the heading turned back)
  float2 local = float2(d.x * i.shape.z - d.z * i.shape.w, d.x * i.shape.w + d.z * i.shape.z) / i.shape.xy;
  if (any(abs(local) > 1)) discard;
  float4 c = decalTexture.Sample(samplerAniso, local * 0.5 + 0.5);
  i.options.x *= steepFade * holeFade;  // [steep] (1 for the water's, a mark's and the map's own; [churn] the holes')
  if (i.options.z > 3.5) {  // a "Water Albedo" decal: unlit, in its own colours (the original's times the light multiplier)
    c.a *= i.options.x;
    if (c.a <= 0.003) discard;
    c.rgb = look > 0.5 ? ToLinear(c.rgb) * (1.6 * (falling.w > 0 ? falling.w : 1)) : c.rgb * lightMultiplier;
    return c;
  }
  if (i.options.z > 2.5) {  // a mark: as it is (the modern look's picture is linear light, mapped back at the end)
    if (look > 0.5) c.rgb = ToLinear(c.rgb) * (1.6 * (falling.w > 0 ? falling.w : 1));  // (less to an eye opened for the night)
    return c;
  }
  if (i.options.z > 1.5) {
    // [snowfix] (under lying snow: its relief from a softer level of its picture, NormalDecalBuried)
    float buried = NormalDecalBuried(i.options);
    // (with the place's steps worked out here, outside the branch, and made that many times larger)
    float2 at = local * 0.5 + 0.5, stepX = ddx(at), stepY = ddy(at);
    [branch] if (buried > 0) c = decalTexture.SampleGrad(samplerAniso, at, stepX * exp2(3.5 * buried), stepY * exp2(3.5 * buried));
#ifdef MODERN_RELIEF
    return NormalDecalModern(c, i.options, i.shape, world);  // [scars] (Decals::modernRelief)
#else
    // a normal decal: the ground's own slope (from how its place changes from pixel to pixel)
    // tilted by the decal's, and the picture multiplied by how much more or less light that takes
    float3 ground = normalize(cross(ddy(world), ddx(world)));
    if (ground.y < 0) ground = -ground;
    float2 tilt = c.ag * 2 - 1;
    float3 inDecal = float3(tilt.x, sqrt(saturate(1 - dot(tilt, tilt))), tilt.y);
    float strength = c.r;
    if (i.options.w > 0.5) {  // the visuals track's own (two channels, x and y, y up the picture): at full strength
      inDecal.z = -tilt.y;
      strength = 1;
    }
    float3 turned = float3(inDecal.x * i.shape.z + inDecal.z * i.shape.w, inDecal.y, -inDecal.x * i.shape.w + inDecal.z * i.shape.z);
    float3 tilted = normalize(ground + turned - float3(0, 1, 0));
    // [snowfix] (in the modern look under lying snow: filled all but a tenth, as the modern relief's)
    [branch] if (buried > 0) tilted = normalize(lerp(tilted, ground, MarkSnowLying(world, ground, weather.y * (-1 - i.options.y)) * buried * 0.9));
    float shade = SunShadow(world, ground);
    float3 before = OriginalLight(dot(sunDirection, ground), shade), after = OriginalLight(dot(sunDirection, tilted), shade);
    float3 change = clamp(after / max(before, 0.03), 0.35, 2.5);
    return float4(lerp(float3(1, 1, 1), change, saturate(strength * i.options.x)), 1);
#endif
  }
  if (i.options.z > 0.5) {  // a light: its glow times its ramp along its time, added
    if (look > 0.5) discard;  // the modern look has real lights for this (lights.h)
    c *= rampTexture.Sample(samplerLinearClamp, float2(i.options.y, 0.5));
    return c;
  }
  c.a *= i.options.x;
  if (c.a <= 0.003) discard;
  // lit as the ground it lies on: the ground's slope from how its place changes from pixel to pixel
  float3 N = normalize(cross(ddy(world), ddx(world)));
  if (N.y < 0) N = -N;
#ifdef DECAL_SNOW_GROUND
  const float3 groundSlope = N;  // [decalsnow]
#endif
#ifdef SETTLED_LAY
  // [settled] (Decals::settleScars) A settled scar laid unlit into the scars' picture, over what is laid there (blended as
  // premultiplied): all of a picture's own that the lit way above and below takes for it - how much of it is there
  // (weathered, by its distance, the slope and the holes: options.x; with the snow ground, less the snow lying over it) -
  // and then 1: its colour as the light takes it (linear in the modern look) times that; or 2: its relief's lean turned
  // into the world (x, z; none: level), softened under the snow as below, and its share of the lying snow, times that.
  {
    float there = c.a;
#ifdef DECAL_SNOW_GROUND
    [branch] if (look > 0.5 && weather.y > 0 && i.options.y > 0) {
      there *= 1 - MarkSnowLying(world, groundSlope, weather.y * i.options.y);
      if (there <= 0.003) discard;
    }
#endif
#if SETTLED_LAY == 1
    return float4((look > 0.5 ? ToLinear(c.rgb) : c.rgb) * there, there);
#else
    float2 lean = 0;
    if (i.options.w > 0.5) {
      float2 t = reliefTexture.Sample(samplerAniso, local * 0.5 + 0.5).ag * 2 - 1;
#ifndef DECAL_SNOW_GROUND
      [branch] if (look > 0.5 && weather.y > 0 && i.options.y > 0) {
        float fallen = weather.y * i.options.y;
        t *= 1 - 0.9 * MarkSnowLying(world, N, fallen) * saturate(fallen * 1.5 - 0.2);
      }
#endif
      lean = float2(t.x * i.shape.z - t.y * i.shape.w, -t.x * i.shape.w - t.y * i.shape.z);  // (as `turned` below: its x and z)
    }
    return float4((lean * 0.5 + 0.5) * there, i.options.y * there, there);
#endif
  }
#endif
  if (i.options.w > 0.5) {  // a remastered picture's own relief, tilting the ground's slope as a normal decal does
    float2 t = reliefTexture.Sample(samplerAniso, local * 0.5 + 0.5).ag * 2 - 1;
#ifndef DECAL_SNOW_GROUND
    // [snowfix] (in the modern look the snow over a picture is laid by ModernLight, which keeps half of any relief: its own
    // relief filled as a normal decal's is, by as much as lies on it; with the snow ground the picture itself thins out instead)
    [branch] if (look > 0.5 && weather.y > 0 && i.options.y > 0) {
      float fallen = weather.y * i.options.y;
      t *= 1 - 0.9 * MarkSnowLying(world, N, fallen) * saturate(fallen * 1.5 - 0.2);
    }
#endif
    float3 inDecal = float3(t.x, sqrt(saturate(1 - dot(t, t))), -t.y);  // (its y up the picture: toward the decal's -z)
    float3 turned = float3(inDecal.x * i.shape.z + inDecal.z * i.shape.w, inDecal.y, -inDecal.x * i.shape.w + inDecal.z * i.shape.z);
    N = normalize(N + turned - float3(0, 1, 0));
  }
#endif
  float shadow = SunShadow(world, N);
  // under water: tinted toward the water ramp's colour by depth, as the ground is (terrain.cpp)
  float4 tint = 0;
  if (mapInfo.z > 0.5) tint = waterRamp.Sample(samplerLinearClamp, float2(waterMap.Sample(samplerLinearClamp, world.xz * mapInfo.xy).g, 0.5));
  if (look > 0.5) {
#ifdef DECAL_SNOW_GROUND
    // [decalsnow] (Decals::snowGround) the snow that lies over the mark is the ground's own: the mark thins out by as much as
    // lies there, and is lit bare
#ifndef SETTLED_LIGHT
    [branch] if (weather.y > 0 && i.options.y > 0) {
      c.a *= 1 - MarkSnowLying(world, groundSlope, weather.y * i.options.y);
      if (c.a <= 0.003) discard;
    }
#endif
    markSnow = 0;
#else
    markSnow = i.options.y;  // (a mark laid on the snow shows on it until new snow covers it)
#endif
#ifdef SETTLED_LIGHT
    float3 lit = ModernLight(c.rgb, 0, 0.9, 1, 0, N, world, shadow);  // [settled] (laid as linear light already)
#else
    float3 lit = ModernLight(ToLinear(c.rgb), 0, 0.9, 1, 0, N, world, shadow);
#endif
#ifdef MSAA
    return float4(lit, c.a);  // (the modern water colours what is under it itself)
#endif
    float full = max(lightMultiplier * modern.x * (skyZenith.w > 0.5 ? skyGlow.w * lerp(1, 0.45, skyHorizon.w) : 1), 0.001);
    float3 screen = lerp(pow(max(lit / full, 0), 1 / 2.2), tint.rgb, tint.a);
    return float4(ToLinear(screen) * full, c.a);
  }
  return float4(lerp(c.rgb * OriginalLight(dot(sunDirection, N), shadow), tint.rgb, tint.a), c.a);
}
)hlsl";

// The decals' shader source ([scars] `relief`: with the normal decals lit as Decals::modernRelief says; [decalsnow] `snow`:
// with the snow ground's, Decals::snowGround).
// ([settled] `settled`: 1, 2 a settled scar laid into the scars' picture, its colour or its lean; 3 that picture lit; 4
// what the place does to them all: Decals::settleScars)
std::string DecalSource(bool relief, bool snow = false, bool ride = false, int settled = 0) {
  // (how much of the lying snow a mark takes is the mark's own: see `snowLying`)
  // ([whitecaps] ride: the pictures on the water laid on the lifted waves, ocean.h's kOceanRideShaderBlock)
  return std::string(settled == 4 ? "#define SETTLED_PLACE 1\n" : "") + std::string(settled == 1 ? "#define SETTLED_LAY 1\n" : settled == 2 ? "#define SETTLED_LAY 2\n" : settled == 3 ? "#define SETTLED_LIGHT 1\n" : "") +
         std::string(snow ? "#define DECAL_SNOW_GROUND 1\n" : "") + (relief ? "#define MODERN_RELIEF 1\n" : "") + (gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "static float markSnow = 1;\n#define SURFACE_SNOW markSnow\n" +
         (ride ? "#define RIDE 1\n" : "") + kViewShaderBlock + (ride ? kOceanRideShaderBlock : "") + RayShaders() + kLightingShaderBlock + HotMetal::ShaderBlock() +
         kShaderHead + kMarkSnowLying + kShaderTail;  // (RayShaders: the small lights' shadows by rays; hot metal: hot_metal.h)
}

// [scars] Whether a DDS picture's blue channel holds anything: the game's normal decals keep it empty (x in alpha, y in
// green, how much applies in red); a plain normal map does not. Read from its first level's colour blocks (DXT1, DXT3,
// DXT5: the two end colours of each, 5:6:5) or its pixels (32 bits, with masks); anything else: no.
bool BlueInDds(const std::string& file) {
  if (file.size() < 128 || file.compare(0, 4, "DDS ") != 0) return false;
  auto u32 = [&](size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(file[at])) | static_cast<uint32_t>(static_cast<unsigned char>(file[at + 1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(file[at + 2])) << 16 | static_cast<uint32_t>(static_cast<unsigned char>(file[at + 3])) << 24;
  };
  const uint32_t height = u32(12), width = u32(16), flags = u32(80), fourCC = u32(84), bits = u32(88), blueMask = u32(100);
  const size_t blocks = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4);
  int stride = 0;
  if ((flags & 4) && fourCC == 0x31545844u) stride = 8;                                               // DXT1
  else if ((flags & 4) && (fourCC == 0x33545844u || fourCC == 0x35545844u)) stride = 16;            // DXT3, DXT5
  if (stride) {
    for (size_t b = 0; b < blocks && 128 + (b + 1) * stride <= file.size(); ++b) {
      const size_t at = 128 + b * stride + (stride - 8);
      const uint32_t ends = u32(at);
      if ((ends & 0x1f) > 1 || ((ends >> 16) & 0x1f) > 1) return true;  // (blue of either end colour, past rounding)
    }
    return false;
  }
  if ((flags & 0x40) && bits == 32 && blueMask) {
    int shift = 0;
    while (!((blueMask >> shift) & 1)) ++shift;
    for (size_t p = 0; p < static_cast<size_t>(width) * height && 128 + (p + 1) * 4 <= file.size(); ++p)
      if (((u32(128 + p * 4) & blueMask) >> shift) > 2) return true;
  }
  return false;
}

}  // namespace

void Decals::SetModernRelief(bool on) {
  modernRelief = on;
  if (!on || reliefTried_) return;
  reliefTried_ = true;
  std::string error;
  const std::string source = DecalSource(true);  // (kept while the pipeline is made: the description points into it)
  gfx::PipelineDesc pd;
  pd.name = "normal decals, modern relief";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Multiply;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  normalsModern_ = device_->CreatePipeline(pd, &error);
  if (!normalsModern_) fprintf(stderr, "render: the marks' relief cannot be lit as the ground (%s)\n", error.c_str());
}

// [settled] (each frame: its shaders made the first time it is on with the battle scars, not in the middle of a game when
// the first scar settles)
void Decals::SetSettleScars(bool on) {
  // (its shaders cannot be made: off, the scars as before)
  settleScars = on && (!battleScars || SettledPipelines(false));
}

void Decals::PrepareSnowGround() {
  // (the pictures' pipeline and the relief's, each with the snow ground's: DECAL_SNOW_GROUND)
  auto make = [&](bool relief, gfx::Pipeline* out, const char* name) {
    std::string error;
    const std::string source = DecalSource(relief, true);  // (kept while the pipeline is made: the description points into it)
    gfx::PipelineDesc pd;
    pd.name = name;
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
    pd.stride = 12;
    pd.cull = gfx::Cull::Front;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.blend = relief ? gfx::Blend::Multiply : gfx::Blend::Alpha;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    *out = device_->CreatePipeline(pd, &error);
    if (!*out) fprintf(stderr, "render: the marks cannot lay the snow ground's snow (%s)\n", error.c_str());
  };
  if (!snowTried_) {
    snowTried_ = true;
    make(false, &picturesSnow_, "decals, snow ground");
  }
  if (modernRelief && !reliefSnowTried_) {
    reliefSnowTried_ = true;
    make(true, &normalsModernSnow_, "normal decals, modern relief, snow ground");
  }
  if (settleScars && battleScars) SettledPipelines(true);  // [settled]
}

bool Decals::Plain(uint32_t texture) {
  auto known = plainOf_.find(texture);
  if (known != plainOf_.end()) return known->second;
  bool plain = false;
  auto file = normalFiles_.find(texture);
  if (file != normalFiles_.end())
    if (auto bytes = vfs_->ReadFile(file->second)) plain = BlueInDds(*bytes);
  plainOf_[texture] = plain;
  return plain;
}

bool Decals::Init(std::string* error) {
  const std::string source = DecalSource(false);
  gfx::PipelineDesc pd;
  pd.name = "decals";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;  // the far faces
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "lights on the ground";
  pd.blend = gfx::Blend::Additive;
  pd.alphaWrite = true;
  lights_ = device_->CreatePipeline(pd, error);
  if (!lights_) return false;
  pd.name = "normal decals";
  pd.blend = gfx::Blend::Multiply;
  pd.alphaWrite = false;
  normals_ = device_->CreatePipeline(pd, error);
  if (!normals_) return false;
  pd.name = "hot marks on the ground";  // (hot metal: a fresh scorch's glow, added; the picture's alpha left alone)
  pd.blend = gfx::Blend::Additive;
  hot_ = device_->CreatePipeline(pd, error);
  if (!hot_) return false;
  // a cube from -1 to 1, its faces wound counter-clockwise seen from outside
  std::vector<float> v;
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int a = 0; a < 3; ++a)
    for (int sign = -1; sign <= 1; sign += 2) {
      int iu = (a + 1) % 3, iw = (a + 2) % 3;
      if (sign < 0) std::swap(iu, iw);
      const Vec3 n = axes[a] * static_cast<float>(sign), u = axes[iu], w = axes[iw];
      const Vec3 p0 = n - u - w, p1 = n + u - w, p2 = n + u + w, p3 = n - u + w;
      for (Vec3 p : {p0, p1, p2, p0, p2, p3}) {
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
      }
    }
  gfx::BufferDesc bd;
  bd.stride = 12;
  bd.size = static_cast<uint32_t>(v.size() * sizeof(float));
  bd.data = v.data();
  cube_ = device_->CreateBuffer(bd);
  batch_.resize(kMaxInstances);
  return static_cast<bool>(cube_);
}

gfx::Texture Decals::Resolve(const std::string& texture, gfx::Texture* relief, bool* ours, std::string* found) {
  if (relief) *relief = {};
  if (ours) *ours = false;
  if (texture.empty()) return {};
  // (a path begins with a slash; a script's short name may have a folder in it: "Tarmacs/Tar12x_01_albedo")
  std::string path;
  if (texture[0] == '/') {
    if (vfs_->Exists(texture)) path = texture;
  } else {
    for (const char* folder : {"/env/common/decals/", "/env/common/splats/"})
      if (path.empty() && vfs_->Exists(std::string(folder) + texture + ".dds")) path = std::string(folder) + texture + ".dds";
  }
  if (path.empty()) return {};
  if (found) *found = path;  // [scars] (the game's file, before the visuals track's takes its place)
  // the visuals track's picture of it, and the relief beside it ("x_albedo.dds", "x_normals.dds") where the game has
  // no normal decal of that name (where it has, its scripts lay that one as a decal of its own: the crater's
  // "crater01_normals" with "crater01_albedo"; it then reads the visuals track's, and the relief is not given twice)
  if (remastered) {
    const std::string mine = EffectFile(*vfs_, path, true);
    if (mine != path) {
      if (ours) *ours = true;
      std::string low = mine;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const std::string tail = "_albedo.dds";
      if (relief && low.size() > tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0) {
        const std::string normals = mine.substr(0, mine.size() - tail.size()) + "_normals.dds";
        const std::string game = path.substr(0, path.size() - tail.size()) + "_normals.dds";
        if (vfs_->Exists(normals) && !vfs_->Exists(game)) *relief = textures_->Get(normals);
      }
      path = mine;
    }
  }
  return textures_->Get(path);
}

bool Decals::IsDepositMark(const std::string& texture, bool* hydrocarbon) {
  std::string name = texture;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const bool mass = name.find("mass_marker") != std::string::npos, hydro = name.find("hydrocarbon_marker") != std::string::npos;
  if (hydrocarbon) *hydrocarbon = hydro;
  return mass || hydro;
}

bool Decals::IsScar(const std::string& texture) {
  std::string name = texture;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name.find("scorch") != std::string::npos || name.find("crater") != std::string::npos;
}

bool Decals::Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  {  // a resource deposit's mark: remembered, for its icon from far up
    bool hydrocarbon = false;
    if (IsDepositMark(texture, &hydrocarbon) && seconds <= 0 && deposits_.size() < 4096) deposits_.push_back({centre, hydrocarbon, cutoff});
  }
  gfx::Texture relief;
  bool ours = false;
  const gfx::Texture t = Resolve(texture, &relief, &ours, &lastFile_);
  if (!t || sizeX == 0 || sizeZ == 0) return false;  // (a negative size mirrors it)
  if (groundHeight) {  // (given far from the ground, beyond what its box reaches: on the ground)
    const float ground = groundHeight(centre.x, centre.z);
    if (std::fabs(centre.y - ground) > std::max(std::fabs(sizeX), std::fabs(sizeZ)) * 0.5f) centre.y = ground;
  }
  decals_.push_back({t, {}, centre, sizeX * 0.5f, sizeZ * 0.5f, heading, cutoff, born, seconds});
  decals_.back().relief = relief;
  decals_.back().ours = ours;
  if (battleScars && seconds > 0 && IsScar(texture)) {  // a battle scar: kept; past the most, the oldest fades out
    decals_.back().scar = true;
    decals_.back().name = ScarName(texture);  // [settled]
    Mark("battle scars kept", static_cast<uint64_t>(scars_ + 1), static_cast<uint64_t>(mostScars));
    if (++scars_ > mostScars) {
      Decal* oldest = nullptr;
      if (settleScars) {
        // [settled] (the oldest whose glow is out settles as it is, and goes on weathering there. All of them still
        // glowing - the most passed by scars of the last minute alone: none goes; they settle as their glow goes out,
        // LaySettled, and the list is over the most for that long)
        for (Decal& d : decals_)
          if (d.scar && d.scarGone <= 0 && &d != &decals_.back() && !Glowing(d, born) && (!oldest || d.born < oldest->born)) oldest = &d;
        if (oldest) {
          Decal moved = std::move(*oldest);
          decals_.erase(decals_.begin() + (oldest - decals_.data()));
          Settle(std::move(moved));
          --scars_;
          oldest = nullptr;
        }
      } else {
        for (Decal& d : decals_)
          if (d.scar && d.scarGone <= 0 && (!oldest || d.born < oldest->born)) oldest = &d;
      }
      if (oldest) oldest->scarGone = born + 10;
    }
  }
  if (hotMetal && IsScar(texture)) decals_.back().burn = true;  // (hot metal: its middle glows while fresh)
  decals_.back().snowAtBirth = settled_ ? snowFallen_ : snowFallen_ - 1;
  decals_.back().id = lastId_ = nextId_++;
  decals_.back().tarmac = TarmacOf(texture);  // [tarmac] (a pad's: its material looked up when remastered pads are drawn)
  decals_.back().original = addingOriginal;
  decals_.back().mapOwn = !settled_ && !addingGames;
  return true;
}

bool Decals::AddWater(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  if (!Add(texture, centre, sizeX, sizeZ, heading, cutoff, born, seconds)) return false;
  Decal& d = decals_.back();
  d.water = true;
  d.centre.y = std::max(d.centre.y, waterLevel);  // (its box reaches the surface)
  ++waterDecals_;
  return true;
}

// [restart] (decals.h)
void Decals::ForgetGame(uint32_t lastOfMap, size_t depositsOfMap) {
  decals_.erase(std::remove_if(decals_.begin(), decals_.end(), [lastOfMap](const Decal& d) { return d.id == 0 || d.id > lastOfMap; }), decals_.end());
  if (deposits_.size() > depositsOfMap) deposits_.resize(depositsOfMap);
  marks_.clear();
  pads_.clear();
  scars_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.scar; }));
  waterDecals_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.water; }));
  cells_.clear();  // [settled] (every settled scar was the game's)
  givenScars_.clear();
  settledCount_ = 0;
  settledVisible_.clear();
  settledDents_.clear();
  settledNow_ = settledLit_ = false;
  snowFallen_ = 2;
  snowClock_ = -1;
  settled_ = false;
}

void Decals::Remove(uint32_t id) {
  for (auto it = decals_.begin(); it != decals_.end(); ++it)
    if (it->id == id) {
      if (it->water) --waterDecals_;
      decals_.erase(it);
      return;
    }
}

// [decalfade]
void Decals::SetNearCutoff(uint32_t id, float nearCutoff) {
  for (auto it = decals_.rbegin(); it != decals_.rend(); ++it)
    if (it->id == id) {
      it->nearCutoff = std::max(nearCutoff, 0.0f);
      return;
    }
}

// [decalfade] (decals.h: fadeFraction, Lod)
float Decals::Measure(Vec3 p, Vec3 eye) const {
  if (fadeFraction < 0) return Length(p - eye) * lod.scale;  // (as before: the distance from the eye)
  // as the original's: the world the view spans across at that depth, times the camera's LOD scale
  return lod.scale * (lod.widthPerDepth * Dot(p - lod.eye, lod.forward));
}

float Decals::LodAlpha(const Decal& d, Vec3 eye) const {
  if (fadeFraction < 0) {  // as before: by its distance from the eye, over the last fifth of its cutoff (a GUESS)
    if (d.cutoff <= 0) return 1;
    const float distance = Measure(d.centre, eye);
    if (distance > d.cutoff) return -1;
    return Clamp((d.cutoff - distance) / (d.cutoff * 0.2f), 0, 1);
  }
  // as the original's GetLODAlpha (0x89d400), whose caller (Render_FlatDecals 0x8003e0) measures at the decal's middle
  // on the ground
  const float f = Clamp(fadeFraction, 0, 1);
  const float m = Measure(d.centre, eye);
  if (d.nearCutoff > 0) {
    // comes in from nothing at f x its near cutoff to whole at it; and is left out beyond its cutoff (the original's
    // spatial query does that by the cutoff, CWldTerrainDecal::Update 0x89df2c and SpatialDB::CollectInView 0x5041e0,
    // testing a whole cell's nearest corner: here the decal's middle, INFERRED)
    if (d.cutoff > 0 && m > d.cutoff) return -1;
    const float start = d.nearCutoff * f;
    if (!(d.nearCutoff > start)) return m >= d.nearCutoff ? 1.0f : 0.0f;  // (f 1: no fade at all)
    return (Clamp(m, start, d.nearCutoff) - start) / (d.nearCutoff - start);
  }
  if (d.cutoff <= 0) return 1;  // (no cutoff: always, as before; the original's formula gives no number there)
  if (m >= d.cutoff) return -1;
  const float start = d.cutoff * f;
  if (!(d.cutoff > start)) return 1;  // (f 1: whole up to its cutoff)
  return 1 - (Clamp(m, start, d.cutoff) - start) / (d.cutoff - start);
}

bool Decals::AddNormal(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  if (!Add(texture, centre, sizeX, sizeZ, heading, cutoff, born, seconds)) return false;
  decals_.back().normal = true;
  // [scars] (its file, for modernRelief to read once whether it is a plain normal map; not the visuals track's own)
  if (!decals_.back().ours) normalFiles_.emplace(decals_.back().texture.id, lastFile_);
  return true;
}

void Decals::AddMark(gfx::Texture texture, Vec3 centre, float sizeX, float sizeZ, float heading) {
  Decal d{texture, {}, centre, sizeX * 0.5f, sizeZ * 0.5f, heading, 0, 0, 0};
  d.mark = true;
  marks_.push_back(d);
}

bool Decals::AddLight(const std::string& texture, const std::string& ramp, Vec3 centre, float size, double born, float seconds) {
  if (size <= 0 || seconds <= 0 || !vfs_->Exists(texture)) return false;
  const gfx::Texture t = textures_->Get(texture);
  const gfx::Texture r = vfs_->Exists(ramp) ? textures_->Get(ramp, textures_->White()) : textures_->White();
  if (!t) return false;
  decals_.push_back({t, r, centre, size, size, 0, 0, born, seconds});
  return true;
}

// [tarmac] A pad's picture's name among those seen (Tarmacs::Key), or -1: not a pad's.
int Decals::TarmacOf(const std::string& texture) {
  const std::string key = Tarmacs::Key(texture);
  if (key.empty()) return -1;
  for (size_t i = 0; i < tarmacKeys_.size(); ++i)
    if (tarmacKeys_[i] == key) return static_cast<int>(i);
  tarmacKeys_.push_back(key);
  tarmacMaterial_.push_back(-2);
  return static_cast<int>(tarmacKeys_.size() - 1);
}

// [tarmac] The material a decal of a pad is drawn in with remastered tarmacs on (-1: the game's pictures): none for one the
// original effects laid, nor for a mark, a light or a "Water Albedo" decal.
int Decals::TarmacMaterial(const Decal& d) {
  if (d.tarmac < 0 || d.original || d.mark || d.water || d.ramp) return -1;
  int& m = tarmacMaterial_[static_cast<size_t>(d.tarmac)];
  if (m == -2) m = tarmacs.MaterialOf(tarmacKeys_[static_cast<size_t>(d.tarmac)]);
  return m >= 0 && tarmacs.Usable(m) ? m : -1;
}

// [tarmac] A pad as Tarmacs draws it: over its material's extent, faded as the game's picture would be (its last seconds;
// with `view`: also as its distance fades it, LodAlpha, and not where the view does not see it).
bool Decals::TarmacPad(const Decal& d, double now, const View* view, Tarmacs::Pad* pad) {
  const int material = TarmacMaterial(d);
  if (material < 0 || d.normal || now < d.born) return false;
  const float extent = tarmacs.Extent(material);
  const float halfX = d.halfX * extent, halfZ = d.halfZ * extent;
  float alpha = d.seconds > 0 ? Clamp(static_cast<float>(d.born + static_cast<double>(d.seconds) - now) / std::min(5.0f, d.seconds * 0.5f), 0, 1) : 1.0f;
  if (view) {
    const float reach = std::max(std::fabs(halfX), std::fabs(halfZ)) * 1.5f;
    const Vec3 r{reach, reach * 0.7f + 1.5f, reach};
    const float lodAlpha = LodAlpha(d, view->eye);  // [decalfade]
    if (lodAlpha < 0 || !view->BoxVisible(d.centre - r, d.centre + r)) return false;
    alpha *= lodAlpha;
  }
  if (alpha <= 0 || (fadeFraction >= 0 && alpha < 1.0f / 255)) return false;  // ([decalfade] as the original leaves it out)
  pad->centre = d.centre;
  pad->halfX = halfX;
  pad->halfZ = halfZ;
  pad->cosHeading = std::cos(d.heading);
  pad->sinHeading = std::sin(d.heading);
  pad->alpha = alpha;
  pad->snow = snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1;  // (as a picture's mark: Draw)
  pad->depth = tarmacs.Depth(material, 2 * d.halfX);
  pad->material = material;
  return true;
}

void Decals::TarmacPads(double now, std::vector<Tarmacs::Pad>* out) {
  out->clear();
  if (!remasteredTarmacs) return;
  Tarmacs::Pad pad;
  for (const Decal& d : decals_)
    if (d.tarmac >= 0 && TarmacPad(d, now, nullptr, &pad)) out->push_back(pad);
}

// [settled] (decals.h) How much of a battle scar is there by its age.
float Decals::ScarAlpha(const Decal& d, double now) const {
  const float past = static_cast<float>(now - d.born) - d.seconds;
  float alpha = 1 - (1 - scarFloor) * Clamp(past / std::max(scarWeathering, 1.0f), 0, 1);
  if (d.scarGone > 0) alpha *= Clamp(static_cast<float>(d.scarGone - now) / 10, 0, 1);
  return alpha;
}

// A decal as the shader draws it this frame (Draw's, and [settled] LaySettled's for the settled scars).
bool Decals::Show(const Decal& d, const View& view, double now, bool overWater, Visible* out) {
  const bool modernNormals = modernRelief && normalsModern_;  // [scars]
  const float reach = std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) * 1.5f;
  // the ground under a decal may rise and fall across it: the box is tall for its width
  const float halfHeight = reach * 0.7f + 1.5f;
  const Vec3 r{reach, halfHeight, reach};
  const float lodAlpha = LodAlpha(d, view.eye);  // [decalfade] (-1: beyond its cutoff)
  if (lodAlpha < 0) return false;
  if (!view.BoxVisible(d.centre - r, d.centre + r)) return false;
  if (now < d.born) return false;  // (a light whose beat the picture has not reached)
  float alpha = 1;
  // (it fades out over its last five seconds, a short-lived one over the second half of its time: a
  // tank's tread marks last a second, a walker's footprints four; how the original fades them is a GUESS)
  if (d.scar && battleScars) {  // a battle scar: weathered after the game's time, faded out once the most is passed
    alpha = ScarAlpha(d, now);
  } else if (d.seconds > 0) {
    alpha = Clamp(static_cast<float>(d.born + static_cast<double>(d.seconds) - now) / std::min(5.0f, d.seconds * 0.5f), 0, 1);
  }
  // it comes in as its distance says (LodAlpha: as before over the last fifth of it, a GUESS; or as the original's
  // GetLODAlpha, fadeFraction), and with the original's fade it is left out where the original leaves it out
  alpha *= lodAlpha;
  if (fadeFraction >= 0 && alpha < 1.0f / 255) return false;
  Instance n{};
  n.centre[0] = d.centre.x;
  n.centre[1] = d.centre.y;
  n.centre[2] = d.centre.z;
  // ([steep] one the game laid, not the map: the shader fades it out on steep ground; told by the sign)
  n.halfHeight = d.mapOwn || d.water || d.mark || d.ramp ? halfHeight : -halfHeight;
  n.halfX = d.halfX;
  n.halfZ = d.halfZ;
  n.cosHeading = std::cos(d.heading);
  n.sinHeading = std::sin(d.heading);
  n.alpha = alpha;
  n.age = d.seconds > 0 ? static_cast<float>((now - d.born) / static_cast<double>(d.seconds)) : 0;
  n.light = d.water ? (overWater ? 5.0f : 4.0f) : d.mark ? 3.0f : d.ramp ? 1.0f : d.normal ? 2.0f : 0.0f;
  // (a plain mark has no use for the age: there it says how much of the lying snow covers it)
  if (!d.mark && !d.ramp && !d.normal) n.age = snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1;
  // (a light's brightness; a picture's 1 with its own relief; a normal decal's 1 when it is the visuals track's)
  n.glow = d.ramp ? glow : d.normal ? (d.ours ? 1.0f : 0.0f) : d.relief && !d.mark && !d.water ? 1.0f : 0.0f;
  // [scars] (modernRelief: a normal decal's age says how much of the lying snow covers it, as a picture's, from -1 down;
  // a plain normal map's glow 2)
  // ([snowfix] every normal decal's: the plain ones' relief is filled under the snow too, NormalDecalBuried)
  if (d.normal && !d.water && !d.mark && !d.ramp) n.age = -1 - (snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1);
  if (modernNormals && d.normal && !d.water && !d.mark && !d.ramp) {
    if (!d.ours && Plain(d.texture.id)) {
      n.glow = 2;
      ++plainNormals;
    }
  }
  *out = {d.texture.id, d.ramp.id, d.relief.id, d.water ? 4 : d.mark ? 3 : d.ramp ? 2 : d.normal ? 1 : 0, n, d.mapOwn};
  d.seenAt = wall_;  // [settled] (a scar settles while no view shows it)
  return true;
}

void Decals::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now, bool overWater) {
  wall_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();  // [settled]
  if (!overWater) {
    drawn = 0;
    tarmacsDrawn = 0;  // [tarmac]
    // the snow that has fallen since the last frame (a mark is covered by one unit of it)
    if (snowClock_ >= 0 && now > snowClock_) snowFallen_ += snowing * static_cast<float>(std::min(now - snowClock_, 0.5)) / 90.0f;
    snowClock_ = now;
    settled_ = true;
    // those whose time is over go
    // (a battle scar only once it has faded out, as the most there may be asks; with battle scars off, at its time)
    auto over = [&](const Decal& d) {
      if (d.scar && battleScars) return d.scarGone > 0 && now >= d.scarGone;
      return d.seconds > 0 && now - d.born >= static_cast<double>(d.seconds);
    };
    decals_.erase(std::remove_if(decals_.begin(), decals_.end(), over), decals_.end());
    scars_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.scar; }));
    waterDecals_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.water; }));
  }
  visible_.clear();
  pads_.clear();  // [tarmac]
  const bool pads = remasteredTarmacs && !overWater;
  const bool modernNormals = modernRelief && normalsModern_;  // [scars]
  if (!overWater) plainNormals = 0;
  // [settled] (settleScars) what LaySettled made ready for this frame: the place the settled scars' picture is lit at -
  // among the pictures, where the lowest texture of those laid stands, before any other of that texture (they are older)
  // - and the settled dents in view, drawn as before
  if (!overWater && settledNow_) {
    if (settledLit_) {
      Visible place{settledFirst_, 0, 0, 0, Instance{}, false};
      place.settled = true;
      visible_.push_back(place);
    } else {  // (too few to pay for their own picture: each drawn as a picture is, in the same place among them)
      for (Visible& scar : settledVisible_) {
        scar.old = true;
        visible_.push_back(scar);
      }
    }
    for (const Visible& dent : settledDents_) {
      if (dent.instance.glow == 2) ++plainNormals;
      visible_.push_back(dent);
    }
  }
  for (size_t which = 0; which < decals_.size() + marks_.size(); ++which) {
    const Decal& d = which < decals_.size() ? decals_[which] : marks_[which - decals_.size()];
    if (overWater && !d.water) continue;
    // [tarmac] a pad the visuals track has a material for: drawn in it (Tarmacs), its "_normals" twin not at all
    if (pads && d.tarmac >= 0 && TarmacMaterial(d) >= 0) {
      tarmacs.Want(TarmacMaterial(d));  // [hitches] (its material read ahead wherever the pad lies: Tarmacs::Want)
      Tarmacs::Pad pad;
      if (TarmacPad(d, now, &view, &pad)) pads_.push_back(pad);
      continue;
    }
    Visible shown;
    if (!Show(d, view, now, overWater, &shown)) continue;
    const Instance& n = shown.instance;
    visible_.push_back(shown);
    // hot metal: a fresh scorch's or crater's middle glows over it (not under water, where it would only steam)
    if (d.burn && hotMetal && heat && !overWater && !d.normal && !d.water && !d.mark && !d.ramp && d.centre.y > waterLevel) {
      const float across = 2 * std::max(std::fabs(d.halfX), std::fabs(d.halfZ));
      const float age = static_cast<float>(now - d.born), seconds = heat->CraterSeconds(across);
      if (age < seconds) {
        Instance h = n;
        h.alpha = age;
        h.age = seconds;
        h.light = heat->softScorch ? 7.0f : 6.0f;  // ([softscorch] the shader: 7 the broad middle a deep red)
        h.glow = static_cast<float>(d.id % 97);
        visible_.push_back({d.texture.id, 0, 0, 5, h});
      }
    }
  }
  if (!overWater) marks_.clear();
  if (visible_.empty() && pads_.empty()) return;
  // pictures first, then lights; by texture; those of one texture keep the order they were made in
  // (hot metal's glows, kind 5, after the lights and before the marks)
  // ([tarmac] with remastered pads drawn: the map's own pictures, the pads, then the pictures laid since: rank 0.5)
  const bool padsNow = !pads_.empty();
  auto rank = [padsNow](const Visible& v) { return v.kind == 5 ? 2.5f : padsNow && v.kind == 0 && !v.mapOwn ? 0.5f : static_cast<float>(v.kind); };
  // ([settled] the settled scars stand together where the lowest texture of them stands, before any other of that
  // texture - their picture's place, or each of them when they are drawn one by one; none there: as before)
  const uint32_t settledAt = settledFirst_;
  auto place = [settledAt](const Visible& v) { return v.settled || v.old ? settledAt : v.texture; };
  std::stable_sort(visible_.begin(), visible_.end(), [&rank, &place](const Visible& a, const Visible& b) {
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (place(a) != place(b)) return place(a) < place(b);
    if ((a.settled || a.old) != (b.settled || b.old)) return a.settled || a.old;
    if (a.texture != b.texture) return a.texture < b.texture;
    return a.ramp != b.ramp ? a.ramp < b.ramp : a.relief < b.relief;
  });

  const bool water = waterMap_ && waterRamp_ && mapWidth_ > 0 && mapHeight_ > 0;
  float cameraConstants[20];
  CameraConstants(view, camera, cameraConstants);
  // [decalsnow] while the ground lays its snow from its shelter (snowGround): the pictures', and with modernRelief the relief's,
  // pipelines with the snow ground's, their constants after the camera's (the wind's way and the map's size, the tuning, the
  // materials), the shelter at t6. (Not on the water's drawing: its decals take no snow.)
  const bool snowNow = snowGround.shelter && !overWater && snowGround.mapWidth > 0 && snowGround.mapHeight > 0;
  const bool picturesSnow = snowNow && picturesSnow_, normalsSnow = snowNow && modernNormals && normalsModernSnow_;
  float snowConstants[32] = {};
  if (snowNow) SnowConstants(cameraConstants, snowConstants);
  // ([padsnow] the pads lay it as the marks do: Tarmacs::snowShelter)
  tarmacs.snowShelter = snowNow ? snowGround.shelter : gfx::Texture{};
  if (snowNow) std::copy(snowConstants + 20, snowConstants + 32, tarmacs.snow);
  // [whitecaps] the pictures on the water, on the lifted waves (World::oceanLift): their pipeline that rides them (made the
  // first time it is wanted)
  bool riding = false;
  if (overWater && ride) {
    if (!rideTried_) {
      rideTried_ = true;
      const std::string source = DecalSource(false, false, true);
      gfx::PipelineDesc pd;
      pd.name = "decals on the water, riding the waves";
      pd.vertexSource = pd.pixelSource = source;
      pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
      pd.stride = 12;
      pd.cull = gfx::Cull::Front;
      pd.depthTest = pd.depthWrite = false;
      pd.depthTarget = false;
      pd.blend = gfx::Blend::Alpha;
      pd.colorFormat = kSceneFormat;
      pd.samples = gSceneSamples;
      pd.alphaWrite = false;
      std::string error;
      waterRide_ = device_->CreatePipeline(pd, &error);
      if (!waterRide_) fprintf(stderr, "render: the pictures on the water cannot ride the waves (%s)\n", error.c_str());
    }
    riding = waterRide_.id != 0;
  }
  int bound = -1;
  bool padsDone = !padsNow;  // [tarmac] (the pads drawn after the map's own pictures, before what was laid since)
  auto drawPads = [&]() {
    tarmacsDrawn = tarmacs.Draw(view, cameraConstants, sceneDepth, water ? waterMap_ : gfx::Texture{}, water ? waterRamp_ : gfx::Texture{}, cube_, pads_);
    padsDone = true;
    bound = -1;  // (the decals' own pipeline and textures bound again)
  };
  for (size_t at = 0; at < visible_.size();) {
    if (!padsDone && rank(visible_[at]) >= 0.5f) drawPads();
    if (visible_[at].settled) {  // [settled] the settled scars' picture, lit once over the ground (LaySettled laid it)
      const bool snowLit = settledSnow_;  // (as it was laid)
      const SettledPicture& picture = settledPictures_[static_cast<size_t>(settledPicture_)];
      if (settledLight_[snowLit ? 1 : 0]) {
        device_->SetPipeline(settledLight_[snowLit ? 1 : 0]);
        device_->SetConstants(0, &view.constants, sizeof view.constants);
        if (snowLit) device_->SetConstants(1, snowConstants, sizeof snowConstants);
        else device_->SetConstants(1, cameraConstants, sizeof cameraConstants);
        if (snowLit) device_->SetTexture(6, snowGround.shelter);
        device_->SetTexture(0, picture.colour);
        device_->SetTexture(5, picture.lean);
        device_->SetTexture(1, sceneDepth);
        device_->SetTexture(3, water ? waterMap_ : textures_->Black());
        device_->SetTexture(4, water ? waterRamp_ : textures_->Black());
        device_->SetTexture(7, groundHeights_ ? groundHeights_ : textures_->Black());
        TrackFieldConstants field = holes.constants;  // (not read by this one: the layers gave way as they were laid)
        field.info[2] = 0;
        device_->SetConstants(3, &field, sizeof field);
        device_->SetTexture(8, textures_->Black());
        device_->SetTexture(9, textures_->Black());
        device_->Draw(3);
        drawn += settledLaid;
        device_->SetTexture(0, {});  // (the scars' picture is drawn into again before the next frame's scene)
        device_->SetTexture(5, {});
        if (snowLit) device_->SetTexture(6, {});
      }
      bound = -1;  // (the decals' own pipeline and textures bound again)
      ++at;
      continue;
    }
    const uint32_t texture = visible_[at].texture, ramp = visible_[at].ramp, relief = visible_[at].relief;
    const int kind = visible_[at].kind;
    size_t n = 0;
    while (at + n < visible_.size() && !visible_[at + n].settled && visible_[at + n].texture == texture && visible_[at + n].ramp == ramp && visible_[at + n].relief == relief &&
           visible_[at + n].kind == kind && n < kMaxInstances) {
      batch_[n] = visible_[at + n].instance;
      ++n;
    }
    if (bound != kind) {
      bound = kind;
      // (marks blend as pictures do; [scars] relief; [decalsnow] the snow ground's)
      const bool snowBound = kind == 1 ? normalsSnow : kind != 2 && kind != 5 && picturesSnow;
      device_->SetPipeline(kind == 5 ? hot_ : kind == 2 ? lights_ : kind == 1 ? (modernNormals ? (normalsSnow ? normalsModernSnow_ : normalsModern_) : normals_)
                           : riding ? waterRide_ : picturesSnow ? picturesSnow_ : pipeline_);
      if (riding) {  // [whitecaps] (the lifted waves' field and constants)
        device_->SetConstants(3, ride->RideConstants(), ride->RideConstantsSize());
        ride->BindRide();
      }
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      if (snowBound) device_->SetConstants(1, snowConstants, sizeof snowConstants);
      else device_->SetConstants(1, cameraConstants, sizeof cameraConstants);
      if (snowBound) device_->SetTexture(6, snowGround.shelter);
      device_->SetTexture(1, sceneDepth);
      device_->SetTexture(3, water ? waterMap_ : textures_->Black());
      device_->SetTexture(4, water ? waterRamp_ : textures_->Black());
      device_->SetTexture(7, groundHeights_ ? groundHeights_ : textures_->Black());  // [steep]
      if (!riding || kind == 5 || kind == 2 || kind == 1) {  // [churn] (the shell holes under the marks; none: nothing read.
        // Every pipeline but the one that rides the waves, which has the waves' constants in this place)
        TrackFieldConstants field = holes.constants;
        if (!holes.field || riding) field.info[2] = 0;
        device_->SetConstants(3, &field, sizeof field);
        device_->SetTexture(8, holes.field ? holes.field : textures_->Black());
        device_->SetTexture(9, holes.kinds ? holes.kinds : textures_->Black());
      }
      device_->SetVertexBuffer(cube_);
    }
    device_->SetTexture(0, gfx::Texture{texture});
    device_->SetTexture(2, ramp ? gfx::Texture{ramp} : textures_->White());
    device_->SetTexture(5, relief ? gfx::Texture{relief} : textures_->FlatNormal());
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->Draw(36, 0, static_cast<uint32_t>(n));
    drawn += static_cast<int>(n);
    at += n;
  }
  if (!padsDone) drawPads();
  if (snowNow) device_->SetTexture(6, {});  // [decalsnow]
  if (holes.field && !riding) {  // [churn] (the field is drawn into again before the next frame's scene)
    device_->SetTexture(8, {});
    device_->SetTexture(9, {});
  }
  if (riding) ride->UnbindRide();  // [whitecaps]
  if (!overWater) settledNow_ = settledLit_ = false;  // [settled] (the next frame's LaySettled says anew)
}

// The camera's constants as the decals' shader takes them (DecalCamera), and with the snow ground's after them.
void Decals::CameraConstants(const View& view, const Camera& camera, float* c) const {
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const bool water = waterMap_ && waterRamp_ && mapWidth_ > 0 && mapHeight_ > 0;
  const float cameraConstants[20] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                                     up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                                     back.x, back.y, back.z, camera.zNear,
                                     water ? 1 / mapWidth_ : 0, water ? 1 / mapHeight_ : 0, water ? 1.0f : 0.0f, water ? waterLevel : 0.0f,
                                     groundHeights_ ? 1 / heightsWidth_ : 0, groundHeights_ ? 1 / heightsHeight_ : 0,  // [steep]
                                     groundHeights_ ? groundUnit_ : 0, 0};
  std::copy(cameraConstants, cameraConstants + 20, c);
}

void Decals::SnowConstants(const float* cameraConstants, float* snowConstants) const {
  std::copy(cameraConstants, cameraConstants + 20, snowConstants);
  const float wind = std::sqrt(snowGround.wind[0] * snowGround.wind[0] + snowGround.wind[1] * snowGround.wind[1]);
  snowConstants[20] = wind > 0 ? snowGround.wind[0] / wind : 0.0f;
  snowConstants[21] = wind > 0 ? snowGround.wind[1] / wind : 0.0f;
  snowConstants[22] = 1 / snowGround.mapWidth;
  snowConstants[23] = 1 / snowGround.mapHeight;
  std::copy(snowGround.tune, snowGround.tune + 4, snowConstants + 24);
  snowConstants[28] = snowGround.materials ? 1.0f : 0.0f;
}

// ---- [settled] settled scars (decals.h, settleScars) ---------------------------------------------------------------------

uint16_t Decals::ScarName(const std::string& texture) {
  for (size_t k = 0; k < scarNames_.size(); ++k)
    if (scarNames_[k] == texture) return static_cast<uint16_t>(k);
  if (scarNames_.size() >= 0xffff) return 0xffff;
  scarNames_.push_back(texture);
  return static_cast<uint16_t>(scarNames_.size() - 1);
}

void Decals::KeepScars(double now, std::vector<KeptScar>* out) const {
  out->clear();
  if (!settleScars || !battleScars) return;
  auto keep = [&](const Decal& d, bool settled) {
    if (!d.scar || d.name >= scarNames_.size() || d.scarGone > 0) return;
    if (!settled && d.born + static_cast<double>(d.seconds) > now) return;  // (not yet past its time: the effects lay it again)
    KeptScar k;
    k.texture = scarNames_[d.name];
    k.centre = d.centre;
    k.sizeX = d.halfX * 2;
    k.sizeZ = d.halfZ * 2;
    k.heading = d.heading;
    k.cutoff = d.cutoff;
    k.born = d.born;
    k.seconds = d.seconds;
    k.snow = Clamp(snowFallen_ - d.snowAtBirth, 0, 1);
    k.normal = d.normal;
    k.settled = settled;
    out->push_back(std::move(k));
  };
  for (const Decal& d : decals_) keep(d, false);
  for (const auto& entry : cells_)
    for (const Decal& d : entry.second.scars) keep(d, true);
  // (in the order they were made: the settled ones' squares come in no order)
  std::stable_sort(out->begin(), out->end(), [](const KeptScar& a, const KeptScar& b) { return a.born < b.born; });
}

void Decals::LayGivenScars() {
  // each laid as a plain mark and then made the scar it was (not counted against the most as a new one, no glow: it is
  // long cold), the settled ones settled at once
  const bool scarsWere = battleScars, hotWas = hotMetal;
  battleScars = hotMetal = false;
  const std::vector<KeptScar> scars = std::move(givenScars_);
  givenScars_.clear();
  for (const KeptScar& k : scars) {
    const bool laid = k.normal ? AddNormal(k.texture, k.centre, k.sizeX, k.sizeZ, k.heading, k.cutoff, k.born, k.seconds)
                               : Add(k.texture, k.centre, k.sizeX, k.sizeZ, k.heading, k.cutoff, k.born, k.seconds);
    if (!laid) continue;
    Decal& d = decals_.back();
    d.scar = true;
    d.name = ScarName(k.texture);
    d.snowAtBirth = snowFallen_ - k.snow;
    d.mapOwn = false;  // (the game's: gone on steep ground, after the map's own and the pads)
    if (k.settled) {
      Decal moved = std::move(d);
      decals_.pop_back();
      Settle(std::move(moved));
    } else {
      ++scars_;
    }
  }
  battleScars = scarsWere;
  hotMetal = hotWas;
}

bool Decals::Glowing(const Decal& d, double now) const {
  if (!d.burn || !hotMetal || !heat) return false;
  return static_cast<float>(now - d.born) < heat->CraterSeconds(2 * std::max(std::fabs(d.halfX), std::fabs(d.halfZ)));
}

void Decals::Settle(Decal&& d) {
  Cell& cell = cells_[CellKey(d.centre.x, d.centre.z)];
  cell.reach = std::max(cell.reach, std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) * 1.5f);
  cell.low = std::min(cell.low, d.centre.y);
  cell.high = std::max(cell.high, d.centre.y);
  if (d.cutoff > 0) cell.cutoff = std::max(cell.cutoff, d.cutoff);
  else cell.anyCutoff = false;
  d.burn = false;
  cell.scars.push_back(std::move(d));
  ++settledCount_;
  Mark("battle scars settled", static_cast<uint64_t>(settledCount_), static_cast<uint64_t>(mostSettled));
  // past the most the oldest of all goes (each square's own are in the order they settled: its first is its oldest)
  while (settledCount_ > std::max(mostSettled, 0)) {
    Cell* from = nullptr;
    for (auto& entry : cells_)
      if (!entry.second.scars.empty() && (!from || entry.second.scars.front().born < from->scars.front().born)) from = &entry.second;
    if (!from) break;
    from->scars.erase(from->scars.begin());
    --settledCount_;
  }
}

void Decals::Unsettle(double now) {
  // all back among the others, each where its number puts it (the list is in the order they were made; a light on the
  // ground has no number and stays behind the one it follows)
  std::vector<std::pair<uint32_t, Decal>> all;
  all.reserve(decals_.size() + static_cast<size_t>(settledCount_));
  uint32_t last = 0;
  for (Decal& d : decals_) {
    if (d.id) last = d.id;
    all.emplace_back(last, std::move(d));
  }
  for (auto& entry : cells_)
    for (Decal& d : entry.second.scars) all.emplace_back(d.id, std::move(d));
  cells_.clear();
  settledCount_ = 0;
  std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  decals_.clear();
  for (auto& entry : all) decals_.push_back(std::move(entry.second));
  // and, as before, no more than the most: the oldest past it fade out
  std::vector<Decal*> scars;
  for (Decal& d : decals_)
    if (d.scar && d.scarGone <= 0) scars.push_back(&d);
  scars_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.scar; }));
  if (battleScars && static_cast<int>(scars.size()) > mostScars) {
    std::stable_sort(scars.begin(), scars.end(), [](const Decal* a, const Decal* b) { return a->born < b->born; });
    for (size_t k = 0; k + static_cast<size_t>(std::max(mostScars, 0)) < scars.size(); ++k) scars[k]->scarGone = now + 10;
  }
}

bool Decals::SettledPipelines(bool snow) {
  const int s = snow ? 1 : 0;
  if (settledTried_[s]) return settledPlace_ && settledLay_[0][s] && settledLay_[1][s] && settledLight_[s];
  settledTried_[s] = true;
  std::string error;
  if (!settledPlace_) {  // what the place does to them all, into its own picture (one channel)
    const std::string source = DecalSource(false, false, false, 4);
    gfx::PipelineDesc pd;
    pd.name = "settled scars: the place";
    pd.vertexSource = pd.pixelSource = source;
    pd.vertexEntry = "VSSettled";
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.cull = gfx::Cull::None;
    pd.blend = gfx::Blend::Opaque;
    pd.colorFormat = gfx::Format::R8;
    pd.samples = 1;
    settledPlace_ = device_->CreatePipeline(pd, &error);
    if (!settledPlace_) fprintf(stderr, "render: the settled scars' place cannot be worked out (%s)\n", error.c_str());
  }
  for (int pass = 0; pass < 3; ++pass) {
    const std::string source = DecalSource(false, snow, false, pass + 1);  // (kept while the pipeline is made: the description points into it)
    gfx::PipelineDesc pd;
    pd.vertexSource = pd.pixelSource = source;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    if (pass < 2) {  // a scar's box laid into the scars' picture: over what is there, premultiplied
      pd.name = pass == 0 ? (snow ? "settled scars laid: colour, snow ground" : "settled scars laid: colour")
                          : (snow ? "settled scars laid: relief and snow, snow ground" : "settled scars laid: relief and snow");
      pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
      pd.stride = 12;
      pd.cull = gfx::Cull::Front;
      pd.blend = gfx::Blend::Premultiplied;
      pd.colorFormat = gfx::Format::RGBA16F;
      pd.samples = 1;
      pd.alphaWrite = true;
      settledLay_[pass][s] = device_->CreatePipeline(pd, &error);
      if (!settledLay_[pass][s]) fprintf(stderr, "render: the settled scars cannot be laid (%s)\n", error.c_str());
    } else {  // the scars' picture lit over the ground
      pd.name = snow ? "settled scars lit, snow ground" : "settled scars lit";
      pd.vertexEntry = "VSSettled";
      pd.cull = gfx::Cull::None;
      pd.blend = gfx::Blend::Alpha;
      pd.colorFormat = kSceneFormat;
      pd.samples = gSceneSamples;
      pd.alphaWrite = false;
      settledLight_[s] = device_->CreatePipeline(pd, &error);
      if (!settledLight_[s]) fprintf(stderr, "render: the settled scars cannot be lit (%s)\n", error.c_str());
    }
  }
  return settledPlace_ && settledLay_[0][s] && settledLay_[1][s] && settledLight_[s];
}

void Decals::LaySettled(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now) {
  wall_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  settledNow_ = true;
  settledLit_ = false;
  settledLaid = 0;
  // (a scars' picture no view has used for ten seconds is let go: no settled scar was in view at its size)
  for (size_t k = settledPictures_.size(); k-- > 0;)
    if (wall_ - settledPictures_[k].usedAt > 10) {
      device_->Destroy(settledPictures_[k].colour);
      device_->Destroy(settledPictures_[k].lean);
      device_->Destroy(settledPictures_[k].place);
      settledPictures_.erase(settledPictures_.begin() + static_cast<std::ptrdiff_t>(k));
    }
  settledVisible_.clear();
  settledDents_.clear();
  if (!settleScars || !battleScars) {  // off: what had settled is back among the others (and goes as they go)
    if (settledCount_) Unsettle(now);
    givenScars_.clear();
    return;
  }
  if (!givenScars_.empty()) LayGivenScars();  // ([devicelost] the scars of the world this one takes the place of)
  // (its pipelines are made ahead: SetSettleScars, PrepareSnowGround. With the snow ground's while the pictures lay theirs so.)
  const bool snowNow = snowGround.shelter && snowGround.mapWidth > 0 && snowGround.mapHeight > 0;
  const bool plain = settledPlace_ && settledLay_[0][0] && settledLay_[1][0] && settledLight_[0];
  const bool withSnow = snowNow && picturesSnow_ && settledLay_[0][1] && settledLay_[1][1] && settledLight_[1];
  settledSnow_ = withSnow;
  if (!plain) return;  // (cannot be laid: every scar stays among the others, as before)
  // the snow that has fallen since the last frame, as Draw moves it on (it then finds it done for this frame)
  if (snowClock_ >= 0 && now > snowClock_) snowFallen_ += snowing * static_cast<float>(std::min(now - snowClock_, 0.5)) / 90.0f;
  snowClock_ = now;
  // what has weathered to its floor settles, while no view shows it (not one that is fading out, nor one whose middle
  // still glows)
  {
    size_t kept = 0;
    for (size_t k = 0; k < decals_.size(); ++k) {
      Decal& d = decals_[k];
      if (d.scar && d.scarGone <= 0 && !d.water && !d.mark && static_cast<float>(now - d.born) - d.seconds >= scarWeathering &&
          (settleInView || wall_ - d.seenAt > 0.5) && !Glowing(d, now)) {
        Settle(std::move(d));
        --scars_;
        continue;
      }
      if (kept != k) decals_[kept] = std::move(d);
      ++kept;
    }
    decals_.resize(kept);
  }
  // (still over the most - scars whose glow kept them from settling when it was passed, Add: the oldest whose glow is
  // out by now settle, in view or not)
  while (scars_ > mostScars) {
    Decal* oldest = nullptr;
    for (Decal& d : decals_)
      if (d.scar && d.scarGone <= 0 && !d.water && !d.mark && !Glowing(d, now) && (!oldest || d.born < oldest->born)) oldest = &d;
    if (!oldest) break;
    Decal moved = std::move(*oldest);
    decals_.erase(decals_.begin() + (oldest - decals_.data()));
    Settle(std::move(moved));
    --scars_;
  }
  if (!settledCount_) return;
  // the settled scars in view, a square at a time
  const int plainBefore = plainNormals;
  const float focal = static_cast<float>(view.height) / (2 * std::tan(camera.fovY * 0.5f));  // (pixels a world unit at distance 1)
  double covered = 0;
  for (const auto& entry : cells_) {
    const Cell& cell = entry.second;
    if (cell.scars.empty()) continue;
    const float x0 = static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(entry.first >> 32))) * kSettledCell;
    const float z0 = static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(entry.first))) * kSettledCell;
    const float tall = cell.reach * 0.7f + 1.5f;
    const Vec3 lo{x0 - cell.reach, cell.low - tall, z0 - cell.reach}, hi{x0 + kSettledCell + cell.reach, cell.high + tall, z0 + kSettledCell + cell.reach};
    if (!view.BoxVisible(lo, hi)) continue;
    if (cell.anyCutoff) {  // (beyond the farthest cutoff of them all: none is drawn, LodAlpha)
      float least;
      if (fadeFraction < 0) {
        const Vec3 nearest{Clamp(view.eye.x, lo.x, hi.x), Clamp(view.eye.y, lo.y, hi.y), Clamp(view.eye.z, lo.z, hi.z)};
        least = Length(nearest - view.eye) * lod.scale;
      } else {
        const Vec3 nearest{lod.forward.x >= 0 ? lo.x : hi.x, lod.forward.y >= 0 ? lo.y : hi.y, lod.forward.z >= 0 ? lo.z : hi.z};
        least = lod.scale * (lod.widthPerDepth * Dot(nearest - lod.eye, lod.forward));
      }
      if (least > cell.cutoff) continue;
    }
    for (const Decal& d : cell.scars) {
      Visible shown;
      if (!Show(d, view, now, false, &shown)) continue;
      shown.id = d.id;
      (d.normal ? settledDents_ : settledVisible_).push_back(shown);
      if (!d.normal) {  // (about how many pixels its picture covers: its size across at its distance, half of the square)
        const float across = std::min(focal * 2 * std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) / std::max(Length(d.centre - view.eye), 0.1f), 2 * focal);
        covered += 0.5 * static_cast<double>(across) * static_cast<double>(across);
      }
    }
  }
  plainNormals = plainBefore;  // (Draw counts the dents' anew)
  if (settledVisible_.empty()) return;
  // laid in the order the pictures are drawn in: by texture, those of one in the order they were made
  std::sort(settledVisible_.begin(), settledVisible_.end(), [](const Visible& a, const Visible& b) {
    if (a.texture != b.texture) return a.texture < b.texture;
    return a.relief != b.relief ? a.relief < b.relief : a.id < b.id;
  });
  settledFirst_ = settledVisible_.front().texture;
  // Whether their own picture pays: it costs three passes over the whole scene whatever is in it (measured at 1920 x
  // 1080: 0.13 ms of the card's time), and saves two thirds of what the scars' own pixels cost lit one by one. So it is
  // made when the scars in view cover, layer by layer, more than 0.6 of the scene's pixels (left again under 0.3: no
  // flicker between the two at the edge); under that each is drawn as a picture is (Draw), in the same place among
  // the pictures, so that going from one way to the other changes the order of nothing.
  {
    const double pixels = static_cast<double>(view.width) * static_cast<double>(view.height);
    settledPays_ = litOnce >= 0 ? litOnce > 0 : covered > (settledPays_ ? 0.3 : 0.6) * pixels;
  }
  if (!settledPays_) return;
  // the scars' picture at the scene's size (made the first time one is in view; a side picture's size has its own)
  settledPicture_ = -1;
  for (size_t k = 0; k < settledPictures_.size(); ++k)
    if (settledPictures_[k].width == view.width && settledPictures_[k].height == view.height) settledPicture_ = static_cast<int>(k);
  if (settledPicture_ < 0) {
    if (settledPictures_.size() >= 4) {  // (the one longest unused gives way)
      size_t oldest = 0;
      for (size_t k = 1; k < settledPictures_.size(); ++k)
        if (settledPictures_[k].usedAt < settledPictures_[oldest].usedAt) oldest = k;
      device_->Destroy(settledPictures_[oldest].colour);
      device_->Destroy(settledPictures_[oldest].lean);
      device_->Destroy(settledPictures_[oldest].place);
      settledPictures_.erase(settledPictures_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    SettledPicture made;
    made.width = view.width;
    made.height = view.height;
    gfx::TargetDesc td;
    td.width = view.width;
    td.height = view.height;
    td.format = gfx::Format::RGBA16F;
    made.colour = device_->CreateTarget(td);
    td.format = gfx::Format::RGBA16F;
    made.lean = device_->CreateTarget(td);
    td.format = gfx::Format::R8;
    td.clearValue[0] = 1;
    made.place = device_->CreateTarget(td);
    if (!made.colour || !made.lean || !made.place) {
      if (made.colour) device_->Destroy(made.colour);
      if (made.lean) device_->Destroy(made.lean);
      if (made.place) device_->Destroy(made.place);
      settledPays_ = false;  // (no room for their picture: each drawn as a picture is, Draw)
      return;
    }
    settledPictures_.push_back(made);
    settledPicture_ = static_cast<int>(settledPictures_.size() - 1);
  }
  SettledPicture& picture = settledPictures_[static_cast<size_t>(settledPicture_)];
  picture.usedAt = wall_;
  float cameraConstants[20], snowConstants[32] = {};
  CameraConstants(view, camera, cameraConstants);
  if (withSnow) SnowConstants(cameraConstants, snowConstants);
  const bool water = waterMap_ && waterRamp_ && mapWidth_ > 0 && mapHeight_ > 0;
  for (int pass = -1; pass < 2; ++pass) {
    gfx::PassDesc into;
    into.color = pass < 0 ? picture.place : pass == 0 ? picture.colour : picture.lean;
    if (pass < 0) into.clearValue[0] = 1;
    device_->BeginPass(into);  // (cleared: nothing laid)
    device_->SetPipeline(pass < 0 ? settledPlace_ : settledLay_[pass][withSnow ? 1 : 0]);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    if (withSnow && pass >= 0) device_->SetConstants(1, snowConstants, sizeof snowConstants);
    else device_->SetConstants(1, cameraConstants, sizeof cameraConstants);
    if (withSnow && pass >= 0) device_->SetTexture(6, snowGround.shelter);
    device_->SetTexture(1, sceneDepth);
    device_->SetTexture(3, water ? waterMap_ : textures_->Black());
    device_->SetTexture(4, water ? waterRamp_ : textures_->Black());
    device_->SetTexture(7, groundHeights_ ? groundHeights_ : textures_->Black());  // [steep]
    TrackFieldConstants field = holes.constants;  // [churn] (the shell holes under the marks, as Draw gives them)
    if (!holes.field) field.info[2] = 0;
    device_->SetConstants(3, &field, sizeof field);
    device_->SetTexture(8, holes.field ? holes.field : textures_->Black());
    device_->SetTexture(9, holes.kinds ? holes.kinds : textures_->Black());
    if (pass < 0) {  // what the place does to every one of them, once a pixel
      device_->SetTexture(2, textures_->White());
      device_->Draw(3);
      device_->SetTexture(8, {});
      device_->SetTexture(9, {});
      continue;
    }
    device_->SetTexture(2, picture.place);
    device_->SetVertexBuffer(cube_);
    for (size_t at = 0; at < settledVisible_.size();) {
      const uint32_t texture = settledVisible_[at].texture, relief = settledVisible_[at].relief;
      size_t n = 0;
      while (at + n < settledVisible_.size() && settledVisible_[at + n].texture == texture && settledVisible_[at + n].relief == relief && n < kMaxInstances) {
        batch_[n] = settledVisible_[at + n].instance;
        ++n;
      }
      device_->SetTexture(0, gfx::Texture{texture});
      device_->SetTexture(5, relief ? gfx::Texture{relief} : textures_->FlatNormal());
      device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
      device_->Draw(36, 0, static_cast<uint32_t>(n));
      at += n;
    }
    if (withSnow) device_->SetTexture(6, {});
    device_->SetTexture(2, {});
    device_->SetTexture(8, {});
    device_->SetTexture(9, {});
  }
  settledLaid = static_cast<int>(settledVisible_.size());
  settledLit_ = true;
}

}  // namespace render
