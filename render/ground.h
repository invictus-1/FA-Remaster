// Where a ray meets the ground of a map (what the mouse points at).
#pragma once
#include <algorithm>

#include "camera.h"
#include "scmap.h"

namespace render {

// Walks along the ray until it is below the ground (or, with `water`, below the water surface),
// then narrows the crossing down. False when the ray leaves the map without touching it.
inline bool HitGround(const MapData& map, const Ray& ray, bool water, Vec3* hit, float maxDistance = 20000) {
  auto surface = [&](Vec3 p) {
    float h = map.Height(p.x, p.z);
    return water && map.water.present ? std::max(h, map.water.elevation) : h;
  };
  float t = 0, previous = 0;
  while (t < maxDistance) {
    Vec3 p = ray.origin + ray.direction * t;
    const bool inside = p.x >= 0 && p.z >= 0 && p.x <= static_cast<float>(map.width) && p.z <= static_cast<float>(map.height);
    if (inside && p.y <= surface(p)) {
      float lo = previous, hi = t;
      for (int i = 0; i < 24; ++i) {
        float mid = 0.5f * (lo + hi);
        Vec3 q = ray.origin + ray.direction * mid;
        (q.y <= surface(q) ? hi : lo) = mid;
      }
      *hit = ray.origin + ray.direction * hi;
      return true;
    }
    previous = t;
    t += std::max(0.5f, t * 0.01f);
  }
  return false;
}

}  // namespace render
