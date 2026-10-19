#pragma once
// Winter water (World::winterWater; Chris, 2026-10-18: the shallows were a bright tropical turquoise beside white
// snow). While snow lies, the modern look's water goes over to a cold palette by how much lies (the view's
// winterWater.x, World::Begin: from snowLying, smoothly, so a thaw brings the summer water back):
//   - the water's colour: deep water a dark slate blue-grey, the shallows a muted grey-green; the water murkier
//     (it hides the bed sooner), red taken first from what is seen through it, then blue, green last, so that a pale
//     bed no longer turns turquoise; the bed itself greyer and dimmer;
//   - the shore: a rim of thin ice in plates along the waterline, broken plate by plate at its outer edge, its
//     cracks opening wider toward that edge, and slush in clumps between the plates and past them; on them the
//     waves are stilled and their foam dies; the foam elsewhere greyer;
//   - and no snow lies under the water (lighting.h, ModernLight): the snow on the bed was most of the turquoise.
// The sky's reflection is as it was (the snow's overcast sky greys it already). Only on the map's own shores: past
// the map's edge the slush and ice fade out (the sea beyond is the horizon's). With no snow lying (winterWater.x
// 0), nothing changes: the water's shader without WINTER is the same source as before (water.cpp), and ModernLight's
// line is skipped.

namespace render {

// HLSL: the water's modern shader takes it when built with WINTER (after the view's and the lighting's blocks).
inline const char kWinterWaterShaderBlock[] = R"hlsl(
// The water's colours by how far winter has come (winterWater.x): what is seen through it (`under`, the bed), what
// the water takes from that on the way (`kept`), how much of it the water hides (`hidden`) and the water's own colour
// in its place (`own`), each moved from its summer value toward its winter one. `through`: the length of water the
// eye looks through to the bed.
void WinterColours(float through, inout float3 under, inout float3 kept, inout float hidden, inout float3 own) {
  float w = winterWater.x;
  // the bed under cold, grey water: greyer and dimmer
  float grey = dot(under, float3(0.3, 0.6, 0.1));
  under = lerp(under, grey * float3(0.94, 0.99, 1.03), 0.4 * w) * (1 - 0.1 * w);
  // murkier water: red gone after about 2.4 of depth, blue after 4, green last after 5.2 (so a pale bed goes
  // grey-green, not turquoise), and the bed hidden nearly twice as soon (4.2 against 7.5)
  kept = lerp(kept, exp(-through * float3(1 / 2.4, 1 / 5.2, 1 / 4.0)), w);
  hidden = lerp(hidden, 1 - exp(-through / 4.2), w);
  // its own colour: a muted grey-green over the shallows to a dark slate where it is deep
  own = lerp(own, lerp(float3(0.10, 0.155, 0.115), float3(0.015, 0.021, 0.033), saturate(1 - exp(-through / 4.0))), w);
}

// Noise of one size (CloudNoise at `q * size`), left at its mean where it would be finer than about two pixels
// (`grain`: how much water one pixel covers): finer, every pixel would draw its own value, a salting of points.
float WinterNoise(float2 q, float size, float seed, float grain) {
  float shown = saturate(1.5 - grain * size * 2);
  return shown > 0 ? lerp(0.5, CloudNoise(float3(q * size, seed)), shown) : 0.5;
}

// What covers the water at a place on a winter shore.
struct WinterIce {
  float cover;    // how much of the place ice and slush cover (0..1)
  float3 albedo;  // their colour (linear), lit as the water's foam is
  float calm;     // how much the waves are stilled under them
  float matte;    // how much of what covers mirrors nothing (snow on the ice, slush)
};

// Plates of ice as cells round scattered points (one in each square of the grid `p` is measured in, jittered): the
// square of the nearest point (`plate`, which names the plate) and how far `p` lies from the nearest border between
// two plates (in the grid's units: from the plane halfway between the nearest point and each of the others).
void WinterPlates(float2 p, out float2 plate, out float border) {
  float2 square = floor(p), f = p - square;
  float best = 8;
  float2 to = 0;
  plate = square;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      float2 s = square + float2(x, y);
      float2 d = float2(x, y) + 0.15 + 0.7 * float2(CloudHash(float3(s, 17.3)), CloudHash(float3(s, 31.9))) - f;
      float dd = dot(d, d);
      if (dd < best) {
        best = dd;
        to = d;
        plate = s;
      }
    }
  border = 8;
  for (int v = -1; v <= 1; ++v)
    for (int u = -1; u <= 1; ++u) {
      float2 s = plate + float2(u, v);
      float2 d = (s - square) + 0.15 + 0.7 * float2(CloudHash(float3(s, 17.3)), CloudHash(float3(s, 31.9))) - f;
      float2 apart = d - to;
      if (dot(apart, apart) > 1e-5) border = min(border, dot(0.5 * (to + d), normalize(apart)));
    }
}

