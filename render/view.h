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
inline constexpr int kShadowMapSlot = 28;    // the sun's shadow maps, one array layer per cascade
inline constexpr int kEnvironmentSlot = 29;  // the sky as a cube map with mip levels (blurrier going down)
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
