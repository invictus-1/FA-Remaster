#include "shadows.h"

#include <algorithm>
#include <cmath>

namespace render {

Shadows::Shadows(gfx::Device* device) : device_(device) { SetSize(kShadowMapSize); }

void Shadows::SetSize(int size) {
  size = std::clamp(size, 256, 8192);
  if (size == size_ && map_) return;
  if (map_) device_->Destroy(map_);
  gfx::TargetDesc td;
  td.width = td.height = size;
  td.layers = kMaxCascades;
  td.format = gfx::Format::D32;
  map_ = device_->CreateTarget(td);
  size_ = size;
}

gfx::PassDesc Shadows::Pass(int i) const {
  gfx::PassDesc p;
  p.depth = map_;
  p.depthLayer = i;
  return p;
}

void Shadows::Place(const Camera& camera, Vec3 sunDirection, View* view) {
  const Vec3 L = Normalize(sunDirection);
  // the sun's axes: it looks along -L
  const Vec3 reference = std::fabs(L.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  const Vec3 right = Normalize(Cross(reference, L));
  const Vec3 up = Cross(L, right);

  // The camera looks at a point on the ground from a distance; what it sees lies around that point,
  // more of it beyond than before. Each cascade is a square of ground around a centre pushed forward.
  const Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
  const float tilt = std::cos(camera.pitch);  // 0 looking straight down: the view is centred on the target
  float radius = std::max(8.0f, camera.distance * 0.55f);
  float nearerX = 0, nearerY = 0, nearerHalf = 0;  // the square the cascade before this one covers
  for (int i = 0; i < count_; ++i, radius *= 3.2f) {
    Vec3 centre = camera.target + forward * (radius * 0.45f * tilt);
    // move the centre in whole texels only, so that shadow edges do not crawl when the camera moves
    const float texel = 2 * radius / static_cast<float>(size_);
    float x = std::floor(Dot(centre, right) / texel) * texel, y = std::floor(Dot(centre, up) / texel) * texel;
    centre = right * x + up * y + L * Dot(centre, L);

    // far enough toward the sun that nothing casting into the square is cut off
    const float reach = radius + 400;
    const Vec3 eye = centre + L * reach;
    const float depth = reach + radius + 100;
    Mat4 lightView = Mat4::View(eye, right, up, L);
    // orthographic, reversed depth: 1 at the sun's "eye", 0 at `depth`
    Mat4 projection = {{{1 / radius, 0, 0, 0}, {0, 1 / radius, 0, 0}, {0, 0, 1 / depth, 1}, {0, 0, 0, 1}}};
    Mat4 lightViewProj = projection * lightView;

    View& cv = cascadeViews_[i];
    cv = *view;  // the camera's eye, field of view and picture size: levels of detail as in the scene
    cv.SetMatrix(lightViewProj);
    cv.constants.shadowsOn = 0;
    cv.coveredRight = right;
    cv.coveredUp = up;
    cv.coveredX = nearerX;
    cv.coveredY = nearerY;
    cv.coveredHalf = nearerHalf;
    // the scene's shaders read a cascade inside 0.02 .. 0.98 of its map and look a few texels around
    nearerX = x;
    nearerY = y;
    nearerHalf = radius * 0.96f - 6 * texel;
    cv.constants.viewport[0] = cv.constants.viewport[1] = static_cast<float>(size_);
    cv.constants.viewport[2] = cv.constants.viewport[3] = 1.0f / static_cast<float>(size_);

    // from the projection's -1..1 (y up) to the map's 0..1 (y down)
    const Mat4 toMap = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    view->constants.shadowMatrix[i] = toMap * lightViewProj;
    view->constants.cascadeTexel[i] = texel;
  }
  view->constants.cascadeCount = static_cast<float>(count_);
  view->constants.shadowsOn = 1;
}

}  // namespace render
