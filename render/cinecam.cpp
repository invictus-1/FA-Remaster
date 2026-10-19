#include "cinecam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace render {
namespace {

constexpr float kPi = 3.14159265f;

// A critically damped spring (after T. Lowe, "Critically Damped Ease-In/Ease-Out Smoothing", Game Programming Gems
// 4): `value` follows `goal` without overshooting, `speed` carried from frame to frame, the same whatever the
// frame's length. `settle`: about the time it takes to close most of the gap.
float Spring(float value, float goal, float* speed, float settle, float dt) {
  if (settle <= 0 || dt <= 0) {
    if (settle <= 0) {
      *speed = 0;
      return goal;
    }
    return value;
  }
  const float omega = 2 / settle, x = omega * dt;
  const float decay = 1 / (1 + x + 0.48f * x * x + 0.235f * x * x * x);
  const float change = value - goal;
  const float carried = (*speed + omega * change) * dt;
  *speed = (*speed - omega * carried) * decay;
  return goal + (change + carried) * decay;
}

// An angle moved by whole turns to lie within half a turn of `near`.
float Near(float angle, float near) {
  const float turn = 2 * kPi;
  return angle - turn * std::round((angle - near) / turn);
}

// Where the line from `eye` along `along` (unit length) first meets what one stands on, within `farthest`.
bool Meets(Vec3 eye, Vec3 along, float farthest, const GroundFn& ground, float* distance) {
  if (!ground) return false;
  float t = 0, before = 0;
  while (t <= farthest) {
    const Vec3 p = eye + along * t;
    if (p.y <= ground(p.x, p.z)) {
      float lo = before, hi = t;
      for (int i = 0; i < 16; ++i) {
        const float mid = 0.5f * (lo + hi);
        const Vec3 q = eye + along * mid;
        (q.y <= ground(q.x, q.z) ? hi : lo) = mid;
      }
      *distance = hi;
      return true;
    }
    before = t;
    t += std::max(0.25f, t * 0.03f);
  }
  return false;
}

}  // namespace

GroundFn MirroredGround(GroundFn onMap, float width, float height) {
  return [onMap = std::move(onMap), width, height](float x, float z) {
    auto mirror = [](float v, float size) {
      if (size <= 0) return v;
      v = std::fabs(v);  // (before the edge at 0: mirrored)
      v = std::fmod(v, 2 * size);
      return v > size ? 2 * size - v : v;  // (past the far edge: mirrored back)
    };
    return onMap ? onMap(mirror(x, width), mirror(z, height)) : 0.0f;
  };
}

void CineCamera::Jump(const CinePose& to) {
  pose = wanted = to;
  eyeSpeed_ = {};
  yawSpeed_ = pitchSpeed_ = fovSpeed_ = 0;
  rollSpeed_ = 0;
  flySpeed_ = {};
  cut_ = true;
}

void CineCamera::GoTo(const CinePose& to) {
  wanted = to;
  wanted.yaw = Near(to.yaw, pose.yaw);
}

void CineCamera::Fly(Vec3 along, bool level) {
  const float sy = std::sin(wanted.yaw), cy = std::cos(wanted.yaw);
  const Vec3 right{cy, 0, -sy};
  Vec3 forward{-sy, 0, -cy};
  if (!level) {
    const float sp = std::sin(wanted.pitch), cp = std::cos(wanted.pitch);
    forward = Vec3{-sy * cp, -sp, -cy * cp};
  }
  wanted.eye += right * along.x + Vec3{0, along.y, 0} + forward * along.z;
}

void CineCamera::Turn(float yaw, float pitch) {
  wanted.yaw += yaw;
  wanted.pitch = Clamp(wanted.pitch + pitch, lowestPitch, highestPitch);
}

void CineCamera::Zoom(float factor) { wanted.fovY = Clamp(wanted.fovY * factor, lowestFov, highestFov); }

