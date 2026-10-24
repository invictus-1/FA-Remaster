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
  // [cardcost] Casters fitted to the view (World::shadowFit, viewer --no-shadow-fit): a cascade draws only what can cast a
  // shadow onto something the camera sees. Everything that reads the maps reads them for a place in the camera's view
  // (the ground and what stands on it, the air, the water, a puff's corner: a little outside at most), and nothing lies
  // lower than the ground's lowest point. So what the camera can see lies inside its view between the eye and that
  // lowest height (`fitFloor`), and a caster matters only where its shadow, cast away from the sun, can fall in there:
  // seen from the sun, where that piece of the view lies (its box in the sun's two sideways axes, `fitMargin` world
  // units wider all round). Each cascade's four sides that cull are drawn in to that box (its picture, its place and
  // its depth stay as they are), so the ground's patches and the models are tested against it as they are against the
  // cascade's square. A caster left out casts its shadow only where nothing seen reads it: the same picture. Where the
  // view reaches the horizon (a corner of it looking level or up) nothing is drawn in.
  bool fitCasters = false;
  float fitFloor = -1e9f;
  float fitMargin = 64;
  bool Fitted() const { return fitted_; }  // (whether this frame's cascades were drawn in)
  // [cardcost] The far cascades kept (World::shadowKeep, viewer --shadow-keep <n>): `keepEvery` 2 or more, each cascade
  // past the nearest is drawn anew only every keepEvery-th frame, in turn (the nearest every frame), and in between its
  // map is kept with the place it was drawn for, as long as it still serves: the same size, the sun within
  // keepSunDegrees of where it was, its own square and the nearer one's moved by at most keepSlack of their radius (each
  // cascade then leaves out what the nearer one covers less that slack, and with fitCasters is drawn for that much more
  // round what the camera sees). Ground and scenery stand still, so their shadows are the same; a unit moving in a far
  // cascade casts where it stood when that was drawn (a frame or so before at 2). 0: every cascade every frame. Set
  // before Place; for each cascade Draws(i) says whether to draw it this frame, and Drawn(i) once it is drawn.
  int keepEvery = 0;
  float keepSlack = 0.02f;
  float keepSunDegrees = 0.02f;
  bool Draws(int i) const { return i < 0 || i >= kMaxCascades || draws_[i]; }
  void Drawn(int i);
  void Forget() {  // (nothing kept: every cascade drawn the next frame; a map just opened)
    for (Kept& k : kept_) k.valid = false;
  }

 private:
  void AlongView(const Camera& camera, const View& view, Vec3* centres, float* radii);
  gfx::Device* device_;
  gfx::Texture map_;
  int size_ = 0;
  int count_ = 3;
  View cascadeViews_[kMaxCascades];
  bool fitted_ = false;  // [cardcost]
  // [cardcost] (keepEvery) a far cascade's map as drawn: what it was drawn for (pending_: drawn this frame, kept_ once
  // Drawn says so)
  struct Kept {
    bool valid = false;
    View view;
    Mat4 matrix;
    Vec3 sun;
    float radius = 0, x = 0, y = 0;
    float nearerX = 0, nearerY = 0, nearerHalf = 0;
    bool fitted = false;
    float seen[4] = {0, 0, 0, 0};
    int count = 0, size = 0;
  };
  Kept kept_[kMaxCascades], pending_[kMaxCascades];
  bool draws_[kMaxCascades] = {true, true, true, true};
  bool keeping_ = false;
  uint64_t keepFrame_ = 0;
};

}  // namespace render
