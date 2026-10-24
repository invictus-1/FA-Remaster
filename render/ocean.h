// [ocean] A real ocean (World::oceanWaves; Chris, 2026-10-23: "real ocean vs heres ground but blue"). The modern look's
// water takes its waves from a field of wind waves worked out on the card each frame, in place of the four scrolling wave
// pictures of the game's: to be seen only, nothing of it goes back to the game.
//
// The field (Tessendorf's way): the sea as the sum of very many waves, each of its own length, way and phase, how high
// each is from a wind sea's spectrum (JONSWAP: a peak by how rough the sea is, the longer waves fewer, the shorter many and
// low) with the waves spread round the wind's way (Donelan and Banner's spreading: narrow at the peak, wider for the short
// ones), plus a little long swell from a way of its own. Each wave runs at its own speed (a deep sea's: longer ones faster,
// omega^2 = g k), so the sea changes shape as it moves and never repeats in a way the eye can follow. The sum is worked out
// by inverse Fourier transforms (radix 2, in the compute shaders' shared memory: a row of 256 at a time, then a column) on
// three squares of sea of different sizes (kCascadeLength: the long waves, the middle, the short), each holding the waves
// of a band of lengths of its own, laid over each other unaligned so that no square repeats visibly.
//
// What each square gives the water (OceanAt in kOceanShaderBlock): the height and the sideways shift of the surface (a
// choppy sea: the crests sharpened, the troughs widened), its slopes, and the slopes' first and second moments with mip
// levels made by averaging (LEAN mapping): read at the level of detail a pixel covers, they tell the water how its waves
// lean on average there AND how much they scatter, so that the sun's glitter, worked out from that spread (a Gaussian of
// slopes, a sun of finite size), stays clean at every distance: sparkles close up, a broad soft path of light far off and
// from high up, never the twinkling pixels of a mirror-like normal map read too coarse.
//
// Whitecaps: where the choppy surface folds over itself (its Jacobian falls below a threshold that comes down as the sea
// gets rougher), foam is laid into a field over the sea round the camera (kFoamCells a side, its cell by the camera's
// distance, kept in whole cells as the camera moves): each frame it fades, drifts downwind and draws out into streaks,
// and new foam is laid where crests fold now. Calmer where the water is shallow.
//
// The wind: its way from the weather's (the trees' and the dust's), how rough the sea is from the weather (as the surf
// has it: a still day ripples, rain roughens it, a storm makes a big sea), eased so that the sea grows and calms over some
// seconds.
//
// [lift] Waves that lift the surface (World::oceanLift, on top of the above): the water is drawn as a grid that follows the
// camera (CDLOD: a quadtree of square patches of 32 x 32 cells over the water, each patch's cells twice the size of the
// nearer one's, chosen each frame by the distance from the eye; near the edge of its range a patch's every other vertex
// slides onto its coarser neighbour's place, so that patches of two sizes meet without a crack and nothing pops; the
// patches keep their places on the water, so the waves never swim under the grid), its points lifted and shifted by the
// same field (OceanDisplacement). How much it is lifted fades out over shallow water (OceanLiftShare: the surf and the
// beach keep the flat sheet's waterline) and on lakes. Ships and hover units ride the long waves in the picture alone
// (BobAt: the same waves summed on the processor, those longer than a hull is long): the game's water stays flat.
//
// Off (or no water): nothing is made or done, and the water's shader is built without OCEAN (the same source as before).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct MapData;

// The water's pass binds its own textures t0..t9 (water.cpp, surf.h); the ocean's take t10..t19 (not a shared slot).
inline constexpr int kOceanFirstSlot = 10;

