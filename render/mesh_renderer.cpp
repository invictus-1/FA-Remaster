#include "mesh_renderer.h"

#include "cpu_times.h"
#include "jobs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "blasts.h"
#include "machinery.h"  // [machinery]
#include "build_effects.h"
#include "card_scenery.h"  // [gpuscenery]
#include "effect_files.h"
#include "hot_metal.h"
#include "lighting.h"
#include "original_shield.h"  // [ogshield]
#include "plants.h"  // [plants]
#include "ray_scene.h"
#include "under_snow.h"  // [undersnow]
#include "unit_grime.h"  // [grime]
#include "unit_wear.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 640;  // per draw call (the size of the shader's array: 64 KB at most; 768 before an instance had its wear)

// The surface of a model, then one of the two lightings.
//
// Surface: the normal map's green and alpha are x and y in the frame binormal / tangent / normal;
// where the specular texture's alpha says so, the team colour replaces the albedo.
//
// Original look, after what the game's "Unit" shader computes, written anew:
//   colour = albedo * (light + glow + 2 * reflected environment * spec.r) + highlight * spec.g
// with glow = 2 * spec.b and highlight = (0.6, 0.8, 0.9) * (R.V)^2.
// The other factions' textures were painted for other formulas, and the game's shaders for them say
// what their channels mean:
//   Aeon: as above, but the light is dimmer (0.6 of the map's multiplier), the reflected environment
//     counts once, and the highlight is (0.8, 0.85, 1.1) * (R.V)^3: a bright hull that lives from
//     what it mirrors.
//   Cybran ("Insect"): the sun counts twice in the light; the highlight comes from the game's lookup
//     texture (/textures/engine/insectlookup.dds, by the sun's mirror image against the eye and the sun
//     against the surface; as stored it goes by the second alone: a warm gleam that grows to white
//     where the sun stands straight over the surface) and, like the faint reflection, is absent on
//     team colour. Without the game's file a plain power stands in.
//   Seraphim: the albedo texture's ALPHA is the team mask, and its colour a dim detail picture: the
//     surface is made of reflection (spec.r, by the game's view-angle ramp, the alpha of
//     /textures/environment/falloff_seraphim_lookup.dds: full face on, in two bands toward the edge)
//     and of team colour that is added, stronger toward the silhouette; the sun only adds a highlight.
//
// Modern look: the surface as a material (metal, roughness, glow, occlusion) for lighting.h.
//   Remastered units carry the material in their textures (see ModelLod::labPbr).
//   Original units: the old texture is translated, per faction (FactionSurface below): reflection
//   strength stands for metal, highlight strength for smoothness, with each faction's own weights;
//   Seraphim surfaces are metal throughout, coloured by a light alloy plus their detail picture.
//   Props are plain rough surfaces.
// A unit under construction (its fraction complete rides in the colour's alpha) is drawn by whose
// it is. The original has a look of its own for each faction's building, and these keep to what
// each of them is about without copying it:
//   UEF (and whatever has no faction's look): built from the top down, as the original's box of
//     light sinks round what is not there yet. Above the level of the work the finished surface,
//     with a line of light at the level; below it only a blue lattice of light with a band
//     running up it, and nothing between its lines.
//   Cybran: put together plate by plate, in no order; a new plate glows red for a moment, and
//     where a plate is missing there is a red lattice.
//   Aeon: it rises out of the ground, a little smaller at first, as liquid metal that sets into
//     its own surface over the last quarter (the original: sunk by its height until 80 %, here by
//     four fifths of it so that its top shows from the start; three
//     quarters of its size at first).
//   Seraphim: it grows from a quarter of its size, dark and glowing, and takes its surface over
//     the second half (the original: from a quarter of its size).
// In all of them the team colour comes in over the last tenth, as in the original. What has no
// team mask (a prop, a mod's unit drawn as one) is only darker while it is built.
//
// Bones: every vertex follows one bone. A model at rest is drawn as its file holds it; for a posed
// one the vertex is first moved by its bone's matrix (three rows in the bone buffer).
const char kShader[] = R"hlsl(
struct Instance {
  float4 row0, row1, row2;
  float4 color;
  float4 bones;
  float4 wear;  // battle scars and snow (unit_wear.h): x where its marks begin in the bone buffer, y how many, z its wear over all, w its snow (-1: the weather's)
};
#ifdef CARD_SCENERY
// [gpuscenery] the still scenery picked on the card (card_scenery.h): this draw is one level's list there. The card's buffer
// in the bones' place (scenery stands at rest and wears no marks: nothing of bones is read), its slots eight rows a prop
// (the instance's six first), and this draw's record: its arguments, then where its list begins.
cbuffer CardDraw : register(b2) {
  uint4 cardDraw;  // x: the word its record begins at in the card's buffer
};
StructuredBuffer<float4> boneRows : register(t0, space1);
uint CardWord(uint w) { return asuint(boneRows[w >> 2][w & 3]); }
Instance CardInstance(uint instance) {
  uint slot = CardWord(CardWord(cardDraw.x + 5) + instance) * 8;
  Instance n;
  n.row0 = boneRows[slot];
  n.row1 = boneRows[slot + 1];
  n.row2 = boneRows[slot + 2];
  n.color = boneRows[slot + 3];
  n.bones = boneRows[slot + 4];
  n.wear = boneRows[slot + 5];
  return n;
}
#else
cbuffer Instances : register(b2) {
  Instance instances[640];
};
StructuredBuffer<float4> boneRows : register(t0, space1);
#endif
cbuffer MeshKind : register(b3) {
  float4 kind;  // x: alpha not above this is cut away (-1: glass, seen through by its alpha and glow), y: 1 = lit by the mesh's normals only (2: a frame of light,
                // 3: a shield's bubble, 4: a slab that fades, 5: a nuke's ball of fire, 6: a nuke's sliding
                // shell, 7: its texture's own colours, 8: a quantum gate's field, 9: a zapper's ball, 10 and 11:
                // a shield's flash where it is hit, 12: a personal shield's shell; none of them is lit),
                // z: 1 = the colour multiplies the albedo (0: it replaces it where the team mask says),
                // w: 1 = the specular texture is a remastered unit's material
  float4 kind2; // x: whose look the textures were painted for: 0 UEF and all else, 1 Aeon, 2 Cybran, 3 Seraphim;
                // y: 1 = leaves (ModelLod::leaves); z: 1 = a remastered level without its material texture;
                // w: 1 = the map's border (ModelLod::border)
  float4 kind3; // x: how much it sways in the wind (0: not at all); y: 1 / its height in model units;
                // z: the model's lowest point and w: 1 / its whole height, in model units (for building)
  float4 kind4; // x: 1 = wreckage, 2 = liquid metal (the Aeon's pool) (a nuke's ball of fire: 1 = the remastered
                // look's, from the visuals track's textures); y, z, w: a shield's tint (a nuke's shell: how
                // often its texture repeats, how fast it slides, from what age it fades)
  float4 kind5; // x: 1 = the Cybran's highlight lookup is there; y: 1 = the Seraphim's mirror ramp is there;
                // w: 1 = the visuals track's lattice for this faction's personal shields is there (shieldPatterns)
  float4 kind6; // x: 1 = its texture's treads scroll (ModelLod::scrolling: the original's anim.w), 2 + d: and its albedo's
                // alpha holds their height there, d world units deep (ModelLod::treadDepth); y: 1 = no glow in the
                // frame's alpha (ModelLod::rgbOnly: the original writes its colour alone); z: what sways, a group of
                // trees: model units a step of where each tree stands (ModelLod::treeStep; 0: none); w: the scene's
                // samples a pixel (NearFade)
  float4 kind12;  // [machinery] a factory's belt (machinery.h): x where its strip of the texture begins along its axis, y how
                  // long it is, z the bone whose vertices are the belt (-1: none), w its axis (0: u, 1: v)
#ifdef PLANTS
  float4 kind7, kind8;  // [plants] (plants.h) PlantLife::Constants
#endif
#ifdef UNDER_SNOW
  float4 kind9, kind10, kind11;  // [undersnow] (under_snow.h) the lying snow over the low plants: its k0, k1, k2
#endif
};
Texture2D albedoMap : register(t0);
Texture2D normalsMap : register(t1);
Texture2D specularMap : register(t2);
TextureCube environmentMap : register(t3);
Texture2D secondaryMap : register(t4);
Texture2D insectLookup : register(t5);
Texture2D seraphimFalloff : register(t6);
// personal shields' lattice (the visuals track's, tiling: R the lattice, G drifting patches, B a value per
// cell), a layer per faction: 0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim
Texture2DArray shieldPatterns : register(t7);

struct VIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float4 uv : TEXCOORD0;
  uint4 bones : BLENDINDICES;
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float4 uv : TEXCOORD1;
  float4 color : COLOR;
  float3 tint : TEXCOORD2;
  float3 shape : TEXCOORD3;  // the place on the model in heights of the model; y: 0 at its foot .. 1 at its top
  // the place on the model and its surface's way at rest, in world units: what is painted on the model
  // (a personal shield's lattice) goes with its bones
  float3 rest : TEXCOORD4;
  float3 restNormal : TEXCOORD5;
  nointerpolation float4 wear : TEXCOORD6;  // (the instance's: unit_wear.h)
  nointerpolation float belt : TEXCOORD10;   // [machinery] how far its belt has moved along its strip (-1: not on a belt)
#ifdef PLANTS
  nointerpolation float4 plants : TEXCOORD7;  // [plants] how it is burnt (PlantsBurnLook: black, brown, lower, embers)
#endif
#ifdef UNDER_SNOW
  float snowAbove : TEXCOORD8;                  // [undersnow] its height over the snow's top (under 0: under the ground)
  nointerpolation float3 snowHold : TEXCOORD9;  // [undersnow] x its share of the snow (-1: not under it), y what stands out, z
                                                // how much of the ground at its foot the snow covers
#endif
};

VOut VS(VIn i, uint instance : SV_InstanceID) {
#ifdef CARD_SCENERY
  Instance n = CardInstance(instance);  // [gpuscenery]
#else
  Instance n = instances[instance];
#endif
  float4 p = float4(i.position, 1);
  float3 normal = i.normal, tangent = i.tangent, binormal = i.binormal;
  [branch] if (n.bones.x >= 0) {  // (a real branch: a model at rest must not read the buffer at all)
    uint at = (uint)n.bones.x + i.bones.x * 3;
    float4 b0 = boneRows[at], b1 = boneRows[at + 1], b2 = boneRows[at + 2];
    float3x3 turn = float3x3(b0.xyz, b1.xyz, b2.xyz);
    p = float4(dot(b0, p), dot(b1, p), dot(b2, p), 1);
    normal = mul(turn, normal);
    tangent = mul(turn, tangent);
    binormal = mul(turn, binormal);
  }
  VOut o;
#ifdef PLANTS
  o.plants = 0;
#endif
  o.rest = i.position * length(n.row0.xyz);
  o.restNormal = i.normal;
  o.shape = float3(p.x, p.y - kind3.z, p.z) * kind3.w;
  [branch] if (n.color.a < 0.999 && kind.z < 0.5) {  // a unit under construction
    int whose = int(kind2.x + 0.5);
    if (whose == 1) {  // Aeon: out of the ground, a little smaller at first
      p.xyz *= max(0.75, n.color.a);
      p.y -= 0.8 * (1 - min(1, 1.25 * n.color.a)) / kind3.w;  // (its top shows from the start)
    } else if (whose == 3) {  // Seraphim: it grows
      p.xyz *= 0.25 + 0.75 * n.color.a;
    }
  }
  o.world = float3(dot(n.row0, p), dot(n.row1, p), dot(n.row2, p));
  [branch] if (kind3.x > 0) {
    // the wind: the higher on the model, the farther it is carried; every tree on its own beat (by
    // where it stands; a tree of a group by its own place too), with a slow swing and a quicker one
    // over it; harder when it rains
    float high = saturate(p.y * kind3.y);
    float size = length(float3(n.row0.x, n.row1.x, n.row2.x)) / kind3.y;  // its height in the world
    float2 foot = float2(n.row0.w, n.row2.w);
    float beat = dot(foot, float2(0.11, 0.07));
    // [foliage] a tree of a group (several trees in one model, one bone a tree; ModelLod::treeStep): where it stands
    // itself, and a beat of its own on top of that place's
    [branch] if (i.bones.w > 0 && kind6.z > 0 && look > 0.5) {  // (the modern look; the original's swing together)
      float2 own = (float2(i.bones.yz) - 128) * kind6.z;  // (model units)
      foot = float2(dot(n.row0, float4(own.x, 0, own.y, 1)), dot(n.row2, float4(own.x, 0, own.y, 1)));
      beat = dot(foot, float2(0.11, 0.07)) + (i.bones.w - 1) * (6.2831853 / 254);
    }
    float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7 + p.x * kind3.y * 4) * 0.4;
    // (it comes in gusts that run across the land with the wind)
    float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
#ifdef PLANTS
    // [plants] a tree the game has felled (lying: its up turned away from the sky) no longer sways
    o.world.xz += float2(0.915, 0.403) * (swing * gust * kind3.x * high * high * size * 0.035) *
                  PlantsUpright(n.row1.y / max(length(float3(n.row0.y, n.row1.y, n.row2.y)), 1e-6));
#else
    o.world.xz += float2(0.915, 0.403) * (swing * gust * kind3.x * high * high * size * 0.035);
#endif
#ifdef BLASTS
    // a blast's wave (blasts.h): bent away from it, the more toward the top, then swaying back
    o.world.xz += BlastPush(foot, 1) * (kind3.x * high * high * size);
#endif
#ifdef PLANTS
    // [plants] what is low (a bush, a fern, a sapling: under 2.2 world units) is pressed over by the units driving through it,
    // the way they push it, and springs back past upright once after they have gone (a third of a second later); burnt
    // where the ground is (its own share of a half burnt place), and lower
    [branch] if (size < 2.2 && kind8.z + kind8.w > 0.5) {  // (and only while something is pressed or burnt)
      float3 root = float3(n.row0.w, n.row1.w, n.row2.w);
      float4 pressed = PlantsPressAt(root.xz, kind7, kind8);
      [branch] if (pressed.z > 0.01) {
        float since = max(pressed.w / pressed.z - 0.3, 0);
        float over = 1.25 * pressed.z * exp(-since / 0.6) * cos(since * 2.4) * (0.45 + 0.55 * high);
        float2 way = pressed.xy / pressed.z;
        way = length(way) > 0.05 ? normalize(way) : float2(0.6, 0.8);
        float rise = max(o.world.y - root.y, 0);
        o.world.xz += way * (rise * sin(over));
        o.world.y -= rise * (1 - cos(over));
      }
#ifndef PLANTS_SHADOW  // (the sun's maps: bent, not burnt; lighter, the shadows being drawn four times over)
      o.plants = PlantsBurnLook(root.xz, kind7, kind8, CloudHash(float3(root.xz, 1.3)));
      o.world.y -= max(o.world.y - root.y, 0) * (o.plants.z * 0.45);
#endif
    }
#endif
  }
#ifdef PLANTS
  // [plants] the game's burnt tree (its burnt mesh: wreckage, with leaves): charred through
  if (kind4.x > 0.5 && kind4.x < 1.5 && kind2.y > 0.5) o.plants = float4(1, 0, 0, 0);
#endif
  o.position = mul(viewProj, float4(o.world, 1));
  // (a personal shield's shell lies close over its unit, and the unit's details stand out of it here and
  // there: it is tested as if a world unit and a half nearer the eye, along the line of sight, so that its
  // patches show over what lies just under it; where it is drawn on the screen does not change)
  [branch] if (kind.y > 11.5) o.position = mul(viewProj, float4(o.world + normalize(cameraPosition - o.world) * 1.5, 1));
  float3x3 r = float3x3(n.row0.xyz, n.row1.xyz, n.row2.xyz);
  o.normal = mul(r, normal);
  o.tangent = mul(r, tangent);
  o.binormal = mul(r, binormal);
  o.uv = i.uv;
  o.color = n.color;
  o.wear = n.wear;
  o.tint = n.bones.yzw;
  o.belt = -1;
  [branch] if (n.bones.y < -0.5) {
    // [machinery] (y -2: a factory whose belt moves, z how far: its vertices on the belt's bone)
    if (n.bones.y < -1.5 && kind12.z >= 0 && i.bones.x == (uint)(kind12.z + 0.5)) o.belt = n.bones.z;
    // a tank's treads, scrolled as it drives, as the original's ComputeScrolledTexcoord does it: the two
    // strips at the foot of its texture, each by its tread's amount (bones.z, bones.w), across both sets;
    // where the level's mesh says its treads scroll
    float scroll = kind6.x < 0.5 ? 0 : i.uv.y > 0.95 ? n.bones.z : i.uv.y > 0.90 ? n.bones.w : 0;
    o.uv.xz += scroll;
    o.tint = 0;  // (no ghost's tint)
  }
#ifdef UNDER_SNOW
  // [undersnow] a low plant under lying snow (under_snow.h): sunk into it (the ground hides what goes under), pressed and
  // bowed by it; buried whole where little of it would stand out (all its corners behind the eye)
  o.snowAbove = 1e4;
  o.snowHold = float3(-1, 0, 0);
  [branch] if (kind3.x > 0) {
    float3 root = float3(n.row0.w, n.row1.w, n.row2.w);
    float scale = length(float3(n.row0.x, n.row1.x, n.row2.x));
    float top = scale / kind3.y;
    float4 plant;
    float3 ground;
    float covered;
    [branch] if (UnderSnowPlant(root, top, scale / kind3.w, kind9, kind10, kind11, plant, ground, covered)) {
      if (plant.w > 0.5) {
        o.position = float4(0, 0, -1, 1);
        return o;
      }
      float turn = CloudHash(root * 1.7 + 0.3) * 6.2831853;
      o.world = UnderSnowCorner(o.world, root, top, plant, ground, float2(cos(turn), sin(turn)), o.normal, o.tangent, o.binormal, o.snowAbove);
      o.snowHold = float3(plant.y, max(top * plant.z - plant.x, 1e-3), covered);
      o.position = mul(viewProj, float4(o.world, 1));
    }
  }
#endif
  return o;
}

// A unit under construction: how much of the finished surface shows at a place of the model
// (0..1; below 0: nothing is there yet), and the light of the work itself.
float BuildSolid(int faction, float f, float3 shape, out float3 work) {
  work = 0;
  if (faction == 1) return saturate((f - 0.75) * 4);  // Aeon: liquid metal until it sets
  if (faction == 3) {  // Seraphim: a glowing form that takes its surface
    work = float3(0.45, 0.7, 1.0) * (1 - f);
    return saturate((f - 0.5) * 2);
  }
  if (faction == 2) {  // Cybran: plate by plate
    float since = f * 1.08 - CloudHash(floor(shape * 6) + 3.1);  // above 0: this plate is in
    if (since > 0) {
      work = float3(1.0, 0.12, 0.05) * (saturate(1 - since / 0.04) * 1.2);
      return 1;
    }
    float3 g = abs(frac(shape * 7 + shape.yzx * 7) - 0.5);
    float wire = 1 - smoothstep(0.0, 0.05, min(g.x, min(g.y, g.z)));
    work = float3(1.0, 0.12, 0.05) * (wire * (1.5 + 0.5 * sin(time * 5 + shape.y * 30)));
    return wire > 0.3 ? 0 : -1;
  }
  // the UEF, and whatever else: from the top down, as the game's own box of light sinks round what is
  // not there yet and its slices of light mark the level of the work
  float level = f * 1.04 - 0.02, down = 1 - shape.y;
  if (down <= level) {
    work = float3(0.25, 0.6, 1.0) * (saturate(1 - (level - down) / 0.035) * 3);
    return 1;
  }
  float3 g = abs(frac(shape * 10) - 0.5);
  float wire = 1 - smoothstep(0.0, 0.07, min(g.x, min(g.y, g.z)));
  float band = saturate(1 - abs(frac(shape.y * 0.5 - time * 0.25) - 0.5) * 6);
  work = float3(0.2, 0.55, 1.0) * (wire * (0.8 + 1.5 * band * band));
  return wire > 0.3 ? 0 : -1;
}
#ifdef REMASTERED_BUILD
// (buildfx: the remastered build look's in its place, build_effects.h, kBuildShaderBlock; with the place on the model at rest)
#define BuildSolid(whose, f, shape, work) RemasteredBuildSolid(whose, f, shape, i.rest, work)
#endif

