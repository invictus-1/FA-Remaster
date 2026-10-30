#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "lighting.h"
#include "ocean.h"
#include "ray_scene.h"
#include "surf.h"
#include "water_glint.h"
#include "winter_water.h"

namespace render {
namespace {

constexpr int kWaterGroundSlot = 20;  // [floatfoam] t20: the map's heights (the lifted water's OceanGround reads them there too)
constexpr int kWaterAfloatSlot = 3;   // [floatfoam] b3: what floats (the lifted water's grid has its own constants there)
constexpr size_t kAfloatMost = 64;    // [floatfoam] (the nearest to the eye; as the shader's WaterAfloat holds)

// What the original's water computes per pixel, written anew:
//   normal   = the four scrolling wave normal maps added up, flattened where the map says so
//   under    = what is below the surface, tinted toward the surface colour by depth
//   result   = under blended toward the reflected sky by the fresnel term, plus the sun's glint,
//              plus white wave crests
// The original takes "what is below" from a copy of the picture and bends it by the normal (refraction).
// Here the sheet is blended over the picture instead, which gives the same colours without the bending:
//   result = below * (1 - a) + rgb, with rgb and a worked out from the formula above.
// Still missing (for the stage that matches the original's look): the refraction, reflected units, and
// the fresnel lookup texture the original uses (the term below is the usual bias-and-power form, an
// assumption until that texture is understood).
const char kShader[] = R"hlsl(
cbuffer WaterConstants : register(b1) {
  float4 mapSize;         // width, height, 1 / width, 1 / height
  float4 surfaceColor;    // rgb, w: elevation
  float4 lerpAndFresnel;  // colour lerp min, max, fresnel bias, fresnel power
  float4 reflection;      // unit reflection, sky reflection, sun shininess, sun glow
  float4 waterSunDirection;  // the direction the sun's light travels, as the map's water settings give it
  float4 waterSunColor;
  float4 normalRepeat;
  float4 movement[4];
};
Texture2D waterMap : register(t0);
Texture2D waves[4] : register(t1);
TextureCube sky : register(t5);

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 corner : POSITION) {
  VOut o;
  // (with a world beyond the map's edge the sheet reaches as far out as the ground does)
  float reach = beyond.z > 0 ? 1 / beyond.z : 0;
  o.world = float3(lerp(-reach, mapSize.x + reach, corner.x), surfaceColor.w, lerp(-reach, mapSize.y + reach, corner.y));
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float2 xz = i.world.xz;
  float4 water = waterMap.Sample(samplerLinearClamp, MirrorUv(xz * mapSize.zw));  // r: waviness, g: depth, a: no crests
  float depth = water.g;
  float3 toPixel = normalize(i.world - cameraPosition);

  float4 sum = waves[0].Sample(samplerLinear, (xz + movement[0].xy * time) * normalRepeat.x)
             + waves[1].Sample(samplerLinear, (xz + movement[1].xy * time) * normalRepeat.y)
             + waves[2].Sample(samplerLinear, (xz + movement[2].xy * time) * normalRepeat.z)
             + waves[3].Sample(samplerLinear, (xz + movement[3].xy * time) * normalRepeat.w);
  float crest = saturate(sum.a - 1);
  float3 N = normalize((2 * sum.xyz - 4).xzy);
  N = lerp(float3(0, 1, 0), N, water.r);

  float3 R = reflect(toPixel, N);
  float3 skyColor = sky.Sample(samplerLinear, R).rgb;
  float facing = saturate(dot(-toPixel, N));
  float fresnel = saturate(lerpAndFresnel.z + (1 - lerpAndFresnel.z) * pow(1 - facing, lerpAndFresnel.w));
  float3 sun = pow(saturate(dot(-R, waterSunDirection.xyz)), reflection.z) * waterSunColor.rgb * fresnel;
  float daylight = 1;
  if (look > 0.5 && skyZenith.w > 0.5) {
    // the computed sky: it is what the water mirrors, and the glint is the falling light's. (The
    // water's own colours are the map's, made for daylight: all of it is dimmed by how bright the
    // day is in the last line, so the mirrored sky is first brought up by as much.)
    daylight = skyGlow.w * lerp(1, 0.45, skyHorizon.w);  // (and less by night: its day colours would glow)
    skyColor = pow(saturate(ComputedSky(R) / (daylight * lightMultiplier * modern.x * 0.55)), 1 / 2.2);
    sun = pow(saturate(dot(R, sunDirection)), reflection.z) * sunColor * fresnel;
  }

  float tint = clamp(depth, lerpAndFresnel.x, lerpAndFresnel.y);
  float k = saturate(reflection.y * saturate(depth * 10) * fresnel);
  float3 rgb = surfaceColor.rgb * tint * (1 - k) + skyColor * k + sun;
  float through = (1 - tint) * (1 - k);
  float c = (1 - water.a) * crest;
  rgb = rgb * (1 - c) + c;
  through *= 1 - c;
  // the modern look works in linear light: the same water for now, its colours converted
  if (look > 0.5) rgb = ThroughAirTo(pow(max(rgb, 0), 2.2) * (lightMultiplier * modern.x * 0.55 * daylight), i.world);
  return float4(BeyondMap(rgb, i.world), 1 - through);
}
)hlsl";

// The modern look's water. Per pixel:
//   behind   = the picture as it was before the water (the bed, what stands in the water), read a
//              little to the side by the waves (refraction), and what the water takes from it on
//              the way: red first, then green, by how much water lies between
//   body     = the water's own colour where it is deep, by the light that falls on it
//   mirror   = the sky, and what the picture shows in the mirrored direction (land, units: found by
//              walking along that direction through the picture's depth), by the angle of view
//   foam     = where the water is shallow (the shore, and round whatever stands in it), and the
//              waves' crests
//   caustics = the bright net the waves throw on a shallow bed in sunlight
// It writes the finished colour (nothing is blended), and says per sample of the pixel whether the
// water is in front there, since the scene's depth can only be read here, not tested by the card.
const char kModernShader[] = R"hlsl(
cbuffer WaterConstants : register(b1) {
  float4 mapSize;         // width, height, 1 / width, 1 / height
  float4 surfaceColor;    // rgb, w: elevation
  float4 lerpAndFresnel;
  float4 reflection;
  float4 waterSunDirection;
  float4 waterSunColor;
  float4 normalRepeat;
  float4 movement[4];
  float4 cameraRight;     // w: tan(half the field of view)
  float4 cameraUp;        // w: aspect
  float4 cameraBack;      // w: near distance
  float4 groundInfo;      // [floatfoam] x the ground's height unit (0: the ground's heights not bound, Water::SetGround)
#if RIPPLES || SURF
  float4 rippleArea;      // the ripples' field (ripples.h): x, y its first cell's corner (world x, z); z 1 / its side; w a cell's size
  float4 rippleLook;      // x how much its slopes tilt the surface; y the slope from which its crests foam; z how much of its height the waterline takes
#endif
#if SURF
  float4 surfArea;        // [surf] the shore field (surf.h): x, y its first texel's corner (world x, z); z, w 1 / its width and height
  float4 surfLook;        // x the waves' height near the shore; y their phase (cycles); z their length at sea; w the run-up's height
  float4 surfLook2;       // x how rough the sea is; y the depth a wave breaks at, by its height; z the run-up's share of a period; w the white water's strength
  float4 surfLook3;       // x a view for checking (Surf::view: 1 the field, 2 the waves; 0 off); y [shore] how much of the beach's
                          // wetness the surf lays (Surf::WetShare; read with World::waterDetail alone)
#endif
};
Texture2D waterMap : register(t0);
Texture2D waves[4] : register(t1);
TextureCube sky : register(t5);
Texture2D behind : register(t6);
#if RIPPLES
Texture2D rippleSurface : register(t8);  // the field's height, and its slope along x and along z
#endif
#if SAMPLES > 1
Texture2DMS<float> sceneDepth : register(t7);
#define SCENE_DEPTH(at, s) sceneDepth.Load(at, s)
#else
Texture2D<float> sceneDepth : register(t7);  // (one sample a pixel: an ordinary depth target)
#define SCENE_DEPTH(at, s) sceneDepth.Load(int3(at, 0))
#endif

static const float kRise = 0.07;  // how far the swell carries the water above and below its level
// [shore] (World::waterDetail) the sun's glint widened by the waves a pixel takes in (water_glint.h, SoftGlint): from a pixel
// of kGlintFrom world units (the game's usual height: as before) to one of kGlintTo (the strategic view), by slopes of
// that spread (the lobe then about a power of 40: a sheen)
static const float kGlintFrom = 0.08, kGlintTo = 2.0, kGlintSlopes = 0.006;
#if SURF_WET
// [shore] (World::waterDetail with the surf: the surf's pipelines built with SURF_WET) how much higher again the sheet lies,
// for the wet beach above the water (WetBeach: as high as the damp band reaches in a storm on an exposed beach)
static const float kSurfWetRise = 1.5;
#endif
#if SURF
// [shorewall] How much of the sheet's extra height for the surf (kSurfRise, and kSurfWetRise) it takes: all of it but under a
// camera low over the water (a ship looked at from about its own deck's height), where the raised sheet would
// lie above the eye: seen from below it was drawn as a wall rising over the beach and the horizon toward the picture's
// sides (the lifted ocean, whose sheet rises to that height over the shallows alone), or not drawn at all (the flat sheet).
// The sheet then lies at most three quarters of the way up from the water to the eye (it still covers all the beach lower
// than that, the run-up's and the wet beach's, and a little room is left for its far side seen edge on); the place each
// pixel is worked out at is moved down by the same share (PS), so the water stays where it was.
float SheetRoom() {
  float extra = kSurfRise;
#if SURF_WET
  extra += kSurfWetRise;
#endif
  return saturate(((cameraPosition.y - surfaceColor.w) * 0.75 - kRise) / extra);
}
#endif