// (photomode) the free camera: what is held moves what is asked, eased; Update then eases the pose to it as always
void CineCamera::Fly(float seconds, Vec3 move, Vec2 turn, float roll) {
  const float dt = std::clamp(seconds, 0.0f, 0.25f);
  if (mode_ != Mode::Free) {
    mode_ = Mode::Free;
    flySpeed_ = {};
  }
  // the speed: as fast as the eye is high over the ground (from the last Update), at least four units a second; what
  // is held eased in and out over about a tenth of a second
  const float speed = std::max(4.0f, flyHeight_ * 1.5f);
  // (two ways held at once go no faster than the harder of them alone)
  const float held = std::max({std::fabs(move.x), std::fabs(move.y), std::fabs(move.z)}), length = Length(move);
  const Vec3 want = length > 0 ? move * (speed * held / length) : Vec3{};
  const float ease = 1 - std::exp(-dt / 0.1f);
  flySpeed_ = flySpeed_ + (want - flySpeed_) * ease;
  if (Length(flySpeed_) < 1e-4f && Length(want) <= 0) flySpeed_ = {};
  Fly(flySpeed_ * dt, true);  // (right, straight up, forward along the ground)
  // turning: a share of the view's height (a drag to the right turns right, a drag down looks down)
  Turn(-turn.x * wanted.fovY, turn.y * wanted.fovY);
  // rolling: 30 degrees a second at 1, kept when let go; near level it settles to level
  constexpr float kRollRate = kPi / 6, kMostRoll = kPi / 2, kLevel = 1.5f * kPi / 180;
  if (roll != 0) wanted.roll = Clamp(wanted.roll + roll * kRollRate * dt, -kMostRoll, kMostRoll);
  else if (std::fabs(wanted.roll) < kLevel) wanted.roll = 0;
  // the reach round the map: a tenth of its longer side past each edge, and no higher over the ground there than that
  // side is long
  if (mapWidth_ > 0 && mapHeight_ > 0) {
    const float side = std::max(mapWidth_, mapHeight_), margin = 0.1f * side;
    wanted.eye.x = Clamp(wanted.eye.x, -margin, mapWidth_ + margin);
    wanted.eye.z = Clamp(wanted.eye.z, -margin, mapHeight_ + margin);
    wanted.eye.y = std::min(wanted.eye.y, pose.eye.y - flyHeight_ + side);  // (the ground under the eye as Update had it)
  }
}

void CineCamera::Start(const Camera& game) {
  pose = wanted = PoseOf(game);
  eyeSpeed_ = {};
  yawSpeed_ = pitchSpeed_ = fovSpeed_ = 0;
  rollSpeed_ = 0;
  flySpeed_ = {};
  on_ = true;
  back_ = 1;
  mode_ = Mode::Hold;
}

Camera CineCamera::Stop(const GroundFn& ground) {
  if (on_) {
    on_ = false;
    left_ = pose;
    back_ = 0;
  }
  mode_ = Mode::Hold;
  return GameView(pose, ground, gamePitch, gameNearest, gameFarthest);
}

Camera CineCamera::Shown(const Camera& game, const GroundFn& ground) const {
  if (on_) return View(ground);
  if (back_ >= 1) return game;
  // (from where it was to the game's camera as the player has it now, eased in and out)
  const float t = back_ * back_ * (3 - 2 * back_);
  const CinePose to = PoseOf(game);
  CinePose p;
  p.eye = Lerp(left_.eye, to.eye, t);
  p.yaw = left_.yaw + (Near(to.yaw, left_.yaw) - left_.yaw) * t;
  p.pitch = left_.pitch + (to.pitch - left_.pitch) * t;
  p.fovY = std::exp(std::log(left_.fovY) + (std::log(to.fovY) - std::log(left_.fovY)) * t);
  p.roll = left_.roll + (to.roll - left_.roll) * t;  // (photomode: a rolled free camera levels out to the game's)
  return MakeView(p, ground);
}

void CineCamera::SetPace(float pace) {
  pace = Clamp(pace, 0, 1);
  moveSeconds = 1.2f + (0.3f - 1.2f) * pace;
  turnSeconds = 0.8f + (0.15f - 0.8f) * pace;
  zoomSeconds = 1.0f + (0.25f - 1.0f) * pace;
  orbitSpeed = 0.03f + (0.15f - 0.03f) * pace;
}

void CineCamera::FollowPath(const CinePath& path, bool easeIn) {
  path_ = path;
  pathTime_ = 0;
  mode_ = path_.keys.empty() ? Mode::Hold : Mode::Path;
  if (mode_ == Mode::Path && !easeIn) Jump(path_.At(path_.keys.front().time));
}