// [foliage] Foliage right at the eye (a cinematic camera low among trees, cinecam.h): its cards would fill the screen
// as flat sheets, so they fade out, dithered, from 1.2 world units to nothing under 0.4. Only what sways and has
// holes (leaves, needles, plants' cards; never a unit or a solid prop; a tree's trunk is in its foliage's mesh and goes
// with it); the game's camera comes that close only zoomed all the way in among trees (ambient_occlusion.cpp leaves
// that zone out too). `coverage`: the caller's alpha is a leaf's coverage (alpha to coverage); with several samples
// a pixel that coverage is multiplied by what is returned, and the samples dither it finely; otherwise whole pixels
// are cut, dithered across the screen. The scene's depth first and its full shading drop the same; the shadows keep
// them all.
float NearFade(float3 world, float2 pixel, bool coverage) {
  [branch] if (kind3.x > 0 && kind.x > 0) {
    float d = length(world - cameraPosition);
    [branch] if (d < 1.2) {
      float fade = smoothstep(0.4, 1.2, d);
      if (coverage && kind6.w > 1.5) return fade;
      float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));  // (interleaved gradient noise)
      if (fade <= noise) discard;
    }
  }
  return 1;
}

#ifdef PLANTS
// [plants] What fire leaves of leaves and needles: those that are green (not bark, not twigs) go by patches over the model,
// the more the harder it is burnt (`burnt`: PlantsBurnLook's). True: gone here.
bool PlantsBurntAway(float4 albedo, float3 shape, float4 burnt) {
  float burn = saturate(burnt.x + burnt.y * 0.7);
  [branch] if (burn < 0.02) return false;
  float patch = CloudNoise(shape * 24 + 3.7) * 0.6 + CloudNoise(shape * 57 + 1.9) * 0.4;  // (about a bough's size, and finer)
  return albedo.g > albedo.r * 1.08 + 0.01 && patch < 0.2 + 0.32 * burn;
}
#endif

// [treads] Where a remastered unit's treads lie on the strips at the foot of its texture with their height in its albedo's
// alpha (1 the surface, 0 a groove's floor; ModelLod::treadDepth), the eye looks into their grooves: the ray from the eye
// walked down into the height (a step every 2 texels the shift crosses, 4 to 32, then between the last two), the texture's
// place shifted to where it meets it. The surface's frame from the screen's derivatives (g: the texture place's, dp1 and
// dp2: the world place's), so that any layout of the strips reads right; the shift kept inside the strip the pixel is on.
// `depth`: world units a groove.
// Gives the shift (xy) and how far down the ray went (z: 0 the surface, 1 a groove's floor; walls and floors darker).
float3 TreadShift(float2 uv, float4 g, float3 dp1, float3 dp2, float3 N, float3 toCamera, float depth) {
  float3 p2 = cross(dp2, N), p1 = cross(N, dp1);
  float det = dot(N, cross(dp1, dp2));
  float vn = dot(toCamera, N);
  if (abs(det) < 1e-14 || vn < 0.05) return float3(0, 0, 1 - albedoMap.SampleGrad(samplerAniso, uv, g.xy, g.zw).a);
  float3 gu = (p2 * g.x + p1 * g.z) / det, gv = (p2 * g.y + p1 * g.w) / det;  // (u's and v's gradients over the surface)
  float3 walk = -(toCamera - N * vn) / max(vn, 0.25) * depth;  // (along the surface, down to a groove's floor)
  float2 full = float2(dot(gu, walk), dot(gv, walk));
  float lo = uv.y > 0.95 ? 0.95 : 0.90;
  float2 band = float2(lo + 0.0005, lo + 0.0495);
  full.y = clamp(uv.y + full.y, band.x, band.y) - uv.y;
  float2 size;
  albedoMap.GetDimensions(size.x, size.y);
  const float steps = clamp(ceil(length(full * size) * 0.5), 4, 32);
  float d = 0, below = 1 - albedoMap.SampleGrad(samplerAniso, uv, g.xy, g.zw).a;
  float lastD = 0, lastBelow = below;
  [loop] for (int k = 0; k < 32 && k < steps && d < below; ++k) {
    lastD = d;
    lastBelow = below;
    d += 1 / steps;
    below = 1 - albedoMap.SampleGrad(samplerAniso, uv + full * d, g.xy, g.zw).a;
  }
  float over = lastBelow - lastD, under = d - below;  // (the ray above the height at the last step, below it at this one)
  float reached = lerp(lastD, d, saturate(over / max(over + under, 1e-5)));
  return float3(full * reached, reached);
}

// shadow maps: depth only, with foliage's holes (and nothing where a unit is not built yet)
void ShadowPS(VOut i) {
  if (kind.x > 0) clip(albedoMap.Sample(samplerLinear, i.uv.xy).a - kind.x);
#if defined(PLANTS) && (!defined(PLANTS_SHADOW) || defined(PLANTS_SHADOW_BURNT))
  // [plants] (the game's burnt trees: their shadows by a variant of their own, PLANTS_SHADOW_BURNT; the code here would
  // slow the shadows of everything else that sways)
  [branch] if (i.plants.x + i.plants.y > 0.02) {
    if (PlantsBurntAway(albedoMap.Sample(samplerAniso, i.uv.xy), i.shape, i.plants)) discard;
  }
#endif
  if (i.color.a < 0.999 && kind.z < 0.5 && kind2.w < 0.5) {
    float3 work;
    clip(BuildSolid(int(kind2.x + 0.5), i.color.a, i.shape, work) - 0.5);
  }
}

// the scene's depth laid first (MeshRenderer::depthPrepass): only the holes the full shading cuts
// (leaves' soft edges with the same coverage, a cut-out's alpha, what is not built yet)
float4 DepthPS(VOut i) : SV_Target {
  float4 albedo = albedoMap.Sample(samplerAniso, i.uv.xy);
  float cover = 1;
  if (kind2.y > 0.5) {
    cover = saturate((albedo.a - kind.x) / max(fwidth(albedo.a), 1e-4) + 0.5);
    clip(cover - 0.02);
  } else if (kind.x > 0) {
    clip(albedo.a - kind.x);
  }
  cover *= NearFade(i.world, i.position.xy, kind2.y > 0.5 && look > 0.5 && kind.y < 0.5);  // [foliage] (as Shade's)
#ifdef PLANTS
  if (PlantsBurntAway(albedo, i.shape, i.plants)) discard;  // [plants] (as Shade's)
#endif
  if (min(i.color.a, 1) < 0.999 && kind.z < 0.5 && kind2.w < 0.5) {
    float3 work;
    clip(BuildSolid(int(kind2.x + 0.5), min(i.color.a, 1), i.shape, work));
  }
  return float4(0, 0, 0, cover);
}

