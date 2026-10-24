// [views] The viewer's --views (with --picture and --sim or --session): several views in one picture through the pixel
// interface (WorldPicture::SetViews, views.h), as the interface will set them: "split" (the window in two halves, the
// left the game's camera, the right a second camera over another part of the map) or "inset[:<unit id>]" (the game's
// camera over the whole window and a follow camera behind a unit, 480 x 270 at the bottom right, drawn every 2nd frame;
// without an id the first unit). Pictures beside the --picture file (<base>_views_<mode>.png, and for "split" each
// half's camera drawn alone at the half's size, <base>_views_alone_left.png / _right.png, to compare), the picking
// calls' answers and the frames' times (one view, then the views) printed. With --picture-window: drawn into the window
// (DrawToWindow), else read back (Draw).
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "frame.h"

namespace render {

class WorldPicture;

struct ViewsTestSetup {
  std::string base;            // pictures go to <base>_views_*.png
  std::string mode;            // "split", "inset", "inset:<unit id>"
  int width = 1280, height = 720;
  bool window = false;         // drawn into the window (DrawToWindow)
  int frames = 120;            // timed frames each way
};

void ViewsTest(WorldPicture& picture, const Camera& gameCamera, double tick, const Frame& frame, const ViewsTestSetup& setup);

}  // namespace render