// The rim of a winter shore at a place on the water: `bed` the height of what is under it, `waviness` the map's (the
// water map's red: calm water holds more ice), `grain` how much water one pixel covers, `lace` the foam's lace there
// (0..1, about a half).
WinterIce WinterShore(float2 xz, float bed, float waviness, float grain, float lace) {
  WinterIce ice;
  ice.cover = 0;
  ice.albedo = 0;
  ice.calm = 0;
  ice.matte = 0;
  // the depth by the water's level (not by the swell, which moves: the ice lies still), and how much it changes from
  // one pixel to the next (read before any branch: neighbours' values are only there outside one)
  float still = winterWater.y - bed;
  float perPixel = max(fwidth(still), 0.004);
  // it gathers once winter has come most of the way, on the map's own shores only
  float2 inside = min(xz, winterWater.zw - xz);
  float share = saturate(winterWater.x * 1.6 - 0.6) * saturate(1 + min(inside.x, inside.y) / 16);
  // how far out the thin ice reaches, in depth of water: unevenly along the shore, less where the water is rough
  float along = CloudNoise(float3(xz * 0.035, 1.9)) * 0.7 + CloudNoise(float3(xz * 0.13, 4.3)) * 0.3;
  float reach = (0.45 + 2.0 * along) * lerp(1.0, 0.8, saturate(waviness)) * share;
  [branch] if (share <= 0 || still > reach * 1.8 + 0.02) return ice;
  float outward = saturate(still / max(reach, 1e-3) / 1.8) * 1.8;  // 0 at the waterline, 1 where the plates end on the whole

  // the plates, about 2.6 across, their borders wandering (the place they are found by bent by a slow noise, so that
  // no straight edge shows): each holds out to its own depth (so the edge is broken plate by plate, and between
  // plates' borders follows the bed). Toward the edge the cracks between them open, where the plates have drifted
  // apart; nearer the shore the ice is one sheet with a hairline crack here and there. Where a crack is finer than a
  // pixel it only darkens a little.
  float2 bend = float2(CloudNoise(float3(xz * 0.45, 5.1)), CloudNoise(float3(xz * 0.45, 8.3))) - 0.5;
  float2 plate;
  float border;
  WinterPlates((xz + bend * 1.8) / 2.6, plate, border);
  float h = CloudHash(float3(plate, 23.1)), k = CloudHash(float3(plate, 41.7));
  float jagged = WinterNoise(xz, 1.6, 3.7, grain) * 0.6 + WinterNoise(xz, 4.1, 7.9, grain) * 0.4;  // (a broken edge, also within a plate)
  float holds = saturate(((0.45 + 0.6 * h + 0.9 * jagged - 0.45) * reach - still) / perPixel + 0.5);
  float apart = saturate((outward - 0.3) * 1.5);  // (0 near the shore, 1 at the edge)
  float shows = max(apart, 0.8 * smoothstep(0.45, 0.6, CloudNoise(float3(xz * 0.25, 12.7))));
  float gap = lerp(0.004, 0.06, apart);
  float pixel = grain / 2.6;
  float crack = shows * (1 - smoothstep(gap, gap + pixel * 1.5 + 0.006, border));
  crack = lerp(crack, shows * gap * 3, saturate(pixel * 6 - 1));  // (far off: the cracks as a shade)
  float plates = holds * (1 - crack);
  // snow dusting the plates: thick near the shore, where it blows onto them, thinner and patchy toward the edge,
  // some plates barer than others; bare ice is grey, the water's dark showing through it
  float dust = saturate(0.85 - 0.45 * outward + 0.3 * (k - 0.5) + (lace - 0.5) * 0.4 + (WinterNoise(xz, 0.9, 6.6, grain) - 0.5) * 0.8);
  float3 plateColour = lerp(float3(0.25, 0.28, 0.30), float3(0.42, 0.44, 0.46), dust);

  // slush: thick along the waterline, in clumps of the lace further out, to nearly twice the plates' reach
  float clumps = lace * 0.6 + WinterNoise(xz, 2.3, 9.1, grain) * 0.4;
  float bar = lerp(0.25, 0.8, outward / 1.8);
  float slush = smoothstep(bar - 0.06 - pixel * 0.1, bar + 0.06 + pixel * 0.1, clumps) * saturate((1.8 - outward) * 4) * 0.92;
  float3 slushColour = float3(0.20, 0.215, 0.225) * (0.85 + 0.3 * WinterNoise(xz, 5.3, 2.9, grain));

  ice.cover = plates + (1 - plates) * slush;
  ice.albedo = (plateColour * plates + slushColour * ((1 - plates) * slush)) / max(ice.cover, 1e-3);
  ice.matte = (plates * (0.85 + 0.15 * dust) + (1 - plates) * slush * 0.95) / max(ice.cover, 1e-3);
  ice.cover *= saturate(share * 3);
  ice.calm = saturate(ice.cover * 1.2);
  return ice;
}
)hlsl";

}  // namespace render