#if LIFT
// [lift] (ocean.h, World::oceanLift) The water as the camera's grid, lifted by the waves: each vertex of a patch (`corner`
// 0..1 across it) slid onto the coarser grid as far as its distance says (CDLOD: at a patch's own level, and, for a
// quarter drawn at its parent's resolution, at the next), then shifted and lifted by the ocean's field as much as the
// water there lets it (OceanLiftShare: none over the shallows, where the flat sheet's waterline, the surf and the beach
// stay as they were). `rest`: where on the still sheet the point rests (x, z), and how much it is lifted.
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 rest : TEXCOORD1;
};

VOut VS(float2 corner : POSITION, uint instance : SV_InstanceID) {
  VOut o;
  float4 node = gridNodes[instance];
  int level = int(node.w);
  float cells = gridInfo.x;
  float2 at = node.xy + corner * node.z;
  float away = length(float3(at.x, surfaceColor.w, at.y) - cameraPosition);
  float2 cell = corner * cells;
  cell -= frac(cell * 0.5) * 2 * saturate((away - gridLods[level].x) * gridLods[level].y);
  cell -= frac(cell * 0.25) * 4 * saturate((away - gridLods[min(level + 1, 15)].x) * gridLods[min(level + 1, 15)].y);
  float2 x0 = node.xy + cell * (node.z / cells);
  float share = OceanLiftShare(x0, surfaceColor.w, mapSize.xy, waterMap.SampleLevel(samplerLinearClamp, MirrorUv(x0 * mapSize.zw), 0).r);
  float3 d = 0;
  [branch] if (share > 0) d = OceanDisplacement(x0, node.z / cells) * share;
  // (where it is not lifted the sheet lies as high as the flat one does: the swell's crests, the run-up, the wet beach)
  float rise = kRise;
#if SURF
  float room = SheetRoom();  // [shorewall] (never above the eye)
  rise += kSurfRise * room;
#if SURF_WET
  rise += kSurfWetRise * room;
#endif
#endif
  o.world = float3(x0.x + d.x, surfaceColor.w + d.y + rise * (1 - share), x0.y + d.z);
  o.rest = float3(x0, share);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}
#else
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 corner : POSITION) {
  VOut o;
  // (the sheet lies as high as the swell's crests reach; where the water stands lower just now is
  // worked out per pixel)
  float reach = beyond.z > 0 ? 1 / beyond.z : 0;
#if SURF
  // [surf] (and as high as the surf's run-up reaches)
  float room = SheetRoom();  // [shorewall] (never above the eye)
  o.world = float3(lerp(-reach, mapSize.x + reach, corner.x), surfaceColor.w + kRise + kSurfRise * room, lerp(-reach, mapSize.y + reach, corner.y));
#if SURF_WET
  o.world.y += kSurfWetRise * room;  // [shore] (with World::waterDetail as high as the beach it wets)
#endif
#else
  o.world = float3(lerp(-reach, mapSize.x + reach, corner.x), surfaceColor.w + kRise, lerp(-reach, mapSize.y + reach, corner.y));
#endif
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}
#endif

// The swell: four trains of waves crossing the water, longer ones faster. x: the height; y, z: how
// it slopes along x and z. grain: how much water one pixel covers (world units): a train whose
// waves are shorter than about three pixels is left out (far off and seen at a low angle they
// would no longer be waves but rings of false pattern round the eye).
float3 Swell(float2 xz, float grain) {
  static const float4 trains[4] = {  // the way it runs (x, z), its length, its height
    float4(0.86, 0.50, 23.0, 0.060), float4(0.60, 0.80, 13.0, 0.040), float4(0.97, -0.24, 7.5, 0.022), float4(0.31, 0.95, 4.3, 0.012)};
  float3 sum = 0;
  for (int k = 0; k < 4; ++k) {
    float shows = saturate(trains[k].z / max(grain, 1e-4) * 0.5 - 1);  // (all of it from waves four pixels long)
    if (shows <= 0) continue;
    float turns = 6.2832 / trains[k].z;
    float phase = dot(trains[k].xy, xz) * turns - time * 0.55 * sqrt(trains[k].z) * turns;
    sum.x += shows * trains[k].w * sin(phase);
    sum.yz += trains[k].xy * (shows * trains[k].w * turns * cos(phase));
  }
  return sum;
}

// Foam's lace: noise of four sizes, each turned against the last so that no grid shows, the fine
// ones drifting. 0..1, mostly about a half. grain: how much water one pixel covers (world units):
// a size of noise finer than two pixels is left at its mean, or from far up every pixel would draw
// its own value and the sea would be salted with white points (Chris, 2026-10-13).
float Lace(float2 q, float grain) {
  float2 a = float2(q.x * 0.80 - q.y * 0.60, q.x * 0.60 + q.y * 0.80);
  float2 b = float2(q.x * 0.28 + q.y * 0.96, -q.x * 0.96 + q.y * 0.28);
  float t = time * 0.12;
  float4 shown = saturate(1.5 - grain * float4(2.3, 5.9, 13.7, 31.0) * 2);
  float4 n = 0.5;
  if (shown.x > 0) n.x = CloudNoise(float3(q * 2.3, t));
  if (shown.y > 0) n.y = CloudNoise(float3(a * 5.9 + t, t * 1.7));
  if (shown.z > 0) n.z = CloudNoise(float3(b * 13.7 - t * 2, t * 2.3));
  if (shown.w > 0) n.w = CloudNoise(float3(q * 31.0 + t * 3, t * 3.1));
  return dot(lerp(0.5, n, shown), float4(0.40, 0.28, 0.20, 0.12));
}

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / max(depth, 1e-6);
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

#if !LIFT
// [floatfoam] The map's ground (Terrain::HeightTexture, one a grid point; Water::SetGround), for telling what stands off it
// in the water. (The lifted water reads it as OceanGround, ocean.h.)
Texture2D<float> waterGround : register(t20);
// The highest of the four grid points round a place (one fetch): never below the ground the terrain draws there, so that
// a steep shore or a rock of the map's own falling into deep water is not taken for something standing off the ground.
float GroundTop(float2 xz) {
  float2 p = clamp(xz, 0, mapSize.xy);
  float4 g = waterGround.GatherRed(samplerPointClamp, (floor(p) + 1) / (mapSize.xy + 1));
  return max(max(g.x, g.y), max(g.z, g.w)) * groundInfo.x;
}
static const float kThroughOver = 0.12, kThroughNear = 0.1;  // (ThroughSurface's test)
// What floats on the water (Water::AddAfloat): the units in the water, each a box round its model seen from above (rows of
// two: its middle x, z and its way forward x, z; its half length and half beam, a margin added). Never more than kAfloatMost.
cbuffer WaterAfloat : register(b3) {
  float4 afloatCount;      // x how many
  float4 afloat[2 * 64];
};
// Whether a place lies under something floating: 1 inside one of the boxes, 0 outside them all (soft over the last tenth).
float UnderAfloat(float2 xz) {
  float under = 0;
  int count = int(afloatCount.x);
  for (int k = 0; k < count; ++k) {
    float4 a = afloat[k * 2], b = afloat[k * 2 + 1];
    float2 d = xz - a.xy;
    float2 outside = abs(float2(dot(d, a.zw), dot(d, float2(-a.w, a.z)))) - b.xy;
    under = max(under, saturate(1 - max(outside.x, outside.y) / 0.1));
  }
  return under;
}
// Whether what lies behind the water at `bed` goes up through the surface right there, as a hull's side does at its
// waterline: the place straight above it, a hand's breadth over the water's level, is on what the eye sees there (the
// scene about as far from the eye as that place). Over a float under the water it is open (the scene there lies
// farther: the float under it), or hidden by what stands nearer (the hull over the float). 1 it does, 0 it does not.
// (Off the screen: taken as going up.)
float ThroughSurface(float3 bed, float level) {
  float3 place = float3(bed.x, level + kThroughOver, bed.z);
  float4 clip = mul(viewProj, float4(place, 1));
  if (clip.w <= 1e-4) return 1;
  float2 at = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * viewport.xy;
  if (any(at < 0) || any(at >= viewport.xy)) return 1;
  float d = length(WorldAt(at, SCENE_DEPTH(int2(at), 0)) - cameraPosition) - length(place - cameraPosition);
  return saturate(1 - abs(d + 0.03) / kThroughNear);
}
#endif

