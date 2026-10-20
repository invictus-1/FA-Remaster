// Range rings as the original draws them: all the rings of a group (one of the game's range
// filters: direct fire, radar, ...) show as ONE outline round the ground they cover together. A
// ring that lies inside the ground the others cover does not show; a ring's inner radius (a
// weapon's least range) cuts a hole with an outline of its own. The line's thickness is given in
// world units for the nearest and for the farthest zoom and goes between them with the camera's
// distance; a group can glow.
//
// How: per group, every ring is drawn twice into a picture of the screen, found on the ground by
// the scene's depth (on the water's surface where the ground is under water): once whole, once
// smaller by the line's thickness. Where the whole rings are and the smaller ones are not is the
// outline of all of them together. The groups' outlines are gathered in a second picture, and
// that is laid over the scene: over the land before anything stands on it, over the water after
// the water is drawn.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct RangeRing {
  Vec3 centre;
  float radius = 1;
  float innerRadius = 0;  // 0: none
};

struct RangeGroup {
  uint32_t color = 0xffffffffu;  // 0xAARRGGBB as the screen shows it; the first byte is not used here
  int glow = 0;                  // 0 (none) .. 255
  // The line's thickness in world units at the nearest and at the farthest zoom: of the outline
  // round the group, and of the outline round the holes that inner radii cut.
  float outerNear = 0.04f, outerFar = 4.0f;
  float innerNear = 0.04f, innerFar = 4.0f;
  // [range fill] The inside of its lines filled white at an alpha of 0.125 (the original's range_Fill, frame.fx's
  // RangeFill): the ground its rings cover less their lines and the holes their inner radii cut (the stencil's 0x80 of
  // the rings' insides, 0x7edc80's first band), blended over the scene before the group's lines are laid; each group's
  // in turn, so where two groups' insides meet both fills show.
  bool fill = false;
  std::vector<RangeRing> rings;
};

class Ranges {
 public:
  explicit Ranges(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);

  // What is drawn; stays until it is changed.
  std::vector<RangeGroup> groups;
  // The camera's distances the "near" and the "far" thickness are meant for.
  float zoomNear = 10, zoomFar = 1000;
  // The heights everything on the ground lies between, and the water's level (far below: no water).
  float low = 0, high = 100, waterLevel = -1e9f;
  float night = 1;  // the dimming of what glows to an eye opened for the night (Lights::night)

  bool Any() const;
  // Makes the picture of the outlines from the scene's depth as it is now (the ground alone).
  // It begins passes of its own: begin the scene's again afterwards.
  void Build(const View& view, const Camera& camera, gfx::Texture sceneDepth);
  // Into a pass that has the scene's colour only: lays the outlines over the land (`onWater`
  // false) or over the water (true). After Build.
  void Lay(const View& view, const Camera& camera, gfx::Texture sceneDepth, bool onWater);

  int drawnRings = 0;

 private:
  void CameraConstants(const View& view, const Camera& camera, float* out) const;
  gfx::Device* device_;
  gfx::Pipeline mask_, gather_, lay_;
  gfx::Buffer cube_;
  gfx::Texture maskTarget_, lines_, linesBefore_;  // (the outlines, and the picture of them one group back)
  int width_ = 0, height_ = 0;
  bool built_ = false;
};

}  // namespace render
