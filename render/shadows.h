// The sun's shadows: a few shadow maps ("cascades"), each a picture of the scene's depth as the sun
// sees it, covering a larger piece of ground than the one before at the same resolution. Near the
// place the camera looks at, shadows are sharp; far away they are coarser.
#pragma once
#include "gfx.h"
#include "view.h"

namespace render {

inline constexpr int kShadowMapSize = 2048;  // lighting.h's shader steps by 1 / this

class Shadows {
 public:
  explicit Shadows(gfx::Device* device);

  // Places the cascades for this frame's camera and writes them into `view` (which the scene's
  // shaders then read). `sunDirection` points toward the sun.
  void Place(const Camera& camera, Vec3 sunDirection, View* view);
  int Count() const { return count_; }
  // The view a cascade's depth pass is drawn with: the sun's projection, but levels of detail still
  // measured from the camera, so that the shadow's shape is the shape of what is drawn.
  const View& CascadeView(int i) const { return cascadeViews_[i]; }
  gfx::Texture Map() const { return map_; }
  // The pass to begin for a cascade.
  gfx::PassDesc Pass(int i) const;

 private:
  gfx::Texture map_;
  int count_ = 3;
  View cascadeViews_[kMaxCascades];
};

}  // namespace render