// HLSL: the water's modern shader takes it when built with OCEAN (after the view's and the lighting's blocks).
inline const char kOceanShaderBlock[] = R"hlsl(
cbuffer Ocean : register(b2) {
  float4 oceanCascade[3];  // each square: x 1 / its length, y its length (world units), z how much of it shows, w half a texel (of 1)
  float4 oceanWind;        // xy the wind's way (x, z, length 1); z how rough the sea is (0..1); w how choppy (the sideways shift's share)
  float4 oceanFoamArea;    // the foam's field: xy its first cell's corner (world x, z); z 1 / its side; w a cell's size
  float4 oceanLook;        // x the glint's least spread of slopes (squared); y the glow through the crests; z the foam's strength; w a view for checking
  float4 oceanLook2;       // x the sea's significant height (world units); y the foam far off (a share); z the slopes' tilt times (from above: twice); w [lift] 1 = the surface lifted
  float4 oceanLook3;       // x the Jacobian below which the surface foams; y the glint's strength (1)
  float4 oceanLook4;       // [whitecaps] xy how far the foam has drifted (world units, within 4096: its pattern drifts with it); z [hulls] how many hulls
  float4 oceanHulls[72];   // [hulls] what floats in the sea (Ocean::AddHull), three rows each: x, z its middle, its way (x, z); its waterline's
                           // half length and half beam, its speed, -; its waterline's height there, the slopes it rides (dh/dx, dh/dz), -
};
Texture2D oceanSlope[3] : register(t10);  // the slopes along x and z, and their squares (mip levels: their means)
Texture2D oceanDisp[3] : register(t13);   // the sideways shift along x, the height, the shift along z; w the slopes' product
Texture2D oceanFold[3] : register(t16);   // the shift's derivatives: d shift x / dx, d shift z / dz, d shift x / dz
Texture2D<float2> oceanFoam : register(t19);  // the foam's field ([whitecaps] x how much, y how fresh)
#if LIFT
// [lift] The grid (Ocean::DrawGrid): its patches and the levels of detail.
cbuffer OceanGrid : register(b3) {
  float4 gridInfo;         // x cells a patch side; y 1 / the ground's height unit (unused); z the ground's height unit; w -
  float4 gridLods[16];     // each level: x the distance its vertices begin to slide at; y 1 / the distance over which they slide
  float4 gridNodes[1024];  // each patch: x, z its corner; its side; its level
};
Texture2D<float> oceanHeights : register(t20);  // the ground's (Terrain::HeightTexture), one a grid point

// The ground's height at a place (the map's heights, clamped at its edge).
float OceanGround(float2 xz, float2 mapSize) {
  float2 p = clamp(xz, 0, mapSize);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapSize);
  float a = oceanHeights.Load(int3(min(c, most), 0)), b = oceanHeights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = oceanHeights.Load(int3(min(c + int2(0, 1), most), 0)), e = oceanHeights.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * gridInfo.z;
}
// How much of the waves lift the surface at a place: none over shallow water (the surf and the beach keep the flat
// sheet's waterline: from a depth of 0.6 up to all of them by 3.6), less on lakes (`waviness`, the map's water map's).
// (The processor's Ocean::LiftShare is the same but for the lakes: keep the two alike.)
float OceanLiftShare(float2 xz, float level, float2 mapSize, float waviness) {
  return oceanLook2.w * (0.35 + 0.65 * waviness) * saturate((level - OceanGround(xz, mapSize) - 0.6) / 3.0);
}
// The surface's shift (x, z) and height (y) at a place on the still sheet, for a grid of cells `cell` world units across:
// each square read at the level of detail of about a cell (finer waves than the grid can hold are left out).
float3 OceanDisplacement(float2 xz, float cell) {
  float3 d = 0;
  [unroll] for (int c = 0; c < 3; ++c) {
    float lod = max(log2(cell * 256 * oceanCascade[c].x) + 0.5, 0);
    d += oceanDisp[c].SampleLevel(samplerLinear, xz * oceanCascade[c].x + oceanCascade[c].w, lod).xyz * oceanCascade[c].z;
  }
  return d;
}
#endif

struct OceanHere {
  float2 slope;     // the mean slope (dh/dx, dh/dz) over what the pixel takes in
  float3 spread;    // the slopes' variance about it: along x, along z, and the two together
  float height;     // the surface's height over the water's level
  float2 shift;     // the surface's sideways shift
  float3 fold;      // the shift's derivatives (xx, zz, xz)
  float foam;       // the foam's field here (0 none .. 1 and more)
  float fresh;      // [whitecaps] how fresh that foam is (0 old .. 1 its crest breaking now)
};

