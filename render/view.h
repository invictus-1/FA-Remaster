// What every drawing pass is told about the current picture: the camera's matrices and position, the
// size of the picture, the time, the light and the shadow maps. Also the test "can this box be seen".
#pragma once
#include "camera.h"
#include "gfx.h"
#include "linalg.h"

namespace render {

// The scene is drawn into a target of this format (colours beyond 0..1 survive until the last pass).
inline constexpr gfx::Format kSceneFormat = gfx::Format::RGBA16F;
// Samples per pixel of the scene's picture (anti-aliasing: 1 off, 2, 4, 8). Set before any renderer
// builds its pipelines; every pipeline that draws into the scene is built for it.
inline int gSceneSamples = 4;
// The lit surfaces' shaders (the ground's, the models') built with ray tracing in them (RAYS, ray_scene.h's block),
// for reflections (World::rayReflections). As gSceneSamples, set before the world is made; only where the device
// has ray tracing.
inline bool gRayShaders = false;
// Texture registers every scene shader may read; the frame code binds them, the renderers leave them alone.
inline constexpr int kLampShadowSlot = 27;   // the lamps' shadow maps (lights.h), one array layer per lamp
inline constexpr int kShadowMapSlot = 28;    // the sun's shadow maps, one array layer per cascade
inline constexpr int kEnvironmentSlot = 29;  // the sky as a cube map with mip levels (blurrier going down)
inline constexpr int kAirSlot = 30;          // light in the air (air.h), a volume over the view, when it is on
inline constexpr int kCloudShadowSlot = 31;  // the volumetric clouds' shadows (clouds.h), when they are on
inline constexpr int kSkyTableSlot = 32;     // the sky from tables (sky.h, Sky::SkyView): its view from the eye, when on
inline constexpr int kBounceLightSlot = 34;   // bounce light (bounce.h): the probes' light from every side
inline constexpr int kBounceSightSlot = 35;   // how far each probe sees every way
inline constexpr int kBounceColumnsSlot = 36; // the ground's height under each column of probes
inline constexpr int kRaySceneSlot = 33;      // ray tracing (ray_scene.h): the scene's structure
inline constexpr int kRayInstancesSlot = 37;  // what each of its instances is
inline constexpr int kRayGroundSlot = 38;     // the ground's normals (39: its colours)
inline constexpr int kMaxCascades = 4;

// The two ways of lighting. Everything is drawn in either; the game's files are the same.
enum class Look { Original = 0, Modern = 1 };

// Laid out as the shaders' `View` constant buffer (register b0).
struct ViewConstants {
  Mat4 viewProj;
  float cameraPosition[3];
  float time;                // seconds, for animated surfaces
  float viewport[4];         // width, height, 1 / width, 1 / height
  float sunDirection[3];     // toward the sun
  float look;                // 0: the original's lighting, 1: the modern one
  float sunColor[3];         // the map's sun colour
  float lightMultiplier;     // the map's lighting multiplier
  float sunAmbience[3];
  float shadowsOn;           // 1 when the shadow maps are filled
  float shadowFill[3];
  float cascadeCount;
  float modern[4];           // x: sun strength, y: sky strength, z: exposure, w: share of the mirrored sky kept
  Mat4 shadowMatrix[kMaxCascades];     // world -> shadow map (x, y: where in the map; z: depth)
  float cascadeTexel[kMaxCascades];    // world size of one shadow-map texel, per cascade
  // The small lights' grid over the ground (lights.h): where its corner is (x, z), 1 / the size
  // of a square, and 1 when there are lights at all; then squares across, squares down, and the
  // row of the lights' buffer where the lights themselves begin.
  float lightGrid[4];
  float lightGridSize[4];
  // The computed sky (sky.h). Zenith and horizon: the sky's light overhead and at the horizon,
  // linear; zenith's w is 1 when the sky is computed at all (0: the map's sky pictures and sun are
  // what lights), horizon's w how much it is night. Glow: the light around the sun or moon; its w
  // is how bright the day is (for what has no lighting of its own). Body: toward the sun, w 1 when
  // the light that falls is the moon's.
  float skyZenith[4];
  float skyHorizon[4];
  float skyGlow[4];
  float skyBody[4];
  float skyMoon[4];  // toward the moon
  // The cloud layer: x how much of the sky it covers (0 none .. 1 all of it); y, z how far it has
  // drifted (world units); w how far its shapes have changed.
  float skyClouds[4];
  // The weather on surfaces and in the air (with a computed sky): x how wet things are, y how much
  // snow lies (both 0..1), z how thick the air is (the share of light lost per world unit), w the
  // most of a far thing the air may hide.
  float weather[4];
  // What falls just now: x how hard it rains, y how hard it snows (0..1), z a lightning's flash.
  // w: what the game's own glowing effects are multiplied by under a computed sky (0: by nothing).
  // They were made for the day's exposure; to an eye opened for the night they would be many
  // times brighter, and a faint ring of dust a white arc.
  float falling[4];
  // The mist that lies on low ground (with a computed sky): x how thick it is at its floor (the
  // share of light lost per world unit; 0: none), y the floor's height, z 1 / the height over
  // which it thins to 1/e, w the most of a thing it may hide.
  float mist[4];
  // A view for checking art (modern look): x says what every lit surface shows in place of its
  // lit colour: 0 nothing special, 1 its colour alone, 2 which way it faces, 3 how rough it is,
  // 4 how much of a metal, 5 how much of the sky's light its creases let in, 6 the sun's shadow,
  // 7 the small lights alone, 8 which of the sun's shadow maps each place uses, 9 which level of
  // detail each model is drawn at.
  float inspect[4];
  // The tracks in lying snow (tracks.h): x, y what a place's x and z are multiplied by to read the
  // picture of what is trodden; z: 1 = there is one.
  float tracksArea[4];
  // The world beyond the map's edge (terrain.h): x, y the map's width and height; z: 1 / how far
  // out the land and sea go on before they are lost in the distance (0: the map ends at its
  // edge); w: how much of its light the ground keeps just outside (the playable ground ends there).
  float beyond[4];
  // Light in the air (air.h): x 1 when its volume is drawn and every surface is seen through it in
  // place of the haze and mist formulas; y the nearest distance the volume holds; z log(farthest /
  // nearest); w the most of a far thing the air may hide.
  float air[4];
  // The volumetric clouds' shadows (clouds.h): x the clouds' underside (0: the layer's own shadows,
  // CloudShadow's formula); y, z the shadow picture's middle (world x, z); w 1 / the width it covers.
  float cloudShadow[4];
  // Soft shadows (lighting.h, SunShadow): x 1 = on; y how much wider a shadow's edge grows for each world
  // unit between what casts it and where it falls (the sun's size); z how far round a place what casts
  // its shadow is looked for, w the widest an edge may grow (both world units).
  float softShadows[4];
  // The temporal upscaler (fsr3.h, World::temporalUpscale): x what is added to the level of detail every
  // surface texture is read at (Sample, below), negative: read as for the screen's size and finer, since the
  // frames laid over each other see between the pixels drawn (0 without it).
  float temporal[4];
  // The sky from tables (sky.h, World::skyTables): x 1 when ComputedSky reads the sky's view table (kSkyTableSlot)
  // in place of the three fitted colours; y how much of it the clouds' grey takes, z that grey's brightness, w
  // how much of the night's own faint light there is. skyExtra: rgb what lightning adds for a moment.
  float skyTable[4];
  float skyExtra[4];
  // Bounce light (bounce.h): x 1 = every lit surface takes the probes' light in place of the sky's fitted light; y
  // the columns' spacing (world units); z, w the grid's first column's number across x and z (column c stands at
  // c * spacing). bounceLayers: the probes' heights over the ground in each column.
  float bounce[4];
  float bounceLayers[4];
  // Reflections by ray tracing (World::rayReflections; shaders built with RAYS): x 1 = on; y how far a mirrored ray
  // looks; z, w the map's width and height.
  float rayEffects[4];
  // Wet shorelines and caustics (World::waterDetail): x 1 = on; y the water's level (world y); z the height of the
  // band over it the waves have wetted; w how strong the bright net on a shallow bed is.
  float shore[4];
};

// The same block, as HLSL source: shaders start with it.
inline const char kViewShaderBlock[] = R"hlsl(
cbuffer View : register(b0) {
  row_major float4x4 viewProj;
  float3 cameraPosition;
  float time;
  float4 viewport;        // width, height, 1 / width, 1 / height
  float3 sunDirection;    // toward the sun
  float look;             // 0: the original's lighting, 1: the modern one
  float3 sunColor;
  float lightMultiplier;
  float3 sunAmbience;
  float shadowsOn;
  float3 shadowFill;
  float cascadeCount;
  float4 modern;          // x: sun strength, y: sky strength, z: exposure
  row_major float4x4 shadowMatrix[4];
  float4 cascadeTexel;
  float4 lightGrid;       // the small lights' grid: corner x, z; 1 / square size; 1 = there are lights
  float4 lightGridSize;   // squares across, down; the row where the lights begin
  float4 skyZenith;       // the computed sky (sky.h): rgb overhead; w: 1 = computed, 0 = the map's own
  float4 skyHorizon;      // rgb at the horizon; w: how much it is night
  float4 skyGlow;         // rgb around the sun or moon; w: how bright the day is
  float4 skyBody;         // toward the sun; w: 1 = the moon's light falls
  float4 skyMoon;         // toward the moon
  float4 skyClouds;       // the cloud layer: x cover 0..1; y, z its drift (world units); w its change
  float4 weather;         // x wet 0..1; y snow lying 0..1; z the air's thickness per world unit; w the most it hides
  float4 falling;         // x rain 0..1; y snow 0..1; z a lightning's flash; w: the night's dimming of glowing effects (0: none)
  float4 mist;            // the mist on low ground: x thickness per world unit at its floor; y the floor's height; z 1 / its depth; w the most it hides
  float4 inspect;         // x: what lit surfaces show instead of their lit colour (0: nothing special; view.h)
  float4 tracksArea;      // the tracks in lying snow: x, y: 1 / the map's width and height; z: 1 = there are tracks
  float4 beyond;          // the world beyond the map's edge: x, y the map's size; z: 1 / how far out it goes (0: not at all); w: the light kept just outside
  float4 air;             // light in the air (air.h): x 1 = on; y the volume's nearest distance; z log(farthest / nearest); w the most it hides
  float4 cloudShadow;     // the volumetric clouds' shadows (clouds.h): x their underside (0: none); y, z the picture's middle; w 1 / its width
  float4 softShadows;     // x 1 = soft shadows; y an edge's growth per world unit from the caster; z the search for casters, w the widest edge (world units)
  float4 temporal;        // the temporal upscaler: x the textures' level of detail bias (0 without it)
  float4 skyTable;        // the sky from tables: x 1 = ComputedSky reads its view; y the clouds' grey share, z its brightness; w the night's faint light
  float4 skyExtra;        // rgb: lightning's light for a moment
  float4 bounce;          // bounce light: x 1 = on; y the columns' spacing; z, w the grid's first column across x and z
  float4 bounceLayers;    // the probes' heights over the ground
  float4 rayEffects;      // reflections by ray tracing: x 1 = on; y how far a ray looks; z, w the map's size
  float4 shore;           // wet shorelines: x 1 = on; y the water's level; z the wet band's height; w the caustics' strength
};
// (every texture read with Sample takes the temporal upscaler's bias: textures with levels - the art's - are
// read as finely as the screen's size wants; pictures drawn by the renderer have one level and do not change)
#define Sample(s, uv) SampleBias(s, uv, temporal.x)
SamplerState samplerAniso : register(s0);        // anisotropic, wrap
SamplerState samplerLinearClamp : register(s1);
SamplerState samplerPointClamp : register(s2);
SamplerState samplerLinear : register(s3);       // trilinear, wrap
SamplerState samplerPoint : register(s4);        // point, wrap
SamplerComparisonState samplerShadow : register(s5);

// The sky from tables (sky.h, Sky::BuildTables): its view from the eye, three slices (the smooth part; the haze's
// light for the sun and for the moon, whose glow round the body is taken per pixel). Where a direction lies in
// it (round from +x; from overhead through the horizon, at the middle, to straight down, as Hillaire maps it
// for an eye 500 m up), and the haze's glow round a body.
Texture3D skyViewTable : register(t32);
float2 SkyTableUv(float3 d) {
  const float r = 6360e3 + 500, rho = sqrt(500.0 * (2 * 6360e3 + 500.0));
  float beta = acos(rho / r), horizon = 3.14159265 - beta;
  float zenith = acos(clamp(d.y, -1, 1));
  float v = zenith < horizon ? 0.5 * (1 - sqrt(saturate(1 - zenith / horizon))) : 0.5 + 0.5 * sqrt(saturate((zenith - horizon) / beta));
  return float2(atan2(d.z, d.x) / (2 * 3.14159265), 0.5 / 108 + v * (1 - 1.0 / 108));
}
float SkyHazeGlow(float mu) {
  const float g = 0.76;
  return 3.0 / (8.0 * 3.14159265) * ((1 - g * g) * (1 + mu * mu)) / ((2 + g * g) * pow(1 + g * g - 2 * g * mu, 1.5));
}
// The air's light from a direction as the table has it (no clouds, no night glow: the sky's pass adds its own).
float3 SkyFromTable(float3 d) {
  float2 uv = SkyTableUv(d);
  return skyViewTable.SampleLevel(samplerLinear, float3(uv, 0.5 / 3), 0).rgb +
         skyViewTable.SampleLevel(samplerLinear, float3(uv, 1.5 / 3), 0).rgb * SkyHazeGlow(dot(d, skyBody.xyz)) +
         skyViewTable.SampleLevel(samplerLinear, float3(uv, 2.5 / 3), 0).rgb * SkyHazeGlow(dot(d, skyMoon.xyz));
}

// Bounce light (bounce.h): 32 x 32 columns of probes from column (bounce.z, bounce.w) on, `bounce.y` apart, four
// probes in each at bounceLayers over the ground (bounceColumns: its height, by the column's number mod 32, as the
// probes' places in the pictures are). Each probe's light from every side (octahedral, 6 x 6 with a border) and
// how far it sees (the mean distance and its square, 14 x 14 with a border).
Texture2D bounceLight : register(t34);
Texture2D bounceSight : register(t35);
Texture2D<float> bounceColumns : register(t36);
float2 BounceOct(float3 n) {  // a direction to the octahedral square's -1..1 (y up)
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  float2 o = n.xz;
  if (n.y < 0) o = (1 - abs(o.yx)) * float2(o.x >= 0 ? 1 : -1, o.y >= 0 ? 1 : -1);
  return o;
}
float3 BounceDir(float2 o) {  // and back
  float3 n = float3(o.x, 1 - abs(o.x) - abs(o.y), o.y);
  if (n.y < 0) n.xz = (1 - abs(n.zx)) * float2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
  return normalize(n);
}
int2 BounceTile(int2 column) { return ((column % 32) + 32) % 32; }
float BounceBase(int2 column) { return bounceColumns.Load(int3(BounceTile(column), 0)); }
float3 BounceProbe(int2 column, int layer) {
  return float3(column.x * bounce.y, BounceBase(column) + bounceLayers[layer], column.y * bounce.y);
}
float4 BounceLightOf(int2 column, int layer, float3 n) {  // (a: 1, or 0 for a probe inside something)
  int2 tile = BounceTile(column) + int2(0, layer * 32);
  return bounceLight.SampleLevel(samplerLinearClamp, (tile * 8 + 1 + (BounceOct(n) * 0.5 + 0.5) * 6) / float2(256, 1024), 0);
}
float2 BounceSightOf(int2 column, int layer, float3 d) {
  int2 tile = BounceTile(column) + int2(0, layer * 32);
  return bounceSight.SampleLevel(samplerLinearClamp, (tile * 16 + 1 + (BounceOct(d) * 0.5 + 0.5) * 14) / float2(512, 2048), 0).rg;
}
// The light coming in at P (facing N) from round about, as the eight probes round it have it (each by how near it
// is, how much it faces the place, and whether it sees it); `inside`: 1 well within the grid, fading to 0 over its
// outer columns.
float3 BounceAt(float3 P, float3 N, out float inside) {
  float2 g = P.xz / bounce.y - bounce.zw;
  inside = saturate(min(min(g.x, g.y), min(31 - g.x, 31 - g.y)) * 0.5);
  if (inside <= 0) return 0;
  int2 i0 = int2(clamp(floor(g), 0, 30));
  float2 f = saturate(g - i0);
  int2 c0 = i0 + int2(bounce.zw);
  float base = lerp(lerp(BounceBase(c0), BounceBase(c0 + int2(1, 0)), f.x), lerp(BounceBase(c0 + int2(0, 1)), BounceBase(c0 + int2(1, 1)), f.x), f.y);
  float h = P.y - base;
  int k = h < bounceLayers.y ? 0 : h < bounceLayers.z ? 1 : 2;
  float t = saturate((h - bounceLayers[k]) / (bounceLayers[k + 1] - bounceLayers[k]));
  float3 biased = P + N * 0.3;
  float3 sum = 0;
  float weights = 0;
  [unroll] for (int i = 0; i < 8; ++i) {
    int2 o = int2(i & 1, (i >> 1) & 1);
    int up = i >> 2, layer = k + up;
    int2 column = c0 + o;
    float3 toProbe = BounceProbe(column, layer) - biased;
    float distance = length(toProbe);
    float3 dir = toProbe / max(distance, 1e-4);
    float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * (up ? t : 1 - t);
    float facing = (dot(dir, N) + 1) * 0.5;
    w *= facing * facing + 0.2;
    float2 seen = BounceSightOf(column, layer, -dir);
    if (distance > seen.x) {  // (the probe sees something nearer that way: Chebyshev's bound on how likely it sees the place)
      float variance = abs(seen.y - seen.x * seen.x), d = distance - seen.x;
      float chance = variance / (variance + d * d);
      w *= max(chance * chance * chance, 0.05);
    }
    float4 light = BounceLightOf(column, layer, N);
    w = max(w, 1e-6) * lerp(0.02, 1, light.a);  // (a probe inside something all but passed over)
    sum += light.rgb * w;
    weights += w;
  }
  return sum / weights;
}

// The computed sky's light from a direction (without the sun's and the moon's own disc). With the sky from
// tables: the table's, greyed by the clouds and lit by the night's faint light and lightning as the fit is.
float3 ComputedSky(float3 d) {
  [branch] if (skyTable.x > 0.5) {
    float3 t = SkyFromTable(d) + float3(0.012, 0.016, 0.028) * skyTable.w;
    t = lerp(t, float3(0.92, 0.96, 1.0) * skyTable.z, skyTable.y) + skyExtra.rgb;
    return lerp(t, skyHorizon.rgb * 0.3, saturate(-d.y * 5));  // below the horizon: the ground far away
  }
  float3 c = lerp(skyHorizon.rgb, skyZenith.rgb, pow(saturate(d.y), 0.5));
  float3 body = skyBody.w > 0.5 ? skyMoon.xyz : skyBody.xyz;
  float s = saturate(dot(d, body));
  float s2 = s * s, s4 = s2 * s2, s16 = s4 * s4 * s4 * s4;
  c += skyGlow.rgb * ((s4 * s * 0.25 + s16 * s16 * s16 * 0.6) * lerp(1, 0.35, saturate(d.y)));
  return lerp(c, skyHorizon.rgb * 0.3, saturate(-d.y * 5));  // below the horizon: the ground far away
}
// The cloud layer (sky.h), 150 above the ground's zero: the layered noise it is cut from at a place
// on the layer (world x, z), how much cloud that makes there, and what of the falling light comes
// through to a place on the ground.
float CloudHash(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float CloudNoise(float3 p) {
  float3 cell = floor(p), f = frac(p);
  f = f * f * (3 - 2 * f);
  return lerp(lerp(lerp(CloudHash(cell), CloudHash(cell + float3(1, 0, 0)), f.x), lerp(CloudHash(cell + float3(0, 1, 0)), CloudHash(cell + float3(1, 1, 0)), f.x), f.y),
              lerp(lerp(CloudHash(cell + float3(0, 0, 1)), CloudHash(cell + float3(1, 0, 1)), f.x), lerp(CloudHash(cell + float3(0, 1, 1)), CloudHash(cell + float3(1, 1, 1)), f.x), f.y),
              f.z);
}
float CloudField(float2 q) {
  float3 p = float3((q + skyClouds.yz) * 0.004, skyClouds.w);
  return CloudNoise(p) * 0.55 + CloudNoise(p * 2.3 + 7.1) * 0.3 + CloudNoise(p * 5.1 + 3.3) * 0.15;
}
float CloudFrom(float field) { return saturate((field - lerp(0.75, 0.2, skyClouds.x)) * 5); }
Texture2D<float> cloudShadowPicture : register(t31);  // (clouds.h: what comes through the clouds toward the sun, at their underside)
float CloudShadow(float3 world) {
  if (skyZenith.w < 0.5 || skyClouds.x <= 0) return 1;
  float under = cloudShadow.x > 0 ? cloudShadow.x : 150;
  float2 q = world.xz + sunDirection.xz * ((under - world.y) / max(sunDirection.y, 0.2));
  [branch] if (cloudShadow.x > 0) {  // the volumetric clouds' own (their light through them)
    float2 at = (q - cloudShadow.yz) * cloudShadow.w + 0.5;
    if (any(at < 0) || any(at > 1)) return 1;
    return 0.25 + 0.75 * cloudShadowPicture.SampleLevel(samplerLinearClamp, at, 0);
  }
  return 1 - 0.75 * CloudFrom(CloudField(q));
}
// The map's pictures (masks, normals, the water's depth) read beyond the map's edge: the map laid
// out again mirrored, so that land and sea go on without a seam. Inside the map: as it is.
float2 MirrorUv(float2 uv) { return 1 - abs(1 - frac(uv * 0.5) * 2); }
// How far outside the map a place is, in world units (0: on the map).
float OutsideMap(float3 world) {
  if (beyond.z <= 0) return 0;
  return length(max(max(-world.xz, world.xz - beyond.xy), 0));
}
// The colour of thick air, and a colour seen through `distance` of it.
// (the light that reaches the ground from the sky, greyed: thick air is lit like the ground under
// it, and is not as bright or as coloured as the sky at the horizon, or a dawn's fog would be orange)
float3 AirColor() {
  float3 light = skyZenith.rgb * 0.85 + skyHorizon.rgb * 0.15;
  return lerp(light, dot(light, float3(0.3, 0.6, 0.1)), 0.7) * 0.65;
}
float3 ThroughAir(float3 color, float distance) {
  if (weather.z <= 0) return color;
  return lerp(color, AirColor(), min(1 - exp(-distance * weather.z), weather.w));
}
// The mist that lies on low ground: how much of what is at `world` it hides from the eye. It is
// thickest at its floor and thins upward; what is lost on the way from the eye is that thickness
// summed along the line of sight (so a valley seen from high above shows as a pool of it, and a
// hilltop stands clear). It lies in banks that drift.
float3 MistColor() { return AirColor() * 0.75; }
float MistLost(float3 world) {
  if (mist.x <= 0) return 0;
  float a = max((cameraPosition.y - mist.y) * mist.z, -1), b = max((world.y - mist.y) * mist.z, -1);
  float d = a - b;
  float mean = abs(d) > 1e-3 ? (exp(-b) - exp(-a)) / d : exp(-a);
  float2 q = world.xz + skyClouds.yz * 0.12;
  float banks = smoothstep(0.28, 0.8, CloudNoise(float3(q * 0.008, skyClouds.w * 2.5 + 11.0)) * 0.5 + CloudNoise(float3(q.yx * 0.021 + 7.3, skyClouds.w * 4.0 + 3.0)) * 0.3 +
                                      CloudNoise(float3(q * 0.057 + 2.9, skyClouds.w * 6.0 + 5.0)) * 0.2) * 1.5;
  return min(1 - exp(-length(world - cameraPosition) * mist.x * mean * banks), mist.w);
}
// Light in the air (air.h), when it is on: what the air between the eye and `world` adds to what is
// seen there (rgb) and what of the light from there still comes through (a). The volume holds, for
// each cell, the sum from the eye to the cell's far end, its depth spaced by the log of the distance.
Texture3D airVolume : register(t30);
float4 AirAt(float3 world) {
  float4 clip = mul(viewProj, float4(world, 1));
  float2 uv = clip.w > 1e-4 ? saturate(clip.xy / clip.w * float2(0.5, -0.5) + 0.5) : float2(0.5, 0.5);
  uint cellsX, cellsY, cellsZ;
  airVolume.GetDimensions(cellsX, cellsY, cellsZ);
  float along = log(max(length(world - cameraPosition), air.y) / air.y) / air.z * cellsZ;  // (in cells)
  float4 a = airVolume.SampleLevel(samplerLinearClamp, float3(uv, max(along - 0.5, 0.5) / cellsZ), 0);
  if (along < 1) a = lerp(float4(0, 0, 0, 1), a, along);  // (nearer than the first cell's far end)
  // (never all of a far thing hidden: what is lost is held to the most the air may hide)
  float kept = max(a.a, 1 - air.w);
  a.rgb *= (1 - kept) / max(1 - a.a, 1e-4);
  return float4(a.rgb, kept);
}
// A colour at `world` as the eye sees it: through the thick air and through the mist.
float3 ThroughAirTo(float3 color, float3 world) {
  [branch] if (air.x > 0.5) {
    float4 a = AirAt(world);
    return color * a.a + a.rgb;
  }
  color = ThroughAir(color, length(world - cameraPosition));
  float lost = MistLost(world);
  return lost > 0 ? lerp(color, MistColor(), lost) : color;
}
// What the far distance looks like in a direction under a computed sky: the haze at the horizon
// that way. The world beyond the map's edge is lost in it, and the sky's pass shows it below the
// horizon then, so the two meet without a rim.
float3 HorizonHaze(float3 d) {
  float3 c = ComputedSky(normalize(float3(d.x, 0.03, d.z)));
  if (weather.z > 0) c = lerp(c, AirColor(), weather.w * saturate(weather.z * 600));
  return c;
}
// What the world beyond the map's edge goes over into far out: the far distance's colour greyed
// and dimmed (Chris, 2026-10-14: "instead of bright white clouds maybe like its just greyed out").
float3 BeyondGrey(float3 c) {
  return dot(c, float3(0.3, 0.6, 0.1)) * float3(0.50, 0.52, 0.55);
}
// What of it reaches a surface that faces that way, from all sides.
float3 SkyAmbient(float3 n) {
  return lerp(skyHorizon.rgb * 0.3, skyZenith.rgb * 0.8 + skyHorizon.rgb * 0.2, saturate(n.y * 0.5 + 0.5));
}
)hlsl";

struct View {
  ViewConstants constants;
  Vec3 eye;        // where levels of detail are measured from (the camera, also in a shadow pass)
  float fovY = 1;
  int width = 1, height = 1;
  Vec4 planes[5];  // left, right, bottom, top, near: a point p is inside when dot(plane.xyz, p) + plane.w >= 0
  // A shadow cascade's view only: the square of ground (seen from the sun) that the cascade before
  // it covers. What casts wholly inside that square is in the nearer map already and every surface
  // its shadow can fall on reads that map, so this cascade need not draw it. coveredHalf 0: none.
  Vec3 coveredRight, coveredUp;  // the sun's sideways axes
  float coveredX = 0, coveredY = 0, coveredHalf = 0;
  // A lamp's shadow view only: what is wholly farther than `reach` from `reachFrom` is beyond the
  // lamp's light and need not be drawn. reach 0: no such limit.
  Vec3 reachFrom;
  float reach = 0;
  // And what the lamp rides on is not drawn into its map (a lamp set into a hull would be shadowed
  // by the hull): the thing whose bounding sphere has its middle at `carrier`. hasCarrier false: none.
  Vec3 carrier;
  bool hasCarrier = false;
  bool CoveredByNearer(Vec3 centre, float radius) const {
    return coveredHalf > 0 && std::fabs(Dot(centre, coveredRight) - coveredX) + radius <= coveredHalf &&
           std::fabs(Dot(centre, coveredUp) - coveredY) + radius <= coveredHalf;
  }