#if SURF_WET
// [shore] (World::waterDetail with the surf: SURF_WET) The beach just above the water as the surf wets it (surf.h, SurfWetAt), at the
// pixel's ground `bed`: the ground as the ground's pass drew it (`behind`, its own wet band by height narrowed meanwhile to
// what a calm bay keeps, World::Begin) darker as far up as the waves wet it, darkest where they run up an exposed beach;
// the film a wave leaves glistening as it drains, the sky in it and the sun's glint. Only where the picture shows the map's
// ground (a unit, a tree, a rock standing on the beach is left as it is: those samples are taken out of `coverage`); a
// pixel left alone is discarded. `grain`: how much ground one pixel covers.
float4 WetBeach(float2 pixel, float3 bed, float grain, inout uint coverage) {
  SurfWet s = SurfWetAt(bed.xz, bed.y, surfaceColor.w, mapSize.xy, surfArea, surfLook, surfLook2);
  // (how far over the field's ground the map's ground may lie: the ground's mesh is coarser far off, steeper ground more)
  float slack = 0.12 + 0.5 * length(s.up.xz) + grain * 0.5;
  [unroll] for (int k = 0; k < SAMPLES; ++k)
    if ((coverage >> k) & 1u) {
      if (WorldAt(pixel, SCENE_DEPTH(int2(pixel), k)).y - s.ground > slack) coverage &= ~(1u << k);
    }
  // (a little less where rain has wetted all the ground already, none under lying snow; fading in with the surf; and only up
  // to the sheet's height: the water's pass sees no ground above it)
  float share = surfLook3.y * (1 - 0.25 * weather.x) * (1 - weather.y) * saturate((surfaceColor.w + kRise + kSurfRise + kSurfWetRise - bed.y) / 0.15);
  float wet = s.wet * share, film = s.film * share;
  // (a view for checking, Surf::view 3: red the film, green how wet; the water itself as it is)
  if (surfLook3.x > 2.5) return float4(film, wet * 0.5, 0, 1);
  if (coverage == 0 || wet < 0.004) discard;
  // what the ground's pass laid there already (its band by height: terrain.cpp's formula, the view's shore)
  float over = bed.y - shore.y;
  float groundWet = saturate(1 - (over + (CloudNoise(float3(bed.xz * 0.7, 2.5)) - 0.5) * shore.z * 0.8) / max(shore.z, 1e-3)) * saturate(over * 20 + 1);
  // soaked sand: its albedo as the ground's band has it (0.6) in a calm bay, darker on an exposed beach, a little more under
  // the film; the darker of the two
  float had = lerp(1, 0.6, groundWet);
  float want = lerp(1, lerp(0.6, 0.48, s.open) * (1 - 0.1 * film), wet);
  float3 c = behind.SampleLevel(samplerLinearClamp, pixel * viewport.zw, 0).rgb * (min(want, had) / had);
  // the film's sheen, as of a thin sheet of water: the sky in it, and the sun's light off it (the sharper the fresher the
  // film, widened by what a pixel takes in as the water's glint is, water_glint.h). (The damp sand above it has drunk its
  // water: matt but for a trace of the sky. Where the ground's own band lies, that lights its sheen itself, roughness 0.22.)
  float3 toPixel = normalize(bed - cameraPosition);
  float3 n = s.up;
  float3 R = reflect(toPixel, n);
  R.y = max(R.y, 0.02);
  R = normalize(R);
  float fresnel = 0.02 + 0.98 * pow(1 - saturate(dot(-toPixel, n)), 5);
  float3 mirror = skyZenith.w > 0.5 ? ComputedSky(R) : ToLinear(sky.Sample(samplerLinear, R).rgb) * (lightMultiplier * modern.x * 0.5);
  float3 air = ThroughAirTo(0, bed);
  c = lerp(c, ThroughAirTo(mirror, bed), fresnel * (0.04 * wet + 0.6 * film));
  float shadow = SunShadow(bed, s.up) * CloudShadow(bed);
  float3 H = normalize(sunDirection - toPixel);
  float rough = lerp(0.3, 0.12, film);
  float a2 = rough * rough * rough * rough + 4 * kGlintSlopes * smoothstep(kGlintFrom, kGlintTo, grain);
  float dd = saturate(dot(n, H)) * saturate(dot(n, H)) * (a2 - 1) + 1;
  float D = a2 / (3.14159 * dd * dd);
  float F = 0.02 + 0.98 * pow(1 - saturate(dot(H, -toPixel)), 5);
  float spec = min(D * F * saturate(dot(n, sunDirection)) / (4 * max(dot(n, -toPixel), 0.05)), 20);
  c += ThroughAirTo(ToLinear(sunColor) * (lightMultiplier * modern.x) * (spec * 0.7 * film * (1 - groundWet) * shadow), bed) - air;
  return float4(c, 1);
}
#endif

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  // where in the pixel the water is in front (a nearer thing has the larger depth)
  int2 pixel = int2(i.position.xy);
  coverage = 0;
  float depthBehind = 1;
  for (int s = 0; s < SAMPLES; ++s) {
    float d = SCENE_DEPTH(pixel, s);
    if (d <= i.position.z) {
      coverage |= 1u << s;
      depthBehind = min(depthBehind, d);
    }
  }
  if (coverage == 0) discard;
#if SURF
  // [surf] the sheet lies higher by kSurfRise, for the run-up; all else is worked out where it lies without the surf (so
  // that the water away from the shores is as without it): the place moved down the line of sight to that height, and
  // its depth on the screen with it
#if LIFT
  float unlifted = 1 - i.rest.z;  // [lift] (the sheet lies higher only where it is not lifted)
#else
  float unlifted = 1;
#endif
  unlifted *= SheetRoom();  // [shorewall] (as high as the vertex shader raised it)
#if SURF_WET
  // ([shore] with World::waterDetail higher again by kSurfWetRise, for WetBeach)
  i.world += normalize(i.world - cameraPosition) * (kSurfWetRise * unlifted / max(normalize(cameraPosition - i.world).y, 1e-3));
#endif
  i.world += normalize(i.world - cameraPosition) * (kSurfRise * unlifted / max(normalize(cameraPosition - i.world).y, 1e-3));
  float4 sheet = mul(viewProj, float4(i.world, 1));
  i.position.z = sheet.z / sheet.w;
#if SURF_WET
  // [shore] ground higher than the water ever stands (the swell's crest and the run-up's top over its level): the beach the
  // surf wets, nothing of the water
  {
    float3 ground = WorldAt(i.position.xy, depthBehind);
    float wetGrain = max(length(ddx(i.world.xz)), length(ddy(i.world.xz)));
    [branch] if (ground.y > surfaceColor.w + kRise + kSurfRise) return WetBeach(i.position.xy, ground, wetGrain, coverage);
  }
#endif
#endif

#if LIFT
  float2 xz = i.rest.xy;  // [lift] (what lies on the water rides it: read where the point rests on the still sheet)
#else
  float2 xz = i.world.xz;
#endif
  float4 water = waterMap.Sample(samplerLinearClamp, MirrorUv(xz * mapSize.zw));  // r: waviness, a: no crests
  float3 toPixel = normalize(i.world - cameraPosition);
  float distance = length(i.world - cameraPosition);
#if OCEAN
  // [ocean] (ocean.h, World::oceanWaves) the waves of the wind sea and the swell worked out on the card, in place of the four
  // scrolling wave pictures and the swell's four trains: their mean slope over what the pixel takes in tilts the surface,
  // their spread widens the sun's glint (below); calmer on lakes (the map's waviness), in bays (with the surf: how exposed
  // the shore before it is) and over shallows
  float grain = max(length(ddx(xz)), length(ddy(xz)));  // (the water one pixel covers)
  float oceanCalm = (0.35 + 0.65 * water.r) * saturate((surfaceColor.w - WorldAt(i.position.xy, depthBehind).y) / 2.5 + 0.15);
#if SURF
  oceanCalm *= lerp(0.4, 1, surfField.SampleLevel(samplerLinearClamp, (xz - surfArea.xy) * surfArea.zw, 0).a);
#endif
  OceanHere ocean = OceanAt(xz, ddx(xz), ddy(xz), oceanCalm, oceanLook2.z * lerp(1.0, 2.0, saturate(-toPixel.y * 2.5)));
  [branch] if (oceanLook.w > 0.5) {  // (a view for checking, Ocean::view)
    if (oceanLook.w < 1.5) return float4(saturate(ocean.slope * 4 + 0.5), 0, 1);
    if (oceanLook.w < 2.5) return float4(saturate(ocean.foam), saturate(ocean.foam), saturate(ocean.foam) * 0.5 + 0.2, 1);
    if (oceanLook.w < 3.5) return float4(saturate(sqrt(ocean.spread.xy) * 5), 0, 1);
    if (oceanLook.w > 5.5) {  // ([whitecaps] 6: the whitecaps' parts, red the crest, green the fresh foam, blue the residue)
      OceanCaps c = OceanWhitecapsParts(ocean, xz, ddx(xz), ddy(xz), Lace(xz, grain));
      return float4(c.crest, c.laid, c.residue, 1);
    }
    if (oceanLook.w > 4.5) {  // (5: the Jacobian, red where it folds below the foam's threshold, green 1 .. black 0)
      float J = (1 + ocean.fold.x) * (1 + ocean.fold.y) - ocean.fold.z * ocean.fold.z;
      return float4(J < oceanLook3.x ? 1 : 0, saturate(J - 0.5), 0, 1);
    }
    return float4(saturate(ocean.height * 8), 0.1, saturate(-ocean.height * 8), 1);
  }
  float crest = 0;
  float3 N = normalize(float3(-ocean.slope.x, 1, -ocean.slope.y));
  float3 swell = float3(ocean.height, 0, 0);
