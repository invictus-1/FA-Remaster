// The fog of war's look: what the army the picture is for does not see now is darkened. What it
// sees comes as a square grid over the map (seen now, seen at some time, never; SimFrames makes it
// from the engine's ReconAt); the picture is multiplied, pixel by pixel, by how much of the place
// under the pixel is seen, found from the scene's depth. So everything there darkens alike: ground,
// water, wrecks. A place never seen can be darker still (`neverSeen`).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"
#include "frame.h"
#include "gfx.h"
#include "view.h"

namespace render {

class SightShade {
 public:
  explicit SightShade(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // What is seen: `cells` x `cells` values over the whole map, row by row from z = 0, each the
  // bits of SightBits (frame.h). An empty grid: nothing is darkened.
  void Set(const std::vector<uint8_t>& grid, int cells);
  bool On() const { return static_cast<bool>(texture_); }
  // Of a place of the map (x and z as shares of its width and height, 0..1): is it seen now, and
  // has it been seen at some time. With no grid, and outside the map: yes.
  bool SeenAt(float u, float v) const { return (At(u, v) & kSightNow) != 0; }
  bool ExploredAt(float u, float v) const { return (At(u, v) & kSightEver) != 0; }
  // Into a pass that has the scene's colour only (the depth is read): darkens what is not seen.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, float mapWidth, float mapHeight);
  // The same kind of pass for what lies outside `rect` (x0, z0, x1, z1 in world units: the part of the
  // map that can be played now): black, as the original's boundary pass makes everything there (on the
  // map; its frame beyond the edge is left); `dimmed`: a little of its light kept instead, a shade
  // toward blue, behind a dark line along the edge that keeps its width in the picture at any zoom,
  // and the world beyond the map's edge with it (the modern look's); [groundfix] with World::beyondLand on (the view's
  // beyondLand set), the softer edge of the map's own instead: dimmed little by little, a soft shadow for the line.
  void DrawBoundary(const View& view, const Camera& camera, gfx::Texture sceneDepth, const float rect[4], float mapWidth, float mapHeight, bool dimmed);
  float outsideKeeps = 0.45f;  // `dimmed`: how much of its light what lies outside keeps

  float darkness = 0.5f;  // how much of the light an unseen place keeps
  // How much a place never seen keeps: the same, when the game's map is known from the start
  // (the game's option FogOfWar "explored", the usual); next to nothing when it is not ("unexplored").
  float neverSeen = 0.5f;
  // How much an unseen place keeps while radar or omni covers it (on water, sonar too): a little
  // more, so that the cover shows on the ground. `waterLevel`: below it a place is water.
  float covered = 0.7f;
  float waterLevel = -1e9f;

 private:
  uint8_t At(float u, float v) const {
    if (grid_.empty() || u < 0 || v < 0 || u >= 1 || v >= 1) return 255;
    return grid_[static_cast<size_t>(v * static_cast<float>(cells_)) * static_cast<size_t>(cells_) + static_cast<size_t>(u * static_cast<float>(cells_))];
  }
  gfx::Device* device_;
  gfx::Pipeline pipeline_, boundary_;
  gfx::Texture texture_;
  std::vector<uint8_t> pixels_, grid_;
  int cells_ = 0;
};

}  // namespace render
