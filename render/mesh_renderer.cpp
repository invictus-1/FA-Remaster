#include "mesh_renderer.h"

#include "cpu_times.h"
#include "jobs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "lighting.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 768;  // per draw call (the size of the shader's array: 64 KB at most)

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
};
cbuffer Instances : register(b2) {
  Instance instances[768];
};
StructuredBuffer<float4> boneRows : register(t0, space1);
cbuffer MeshKind : register(b3) {
  float4 kind;  // x: alpha not above this is cut away (-1: glass, seen through by its alpha and glow), y: 1 = lit by the mesh's normals only (2: a frame of light,
                // 3: a shield's bubble, 4: a slab that fades, 5: a nuke's ball of fire, 6: a nuke's sliding
                // shell, 7: its texture's own colours, 8: a quantum gate's field, 9: a zapper's ball; none of
                // them is lit),
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
  float4 kind5; // x: 1 = the Cybran's highlight lookup is there; y: 1 = the Seraphim's mirror ramp is there
};
Texture2D albedoMap : register(t0);
Texture2D normalsMap : register(t1);
Texture2D specularMap : register(t2);
TextureCube environmentMap : register(t3);
Texture2D secondaryMap : register(t4);
Texture2D insectLookup : register(t5);
Texture2D seraphimFalloff : register(t6);

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
};

VOut VS(VIn i, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
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
    // where it stands), with a slow swing and a quicker one over it; harder when it rains
    float high = saturate(p.y * kind3.y);
    float size = length(float3(n.row0.x, n.row1.x, n.row2.x)) / kind3.y;  // its height in the world
    float beat = dot(float2(n.row0.w, n.row2.w), float2(0.11, 0.07));
    float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7 + p.x * kind3.y * 4) * 0.4;
    // (it comes in gusts that run across the land with the wind)
    float2 foot = float2(n.row0.w, n.row2.w);
    float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
    o.world.xz += float2(0.915, 0.403) * (swing * gust * kind3.x * high * high * size * 0.035);
  }
  o.position = mul(viewProj, float4(o.world, 1));
  float3x3 r = float3x3(n.row0.xyz, n.row1.xyz, n.row2.xyz);
  o.normal = mul(r, normal);
  o.tangent = mul(r, tangent);
  o.binormal = mul(r, binormal);
  o.uv = i.uv;
  o.color = n.color;
  o.tint = n.bones.yzw;
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

// shadow maps: depth only, with foliage's holes (and nothing where a unit is not built yet)
void ShadowPS(VOut i) {
  if (kind.x > 0) clip(albedoMap.Sample(samplerLinear, i.uv.xy).a - kind.x);
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
  if (min(i.color.a, 1) < 0.999 && kind.z < 0.5 && kind2.w < 0.5) {
    float3 work;
    clip(BuildSolid(int(kind2.x + 0.5), min(i.color.a, 1), i.shape, work));
  }
  return float4(0, 0, 0, cover);
}

