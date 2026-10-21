// [undersnow] The map's low plants under lying snow (World::plantsUnderSnow; Chris, 2026-10-21, after his test run).
//
// The ground cover sinks under lying snow (grass.cpp, GroundCover::snowDepth), but the map's own undergrowth, its ferns,
// bushes, weeds and tufts placed as props (models, drawn by mesh_renderer.cpp), stood green through it: they took only the
// light's snow on what faces up, at half the ground's share (lighting.h, SURFACE_SNOW). With this, while snow lies, each
// low plant (what sways, no wreck, lower than a tree: the snow's shelter's own measure, under 1.5 world units from its
// lowest point to its top) stands in the same snow as the cover round it:
//  - how much snow lies at its foot, and how deep, is worked out as for the cover round it (grass.cpp's CoverSnowDepth,
//    line for line: the light's patches, level ground first, by World::snowLying; with World::snowGround its shelter,
//    deeper in hollows and the wind's lee, thinner under the trees' crowns and on steep and windward ground), and with the
//    map's snow materials over the ground's mottled margin as the marks take it (decals.cpp's MarkSnowLying), so that a
//    plant on ground the snow leaves bare stays as it was; to GroundCover::snowDepth where all of it lies;
//  - the plant is sunk by that depth (the ground hides what goes under it: the snow's top is the ground's own surface, as
//    the eye sees it, so what stands out meets it and its shadow falls where it stands), pressed down by the snow's weight
//    and its outer parts bowed down the farther out they reach (a low herb most, a bush less, a plant over a unit and a
//    half all but not), spread a little and leaning its own way; buried whole where less than three tenths of it would
//    stand out (as the cover's blades), so the lowest go first and the tall ones poke out;
//  - what stands out takes the lying snow on what faces up at the ground's own measure where the ground at its foot is
//    covered (not the half the other models take: it stands in it; none where the ground there is bare), is dusted all
//    over a little by its own share of the snow, more toward its top, and all round where it comes out of the snow; its
//    leaves let less light through there.
// Its shadow is sunk and bent the same; the motion (FSR 3, frame generation) follows it. The trees and rocks keep the snow
// they had. Drawn by variants of the models' shaders built with UNDER_SNOW (and with the blasts' waves and plants that
// react in them, each off by its constants when off: MeshRenderer::PrepareUnderSnow), so that without it, and while no snow
// lies, every model is drawn by the shaders as they were.
#pragma once