// The sea at a place (world x, z) for a pixel whose footprint is dxdx, dxdy (the place's change across the pixel, ddx and
// ddy of it): each square read at the level of detail the pixel covers (anisotropic, so that the far sea seen at a low
// angle averages along the line of sight as much as the pixel does), its moments added up (the squares' waves are apart).
// `calm` 0..1 scales the waves (bays, shallows); `tilt` their slopes again (seen from above the slopes are all that shows
// of the waves, and they are drawn steeper there than they are, as the swell's were).
OceanHere OceanAt(float2 xz, float2 dxdx, float2 dxdy, float calm, float tilt) {
  OceanHere o = (OceanHere)0;
  float k = calm;
  // (gusts: patches of the wind some 80 units across, drifting downwind, in which the short waves are rougher and between
  // which they are calmer, as cat's paws darken a sea; the long waves do not feel them)
  float gust = 0.55 + 0.9 * CloudNoise(float3((xz - oceanWind.xy * (time * 1.5)) * 0.012, time * 0.02));
  [unroll] for (int c = 0; c < 3; ++c) {
    float shown = oceanCascade[c].z * k * lerp(1, gust, c == 2 ? 1.0 : c == 1 ? 0.5 : 0.0);
    float2 uv = xz * oceanCascade[c].x + oceanCascade[c].w;
    float2 gx = dxdx * oceanCascade[c].x, gy = dxdy * oceanCascade[c].x;
    float4 s = oceanSlope[c].SampleGrad(samplerAniso, uv, gx, gy);
    float4 d = oceanDisp[c].SampleGrad(samplerAniso, uv, gx, gy);
    o.slope += s.xy * (shown * tilt);
    o.spread += float3(max(s.z - s.x * s.x, 0), max(s.w - s.y * s.y, 0), d.w - s.x * s.y) * (shown * shown * tilt * tilt);
    o.height += d.y * shown;
    o.shift += d.xz * shown;
    // ([whitecaps] the shift's derivatives: the crests breaking now, where the surface folds at this pixel)
    o.fold += oceanFold[c].SampleGrad(samplerAniso, uv, gx, gy).xyz * shown;
  }
  // the foam: the field's where it lies (fading out toward its edges), beyond it the sea's mean share of white
  float2 at = (xz - oceanFoamArea.xy) * oceanFoamArea.z;
  float2 inside = min(at, 1 - at);
  float there = saturate(min(inside.x, inside.y) * 12);
  float2 field = there > 0 ? oceanFoam.SampleLevel(samplerLinearClamp, at, 0) : 0;
  o.foam = lerp(oceanLook2.y, field.x, there) * calm;
  o.fresh = field.y * there;
  return o;
}

// [hulls] The nearest hull floating in the sea to a place on it (Ocean::AddHull: the ships riding the lifted waves): `away` how
// far the place lies outside its waterline (world units; below 0 inside it), `along` where along it (-1 its stern .. 1 its
// bow), `speed` its speed (world units a second), `waterline` the height its waterline stands at, carried on to the place
// (the hull pitches and rolls with the long waves: the slopes it rides), `beam` its half beam, `outward` the way out of the hull
// there (x, z, length 1: its side's outward way, the bow's forward). The waterline's shape: an
// ellipse toward the bow, fuller toward the stern (a transom), its distance measured to first order (the shape's value over
// how fast it changes), close enough near the hull, where alone it is wanted. None (away very large) without hulls.
struct OceanHullNear {
  float away, along, speed, waterline, beam;
  float2 outward;
};
OceanHullNear OceanHullAt(float2 xz) {
  OceanHullNear o;
  o.away = 1e6;
  o.along = 0;
  o.speed = 0;
  o.waterline = 0;
  o.beam = 1;
  o.outward = float2(1, 0);
  int count = int(oceanLook4.z);
  for (int h = 0; h < count; ++h) {
    float4 a = oceanHulls[h * 3], b = oceanHulls[h * 3 + 1];
    float2 d = xz - a.xy;
    float u = dot(d, a.zw), v = dot(d, float2(-a.w, a.z));
    if (abs(u) > b.x + 3 || abs(v) > b.y + 3) continue;  // (well away from it)
    float p = u > 0 ? 2 : 4;
    float2 q = abs(float2(u / b.x, v / b.y)) + 1e-5;
    float2 qp = pow(q, p);
    float f = pow(qp.x + qp.y, 1 / p);
    float2 grad = pow(q, p - 1) / float2(b.x, b.y) * pow(f, 1 - p);
    float away = (f - 1) / max(length(grad), 1e-4);
    if (away < o.away) {
      float4 c = oceanHulls[h * 3 + 2];
      o.away = away;
      o.along = clamp(u / b.x, -1, 1);
      o.speed = b.z;
      o.waterline = c.x + dot(c.yz, d);
      o.beam = b.y;
      float2 g = normalize(grad * sign(float2(u, v)) + 1e-6);
      o.outward = a.zw * g.x + float2(-a.w, a.z) * g.y;
    }
  }
  return o;
}