#else
  float4 sum = waves[0].Sample(samplerLinear, (xz + movement[0].xy * time) * normalRepeat.x)
             + waves[1].Sample(samplerLinear, (xz + movement[1].xy * time) * normalRepeat.y)
             + waves[2].Sample(samplerLinear, (xz + movement[2].xy * time) * normalRepeat.z)
             + waves[3].Sample(samplerLinear, (xz + movement[3].xy * time) * normalRepeat.w);
  float crest = saturate(sum.a - 1) * (1 - water.a) * 0.25;
  float3 N = normalize((2 * sum.xyz - 4).xzy);
  N = normalize(lerp(float3(0, 1, 0), N, water.r));
  // the swell: it tilts the surface (more than its true slope: seen from above, the tilt is all
  // that shows of it), calmer where the map says the water is (lakes), higher when it rains
  float grain = max(length(ddx(xz)), length(ddy(xz)));  // (the water one pixel covers)
  float3 swell = Swell(xz, grain) * ((0.35 + 0.65 * water.r) * (1 + 1.2 * falling.x));
  // (the tilt is overdone for the view from above, where it is all that shows of the swell; toward
  // the horizon, where the water is seen at a low angle, the waves show by themselves, and overdone
  // they stood as ruled stripes across the far sea)
  N = normalize(N + float3(-swell.y, 0, -swell.z) * lerp(1.0, 2.5, saturate(-toPixel.y * 2.5)));
#endif
  // near the eye: fine ripples, two sizes of them crossing, which the game's wave textures are too
  // coarse to show (Chris, 2026-10-13: "if you go up close it looks very pixelated"). Each size is
  // left out where it would be finer than about two pixels.
  float2 fine = saturate(1.5 - grain * float2(4.0, 9.0) * 2);
  [branch] if (fine.x > 0) {
    const float e = 0.06;
    float t = time * 0.5;
    float2 q = xz * 4.0 + float2(t, t * 0.7);
    float h = CloudNoise(float3(q, t));
    float2 tilt = float2(CloudNoise(float3(q + float2(e, 0), t)) - h, CloudNoise(float3(q + float2(0, e), t)) - h) * (fine.x * 0.13 / e);
    if (fine.y > 0) {
      // (turned against the first, so that no grid shows)
      float2 r = float2(xz.x * 0.8 - xz.y * 0.6, xz.x * 0.6 + xz.y * 0.8) * 9.0 - float2(t * 1.3, t);
      float g = CloudNoise(float3(r, t * 1.7));
      float2 turned = float2(CloudNoise(float3(r + float2(e, 0), t * 1.7)) - g, CloudNoise(float3(r + float2(0, e), t * 1.7)) - g) * (fine.y * 0.09 / e);
      tilt += float2(turned.x * 0.8 + turned.y * 0.6, -turned.x * 0.6 + turned.y * 0.8);
    }
    N = normalize(N + float3(-tilt.x, 0, -tilt.y) * (0.4 + 0.6 * water.r));
  }
  // rain: every small square of water has a drop falling into it again and again; its ring runs
  // outward, tilting the surface, and the whole surface is roughened
  float rings = 0;
  [branch] if (falling.x > 0 && distance < 220) {
    float2 g = xz * 2.2;
    float2 cell = floor(g);
    float h = CloudHash(float3(cell, 3.1));
    float age = frac(time * (0.8 + h) + h * 9.0);
    float2 from = g - (cell + 0.2 + 0.6 * float2(CloudHash(float3(cell, 6.7)), CloudHash(float3(cell, 11.3))));
    float away = length(from);
    float ring = saturate(1 - abs(away - age * 0.5) * 7) * (1 - age) * step(h, 0.08 + 0.3 * falling.x) * saturate(1 - distance / 220);
    N = normalize(N + float3(from.x, 0, from.y) * (ring * 0.9 / max(away, 0.05)));
    rings = ring;
    N = normalize(N + float3(CloudNoise(float3(xz * 9, time * 3)) - 0.5, 0, CloudNoise(float3(xz * 9 + 5, time * 3)) - 0.5) * (0.25 * falling.x));
  }
#if RIPPLES
  // ripples (ripples.h, World::waterRipples): the field's slopes tilt the surface where it lies (fading out
  // toward its edges) and where its cells are not much finer than a pixel (finer, they would flicker); its
  // height moves the waterline a little, and its steepest crests foam
  float rippleHeight = 0, rippleFoam = 0;
  float2 rippleTilt = 0;
  {
    float2 at = (xz - rippleArea.xy) * rippleArea.z;
    float2 inside = min(at, 1 - at);
    float shown = saturate(min(inside.x, inside.y) * 16) * saturate((rippleArea.w / max(grain, 1e-4) - 0.7) * 2);
    if (rippleLook.w > 0.5) {  // (a view for checking, Ripples::view: red up, blue down, green where it shows)
      float4 r = rippleSurface.SampleLevel(samplerLinearClamp, at, 0);
      return float4(saturate(r.x * rippleLook.w), saturate(shown) * 0.3, saturate(-r.x * rippleLook.w), 1);
    }
    [branch] if (shown > 0) {
      float4 r = rippleSurface.SampleLevel(samplerLinearClamp, at, 0);
      // (steep slopes tilt it less than they are, never more than about 0.4: tilted further, a fresh splash's
      // crater turned toward the horizon and mirrored the bright sky as a white ring)
      rippleTilt = -r.yz * (rippleLook.x * shown / (1 + length(r.yz) * rippleLook.x * 2.5));
      N = normalize(N + float3(rippleTilt.x, 0, rippleTilt.y));
      rippleHeight = r.x * shown;
      rippleFoam = smoothstep(rippleLook.y, rippleLook.y * 2.5, length(r.yz)) * shown;
    }
  }
#endif
#if SURF
  // [surf] (surf.h, World::surf): the waves running in to the shore, breaking, running up the beach; found where the line
  // of sight meets the water at its level (the sheet lies higher)
  float3 still = i.world + toPixel * ((surfaceColor.w - i.world.y) / min(toPixel.y, -1e-3));
  SurfHere surf = SurfAt(still.xz, grain, surfaceColor.w, mapSize.xy, surfArea, surfLook, surfLook2);
  // (its tilt is overdone for the view from above, as the swell's; seen at a low angle, toward the horizon, the waves
  // show by themselves, and overdone they lay as dark ruled bands along the shore)
  N = normalize(N + float3(surf.tilt.x, 0, surf.tilt.y) * lerp(0.4, 1.0, saturate(-toPixel.y * 2.5)));
  [branch] if (surfLook3.x > 0.5 && surfLook3.x < 2.5) return float4(SurfView(still.xz, surf, surfArea, surfLook3.x), 1);  // (Surf::view; [shore] 3: WetBeach)
#endif

  // the light on the water
  float shadow = SunShadow(i.world, float3(0, 1, 0)) * CloudShadow(i.world);
  float3 sunLight = ToLinear(sunColor) * (lightMultiplier * modern.x);
  float3 skyLight = skyZenith.w > 0.5 ? SkyAmbient(float3(0, 1, 0)) : sunLight * 0.3;
  float3 light = skyLight + sunLight * (saturate(sunDirection.y) * 0.6 * shadow);
#if RIPPLES
  // (the ripples' relief in the sun: their sides toward it a little brighter, those away from it darker; from the
  // game's height, looking down into the water, this is most of what shows of them)
  light *= 1 + clamp(dot(rippleTilt, normalize(sunDirection.xz + 1e-5)) * saturate(sunDirection.y * 4) * shadow, -0.45, 0.45);
#endif
  // lamps and flashes near the water: their light on its body and foam, and their glint on its waves
#if RAYS
  rayLightsAtMost = 2;  // (two rays for the small lights' shadows on the water; lighting.h, SmallLightsTraced)
#endif
  float3 lamps = SmallLights(i.world, N, -toPixel, float3(0.5, 0.5, 0.5), float3(0.04, 0.04, 0.04), 0.0016, 0.1, saturate(dot(N, -toPixel)) + 1e-4);

  // what is behind, straight on and bent by the waves; the bending is left out where it would
  // bring in something that stands in front of the water
  float3 bed = WorldAt(i.position.xy, depthBehind);
  // how high the water stands here just now, against the sheet: where the bed is higher, the
  // water has run back from it (the waterline moves up and down the shore with the swell)
  // (and unevenly, by the foam's own lace: a waterline is never a ruled line)
  float lace = Lace(xz + N.xz * 0.12, grain);
#if RIPPLES
  float level = clamp(swell.x * 1.2 + (lace - 0.5) * 0.05 + rippleHeight * rippleLook.z, -kRise, kRise) - kRise;
#else
  float level = clamp(swell.x * 1.2 + (lace - 0.5) * 0.05, -kRise, kRise) - kRise;
#endif
#if SURF
#if WINTER
  // [surf] (the waves near the shore and their run-up; [groundfix] none where winter's ice lies along the shore: it lies
  // still at the water's level, and the run-up slid the waterline in under its rim and out over the beach past it)
  level += surf.lift * (1 - WinterShare(xz));
#else
  level += surf.lift;  // [surf] (the waves near the shore and their run-up)
#endif
#endif
#if LIFT
  level *= 1 - i.rest.z;  // [lift] (where the surface is lifted it is where it lies)
#endif
  float deep = i.world.y + level - bed.y;
#if SURF_WET
  // [shore] (ground the water has run back from just now: the beach the surf wets, with World::waterDetail)
  [branch] if (deep <= 0) return WetBeach(i.position.xy, bed, grain, coverage);
#endif
  if (deep <= 0) {
    coverage = 0;
    discard;
  }
