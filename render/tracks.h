// Tracks in lying snow: where units stand and drive, the snow is trodden away, and what falls
// afterwards covers the ground again. Nothing of this is in the original (which has no weather).
//
// One picture laid over the whole map holds how much of the snow is trodden at each place (0 none,
// 1 all). Every frame the units on the ground are stamped into it as soft discs, and while it snows
// the whole picture fades back toward untrodden. The ground's shader (lighting.h, SURFACE_TRACKS)
// reads it where it lays the snow.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"

namespace render {

class Tracks {
 public:
  explicit Tracks(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // The map the picture lies over (world units); makes the picture, untrodden.
  void SetMap(float width, float height);
  // Empties this frame's list of what treads.
  void Begin() {
    stamps_.clear();
    ++frame_;
  }
  // Something stands or moves on the ground there, treading a disc of that radius. `id` says which
  // thing it is (a unit's id): the way it has come since the frame before is trodden too, so a fast
  // unit leaves a track and not a row of dots.
  void Add(uint32_t id, Vec3 position, float radius);
  // Draws the frame's stamps into the picture, and lets `covering` (0..1) of the trodden snow lie
  // again (new snowfall); `clear`: all of it at once (no snow lies any more). Its own pass: call
  // before the scene's.
  void Draw(float covering, bool clear);
  // The picture (none before SetMap), and what a place's x and z are multiplied by to read it.
  gfx::Texture Map() const { return map_; }
  float InverseWidth() const { return width_ > 0 ? 1 / width_ : 0; }
  float InverseHeight() const { return height_ > 0 ? 1 / height_ : 0; }

  bool on = true;  // false: nothing is trodden (a player's choice, or a host that wants plain snow)

 private:
  struct Vertex {
    float x, y;        // in the picture, -1..1
    float u, v;        // across the disc, -1..1 (0, 0 all over: the whole of it)
    float value, alpha;  // what is written (1 trodden, 0 untrodden) and how much of it
  };
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  gfx::Buffer buffer_;
  gfx::Texture map_;
  float width_ = 0, height_ = 0;
  bool trodden_ = false;  // anything in the picture at all
  bool fresh_ = false;    // just made: to be cleared when first drawn into
  struct Stamp {
    float x, z, radius;
  };
  std::vector<Stamp> stamps_;
  struct Last {
    float x, z;
    uint32_t frame;
  };
  std::unordered_map<uint32_t, Last> last_;  // where each thing was when it last trod
  uint32_t frame_ = 0;
  std::vector<Vertex> vertices_;
};

}  // namespace render
