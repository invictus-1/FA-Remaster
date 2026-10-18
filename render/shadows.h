// The sun's shadows: a few shadow maps ("cascades"), each a picture of the scene's depth as the sun
// sees it, covering a larger piece of ground than the one before at the same resolution. Near the
// place the camera looks at, shadows are sharp; far away they are coarser.
#pragma once
#include "gfx.h"
#include "view.h"

namespace render {

inline constexpr int kShadowMapSize = 2048;  // as at first (Shadows::SetSize; lighting.h's shader reads the map's own size)

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
  // The maps' width and height in texels (2048 at first). Larger is sharper and costs more of the
  // graphics card; a setting for slower cards (512 to 4096).
  void SetSize(int size);
  int Size() const { return size_; }
  // The pass to begin for a cascade.
  gfx::PassDesc Pass(int i) const;
  // A cinematic camera's cascades (Camera::cinematic; AlongView): how far from the eye the shadows reach (world
  // units), and how the slices' ends are spaced (0 evenly .. 1 by a constant ratio: sharper near, coarser far).
  float cinematicReach = 1000;
  float cinematicSplit = 0.9f;

 private:
  void AlongView(const Camera& camera, const View& view, Vec3* centres, float* radii);
  gfx::Device* device_;
  gfx::Texture map_;
  int size_ = 0;
  int count_ = 3;
  View cascadeViews_[kMaxCascades];
};

}  // namespace render