namespace render {

// The shader block (after kViewShaderBlock and kLightingShaderBlock: CloudNoise, CloudHash, `weather`; before the models'
// shader, which reads it under UNDER_SNOW). Its two textures are registers of the models' shaders alone.
//   k0: x world units the snow lies at the most where all of it lies (GroundCover::snowDepth; 0: none), y 1 = the snow
//       ground's shelter is read, z a plant this tall or taller (its model's whole height in the world) keeps the snow it
//       had (a tree), w 1 = the ground lays the map's snow materials (their mottled margin: Terrain::SnowMaterialsLaid)
//   k1: xy the way the wind blows (unit length), z the trees' crowns' weight, w the drifts' (Terrain::snowTune y, w)
//   k2: xy 1 / the map's width and height
inline const char kUnderSnowShaderBlock[] = R"hlsl(
Texture2D underSnowShelter : register(t10);  // (Terrain::SnowShelterPicture) g the sky it sees, b the trees' crowns over it, a how it lies
Texture2D underSnowGround : register(t11);   // (Terrain::NormalMap) the ground's normal: x in alpha, z in green

// How readily a plant this tall (its top over its foot, world units) bends under the snow: a low herb or a tuft fully, a
// bush less, a plant over a unit and a half (a sapling, nearly a tree) all but not
float UnderSnowBends(float top) { return 1 - smoothstep(0.4, 1.5, top); }

// A low plant under the snow, by its foot `root` (world), its top over its foot `top` and its whole height `whole` (world
// units): `plant` x how deep the snow lies over its foot, y its share of a full cover (how much is dusted on it), z what of
// its height the snow's weight leaves it, w 1 = buried whole; `ground` the ground's normal at its foot, `covered` how much
// of the ground there the snow covers (0..1, as the ground lays it). False: it keeps the look it had (no snow lies, or it is
// a tree).
bool UnderSnowPlant(float3 root, float top, float whole, float4 k0, float4 k1, float4 k2, out float4 plant, out float3 ground, out float covered) {
  plant = float4(0, 0, 1, 0);
  ground = float3(0, 1, 0);
  covered = 0;
  if (k0.x <= 0 || weather.y <= 0 || whole >= k0.z) return false;
  // (the land beyond the map's edge is the map's mirror image, and so are its plants: each takes the snow of the place it
  // mirrors, its slope turned with it)
  float2 uv = root.xz * k2.xy;
  float2 flip = float2(uv.x < 0 || uv.x > 1 ? -1 : 1, uv.y < 0 || uv.y > 1 ? -1 : 1);
  uv = 1 - abs(1 - abs(uv));
  float4 under = underSnowGround.SampleLevel(samplerLinearClamp, uv, 0);
  ground.xz = (under.ag * 2 - 1) * flip;
  ground.y = sqrt(saturate(1 - dot(ground.xz, ground.xz)));
  // (how deep, as grass.cpp's CoverSnowDepth works it out for the cover round it, line for line)
  float lie = 1, less = 0;
  [branch] if (k0.y > 0.5) {
    float4 s = underSnowShelter.SampleLevel(samplerLinearClamp, uv, 0);
    float crowns = s.b, lay = s.a * 2 - 1, slope = length(ground.xz);
    float lee = slope > 1e-3 ? dot(ground.xz / slope, k1.xy) * saturate(slope * 5) : 0;
    lie = (1 + 0.4 * lay * k1.w + 0.15 * lee) * (1 - 0.3 * crowns * k1.z);
    less = 0.08 * crowns * k1.z + 0.25 * saturate((0.85 - ground.y) * 4) + 0.1 * saturate(-lee) + 0.3 * saturate(-lay) * k1.w;
  }
  float fallen = weather.y * lie;
  float patch = CloudNoise(root * 0.9) * 0.55 + CloudNoise(float3(root.xz * 0.21, 5.3)) * 0.45;
  float takes = patch * 0.75 + saturate((ground.y - 0.35) * 1.6) * 0.25 - less;
  float bar = 1.02 - fallen * 1.15;
  float lying = (k0.y > 0.5 ? smoothstep(bar - 0.15, bar + 0.35, takes) : smoothstep(bar, bar + 0.2, takes)) * smoothstep(0.25, 0.5, ground.y);
  // (with the map's snow materials the ground's margin is mottled: thinner snow, islands and holes; as MarkSnowLying)
  [branch] if (k0.w > 0.5) {
    float mottle = CloudNoise(float3(root.xz * 0.45, 1.3)) * 0.6 + CloudNoise(float3(root.xz * 1.3, 4.7)) * 0.4;
    lying = saturate(lying + (mottle - 0.5) * 3.2 * lying * (1 - lying));
  }
  covered = lying;
  float share = saturate(lying * fallen);
  float deep = lying * fallen * k0.x * (0.75 + 0.5 * CloudNoise(float3(root.xz * 0.8, 2.9)));
  // (pressed by the snow on it, a low herb to four fifths of its height under a full cover; buried where less than three
  // tenths of what is left would stand out, as the cover's blades: a tip a hair over the snow would read as a speck of dirt)
  float pressed = 1 - 0.2 * share * UnderSnowBends(top);
  float stands = top * pressed;
  plant = float4(deep, share, pressed, deep > 0 && stands - deep < max(0.025, 0.3 * stands) ? 1 : 0);
  return true;
}

// A corner of a plant under the snow (UnderSnowPlant's `plant` and `ground`; `top` the plant's top over its foot, `lean` the
// way it leans, its own), at `world`: where the snow puts it, sunk by the snow's depth, pressed down toward the ground by the
// snow's weight, its outer parts bowed down the farther out they reach, spread a little and leaning its own way; its
// normal, tangent and binormal turned with it (by how the snow bends the plant there: pressed, spread, bowed); `above` its
// height over the snow's top there (the ground's plane through the foot, which the ground's surface stands for).
float3 UnderSnowCorner(float3 world, float3 root, float top, float4 plant, float3 ground, float2 lean, inout float3 normal, inout float3 tangent,
                       inout float3 binormal, out float above) {
  float share = plant.y, pressed = plant.z;
  float gy = max(ground.y, 0.35);
  float2 off = world.xz - root.xz;
  float a = world.y - plant.x - (root.y - dot(ground.xz, off) / gy);  // (sunk: its height over the ground's plane)
  float r = length(off);
  float2 out_ = r > 1e-4 ? off / r : float2(0, 0);
  float bends = share * UnderSnowBends(top);
  float spread = 1 + 0.1 * bends;
  float droop = 0.2 * bends / max(top, 0.15);  // (a part r out from the foot goes down by droop * r * r)
  float a2 = a * pressed - droop * r * r;
  float2 off2 = off * spread + lean * (0.1 * bends * max(a2, 0));
  float3 moved = float3(root.x + off2.x, world.y - plant.x + (a2 - a), root.z + off2.y);
  above = moved.y - (root.y - dot(ground.xz, off2) / gy);
  // (the bend's own way of turning a surface: up squeezed by `pressed`, out stretched by `spread`, the slope of the bow `q`;
  // a normal by its inverse transposed, a tangent by itself, each left for the pixel to normalise)
  float q = 2 * droop * r;
  normal = float3(pressed * normal.x, spread * normal.y, pressed * normal.z) + float3(out_.x, 0, out_.y) * (q * normal.y);
  tangent = float3(spread * tangent.x, pressed * tangent.y - q * dot(tangent.xz, out_), spread * tangent.z);
  binormal = float3(spread * binormal.x, pressed * binormal.y - q * dot(binormal.xz, out_), spread * binormal.z);
  return moved;
}

// How much snow dusts a pixel of a plant under the snow besides the light's own snow on what faces up (0..1): `hold` x its
// share of the snow, y what of it stands out over the snow (world units); `above` the pixel's height over the snow's top,
// N its normal (seen from behind a leaf: its back's), `world` its place. A little all over, more where it faces up and
// toward its top, in patches a tenth of a unit across, and all round where it comes out of the snow (snow heaped round its
// stems); never quite white all over.
float UnderSnowDust(float2 hold, float above, float3 N, float3 world) {
  float up = saturate(N.y * 2.5 + 0.4);
  float high = saturate(above / max(hold.y, 1e-3));
  float rim = 1 - smoothstep(0.0, 0.05, above);
  float patches = 0.7 + 0.6 * CloudNoise(world * 9.0);
  return saturate(hold.x * ((0.2 + 0.3 * up) * (0.5 + 0.5 * high) * patches + 0.7 * rim)) * 0.9;
}
)hlsl";

}  // namespace render
