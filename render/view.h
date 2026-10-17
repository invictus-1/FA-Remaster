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
// Texture registers every scene shader may read; the frame code binds them, the renderers leave them alone.
inline constexpr int kLampShadowSlot = 27;   // the lamps' shadow maps (lights.h), one array layer per lamp
inline constexpr int kShadowMapSlot = 28;    // the sun's shadow maps, one array layer per cascade
inline constexpr int kEnvironmentSlot = 29;  // the sky as a cube map with mip levels (blurrier going down)
inline constexpr int kAirSlot = 30;          // light in the air (air.h), a volume over the view, when it is on
inline constexpr int kCloudShadowSlot = 31;  // the volumetric clouds' shadows (clouds.h), when they are on
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
};
SamplerState samplerAniso : register(s0);        // anisotropic, wrap
SamplerState samplerLinearClamp : register(s1);
SamplerState samplerPointClamp : register(s2);
SamplerState samplerLinear : register(s3);       // trilinear, wrap
SamplerState samplerPoint : register(s4);        // point, wrap
SamplerComparisonState samplerShadow : register(s5);

// The computed sky's light from a direction (without the sun's and the moon's own disc).
float3 ComputedSky(float3 d) {
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