// The sun's glitter off the sea: the share of the sun's light (times its colour) mirrored toward the eye by the facets of a
// sea whose slopes are spread as a Gaussian about `slope` with variance `spread` (OceanAt's, plus the least spread, which
// stands for the waves finer than the field and the sun's own disc). V: toward the eye; L: toward the sun. (A microfacet
// glint: the facets' share that face the half way between eye and sun, by their Fresnel term and as many of them as the
// others do not hide; in the modern look's light units, where a white matt surface shows its light as it is.)
float OceanGlint(float3 V, float3 L, float2 slope, float3 spread) {
  float3 H = normalize(V + L);
  if (H.y <= 0.02 || L.y <= 0 || V.y <= 0) return 0;
  float2 s = -H.xz / H.y - slope;  // the slope that mirrors the sun into the eye, from the mean
  float a = spread.x + oceanLook.x, b = spread.y + oceanLook.x, c = spread.z;
  float det = max(a * b - c * c, 1e-8);
  float e = (b * s.x * s.x - 2 * c * s.x * s.y + a * s.y * s.y) / det;
  float p = exp(-0.5 * e) / (6.2831853 * sqrt(det));  // the share of facets with that slope (per slope squared)
  float h2 = H.y * H.y;
  float F = 0.02 + 0.98 * pow(1 - saturate(dot(V, H)), 5);
  // (the facets the others hide, from the eye and from the sun: Smith's term for a Gaussian of slopes, by the spread along
  // the way each looks)
  float2 v2 = normalize(V.xz + 1e-6), l2 = normalize(L.xz + 1e-6);
  float sv = sqrt(max(v2.x * v2.x * a + v2.y * v2.y * b + 2 * v2.x * v2.y * c, 1e-6));
  float sl = sqrt(max(l2.x * l2.x * a + l2.y * l2.y * b + 2 * l2.x * l2.y * c, 1e-6));
  float av = V.y / (sv * sqrt(max(1 - V.y * V.y, 1e-6))), al = L.y / (sl * sqrt(max(1 - L.y * L.y, 1e-6)));
  float lv = av < 1.6 ? (1 - 1.259 * av + 0.396 * av * av) / (3.535 * av + 2.181 * av * av) : 0;
  float ll = al < 1.6 ? (1 - 1.259 * al + 0.396 * al * al) / (3.535 * al + 2.181 * al * al) : 0;
  float G = 1 / (1 + lv + ll);
  return min(3.14159 * p * F * G / (4 * h2 * h2 * V.y), 60);
}

