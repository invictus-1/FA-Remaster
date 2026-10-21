// Wakes: the foam a ship leaves on the water as it goes. Each is one flat shape lying on the water
// behind the ship, as wide as the ship at its stern and wider farther back: two arms of foam along
// its edges and churned water between them close to the stern, fading with distance. It is laid
// in pieces along the places the ship has been (AddPath), so it curves behind a ship that turns.
//
// [wakes] The remade wake (World::wakesRemade, the modern look; Chris's test run of 21 October: on the large mod
// battleship the wake showed as two broad, blotchy white bands cut off square at their end, "white waves but very low
// res"): the same shape drawn so that it holds for a ship of any size and speed. Its arms are foam lines of a width
// in world units (not a share of the wake's width, which made broad bands of a wide hull's), its foam streaks lie
// along the way the ship went and stay where they were left on the water (by how far the ship had gone, the
// `odometer`), finer streaks are let fade where they would be finer than about two pixels (no blocks at any zoom),
// it is laid along the way the ship's middle went (a long hull's stern swings as it steers, which kinked the wake
// laid along the stern's places into folds with hard edges), and it runs out softly however short the way kept
// behind the ship is (it was cut off square where twelve places a third of a second apart did not reach its end).
// Without it, or in the original look, the wake is drawn as before, pixel for pixel.
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
  // `remade` [wakes]: this frame's wakes are the remade ones (World::wakesRemade in the modern look).
  void Begin(bool remade = false) {
    wakes_.clear();
    remade_ = remade;
  }
  // `stern`: where the wake begins, on the water; `back`: the way it trails (level, length 1);
  // `length`: how far; `halfWidth`: half the ship's width; `strength` 0..1. [wakes] `odometer`: how far the ship has
  // gone in all (world units; the remade wake's foam keeps its place on the water by it).
  void Add(Vec3 stern, Vec3 back, float length, float halfWidth, float strength, float odometer = 0);
  // The same along a way: `places` from the stern backward (the newest first), on the water; the
  // wake runs along them for at most `length`.
  void AddPath(const Vec3* places, int count, float length, float halfWidth, float strength, float odometer = 0);
  // [wakes] The remade wake's: `places` where the ship's MIDDLE has been (the newest first: where it is now), so that a
  // long hull's swinging stern (its heading changes as it steers) puts no kinks in it; the wake begins `skip` along that
  // way (under the stern) and runs `length` on, and where the way kept is shorter it runs on straight the way the last
  // piece went (`back`: the way it trails when nothing is kept yet), so that it fades out and is never cut off square.
  // `odometer`: how far the ship has gone in all, at places[0].
  void AddPathRemade(const Vec3* places, int count, float skip, Vec3 back, float length, float halfWidth, float strength,
                     float odometer);
  // Into the scene's pass, after the water.
  void Draw(const View& view);
  int Count() const { return static_cast<int>(wakes_.size()); }

 private:
  struct Wake {
    float from[4], to[4], sideFrom[4], sideTo[4], range[4];
  };
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  gfx::Pipeline remadePipeline_;  // [wakes]
  bool remade_ = false;           // [wakes]
  bool remadeTried_ = false;      // [wakes] (its pipeline asked for once)
  std::vector<Wake> wakes_;
};

}  // namespace render