void CineCamera::Follow(Vec3 place, float distance, float height) {
  mode_ = Mode::Follow;
  followPlace_ = place;
  followDistance_ = std::max(distance, 0.5f);
  followHeight_ = height;
  const Vec3 off = pose.eye - place;  // (from the side it is on now)
  orbit_ = std::fabs(off.x) + std::fabs(off.z) > 1e-3f ? std::atan2(off.x, off.z) : pose.yaw;
}

CinePose CineCamera::PoseOf(const Camera& camera) {
  CinePose p;
  p.eye = camera.Eye();
  p.yaw = camera.heading;
  p.pitch = camera.pitch;
  p.fovY = camera.fovY;
  p.roll = camera.roll;
  return p;
}

Camera CineCamera::GameView(const CinePose& pose, const GroundFn& ground, float leastPitch, float nearest, float farthest) {
  Camera c;
  c.heading = pose.yaw;
  c.pitch = Clamp(pose.pitch, std::max(leastPitch, 0.01f), kPi / 2);
  c.fovY = pose.fovY;
  Vec3 right, up, back;
  c.Axes(&right, &up, &back);
  float away = 0;
  if (!Meets(pose.eye, back * -1.0f, farthest, ground, &away)) {
    // (looking out over the ground and not meeting it near enough: the level of the ground under the eye)
    const float under = ground ? ground(pose.eye.x, pose.eye.z) : 0;
    away = std::max(pose.eye.y - under, 0.0f) / std::max(std::sin(c.pitch), 0.01f);
  }
  c.distance = std::clamp(away, nearest, farthest);
  c.target = pose.eye - back * c.distance;
  return c;
}

void CineCamera::Update(float seconds, const GroundFn& ground) {
  const float dt = std::clamp(seconds, 0.0f, 0.25f);
  if (!on_) {  // (switched off: only the easing back moves on)
    if (back_ < 1) back_ = easeBackSeconds > 0 ? std::min(1.0f, back_ + dt / easeBackSeconds) : 1.0f;
    return;
  }
  if (mode_ == Mode::Path && !path_.keys.empty()) {  // on the path, by its clock
    pathTime_ += dt;
    const CinePose p = path_.At(path_.keys.front().time + pathTime_);
    wanted = p;
    wanted.yaw = Near(p.yaw, pose.yaw);
  } else if (mode_ == Mode::Follow) {  // round the place, looking at it
    orbit_ += orbitSpeed * dt;
    wanted.eye = followPlace_ + Vec3{std::sin(orbit_) * followDistance_, followHeight_, std::cos(orbit_) * followDistance_};
    if (ground) wanted.eye.y = std::max(wanted.eye.y, ground(wanted.eye.x, wanted.eye.z) + clearance);
    const Vec3 look = Normalize(followPlace_ - wanted.eye);
    wanted.yaw = Near(std::atan2(-look.x, -look.z), pose.yaw);
    wanted.pitch = -std::asin(Clamp(look.y, -1, 1));
    wanted.roll = 0;  // (photomode: level again, as it circles)
  }
  wanted.pitch = Clamp(wanted.pitch, lowestPitch, highestPitch);
  wanted.fovY = Clamp(wanted.fovY, lowestFov, highestFov);
  // (what is asked is kept over the ground too, or the eye would be pulled into a hill and pushed back out of it)
  if (ground) wanted.eye.y = std::max(wanted.eye.y, ground(wanted.eye.x, wanted.eye.z) + clearance);
  pose.eye.x = Spring(pose.eye.x, wanted.eye.x, &eyeSpeed_.x, moveSeconds, dt);
  pose.eye.y = Spring(pose.eye.y, wanted.eye.y, &eyeSpeed_.y, moveSeconds, dt);
  pose.eye.z = Spring(pose.eye.z, wanted.eye.z, &eyeSpeed_.z, moveSeconds, dt);
  pose.yaw = Spring(pose.yaw, wanted.yaw, &yawSpeed_, turnSeconds, dt);
  pose.pitch = Spring(pose.pitch, wanted.pitch, &pitchSpeed_, turnSeconds, dt);
  // (the field of view by its logarithm: a zoom from 10 to 20 degrees takes as long as one from 40 to 80)
  pose.fovY = std::exp(Spring(std::log(pose.fovY), std::log(wanted.fovY), &fovSpeed_, zoomSeconds, dt));
  pose.roll = Spring(pose.roll, wanted.roll, &rollSpeed_, turnSeconds, dt);  // (photomode)
  if (ground) pose.eye.y = std::max(pose.eye.y, ground(pose.eye.x, pose.eye.z) + clearance);
  if (ground) flyHeight_ = std::max(pose.eye.y - ground(pose.eye.x, pose.eye.z), 0.0f);  // (photomode: Fly's speed)
  // (the angles kept near each other's numbers, both moved together by whole turns)
  const float turns = std::round(pose.yaw / (2 * kPi)) * 2 * kPi;
  pose.yaw -= turns;
  wanted.yaw -= turns;
}

