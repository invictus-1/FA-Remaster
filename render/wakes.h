// Wakes: the foam a ship leaves on the water as it goes. Each is one flat shape lying on the water
// behind the ship, as wide as the ship at its stern and wider farther back: two arms of foam along
// its edges and churned water between them close to the stern, fading with distance. It is drawn
// from where the ship is and how fast it goes now, straight back along its heading: a ship that
// turns drags its wake round with it (no record is kept of where it has been).
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class Wakes {
 public:
  explicit Wakes(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  void Begin() { wakes_.clear(); }
  // `stern`: where the wake begins, on the water; `back`: the way it trails (level, length 1);
  // `length`: how far; `halfWidth`: half the ship's width; `strength` 0..1.
  void Add(Vec3 stern, Vec3 back, float length, float halfWidth, float strength);
  // Into the scene's pass, after the water.
  void Draw(const View& view);
  int Count() const { return static_cast<int>(wakes_.size()); }

 private:
  struct Wake {
    float from[4], to[4], side[4];
  };
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  std::vector<Wake> wakes_;
};

}  // namespace render