// [whitecaps] The whitecaps on the sea at a place (world x, z, its footprint dxdx, dxdy as OceanAt's) from what OceanAt read
// there and the foam's lace (water.cpp, Lace): x how much of the pixel is white, y how bright that foam is (times the light
// on the water). Three parts, as a breaking sea has them:
// - the crest breaking now: where the surface folds at the pixel itself (its Jacobian, from the field's derivatives at the
//   pixel's footprint), a hard edge along the crest's line, torn by the pattern below; solid white;
// - the fresh foam it has just laid (the field's y, fading within a second): solid but for the pattern's deepest gaps;
// - the residue (the field's x, fading over seconds): the pattern cut at a level that rises as the foam thins, so that the
//   patch tears into streaks along the wind and thins into lace, greyer and more see-through the older it is.
// The pattern: ridged noise of three sizes drawn out five times along the wind (filaments 0.13 .. 0.9 world units across),
// drifting with the foam, over the lace. Each size finer than about a pixel and a half is left at its mean, and the cut is
// softened by as much as the sizes left out would vary: there a pixel shows the share of white its footprint holds, as a
// photograph's would, never a white smear nor salted points (far off and from high up the foam lies dim and grey).
// (the parts, also for the view for checking, 6: OceanWhitecapsParts)
struct OceanCaps {
  float crest;    // the crest breaking now (0..1 of the pixel)
  float laid;     // the fresh foam
  float residue;  // the residue
  float young;    // how fresh the foam is (0 old .. 1 breaking now)
  float thick;    // how far inside its patch the pixel lies (0 at its torn edge .. 1 its thick middle)
};
OceanCaps OceanWhitecapsParts(OceanHere o, float2 xz, float2 dxdx, float2 dxdy, float lace) {
  OceanCaps caps;
  float2 way = oceanWind.xy;
  float2 p = xz - oceanLook4.xy;  // (drifting with the foam's field)
  float2 q = float2(dot(p, float2(-way.y, way.x)), dot(p, way));  // across the wind, along it
  float grain = max(length(dxdx), length(dxdy));
  const float3 size = float3(1.1, 2.9, 7.7);  // (cycles a world unit across the wind; a fifth of that along it)
  const float3 weight = float3(0.45, 0.33, 0.22);
  float3 shown = saturate(1.5 - grain * size * 2);
  float3 ridge = 0.73;  // (the ridges' mean)
  [unroll] for (int k = 0; k < 3; ++k) {  // (no branch: the cut's softness reads the pattern's change across pixels)
    float n = CloudNoise(float3(q.x * size[k], q.y * size[k] * 0.2, 2.3 + 3.1 * k));
    ridge[k] = lerp(0.73, 1 - abs(2 * n - 1), shown[k]);
  }
  // (spread to a mean of a half and a deviation of 0.2: the sum's own are about 0.66 and 0.083, from a model of the noise)
  float pattern = (dot(ridge, weight) * 0.7 + lace * 0.3 - 0.66) * 2.4 + 0.5;
  // (how much the pattern would vary that does not show at this pixel: the cut softened by it)
  float hidden = 0.2 * sqrt(dot(weight * weight, 1 - shown * shown) / 0.357);
  float soft = max(max(fwidth(pattern) * 0.8, hidden * 2), 0.012);
  float strength = oceanLook.z;
  float cover = o.foam * strength, fresh = saturate(o.fresh * strength);
  // (the share of white about the foam's own share there: the cut at that share of the pattern's spread; none where there
  // is no foam)
  // (the residue never all white: holes in it even where it lies thick, lace as it thins; the fresh foam nearly whole)
  float overResidue = pattern - (0.9 - 0.65 * saturate(cover * 2.2)), overLaid = pattern - (0.9 - 0.72 * saturate(fresh * 1.6));
  caps.residue = smoothstep(-soft, soft, overResidue) * saturate(cover * 6);
  caps.laid = smoothstep(-soft, soft, overLaid) * saturate(fresh * 6);
  caps.thick = saturate(max(overResidue, overLaid) / max(0.45, soft * 2));
  // (the crest: the fold's edge torn by the pattern, cut as sharply as the pixel allows; only where the field lays foam
  // now, so that the short waves' folds finer than its cells do not salt the sea with white points)
  // (a hard edge: no grey half-white where the fold is shallow)
  float J = (1 + o.fold.x) * (1 + o.fold.y) - o.fold.z * o.fold.z + (pattern - 0.5) * 0.2;
  caps.crest = saturate((oceanLook3.x + 0.03 - J) / max(fwidth(J), 0.004) + 0.5) * smoothstep(0.1, 0.35, fresh);
  caps.young = max(caps.crest, fresh);
  return caps;
}
float2 OceanWhitecaps(OceanHere o, float2 xz, float2 dxdx, float2 dxdy, float lace) {
  OceanCaps c = OceanWhitecapsParts(o, xz, dxdx, dxdy, lace);
  // (thin at a patch's torn edge, the water showing through, thick and brighter in its middle; the residue thinner,
  // see-through and grey; far off and from high up all of it dimmer and less, so that the units on the sea read over it:
  // from a pixel of some 0.06 world units on, to half by 0.26)
  float far = saturate((max(length(dxdx), length(dxdy)) - 0.06) / 0.2);
  float body = lerp(0.6, 1, c.thick);
  float white = max(max(c.crest, c.laid * body), c.residue * body * lerp(0.8, 1, c.young)) * lerp(1, 0.5, far);
  float bright = lerp(0.5, 0.95, c.young) * lerp(0.8, 1.05, max(c.thick, c.crest));
  return float2(white, bright * lerp(1, 0.75, far));
}
)hlsl";

