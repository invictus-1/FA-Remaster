// The game camera: it looks at a point on the ground from a distance, with a heading and a pitch.
// Zooming changes the distance; at 90 degrees of pitch it looks straight down (the strategic view).
#pragma once
#include "linalg.h"

namespace render {

struct Ray {
  Vec3 origin, direction;
};

struct Camera {
  Vec3 target;               // the point looked at
  float distance = 60;       // eye to target
  float heading = 0;         // radians; 0: north is up on the screen
  float pitch = 1.0f;        // radians above the horizon; pi/2 looks straight down
  float fovY = 0.9f;         // vertical field of view, radians
  float zNear = 0.25f;
  // A cinematic camera's (cinecam.h, CineCamera::View): free in the world, low down and toward the horizon. The
  // target is then a point ahead on the line of sight (what lies at the picture's middle, near enough), and what
  // is placed round what the camera looks at is placed for such a view: the sun's shadow maps along the view out
  // to the distance, the ground cover ahead of the eye. False: the game's camera, as at first.
  bool cinematic = false;

  // right, up and back (pointing from the target to the eye), unit length
  void Axes(Vec3* right, Vec3* up, Vec3* back) const {
    float sh = std::sin(heading), ch = std::cos(heading), sp = std::sin(pitch), cp = std::cos(pitch);
    *back = {sh * cp, sp, ch * cp};
    *right = {ch, 0, -sh};
    *up = Cross(*back, *right);
  }
  Vec3 Eye() const {
    Vec3 r, u, b;
    Axes(&r, &u, &b);
    return target + b * distance;
  }
  Mat4 View() const {
    Vec3 r, u, b;
    Axes(&r, &u, &b);
    return Mat4::View(target + b * distance, r, u, b);
  }
  Mat4 Projection(float aspect) const { return Mat4::Perspective(fovY, aspect, zNear); }

  // The ray through a pixel of a width x height picture.
  Ray PixelRay(float px, float py, float width, float height) const {
    Vec3 r, u, b;
    Axes(&r, &u, &b);
    float t = std::tan(fovY * 0.5f);
    float nx = (2 * px / width - 1) * t * (width / height);
    float ny = (1 - 2 * py / height) * t;
    return {target + b * distance, Normalize(r * nx + u * ny - b)};
  }
};

// Where a ray meets the horizontal plane at height y. False when it points away from it.
inline bool HitPlaneY(const Ray& ray, float y, Vec3* hit) {
  if (std::fabs(ray.direction.y) < 1e-6f) return false;
  float t = (y - ray.origin.y) / ray.direction.y;
  if (t <= 0) return false;
  *hit = ray.origin + ray.direction * t;
  return true;
}

}  // namespace render
