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

// [cardcost] Where the part of `view` that can be seen lies, seen from the sun (fitCasters): its box in the sun's sideways
// axes `right`, `up` (x0, y0, x1, y1). That part is the view's pyramid from its near plane down to the height `floor`:
// the eight corners of its near plane and of where its corner rays reach that height hold it. False where a corner ray
// does not go down (the view reaches the horizon: no end to it).
static bool SeenFromSun(const View& view, float floor, Vec3 right, Vec3 up, float* box) {
  const Mat4 back = Inverse(view.constants.viewProj);
  auto at = [&](float x, float y, float z) {
    const Vec4 p = back * Vec4{x, y, z, 1};
    return Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
  };
  box[0] = box[1] = 1e30f;
  box[2] = box[3] = -1e30f;
  auto take = [&](Vec3 p) {
    const float x = Dot(p, right), y = Dot(p, up);
    box[0] = std::min(box[0], x), box[1] = std::min(box[1], y);
    box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
  };
  for (int k = 0; k < 4; ++k) {
    const float x = k & 1 ? 1.0f : -1.0f, y = k & 2 ? 1.0f : -1.0f;
    const Vec3 nearPoint = at(x, y, 1), further = at(x, y, 0.5f);  // (reversed depth: 1 at the near plane)
    const Vec3 d = further - nearPoint;
    if (!(d.y < -1e-4f * Length(d)) || nearPoint.y < floor) return false;
    take(nearPoint);
    take(nearPoint + d * ((floor - nearPoint.y) / d.y));
  }
  return std::isfinite(box[0]) && std::isfinite(box[1]) && std::isfinite(box[2]) && std::isfinite(box[3]);
}