float4 Shade(VOut i, bool front) {
  // (a finished unit whose personal shield is up says so with an alpha above 1)
  bool shielded = i.color.a > 1.5;
  i.color.a = min(i.color.a, 1);
  if (inspect.x > 8.5 && look > 0.5) {
    // a view for checking art: which level of detail each model is drawn at: white the finest,
    // then yellow, orange, red, violet; shaded a little by which way the surface faces
    float level = kind5.z;
    float3 mark = level < 0.5 ? float3(1, 1, 1) : level < 1.5 ? float3(1, 0.9, 0.25) : level < 2.5 ? float3(1, 0.55, 0.15) : level < 3.5 ? float3(1, 0.2, 0.15) : float3(0.7, 0.3, 1);
    return float4(mark * (0.45 + 0.55 * saturate(normalize(i.normal).y * 0.5 + 0.5)) * (modern.x * lightMultiplier) * 0.6, 1);
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
  float4 albedo = albedoMap.Sample(samplerAniso, i.uv.xy);
  float2 nxy = normalsMap.Sample(samplerAniso, i.uv.zw).ga * 2 - 1;
  float4 spec = specularMap.Sample(samplerAniso, i.uv.xy);
  float occlusion = secondaryMap.Sample(samplerAniso, i.uv.xy).r;
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
    if (solid <= 0 && faction != 1 && faction != 3) return float4(look > 0.5 ? work * 2 : saturate(work), 1);
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
    } else if (kind4.x > 0.5) {  // wreckage: most of its colour gone, dark where the soot is thick with a little rust, dull
      float grey = dot(base, float3(0.3, 0.6, 0.1));
      base = lerp(float3(grey, grey, grey), base, 0.35) * lerp(0.12, 0.55, soot);
      base = lerp(base, float3(0.09, 0.05, 0.03), 0.25 * (1 - soot));
      metallic *= 0.3;
      roughness = lerp(0.8, 0.95, soot);
      emission = 0;
    }
    if (building) {
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
    }
    if (shielded) {  // its personal shield: light of its faction's shield along its outline, breathing slowly
      float3 tint = faction == 1 ? float3(0.35, 1.0, 0.75) : faction == 2 ? float3(1.0, 0.2, 0.12) : faction == 3 ? float3(0.95, 0.85, 0.55) : float3(0.25, 0.5, 1.0);
      float outline = pow(1 - saturate(dot(toCamera, N)), 3);  // (only where the surface turns away from the eye)
      emission += tint * ((0.03 + 1.1 * outline) * (0.8 + 0.2 * sin(time * 2.2)) * (falling.w > 0 ? falling.w : 1));
    }
    float3 lit = ModernLight(base, metallic, roughness, occlusion, emission, N, i.world, shadow);
    if (kind2.y > 0.5) {  // leaves: some of the light that falls on the far side comes through
      float3 falls = ToLinear(sunColor) * (lightMultiplier * modern.x);
      lit += base * falls * (saturate(dot(-N, sunDirection)) * shadow * CloudShadow(i.world) * spec.a * 0.6);
    }
    return float4(lit * built, kind.y > 0.5 ? albedo.a : cover);
  }

  if (kind2.w > 0.5) {  // the map's border as the original lights it: by the sun's angle, but not where it glows
    float shade = lerp(saturate(dot(sunDirection, N)), 1, spec.b);
    return float4(shade * (albedo.rgb + albedo.a) + 0.02, 1);
  }
  if (shielded) work += (faction == 1 ? float3(0.35, 1.0, 0.75) : faction == 2 ? float3(1.0, 0.2, 0.12) : faction == 3 ? float3(0.95, 0.85, 0.55) : float3(0.25, 0.5, 1.0)) * (0.03 + 0.6 * pow(1 - saturate(dot(toCamera, N)), 3));
  float3 light = OriginalLight(dot(sunDirection, N), shadow);
  if (kind.y > 0.5) return float4(i.color.rgb * albedo.rgb * light, albedo.a);  // the plain look ("VertexNormal")
  float3 environment = environmentMap.SampleLevel(samplerLinear, reflect(-toCamera, N), 0).rgb;
  float highlight = saturate(dot(reflect(-sunDirection, N), toCamera));
  if (kind4.x > 1.5) return float4(float3(0.55, 0.58, 0.62) * (0.4 + environment * 1.2 + pow(highlight, 12)), 1);  // liquid metal
  if (kind.z < 0.5 && faction == 3) {  // Seraphim
    float3 ambience = sunAmbience + (1 - sunAmbience) * shadowFill;
    // (how strongly it mirrors, by the angle it is seen at: the game's own ramp, in bands, where
    // the game's files have it)
    float mirrors = kind5.y > 0.5 ? seraphimFalloff.SampleLevel(samplerLinearClamp, float2(pow(1 - saturate(dot(toCamera, N)), 0.6), 0.5), 0).a : lerp(0.35, 1, edge);
    float3 seraphim = albedo.rgb * ambience + environment * spec.r * mirrors +
                      float3(0.5, 0.6, 0.7) * pow(highlight, 9) * spec.g + i.color.rgb * rim * albedo.a;
    return float4(seraphim * built + work * (0.3 + edge), 1);
  }
  albedo.rgb = kind.z > 0.5 ? albedo.rgb * i.color.rgb : lerp(i.color.rgb, albedo.rgb, 1 - spec.a);
  if (kind.z < 0.5 && faction == 1) {  // Aeon
    float3 sun = sunColor * saturate(dot(sunDirection, N)) * shadow + sunAmbience;
    float3 aeonLight = 0.6 * lightMultiplier * sun + (1 - sun) * shadowFill;
    float3 aeon = albedo.rgb * (2 * spec.b + aeonLight + spec.r * environment) + float3(0.8, 0.85, 1.1) * pow(highlight, 3) * spec.g;
    return float4(lerp(float3(0.55, 0.58, 0.62) * (0.4 + environment * 1.2 + pow(highlight, 12)), aeon, solid), 1);
  }
  if (kind.z < 0.5 && faction == 2) {  // Cybran
    float3 sun = 2 * sunColor * saturate(dot(sunDirection, N)) * shadow + sunAmbience;
    float3 cybranLight = lightMultiplier * sun + (1 - sun) * shadowFill;
    // (the highlight: the game's own lookup, by the sun's mirror image against the eye and by the
    // sun against the surface, where the game's files have it)
    float3 gleam = kind5.x > 0.5 ? insectLookup.SampleLevel(samplerLinearClamp, float2(dot(reflect(-sunDirection, N), toCamera), dot(sunDirection, N)), 0).rgb
                                 : pow(highlight, 6);
    float3 sheen = (gleam * spec.g + 0.05 * spec.r * environment) * (1 - spec.a);
    return float4((albedo.rgb * (2 * spec.b + cybranLight) + sheen) * built + work, 1);
  }
  float3 additive = float3(0.6, 0.8, 0.9) * highlight * highlight * spec.g;
  float3 color = albedo.rgb * (2 * spec.b + light + 2 * environment * spec.r) + additive;
  return float4(color * built + work, 1);
}