float4 Shade(VOut i, bool front) {
  // (a finished unit whose personal shield is up says so with an alpha above 1)
  bool shielded = i.color.a > 1.5;
  // (a hit on its shield this many seconds ago, below 1; i.tint: where it struck on the model at rest)
  float hitAge = shielded && i.color.a > 3.0005 ? (i.color.a - 3.001) / 0.9 : -1;
  i.color.a = min(i.color.a, 1);
  if (inspect.x > 8.5 && inspect.x < 9.5 && look > 0.5) {
    // a view for checking art: which level of detail each model is drawn at: white the finest,
    // then yellow, orange, red, violet; shaded a little by which way the surface faces
    float level = floor(kind5.z);  // [grime] (its quarters say which wear art the level has)
    float3 mark = level < 0.5 ? float3(1, 1, 1) : level < 1.5 ? float3(1, 0.9, 0.25) : level < 2.5 ? float3(1, 0.55, 0.15) : level < 3.5 ? float3(1, 0.2, 0.15) : float3(0.7, 0.3, 1);
    return float4(mark * (0.45 + 0.55 * saturate(normalize(i.normal).y * 0.5 + 0.5)) * (modern.x * lightMultiplier) * 0.6, 1);
  }
  if (kind.y > 11.5) {
    // a personal shield's shell (ModelLod::shell: a mod's Display.PersonalShield): nothing but round its
    // hits. Each lights a patch of its lattice (the albedo: RGB its colour, A how much shows) round where
    // it struck, with a white-hot middle and a faint ring going out, and fades within its second. The
    // instance's colour says where its hits are in the bone buffer (x) and how many (y): each where it
    // struck on the unit at rest (world units) and its age (0..1).
    float3 q = i.rest * (1.0 / 3.84);  // (the lattice: 3.84 world units a tile, sixteen cells across)
    float3 w = pow(abs(normalize(i.restNormal)), 4);
    w /= w.x + w.y + w.z;
    float4 lattice = albedoMap.Sample(samplerAniso, q.zy) * w.x + albedoMap.Sample(samplerAniso, q.xz) * w.y +
                     albedoMap.Sample(samplerAniso, q.xy) * w.z;
    float3 c = 0;
    uint first = (uint)(i.color.x + 0.5), count = (uint)(i.color.y + 0.5);
    for (uint k = 0; k < count; ++k) {
      float4 hit = boneRows[first + k];
      float d = length(i.rest - hit.xyz);
      float fade = saturate(1 - hit.w);
      fade *= fade;
      float patch = saturate(1 - d / 3.9);  // (some 26 of the visuals track's model units round it)
      patch = patch * patch * (3 - 2 * patch);
      float ring = saturate(1 - abs(d - lerp(2.6, 3.6, hit.w)) / 0.3);  // (going out as it fades)
      float core = saturate(1 - d / 0.6);
      c += lattice.rgb * (lattice.a * (patch * 3.0 + ring * 1.5) + patch * 0.6) * fade + float3(1.0, 0.92, 0.88) * (core * core * 5 * fade);
    }
    float dark = falling.w > 0 ? falling.w : 1;
    // (the pipeline adds what is returned; its alpha is the original look's glow, for its bloom)
    // (a glow of its own: strong, as the bright red disc of the visuals track's pictures; its lines kept from blowing out)
    return float4(look > 0.5 ? pow(clamp(c, 0, 2), 2.2) * 10 * dark : c, saturate(max(c.r, max(c.g, c.b)) * 0.5));
  }
  if (kind.y > 9.5) {
    // the flash on a shield where it is hit (the original's "ShieldImpact" and its Cybran kind,
    // after what its shader computes): a patch of the bubble turned toward the hit, its age in
    // seconds in the instance's colour. It lives two seconds; the flash is its textures sliding.
    float age = i.color.r;
    float dark = falling.w > 0 ? falling.w : 1;
    float3 c;
    float alpha;
    if (kind.y > 10.5) {
      // the Cybran's: its one texture read three times: still, sliding, and moved sideways by a
      // different step for each hit; laid over what is behind it
      float4 still = specularMap.Sample(samplerAniso, i.uv.xy);
      float4 sliding = specularMap.Sample(samplerAniso, i.uv.xy + float2(-0.003, -0.06) * age);
      float4 moved = specularMap.Sample(samplerAniso, i.uv.xy + float2(frac(i.color.g), 0));
      c = sliding.rrr + still.g;
      alpha = saturate(sliding.b * moved.r * 4.5 * saturate(1 - (age - 6) * 0.15) * still.a);
      return float4(look > 0.5 ? pow(max(c, 0), 2.2) * 3.0 * dark : c, alpha);
    }
    // the others': a ring pattern (the albedo's green) sliding outward, slower as it goes, through
    // a still coarse pattern and the patch's own outline; pale blue, adding its light
    float4 ring = albedoMap.Sample(samplerAniso, i.uv.xy + float2(-0.003 * age, -0.15 + lerp(-0.1, -0.085, age * 0.5) * age));
    float4 coarse = specularMap.Sample(samplerAniso, i.uv.xy * 0.25);
    float outline = specularMap.Sample(samplerAniso, i.uv.xy).a;
    c = float3(0, 0, 0.5) + coarse.r;
    alpha = saturate(outline * ring.g * (coarse.r + coarse.g) * saturate(1 - (age - 2) * 0.2));
    // (the pipeline adds what is returned as it is)
    // (four times over in the modern look: it is a flash of light, and among bubbles lying over
    // each other a weaker one was not to be found)
    return float4((look > 0.5 ? pow(max(c, 0), 2.2) * 4.0 * dark : c) * alpha, 1);
  }
  if (kind.y > 6.5) {
    // not lit by the scene: what the textures say, as bright as the hour lets a thing be seen
    float seen = falling.w > 0 ? falling.w : 1;
    float4 own = albedoMap.Sample(samplerAniso, i.uv.xy);
    float3 c;
    float alpha = 1;
    if (kind.y > 8.5) {
      // a zapper's ball: two textures sliding over it at different speeds, faint
      float4 first = albedoMap.Sample(samplerAniso, i.uv.xy + float2(-0.05, 0) * time);
      float3 second = normalsMap.Sample(samplerAniso, i.uv.xy + float2(-0.05, -0.025) * time).rgb;
      c = (first.rgb + second) * first.a;
      alpha = 0.2;
    } else if (kind.y > 7.5) {
      // a quantum gate's field: its texture sliding upward, a four times finer second one sliding
      // across taken away from it; seen through, as much as the texture's alpha says at rest
      float3 field = albedoMap.Sample(samplerAniso, i.uv.xy + float2(0, 0.025) * time).rgb;
      float3 fine = normalsMap.Sample(samplerAniso, i.uv.xy * 4 + float2(0.025, 0.025) * time).rgb;
      c = max(field - fine, 0);
      alpha = 0.4 * own.a;
    } else {
      clip(own.a - kind.x);
      c = own.rgb;
    }
    return float4(look > 0.5 ? pow(max(c, 0), 2.2) * 1.3 * seen : c, alpha);
  }
  if (kind.y > 4.5) {
    // a nuclear explosion's ball of fire; its age in seconds comes as the instance's colour
    float age = i.color.r;
    float3 fire;
    float alpha;
    if (kind.y > 5.5) {
      // a shell: its texture sliding along it, thinned by its second texture, fading late
      float4 shell = albedoMap.Sample(samplerAniso, i.uv.xy * kind4.y + float2(0, age * kind4.z));
      fire = shell.rgb;
      alpha = shell.a * normalsMap.Sample(samplerAniso, i.uv.xy).g * saturate(1 - (age - kind4.w) * 0.025);
    } else if (kind4.x > 0.5) {
      // the remastered look's (visuals, section 25): the visuals track's picture of fire as light of its
      // own, not greyed by the noise; its second texture's three noises (red, green, blue) at the
      // original's sizes and creeping at the original's speeds bend where the picture is read, so it
      // churns as the original does. The picture's alpha and the original's fade from 50 seconds and
      // cut make its ragged rim behave the same.
      float creep = age * 0.018;
      float4 fine = normalsMap.Sample(samplerAniso, i.uv.xy * 6 + float2(0, creep * 1.25));
      float4 middle = normalsMap.Sample(samplerAniso, i.uv.xy * 4 + float2(0, creep * 1.25));
      float4 wide = normalsMap.Sample(samplerAniso, i.uv.xy * 0.25 + float2(0, creep * 0.1));
      float2 bend = (float2(middle.g, fine.b) - 0.5) * 0.05 + (float2(wide.r, wide.b) - 0.5) * 0.15;
      float4 picture = albedoMap.Sample(samplerAniso, i.uv.xy + bend + float2(0, creep * 0.1));
      fire = picture.rgb;
      alpha = picture.a * saturate(1 - (age - 50) * 0.015);
      clip(alpha - 15.0 / 255);
      fire = saturate(fire);
      // (bright enough that its hottest parts glow past what the screen shows: the picture's last pass blooms them)
      return float4(look > 0.5 ? pow(fire, 2.2) * 4 : fire, alpha);
    } else {
      // churning fire: the fire's picture mixed half and half with its second texture's noise at
      // three sizes, which creeps along the ball; holes where the fine noise is thin; after 20
      // seconds the noise eats away what the picture's alpha does not hold, and from 50 it fades
      float creep = age * 0.018;
      float4 picture = albedoMap.Sample(samplerAniso, i.uv.xy);
      float4 fine = normalsMap.Sample(samplerAniso, i.uv.xy * 6 + float2(0, creep * 1.25));
      float4 middle = normalsMap.Sample(samplerAniso, i.uv.xy * 4 + float2(0, creep * 1.25));
      float4 wide = normalsMap.Sample(samplerAniso, i.uv.xy * 0.25 + float2(0, creep * 0.1));
      float churn = ((middle.g + fine.b) * 0.5 + wide.b) * 0.5 + wide.r * 0.5;
      fire = (lerp(float3(churn, churn, churn), picture.rgb, 0.5) + fine.b * 0.3) * middle.g + fine.b;
      alpha = picture.a * saturate(1 - (age - 50) * 0.015);
      if (age >= 20 && fine.a * middle.b * fine.g >= picture.a) alpha = 0;
      alpha *= fine.g;
      clip(alpha - 15.0 / 255);
    }
    fire = saturate(fire);
    return float4(look > 0.5 ? pow(fire, 2.2) * 2 : fire, alpha);
  }
  if (kind.y > 3.5) {
    // a slab of light that fades (the UEF's build slices): its texture's colour, see-through by
    // the texture's alpha and by how much of it is left (the instance says: its age's doing)
    float4 slab = albedoMap.Sample(samplerAniso, i.uv.xy);
    // (weak, and its lilac taken half way to a pale blue: several lie over each other at the level
    // of the work, and what is being built must read through them - Chris, 2026-10-13)
    float3 pale = lerp(slab.rgb, float3(0.55, 0.75, 1.0), 0.5);
    float3 glow = look > 0.5 ? pow(max(pale, 0), 2.2) * 1.3 * (falling.w > 0 ? falling.w : 1) : pale;
    return float4(glow, slab.a * saturate(i.color.a) * 0.28);
  }
  if (kind.y > 2.5) {
    // [ogshield] the original look: each faction's bubble as the game draws it (OriginalShield)
    [branch] if (look < 0.5)
      return OriginalShieldLook(int(kind2.x + 0.5), i.uv.xy, normalize(i.normal), normalize(i.tangent), normalize(i.binormal),
                                normalize(i.world - cameraPosition), time, 1, false, albedoMap, normalsMap, specularMap, secondaryMap,
                                environmentMap);  // (original_shield.h)
    // a shield's bubble: a faint shell in its faction's tint, stronger where it is seen edge on,
    // with its texture's pattern drifting over it at two sizes against each other
    float rim = pow(1 - abs(dot(normalize(i.normal), normalize(cameraPosition - i.world))), 2.5);
    float pattern = saturate(albedoMap.Sample(samplerLinear, i.uv.xy * 3 + float2(0.01, 0.03) * time).r *
                             albedoMap.Sample(samplerLinear, i.uv.xy * 7 - float2(0.02, 0.013) * time).g * 2.5);
    // (by night much less of its own light, and less of it in the way: an eye opened for the dark
    // would otherwise see a white dome and nothing in it)
    float dark = falling.w > 0 ? falling.w : 1;
    float3 tint = kind4.yzw * (dark * lerp(0.35, 1, dark));
    return float4(look > 0.5 ? tint * (0.8 + 2.0 * rim + 1.2 * pattern) : tint, saturate(0.05 + 0.45 * rim + 0.18 * pattern) * lerp(0.55, 1, dark));
  }
  if (kind.y > 1.5) {
    // a frame of light (the UEF's box round what it builds): see-through and blue, its texture's
    // clouds drifting over it as what can be seen through, fine lines running along it
    float2 drift = i.uv.xy + float2(0.012, 0.062) * time;
    float cloud = albedoMap.Sample(samplerLinear, drift * 0.35).a;
    float2 g = abs(frac(drift * 6) - 0.5);
    float wire = 1 - smoothstep(0.0, 0.05, min(g.x, g.y));
    float3 light = lerp(float3(0.08, 0.42, 0.95), float3(0.45, 0.8, 1.0), wire) * (falling.w > 0 ? falling.w : 1);
    return float4(look > 0.5 ? light * (0.6 + 1.2 * wire) : light, saturate(0.28 * cloud + 0.35 * wire));
  }
  float4 albedo, spec, secondary;
  float2 nxy;
  float2 treadShift = 0;
  float treadDown = 0;
  [branch] if (kind6.x > 1.5 && look > 0.5) {  // [treads] (the whole draw's: a remastered unit's treads with their height)
    float4 g = float4(ddx(i.uv.xy), ddy(i.uv.xy));
    float3 dp1 = ddx(i.world), dp2 = ddy(i.world);
    [branch] if (i.uv.y > 0.90) {
      float3 t = TreadShift(i.uv.xy, g, dp1, dp2, normalize(i.normal), normalize(cameraPosition - i.world), kind6.x - 2);
      treadShift = t.xy;
      treadDown = t.z;
    }
    i.uv += treadShift.xyxy;
    albedo = albedoMap.SampleGrad(samplerAniso, i.uv.xy, g.xy, g.zw);
    nxy = normalsMap.SampleGrad(samplerAniso, i.uv.zw, g.xy, g.zw).ga * 2 - 1;
    spec = specularMap.SampleGrad(samplerAniso, i.uv.xy, g.xy, g.zw);
    secondary = secondaryMap.SampleGrad(samplerAniso, i.uv.xy, g.xy, g.zw);
    if (i.uv.y > 0.90) {
      albedo.a = 1;  // (its height, not a hole or a glow)
      albedo.rgb *= 1 - 0.6 * treadDown;  // (down in a groove, less light reaches: its walls and floor darker)
    }
  } else {
    albedo = albedoMap.Sample(samplerAniso, i.uv.xy);
    nxy = normalsMap.Sample(samplerAniso, i.uv.zw).ga * 2 - 1;
    spec = specularMap.Sample(samplerAniso, i.uv.xy);
    secondary = secondaryMap.Sample(samplerAniso, i.uv.xy);  // [grime] (all of it: a unit's masks, unit_grime.h)
  }
  float occlusion = secondary.r;
  if (kind2.z > 0.5) spec = float4(0, 0.85, 0, kind2.y > 0.5 ? 0.35 : 0);  // rough, no metal; leaves let some light through
  // holes: cut where the alpha is not above the mark; leaves get a soft edge a pixel wide instead,
  // which the samples of a pixel turn into a smooth one (alpha to coverage)
  float cover = 1;
  if (kind2.y > 0.5) {
    cover = saturate((albedo.a - kind.x) / max(fwidth(albedo.a), 1e-4) + 0.5);
    clip(cover - 0.02);
  } else if (kind.x > 0) {
    clip(albedo.a - kind.x);
  }
  // [foliage] (a leaf's coverage is its alpha below in the modern look, unless lit by its normals alone)
  cover *= NearFade(i.world, i.position.xy, kind2.y > 0.5 && look > 0.5 && kind.y < 0.5);
#ifdef PLANTS
  if (PlantsBurntAway(albedo, i.shape, i.plants)) discard;  // [plants]
#endif
  float3 vertexNormal = normalize(i.normal);
  if (kind2.y > 0.5 && !front) {  // a leaf seen from behind is lit as from behind
    vertexNormal = -vertexNormal;
    i.tangent = -i.tangent;
    i.binormal = -i.binormal;
  }
  float3 n = float3(nxy, sqrt(saturate(1 - dot(nxy, nxy))));
  float3 N = kind.y > 0.5 ? vertexNormal : normalize(n.x * normalize(i.binormal) + n.y * normalize(i.tangent) + n.z * vertexNormal);
  float shadow = SunShadow(i.world, vertexNormal);
  int faction = int(kind2.x + 0.5);
  float teamMask = kind.z > 0.5 ? 0 : (faction == 3 ? albedo.a : spec.a);
  // under construction: by its faction (BuildSolid); what has no team mask is only darker
  bool building = i.color.a < 0.999 && kind.z < 0.5 && kind2.w < 0.5;
  float built = building ? 1 : 0.3 + 0.7 * i.color.a;
  float solid = 1;
  float3 work = 0;
  if (building) {
    solid = BuildSolid(faction, i.color.a, i.shape, work);
    clip(solid);
    work *= falling.w > 0 ? falling.w : 1;  // (less to an eye opened for the night)
    // (a lattice's line: its own light and nothing else)
    if (solid <= 0 && faction != 1 && faction != 3) return float4(look > 0.5 ? work * 2 : saturate(work), look > 0.5 ? 1 : 0.01);
    teamMask *= saturate((i.color.a - 0.9) * 10);
    if (faction == 3) albedo.a = teamMask;
    else spec.a = teamMask;
    built = faction == 3 ? lerp(0.35, 1, solid) : 1;
  }
  // wreckage: burnt. No team colour; soot in patches by the place on the model.
  float soot = 1;
  if (kind4.x > 0.5 && kind4.x < 1.5) {
    soot = CloudNoise(i.shape * 9) * 0.6 + CloudNoise(i.shape * 31) * 0.4;
    teamMask = 0;
    if (faction == 3) albedo.a = 0;
    else spec.a = 0;
    spec.b = 0;  // (nothing on it glows any more)
    built = look > 0.5 ? 1 : lerp(0.18, 0.55, soot);
  }
  float3 toCamera = normalize(cameraPosition - i.world);
  // toward the silhouette (0 facing the camera, 1 edge on), and the Seraphim team colour's strength by it
  float edge = pow(1 - saturate(dot(toCamera, N)), 0.6);
  float rim = 2 * pow(edge, 6) - 2 * edge + 1.5;

  if (look > 0.5) {
    float3 base = ToLinear(albedo.rgb);
    float3 tint = ToLinear(i.color.rgb);
    base = kind.z > 0.5 ? base * tint : lerp(base, tint, teamMask);
    float metallic = 0, roughness = 0.9;
    float3 emission = 0;
    if (kind.w > 0.5) {
      // a remastered unit: the material as painted. Team-coloured panels take the army's colour and
      // keep the paint's own light and dark (the rule of the lab's LabPBR shader for the original game).
      float luma = dot(albedo.rgb, float3(0.299, 0.587, 0.114));
      base = lerp(ToLinear(albedo.rgb), ToLinear(i.color.rgb * saturate(0.15 + luma * 1.3)), teamMask);
      metallic = spec.r;
      roughness = spec.g;
      emission = base * spec.b * 1.5;
    } else if (kind.z < 0.5) {
      occlusion = 1;
      if (faction == 1) {
        // Aeon: a light, smooth hull. Its paint is dark in the texture and was lifted by what it
        // mirrored; here the paint itself is lifted and only part of it is metal.
        float full = saturate(teamMask * 2.5);
        float mirror = saturate(spec.r * 2);
        // (the team colour at half strength: at full strength, lit, the last pass's curve bleaches it)
        base = lerp(ToLinear(saturate(albedo.rgb * (0.85 + 0.6 * mirror))), tint * 0.5, full);
        metallic = 0.5 * mirror * (1 - full);
        roughness = lerp(lerp(0.6, 0.25, saturate(spec.g * 1.6)), 0.5, full);
        emission = base * spec.b * 3 + tint * teamMask * 0.08;
      } else if (faction == 2) {
        // Cybran: armour painted very dark (the original lit it with the sun counted twice); both
        // reflection and highlight strength mark the metal. The paint is lifted before it is read
        // as linear colour, or it stays black whatever lights it.
        metallic = saturate(spec.r + max(spec.g - 0.1, 0) * 0.9 - teamMask * 2) * 0.6;
        base = lerp(ToLinear(saturate(albedo.rgb * (2.2 + 1.5 * metallic))), tint * 0.8, teamMask);
        roughness = lerp(0.8, 0.3, saturate(spec.g * 1.3));
        emission = base * sqrt(max(spec.b - 0.04, 0)) * 2;
      } else if (faction == 3) {
        // Seraphim: metal throughout, a light alloy plus the detail picture; team colour sits on
        // top and glows, more toward the silhouette
        float3 alloy = float3(0.20, 0.24, 0.31) + ToLinear(albedo.rgb) * 1.2;
        base = lerp(saturate(alloy), tint, teamMask * 0.8);
        metallic = 0.9 * (1 - teamMask * 0.6);
        roughness = clamp(0.85 - sqrt(spec.g) * 0.6, 0.15, 0.85);
        emission = tint * teamMask * rim * 0.25 + saturate(spec.b - 0.1) * 1.2;
      } else {
        metallic = saturate(spec.r * 1.2) * 0.7 * (1 - teamMask);
        roughness = lerp(0.75, 0.35, saturate(spec.g * 1.5));
        emission = base * spec.b * 3;
      }
    } else {
      occlusion = 1;
    }
    if (kind2.w > 0.5) {  // the map's border: dark machinery, glowing where its texture says
      base = ToLinear(saturate(albedo.rgb + albedo.a));
      metallic = 0.4;
      roughness = 0.55;
      emission = base * spec.b * 2.5;
    }
    if (kind4.x > 1.5) {  // liquid metal
      base = float3(0.72, 0.75, 0.78);
      metallic = 1;
      roughness = 0.14;
      emission = 0;
#ifdef REMASTERED_BUILD
      // (buildfx: the remastered build look's Aeon pool is liquid gold with the same faint warm light of its own as the
      // unit rising out of it, so the two are one liquid by day and by night (the light dimmed with the night as the
      // game's glowing effects are, or the eye opened for the night sees a flat yellow disc); build_effects.h)
      base = float3(1.0, 0.74, 0.33);
      emission = base * (0.06 * (falling.w > 0 ? falling.w : 1));
#endif
    } else if (kind4.x > 0.5) {  // wreckage: most of its colour gone, dark where the soot is thick with a little rust, dull
      float grey = dot(base, float3(0.3, 0.6, 0.1));
      base = lerp(float3(grey, grey, grey), base, 0.35) * lerp(0.12, 0.55, soot);
      base = lerp(base, float3(0.09, 0.05, 0.03), 0.25 * (1 - soot));
      metallic *= 0.3;
      roughness = lerp(0.8, 0.95, soot);
      emission = 0;
    }
#ifdef PLANTS
    // [plants] a burnt bush or fern (the game's own burnt trees are drawn as wreckage, above): black, then brown, its
    // embers glowing for the first seconds
    [branch] if (kind4.x < 0.5 && dot(i.plants, 1) > 0.002) {
      base = PlantsBurnColour(base, i.plants);
      emission += PlantsEmber(i.plants.w, i.world.xz, time) * (falling.w > 0 ? falling.w : 1);
    } else if (kind4.x > 0.5 && i.plants.x > 0.5) {
      base *= 0.45;  // (the game's burnt tree: what is left of it charred, not the wreckage's grey-brown)
    }
#endif
    if (building) {
#ifdef REMASTERED_BUILD
      RemasteredBuildSurface(faction, solid, i.rest, edge, N, base, metallic, roughness, emission, work);  // (buildfx: build_effects.h)
#else
      if (faction == 1) {  // Aeon: liquid metal that sets
        base = lerp(float3(0.72, 0.75, 0.78), base, solid);
        metallic = lerp(1, metallic, solid);
        roughness = lerp(0.14, roughness, solid);
        emission *= solid;
      } else if (faction == 3) {  // Seraphim: dark, lit from within, more toward the silhouette
        base *= lerp(0.25, 1, solid);
        emission = emission * solid + work * (0.4 + 2.5 * edge * edge);
        work = 0;
      }
      emission += work * 2;
#endif
    }
    if (shielded) {  // its personal shield: light of its faction's shield along its outline, breathing slowly
      float3 tint = faction == 1 ? float3(0.35, 1.0, 0.75) : faction == 2 ? float3(1.0, 0.2, 0.12) : faction == 3 ? float3(0.95, 0.85, 0.55) : float3(0.25, 0.5, 1.0);
      float outline = pow(1 - saturate(dot(toCamera, N)), 3);  // (only where the surface turns away from the eye)
      float3 glow = tint * ((0.03 + 1.1 * outline) * (0.8 + 0.2 * sin(time * 2.2)));
      [branch] if (kind5.w > 0.5) {
        // and its faction's lattice over it, as the bubble's (the visuals track's sheet): painted on the
        // model at rest and seen along the three axes, so it goes with the unit and its bones and shows
        // on every side; faint where the surface faces the eye (the paint must show), stronger toward
        // the outline, a little more where the sheet's patches drift, each cell on its own slow beat
        float3 q = i.rest * (1.0 / 3.0);  // (the sheet some 3 world units across: about 0.4 a cell)
        float3 w = pow(abs(normalize(i.restNormal)), 4);
        w /= w.x + w.y + w.z;
        const float layer = faction;
        float3 pat = shieldPatterns.Sample(samplerAniso, float3(q.zy, layer)).rgb * w.x +
                     shieldPatterns.Sample(samplerAniso, float3(q.xz, layer)).rgb * w.y +
                     shieldPatterns.Sample(samplerAniso, float3(q.xy, layer)).rgb * w.z;
        float beat = 0.75 + 0.25 * sin(time * 1.7 + pat.b * 6.283);
        float drift = pat.g * (0.6 + 0.4 * sin(time * 0.9 + pat.b * 6.283));
        glow += tint * (pat.r * (beat * (0.18 + 1.5 * outline) + drift * 0.36));
        [branch] if (hitAge >= 0) {  // a hit: the cells round it light up and fade, in about a second
          float near = saturate(1 - length(i.rest - i.tint) / 0.9);  // (some two cells round it)
          float fade = 1 - hitAge;
          glow += tint * (near * near * fade * fade * (0.35 + 2.5 * pat.r) * (0.7 + 0.3 * pat.b) * 2.0);
        }
      }
      emission += glow * (falling.w > 0 ? falling.w : 1);
    }
    // leaves: some of the light that falls on their far side comes through (World::leafLight: deeper in colour and
    // stronger toward an eye that looks at the sun through them, LeafLight; without it as at first, below)
#ifdef UNDER_SNOW
    // [undersnow] a low plant under lying snow stands in it: the light's snow lies on what faces up at the ground's own
    // measure where the ground at its foot is covered (twice the half the models take; none where the ground is bare), and
    // it is dusted a little all over by its own share (under_snow.h, UnderSnowDust); less light comes through its leaves
    // where snow lies
    float snowDust = i.snowHold.x >= 0 ? UnderSnowDust(i.snowHold.xy, i.snowAbove, N, i.world) : 0;
    if (kind2.y > 0.5 && leaves.x > 0.5) emission += LeafLight(base, LeafThrough(N, toCamera), i.world, shadow, spec.a) * (1 - snowDust);
    [branch] if (i.snowHold.x >= 0) {
      surfaceSnowShare = 2 * i.snowHold.z;
      base = lerp(base, SNOW_COLOUR, snowDust);
      roughness = lerp(roughness, SNOW_ROUGH, snowDust);
      metallic *= 1 - snowDust;
    }
#else
    if (kind2.y > 0.5 && leaves.x > 0.5) emission += LeafLight(base, LeafThrough(N, toCamera), i.world, shadow, spec.a);
#endif
    // [grime] battle grime, weather dirt and visible damage (unit_grime.h; World::unitGrime, unitDamage): its rows after its
    // marks and its dirt's (a half in the wear's y says they are there); its masks in its secondary texture where its level
    // has them (a quarter in kind5.z), where the dirt's fresh mud lies too
    float4 grimeGrad = float4(ddx(i.uv.xy - treadShift), ddy(i.uv.xy - treadShift));  // ([treads] the surface's own, unshifted)
    grimeSplash = GrimeHasMasks(kind5.z) ? secondary.a : -1;
    [branch] if (frac(i.wear.y) > 0.4 && i.wear.x >= 0 && !building && kind4.x < 0.5 && kind2.w < 0.5 && kind.y < 0.5)
      GrimeSurface(boneRows, (uint)(i.wear.x + 0.5) + (uint)i.wear.y * 3 + (frac(i.wear.y * 2) > 0.1 ? 3 : 0), i.uv.xy, grimeGrad, secondary, kind5.z,
                   occlusion, i.rest, normalize(i.restNormal), i.world, teamMask, i.color.rgb, falling.w > 0 ? falling.w : 1, normalize(i.tangent),
                   normalize(i.binormal), vertexNormal, base, metallic, roughness, emission, N);
    // battle scars (unit_wear.h, World::unitScars): its marks and its wear over all, on a finished unit's lit surface
    [branch] if ((i.wear.y > 0.9 || i.wear.z > 0.001) && !building && kind4.x < 0.5 && kind2.w < 0.5 && kind.y < 0.5)
      WearSurface(boneRows, i.wear, i.rest, normalize(i.restNormal), i.world, teamMask, falling.w > 0 ? falling.w : 1, base, metallic, roughness, emission, N);
    // [unit dirt] wet and muddy units (unit_wear.h, World::unitDirt): its own wetness, mud and dust, in the rows after its
    // marks (a quarter in the wear's y says they are there; [grime] a half is the grime's)
    [branch] if (frac(i.wear.y * 2) > 0.1 && i.wear.x >= 0 && !building && kind4.x < 0.5 && kind2.w < 0.5 && kind.y < 0.5)
      DirtSurface(boneRows, (uint)(i.wear.x + 0.5) + (uint)i.wear.y * 3, i.rest, normalize(i.restNormal), i.world, teamMask, base, metallic, roughness, N);
    // snow on units (World::unitSnow): its own share of what lies, none on what glows (engines, hot parts)
    [branch] if (i.wear.w >= 0) {
      surfaceSnowShare = i.wear.w * saturate(1 - dot(emission, float3(0.3, 0.6, 0.1)) * 6);
      surfaceSnowOwn = true;
      surfaceSnowPlace = i.rest * 3.0;  // (patches a third the size of the ground's: each flat top of a unit its own)
    }
#if RAYS
    // (leaves cast no rays for the small lights' shadows, lighting.h's SmallLightsTraced: a leaf's ray first passes the
    // many layers of its own crown, which cost a third of the option for little to see)
    if (kind2.y > 0.5) rayLightsAtMost = 0;
#endif
    // hot metal (World::hotMetal, hot_metal.h): a fresh wreck glows where it broke and on its thin parts, cooling; its age
    // and how long it glows ride in the ghost's tint, marked by -0.25 first (HotMetal::Tint)
    [branch] if (i.tint.z < 0 && abs(i.tint.x + 0.25) < 0.01) {
      // (for its thin parts: how sharply the surface turns for the model's size, from the neighbouring pixels, which take
      // this branch alike: they are of the same triangle)
      float turn = length(fwidth(vertexNormal)) / max(length(fwidth(i.shape)), 1e-5);
      emission += HotMetalGlow(-i.tint.y, -i.tint.z, i.shape, turn, kind.w > 0.5, albedo.rgb, spec.r, spec.g, soot) *
                  (kHotMetalStrength * pow(falling.w > 0 ? falling.w : 1, 0.6));
    }
    float3 lit = ModernLight(base, metallic, roughness, occlusion, emission, N, i.world, shadow);
    if (kind2.y > 0.5 && leaves.x < 0.5) {  // leaves: some of the light that falls on the far side comes through
      float3 falls = ToLinear(sunColor) * (lightMultiplier * modern.x);
#ifdef UNDER_SNOW
      falls *= 1 - snowDust;  // [undersnow]
#endif
      lit += base * falls * (saturate(dot(-N, sunDirection)) * shadow * CloudShadow(i.world) * spec.a * 0.6);
    }
    return float4(lit * built, kind.y > 0.5 ? albedo.a : cover);
  }

  if (kind2.w > 0.5) {  // the map's border as the original lights it: by the sun's angle, but not where it glows
    float shade = lerp(saturate(dot(sunDirection, N)), 1, spec.b);
    return float4(shade * (albedo.rgb + albedo.a) + 0.02, 0);  // (no glow: the original's boundary writes none)
  }
  // (the original look's glow, in the picture's alpha, for its bloom: what the original's unit shaders
  // write, its specular's blue plus glowMinimum 0.01; wreckage and the like only the 0.01; render 28)
  const float glowAlpha = kind4.x > 0.5 || kind6.y > 0.5 ? 0.01 : spec.b + 0.01;
  if (shielded) work += (faction == 1 ? float3(0.35, 1.0, 0.75) : faction == 2 ? float3(1.0, 0.2, 0.12) : faction == 3 ? float3(0.95, 0.85, 0.55) : float3(0.25, 0.5, 1.0)) * (0.03 + 0.6 * pow(1 - saturate(dot(toCamera, N)), 3));
  float3 light = OriginalLight(dot(sunDirection, N), shadow);
  if (kind.y > 0.5) return float4(i.color.rgb * albedo.rgb * light, kind2.y > 0.5 ? albedo.a : 0.01);  // the plain look ("VertexNormal"; leaves: their coverage)
  float3 environment = environmentMap.SampleLevel(samplerLinear, reflect(-toCamera, N), 0).rgb;
  float highlight = saturate(dot(reflect(-sunDirection, N), toCamera));
  if (kind4.x > 1.5) return float4(float3(0.55, 0.58, 0.62) * (0.4 + environment * 1.2 + pow(highlight, 12)), glowAlpha);  // liquid metal
  if (kind.z < 0.5 && faction == 3) {  // Seraphim
    float3 ambience = sunAmbience + (1 - sunAmbience) * shadowFill;
    // (how strongly it mirrors, by the angle it is seen at: the game's own ramp, in bands, where
    // the game's files have it)
    float mirrors = kind5.y > 0.5 ? seraphimFalloff.SampleLevel(samplerLinearClamp, float2(pow(1 - saturate(dot(toCamera, N)), 0.6), 0.5), 0).a : lerp(0.35, 1, edge);
    float3 seraphim = albedo.rgb * ambience + environment * spec.r * mirrors +
                      float3(0.5, 0.6, 0.7) * pow(highlight, 9) * spec.g + i.color.rgb * rim * albedo.a;
    return float4(seraphim * built + work * (0.3 + edge), glowAlpha);
  }
  albedo.rgb = kind.z > 0.5 ? albedo.rgb * i.color.rgb : lerp(i.color.rgb, albedo.rgb, 1 - spec.a);
  if (kind.z < 0.5 && faction == 1) {  // Aeon
    float3 sun = sunColor * saturate(dot(sunDirection, N)) * shadow + sunAmbience;
    float3 aeonLight = 0.6 * lightMultiplier * sun + (1 - sun) * shadowFill;
    float3 aeon = albedo.rgb * (2 * spec.b + aeonLight + spec.r * environment) + float3(0.8, 0.85, 1.1) * pow(highlight, 3) * spec.g;
    return float4(lerp(float3(0.55, 0.58, 0.62) * (0.4 + environment * 1.2 + pow(highlight, 12)), aeon, solid), glowAlpha);
  }
  if (kind.z < 0.5 && faction == 2) {  // Cybran
    float3 sun = 2 * sunColor * saturate(dot(sunDirection, N)) * shadow + sunAmbience;
    float3 cybranLight = lightMultiplier * sun + (1 - sun) * shadowFill;
    // (the highlight: the game's own lookup, by the sun's mirror image against the eye and by the
    // sun against the surface, where the game's files have it)
    float3 gleam = kind5.x > 0.5 ? insectLookup.SampleLevel(samplerLinearClamp, float2(dot(reflect(-sunDirection, N), toCamera), dot(sunDirection, N)), 0).rgb
                                 : pow(highlight, 6);
    float3 sheen = (gleam * spec.g + 0.05 * spec.r * environment) * (1 - spec.a);
    return float4((albedo.rgb * (2 * spec.b + cybranLight) + sheen) * built + work, glowAlpha);
  }
  float3 additive = float3(0.6, 0.8, 0.9) * highlight * highlight * spec.g;
  float3 color = albedo.rgb * (2 * spec.b + light + 2 * environment * spec.r) + additive;
  return float4(color * built + work, glowAlpha);
}