void Shadows::Place(const Camera& camera, Vec3 sunDirection, View* view) {
  const Vec3 L = Normalize(sunDirection);
  // the sun's axes: it looks along -L
  const Vec3 reference = std::fabs(L.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  const Vec3 right = Normalize(Cross(reference, L));
  const Vec3 up = Cross(L, right);
  // [cardcost] (fitCasters) where what the camera sees lies, seen from the sun, before the cascades take the view's place
  float seen[4] = {0, 0, 0, 0};
  fitted_ = fitCasters && SeenFromSun(*view, fitFloor, right, up, seen);
  if (fitted_) {
    seen[0] -= fitMargin, seen[1] -= fitMargin;
    seen[2] += fitMargin, seen[3] += fitMargin;
  }

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
  // [cardcost] (keepEvery) whether far cascades may be kept this frame, and whose turn it is to be drawn anew
  keeping_ = keepEvery >= 2;
  if (!keeping_)
    for (Kept& k : kept_) k.valid = false;
  const int turn = keeping_ ? static_cast<int>(keepFrame_++ % static_cast<uint64_t>(keepEvery)) : 0;
  const float keepCos = std::cos(keepSunDegrees * 0.017453293f);
  float nearerX = 0, nearerY = 0, nearerHalf = 0;  // the square the cascade before this one covers
  float nearerSlack = 0;                           // [cardcost] (and how far that one may move while this one is kept)
  for (int i = 0; i < count_; ++i) {
    const float radius = radii[i];
    Vec3 centre = centres[i];
    // move the centre in whole texels only, so that shadow edges do not crawl when the camera moves
    const float texel = 2 * radius / static_cast<float>(size_);
    float x = std::floor(Dot(centre, right) / texel) * texel, y = std::floor(Dot(centre, up) / texel) * texel;
    centre = right * x + up * y + L * Dot(centre, L);
    // [cardcost] (keepEvery) a far cascade drawn a frame or more before is kept, not drawn, where it is not its turn and
    // it still serves: the same size, the sun within keepSunDegrees of where it was, its own place and the nearer one's
    // moved by at most their slack (a share of their radius), and what the camera sees within what it was drawn for
    const float slack = keeping_ ? keepSlack * radius : 0.0f;
    draws_[i] = true;
    Kept& k = kept_[i];
    if (keeping_ && i > 0 && k.valid && (i - 1) % keepEvery != turn && k.count == count_ && k.size == size_ && k.radius == radius &&
        Dot(k.sun, L) >= keepCos && std::fabs(x - k.x) <= slack && std::fabs(y - k.y) <= slack && k.nearerHalf == nearerHalf &&
        std::fabs(nearerX - k.nearerX) <= nearerSlack && std::fabs(nearerY - k.nearerY) <= nearerSlack &&
        (!k.fitted || (fitted_ && seen[0] >= k.seen[0] && seen[1] >= k.seen[1] && seen[2] <= k.seen[2] && seen[3] <= k.seen[3]))) {
      cascadeViews_[i] = k.view;
      view->constants.shadowMatrix[i] = k.matrix;
      view->constants.cascadeTexel[i] = texel;
      draws_[i] = false;
      nearerX = k.x;
      nearerY = k.y;
      nearerHalf = radius * 0.96f - 6 * texel - slack;
      nearerSlack = slack;
      continue;
    }
    k.valid = false;  // (drawn anew: what its map held is gone)
    // (kept cascades: what it is drawn for this much wider, so that it serves while the camera moves a little)
    const float box[4] = {seen[0] - slack, seen[1] - slack, seen[2] + slack, seen[3] + slack};

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
    if (fitted_) {  // [cardcost] (fitCasters) its four sides drawn in to where what is seen lies (none: nothing drawn)
      const float x0 = std::max(x - radius, box[0]), x1 = std::min(x + radius, box[2]);
      const float y0 = std::max(y - radius, box[1]), y1 = std::min(y + radius, box[3]);
      if (x0 < x1 && y0 < y1) {
        cv.planes[0] = Vec4{right.x, right.y, right.z, -x0};
        cv.planes[1] = Vec4{-right.x, -right.y, -right.z, x1};
        cv.planes[2] = Vec4{up.x, up.y, up.z, -y0};
        cv.planes[3] = Vec4{-up.x, -up.y, -up.z, y1};
      } else {
        cv.planes[0] = Vec4{0, 0, 0, -1};  // (no point is inside)
      }
    }
    cv.constants.shadowsOn = 0;
    cv.coveredRight = right;
    cv.coveredUp = up;
    cv.coveredX = nearerX;
    cv.coveredY = nearerY;
    cv.coveredHalf = nearerHalf;
    cv.constants.viewport[0] = cv.constants.viewport[1] = static_cast<float>(size_);
    cv.constants.viewport[2] = cv.constants.viewport[3] = 1.0f / static_cast<float>(size_);

    // from the projection's -1..1 (y up) to the map's 0..1 (y down)
    const Mat4 toMap = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    view->constants.shadowMatrix[i] = toMap * lightViewProj;
    view->constants.cascadeTexel[i] = texel;
    if (keeping_ && i > 0) {  // [cardcost] (what it is drawn for, kept once it is drawn: Drawn)
      Kept& p = pending_[i];
      p.view = cv;
      p.matrix = view->constants.shadowMatrix[i];
      p.sun = L;
      p.radius = radius;
      p.x = x, p.y = y;
      p.nearerX = nearerX, p.nearerY = nearerY, p.nearerHalf = nearerHalf;
      p.fitted = fitted_;
      std::copy(box, box + 4, p.seen);
      p.count = count_;
      p.size = size_;
    }
    // the scene's shaders read a cascade inside 0.02 .. 0.98 of its map and look a few texels around ([cardcost] and,
    // while far cascades are kept, the slack this one may move by while the one after it is kept)
    nearerX = x;
    nearerY = y;
    nearerHalf = radius * 0.96f - 6 * texel - slack;
    nearerSlack = slack;
  }
  view->constants.cascadeCount = static_cast<float>(count_);
  view->constants.shadowsOn = 1;
}

void Shadows::Drawn(int i) {
  if (i <= 0 || i >= kMaxCascades || !keeping_ || !draws_[i]) return;
  kept_[i] = pending_[i];
  kept_[i].valid = true;
}

}  // namespace render