#if SURF
  // [surf] (with the sheet higher, a sample of the land just above the water passed the test at the start: only the
  // samples whose ground the water covers now are kept, so that the waterline's edge stays where it was)
  [unroll] for (int k = 0; k < SAMPLES; ++k)
    if ((coverage >> k) & 1u) {
      if (i.world.y + level - WorldAt(i.position.xy, SCENE_DEPTH(pixel, k)).y <= 0) coverage &= ~(1u << k);
    }
  if (coverage == 0) discard;
#endif
#if WINTER
  // winter water (winter_water.h): slush and thin ice along the shore, on which the waves are stilled
  WinterIce ice = WinterShore(xz, bed.y, water.r, grain, lace);
  N = normalize(lerp(N, float3(0, 1, 0), ice.calm));
#endif
  float2 uv = i.position.xy * viewport.zw;
  float2 bent = uv + N.xz * (0.035 * saturate(deep * 1.5) / (1 + distance * 0.02));
  float depthBent = SCENE_DEPTH(int2(bent * viewport.xy), 0);
  if (depthBent > i.position.z || any(bent < 0) || any(bent > 1)) {
    bent = uv;
    depthBent = depthBehind;
  }
  float3 bedBent = WorldAt(bent * viewport.xy, depthBent);
  float through = min(length(bedBent - i.world), 80);
  float deepBent = max(i.world.y + level - bedBent.y, 0);
  float3 under = behind.SampleLevel(samplerLinearClamp, bent, 0).rgb;

  // the bright net on a shallow bed in sunlight
  float2 q = bedBent.xz;
  float n1 = CloudNoise(float3(q * 1.3 + time * 0.30, time * 0.25));
  float n2 = CloudNoise(float3(q * 1.3 - time * 0.25 + 7, time * 0.20 + 3));
  float net = pow(saturate(1 - abs(n1 - n2) * 5), 4);
  float netStrength = 0.6;
  [branch] if (shore.x > 0.5) {  // (World::waterDetail: a sharper net, a finer one over it, stronger)
    float n3 = CloudNoise(float3(q * 3.1 + time * 0.45 + 13, time * 0.35 + 5));
    float n4 = CloudNoise(float3(q * 3.1 - time * 0.40 + 29, time * 0.30 + 9));
    net = pow(saturate(1 - abs(n1 - n2) * 7), 3) + 0.5 * pow(saturate(1 - abs(n3 - n4) * 7), 3);
    netStrength = shore.w;
  }
  under *= 1 + net * netStrength * saturate(deepBent * 3) * exp(-deepBent * 0.35) * shadow * saturate(sunDirection.y * 3);

  // what the water takes from it, and the water's own colour in its place. The numbers are the
  // depth colours Chris chose on the remastered Seton's (the visuals track's water ramp, put on
  // record in its reply, section 15): red is gone after about 3 of depth, green after 8, blue
  // after 10; the bed is hidden by 1 - exp(-depth / 7.5); the water itself goes from a turquoise
  // over the shallows to a deep navy
  float3 kept = exp(-through * float3(1 / 3.0, 1 / 8.0, 1 / 10.0));
  float hidden = 1 - exp(-through / 7.5);
  float3 own = lerp(float3(0.027, 0.672, 0.604), float3(0.001, 0.019, 0.058), saturate(1 - exp(-through / 5.0)));
#if WINTER
  WinterColours(through, under, kept, hidden, own);  // (winter water: the cold palette, by how far winter has come)
#endif
#if LIFT
  // [hulls] a riding ship's hull below its waterline is hidden by the water before it within a hand's breadth, going over to
  // the colour the sea round it shows (the bed's rule above hides a hull just under the surface hardly at all: a pale, clear
  // strip of water along its bottom edge read as a ship floating on the water). Only by a riding hull's waterline (the
  // hulls' list, OceanHullAt): a submarine under the surface, or a rock, keeps the bed's rule, seen through the water as
  // in the original
  OceanHullNear hull = OceanHullAt(xz);
  {
    float ground = OceanGround(bedBent.xz, mapSize.xy);
    float standing = saturate((bedBent.y - ground - 0.5) / 0.4) * saturate((surfaceColor.w - ground - 0.6) / 1.5) *
                     saturate(1 - OceanHullAt(bedBent.xz).away / 0.3);  // (what lies behind the water is a riding hull's)
    float murk = standing * (1 - exp(-through / 0.12));
    hidden = lerp(hidden, 1, murk);
    own = lerp(own, lerp(float3(0.027, 0.672, 0.604), float3(0.001, 0.019, 0.058), 0.97), standing);
  }
#endif
  float3 color = lerp(under * kept, own * (light + lamps) * 0.6, hidden);

  // foam: at the shore and round what stands in the water. Thick at the waterline and thinning to
  // lace over the first half unit of depth; fronts of it run in toward the land, broken by the
  // same lace; and the waves' crests
  float edge = saturate(1 - deep / 0.5);
#if LIFT
  // [hulls] (none of this even band, nor its fronts, along a hull in the open sea: its own wash, below, rides with the waves;
  // a still pad of white the hull's width read as the hull floating on it)
  float openHull;
  {
    float ground = OceanGround(bed.xz, mapSize.xy);
    openHull = saturate((bed.y - ground - 0.5) / 0.4) * saturate((surfaceColor.w - ground - 0.6) / 1.5);
    edge *= 1 - openHull;
  }
#else
  // [floatfoam] (nor where what lies behind the water stands well off the map's ground in water deep enough to float in:
  // the floats under an amphibious unit afloat, a hand's breadth under the surface, read as a shore all over and showed as
  // flat white blocks. Only under what floats, the units in the water (Water::AddAfloat): a rock keeps its band, and so
  // does a shore of the map's own, the ground there being as high as what shows; a unit wading ashore keeps it too, the
  // water being shallow there)
  float openHull = 0;
  [branch] if (edge > 0 && afloatCount.x > 0 && groundInfo.x > 0) {
    openHull = UnderAfloat(bed.xz);
    [branch] if (openHull > 0) {
      float ground = GroundTop(bed.xz);
      openHull *= saturate((bed.y - ground - 0.5) / 0.4) * saturate((surfaceColor.w - ground - 0.6) / 1.5);
      // (a hull going up through the surface keeps the thin line of foam along its waterline: its last hand's breadth)
      [branch] if (openHull > 0) openHull *= 1 - ThroughSurface(bed, surfaceColor.w) * saturate(1.5 - deep / 0.1);
    }
    edge *= 1 - openHull;
  }
#endif
  // (how much of the lace is foam: nearly all of it only in the last hand's breadth of water)
  float much = edge * edge * edge * 0.6 + saturate(1 - deep / 0.05) * 0.3;
  much *= 1 - openHull;  // [hulls] ([floatfoam] and what floats on the still water)
#if SURF
  // [coast] at the map's own shores the surf's waves draw the white water (surf.h, SurfAt): of this even band along the
  // waterline only what the last wave left stays (surf.lap: little where the water is calm or sheltered); round what
  // stands in the water (higher than the map's ground) it stays as it was
  float shoreLap = lerp(surf.lap, 1, smoothstep(0.3, 0.8, bed.y - surf.ground));
  much *= shoreLap;
#endif
  // (open water, away from every shore: only wisps of it are left, soft at their edges, on the
  // swell's crests and near the eye; from far up the sea is one blue - Chris, 2026-10-13: "too
  // sparkly ... the white should blend in more")
  float open = saturate(1 - much * 8);
#if OCEAN
  float wisps = 0;  // [ocean] (its own whitecaps and their streaks instead: below)
#else
  float wisps = 0.3 * saturate(swell.x * 9 + 0.3) * saturate(1.2 - distance / 150);
#endif
  float foam = smoothstep(1.05 - much * 1.1 - 0.04 * open, 1.30 - much * 1.1 + 0.14 * open, lace + 0.42) * (0.55 + 0.45 * edge) * lerp(1, wisps, open);
  float fronts = pow(saturate(sin(deep * 20 - time * 1.5 + lace * 3.0) * 0.5 + 0.5), 5) * edge;
#if SURF
  fronts *= shoreLap;  // [coast] (its fronts as well: the surf's waves run in there)
#endif
  foam = saturate(foam + fronts * smoothstep(0.42, 0.66, lace) * 0.55 + crest * saturate(swell.x * 14 + 0.25));
#if RIPPLES
  foam = saturate(foam + rippleFoam * smoothstep(0.35, 0.7, lace) * 0.35);
#endif
#if OCEAN
  // [ocean] the whitecaps where the waves fold, and the foam they leave drifting downwind ([whitecaps] ocean.h,
  // OceanWhitecaps: the crest's hard edge, the fresh foam, the residue torn into streaks and lace; laid over below)
  float2 caps = OceanWhitecaps(ocean, xz, ddx(xz), ddy(xz), lace);
#endif
#if SURF
  // [surf] its white water, broken by the lace; thinned where something stands in it higher than the map's ground (a
  // unit wading ashore keeps its outline)
  foam = saturate(foam + SurfFoam(surf, lace) * (1 - smoothstep(0.6, 1.2, bed.y - surf.ground)));
#endif
#if WINTER
  // (winter water: the foam greyer, none on the ice and slush, which lie over the water)
  foam *= 1 - ice.cover;
  color = lerp(color, (light + lamps) * (0.85 * (1 - 0.3 * winterWater.x)), foam * 0.94);
  color = lerp(color, ice.albedo * (light + lamps), ice.cover);