// a ghost: its shape in one colour, the light and dark of the model kept
float4 PS(VOut i, bool front : SV_IsFrontFace) : SV_Target {
  // [machinery] a belt's strip of the texture slid along its axis, wrapped within it
  [branch] if (i.belt >= 0) {
    float along = kind12.w > 0.5 ? i.uv.y : i.uv.x;
    along = kind12.x + frac((along - kind12.x + i.belt) / kind12.y) * kind12.y;
    if (kind12.w > 0.5) i.uv.y = along;
    else i.uv.x = along;
  }
  float4 c = Shade(i, front);
  c.rgb = BeyondScenery(c.rgb, i.world);  // (the trees and rocks of the world beyond the map's edge)
  // glass: as much of it shows as its albedo's alpha and its glow say (never quite nothing, or a
  // clear pane would not be there at all)
  if (kind.x < -0.5) c.a = saturate(max(albedoMap.Sample(samplerAniso, i.uv.xy).a + specularMap.Sample(samplerAniso, i.uv.xy).b, 0.12));
  if (i.color.a < 1.5 && dot(i.tint, float3(1, 1, 1)) > 0) {  // (a shielded unit's tint says where its shield was hit)
    float lightness = dot(c.rgb, float3(0.3, 0.6, 0.1));
    float3 tint = look > 0.5 ? ToLinear(i.tint) : i.tint;
    c.rgb = lerp(c.rgb, tint * (lightness * 1.5 + 0.1), 0.8);
  }
  return c;
}
)hlsl";

// Motion (motion.h): a model drawn again where its bones and place put it this frame, exactly as the
// scene's pass computes them (so it meets the depth that pass left), writing where its bones and place
// had the same point the frame before; both seen by the views without the jitter.
const char kMotionShader[] = R"hlsl(
struct Instance {
  float4 row0, row1, row2;
  float4 color;
  float4 bones;
  float4 wear;  // battle scars and snow (unit_wear.h): x where its marks begin in the bone buffer, y how many, z its wear over all, w its snow (-1: the weather's)
};
cbuffer Instances : register(b2) {
  Instance instances[640];
};
struct Last {
  float4 row0, row1, row2;
  float4 bones;  // x: where its bones of the frame before begin (-1: at rest); y: 1 = known
};
cbuffer Lasts : register(b3) {
  Last lasts[640];
};
cbuffer Views : register(b1) {
  row_major float4x4 now;
  row_major float4x4 before;
};
StructuredBuffer<float4> boneRows : register(t0, space1);

struct VIn {
  float3 position : POSITION;
  uint4 bones : BLENDINDICES;
};
struct VOut {
  float4 position : SV_Position;
  float4 here : TEXCOORD0;
  float4 then : TEXCOORD1;
};

float4 Posed(float3 position, float firstRow, uint bone) {
  float4 p = float4(position, 1);
  [branch] if (firstRow >= 0) {
    uint at = (uint)firstRow + bone * 3;
    float4 b0 = boneRows[at], b1 = boneRows[at + 1], b2 = boneRows[at + 2];
    p = float4(dot(b0, p), dot(b1, p), dot(b2, p), 1);
  }
  return p;
}

VOut VS(VIn i, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
  float4 p = Posed(i.position, n.bones.x, i.bones.x);
  float3 world = float3(dot(n.row0, p), dot(n.row1, p), dot(n.row2, p));
  VOut o;
  o.position = mul(viewProj, float4(world, 1));
  o.here = mul(now, float4(world, 1));
  float3 then = world;
  Last l = lasts[instance];
  [branch] if (l.bones.y > 0.5) {
    float4 q = Posed(i.position, l.bones.x, i.bones.x);
    then = float3(dot(l.row0, q), dot(l.row1, q), dot(l.row2, q));
  }
  o.then = mul(before, float4(then, 1));
  return o;
}

float2 PS(VOut i) : SV_Target {
  return (i.then.xy / i.then.w - i.here.xy / i.here.w) * float2(0.5, -0.5);
}
)hlsl";

// [leftover] The motion of what sways (MeshRenderer::swayMotion): the trees and bushes the scene drew, again, each corner
// where the scene's shader puts it (the wind's swing and gusts, a group's trees each on its own beat, the blasts' waves, the
// plants pressed and burnt: kShader's VS, kept in step with it), writing where the same corner was the frame before: the
// wind at that frame's time, what presses it and the blasts' waves as long ago less the frame's seconds (BlastPushBack:
// blasts.cpp's BlastPush for a tree, kept in step with it). Holes are cut as the scene cut them (a leaf's coverage at a
// half, a cut-out's alpha), so that what shows through a leaf keeps its own motion. Built with the blasts' waves and plants
// that react in it, each left out by its constants when off (none in the waves' list; plants: motionWhen.z 0).
const char kSwayMotionShader[] = R"hlsl(
struct Instance {
  float4 row0, row1, row2;
  float4 color;
  float4 bones;
  float4 wear;
};
#ifdef CARD_SCENERY
// [gpuscenery] (as kShader's: one level's list on the card, card_scenery.h)
cbuffer CardDraw : register(b2) {
  uint4 cardDraw;
};
StructuredBuffer<float4> boneRows : register(t0, space1);
uint CardWord(uint w) { return asuint(boneRows[w >> 2][w & 3]); }
Instance CardInstance(uint instance) {
  uint slot = CardWord(CardWord(cardDraw.x + 5) + instance) * 8;
  Instance n;
  n.row0 = boneRows[slot];
  n.row1 = boneRows[slot + 1];
  n.row2 = boneRows[slot + 2];
  n.color = boneRows[slot + 3];
  n.bones = boneRows[slot + 4];
  n.wear = boneRows[slot + 5];
  return n;
}
#else
cbuffer Instances : register(b2) {
  Instance instances[640];
};
#endif
cbuffer SwayMotion : register(b3) {
  row_major float4x4 motionNow;     // this frame's view without the jitter
  row_major float4x4 motionBefore;  // the frame before's
  float4 motionWhen;  // x: the frame before's time; y: the seconds between the two; z: 1 = plants react
  float4 kind;        // x: alpha not above this is cut away (MeshKind's)
  float4 kind2;       // y: 1 = leaves
  float4 kind3;       // x: how much it sways; y: 1 / its height in model units
  float4 kind6;       // z: a group of trees: model units a step of where each tree stands (0: none)
  float4 kind7, kind8;  // (plants.h) PlantLife::Constants, as the scene's pass had them
#ifdef UNDER_SNOW
  float4 kind9, kind10, kind11;  // [undersnow] (under_snow.h) the lying snow over the low plants, as the scene's pass had it
#endif
};
#ifndef CARD_SCENERY
StructuredBuffer<float4> boneRows : register(t0, space1);
#endif
Texture2D albedoMap : register(t0);

struct VIn {
  float3 position : POSITION;
  float4 uv : TEXCOORD0;
  uint4 bones : BLENDINDICES;
};
struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 here : TEXCOORD1;
  float4 then : TEXCOORD2;
};

// the blasts' push on what sways (as kBlastShaderBlock's BlastPush works it out for a tree) `back` seconds earlier than now:
// each wave's age less that (one not yet begun then pushes nothing)
float2 BlastPushBack(float2 xz, float back) {
  float2 push = 0;
  uint n = (uint)blastHead.x;
  [loop] for (uint i = 0; i < n; ++i) {
    float4 a = blastList[i * 3], b = blastList[i * 3 + 1];
    float2 off = xz - a.xy;
    float d = length(off);
    float since = b.x - back - d / b.y;
    if (d >= a.z || since <= 0) continue;
    float x = d / a.z;
    float fall = (1 - x) * (1 - x) * (1 + 2 * x);
    float swing = saturate(since / 0.12) * exp(-since / 0.85) * cos(since * 4.2);
    push += off * (b.w * fall * swing / max(d, 0.05));
  }
  return push;
}

// where the scene's shader puts a corner at time `t`, what presses it and the blasts' waves `back` seconds earlier than now
float3 Swayed(VIn i, Instance n, float t, float back) {
  float4 p = float4(i.position, 1);
  [branch] if (n.bones.x >= 0) {
    uint at = (uint)n.bones.x + i.bones.x * 3;
    float4 b0 = boneRows[at], b1 = boneRows[at + 1], b2 = boneRows[at + 2];
    p = float4(dot(b0, p), dot(b1, p), dot(b2, p), 1);
  }
  float3 world = float3(dot(n.row0, p), dot(n.row1, p), dot(n.row2, p));
  float high = saturate(p.y * kind3.y);
  float size = length(float3(n.row0.x, n.row1.x, n.row2.x)) / kind3.y;
  float2 foot = float2(n.row0.w, n.row2.w);
  float beat = dot(foot, float2(0.11, 0.07));
  [branch] if (i.bones.w > 0 && kind6.z > 0 && look > 0.5) {
    float2 own = (float2(i.bones.yz) - 128) * kind6.z;
    foot = float2(dot(n.row0, float4(own.x, 0, own.y, 1)), dot(n.row2, float4(own.x, 0, own.y, 1)));
    beat = dot(foot, float2(0.11, 0.07)) + (i.bones.w - 1) * (6.2831853 / 254);
  }
  float swing = sin(t * 1.3 + beat) * 0.6 + sin(t * 2.9 + beat * 1.7 + p.x * kind3.y * 4) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (t * 7)) * 0.02, t * 0.07)));
  float2 carried = float2(0.915, 0.403) * (swing * gust * kind3.x * high * high * size * 0.035);
  if (motionWhen.z > 0.5) carried *= PlantsUpright(n.row1.y / max(length(float3(n.row0.y, n.row1.y, n.row2.y)), 1e-6));
  world.xz += carried;
  world.xz += (back > 0 ? BlastPushBack(foot, back) : BlastPush(foot, 1)) * (kind3.x * high * high * size);
  [branch] if (motionWhen.z > 0.5 && size < 2.2 && kind8.z + kind8.w > 0.5) {
    float3 root = float3(n.row0.w, n.row1.w, n.row2.w);
    float4 pressed = PlantsPressAt(root.xz, kind7, kind8);
    [branch] if (pressed.z > 0.01) {
      float since = max(pressed.w / pressed.z - 0.3 - back, 0);
      float over = 1.25 * pressed.z * exp(-since / 0.6) * cos(since * 2.4) * (0.45 + 0.55 * high);
      float2 way = pressed.xy / pressed.z;
      way = length(way) > 0.05 ? normalize(way) : float2(0.6, 0.8);
      float rise = max(world.y - root.y, 0);
      world.xz += way * (rise * sin(over));
      world.y -= rise * (1 - cos(over));
    }
    float4 burnt = PlantsBurnLook(root.xz, kind7, kind8, CloudHash(float3(root.xz, 1.3)));
    world.y -= max(world.y - root.y, 0) * (burnt.z * 0.45);
  }
  return world;
}

VOut VS(VIn i, uint instance : SV_InstanceID) {
#ifdef CARD_SCENERY
  Instance n = CardInstance(instance);  // [gpuscenery]
#else
  Instance n = instances[instance];
#endif
  float3 now = Swayed(i, n, time, 0), before = Swayed(i, n, motionWhen.x, motionWhen.y);
  VOut o;
#ifdef UNDER_SNOW
  // [undersnow] a low plant under lying snow where the scene's shader puts it (kShader's VS, under_snow.h): the snow the same
  // at both moments, each corner bent by it after its sway; buried whole: none of it
  [branch] if (kind3.x > 0) {
    float3 root = float3(n.row0.w, n.row1.w, n.row2.w);
    float scale = length(float3(n.row0.x, n.row1.x, n.row2.x));
    float top = scale / kind3.y;
    float4 plant;
    float3 ground;
    float covered;
    [branch] if (UnderSnowPlant(root, top, scale / kind3.w, kind9, kind10, kind11, plant, ground, covered)) {
      if (plant.w > 0.5) {
        o.position = float4(0, 0, -1, 1);
        o.uv = 0;
        o.here = o.then = float4(0, 0, 0, 1);
        return o;
      }
      float turn = CloudHash(root * 1.7 + 0.3) * 6.2831853;
      float3 normal = 0, tangent = 0, binormal = 0;
      float above;
      now = UnderSnowCorner(now, root, top, plant, ground, float2(cos(turn), sin(turn)), normal, tangent, binormal, above);
      before = UnderSnowCorner(before, root, top, plant, ground, float2(cos(turn), sin(turn)), normal, tangent, binormal, above);
    }
  }
#endif
  o.position = mul(viewProj, float4(now, 1));
  o.uv = i.uv.xy;
  o.here = mul(motionNow, float4(now, 1));
  o.then = mul(motionBefore, float4(before, 1));
  return o;
}

float2 PS(VOut i) : SV_Target {
  float a = albedoMap.Sample(samplerAniso, i.uv).a;
  if (kind2.y > 0.5) clip(saturate((a - kind.x) / max(fwidth(a), 1e-4) + 0.5) - 0.5);  // (a leaf's coverage, as the scene's)
  else if (kind.x > 0) clip(a - kind.x);
  return (i.then.xy / i.then.w - i.here.xy / i.here.w) * float2(0.5, -0.5);
}
)hlsl";

}  // namespace