// a ghost: its shape in one colour, the light and dark of the model kept
float4 PS(VOut i, bool front : SV_IsFrontFace) : SV_Target {
  float4 c = Shade(i, front);
  c.rgb = BeyondScenery(c.rgb, i.world);  // (the trees and rocks of the world beyond the map's edge)
  // glass: as much of it shows as its albedo's alpha and its glow say (never quite nothing, or a
  // clear pane would not be there at all)
  if (kind.x < -0.5) c.a = saturate(max(albedoMap.Sample(samplerAniso, i.uv.xy).a + specularMap.Sample(samplerAniso, i.uv.xy).b, 0.12));
  if (dot(i.tint, float3(1, 1, 1)) > 0) {
    float lightness = dot(c.rgb, float3(0.3, 0.6, 0.1));
    float3 tint = look > 0.5 ? ToLinear(i.tint) : i.tint;
    c.rgb = lerp(c.rgb, tint * (lightness * 1.5 + 0.1), 0.8);
  }
  return c;
}
)hlsl";

}  // namespace

bool MeshRenderer::Init(std::string* error) {
  // (units and props take half the lying snow the ground takes: they must still be told apart)
  std::string source = std::string("#define SURFACE_SNOW 0.5\n") + kViewShaderBlock + kLightingShaderBlock + kShader;
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
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "models, blended";
  pd.blend = gfx::Blend::Alpha;
  blended_ = device_->CreatePipeline(pd, error);
  if (!blended_) return false;
  pd.name = "models, see-through";
  pd.cull = gfx::Cull::None;  // (a bubble's far side shows through its near side)
  pd.depthWrite = false;
  glass_ = device_->CreatePipeline(pd, error);
  if (!glass_) return false;
  pd.cull = gfx::Cull::Back;
  pd.depthWrite = true;
  pd.name = "models, a ball of fire";  // (seen from outside only, and it hides what burns inside it)
  dome_ = device_->CreatePipeline(pd, error);
  if (!dome_) return false;
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
  leaves_ = device_->CreatePipeline(pd, error);
  if (!leaves_) return false;
  pd.name = "models, leaves' depth first";
  pd.pixelEntry = "DepthPS";
  pd.colorWrite = false;
  prepassLeaves_ = device_->CreatePipeline(pd, error);
  if (!prepassLeaves_) return false;
  pd.alphaToCoverage = false;
  pd.cull = gfx::Cull::Back;
  pd.name = "models, depth first";
  prepass_ = device_->CreatePipeline(pd, error);
  if (!prepass_) return false;
  pd.colorWrite = true;
  pd.name = "models, shadow";
  pd.samples = 1;
  pd.blend = gfx::Blend::Opaque;
  pd.pixelEntry = "ShadowPS";
  pd.depthOnly = true;
  pd.cull = gfx::Cull::None;  // thin and open meshes cast a shadow from either side
  shadow_ = device_->CreatePipeline(pd, error);
  batch_.resize(kMaxInstances);
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  boneBuffer_ = device_->CreateBuffer(bd);
  return static_cast<bool>(shadow_);
}

void MeshRenderer::Begin() {
  entries_.clear();
  culls_.clear();
  groupsListed_.clear();
  rows_.clear();
  prepared_ = false;
}