#else
  color = lerp(color, (light + lamps) * 0.85, foam * 0.94);
#endif
#if OCEAN
  // [whitecaps] (white where fresh, greyer as it ages; nothing mirrored and no glint where it lies). Lit as the matt white it
  // is, by the whole of the sun on the wave's face under it (the water's own light takes the sun at 0.6: its body
  // scatters less of it back), so that a crest's side toward the sun stands out bright and its lee side greyer.
  {
    float3 face = normalize(float3(-ocean.slope.x, 1, -ocean.slope.y));
    float3 capsLight = skyLight + sunLight * (saturate(dot(face, sunDirection)) * shadow) + lamps;
    color = lerp(color, capsLight * caps.y, caps.x);
  }
  foam = 1 - (1 - foam) * (1 - caps.x);
#endif
#if LIFT
  // [hulls] where the waves meet a hull (Ocean::AddHull): a broken white wash along its waterline, wider and whiter where the
  // water stands higher against the hull just now (a short wave running along it, the bob's long waves being the hull's own
  // rise), the rougher the sea, and at its bow the faster it goes; torn by the lace, lit as the whitecaps
  [branch] if (hull.away < 1.5) {
    float rough = oceanWind.z;
    float excess = saturate((i.world.y - hull.waterline) / max(oceanLook2.x * 0.2, 0.02));
    float run = saturate(hull.speed / 4);
    float bow = saturate(hull.along * 2.5 - 1.5);
    float width = (0.025 + 0.06 * rough + 0.04 * run + 0.10 * bow * run) * (1 + 1.2 * excess);
    float d = max(hull.away, 0) / width;
    float body = 1 - smoothstep(0.2, 1, d);
    // (torn finer than the lace: a noise drifting along with the water, its grain about a tenth of a unit, left at its mean
    // where a pixel takes in more)
    float fine = CloudNoise(float3(xz * 9 + oceanWind.xy * time * 0.6, time * 0.8));
    fine = lerp(0.5, fine, saturate(1.5 - grain * 18));
    float torn = smoothstep(0.5, 0.72, lace * 0.5 + fine * 0.5 + 0.18 * (1 - d) - 0.08 + 0.15 * excess);
    // (most where the waves run against it, its side toward the wind's sea, and at its bow under way; least in its lee and
    // at its stern, where its wake takes over)
    float windward = saturate(-dot(hull.outward, oceanWind.xy) * 1.5 + 0.2);
    float stern = saturate(-hull.along * 2.5 - 1.5);
    float wash = body * torn * saturate(0.2 + 0.6 * rough * windward + 0.8 * bow * run + 0.5 * excess) * (1 - 0.7 * stern);
    float3 face = normalize(float3(-ocean.slope.x, 1, -ocean.slope.y));
    float3 washLight = skyLight + sunLight * (saturate(dot(face, sunDirection)) * shadow) + lamps;
    color = lerp(color, washLight * 0.9, wash * 0.95);
    foam = max(foam, wash);
  }
#endif
  color += lamps * 0.35;
  color += light * (rings * 0.04);

#if OCEAN
  // [ocean] the glow through the crests: looking toward a low sun, the light that comes through the thin tops of the waves
  // (the higher above the sea's mean a crest stands, the more), tinted by the water it passes through
  {
    float2 sunWay = normalize(sunDirection.xz + 1e-5), lookWay = normalize(toPixel.xz + 1e-5);
    float toward = saturate(dot(lookWay, sunWay));
    float up = saturate(ocean.height / max(oceanLook2.x * 0.6, 1e-3));
    // (seen from the side, through the crest's face turned toward the eye; from above none shows)
    float face = saturate(dot(N.xz, -lookWay) * 6);
    float glow = oceanLook.y * toward * toward * toward * up * up * face * saturate(1 - -toPixel.y * 2) * saturate(1.2 - sunDirection.y * 1.5) *
                 saturate(sunDirection.y * 6) * shadow;
    color += float3(0.027, 0.672, 0.604) * sunLight * (glow * 0.35 * (1 - foam));
  }
#endif
  // the mirror: the sky, and what the picture shows that way
  float3 R = reflect(toPixel, N);
  R.y = max(R.y, 0.02);
  R = normalize(R);
  float3 mirror = skyZenith.w > 0.5 ? ComputedSky(R) : ToLinear(sky.Sample(samplerLinear, R).rgb) * (lightMultiplier * modern.x * 0.5);
#if RAYS
  [branch] if (rayEffects.x > 0.5) {
    // by ray tracing (ray_scene.h): what the mirrored ray meets (not the water itself: mask 1), lit as the rays light
    // it (the sun by a ray toward it; round about by the probes, bounce.h, or the sky's fitted light) and seen through
    // the air; the picture's march below still has the last word where the picture shows it
    // (from calmer surfaces than the waves': one ray a pixel off every small wave would break what it mirrors into
    // flickering streaks; a still lake's mirror image, rippled, is what the eye takes it for. Two rays, the waves'
    // tilt kept at 45% and at 15%, averaged: where one meets the shore and the other the sky, a half step, as the
    // many facets of real ripples give, not a hard dash.)
    uint seed = RayHash(asuint(i.world.x * 37.0) ^ RayHash(asuint(i.world.z * 53.0)));
    float3 mirrored = 0;
    for (int ray = 0; ray < 2; ++ray) {
      float3 calm = reflect(toPixel, normalize(lerp(N, float3(0, 1, 0), ray == 0 ? 0.55 : 0.85)));
      calm.y = max(calm.y, 0.02);
      calm = normalize(calm);
      RayHit met;
      float3 c = mirror;
      if (RayTrace(i.world + float3(0, 0.05, 0), calm, 0.05, rayEffects.y, 0x01, seed + ray, met)) {
        float3 hit = i.world + calm * met.t, hitAlbedo, n;
        RaySurfaceAt(met, hit, calm, rayEffects.zw, hitAlbedo, n);
        c = ThroughAirTo(RayLitAt(hit, n, hitAlbedo, seed + ray, 8), hit);  // (8 of the small lights at most: all would be dear)
      }
      mirrored += c * 0.5;
    }
    mirror = mirrored;
  }
#endif
  {
    float3 p = i.world;
    float stride = 1.2;
    int first = 0;
#if OCEAN
    // [reflblot] (the ocean, its lifted waves above all: close to the eye a ship's hull is narrower and lower than the first
    // step of 1.2, and a ray off the water just before it stepped over it, so that under the hull's waterline the sea
    // mirrored the sky, and the hull showed only where a step happened to end inside it: in patches lying detached in
    // the sea near the eye, well below the ship. Close to the eye the march starts with steps as short as the water's
    // distance asks, a sixtieth of it (never under 0.03), and lengthens them by the same 1.35 until they reach 1.2, from
    // where it goes on as it always did: from the game's height, where the water lies 60 and more away, its steps are as
    // they were)
    float startStride = clamp(distance / 60, 0.03, 1.2);
    first = -int(ceil(log(1.2 / startStride) / log(1.35) - 1e-3));
    stride = startStride;
    // (and it goes off the calmer surface the rays' mirror goes off, above, the waves' tilt kept at 45%: off the full tilt,
    // drawn steeper than the waves are for their shading, the faces of the waves near the eye turned away from it mirrored
    // the ship's hull in sharp-edged patches far below it, lying amid the sky's mirror image of the faces round them)
    float3 marchR = reflect(toPixel, normalize(lerp(N, float3(0, 1, 0), 0.55)));
    marchR.y = max(marchR.y, 0.02);
    marchR = normalize(marchR);
#else
    float3 marchR = R;
#endif
    for (int k = first; k < 14; ++k) {
#if OCEAN
      if (k == 0) stride = 1.2;
      float stepped = stride;
#endif
      p += marchR * stride;
      stride *= 1.35;
      float4 c = mul(viewProj, float4(p, 1));
      if (c.w <= 0) break;
      float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
      if (any(at < 0) || any(at > 1)) break;
      float there = SCENE_DEPTH(int2(at * viewport.xy), 0);
      float here = c.z / c.w;
      if (there > here) {  // the picture has something nearer than this place on the way: it is what is mirrored
        float gap = cameraBack.w / here - cameraBack.w / there;
        float slack = stride * 2 + 1;
#if OCEAN
        // [reflblot] (and where along the last step the ray went behind what the picture shows, found by halving the step
        // six times; mirrored only where the ray truly meets it there, not where it passes behind it: its steps grow long
        // fast, and a step's end well past a hull, where the hull's side high up showed through, was taken for where the
        // ray met it)
        {
          float3 inFront = p - marchR * stepped, atBack = p;
          for (int j = 0; j < 6; ++j) {
            float3 m = (inFront + atBack) * 0.5;
            float4 cm = mul(viewProj, float4(m, 1));
            float2 atm = float2(cm.x / cm.w * 0.5 + 0.5, 0.5 - cm.y / cm.w * 0.5);
            if (SCENE_DEPTH(int2(min(saturate(atm) * viewport.xy, viewport.xy - 1)), 0) > cm.z / cm.w) atBack = m; else inFront = m;
          }
          c = mul(viewProj, float4(atBack, 1));
          at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
          there = SCENE_DEPTH(int2(min(saturate(at) * viewport.xy, viewport.xy - 1)), 0);
          here = c.z / c.w;
          gap = cameraBack.w / here - cameraBack.w / there;
          slack = length(atBack - inFront) * 2 + clamp(distance / 200, 0.02, 0.3);  // (what the ray passes behind is not met)
        }
#endif
        if (gap < slack) {
          float2 edge = min(at, 1 - at);
          float seen = saturate(min(edge.x, edge.y) * 10) * (1 - max(k, 0) / 14.0);  // ([reflblot] its short steps first)
          mirror = lerp(mirror, behind.SampleLevel(samplerLinearClamp, at, 0).rgb, seen);
        }
        break;
      }
    }
  }
  float facing = saturate(dot(-toPixel, N));
  float fresnel = 0.02 + 0.98 * pow(1 - facing, 5);
