// Weather for the computed sky (sky.h): what falls. Rain and snow are drawn as many small streaks
// or flakes in a box of air around what the camera looks at; each one's place follows from its
// number and the time alone, so nothing is kept or moved on the processor, and the box goes along
// with the camera while the drops stay where they are in the world. The box is as large as the
// camera is far, so there is as much to see from near as from far (this is rain as a picture of
// rain, not a count of real drops).
//
// What weather does to surfaces (wet, snow lying) and to the air (fog) is in the lighting every
// shader shares (lighting.h, by the view's `weather` values); the clouds are sky.h's.
#pragma once
#include <string>

#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

class Precipitation {
 public:
  explicit Precipitation(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // Into the scene's pass, over everything (it is tested against the scene's depth). `rain` and
  // `snow`: how hard each falls, 0 (not at all) .. 1.
  void Draw(const View& view, const Camera& camera, float rain, float snow);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
};

}  // namespace render