void MeshRenderer::Prepare(const std::vector<const View*>& views) {
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < culls_.size(); ++i) {
    const Cull& c = culls_[i];
    if (!(c.flags & kPosed)) continue;
    Entry& e = entries_[i];
    const Vec3 r{c.radius, c.radius, c.radius};
    bool seen = views.empty();
    for (const View* view : views)
      if (view->BoxVisible(e.centre - r, e.centre + r)) {
        seen = true;
        break;
      }
    if (seen) BuildRows(&e);
  }
  if (rows_.empty()) rows_.assign(4, 0.0f);  // the shaders' buffer is never left unset
  device_->UpdateBuffer(boneBuffer_, rows_.data(), static_cast<uint32_t>(rows_.size() * sizeof(float)));
  prepared_ = true;
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  prepareMs = prepareMs == 0 ? ms : prepareMs * 0.95f + ms * 0.05f;
}

// The rows of a posed model's bones. A bone's matrix is: back from where the bone stands at rest,
// then to where it is now. Now is between the two beats' poses; a bone a beat does not list is at
// rest in that beat.
void MeshRenderer::BuildRows(Entry* entry) {
  const std::vector<ModelBone>& bones = entry->model->bones;
  const Pose& pose = entry->pose;
  entry->rows = static_cast<int>(rows_.size() / 4);
  rows_.resize(rows_.size() + bones.size() * 12);
  float* out = rows_.data() + static_cast<size_t>(entry->rows) * 4;
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
      const float gone[12] = {0, 0, 0, rest.position.x, 0, 0, 0, rest.position.y, 0, 0, 0, rest.position.z};
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

MeshRenderer::Entry MeshRenderer::MakeEntry(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Vec3* tint) const {
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
  e.instance.bones[1] = tint ? tint->x : 0;
  e.instance.bones[2] = tint ? tint->y : 0;
  e.instance.bones[3] = tint ? tint->z : 0;
  e.sizeRadius = e.radius;
  for (const ModelLod& lod : model->lods) e.flags |= lod.glass ? kHasGlass : kHasSolid;
  return e;
}

void MeshRenderer::Add(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Pose* pose, const Vec3* tint) {
  if (!model || !model->ok) return;
  Entry e = MakeEntry(model, world, teamColor, built, tint);
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

uint32_t MeshRenderer::AddGroup() {
  groups_.emplace_back();
  return static_cast<uint32_t>(groups_.size() - 1);
}

void MeshRenderer::ClearGroup(uint32_t group) {
  groups_[group].entries.clear();
  groups_[group].culls.clear();
}

void MeshRenderer::AddToGroup(uint32_t group, const Model* model, const Mat4& world) {
  if (!model || !model->ok) return;
  Entry e = MakeEntry(model, world, {1, 1, 1}, 1, nullptr);
  e.scenery = true;
  e.flags |= kScenery;
  groups_[group].entries.push_back(e);
  groups_[group].culls.push_back({e.centre, e.radius, e.sizeRadius, e.flags});
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
  if (part != 2)
    for (uint32_t g : groupsListed_) cut(groups_[g].culls.data(), groups_[g].entries.data(), groups_[g].culls.size());
  cut(culls_.data(), entries_.data(), culls_.size());
  if (pieceItems_.size() < pieces_.size()) pieceItems_.resize(pieces_.size());
  Jobs::Shared().Run(static_cast<int>(pieces_.size()), [&](int k) {
    const Piece& p = pieces_[static_cast<size_t>(k)];
    std::vector<Item>& out = pieceItems_[static_cast<size_t>(k)];
    out.clear();
    for (size_t i = 0; i < p.count; ++i) Consider(p.culls[i], p.entries[i], view, shadowPass, glass, part, needed, focal, smallest, wish, &out);
  });
  for (size_t k = 0; k < pieces_.size(); ++k) items_.insert(items_.end(), pieceItems_[k].begin(), pieceItems_[k].end());
  if (!shadowPass)
    for (const Item& item : items_) posedInstances += item.entry->posed ? 1 : 0;
  DrawItems(view, environment, shadowPass, glass);
  CpuTimes::Part(outer);
}

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
    const float wanted = wish * pixels * pixels;
    // the level whose triangle count is nearest to that wish (nearest by ratio: a step down is taken
    // at the geometric mean of two neighbours, so chains with big gaps - a tree and its flat card -
    // behave as well as even ones)
    const std::vector<ModelLod>& lods = e.model->lods;
    size_t level = 0;
    const std::vector<float>& sizes = e.model->levelPixels;
    if (!sizes.empty()) {
      // built to the standard: each level down to the size it was made for (the quality setting and
      // the shadows' coarser wish count as a smaller size)
      const float size = pixels * 0.75f * std::sqrt(detail * (shadowPass ? shadowDetail : 1.0f));
      while (level < sizes.size() && level + 1 < lods.size() && size < sizes[level]) ++level;
    } else {
      while (level + 1 < lods.size() &&
             wanted * wanted < static_cast<float>(lods[level].indexCount / 3) * static_cast<float>(lods[level + 1].indexCount / 3))
        ++level;
    }
    const ModelLod* lod = &lods[level];
    if (lod->hidden || lod->glass != glass) return;
    // the far-away cards of trees are flat pictures: they cast no shadow
    if (shadowPass && lod->blend) return;
    out->push_back({lod, &e, e.posed && e.rows >= 0 ? static_cast<float>(e.rows) : -1.0f});
  }
}