bool MeshRenderer::Init(std::string* error) {
  // (units and props take half the lying snow the ground takes: they must still be told apart)
  // ([unit dirt] SURFACE_OWN_WET: a unit's own wetness may stand in for the weather's, lighting.h)
  std::string source = std::string("#define SURFACE_SNOW 0.5\n#define SURFACE_OWN_WET 1\n") + kViewShaderBlock + RayShaders() + kLightingShaderBlock + kGrimeShaderBlock + kWearShaderBlock + HotMetal::ShaderBlock() + kOriginalShieldBlock + kShader;  // (hot metal: hot_metal.h)
  // (the same with the blasts' waves, for what sways while blasts are under way: PrepareBlasts makes them)
  blastSource_ = std::string("#define BLASTS 1\n#define SURFACE_SNOW 0.5\n#define SURFACE_OWN_WET 1\n") + kViewShaderBlock + RayShaders() + kLightingShaderBlock +
                 kBlastShaderBlock + kGrimeShaderBlock + kWearShaderBlock + HotMetal::ShaderBlock() + kOriginalShieldBlock + kShader;
  // [plants] (the same with plants that react, the blasts' waves in them too: PreparePlants makes them; its textures at t8, t9)
  plantsSource_ = std::string("#define BLASTS 1\n#define PLANTS 1\n#define PLANTS_PRESS t8\n#define PLANTS_BURN t9\n#define SURFACE_SNOW 0.5\n#define SURFACE_OWN_WET 1\n") +
                  kViewShaderBlock + RayShaders() + kLightingShaderBlock + kBlastShaderBlock + kPlantsShaderBlock + kGrimeShaderBlock + kWearShaderBlock +
                  HotMetal::ShaderBlock() + kOriginalShieldBlock + kShader;
  // [plants] (the sun's maps: bent, not burnt; the game's burnt trees with their needles gone by a variant of their own)
  plantsShadowSource_ = "#define PLANTS_SHADOW 1\n" + plantsSource_;
  plantsShadowBurntSource_ = "#define PLANTS_SHADOW 1\n#define PLANTS_SHADOW_BURNT 1\n" + plantsSource_;
  // [undersnow] (the same as the plants' with the low plants under lying snow, under_snow.h: PrepareUnderSnow makes them; the
  // blasts' waves and plants that react in them, each off by its constants when off; the sun's maps bent, not burnt)
  snowSource_ = std::string("#define BLASTS 1\n#define PLANTS 1\n#define PLANTS_PRESS t8\n#define PLANTS_BURN t9\n#define SURFACE_SNOW 0.5\n#define SURFACE_OWN_WET 1\n#define UNDER_SNOW 1\n") +
                kViewShaderBlock + RayShaders() + kLightingShaderBlock + kBlastShaderBlock + kPlantsShaderBlock + kGrimeShaderBlock + kWearShaderBlock +
                HotMetal::ShaderBlock() + kUnderSnowShaderBlock + kOriginalShieldBlock + kShader;
  snowShadowSource_ = "#define PLANTS_SHADOW 1\n" + snowSource_;
  // (buildfx: the same with the remastered build look, for while it is on: PrepareBuild makes them)
  buildSource_ = std::string("#define REMASTERED_BUILD 1\n#define SURFACE_SNOW 0.5\n#define SURFACE_OWN_WET 1\n") + kViewShaderBlock + RayShaders() + kLightingShaderBlock +
                 kGrimeShaderBlock + kWearShaderBlock + HotMetal::ShaderBlock() + kBuildShaderBlock + kOriginalShieldBlock + kShader;
  {  // [gpuscenery] the same, reading each instance from the card's lists (card_scenery.h): made when cardScenery is first on
    const std::string card = "#define CARD_SCENERY 1\n";
    baseSource_ = source;
    const std::string* of[8] = {&baseSource_, &blastSource_, &plantsSource_, &plantsShadowSource_, &plantsShadowBurntSource_,
                                &snowSource_, &snowShadowSource_, &buildSource_};
    for (int k = 0; k < 8; ++k) cardSources_[k] = card + *of[k];
  }
  int kept = 0;
  auto keep = [&](gfx::Pipeline p, const gfx::PipelineDesc& d) {
    bending_[kept++] = {p, d, {}};
    buildVariants_.push_back({p, d, {}});  // (buildfx)
  };
  gfx::PipelineDesc pd;
  pd.name = "models";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
              {"NORMAL", 0, gfx::AttrType::Float3, offsetof(ScmVertex, normal)},
              {"TANGENT", 0, gfx::AttrType::Float3, offsetof(ScmVertex, tangent)},
              {"BINORMAL", 0, gfx::AttrType::Float3, offsetof(ScmVertex, binormal)},
              {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)},
              {"BLENDINDICES", 0, gfx::AttrType::UByte4, offsetof(ScmVertex, bones)}};
  pd.stride = sizeof(ScmVertex);
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = true;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  keep(pipeline_, pd);
  pd.name = "models, blended";
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;
  blended_ = device_->CreatePipeline(pd, error);
  if (!blended_) return false;
  keep(blended_, pd);
  pd.name = "models, see-through";
  pd.cull = gfx::Cull::None;  // (a bubble's far side shows through its near side)
  pd.depthWrite = false;
  pd.alphaWrite = true;
  glass_ = device_->CreatePipeline(pd, error);
  if (!glass_) return false;
  buildVariants_.push_back({glass_, pd, {}});  // (buildfx: a unit's glass is built too)
  pd.name = "models, see-through, colour alone";
  pd.alphaWrite = false;
  glassRgb_ = device_->CreatePipeline(pd, error);
  if (!glassRgb_) return false;
  buildVariants_.push_back({glassRgb_, pd, {}});
  pd.alphaWrite = true;
  pd.cull = gfx::Cull::Back;
  pd.depthWrite = true;
  pd.name = "models, a ball of fire";  // (seen from outside only, and it hides what burns inside it)
  dome_ = device_->CreatePipeline(pd, error);
  if (!dome_) return false;
  pd.name = "models, a ball of fire, colour alone";
  pd.alphaWrite = false;
  domeRgb_ = device_->CreatePipeline(pd, error);
  if (!domeRgb_) return false;
  pd.alphaWrite = true;
  pd.name = "models, adding their light";  // (a shield's flash where it is hit)
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Additive;
  adding_ = device_->CreatePipeline(pd, error);
  if (!adding_) return false;
  pd.depthWrite = true;
  pd.name = "models, leaves";
  pd.blend = gfx::Blend::Opaque;
  pd.cull = gfx::Cull::None;
  pd.alphaToCoverage = true;
  pd.alphaWrite = false;
  leaves_ = device_->CreatePipeline(pd, error);
  if (!leaves_) return false;
  keep(leaves_, pd);
  pd.name = "models, leaves' depth first";
  pd.pixelEntry = "DepthPS";
  pd.colorWrite = false;
  prepassLeaves_ = device_->CreatePipeline(pd, error);
  if (!prepassLeaves_) return false;
  keep(prepassLeaves_, pd);
  pd.alphaToCoverage = false;
  pd.cull = gfx::Cull::Back;
  pd.name = "models, depth first";
  prepass_ = device_->CreatePipeline(pd, error);
  if (!prepass_) return false;
  keep(prepass_, pd);
  // after it: the full shading writing no depth (the depth is there already). A pass that writes depth
  // and cuts holes (a leaf's edge) has its depth tested only after its shading, so every leaf behind
  // another would still be shaded; one that writes none can be tested first, and what lies behind is
  // thrown away unshaded.
  pd.colorWrite = true;
  pd.pixelEntry = "PS";
  pd.depthWrite = false;
  pd.name = "models, depth laid";
  pd.alphaWrite = true;
  laid_ = device_->CreatePipeline(pd, error);
  if (!laid_) return false;
  keep(laid_, pd);
  pd.name = "models, leaves, depth laid";
  pd.cull = gfx::Cull::None;
  pd.alphaToCoverage = true;
  pd.alphaWrite = false;
  laidLeaves_ = device_->CreatePipeline(pd, error);
  if (!laidLeaves_) return false;
  keep(laidLeaves_, pd);
  pd.alphaToCoverage = false;
  pd.depthWrite = true;
  pd.colorWrite = true;
  pd.name = "models, shadow";
  pd.samples = 1;
  pd.blend = gfx::Blend::Opaque;
  pd.pixelEntry = "ShadowPS";
  pd.depthOnly = true;
  pd.cull = gfx::Cull::None;  // thin and open meshes cast a shadow from either side
  shadow_ = device_->CreatePipeline(pd, error);
  if (!shadow_) return false;
  keep(shadow_, pd);
  {  // motion: drawn over the scene's depth (one sample a pixel), testing it a hair nearer, writing none
    const std::string motionSource = std::string(kViewShaderBlock) + kMotionShader;
    gfx::PipelineDesc md;
    md.name = "models, motion";
    md.vertexSource = md.pixelSource = motionSource;
    md.attrs = pd.attrs;
    md.stride = pd.stride;
    md.colorFormat = gfx::Format::RG16F;
    md.depthWrite = false;
    md.depthBias = 16;  // (the same placement compiled in another shader may land a hair farther)
    motionPipeline_ = device_->CreatePipeline(md, error);
    if (!motionPipeline_) return false;
  }
  batch_.resize(kMaxInstances);
  lastBatch_.resize(kMaxInstances);
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  boneBuffer_ = device_->CreateBuffer(bd);
  return static_cast<bool>(shadow_);
}

void MeshRenderer::Begin() {
  motionItems_.clear();
  swayItems_.clear();  // [leftover]
  markedItems_.clear();  // (hidden outlines)
  marks_ = 0;
  markedSorted_ = false;
  entries_.clear();
  culls_.clear();
  groupsListed_.clear();
  rowsOut_ = nullptr;
  shellHits_.clear();
  wears_.clear();
  wearRows_.clear();
  prepared_ = false;
}

void MeshRenderer::Prepare(const std::vector<const View*>& views) {
  const auto t0 = std::chrono::steady_clock::now();
  const char* outer = CpuTimes::Current();
  CpuTimes::Part("bones: posed models listed");
  // which posed models some pass sees (a test per view for each: on several threads at once)
  posedSeen_.resize(culls_.size());
  constexpr size_t kTestPiece = 2048;
  Jobs::Shared().Run(static_cast<int>((culls_.size() + kTestPiece - 1) / kTestPiece), [&](int k) {
    const size_t from = static_cast<size_t>(k) * kTestPiece, to = std::min(from + kTestPiece, culls_.size());
    for (size_t i = from; i < to; ++i) {
      const Cull& c = culls_[i];
      bool seen = false;
      if (c.flags & kPosed) {
        const Vec3 at = entries_[i].centre, r{c.radius, c.radius, c.radius};
        seen = views.empty();
        for (const View* view : views)
          if (view->BoxVisible(at - r, at + r)) {
            seen = true;
            break;
          }
      }
      posedSeen_[i] = seen ? 1 : 0;
    }
  });
  // each its own stretch of the rows, given in order
  size_t floats = 0;
  for (size_t i = 0; i < culls_.size(); ++i) {
    if (!posedSeen_[i]) continue;
    Entry& e = entries_[i];
    e.rows = static_cast<int>(floats / 4);
    floats += e.model->bones.size() * 12;
    rowsToBuild_.push_back(&e);
  }
  // after them the shells' hits, a row each; the instance's colour says where they begin and how many
  const size_t rowsEnd = floats;
  const size_t hitsFrom = floats;
  for (const ShellHits& s : shellHits_) {
    Entry& e = entries_[s.entry];
    e.instance.color[0] = static_cast<float>(floats / 4);
    e.instance.color[1] = static_cast<float>(s.hits.size());
    floats += s.hits.size() * 4;
  }
  // battle scars (Wear): after them the marks of the units some pass sees, three rows each; the instance's wear says
  // where they begin (the others drawn without them)
  for (WearRows& w : wears_) {
    Entry& e = entries_[w.entry];
    bool seen = views.empty();
    const Vec3 r{e.radius, e.radius, e.radius};
    for (const View* view : views)
      if (view->BoxVisible(e.centre - r, e.centre + r)) {
        seen = true;
        break;
      }
    w.at = seen ? static_cast<int64_t>(floats / 4) : -1;
    e.instance.wear[0] = seen ? static_cast<float>(floats / 4) : -1.0f;
    if (!seen) e.instance.wear[1] = 0;
    else floats += static_cast<size_t>(w.rows) * 4;
  }
  // motion: after the hits, the bones of the frame before of each keyed model the camera's view may see
  // (where the frame before knew it, as the same model)
  const size_t lastFrom = floats;
  if (motion) {
    lastBonesAt_.assign(entries_.size(), -2);
    lastOf_.assign(entries_.size(), LastInstance{});
    for (size_t i = 0; i < entries_.size(); ++i) {
      const Entry& e = entries_[i];
      if (!e.key) continue;
      const auto known = last_.find(e.key);
      if (known == last_.end() || known->second.model != e.model) continue;
      if (!views.empty()) {
        const Vec3 r{e.radius, e.radius, e.radius};
        if (!views[0]->BoxVisible(e.centre - r, e.centre + r)) continue;
      }
      lastBonesAt_[i] = -1;
      if (known->second.bones >= 0) {
        lastBonesAt_[i] = static_cast<int>(floats / 4);
        floats += e.model->bones.size() * 12;
      }
      LastInstance& l = lastOf_[i];
      std::memcpy(l.row0, known->second.row0, sizeof l.row0);
      std::memcpy(l.row1, known->second.row1, sizeof l.row1);
      std::memcpy(l.row2, known->second.row2, sizeof l.row2);
      l.bones[0] = static_cast<float>(lastBonesAt_[i]);
      l.bones[1] = 1;
    }
  }
  // the bones' matrices, written straight into the frame's copy of the buffer: many models, each into
  // its own stretch, so on several threads at once (with motion on, into a copy here first, kept for the
  // next frame: the buffer's memory is slow to read back)
  CpuTimes::Part("bones: matrices made");
  rowsOut_ = floats ? static_cast<float*>(device_->WriteBuffer(boneBuffer_, static_cast<uint32_t>(floats * sizeof(float)))) : nullptr;
  if (rowsOut_) {
    if (motion) rowsCopy_.resize(rowsEnd);
    float* rowsTo = motion ? rowsCopy_.data() : rowsOut_;
    constexpr size_t kPerPiece = 256;
    Jobs::Shared().Run(static_cast<int>((rowsToBuild_.size() + kPerPiece - 1) / kPerPiece), [&](int k) {
      const size_t from = static_cast<size_t>(k) * kPerPiece, to = std::min(from + kPerPiece, rowsToBuild_.size());
      for (size_t j = from; j < to; ++j) BuildRows(rowsToBuild_[j], rowsTo);
    });
    if (motion) {
      if (rowsEnd) std::memcpy(rowsOut_, rowsCopy_.data(), rowsEnd * sizeof(float));
      // the frame before's bones where this frame's buffer has room for them
      for (size_t i = 0; i < entries_.size(); ++i) {
        if (lastBonesAt_[i] < 0) continue;
        const LastPlace& l = last_[entries_[i].key];
        std::memcpy(rowsOut_ + static_cast<size_t>(lastBonesAt_[i]) * 4, lastRows_.data() + static_cast<size_t>(l.bones) * 4,
                    entries_[i].model->bones.size() * 12 * sizeof(float));
      }
    }
    float* out = rowsOut_ + hitsFrom;
    for (const ShellHits& s : shellHits_)
      for (const Vec4& h : s.hits) {
        out[0] = h.x, out[1] = h.y, out[2] = h.z, out[3] = h.w;
        out += 4;
      }
    for (const WearRows& w : wears_)
      if (w.at >= 0) std::memcpy(rowsOut_ + w.at * 4, wearRows_.data() + w.from, static_cast<size_t>(w.rows) * sizeof(Vec4));
    (void)lastFrom;
  } else {
    for (Entry* e : rowsToBuild_) e->rows = -1;  // (no room this frame: drawn at rest)
    if (motion) {  // (and nothing of the frame before)
      lastBonesAt_.assign(entries_.size(), -2);
      lastOf_.assign(entries_.size(), LastInstance{});
    }
    for (const ShellHits& s : shellHits_) entries_[s.entry].instance.color[1] = 0;  // (and the shells without their hits)
    for (const WearRows& w : wears_) entries_[w.entry].instance.wear[1] = 0;      // (and the units without their marks)
    const float none[4] = {0, 0, 0, 0};  // (the shaders' buffer is never left unset)
    device_->UpdateBuffer(boneBuffer_, none, sizeof none);
  }
  rowsToBuild_.clear();
  if (motion) {  // every keyed model's place and bones, for the next frame
    lastNext_.clear();
    lastRowsNext_.clear();
    for (const Entry& e : entries_) {
      if (!e.key) continue;
      LastPlace& l = lastNext_[e.key];
      l.model = e.model;
      std::memcpy(l.row0, e.instance.row0, sizeof l.row0);
      std::memcpy(l.row1, e.instance.row1, sizeof l.row1);
      std::memcpy(l.row2, e.instance.row2, sizeof l.row2);
      l.bones = -1;
      if (e.rows >= 0 && rowsOut_) {
        l.bones = static_cast<int>(lastRowsNext_.size() / 4);
        const float* from = rowsCopy_.data() + static_cast<size_t>(e.rows) * 4;
        lastRowsNext_.insert(lastRowsNext_.end(), from, from + e.model->bones.size() * 12);
      }
    }
    last_.swap(lastNext_);
    lastRows_.swap(lastRowsNext_);
  } else if (!last_.empty() && !keepLast) {
    last_.clear();
    lastRows_.clear();
  }
  // [gpuscenery] the still scenery picked on the card for the frame's passes (it dispatches: between passes, as Prepare is)
  cardNow_ = false;
  if (cardScenery && !views.empty()) {
    CpuTimes::Part("scenery picked on the card");
    if (CardPays()) cardNow_ = CardPrepare(views);  // [step2b] (cardByNeed: only where it pays)
    else CardKeep();
  }
  CpuTimes::Part(outer);
  prepared_ = true;
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  prepareMs = prepareMs == 0 ? ms : prepareMs * 0.95f + ms * 0.05f;
}

// The rows of a posed model's bones. A bone's matrix is: back from where the bone stands at rest,
// then to where it is now. Now is between the two beats' poses; a bone a beat does not list is at
// rest in that beat.
void MeshRenderer::BuildRows(Entry* entry, float* rows) {
  const std::vector<ModelBone>& bones = entry->model->bones;
  const Pose& pose = entry->pose;
  float* out = rows + static_cast<size_t>(entry->rows) * 4;
  const float toModel = 1.0f / entry->model->scale;  // poses are in world units
  size_t ia = 0, ib = 0;
  for (size_t i = 0; i < bones.size(); ++i, out += 12) {
    while (ia < pose.nowCount && pose.now[ia].bone < i) ++ia;
    while (ib < pose.nextCount && pose.next[ib].bone < i) ++ib;
    const FrameBone* a = ia < pose.nowCount && pose.now[ia].bone == i ? &pose.now[ia] : nullptr;
    const FrameBone* b = ib < pose.nextCount && pose.next[ib].bone == i ? &pose.next[ib] : nullptr;
    if (!a && !b) {  // at rest: nothing to do to its vertices
      const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      std::copy(identity, identity + 12, out);
      continue;
    }
    const ModelBone& rest = bones[i];
    if ((a && a->hidden) || (!a && b && b->hidden)) {  // hidden: its vertices at no size, where its joint is
      // ([machinery] where it is posed, not where it rests: a conveyor's part hidden for its jump back waits where it comes
      // back, and the frame it shows again moves its vertices only by its own size there, not from its joint at rest)
      const FrameBone* h = a ? a : b;
      const Vec3 at = Vec3{h->position[0], h->position[1], h->position[2]} * toModel;
      const float gone[12] = {0, 0, 0, at.x, 0, 0, 0, at.y, 0, 0, 0, at.z};
      std::copy(gone, gone + 12, out);
      continue;
    }
    auto place = [&](const FrameBone* f) { return f ? Vec3{f->position[0], f->position[1], f->position[2]} * toModel : rest.position; };
    auto turn = [&](const FrameBone* f) { return f ? Quat{f->orientation[0], f->orientation[1], f->orientation[2], f->orientation[3]} : rest.rotation; };
    const Quat q = Mul(Nlerp(turn(a), turn(b), pose.blend), Conjugate(rest.rotation));
    const Vec3 t = Lerp(place(a), place(b), pose.blend) - Rotate(q, rest.position);
    const Mat4 m = Mat4::FromTRS(t, q);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) out[r * 4 + c] = m.m[r][c];
  }
}

MeshRenderer::Entry MeshRenderer::MakeEntry(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Vec3* tint,
                                            const float* treads) const {
  Entry e;
  e.model = model;
  // the model's bounding sphere in the world (the matrix is a rotation with one scale)
  const Vec3 centre = (model->boundsMin + model->boundsMax) * 0.5f;
  const float scale = std::sqrt(world.m[0][0] * world.m[0][0] + world.m[1][0] * world.m[1][0] + world.m[2][0] * world.m[2][0]);
  e.radius = Length(model->boundsMax - model->boundsMin) * 0.5f * scale;
  const Vec4 c = world * Vec4{centre.x, centre.y, centre.z, 1};
  e.centre = {c.x, c.y, c.z};
  for (int k = 0; k < 4; ++k) {
    e.instance.row0[k] = world.m[0][k];
    e.instance.row1[k] = world.m[1][k];
    e.instance.row2[k] = world.m[2][k];
  }
  e.instance.color[0] = teamColor.x;
  e.instance.color[1] = teamColor.y;
  e.instance.color[2] = teamColor.z;
  e.instance.color[3] = built;
  e.instance.bones[0] = -1;
  e.instance.wear[0] = -1;  // (no marks, no wear, the weather's snow: unit_wear.h)
  e.instance.wear[1] = 0;
  e.instance.wear[2] = 0;
  e.instance.wear[3] = -1;
  e.instance.bones[1] = tint ? tint->x : 0;
  e.instance.bones[2] = tint ? tint->y : 0;
  e.instance.bones[3] = tint ? tint->z : 0;
  if (treads) {  // (marked by y -1, which no tint has; the amounts kept small, the texture repeats)
    e.instance.bones[1] = -1;
    e.instance.bones[2] = treads[0] - std::floor(treads[0]);
    e.instance.bones[3] = treads[1] - std::floor(treads[1]);
  }
  e.sizeRadius = e.radius;
  for (const ModelLod& lod : model->lods) e.flags |= lod.glass ? kHasGlass : kHasSolid;
  return e;
}