Camera CineCamera::MakeView(const CinePose& pose, const GroundFn& ground) {
  Camera c;
  c.cinematic = true;
  c.heading = pose.yaw;
  c.pitch = Clamp(pose.pitch, -1.55f, kPi / 2);
  c.fovY = Clamp(pose.fovY, 0.02f, 2.6f);
  c.roll = pose.roll;  // (photomode)
  c.zNear = 0.15f;  // (an eye a little over the grass sees it close by)
  Vec3 right, up, back;
  c.Axes(&right, &up, &back);
  float ahead = 120;
  if (Meets(pose.eye, back * -1.0f, 120, ground, &ahead)) ahead = std::clamp(ahead, 2.0f, 120.0f);
  c.distance = ahead;
  c.target = pose.eye - back * ahead;
  return c;
}

CinePose CinePath::At(float time) const {
  if (keys.empty()) return {};
  if (keys.size() == 1 || time <= keys.front().time) return keys.front().pose;
  if (time >= keys.back().time) return keys.back().pose;
  size_t i = 0;
  while (i + 2 < keys.size() && time > keys[i + 1].time) ++i;
  const Key &a = keys[i], &b = keys[i + 1];
  const float span = std::max(b.time - a.time, 1e-4f), s = Clamp((time - a.time) / span, 0, 1);
  // Hermite between the two keys, each key's slope the change from the key before it to the key after it over
  // their time (none at the first and the last: the flight starts and ends gently), so that the speed is even
  // through every key whatever the times between them
  auto slope = [&](size_t k, auto value) {
    if (k == 0 || k + 1 >= keys.size()) return 0.0f;
    return (value(keys[k + 1]) - value(keys[k - 1])) / std::max(keys[k + 1].time - keys[k - 1].time, 1e-4f);
  };
  const float s2 = s * s, s3 = s2 * s;
  const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
  auto blend = [&](auto value) {
    return h00 * value(a) + h10 * span * slope(i, value) + h01 * value(b) + h11 * span * slope(i + 1, value);
  };
  CinePose p;
  p.eye.x = blend([](const Key& k) { return k.pose.eye.x; });
  p.eye.y = blend([](const Key& k) { return k.pose.eye.y; });
  p.eye.z = blend([](const Key& k) { return k.pose.eye.z; });
  p.yaw = blend([](const Key& k) { return k.pose.yaw; });  // (Parse keeps each key's yaw near the one before)
  p.pitch = blend([](const Key& k) { return k.pose.pitch; });
  p.fovY = std::exp(blend([](const Key& k) { return std::log(std::max(k.pose.fovY, 1e-3f)); }));
  return p;
}

bool CinePath::Parse(const std::string& text, const GroundFn& ground, std::string* error) {
  keys.clear();
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    const std::string one = text.substr(at, end - at);
    at = end + 1;
    if (one.find_first_not_of(" \t") == std::string::npos) continue;
    Key k;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, fov = 40;
    if (sscanf(one.c_str(), "%f:%f,%f,%f,%f,%f,%f", &k.time, &x, &y, &z, &yaw, &pitch, &fov) < 6) {
      if (error) *error = "a key \"" + one + "\" is not t:x,y,z,yaw,pitch[,fov]";
      return false;
    }
    k.pose.eye = {x, (ground ? ground(x, z) : 0) + y, z};
    k.pose.yaw = yaw * kPi / 180;
    k.pose.pitch = pitch * kPi / 180;
    k.pose.fovY = Clamp(fov, 1, 150) * kPi / 180;
    if (!keys.empty()) {
      if (k.time <= keys.back().time) {
        if (error) *error = "the keys' times must grow";
        return false;
      }
      k.pose.yaw = Near(k.pose.yaw, keys.back().pose.yaw);
    }
    keys.push_back(k);
  }
  if (keys.empty() && error) *error = "no keys";
  return !keys.empty();
}

}  // namespace render