// [lift] HLSL for what lies on the water and is drawn after it (the wakes, wakes.cpp built with RIDE): a place on the still
// water carried to where the lifted surface has it, as the water's grid carries its own points (OceanDisplacement, its
// cell guessed from the distance; lakes not told apart). Its constants (b3) and textures (t13..t15, t20) are bound by
// Ocean::BindRide.
inline const char kOceanRideShaderBlock[] = R"hlsl(
cbuffer OceanRide : register(b3) {
  float4 rideCascade[3];  // as the water's oceanCascade
  float4 rideMap;         // x, y the map's width and height; z the ground's height unit; w the water's level
};
Texture2D rideDisp[3] : register(t13);
Texture2D<float> rideHeights : register(t20);
float3 OceanRide(float3 world) {
  float2 p = clamp(world.xz, 0, rideMap.xy);
  int2 c = int2(floor(p));
  float2 t = p - floor(p);
  int2 most = int2(rideMap.xy);
  float a = rideHeights.Load(int3(min(c, most), 0)), b = rideHeights.Load(int3(min(c + int2(1, 0), most), 0));
  float d = rideHeights.Load(int3(min(c + int2(0, 1), most), 0)), e = rideHeights.Load(int3(min(c + int2(1, 1), most), 0));
  float ground = lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * rideMap.z;
  float share = saturate((rideMap.w - ground - 0.6) / 3.0);
  if (share <= 0) return world;
  float cell = max(length(world - cameraPosition) / 64, 0.125);
  float3 shift = 0;
  [unroll] for (int k = 0; k < 3; ++k) {
    float lod = max(log2(cell * 256 * rideCascade[k].x) + 0.5, 0);
    shift += rideDisp[k].SampleLevel(samplerLinear, world.xz * rideCascade[k].x + rideCascade[k].w, lod).xyz * rideCascade[k].z;
  }
  return world + shift * share;
}
// [whitecaps] Where a line of sight from `eye` along `dir` (looking down) meets the lifted surface, as the place on the still
// water whose lifted point it is: what lies on the water (Decals::AddWater's pictures) is read there and so rides the waves
// as the water's own foam does. (The place found again from the lift there three times: the lift changes slowly over a
// place, and its shift is small against the waves' lengths.)
float3 OceanRideRest(float3 eye, float3 dir) {
  float level = rideMap.w;
  float down = min(dir.y, -1e-4);
  float2 rest = eye.xz + dir.xz * ((level - eye.y) / down);
  [unroll] for (int k = 0; k < 3; ++k) {
    float3 at = float3(rest.x, level, rest.y);
    float3 moved = OceanRide(at) - at;
    rest = eye.xz + dir.xz * ((level + moved.y - eye.y) / down) - moved.xz;
  }
  return float3(rest.x, level, rest.y);
}
)hlsl";

class Ocean {
 public:
  explicit Ocean(gfx::Device* device) : device_(device) {}
  ~Ocean();
  Ocean(const Ocean&) = delete;
  Ocean& operator=(const Ocean&) = delete;

  // The map whose water it is (Water::Load).
  void SetMap(const MapData* map);

  // --- each frame ---
  // At the frame's start (World::Begin): `time` the water's clock (seconds: the waves move on while a game is paused, as
  // the water's do); `on` the option and what it needs (the modern look, a map with water); `side` a frame drawn for
  // something else (the minimap): nothing moves on, the field of the last frame is shown; `rough` how rough the sea is
  // (0 still .. 1 a storm; eased); `wind` the wind's way (x, z; any length).
  void Begin(double time, bool on, bool side, float rough, const float wind[2]);
  // Works the field out for this frame and moves the foam on (it dispatches: between passes, before the scene's pass).
  // `heights` the ground's (Terrain::HeightTexture) for the foam's shallows.
  void Update(const Camera& camera, const View& view, gfx::Texture heights, float heightUnit);
  // Whether the water shows the ocean this frame; its constants (the shader's Ocean block, b2) and its textures (t10..t19).
  bool Active() const { return active_; }
  bool On() const { return on_ && !side_; }  // (this frame moves the field on: Update is to be called)
  const void* ShaderConstants() const { return &constants_; }
  uint32_t ShaderConstantsSize() const { return sizeof constants_; }
  void Bind();
  void Unbind();
  // Makes the compute pipelines and the targets, the first time; whether they are there.
  bool Ready();
  // [whitecaps] Makes (asks for) the compute pipelines alone, the first time (World::PrepareAhead: on the device's own thread
  // while the ocean is off, so that switching it on never waits for the driver); whether they are there.
  bool PreparePipelines();