#if WINTER
  // (winter water: snow on the ice and slush mirror nothing; bare ice as the water)
  foam = 1 - (1 - foam) * (1 - ice.cover * ice.matte);
#endif
  color = lerp(color, mirror, fresnel * (1 - foam));
  // [sunlight] (drifting cloud shadows, sunlight.h) the sea darker in a cloud's shadow, its mirror of the sky too: from
  // above, a cloud's shadow on the water reads as one darker patch, as on the land
  [branch] if (cloudDrift.x > 0.5) color *= 1 - cloudDrift.w * saturate((1 - CloudShadow(i.world)) / max(cloudDrift.y, 1e-3));
  // the sun's (or the moon's) glint ([shore] with World::waterDetail: widened and dimmed by the waves a pixel takes in,
  // water_glint.h: from far up a calm sheen where the sun's mirror image lies, not single pixels twinkling)
#if OCEAN
  // [ocean] the sun's glitter off the waves' facets, by how their slopes spread over what the pixel takes in (ocean.h,
  // OceanGlint: sparkles close up, a soft broad path of light far off), about the surface's own lean
  color += sunLight * (OceanGlint(-toPixel, sunDirection, -N.xz / max(N.y, 0.05), ocean.spread) * oceanLook3.y * shadow * (1 - foam));
#else
  [branch] if (shore.x > 0.5 && grain > kGlintFrom) {  // (closer in exactly as before)
    // (the swell's long trains, which a pixel far off still shows, counted among the slopes it takes in there: their even
    // tilt, mirrored by a widened lobe, lay as ruled stripes of light across the far sea)
    float far = smoothstep(kGlintFrom, kGlintTo, grain);
    float3 swellTilt = float3(-swell.y, 0, -swell.z) * lerp(1.0, 2.5, saturate(-toPixel.y * 2.5));
    float3 calm = reflect(toPixel, normalize(N - swellTilt * far));
    calm.y = max(calm.y, 0.02);
    color += sunLight * (SoftGlint(dot(normalize(calm), sunDirection), 500, grain, kGlintFrom, kGlintTo, kGlintSlopes) * 0.9 * shadow * (1 - foam));
  } else {
    color += sunLight * (pow(saturate(dot(R, sunDirection)), 500) * 0.9 * shadow * (1 - foam));
  }
#endif
  // at its very edge the water thins out to nothing over what is behind it
  color = lerp(behind.SampleLevel(samplerLinearClamp, uv, 0).rgb, ThroughAirTo(color, i.world), saturate(deep / 0.035));
  return float4(BeyondMap(color, i.world), 1);
}
)hlsl";

struct WaterConstants {
  float mapSize[4];
  float surfaceColor[4];
  float lerpAndFresnel[4];
  float reflection[4];
  float sunDirection[4];
  float sunColor[4];
  float normalRepeat[4];
  float movement[4][4];
  float cameraRight[4], cameraUp[4], cameraBack[4];  // (the modern water's)
  float groundInfo[4];                                // [floatfoam] (the modern water's: x the ground's height unit, 0 none)
  float rippleArea[4], rippleLook[4];                // (the modern water's with ripples: given only to it)
  float surfArea[4], surfLook[4], surfLook2[4], surfLook3[4];  // [surf] (the modern water's with the surf: given only to it)
};

// How the ripples' field shows on the water (rippleLook): its slopes' tilt of the surface (as they are), the slope
// at which its crests begin to foam, and how much of its height the waterline takes.
constexpr float kRippleTilt = 2.0f, kRippleFoamSlope = 0.8f, kRippleWaterline = 1.0f;

}  // namespace

gfx::Pipeline Water::ModernPipeline(bool ripples, std::string* error, bool winter, bool surf, bool wet, bool ocean, bool lift) {
  // (the modern look's water reads the scene's depth: several samples a pixel, or one. Without ripples, winter and the
  // surf its source is the same as it ever was, but for [shore] the glint's block, water_glint.h, read only with
  // World::waterDetail.)
  std::string modern = std::string(ripples ? "#define RIPPLES 1\n" : "") + (winter ? "#define WINTER 1\n" : "") + (surf ? "#define SURF 1\n" : "") +
                       (surf && wet ? "#define SURF_WET 1\n" : "") +  // [shore]
                       (ocean ? "#define OCEAN 1\n" : "") +           // [ocean]
                       (ocean && lift ? "#define LIFT 1\n" : "") +    // [lift]
                       "#define AIR_SMOOTH 1\n" +  // [weather] (view.h, AirAt)
                       "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" + kViewShaderBlock + RayShaders() + kLightingShaderBlock +
                       kGlintShaderBlock + (winter ? kWinterWaterShaderBlock : "") + (surf ? kSurfShaderBlock : "") + (ocean ? kOceanShaderBlock : "") +
                       kModernShader;
  gfx::PipelineDesc pd;
  pd.name = winter ? (ripples ? "water, modern, ripples, winter" : "water, modern, winter") : ripples ? "water, modern, ripples" : "water, modern";
  if (surf) pd.name = winter ? (ripples ? "water, modern, ripples, winter, surf" : "water, modern, winter, surf") : ripples ? "water, modern, ripples, surf" : "water, modern, surf";
  if (surf && wet) pd.name = winter ? (ripples ? "water, modern, ripples, winter, surf, wet beach" : "water, modern, winter, surf, wet beach")
                             : ripples ? "water, modern, ripples, surf, wet beach" : "water, modern, surf, wet beach";  // [shore]
  if (ocean) pd.name = lift ? "water, modern, ocean, lifted" : "water, modern, ocean";  // [ocean] (OceanPipeline says which variant)
  pd.vertexSource = pd.pixelSource = modern;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
  pd.stride = 8;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Opaque;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pd.depthTest = false;
  pd.depthTarget = false;
  return device_->CreatePipeline(pd, error);
}

void Water::PrepareRipples() {
  if (!map_ || !map_->water.present || !modern_) return;
  ripples.Ready();
  if (ripplesTried_) return;
  ripplesTried_ = true;
  std::string why;
  modernRipples_ = ModernPipeline(true, &why);
  if (!modernRipples_) fprintf(stderr, "render: the water cannot show ripples (%s)\n", why.c_str());
}

void Water::PrepareSurf(bool ripples, bool winter, bool wet) {
  if (!map_ || !map_->water.present || !modern_) return;
  for (int k = 0; k < 8; ++k) {
    // (the variants the options on now may want: with the ripples' field or without it, in winter or not)
    // ([shore] and, 4, the wet beach's: World::waterDetail on)
    if (((k & 1) != 0 && !ripples) || ((k & 2) != 0 && !winter) || ((k & 4) != 0 && !wet) || surfTried_[k]) continue;
    surfTried_[k] = true;
    std::string why;
    modernSurf_[k] = ModernPipeline((k & 1) != 0, &why, (k & 2) != 0, true, (k & 4) != 0);
    if (!modernSurf_[k]) fprintf(stderr, "render: the water cannot show the surf (%s)\n", why.c_str());
  }
}

// [ocean] The ocean's variant of the modern water (key: 1 the ripples' field, 2 winter, 4 the surf, 8 the wet beach, [lift]
// 16 lifted by the waves), made the first time it is wanted.
gfx::Pipeline Water::OceanPipeline(int key) {
  if (!oceanTried_[key]) {
    oceanTried_[key] = true;
    std::string why;
    oceanPipes_[key] = ModernPipeline((key & 1) != 0, &why, (key & 2) != 0, (key & 4) != 0, (key & 8) != 0, true, (key & 16) != 0);
    if (!oceanPipes_[key]) fprintf(stderr, "render: the water cannot show the ocean (variant %d: %s)\n", key, why.c_str());
  }
  return oceanPipes_[key];
}

void Water::PrepareOcean(bool ripples, bool winter, bool surf, bool wet, bool lift) {
  if (!map_ || !map_->water.present || !modern_) return;
  // (the variants the options on now may want: with the ripples' field or without it, in winter or not, with the surf;
  // [lift] lifted, or flat while the lift is off)
  for (int k = 0; k < 32; ++k) {
    if (((k & 1) != 0 && !ripples) || ((k & 2) != 0 && !winter) || ((k & 4) != 0 && !surf) || ((k & 8) != 0 && !(surf && wet)) || ((k & 16) != 0) != lift) continue;
    OceanPipeline(k);
  }
}

void Water::PrepareWinter(bool ripples) {
  if (!map_ || !map_->water.present || !modern_) return;
  const int k = ripples ? 1 : 0;
  if (winterTried_[k]) return;
  winterTried_[k] = true;
  std::string why;
  modernWinter_[k] = ModernPipeline(ripples, &why, true);
  if (!modernWinter_[k]) fprintf(stderr, "render: the water cannot show winter (%s)\n", why.c_str());
}