// items_ by mesh (by its level's serial number), the blended ones last, and otherwise in the order
// they were found: what a stable sort by (blended, serial) gives, done as a radix sort of the
// key's bits, eleven at a time from the lowest (which keeps each step's order).
void MeshRenderer::SortItems() {
  const size_t n = items_.size();
  if (n < 64) {
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
      return a.lod->blend != b.lod->blend ? b.lod->blend : a.lod->serial < b.lod->serial;
    });
    return;
  }
  auto key = [](const Item& item) {
    return (static_cast<uint64_t>(item.lod->blend ? 1 : 0) << 32) | static_cast<uint64_t>(item.lod->serial);
  };
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
  }

  // (the scene's solid models: their depth first, then in full; a level of a look of its own - see-
  // through, unlit, a shield, a frame of light - is drawn in full alone)
  const bool prepass = depthPrepass && !shadowPass && !glass && prepass_ && prepassLeaves_;
  auto plain = [](const ModelLod* l) {
    return !l->blend && !l->glass && !l->impact && !l->unlit && !l->nuke && !l->fades && !l->shield && !l->frame;
  };
  for (int round = prepass ? 0 : 1; round < 2; ++round) {
  const bool depthOnly = round == 0;
  int drawing = -1;  // which pipeline is set: 0 the plain one (or the shadows'), 1 leaves, 2 blended, ...
  for (size_t at = 0; at < items_.size();) {
    const ModelLod* lod = items_[at].lod;
    if (depthOnly && !plain(lod)) {  // (not in the depth round)
      while (at < items_.size() && items_[at].lod == lod) ++at;
      continue;
    }
    const int wanted = depthOnly ? (lod->leaves ? 7 : 6)
                       : shadowPass ? 0 : glass ? (lod->impact == 1 ? 5 : lod->nuke == 1 ? 4 : 3) : lod->blend ? 2 : lod->leaves ? 1 : 0;
    if (wanted != drawing) {
      device_->SetPipeline(wanted == 7 ? prepassLeaves_ : wanted == 6 ? prepass_ : wanted == 5 ? adding_ : wanted == 4 ? dome_ : wanted == 3 ? glass_ :
                           wanted == 2 ? blended_ : wanted == 1 ? leaves_ : shadowPass ? shadow_ : pipeline_);
      drawing = wanted;
    }
    size_t n = 0;
    while (at + n < items_.size() && items_[at + n].lod == lod && n < kMaxInstances) {
      batch_[n] = items_[at + n].entry->instance;
      batch_[n].bones[0] = items_[at + n].bones;
      ++n;
    }
    float kind[20] = {lod->alphaCut, lod->impact ? 9.0f + static_cast<float>(lod->impact) : lod->unlit ? 6.0f + static_cast<float>(lod->unlit) : lod->nuke ? 4.0f + static_cast<float>(lod->nuke) : lod->fades ? 4.0f : lod->shield ? 3.0f : lod->frame ? 2.0f : lod->vertexNormals ? 1.0f : 0.0f, lod->teamMask ? 0.0f : 1.0f, lod->labPbr ? 1.0f : 0.0f,
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
    kind[17] = haveSeraphimFalloff_ ? 1.0f : 0.0f;
    device_->SetConstants(3, kind, sizeof kind);
    device_->SetTexture(0, lod->albedo);
    if (!shadowPass) {
      device_->SetTexture(1, lod->normals);
      device_->SetTexture(2, lod->specular);
      device_->SetTexture(4, lod->secondary);
    }
    device_->SetVertexBuffer(lod->vertices);
    device_->SetIndexBuffer(lod->indices);
    // only the instances drawn are handed over: the shader reads no further into its array
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->DrawIndexed(lod->indexCount, 0, 0, static_cast<uint32_t>(n));
    const int triangles = static_cast<int>(n) * static_cast<int>(lod->indexCount / 3);
    if (depthOnly) {
    } else if (shadowPass) {
      shadowTriangles += triangles;
    } else {
      drawnInstances += static_cast<int>(n);
      drawnTriangles += triangles;
    }
    at += n;
  }
  }
}

}  // namespace render