  // [lift] Waves that lift the surface (World::oceanLift: set each frame before Update). Lifted(): the water draws the grid
  // this frame (its pipeline built with LIFT): DrawGrid binds the grid's buffers, constants (b3) and the ground's heights
  // (t20) and draws its patches, the water's pipeline and its other constants and textures set.
  bool lift = false;
  bool Lifted() const { return active_ && lift && !nodes_.empty(); }
  // [whitecaps] Whether what floats rides the waves this frame (BobAt answers): from the frame's Begin on (Lifted() is only
  // known from Update on, and the units are drawn, the effects moved and the debris moved before it: they asked Lifted() and
  // were always told no, so nothing bobbed), the long waves those of the frame before.
  bool Riding() const { return on_ && !side_ && lift && haveWater_ && !bobNow_.empty(); }
  void DrawGrid();
  // [views] For a view of the frame besides the first (World::extraView), after the first's Update: the field and the foam
  // as that left them, only the lifted grid's patches placed anew for this view (Update places them for the first).
  void PlaceGridFor(const Camera& camera, const View& view);
  int Patches() const { return static_cast<int>(nodes_.size()); }
  // [lift] For what is drawn on the water after it (kOceanRideShaderBlock): its constants (b3) and its textures bound.
  const void* RideConstants() const { return ride_; }
  uint32_t RideConstantsSize() const { return sizeof ride_; }
  void BindRide() const;
  void UnbindRide() const;
  // [lift] How the waves carry something floating at (x, z), `size` world units long (the waves shorter than about it
  // average out under it): `rise` how far above the water's level the surface stands there (world units), `slopeX`, `slopeZ`
  // the surface's slopes (dh/dx, dh/dz) under it. The long waves (those of the largest square and the longer of the middle
  // one's, kept on the processor: the strongest of them, KeepBobWaves) summed there now, as the card's field has them;
  // fading out over shallow water as the lift does. False (all 0) while nothing is lifted.
  struct Bob {
    float rise = 0, slopeX = 0, slopeZ = 0;
  };
  bool BobAt(float x, float z, float size, Bob* out) const;
  // [hulls] A hull floating in the sea this frame (World: each ship riding the waves, World::RideWaves), for where the waves
  // meet it (the water's shader: the white water round it, the hull below the surface hidden by the sea): `x`, `z` the
  // middle of its waterline, `forwardX`, `forwardZ` its way (any length), its waterline's half length and half beam, its
  // speed (world units a second), the bob it rides (its waterline's height is the water's level plus its rise). Listed
  // anew each frame from Begin on; the kHulls nearest the eye go to the shader (Update).
  void AddHull(float x, float z, float forwardX, float forwardZ, float halfLength, float halfBeam, float speed, const Bob& bob);
  static constexpr int kHulls = 24;
  int Hulls() const { return hullCount_; }  // (those the shader was told of this frame)

  // Tuning (viewer --ocean-*): the waves' height times (1), how rough the sea is whatever the weather (-1: by it), the
  // glint's (0.8: the glitter a little under a sea's own, nearer today's water), the foam's and the glow's strength (1).
  float heightScale = 1, roughness = -1, glint = 0.8f, foamStrength = 1, glow = 1;
  // How much steeper the slopes are drawn than they are (seen from above twice that again: there the slopes are all that
  // shows of the waves).
  float tilt = 1.5f;
  // A view for checking (viewer --ocean-view n): the water shows 1 the slopes (red x, green z), 2 the foam field, 3 the
  // slopes' spread, 4 the height (red up, blue down), 5 the Jacobian, 6 [whitecaps] the whitecaps' parts; 0 off.
  int view = 0;

  // The sea as it is now, for the log: how rough (eased), its significant wave height, the peak's length, the foam field's
  // cell.
  float Rough() const { return rough_; }
  float SignificantHeight() const { return hs_; }
  float PeakLength() const { return peak_; }
  float FoamCell() const { return foamCell_; }

  static constexpr int kN = 256;  // a square's samples a side
  static constexpr int kCascades = 3;
  static constexpr int kFoamCells = 1024;
  static constexpr int kPatchCells = 32;   // [lift] a patch's cells a side
  static constexpr int kMostPatches = 1024;
  static constexpr int kBobWaves = 640;     // [lift] the long waves kept on the processor for BobAt