bool Water::Load(const MapData* map, std::string* error) {
  map_ = map;
  ripples.SetMap(map);
  surf.SetMap(map);
  ocean.SetMap(map);  // [ocean]
  if (!map->water.present) return true;
  if (!pipeline_) {
    std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "water";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.cull = gfx::Cull::None;
    pd.depthWrite = false;
    pd.blend = gfx::Blend::Premultiplied;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    const float corners[12] = {0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
    gfx::BufferDesc bd;
    bd.stride = 8;
    bd.size = sizeof corners;
    bd.data = corners;
    vertices_ = device_->CreateBuffer(bd);
    // the modern look's water
    modern_ = ModernPipeline(false, error);
    if (!modern_) return false;
  }
  waterMap_ = map->waterMap.empty() ? gfx::Texture{} : textures_->FromDds(map->waterMap, "the map's water map");
  if (!waterMap_) waterMap_ = textures_->White();
  for (int i = 0; i < 4; ++i) waves_[i] = textures_->Get(map->water.waves[i].texture, textures_->FlatNormal());
  sky_ = textures_->Get(map->water.cubemap, textures_->GreyCube());
  return true;
}

void Water::Draw(const View& view) { DrawWith(view, nullptr, {}, {}); }

void Water::AddAfloat(Vec3 position, const Quat& orientation, Vec3 lo, Vec3 hi) {
  if (!map_ || !map_->water.present) return;
  const float level = map_->water.elevation;
  // (in the water: reaching down to it, and not deep under it; and over water deep enough to float in)
  if (position.y + lo.y > level + 0.3f || position.y + hi.y < level - 1.0f) return;
  const Vec3 middle = position + Rotate(orientation, (lo + hi) * 0.5f);
  if (map_->Height(middle.x, middle.z) > level - 0.6f) return;
  Vec3 forward = Rotate(orientation, {0, 0, 1});
  forward.y = 0;
  forward = Length(forward) > 1e-3f ? Normalize(forward) : Vec3{0, 0, 1};
  // (its box seen from above, a little wider: what hangs under it, its floats, lies within its model's bounds)
  constexpr float kMargin = 0.2f;
  afloat_.push_back({middle.x, middle.z, forward.x, forward.z, (hi.z - lo.z) * 0.5f + kMargin, (hi.x - lo.x) * 0.5f + kMargin, 0});
}

void Water::DrawModern(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth) { DrawWith(view, &camera, behind, sceneDepth); }

void Water::DrawWith(const View& view, const Camera* camera, gfx::Texture behind, gfx::Texture sceneDepth) {
  if (!map_ || !map_->water.present || !pipeline_) return;
  const MapWater& w = map_->water;
  WaterConstants c = {};
  c.mapSize[0] = static_cast<float>(map_->width);
  c.mapSize[1] = static_cast<float>(map_->height);
  c.mapSize[2] = 1.0f / static_cast<float>(map_->width);
  c.mapSize[3] = 1.0f / static_cast<float>(map_->height);
  c.surfaceColor[0] = w.surfaceColor.x;
  c.surfaceColor[1] = w.surfaceColor.y;
  c.surfaceColor[2] = w.surfaceColor.z;
  c.surfaceColor[3] = w.elevation;
  c.lerpAndFresnel[0] = w.colorLerp.x;
  c.lerpAndFresnel[1] = w.colorLerp.y;
  c.lerpAndFresnel[2] = w.fresnelBias;
  c.lerpAndFresnel[3] = w.fresnelPower;
  c.reflection[0] = w.unitReflection;
  c.reflection[1] = w.skyReflection;
  c.reflection[2] = w.sunShininess;
  c.reflection[3] = w.sunGlow;
  Vec3 sun = Normalize(w.sunDirection);
  c.sunDirection[0] = sun.x;
  c.sunDirection[1] = sun.y;
  c.sunDirection[2] = sun.z;
  c.sunColor[0] = w.sunColor.x;
  c.sunColor[1] = w.sunColor.y;
  c.sunColor[2] = w.sunColor.z;
  for (int i = 0; i < 4; ++i) {
    c.normalRepeat[i] = w.normalRepeat[i];
    c.movement[i][0] = w.waves[i].movement.x;
    c.movement[i][1] = w.waves[i].movement.y;
  }
  const bool modern = camera && modern_ && behind && sceneDepth;
  if (modern) {
    Vec3 right, up, back;
    camera->Axes(&right, &up, &back);
    const float axes[12] = {right.x, right.y, right.z, std::tan(camera->fovY * 0.5f),
                            up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                            back.x, back.y, back.z, camera->zNear};
    std::memcpy(c.cameraRight, axes, sizeof axes);
    if (ground_) c.groundInfo[0] = groundUnit_;  // [floatfoam]
  }
  // with ripples on the water this frame (ripples.h): the modern water that reads their field
  if (modern && ripples.Active()) PrepareRipples();
  const bool rippling = modern && ripples.Active() && modernRipples_;
  // with snow lying (winter_water.h): the water's winter pipeline, with the ripples' field or without
  const bool wintry = modern && view.constants.winterWater[0] > 0;
  if (wintry) PrepareWinter(rippling);
  const gfx::Pipeline winter = wintry ? modernWinter_[rippling ? 1 : 0] : gfx::Pipeline{};
  if (rippling) {
    std::memcpy(c.rippleArea, ripples.Area(), sizeof c.rippleArea);
    const float look[4] = {kRippleTilt, kRippleFoamSlope, kRippleWaterline, ripples.view};
    std::memcpy(c.rippleLook, look, sizeof look);
  }
  // [surf] with the surf on (surf.h): the water's surf pipeline of the same kind, its constants and its field
  // ([shore] the wet beach's with World::waterDetail on: the view's shore.x)
  const bool wet = view.constants.shore[0] > 0.5f;
  if (modern && surf.Active()) PrepareSurf(rippling, wintry, wet);
  const gfx::Pipeline surfing = modern && surf.Active() ? modernSurf_[(rippling ? 1 : 0) | (wintry ? 2 : 0) | (wet ? 4 : 0)] : gfx::Pipeline{};
  if (surfing) surf.Constants(c.surfArea, c.surfLook, c.surfLook2, c.surfLook3);
  // [ocean] with the ocean on (ocean.h): its variant of the same kind, its constants and its field
  const bool lifted = modern && ocean.Lifted();  // [lift]
  const gfx::Pipeline oceanic = modern && ocean.Active() ? OceanPipeline((rippling ? 1 : 0) | (wintry ? 2 : 0) | (surfing ? 4 : 0) | (surfing && wet ? 8 : 0) |
                                                                         (lifted ? 16 : 0))
                                                         : gfx::Pipeline{};
  device_->SetPipeline(oceanic ? oceanic : surfing ? surfing : winter ? winter : rippling ? modernRipples_ : modern ? modern_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, static_cast<uint32_t>(surfing ? sizeof c : rippling ? offsetof(WaterConstants, surfArea) : offsetof(WaterConstants, rippleArea)));
  if (oceanic) {
    device_->SetConstants(2, ocean.ShaderConstants(), ocean.ShaderConstantsSize());
    ocean.Bind();
  }
  if (modern) {
    // [floatfoam] what floats, the nearest to the eye first (the lifted water's grid sets its own constants there)
    float afloat[4 + 8 * kAfloatMost] = {};
    const Vec3 eye = camera->Eye();
    for (Afloat& a : afloat_) a.away = (a.x - eye.x) * (a.x - eye.x) + (a.z - eye.z) * (a.z - eye.z);
    const size_t count = std::min(afloat_.size(), kAfloatMost);
    std::partial_sort(afloat_.begin(), afloat_.begin() + static_cast<std::ptrdiff_t>(count), afloat_.end(),
                      [](const Afloat& a, const Afloat& b) { return a.away < b.away; });
    afloat[0] = static_cast<float>(count);
    for (size_t k = 0; k < count; ++k) {
      const Afloat& a = afloat_[k];
      const float row[8] = {a.x, a.z, a.forwardX, a.forwardZ, a.halfLength, a.halfBeam, 0, 0};
      std::memcpy(afloat + 4 + 8 * k, row, sizeof row);
    }
    device_->SetConstants(kWaterAfloatSlot, afloat, static_cast<uint32_t>(sizeof(float) * (4 + 8 * count)));
  }
  if (modern) {
    device_->SetTexture(6, behind);
    device_->SetTexture(7, sceneDepth);
    if (ground_) device_->SetTexture(kWaterGroundSlot, ground_);  // [floatfoam]
  }
  if (surfing) device_->SetTexture(kSurfFieldSlot, surf.Field());
  if (rippling) device_->SetTexture(8, ripples.Surface());
  device_->SetTexture(0, waterMap_);
  for (int i = 0; i < 4; ++i) device_->SetTexture(1 + i, waves_[i]);
  device_->SetTexture(5, sky_);
  if (oceanic && lifted) {
    ocean.DrawGrid();  // [lift] (the camera's grid, lifted by the waves)
  } else {
    device_->SetVertexBuffer(vertices_);
    device_->Draw(6);
  }
  if (rippling) device_->SetTexture(8, {});  // (the field is written again next frame: not left bound)
  if (surfing) device_->SetTexture(kSurfFieldSlot, {});
  if (oceanic) ocean.Unbind();  // [ocean] (its field too)
  if (modern && ground_) device_->SetTexture(kWaterGroundSlot, {});  // [floatfoam]
}

}  // namespace render
