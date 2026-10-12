// What every drawing pass is told about the current picture: the camera's matrices and position, the
// size of the picture, the time. Also the test "can this box be seen at all".
#pragma once
#include "camera.h"
#include "linalg.h"

namespace render {

// Laid out as the shaders' `View` constant buffer (register b0).
struct ViewConstants {
  Mat4 viewProj;
  float cameraPosition[3];
  float time;         // seconds, for animated surfaces
  float viewport[4];  // width, height, 1 / width, 1 / height
};

// The same block, as HLSL source: shaders start with it.
inline const char kViewShaderBlock[] = R"hlsl(
cbuffer View : register(b0) {
  row_major float4x4 viewProj;
  float3 cameraPosition;
  float time;
  float4 viewport;  // width, height, 1 / width, 1 / height
};
SamplerState samplerAniso : register(s0);        // anisotropic, wrap
SamplerState samplerLinearClamp : register(s1);
SamplerState samplerPointClamp : register(s2);
SamplerState samplerLinear : register(s3);       // trilinear, wrap
SamplerState samplerPoint : register(s4);        // point, wrap
)hlsl";

struct View {
  ViewConstants constants;
  Vec3 eye;
  float fovY = 1;
  int width = 1, height = 1;
  Vec4 planes[5];  // left, right, bottom, top, near: a point p is inside when dot(plane.xyz, p) + plane.w >= 0

  static View Make(const Camera& camera, int width, int height, float time) {
    View v;
    v.width = width;
    v.height = height;
    v.fovY = camera.fovY;
    v.eye = camera.Eye();
    Mat4 m = camera.Projection(static_cast<float>(width) / static_cast<float>(height)) * camera.View();
    v.constants.viewProj = m;
    v.constants.cameraPosition[0] = v.eye.x;
    v.constants.cameraPosition[1] = v.eye.y;
    v.constants.cameraPosition[2] = v.eye.z;
    v.constants.time = time;
    v.constants.viewport[0] = static_cast<float>(width);
    v.constants.viewport[1] = static_cast<float>(height);
    v.constants.viewport[2] = 1.0f / static_cast<float>(width);
    v.constants.viewport[3] = 1.0f / static_cast<float>(height);
    auto row = [&](int r) { return Vec4{m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]}; };
    auto add = [](Vec4 a, Vec4 b, float s) { return Vec4{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s}; };
    v.planes[0] = add(row(3), row(0), 1);
    v.planes[1] = add(row(3), row(0), -1);
    v.planes[2] = add(row(3), row(1), 1);
    v.planes[3] = add(row(3), row(1), -1);
    v.planes[4] = add(row(3), row(2), -1);  // reversed depth: the near plane is z = w
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