void MeshRenderer::Add(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Pose* pose, const Vec3* tint,
                       const float* treads, uint32_t key) {
  if (!model || !model->ok) return;
  Entry e = MakeEntry(model, world, teamColor, built, tint, treads);
  e.key = key;
  e.scenery = addingScenery;
  if (addingScenery) e.flags |= kScenery;
  if (pose && (pose->nowCount || pose->nextCount) && !model->bones.empty()) {
    e.pose = *pose;
    e.posed = true;
    e.flags |= kPosed;
    e.radius *= 1.25f;  // parts swing out of the rest pose's bounds
  }
  Push(e);
}

void MeshRenderer::AddShell(const Model* shell, const Mat4& world, const std::vector<Vec4>& hits) {
  if (!shell || !shell->ok || hits.empty()) return;
  Entry e = MakeEntry(shell, world, {0, 0, 0}, 1, nullptr);
  Push(e);
  shellHits_.push_back({entries_.size() - 1, hits});
}

void MeshRenderer::Wear(const WearLook& look) {
  if (entries_.empty()) return;
  Entry& e = entries_.back();
  e.instance.wear[1] = static_cast<float>(look.count) + (look.dirt ? 0.25f : 0.0f);  // [unit dirt] (a quarter: its dirt's rows follow the marks)
  if (look.grime) e.instance.wear[1] += 0.5f, grimeWanted_ = true;  // [grime] (a half: its grime's rows follow those)
  e.instance.wear[2] = look.general;
  e.instance.wear[3] = look.snow;
  const int rows = look.count * 3 + (look.dirt ? kDirtRows : 0) + (look.grime ? kGrimeRows : 0);  // ([grime])
  if (rows > 0) {
    wears_.push_back({entries_.size() - 1, rows, wearRows_.size(), -1});
    wearRows_.insert(wearRows_.end(), look.rows, look.rows + look.count * 3);
    if (look.dirt) wearRows_.insert(wearRows_.end(), look.dirtRows, look.dirtRows + kDirtRows);  // [unit dirt]
    if (look.grime) wearRows_.insert(wearRows_.end(), look.grimeRows, look.grimeRows + kGrimeRows);  // [grime]
  }
}

void MeshRenderer::Belt(float amount) {
  if (amount < 0 || entries_.empty()) return;
  Entry& e = entries_.back();
  if (e.instance.bones[1] != 0 || e.instance.bones[2] != 0 || e.instance.bones[3] != 0) return;  // (a ghost's tint, treads)
  e.instance.bones[1] = -2;  // (the vertex shader: y -2, a belt that moves by z)
  e.instance.bones[2] = amount;
}

uint32_t MeshRenderer::AddGroup() {
  groups_.emplace_back();
  return static_cast<uint32_t>(groups_.size() - 1);
}

void MeshRenderer::ClearGroup(uint32_t group) {
  Group& g = groups_[group];
  g.entries.clear();
  g.culls.clear();
  g.cardDirty = true;  // [gpuscenery] (given to the card anew when next listed)
  g.rest.clear();
  g.restCulls.clear();
  g.lo = {1e30f, 1e30f, 1e30f};
  g.hi = {-1e30f, -1e30f, -1e30f};
}

void MeshRenderer::AddToGroup(uint32_t group, const Model* model, const Mat4& world) {
  if (!model || !model->ok) return;
  Entry e = MakeEntry(model, world, {1, 1, 1}, 1, nullptr);
  e.scenery = true;
  e.flags |= kScenery;
  Group& g = groups_[group];
  g.cardDirty = true;  // [gpuscenery]
  g.entries.push_back(e);
  g.culls.push_back({e.centre, e.radius, e.sizeRadius, e.flags});
  g.lo = {std::min(g.lo.x, e.centre.x - e.radius), std::min(g.lo.y, e.centre.y - e.radius), std::min(g.lo.z, e.centre.z - e.radius)};
  g.hi = {std::max(g.hi.x, e.centre.x + e.radius), std::max(g.hi.y, e.centre.y + e.radius), std::max(g.hi.z, e.centre.z + e.radius)};
}

void MeshRenderer::Draw(const View& view, gfx::Texture environment, bool shadowPass, bool glass, int part) {
  if (glass || part == 2) {
    // (counted with the scene's)
  } else if (shadowPass) {
    if (countedScene_) shadowTriangles = 0;
    countedScene_ = false;
  } else {
    drawnInstances = drawnTriangles = posedInstances = 0;
    countedScene_ = true;
  }
  if (!prepared_) Prepare({});  // (a caller that did not say which views there are: every posed model's bones)
  // What this view sees, each at the level its size on the SCREEN asks for (view.eye, fovY and height
  // are the camera's in a shadow pass too, so a model's shadow comes from a level near the one drawn).
  const char* outer = CpuTimes::Current();
  CpuTimes::Part("models: tested for each pass");
  items_.clear();
  const float focal = static_cast<float>(view.height) / (2 * std::tan(view.fovY * 0.5f));
  const float smallest = shadowPass ? shadowMinPixels : minPixels;
  const float wish = trianglesPerPixel * detail * (shadowPass ? shadowDetail : 1.0f) * 0.7854f;  // x pi / 4: a circle's area
  const uint8_t needed = glass ? kHasGlass : kHasSolid;  // (a model with no level of this pass's kind is passed over at once)
  // (the scenery's groups first, then the frame's own list: in pieces, each tested on its own)
  pieces_.clear();
  auto cut = [&](const Cull* culls, Entry* entries, size_t count) {
    for (size_t at = 0; at < count; at += kPieceModels) pieces_.push_back({culls + at, entries + at, std::min(kPieceModels, count - at)});
  };
  // [gpuscenery] a pass the card picked for: of the groups only what the card does not take is tested here
  const int cardPass = cardNow_ && !glass && part != 2 ? CardPassOf(view) : -1;
  if (part != 2)
    for (uint32_t g : groupsListed_) {
      Group& group = groups_[g];
      const std::vector<Cull>& groupCulls = cardPass >= 0 ? group.restCulls : group.culls;  // [gpuscenery]
      std::vector<Entry>& groupEntries = cardPass >= 0 ? group.rest : group.entries;
      if (groupCulls.empty()) continue;
      // (a group this pass does not see is passed over whole: its box holds every member's sphere, so
      // each member would fail the same test, Consider's, on its own)
      if (!view.BoxVisible(group.lo, group.hi)) continue;
      const Vec3 centre = (group.lo + group.hi) * 0.5f;
      const float radius = Length(group.hi - group.lo) * 0.5f;
      if (shadowPass && view.CoveredByNearer(centre, radius)) continue;
      if (view.reach > 0 && Length(centre - view.reachFrom) - radius > view.reach) continue;
      cut(groupCulls.data(), groupEntries.data(), groupCulls.size());
    }
  cut(culls_.data(), entries_.data(), culls_.size());
  if (pieceItems_.size() < pieces_.size()) pieceItems_.resize(pieces_.size());
  Jobs::Shared().Run(static_cast<int>(pieces_.size()), [&](int k) {
    const Piece& p = pieces_[static_cast<size_t>(k)];
    std::vector<Item>& out = pieceItems_[static_cast<size_t>(k)];
    out.clear();
    for (size_t i = 0; i < p.count; ++i) Consider(p.culls[i], p.entries[i], view, shadowPass, glass, part, needed, focal, smallest, wish, &out);
  });
  if (cardPass >= 0) {  // [gpuscenery] the card's lists first (so that, sorted, each level's list comes before the frame's own)
    const std::vector<CardScenery::Draw>& draws = card_->Draws(cardPass);
    for (size_t i = 0; i < draws.size(); ++i) {
      const CardScenery::Draw& d = draws[i];
      Entry* e = &cardEntries_[d.model_];
      items_.push_back({d.lod, e, -1.0f, d.near, SortKey(d.lod, d.model, d.near), static_cast<int32_t>(i)});
    }
  }
  for (size_t k = 0; k < pieces_.size(); ++k) items_.insert(items_.end(), pieceItems_[k].begin(), pieceItems_[k].end());
  if (!shadowPass)
    for (const Item& item : items_) posedInstances += item.entry->posed ? 1 : 0;
  if (motion && !shadowPass && !glass) {  // (the keyed models the scene draws, for DrawMotion; [leftover] and what sways, large enough)
    const float least = blastConstants && blastConstantsSize ? swayMotionPixelsBlasts : swayMotionPixels;
    for (const Item& item : items_) {
      if (item.card >= 0) continue;  // [gpuscenery] (the card lists what sways itself: its sway pass, DrawSwayMotion)
      if (item.entry->key) {
        motionItems_.push_back({item.lod, item.entry, item.bones});
      } else if (swayMotion && item.lod->sway > 0) {
        // (how tall one of its trees stands on the screen: its height in the world, as the shader's sway counts it, over the
        // distance; a group of trees is one model, but each tree sways by its own height)
        const float* r0 = item.entry->instance.row0;
        const float* r1 = item.entry->instance.row1;
        const float* r2 = item.entry->instance.row2;
        const float tall = item.lod->swayHeight * std::sqrt(r0[0] * r0[0] + r1[0] * r1[0] + r2[0] * r2[0]);
        if (tall * focal >= least * std::max(Length(Vec3{r0[3], r1[3], r2[3]} - view.eye), 0.01f)) swayItems_.push_back({item.lod, item.entry, item.bones});
      }
    }
  }
  if (marks_ && !shadowPass && !glass)  // (hidden outlines: the marked models the scene draws, for DrawMarked)
    for (const Item& item : items_)
      if (item.entry->mark) markedItems_.push_back({item.lod, item.entry, item.bones});
  DrawItems(view, environment, shadowPass, glass);
  CpuTimes::Part(outer);
}

// How many pixels across a model must be on screen to have its depth laid first (depthPrepass).
constexpr float kPrepassPixels = 32;

// Whether a pass draws that model, and at which level: onto items_ if it does.
void MeshRenderer::Consider(const Cull& c, Entry& e, const View& view, bool shadowPass, bool glass, int part, uint8_t needed, float focal,
                            float smallest, float wish, std::vector<Item>* out) const {
  {
    if (!(c.flags & needed)) return;
    if (part != 0 && ((c.flags & kScenery) != 0) != (part == 1)) return;
    const Vec3 r{c.radius, c.radius, c.radius};
    if (!view.BoxVisible(c.centre - r, c.centre + r)) return;
    if (shadowPass && view.CoveredByNearer(c.centre, c.radius)) return;  // the nearer shadow map has it
    if (view.reach > 0 && Length(c.centre - view.reachFrom) - c.radius > view.reach) return;  // beyond a lamp's light
    if (view.hasCarrier && Length(c.centre - view.carrier) < 0.02f) return;                   // what the lamp rides on
    const float pixels = 2 * c.sizeRadius * focal / std::max(Length(c.centre - view.eye), 0.01f);  // its diameter on screen
    if (pixels < smallest) return;
    // [lodscale] the level is chosen as if it stood view.lodScale times as far (the camera's LOD scale; 1: its size)
    const float sized = pixels / view.lodScale;
    const float wanted = wish * sized * sized;
    // the level whose triangle count is nearest to that wish (nearest by ratio: a step down is taken
    // at the geometric mean of two neighbours, so chains with big gaps - a tree and its flat card -
    // behave as well as even ones)
    const std::vector<ModelLod>& lods = e.model->lods;
    size_t level = 0;
    const std::vector<float>& sizes = e.model->levelPixels;
    if (!sizes.empty()) {
      // built to the standard: each level down to the size it was made for (the quality setting and
      // the shadows' coarser wish count as a smaller size)
      const float size = sized * 0.75f * std::sqrt(detail * (shadowPass ? shadowDetail : 1.0f));
      while (level < sizes.size() && level + 1 < lods.size() && size < sizes[level]) ++level;
    } else {
      while (level + 1 < lods.size() &&
             wanted * wanted < static_cast<float>(lods[level].indexCount / 3) * static_cast<float>(lods[level + 1].indexCount / 3))
        ++level;
    }
    const ModelLod* lod = &lods[level];
    if (lod->hidden || lod->glass != glass) return;
    if (lod->stream && divertStream) return;  // [stream] (the streams' own pass draws it: stream_water.h)
    // the far-away cards of trees are flat pictures: they cast no shadow
    if (shadowPass && lod->blend) return;
    if (shadowPass && lod->noShadow) return;  // (a shader of the original's that casts none)
    // (near: so large on screen that its leaves and parts lie over each other on many pixels)
    const bool near = pixels >= kPrepassPixels;
    out->push_back({lod, &e, e.posed && e.rows >= 0 ? static_cast<float>(e.rows) : -1.0f, near, SortKey(lod, e.model, near)});
  }
}

// items_ by mesh (by its level's serial number), the blended ones last, and otherwise in the order
// they were found: what a stable sort by (blended, serial) gives, done as a radix sort of the
// key's bits, eleven at a time from the lowest (which keeps each step's order).
void MeshRenderer::SortItems() {
  const size_t n = items_.size();
  // [plants] (with plants that react, what is drawn by their variants after the rest, together: not a change of shaders
  // between each two meshes; without it nothing reacts, and the order is as it was)
  const bool plants = plantsConstants && plantsMade_ > 0;
  auto reacts = [plants](const ModelLod* l) { return plants && (l->sway > 0 || (l->wreck && l->leaves)); };
  // [undersnow] (the low plants under lying snow likewise, after the rest, and after what reacts that is not under it; without
  // snow none sinks, and the order is as it was)
  auto sinks = [this](const Item& item) { return Sinks(item.lod, item.entry->model); };
  if (n < 64) {
    std::stable_sort(items_.begin(), items_.end(), [&](const Item& a, const Item& b) {
      const bool ra = reacts(a.lod) || sinks(a), rb = reacts(b.lod) || sinks(b);
      return a.lod->blend != b.lod->blend ? b.lod->blend : ra != rb ? rb : sinks(a) != sinks(b) ? sinks(b) : a.lod->serial != b.lod->serial ? a.lod->serial < b.lod->serial : a.near && !b.near;
    });
    return;
  }
  // [mcrec] (each item's key worked out where it was found, Consider: SortKey; going to every item's level and model for it
  // in each of the sort's steps was most of the sort's time)
  auto key = [](const Item& item) { return item.key; };
  // (only the bits that differ between the items need sorting)
  uint64_t all = 0, any = 0, first = key(items_[0]);
  for (const Item& item : items_) {
    const uint64_t k = key(item);
    any |= k ^ first;
  }
  (void)all;
  if (!any) return;  // one mesh only
  int top = 63;
  while (top > 0 && !((any >> top) & 1)) --top;
  int low = 0;
  while (low < top && !((any >> low) & 1)) ++low;
  constexpr int kBits = 11;
  constexpr uint32_t kBuckets = 1u << kBits;
  buckets_.resize(kBuckets);
  sorting_.resize(n);
  std::vector<Item>* from = &items_;
  std::vector<Item>* to = &sorting_;
  for (int shift = low; shift <= top; shift += kBits) {
    std::fill(buckets_.begin(), buckets_.end(), 0u);
    for (const Item& item : *from) ++buckets_[(key(item) >> shift) & (kBuckets - 1)];
    uint32_t sum = 0;
    for (uint32_t& b : buckets_) {
      const uint32_t c = b;
      b = sum;
      sum += c;
    }
    for (const Item& item : *from) (*to)[buckets_[(key(item) >> shift) & (kBuckets - 1)]++] = item;
    std::swap(from, to);
  }
  if (from != &items_) items_.swap(sorting_);
}

// [mcrec] What SortItems sorts an item by: blended last; then what reacts or sinks under the snow after the rest, and what
// sinks after what only reacts; then by mesh (its level's serial number), the large on screen first.
uint64_t MeshRenderer::SortKey(const ModelLod* lod, const Model* model, bool near) const {
  const bool plants = plantsConstants && plantsMade_ > 0;
  const bool reacts = plants && (lod->sway > 0 || (lod->wreck && lod->leaves));  // [plants]
  const bool sunk = Sinks(lod, model);                                             // [undersnow]
  return (static_cast<uint64_t>(lod->blend ? 1 : 0) << 34) | (static_cast<uint64_t>(reacts || sunk ? 1 : 0) << 33) |
         (static_cast<uint64_t>(sunk ? 1 : 0) << 32) | (static_cast<uint64_t>(lod->serial) << 1) | (near ? 0u : 1u);
}