 private:
  struct Constants {
    float cascade[kCascades][4];
    float wind[4];
    float foamArea[4];
    float look[4];
    float look2[4];
    float look3[4];
    float look4[4];  // [whitecaps]
    float hulls[kHulls * 3][4];  // [hulls] (AddHull's, the nearest)
  };
  // [hulls] the hulls listed this frame (AddHull), and how many the shader was told of
  struct Hull {
    float x, z, fx, fz, halfLength, halfBeam, speed, waterline, slopeX, slopeZ, away;
  };
  std::vector<Hull> hulls_;
  int hullCount_ = 0;
  void Spectrum();  // the spectrum's numbers for this frame, from rough_ and the wind (sim_)
  // [lift] The grid's patches for this view (the quadtree walked from its root, farthest first), and the ordering of a
  // patch's triangles back to front.
  void PlaceGrid(const View& view, float reach);
  void Select(float x, float z, int level, const View& view, Vec3 eye, float reach);
  bool MakeGrid();
  void KeepBobWaves();  // the strongest long waves, when the sea has changed (BobAt)

  gfx::Device* device_;
  const MapData* map_ = nullptr;
  bool haveWater_ = false;
  float level_ = 0;
  bool tried_ = false, made_ = false;
  bool pipesTried_ = false;  // [whitecaps] (PreparePipelines)
  std::string pipesError_;
  gfx::Pipeline rows_, columns_, mipsNear_, mipsFar_, foam_;
  gfx::Texture rowOut_[kCascades][2];   // the row pass's four complex sums (two a texel), RGBA32F
  gfx::Texture base_[kCascades][3];     // the column pass's slope, shift and fold (no levels)
  gfx::Texture field_[kCascades][3];    // the same with their levels (what the water reads)
  gfx::Texture top_[kCascades][3];      // their level 4 (16 x 16), from which the far levels are made
  gfx::Texture foamMap_[2];
  int foamCur_ = 0;
  bool on_ = false, side_ = false, active_ = false, foamFresh_ = true;
  double time_ = 0, lastTime_ = -1;
  float rough_ = -1, hs_ = 0, peak_ = 0;
  float wind_[2] = {0.915f, 0.403f};
  float foamCell_ = 0;
  int foamX_ = 0, foamZ_ = 0;
  double driftX_ = 0, driftZ_ = 0;  // [whitecaps] how far the foam has drifted since the field began (world units)
  float foamArea_[4] = {0, 0, 0, 0};
  float sim_[16] = {};  // (the compute passes' spectrum numbers: Spectrum)
  double seaAlpha_ = 0, swellAlpha_ = 0, wp_ = 1;
  float foamThreshold_ = 0.5f;
  Constants constants_ = {};
  // [lift] the grid
  struct GridConstants {
    float info[4];
    float lods[16][4];
    float nodes[kMostPatches][4];
  };
  gfx::Buffer gridVertices_, gridIndices_;
  bool gridTried_ = false;
  int gridOrder_ = 0;      // which of the eight orderings of a patch's triangles
  float rootX_ = 0, rootZ_ = 0, mapW_ = 0, mapH_ = 0;
  int rootLevel_ = 0;
  float ranges_[16] = {};
  struct Node {
    float x, z, size, level, away;
  };
  std::vector<Node> nodes_;
  GridConstants grid_ = {};
  float ride_[4][4] = {};  // (kOceanRideShaderBlock's constants)
  gfx::Texture heights_;
  float heightUnit_ = 1;
  // [lift] the long waves BobAt sums: each its wave vector, its frequency and its two halves' amplitudes (complex), kept
  // when the sea changed (keptRough_, keptWind_)
  struct BobWave {
    float kx, kz, w;
    float a0r, a0i, a1r, a1i;  // h0(k) and conj(h0(-k)), without their amplitudes
    float amp = 0, ampOpp = 0;  // [whitecaps] their amplitudes (as the sea's numbers were in ampSim_)
    int square = 0;
  };
  std::vector<BobWave> bobWaves_;
  // [whitecaps] the set chosen again a few rows of candidates a frame (KeepBobWaves), from the sea's numbers as they were
  struct BobPick {
    struct Candidate {
      float energy;
      int c, n, m;
    };
    bool running = false;
    float sim[16] = {};
    std::vector<Candidate> found;
    int square = 0, row = 0;
  };
  BobPick pick_;
  float ampSim_[16] = {};
  struct BobNow {
    float kx, kz, hr, hi;  // the wave now: h(k, t)
  };
  std::vector<BobNow> bobNow_;
  float keptRough_ = -1, keptWind_[2] = {0, 0}, keptHeight_ = -1;
};

}  // namespace render