  // Sets the matrix everything is projected with, and the planes that bound what it shows.
  void SetMatrix(const Mat4& m) {
    constants.viewProj = m;
    auto row = [&](int r) { return Vec4{m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]}; };
    auto add = [](Vec4 a, Vec4 b, float s) { return Vec4{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s}; };
    planes[0] = add(row(3), row(0), 1);
    planes[1] = add(row(3), row(0), -1);
    planes[2] = add(row(3), row(1), 1);
    planes[3] = add(row(3), row(1), -1);
    planes[4] = add(row(3), row(2), -1);  // reversed depth: the near plane is z = w
  }

  static View Make(const Camera& camera, int width, int height, float time) {
    View v;
    v.constants = ViewConstants{};
    v.width = width;
    v.height = height;
    v.fovY = camera.fovY;
    v.eye = camera.Eye();
    v.SetMatrix(camera.Projection(static_cast<float>(width) / static_cast<float>(height)) * camera.View());
    v.constants.cameraPosition[0] = v.eye.x;
    v.constants.cameraPosition[1] = v.eye.y;
    v.constants.cameraPosition[2] = v.eye.z;
    v.constants.time = time;
    v.constants.viewport[0] = static_cast<float>(width);
    v.constants.viewport[1] = static_cast<float>(height);
    v.constants.viewport[2] = 1.0f / static_cast<float>(width);
    v.constants.viewport[3] = 1.0f / static_cast<float>(height);
    v.constants.sunDirection[1] = 1;
    v.constants.lightMultiplier = 1;
    return v;
  }

  // False when the box lies wholly outside one of the planes.
  bool BoxVisible(Vec3 lo, Vec3 hi) const {
    for (const Vec4& p : planes) {
      float d = p.x * (p.x >= 0 ? hi.x : lo.x) + p.y * (p.y >= 0 ? hi.y : lo.y) + p.z * (p.z >= 0 ? hi.z : lo.z) + p.w;
      if (d < 0) return false;
    }
    return true;
  }
};

}  // namespace render