// Draws items_: by mesh, each mesh's instances together.
void MeshRenderer::DrawItems(const View& view, gfx::Texture environment, bool shadowPass, bool glass) {
  if (items_.empty()) return;
  CpuTimes::Part("models: sorted by mesh");
  // by mesh, the blended ones last (they are not sorted by distance among themselves yet)
  SortItems();

  CpuTimes::Part("models: handed to the card");
  device_->SetPipeline(shadowPass ? shadow_ : pipeline_);
  device_->SetShaderBuffer(boneBuffer_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const bool blasts = blastConstants && blastConstantsSize && blastsMade_ > 0;  // (the blasts' waves: for what sways)
  if (blasts) device_->SetConstants(1, blastConstants, blastConstantsSize);
  // [plants] (plants that react: their textures; with no waves under way, none in the waves' list its shaders read)
  const bool plants = plantsConstants && plantsMade_ > 0;
  if (plants) {
    static const float kNoBlasts[4] = {0, 0, 0, 0};
    if (!blasts) device_->SetConstants(1, kNoBlasts, sizeof kNoBlasts);
    device_->SetTexture(8, plantsPress ? plantsPress : textures_->Black());
    device_->SetTexture(9, plantsBurnt ? plantsBurnt : textures_->Black());
  }
  // [undersnow] (the low plants under lying snow: their shaders have the blasts' waves and plants that react in them, each
  // left out by its constants when off; the snow ground's shelter, read only where it is there, and the ground's normals)
  float snowK[12] = {};
  const bool snowing = underSnow.depth > 0 && snowMade_ > 0;
  if (snowing) {
    static const float kNoBlasts[4] = {0, 0, 0, 0};
    if (!blasts && !plants) device_->SetConstants(1, kNoBlasts, sizeof kNoBlasts);
    if (!plants) {
      device_->SetTexture(8, textures_->Black());
      device_->SetTexture(9, textures_->Black());
    }
    device_->SetTexture(10, underSnow.shelter ? underSnow.shelter : textures_->Black());
    device_->SetTexture(11, underSnow.groundNormals ? underSnow.groundNormals : textures_->FlatNormal());
    UnderSnowConstants(snowK);
  }
  if (!shadowPass) {
    device_->SetTexture(3, environment ? environment : textures_->GreyCube());
    if (!insectLookup_) {  // (once: the two lookups, or white where the game's files have none)
      insectLookup_ = textures_->Get("/textures/engine/insectlookup.dds", gfx::Texture{});
      haveInsectLookup_ = static_cast<bool>(insectLookup_);
      if (!insectLookup_) insectLookup_ = textures_->White();
      seraphimFalloff_ = textures_->Get("/textures/environment/falloff_seraphim_lookup.dds", gfx::Texture{});
      haveSeraphimFalloff_ = static_cast<bool>(seraphimFalloff_);
      if (!seraphimFalloff_) seraphimFalloff_ = textures_->White();
    }
    device_->SetTexture(5, insectLookup_);
    device_->SetTexture(6, seraphimFalloff_);
    if (!triedShieldPatterns_) {  // (once: the visuals track's lattice for each faction's personal shields)
      triedShieldPatterns_ = true;
      std::vector<std::string> paths;
      for (const char* faction : {"uef", "aeon", "cybran", "seraphim"})
        paths.push_back(std::string(kRemasteredEffects) + "/effects/entities/personalshield/" + faction + "/personalshield_pattern.dds");
      std::vector<bool> present;
      shieldPatterns_ = textures_->Layers(paths, &present);
      for (size_t f = 0; f < 4; ++f) shieldPatternOf_[f] = shieldPatterns_ && present[f];
    }
    if (shieldPatterns_) device_->SetTexture(7, shieldPatterns_);
    // [grime] the units' shared grime layers (unit_grime.h), made the first time a unit wears grime
    if (grimeWanted_ && !grimeLayers_) grimeLayers_ = MakeGrimeLayers(device_, textures_);
    if (grimeLayers_) device_->SetTexture(24, grimeLayers_);
  }

  // (the scene's solid models: their depth first, then in full; a level of a look of its own - see-
  // through, unlit, a shield, a frame of light - is drawn in full alone)
  RunSetup s;
  s.shadowPass = shadowPass;
  s.glass = glass;
  s.prepass = depthPrepass && !shadowPass && !glass && prepass_ && prepassLeaves_;
  s.blasts = blasts;
  s.plants = plants;
  s.snowing = snowing;
  std::copy(snowK, snowK + 12, s.snowK);
  s.cardPass = cardNow_ && !glass ? CardPassOf(view) : -1;  // [gpuscenery]
  bool anyCard = false;
  // [mcrec] the pass's draws, in their order: each mesh's instances in draws of up to kMaxInstances, the depth round's first
  // (in the depth round only what is large on screen, where leaves and parts lie over each other; for what is small,
  // drawing it twice costs more than the shading it saves)
  runs_.clear();
  for (int round = s.prepass ? 0 : 1; round < 2; ++round) {
    const bool depthOnly = round == 0;
    for (size_t at = 0; at < items_.size();) {
      const ModelLod* lod = items_[at].lod;
      const bool near = items_[at].near;
      if (depthOnly && !(Plain(lod) && near)) {  // (not in the depth round)
        while (at < items_.size() && items_[at].lod == lod && items_[at].near == near) ++at;
        continue;
      }
      if (items_[at].card >= 0) {  // [gpuscenery] (one of the card's lists: a draw of its own)
        if (s.cardPass >= 0) runs_.push_back({static_cast<uint32_t>(at), 1, depthOnly, items_[at].card});
        anyCard = anyCard || s.cardPass >= 0;
        ++at;
        continue;
      }
      size_t n = 0;
      while (at + n < items_.size() && items_[at + n].lod == lod && items_[at + n].near == near && items_[at + n].card < 0 && n < kMaxInstances) ++n;
      runs_.push_back({static_cast<uint32_t>(at), static_cast<uint32_t>(n), depthOnly});
      at += n;
    }
  }
  // [mcrec] recorded on several threads, in stretches of the draws in their order, one recorder each (gfx.h, BeginParallel:
  // each starts from what was set above, and every draw sets what it reads beyond that); or here, one after the other
  RunCounts counts;
  int stretches = Stretches();
  // (cut where each stretch has about its share of the work: a draw's own, and its instances' copy; none left empty, so
  // that the last recorder is the one that drew last)
  size_t cut[kMostStretches + 1] = {};
  if (stretches > 1) {
    double total = 0;
    for (const Run& r : runs_) total += kRunWeight + r.n;
    double sum = 0;
    int k = 1;
    for (size_t i = 0; i + 1 < runs_.size() && k < stretches; ++i) {
      sum += kRunWeight + runs_[i].n;
      if (sum >= total * k / stretches) cut[k++] = i + 1;
    }
    stretches = k;
    cut[k] = runs_.size();
  }
  if (stretches <= 1 || !device_->BeginParallel(stretches)) {
    RecordRuns(*device_, s, 0, runs_.size(), batch_, &counts);
  } else {
    static bool told = false;  // (once: that it does)
    if (!told) printf("render: a pass's draws recorded on %d threads at once (%s)\n", stretches, device_->BackendName());
    told = true;
    if (chunkBatches_.size() < static_cast<size_t>(stretches)) chunkBatches_.resize(static_cast<size_t>(stretches));
    RunCounts each[kMostStretches];
    Jobs::Shared().Run(stretches, [&](int i) {
      std::vector<Instance>& batch = chunkBatches_[static_cast<size_t>(i)];
      if (batch.size() < kMaxInstances) batch.resize(kMaxInstances);
      RecordRuns(*device_->Parallel(i), s, cut[i], cut[i + 1], batch, &each[i]);
    });
    device_->EndParallel();
    for (int i = 0; i < stretches; ++i) {
      counts.instances += each[i].instances;
      counts.triangles += each[i].triangles;
      counts.shadowTriangles += each[i].shadowTriangles;
    }
  }
  shadowTriangles += counts.shadowTriangles;
  drawnInstances += counts.instances;
  drawnTriangles += counts.triangles;
  if (anyCard) device_->SetShaderBuffer(boneBuffer_);  // [gpuscenery] (the bones again in their slot, as before the card's draws)
}

// [mcrec] How many stretches a pass's draws are recorded in: one for every kStretchWeight of work, at most as many as there
// are threads to record them (the jobs pool's workers and the thread drawing) and recorders (1: one, as before).
int MeshRenderer::Stretches() const {
  if (recordThreads == 1 || runs_.size() < 2) return 1;
  const int threads = recordThreads > 0 ? recordThreads : Jobs::Shared().Workers() + 1;
  const int most = std::min({threads, device_->ParallelRecorders(), kMostStretches});
  if (most <= 1) return 1;
  double total = 0;
  for (const Run& r : runs_) total += kRunWeight + r.n;
  return std::clamp(static_cast<int>(total / kStretchWeight), 1, most);
}

// The levels drawn as the plain shading draws them (the depth round lays theirs first, depthPrepass).
bool MeshRenderer::Plain(const ModelLod* l) {
  return !l->blend && !l->glass && !l->impact && !l->unlit && !l->nuke && !l->fades && !l->shield && !l->frame;
}

// [mcrec] Draws runs_[from, end) into `to`: the device itself or one of its recorders (gfx.h, Recorder), with `batch` for the
// instances' copy. The same calls either way.
template <class R>
void MeshRenderer::RecordRuns(R& to, const RunSetup& s, size_t from, size_t end, std::vector<Instance>& batch, RunCounts* counts) const {
  const bool shadowPass = s.shadowPass, glass = s.glass, blasts = s.blasts, plants = s.plants, snowing = s.snowing;
  int drawing = -1;  // which pipeline is set: 0 the plain one (or the shadows'), 1 leaves, 2 blended, ... (none at a round's start)
  bool cardBound = false;  // [gpuscenery] the card's buffer in the bones' slot (each stretch begins with the bones there)
  for (size_t r = from; r < end; ++r) {
    const Run& run = runs_[r];
    const bool depthOnly = run.depthOnly;
    const bool card = run.card >= 0;  // [gpuscenery]
    if (r > from && depthOnly != runs_[r - 1].depthOnly) drawing = -1;
    const size_t at = run.at, n = run.n;
    const ModelLod* lod = items_[at].lod;
    const bool near = items_[at].near;
    const bool early = Plain(lod) && near;
    const bool laid = s.prepass && !depthOnly && early;  // (its depth laid in the round before)
    const int wanted = depthOnly ? (lod->leaves ? 7 : 6)
                       : laid ? (lod->leaves ? 9 : 8)
                       : shadowPass ? 0 : glass ? (lod->impact == 1 || lod->shell ? 5 : lod->nuke == 1 ? (lod->rgbOnly ? 11 : 4) : lod->rgbOnly ? 10 : 3)
                       : lod->blend ? 2 : lod->leaves ? 1 : 0;
    const bool bends = blasts && lod->sway > 0;  // (what sways, while blasts' waves are under way: the shaders with them)
    const bool reacts = plants && (lod->sway > 0 || (lod->wreck && lod->leaves));  // [plants] (and the game's burnt trees)
    const bool charred = reacts && shadowPass && lod->wreck;  // [plants] (a burnt tree's shadow: its own variant)
    const bool sinks = snowing && Sinks(lod, items_[at].entry->model);  // [undersnow] (a low plant under lying snow)
    const int variant = (sinks ? 400 : charred ? 300 : reacts ? 200 : bends ? 100 : 0) + (card ? 1000 : 0);
    if (wanted + variant != drawing) {
      gfx::Pipeline p = wanted == 11 ? domeRgb_ : wanted == 10 ? glassRgb_ : wanted == 9 ? laidLeaves_ : wanted == 8 ? laid_ : wanted == 7 ? prepassLeaves_ :
                        wanted == 6 ? prepass_ : wanted == 5 ? adding_ : wanted == 4 ? dome_ : wanted == 3 ? glass_ : wanted == 2 ? blended_ :
                        wanted == 1 ? leaves_ : shadowPass ? shadow_ : pipeline_;
      if (sinks) {  // [undersnow]
        for (const Bending& b : bending_)
          if (b.original.id == p.id && b.snow) p = b.snow;
      } else if (reacts) {  // [plants]
        for (const Bending& b : bending_)
          if (b.original.id == p.id && b.plants) p = charred && b.plantsBurnt ? b.plantsBurnt : b.plants;
      } else if (bends) {
        for (const Bending& b : bending_)
          if (b.original.id == p.id && b.bending) p = b.bending;
      }
      if (!bends && !reacts && remasteredBuild && buildMade_ > 0)  // (buildfx: the remastered build look's variant)
        for (const BuildVariant& b : buildVariants_)
          if (b.original.id == p.id && b.variant) p = b.variant;
      if (card) p = CardOf(p);  // [gpuscenery] (the same, reading its instances from the card's list)
      if (card && !p) {  // ([gpuscenery] none made: CardPipelines says the card is not ready then, so never; drawn by nothing rather than the wrong one)
        drawing = -1;
        continue;
      }
      to.SetPipeline(p);
      drawing = wanted + variant;
    }
    if (card != cardBound) {  // [gpuscenery]
      to.SetShaderBuffer(card ? card_->Buffer() : boneBuffer_, 0);
      cardBound = card;
    }
    if (!card)
      for (size_t i = 0; i < n; ++i) {
        batch[i] = items_[at + i].entry->instance;
        batch[i].bones[0] = items_[at + i].bones;
      }
    float kind[48] = {lod->alphaCut, lod->shell ? 12.0f : lod->impact ? 9.0f + static_cast<float>(lod->impact) : lod->unlit ? 6.0f + static_cast<float>(lod->unlit) : lod->nuke ? 4.0f + static_cast<float>(lod->nuke) : lod->fades ? 4.0f : lod->shield ? 3.0f : lod->frame ? 2.0f : lod->vertexNormals ? 1.0f : 0.0f, lod->teamMask ? 0.0f : 1.0f, lod->labPbr ? 1.0f : 0.0f,
                            static_cast<float>(lod->faction), lod->leaves ? 1.0f : 0.0f, lod->plainMaterial ? 1.0f : 0.0f, lod->border ? 1.0f : 0.0f,
                            lod->sway, 1 / lod->swayHeight, 0, 1,
                            lod->chrome ? 2.0f : lod->wreck ? 1.0f : 0.0f, lod->shieldTint.x, lod->shieldTint.y, lod->shieldTint.z};
    if (lod->nuke) {
      kind[12] = lod->remasteredEffect ? 1.0f : 0.0f;  // (the remastered look's ball of fire)
      kind[13] = lod->nukeShell.x;
      kind[14] = lod->nukeShell.y;
      kind[15] = lod->nukeShell.z;
    }
    {  // (the model's foot and height, for a unit under construction)
      const Model* of = items_[at].entry->model;
      kind[10] = of->boundsMin.y;
      kind[11] = 1 / std::max(of->boundsMax.y - of->boundsMin.y, 1e-3f);
    }
    kind[16] = haveInsectLookup_ ? 1.0f : 0.0f;
    kind[18] = static_cast<float>(lod - items_[at].entry->model->lods.data());  // (which level this is, for the checking view)
    kind[18] += (lod->wearMasks ? 0.25f : 0.0f) + (lod->damageAlbedo ? 0.5f : 0.0f);  // [grime] (its wear art: unit_grime.h)
    kind[17] = haveSeraphimFalloff_ ? 1.0f : 0.0f;
    kind[19] = !shadowPass && lod->faction >= 0 && lod->faction < 4 && shieldPatternOf_[lod->faction] ? 1.0f : 0.0f;
    kind[20] = lod->scrolling ? (lod->treadDepth > 0 ? 2 + lod->treadDepth : 1.0f) : 0.0f;  // [treads] (2 + their grooves' depth)
    kind[21] = lod->rgbOnly ? 1.0f : 0.0f;
    kind[22] = lod->sway > 0 ? lod->treeStep : 0.0f;  // [foliage] (a group's trees, each on its own beat)
    kind[23] = static_cast<float>(gSceneSamples);     // [foliage] (NearFade: leaves fade by their coverage)
    kind[26] = -1;  // [machinery] (kind12: a factory's belt, MachinerySpec)
    if (const MachinerySpec* m = items_[at].entry->model->machinery.get(); m && m->beltBone >= 0) {
      kind[24] = m->beltFrom;
      kind[25] = m->beltLength;
      kind[26] = static_cast<float>(m->beltBone);
      kind[27] = static_cast<float>(m->beltAxis);
    }
    if (reacts) {  // [plants] (kind7, kind8; the game's trees neither pressed nor burnt by the picture: Model::simTree)
      std::copy(plantsConstants, plantsConstants + 8, kind + 28);
      if (items_[at].entry->model->simTree) kind[34] = kind[35] = 0;
      if (shadowPass) kind[34] = 0;  // (shadows: bent as the plants are, but not burnt: it would cost the sun's maps too much)
    }
    if (sinks) std::copy(s.snowK, s.snowK + 12, kind + 36);  // [undersnow] (kind9..11; kind7, kind8 nought when nothing reacts)
    to.SetConstants(3, kind, static_cast<uint32_t>(sizeof(float) * (sinks ? 48 : reacts ? 36 : 28)));
    to.SetTexture(0, lod->albedo);
    if (!shadowPass) {
      to.SetTexture(1, lod->normals);
      to.SetTexture(2, lod->specular);
      to.SetTexture(4, lod->secondary);
      to.SetTexture(22, lod->damageAlbedo);  // [grime] (none but on a unit with its damage set)
      to.SetTexture(23, lod->damageNormal);
    }
    to.SetVertexBuffer(lod->vertices);
    to.SetIndexBuffer(lod->indices);
    if (card) {  // [gpuscenery] the card's list: as many as the card found, its record's arguments
      const uint32_t record = card_->Draws(s.cardPass)[static_cast<size_t>(run.card)].record;
      const uint32_t pick[4] = {record / 4, 0, 0, 0};
      to.SetConstants(2, pick, sizeof pick);
      to.DrawIndexedIndirect(card_->Buffer(), record);
      continue;
    }
    // only the instances drawn are handed over: the shader reads no further into its array
    to.SetConstants(2, batch.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    to.DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    const int triangles = static_cast<int>(n) * static_cast<int>(lod->indexCount / 3);
    if (depthOnly) {
    } else if (shadowPass) {
      counts->shadowTriangles += triangles;
    } else {
      counts->instances += static_cast<int>(n);
      counts->triangles += triangles;
    }
  }
}

bool MeshRenderer::PrepareBlasts() {
  if (blastsMade_) return blastsMade_ > 0;
  blastsMade_ = -1;
  for (Bending& b : bending_) {
    if (!b.original) continue;
    gfx::PipelineDesc d = b.desc;
    d.vertexSource = d.pixelSource = blastSource_;
    std::string error;
    b.bending = device_->CreatePipeline(d, &error);
    if (!b.bending) {
      fprintf(stderr, "render: models cannot bend in blasts' waves (%s)\n", error.c_str());
      return false;
    }
  }
  blastsMade_ = 1;
  return true;
}

// [plants] The variants of what sways with plants that react (and the blasts' waves), made the first time the world wants them.
bool MeshRenderer::PreparePlants() {
  if (plantsMade_) return plantsMade_ > 0;
  plantsMade_ = -1;
  for (Bending& b : bending_) {
    if (!b.original) continue;
    gfx::PipelineDesc d = b.desc;
    d.vertexSource = d.pixelSource = b.original.id == shadow_.id ? plantsShadowSource_ : plantsSource_;
    std::string error;
    b.plants = device_->CreatePipeline(d, &error);
    if (b.plants && b.original.id == shadow_.id) {
      d.vertexSource = d.pixelSource = plantsShadowBurntSource_;
      b.plantsBurnt = device_->CreatePipeline(d, &error);
    }
    if (!b.plants || (b.original.id == shadow_.id && !b.plantsBurnt)) {
      fprintf(stderr, "render: plants cannot react in the models' shaders (%s)\n", error.c_str());
      return false;
    }
  }
  plantsMade_ = 1;
  return true;
}

// [undersnow] The variants that draw the low plants under lying snow (under_snow.h), and their motion's, made the first time
// the world wants them (the first frame the option is on with the weather: not when the first snow lies, a stall then).
bool MeshRenderer::PrepareUnderSnow() {
  if (snowMade_) return snowMade_ > 0;
  snowMade_ = -1;
  const auto start = std::chrono::steady_clock::now();
  for (Bending& b : bending_) {
    if (!b.original) continue;
    gfx::PipelineDesc d = b.desc;
    d.vertexSource = d.pixelSource = b.original.id == shadow_.id ? snowShadowSource_ : snowSource_;
    std::string error;
    b.snow = device_->CreatePipeline(d, &error);
    if (!b.snow) {
      fprintf(stderr, "render: the low plants cannot go under the snow in the models' shaders (%s)\n", error.c_str());
      return false;
    }
  }
  // (their motion: as what sways, kSwayMotionShader, with the snow in it)
  const std::string source = std::string("#define PLANTS_PRESS t8\n#define PLANTS_BURN t9\n#define UNDER_SNOW 1\n") + kViewShaderBlock + kBlastShaderBlock +
                             kPlantsShaderBlock + kUnderSnowShaderBlock + kSwayMotionShader;
  gfx::PipelineDesc md;
  md.name = "models, what sways under the snow, motion";
  md.vertexSource = md.pixelSource = source;
  md.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
              {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)},
              {"BLENDINDICES", 0, gfx::AttrType::UByte4, offsetof(ScmVertex, bones)}};
  md.stride = sizeof(ScmVertex);
  md.colorFormat = gfx::Format::RG16F;
  md.cull = gfx::Cull::None;
  md.depthWrite = false;
  md.depthBias = 16;
  std::string error;
  cardSources_[9] = "#define CARD_SCENERY 1\n" + source;  // [gpuscenery] (its variant for the card's lists)
  swaySnowMotionDesc_ = md;
  swaySnowMotionPipeline_ = device_->CreatePipeline(md, &error);
  if (!swaySnowMotionPipeline_) fprintf(stderr, "render: the motion of the low plants under the snow cannot be drawn (%s)\n", error.c_str());
  snowMade_ = 1;
  printf("render: the low plants under the snow: their shaders made, %.0f ms\n",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return true;
}

// [undersnow] under_snow.h's k0, k1, k2 from underSnow (twelve floats)
void MeshRenderer::UnderSnowConstants(float* to) const {
  const float wind = std::sqrt(underSnow.wind[0] * underSnow.wind[0] + underSnow.wind[1] * underSnow.wind[1]);
  const float k[12] = {underSnow.depth, underSnow.shelter ? 1.0f : 0.0f, underSnow.tallest, underSnow.shelter && underSnow.materials ? 1.0f : 0.0f,
                       wind > 0 ? underSnow.wind[0] / wind : 0.0f, wind > 0 ? underSnow.wind[1] / wind : 0.0f, underSnow.tune[0], underSnow.tune[1],
                       1 / std::max(underSnow.mapSize[0], 1.0f), 1 / std::max(underSnow.mapSize[1], 1.0f), 0, 0};
  std::copy(k, k + 12, to);
}

// (buildfx) The remastered build look's variants of the pipelines that may draw something under construction, made the
// first time the world wants them.
bool MeshRenderer::PrepareBuild() {
  if (buildMade_) return buildMade_ > 0;
  buildMade_ = -1;
  for (BuildVariant& b : buildVariants_) {
    gfx::PipelineDesc d = b.desc;
    d.vertexSource = d.pixelSource = buildSource_;
    std::string error;
    b.variant = device_->CreatePipeline(d, &error);
    if (!b.variant) {
      fprintf(stderr, "render: the remastered build look cannot be drawn (%s)\n", error.c_str());
      return false;
    }
  }
  buildMade_ = 1;
  return true;
}

void MeshRenderer::DrawMotion(const View& view, const Mat4& now, const Mat4& before, float timeBefore) {
  const bool cardSway = cardNow_ && cardSway_ >= 0 && !card_->Draws(cardSway_).empty();  // [gpuscenery]
  if (motion && swayMotion && (!swayItems_.empty() || cardSway)) DrawSwayMotion(view, now, before, timeBefore);  // [leftover]
  if (!motion || motionItems_.empty() || !motionPipeline_) return;
  // by level, so that each level's instances go together
  std::stable_sort(motionItems_.begin(), motionItems_.end(), [](const MotionItem& a, const MotionItem& b) { return a.lod < b.lod; });
  device_->SetPipeline(motionPipeline_);
  device_->SetShaderBuffer(boneBuffer_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  const Mat4 views[2] = {now, before};
  device_->SetConstants(1, views, sizeof views);
  for (size_t at = 0; at < motionItems_.size();) {
    const ModelLod* lod = motionItems_[at].lod;
    size_t n = 0;
    while (at + n < motionItems_.size() && motionItems_[at + n].lod == lod && n < kMaxInstances) {
      const MotionItem& item = motionItems_[at + n];
      batch_[n] = item.entry->instance;
      batch_[n].bones[0] = item.bones;
      // (its place of the frame before, as Prepare kept it; unknown: it is taken to stand where it is)
      const size_t index = static_cast<size_t>(item.entry - entries_.data());
      lastBatch_[n] = index < lastOf_.size() ? lastOf_[index] : LastInstance{};
      ++n;
    }
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->SetConstants(3, lastBatch_.data(), static_cast<uint32_t>(n * sizeof(LastInstance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    at += n;
  }
}

// [leftover] What sways, its motion (swayMotion; kSwayMotionShader): into DrawMotion's pass, by level.
void MeshRenderer::DrawSwayMotion(const View& view, const Mat4& now, const Mat4& before, float timeBefore) {
  if (!swayMotionTried_) {  // (made the first time: with the blasts' waves and plants that react in it, each off by its constants)
    swayMotionTried_ = true;
    const std::string source = std::string("#define PLANTS_PRESS t8\n#define PLANTS_BURN t9\n") + kViewShaderBlock + kBlastShaderBlock + kPlantsShaderBlock +
                               kSwayMotionShader;
    gfx::PipelineDesc md;
    md.name = "models, what sways, motion";
    md.vertexSource = md.pixelSource = source;
    md.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)},
                {"TEXCOORD", 0, gfx::AttrType::Float4, offsetof(ScmVertex, uv0)},
                {"BLENDINDICES", 0, gfx::AttrType::UByte4, offsetof(ScmVertex, bones)}};
    md.stride = sizeof(ScmVertex);
    md.colorFormat = gfx::Format::RG16F;
    md.cull = gfx::Cull::None;  // (leaves are seen from both sides)
    md.depthWrite = false;
    md.depthBias = 16;  // (as the keyed models' motion: the same placement compiled in another shader may land a hair farther)
    std::string error;
    cardSources_[8] = "#define CARD_SCENERY 1\n" + source;  // [gpuscenery] (its variant for the card's lists)
    swayMotionDesc_ = md;
    swayMotionPipeline_ = device_->CreatePipeline(md, &error);
    if (!swayMotionPipeline_) fprintf(stderr, "render: the motion of what sways cannot be drawn (%s)\n", error.c_str());
  }
  if (!swayMotionPipeline_) return;
  // by level, so that each level's instances go together
  std::stable_sort(swayItems_.begin(), swayItems_.end(), [](const MotionItem& a, const MotionItem& b) { return a.lod < b.lod; });
  device_->SetPipeline(swayMotionPipeline_);
  device_->SetShaderBuffer(boneBuffer_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  // (the blasts' waves as the scene's pass had them, else none in their list; the plants' textures, black when they do not react)
  const bool blasts = blastConstants && blastConstantsSize && blastsMade_ > 0;
  static const float kNoBlasts[4] = {0, 0, 0, 0};
  if (blasts) device_->SetConstants(1, blastConstants, blastConstantsSize);
  else device_->SetConstants(1, kNoBlasts, sizeof kNoBlasts);
  const bool plants = plantsConstants && plantsMade_ > 0;
  device_->SetTexture(8, plants && plantsPress ? plantsPress : textures_->Black());
  device_->SetTexture(9, plants && plantsBurnt ? plantsBurnt : textures_->Black());
  struct {
    Mat4 now, before;
    float when[4];
    float kind[4], kind2[4], kind3[4], kind6[4], kind7[4], kind8[4];
    float snow[12];  // [undersnow] (kind9..11: read by the variant under the snow alone)
  } k{now, before, {timeBefore, view.constants.time - timeBefore, plants ? 1.0f : 0.0f, 0}, {}, {}, {}, {}, {}, {}, {}};
  // [undersnow] (the low plants under lying snow: by a variant of their own, the snow as the scene's pass had it)
  const bool snowing = underSnow.depth > 0 && snowMade_ > 0 && swaySnowMotionPipeline_;
  if (snowing) {
    device_->SetTexture(10, underSnow.shelter ? underSnow.shelter : textures_->Black());
    device_->SetTexture(11, underSnow.groundNormals ? underSnow.groundNormals : textures_->FlatNormal());
    UnderSnowConstants(k.snow);
  }
  // a level's constants and what it reads (its albedo, its mesh)
  auto level = [&](const ModelLod* lod, const Model* of, bool sinks) {
    // (under the snow the plants' own sway as the scene's variant has it: PlantsUpright always, the rest off by kind8)
    k.when[2] = plants || sinks ? 1.0f : 0.0f;
    k.kind[0] = lod->alphaCut;
    k.kind2[1] = lod->leaves ? 1.0f : 0.0f;
    k.kind3[0] = lod->sway;
    k.kind3[1] = 1 / lod->swayHeight;
    // [undersnow] (the model's lowest point and its whole height, as the scene's MeshKind has them)
    k.kind3[2] = of->boundsMin.y;
    k.kind3[3] = 1 / std::max(of->boundsMax.y - of->boundsMin.y, 1e-3f);
    k.kind6[2] = lod->treeStep;
    if (plants) {  // (as DrawItems hands them: the game's trees neither pressed nor burnt by the picture)
      std::copy(plantsConstants, plantsConstants + 4, k.kind7);
      std::copy(plantsConstants + 4, plantsConstants + 8, k.kind8);
      if (of->simTree) k.kind8[2] = k.kind8[3] = 0;
    }
    device_->SetConstants(3, &k, sizeof k);
    device_->SetTexture(0, lod->albedo);
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
  };
  bool sinking = false;
  for (size_t at = 0; at < swayItems_.size();) {
    const ModelLod* lod = swayItems_[at].lod;
    size_t n = 0;
    while (at + n < swayItems_.size() && swayItems_[at + n].lod == lod && n < kMaxInstances) {
      const MotionItem& item = swayItems_[at + n];
      batch_[n] = item.entry->instance;
      batch_[n].bones[0] = item.bones;
      ++n;
    }
    const bool sinks = snowing && Sinks(lod, swayItems_[at].entry->model);  // [undersnow]
    if (sinks != sinking) {
      device_->SetPipeline(sinks ? swaySnowMotionPipeline_ : swayMotionPipeline_);
      sinking = sinks;
    }
    level(lod, swayItems_[at].entry->model, sinks);
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    at += n;
  }
  // [gpuscenery] and the card's lists of what sways (its sway pass: the same test, on the card), by their variants
  if (cardNow_ && cardSway_ >= 0 && !card_->Draws(cardSway_).empty()) {
    device_->SetShaderBuffer(card_->Buffer());
    int set = -1;
    for (const CardScenery::Draw& d : card_->Draws(cardSway_)) {
      const bool sinks = snowing && Sinks(d.lod, d.model);
      const gfx::Pipeline p = CardOf(sinks ? swaySnowMotionPipeline_ : swayMotionPipeline_);
      if (!p) continue;
      if (set != (sinks ? 1 : 0)) {
        device_->SetPipeline(p);
        set = sinks ? 1 : 0;
      }
      level(d.lod, d.model, sinks);
      const uint32_t pick[4] = {d.record / 4, 0, 0, 0};
      device_->SetConstants(2, pick, sizeof pick);
      device_->DrawIndexedIndirect(card_->Buffer(), d.record);
    }
    device_->SetShaderBuffer(boneBuffer_);
  }
  device_->SetTexture(8, {});
  device_->SetTexture(9, {});
  if (snowing) {  // [undersnow]
    device_->SetTexture(10, {});
    device_->SetTexture(11, {});
  }
}

// Hidden outlines (hidden_outlines.h): the marked models the scene drew, again, as DrawMotion draws the keyed ones.
void MeshRenderer::DrawMarked(const View& view, uint32_t mask, uint32_t want) {
  // (hidden_outlines.cpp's mask shader reads these instances by its own copy of their layout: six rows of four, 640 a draw)
  static_assert(sizeof(Instance) == 6 * 16 && kMaxInstances == 640, "hidden_outlines.cpp's kMaskShader must read instances as they are laid out here");
  if (markedItems_.empty()) return;
  if (!markedSorted_) {  // by level, so that each level's instances go together (once a frame)
    std::stable_sort(markedItems_.begin(), markedItems_.end(), [](const MotionItem& a, const MotionItem& b) { return a.lod < b.lod; });
    markedSorted_ = true;
  }
  device_->SetShaderBuffer(boneBuffer_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  for (size_t at = 0; at < markedItems_.size();) {
    const ModelLod* lod = markedItems_[at].lod;
    size_t n = 0;
    while (at < markedItems_.size() && markedItems_[at].lod == lod && n < kMaxInstances) {
      const MotionItem& item = markedItems_[at++];
      const uint32_t mark = item.entry->mark;
      if ((mark & mask) != want) continue;
      batch_[n] = item.entry->instance;
      batch_[n].bones[0] = item.bones;
      for (int k = 0; k < 4; ++k) batch_[n].color[k] = static_cast<float>((mark >> (8 * k)) & 0xffu);
      ++n;
    }
    if (!n) continue;
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
  }
}

// [gpuscenery] The still scenery on the card (card_scenery.h).
MeshRenderer::~MeshRenderer() = default;  // (CardScenery is whole here)

float MeshRenderer::CardPickMs() const { return card_ ? card_->pickMs : 0.0f; }

// Which of the card's passes a view is (the one Prepare was given; not the sway motion's), or -1.
int MeshRenderer::CardPassOf(const View& view) const {
  for (size_t i = 0; i < cardViews_.size(); ++i)
    if (cardViews_[i] == &view && static_cast<int>(i) != cardSway_) return static_cast<int>(i);
  return -1;
}

// The groups listed that changed since, given to the card anew (each its props the card does not take kept for the
// processor), and the frame's passes picked: the camera's (Prepare's first view), the sun's cascades (those after it with
// no lamp's reach), and the camera's motion of what sways when motion is on. False: the processor draws the scenery.
bool MeshRenderer::CardPrepare(const std::vector<const View*>& views) {
  static_assert(sizeof(Instance) == 24 * sizeof(float), "card_scenery.h reads an instance as 24 floats");
  if (!device_->IndirectDraws()) return false;
  if (!card_) card_ = std::make_unique<CardScenery>(device_);
  if (!CardPipelines()) return false;
  ++cardFrame_;
  CardGive();
  cardViews_.clear();
  cardSway_ = -1;
  CardScenery::Pass passes[CardScenery::kMostViews];
  int n = 0;
  auto set = [&](const View* v, bool shadow, bool sway) {
    CardScenery::Pass& p = passes[n++];
    p.view = v;
    p.shadow = shadow;
    p.sway = sway;
    p.smallest = shadow ? shadowMinPixels : minPixels;
    p.wish = trianglesPerPixel * detail * (shadow ? shadowDetail : 1.0f) * 0.7854f;  // (as Draw's)
    p.standard = std::sqrt(detail * (shadow ? shadowDetail : 1.0f));                // (as Consider's)
    p.swayPixels = blastConstants && blastConstantsSize ? swayMotionPixelsBlasts : swayMotionPixels;
    cardViews_.push_back(v);
  };
  for (size_t i = 0; i < views.size() && n < CardScenery::kMostViews - 1; ++i)
    if (views[i]->reach <= 0 && !views[i]->hasCarrier) set(views[i], i > 0, false);
  if (n && motion && swayMotion && cardViews_[0] == views[0]) {
    cardSway_ = n;
    set(views[0], false, true);
  }
  if (!card_->Pick(groupsListed_, passes, n, divertStream, cardFrame_)) return false;
  static bool told = false;  // (once: from when)
  if (!told)
    printf("render: the scenery picked on the card from its frame %llu: %zu props of %zu models, %zu lists, %d passes (%s)\n",
           static_cast<unsigned long long>(cardFrame_), card_->Props(), card_->Models(), card_->Lists(), n, device_->BackendName());
  told = true;
  while (cardEntries_.size() < card_->Models()) {  // (an entry for each of its models: what a draw's model is read from)
    Entry e{};
    e.model = card_->ModelAt(cardEntries_.size());
    e.scenery = true;
    e.flags = kScenery;
    cardEntries_.push_back(e);
  }
  return true;
}

// [step2b] The groups listed that changed since they were last given to the card, given anew (each its props the card does
// not take kept for the processor).
void MeshRenderer::CardGive() {
  std::vector<CardScenery::Member> members;
  std::vector<uint32_t> left;
  for (uint32_t g : groupsListed_) {
    if (g >= groups_.size()) continue;
    Group& group = groups_[g];
    if (!group.cardDirty && card_->Has(g)) continue;
    members.clear();
    for (const Entry& e : group.entries)
      members.push_back({e.model, reinterpret_cast<const float*>(&e.instance), e.centre, e.radius, e.sizeRadius});
    card_->SetGroup(g, group.order, members.data(), members.size(), &left);
    group.rest.clear();
    group.restCulls.clear();
    for (uint32_t i : left) {
      group.rest.push_back(group.entries[i]);
      group.restCulls.push_back(group.culls[i]);
    }
    group.cardDirty = false;
  }
}

// [step2b] Whether the card picks this frame's scenery (cardByNeed): by the props of the groups listed, with the frames in a
// row each way (cardOnFrames, cardOffFrames) before it changes, so that a camera at the edge of the two does not switch to
// and fro. Without cardByNeed: always.
bool MeshRenderer::CardPays() {
  size_t props = 0;
  for (uint32_t g : groupsListed_)
    if (g < groups_.size()) props += groups_[g].entries.size();
  cardListedProps = props;
  if (!cardByNeed) return true;
  const bool other = cardPays_ ? props < static_cast<size_t>(std::max(cardOffProps, 0)) : props >= static_cast<size_t>(std::max(cardOnProps, 0));
  cardTurn_ = other ? cardTurn_ + 1 : 0;
  if (cardTurn_ >= std::max(cardPays_ ? cardOffFrames : cardOnFrames, 1)) {
    cardPays_ = !cardPays_;
    cardTurn_ = 0;
    if (++cardSwitches_ <= 20)
      printf("render: the scenery picked by the %s from here (%zu props listed)%s\n", cardPays_ ? "card" : "processor", props,
             cardSwitches_ == 20 ? "; no more of these" : "");
  }
  return cardPays_;
}

// [step2b] A frame the card does not pick for (cardByNeed): what it will need kept ready all the same, the variants of the
// pipelines asked for in the background and the groups that changed given to it, so that the first frame it picks again
// has no more to do than any other.
void MeshRenderer::CardKeep() {
  if (!device_->IndirectDraws()) return;
  if (!card_) card_ = std::make_unique<CardScenery>(device_);
  CardPipelines();
  device_->BeginBackgroundPipelines();  // [zoomhitch] (the picking's own, likewise)
  card_->PrepareAhead();
  device_->EndBackgroundPipelines();
  CardGive();
}

// The variants of the models' pipelines that read the card's lists: of each the world has made (the plain ones, and those
// of the blasts' waves, plants that react, the snow, the remastered build look, the motion of what sways), asked for on the
// device's own thread when the set grows; true once all are made.
bool MeshRenderer::CardPipelines() {
  const int wanted = (blastsMade_ > 0 ? 1 : 0) | (plantsMade_ > 0 ? 2 : 0) | (snowMade_ > 0 ? 4 : 0) | (buildMade_ > 0 ? 8 : 0) |
                     (swayMotionPipeline_ ? 16 : 0) | (swaySnowMotionPipeline_ ? 32 : 0);
  if (wanted != cardWanted_ && !cardFailed_) {
    cardWanted_ = wanted;
    cardReady_ = false;
    std::string error;
    device_->BeginBackgroundPipelines();
    auto want = [&](gfx::Pipeline original, const gfx::PipelineDesc& desc, const std::string& source) {
      if (!original || cardFailed_ || CardOf(original)) return;
      gfx::PipelineDesc d = desc;
      d.vertexSource = d.pixelSource = source;
      const gfx::Pipeline p = device_->CreatePipeline(d, &error);
      if (!p) {
        fprintf(stderr, "render: the scenery on the card cannot be drawn (%s); the processor picks it\n", error.c_str());
        cardFailed_ = true;
        return;
      }
      cardOf_.push_back({original.id, p});
    };
    for (const Bending& b : bending_) {
      if (!b.original) continue;
      const bool shadow = b.original.id == shadow_.id;
      want(b.original, b.desc, cardSources_[0]);
      want(b.bending, b.desc, cardSources_[1]);
      want(b.plants, b.desc, cardSources_[shadow ? 3 : 2]);
      want(b.plantsBurnt, b.desc, cardSources_[4]);
      want(b.snow, b.desc, cardSources_[shadow ? 6 : 5]);
    }
    for (const BuildVariant& v : buildVariants_) {  // (those of the pipelines that draw scenery: not the glass')
      bool scenery = false;
      for (const Bending& b : bending_) scenery = scenery || (b.original && b.original.id == v.original.id);
      if (scenery) want(v.variant, v.desc, cardSources_[7]);
    }
    want(swayMotionPipeline_, swayMotionDesc_, cardSources_[8]);
    want(swaySnowMotionPipeline_, swaySnowMotionDesc_, cardSources_[9]);
    device_->EndBackgroundPipelines();
  }
  if (cardFailed_) return false;
  if (!cardReady_) {
    for (const auto& c : cardOf_)
      if (!device_->PipelineReady(c.second)) return false;
    cardReady_ = true;
  }
  return true;
}

}  // namespace render
