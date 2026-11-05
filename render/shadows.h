// The sun's shadows: a few shadow maps ("cascades"), each a picture of the scene's depth as the sun
// sees it, covering a larger piece of ground than the one before at the same resolution. Near the
// place the camera looks at, shadows are sharp; far away they are coarser.
#pragma once
#include "gfx.h"
#include "view.h"

namespace render {

class Terrain;  // [shadowtri] (Shadows::ground)

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
  // [shadowtri] Chris's second test (2026-11-04, his overlay early in a game at 4K: "triangles 0.52M + shadows 15.48M",
  // the sun's shadows 1.87 ms of a 19.6 ms frame bound by the card): the sun's maps drew thirty times the triangles the
  // camera's picture did, and nearly all of them were casters the camera does not see, the scenery round the view (the
  // squares of props listed for it reach well past it). Two things made the box they are fitted to far wider than what
  // is seen. (1) Its floor was the whole map's lowest point less 32 (World::Begin): on SETONS_R the sea's bed, some 12
  // world units under the usual ground and so 44 under it with the 32, and every corner of the view was followed down to
  // that depth, which for the far corners is a long way out. Now (`fitLocalFloor`, with `ground` set: the world's
  // Terrain) the floor is the lowest ground under the box those corners reach at the map's floor (Terrain::LowestIn, its
  // nodes' lowest), less `fitFloorSlack`: still exact, as the ground is a height field, so whatever lies lower than the
  // lowest ground under a place is inside the ground there and cannot be seen; the slack is for the ground's relief and
  // the ruts pressed into it, a few tenths of a unit. (2) `fitMargin` was 64 world units all round, where the readers
  // that look outside the view by more than their filter's reach are the lit smoke's puffs (particles.cpp, at 0.6 of the
  // way to a corner), the dust's and the blasts' puffs (at their middles) and the ground cover's card corners: 16 covers a
  // puff 16 units across whose middle is off the picture, and each cascade adds `fitTexels` of its own texels for the
  // filters (lighting.h: the lookup moved 2.5 texels along the normal and toward the sun; the soft shadow's search,
  // 0.8 world units or 1.5 texels, and its spread, 1.6 or 1.2 texels; the sharp one's 3 x 3). By a model of the game's
  // camera on SETONS_R (pitch and field of view by zoom as the interface has them, the sun at 30-60 degrees) the area
  // the cascades draw drops to 15-22 % at the zooms of an early game (the eye 30-70 units away), 34 % at 125 away,
  // 50 % at 250 and 67 % at 500 (where the cascades' squares, not the fit, bound it). Viewer --shadow-fit-margin <units>
  // (64: as before), --no-shadow-fit-floor (the map's floor, as before).
  float fitMargin = 16;
  float fitTexels = 6;
  bool fitLocalFloor = true;
  float fitFloorSlack = 4;
  const Terrain* ground = nullptr;
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
  // [shadowstep] The game camera's cascades sized from a ladder (World::shadowZoomSteps; viewer --shadow-zoom-smooth: off).
  // Chris's second test (2026-11-04, "small foliage flicker: less, still there", his picture of ferns and bushes in the
  // woods' shade): the nearest cascade was max(8, the camera's distance x 0.55) wide, so every frame of a zoom its texel
  // changed size and the whole map of the trees' dappled shade slid under the plants, by a part of a texel a frame. The
  // cards take the sun's shadow at their corners (grass.cpp), so a corner at the shade's edge went lit, dark, lit as the
  // texels went over it; the 3D plants' small leaves shimmered the same way, and so the rest of the shadows, less to be
  // seen on larger things. With it on, the nearest cascade's half width is 8 world units times a power of 2^(1/8), the
  // least that covers what the zoom asks, kept while it is still that or a step larger (as the pads' and the far field's
  // grids, grass.cpp): while the camera zooms the shadows stand still, and change at once at a step (every 9 % of zoom
  // out, every 19 % in); the shadows at most two steps (19 %) coarser than the zoom asks, 9 % on the mean. The cascades
  // beyond it are 3.2 times the one before, as before. A cinematic camera's (AlongView) are as before.
  bool zoomSteps = true;

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
  int zoomStep_ = -1;  // [shadowstep] the nearest cascade's size as a step of its ladder (-1: none yet)
};

}  // namespace render
