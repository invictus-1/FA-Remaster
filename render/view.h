// What every drawing pass is told about the current picture: the camera's matrices and position, the
// size of the picture, the time, the light and the shadow maps. Also the test "can this box be seen".
#pragma once
#include "camera.h"
#include "gfx.h"
#include "linalg.h"

namespace render {

// The scene is drawn into a target of this format (colours beyond 0..1 survive until the last pass).
inline constexpr gfx::Format kSceneFormat = gfx::Format::RGBA16F;
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
  float modern[4];           // x: sun strength, y: sky strength, z: exposure, w: unused
  Mat4 shadowMatrix[kMaxCascades];     // world -> shadow map (x, y: where in the map; z: depth)
  float cascadeTexel[kMaxCascades];    // world size of one shadow-map texel, per cascade
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
};
SamplerState samplerAniso : register(s0);        // anisotropic, wrap
SamplerState samplerLinearClamp : register(s1);
SamplerState samplerPointClamp : register(s2);
SamplerState samplerLinear : register(s3);       // trilinear, wrap
SamplerState samplerPoint : register(s4);        // point, wrap
SamplerComparisonState samplerShadow : register(s5);
)hlsl";

struct View {
  ViewConstants constants;
  Vec3 eye;        // where levels of detail are measured from (the camera, also in a shadow pass)
  float fovY = 1;
  int width = 1, height = 1;
  Vec4 planes[5];  // left, right, bottom, top, near: a point p is inside when dot(plane.xyz, p) + plane.w >= 0

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
