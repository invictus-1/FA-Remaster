// [views] Several views in one picture (the interface's request 35; docs/render-answers.md, "To the interface, request
// 35"): a split screen (both halves full views, the original's and FAF's worldview viewRight) and a picture-in-picture
// follow camera, drawn by ONE world (WorldPicture::SetViews). A second WorldPicture would move the world's effects,
// particles, weather and lights on twice a frame and keep two of everything on the card; views of one world share all of
// that, and only what depends on the camera is done for each.
//
// A frame with several views goes (world_picture.cpp, DrawViews):
//   the first view (view 0) as every frame: Begin, the game's frame added (AddFrame), Render - into the window where it
//     fills it, else into a picture of its own (gfx Device::SetViewWindow);
//   each other view: its own parts swapped into the world (World::SwapView: its pictures at its size, what it keeps of
//     its frames before), the same moment begun again from its camera with World::extraView (nothing of the world moves
//     on a second time), the game's frame added again, rendered into its own picture, its parts swapped out again;
//   every picture of a view that does not fill the window laid into the window at its place (ViewLayer), the interface
//     over all of it after.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "air.h"
#include "ambient_occlusion.h"
#include "bounce.h"
#include "camera.h"
#include "clouds.h"
#include "fsr3.h"
#include "gfx.h"
#include "hidden_outlines.h"
#include "mesh_renderer.h"
#include "motion.h"
#include "post.h"
#include "ranges.h"
#include "upscale.h"
#include "view.h"
#include "world.h"

namespace render {

// What one view of a picture with several has of its own: the world's parts that hold pictures at a view's size or keep
// something of a view's frames before, and the world's numbers that go with them (World::SwapView swaps them all with
// the world's). The view's picture and depth it is drawn into (the window's place, SetViewWindow) are here too.
struct ViewSide {
  explicit ViewSide(gfx::Device* device);
  ~ViewSide();
  ViewSide(const ViewSide&) = delete;
  ViewSide& operator=(const ViewSide&) = delete;

  gfx::Device* device;
  // its picture (RGBA8, as the window's) and depth (D32), `width` x `height`; made by Size
  gfx::Texture color, depth;
  int width = 0, height = 0;
  bool Size(int w, int h);
  bool drawn = false;  // a picture has been drawn into it (an inset drawn every few frames keeps the last)

  // the world's parts, a view's own (SwapView methods: only their pictures and what they keep; pipelines and settings stay)
  Post post;
  AmbientOcclusion ao;
  Upscaler upscaler;
  Ranges ranges;
  HiddenOutlines outlines;
  Fsr3 fsr3;
  Motion motion;
  Air air;
  Clouds clouds;
  BounceLight bounce;
  SunMask sunMask;  // [shadowmask]
  MeshRenderer::Before before;

  // the world's numbers of a view (World's private members of those names)
  Camera camera;
  View view, screenView;
  int screenWidth = 1, screenHeight = 1;
  std::vector<World::BarAnchor> barAnchors;
  bool temporalNow = false, temporalBefore = false, temporalCut = false;
  int fsr3State = 0, temporalFrame = 0;
  Vec3 lastEye{};
  bool lensSnap = false;
  bool framegenNow = false, framegenBefore = false, framegenCut = false;
  bool bounceTried = false, bounceMade = false, bounceNow = false;
  int lampUnits = 0, lampUnitsBefore = 0;
};

// Lays a view's picture into the window at its place (in the window's pass, after the views are drawn).
class ViewLayer {
 public:
  explicit ViewLayer(gfx::Device* device) : device_(device) {}
  ~ViewLayer();
  bool Init(std::string* error);
  // `picture` (`x`, `y` its top left in the window's pixels, `width` x `height`) into the window of `windowWidth` x
  // `windowHeight`, pixel for pixel, in the window's pass under way.
  void Lay(gfx::Texture picture, int x, int y, int width, int height, int windowWidth, int windowHeight);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
};

}  // namespace render
