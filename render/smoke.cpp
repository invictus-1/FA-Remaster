#include "smoke.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

constexpr float kHeightUnit = 16;  // world units of height along the light one unit of the map's heights spans

}  // namespace

bool SmokeShadowMap::Place(const Camera& camera, Vec3 sunDirection, View* view, bool surfaces) {
  const Vec3 L = Normalize(sunDirection);
  if (L.y < 0.05f) return false;  // (a light along the ground: its shadows would be streaks across the map)
  if (!map_ || made_ != size) {
    if (map_) device_->Destroy(map_);
    gfx::TargetDesc td;
    td.width = td.height = size;
    td.format = gfx::Format::RGBA16F;
    map_ = device_->CreateTarget(td);
    made_ = size;
  }
  if (!map_) return false;
  // the sun's axes, as the shadow maps' (shadows.cpp)
  const Vec3 reference = std::fabs(L.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  const Vec3 right = Normalize(Cross(reference, L));
  const Vec3 up = Cross(L, right);
  // a square round what the camera looks at, pushed forward as the nearest cascade is (more is seen beyond the
  // target than before it), as wide as about what a close camera sees
  const Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
  const float tilt = std::cos(camera.pitch);
  const float radius = std::clamp(camera.distance * 0.9f, 20.0f, 500.0f);
  Vec3 centre = camera.target + forward * (radius * 0.45f * tilt);
  // moved in whole texels only, so that the shadows do not crawl when the camera moves
  const float texel = 2 * radius / static_cast<float>(size);
  const float x = std::floor(Dot(centre, right) / texel) * texel, y = std::floor(Dot(centre, up) / texel) * texel;
  centre = right * x + up * y + L * Dot(centre, L);
  const float reach = radius + 400;
  const Vec3 eye = centre + L * reach;
  const float depth = reach + radius + 100;
  const Mat4 projection = {{{1 / radius, 0, 0, 0}, {0, 1 / radius, 0, 0}, {0, 0, 1 / depth, 1}, {0, 0, 0, 1}}};
  const Mat4 lightViewProj = projection * Mat4::View(eye, right, up, L);
  const Mat4 toMap = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
  Mat4 m = toMap * lightViewProj;
  // the third row: the height over the camera's target, over the sun's height above the horizon (so a unit of it
  // is about kHeightUnit along the light, and stays small wherever the line runs)
  const float over = 1 / (std::max(L.y, 0.25f) * kHeightUnit);
  m.m[2][0] = 0;
  m.m[2][1] = over;
  m.m[2][2] = 0;
  m.m[2][3] = -camera.target.y * over;

  ViewConstants& c = view->constants;
  c.smokeMatrix = m;
  c.smokeShadow[0] = surfaces ? 1.0f : 0.0f;
  c.smokeShadow[1] = leastSpread * L.y * over;  // (a distance along the light, in the map's heights)
  c.smokeShadow[2] = strength;
  c.smokeShadow[3] = 1;
  sunView_ = *view;
  sunView_.SetMatrix(lightViewProj);
  sunView_.constants.shadowsOn = 0;
  sunView_.constants.smokeShadow[0] = sunView_.constants.smokeShadow[3] = 0;  // (the smoke is not lit while it is drawn)
  // (far toward the sun: particles stretched along their flight turn their faces to it)
  const Vec3 far = centre + L * 1e5f;
  sunView_.constants.cameraPosition[0] = far.x;
  sunView_.constants.cameraPosition[1] = far.y;
  sunView_.constants.cameraPosition[2] = far.z;
  sunView_.constants.viewport[0] = sunView_.constants.viewport[1] = static_cast<float>(size);
  sunView_.constants.viewport[2] = sunView_.constants.viewport[3] = 1.0f / static_cast<float>(size);
  return true;
}

gfx::PassDesc SmokeShadowMap::Pass() {
  gfx::PassDesc p;
  p.color = map_;
  p.clearColor = true;  // (to no smoke)
  p.clearDepth = false;
  return p;
}

}  // namespace render
