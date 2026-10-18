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

// A cinematic camera (Camera::cinematic) sees from low down far toward the horizon, where the game's camera sees a
// patch of ground round what it looks at: its four cascades follow the view itself instead. Each is the ball round
// a slice of what the camera sees, from the eye out to `cinematicReach`, the slices' ends spaced between evenly
// and by a constant ratio (Zhang et al.'s "practical" split), so that shadows are sharp near the eye and reach far.
// A ball does not change as the camera turns, so the shadows' edges stand still then. The first slice starts at
// the eye's height over the ground ahead: a camera high in the air sees no ground nearer than that.
void Shadows::AlongView(const Camera& camera, const View& view, Vec3* centres, float* radii) {
  count_ = kMaxCascades;
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const Vec3 eye = camera.Eye();
  const float tanY = std::tan(camera.fovY * 0.5f), tanX = tanY * static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1));
  const float spread = tanX * tanX + tanY * tanY;  // (a slice's far corner lies this far off the axis, squared, per unit along it)
  const float above = std::max(0.0f, eye.y - camera.target.y) * 0.6f;
  const float first = 0.5f, last = above + cinematicReach;
  float from = first;
  for (int i = 0; i < count_; ++i) {
    const float share = static_cast<float>(i + 1) / static_cast<float>(count_);
    const float growing = first * std::pow(cinematicReach / first, share), even = first + (cinematicReach - first) * share;
    const float to = i + 1 == count_ ? last : above + cinematicSplit * growing + (1 - cinematicSplit) * even;
    // the smallest ball round the slice from `from` to `to`: its middle on the axis, as far out as makes the near
    // and the far corners equally far from it (at most at the far end)
    const float along = std::min(to, 0.5f * (from + to) * (1 + spread));
    radii[i] = std::sqrt((to - along) * (to - along) + to * to * spread);
    centres[i] = eye - back * along;
    from = to;
  }
}

void Shadows::Place(const Camera& camera, Vec3 sunDirection, View* view) {
  const Vec3 L = Normalize(sunDirection);
  // the sun's axes: it looks along -L
  const Vec3 reference = std::fabs(L.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  const Vec3 right = Normalize(Cross(reference, L));
  const Vec3 up = Cross(L, right);

  // The camera looks at a point on the ground from a distance; what it sees lies around that point,
  // more of it beyond than before. Each cascade is a square of ground around a centre pushed forward.
  Vec3 centres[kMaxCascades];
  float radii[kMaxCascades];
  if (camera.cinematic) {
    AlongView(camera, *view, centres, radii);
  } else {
    count_ = 3;
    const Vec3 forward{-std::sin(camera.heading), 0, -std::cos(camera.heading)};
    const float tilt = std::cos(camera.pitch);  // 0 looking straight down: the view is centred on the target
    float radius = std::max(8.0f, camera.distance * 0.55f);
    for (int i = 0; i < count_; ++i, radius *= 3.2f) {
      radii[i] = radius;
      centres[i] = camera.target + forward * (radius * 0.45f * tilt);
    }
  }
  float nearerX = 0, nearerY = 0, nearerHalf = 0;  // the square the cascade before this one covers
  for (int i = 0; i < count_; ++i) {
    const float radius = radii[i];
    Vec3 centre = centres[i];
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
