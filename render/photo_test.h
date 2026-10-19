// (photomode) The viewer's --photo-test (with --picture and --sim or --session): the photo mode's pieces through the
// pixel interface, as the interface's screen will use them, without it: the free camera flown by a script of inputs
// (CineCamera::Fly), moments of the picture's own (SetPhotoMoment), focus by a pixel (FocusAt), a shallow-focus photo,
// and photos of 1, 2 and 4 times the window's size at the sizes asked for, timed (StartPhoto, PhotoProgress,
// TakeDonePhoto); pictures and numbers written beside the --picture file. With --picture-window: also the window kept
// drawn while a photo is made (DrawToWindow, a made-up interface with the photo's progress).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"

namespace render {

class WorldPicture;

struct PhotoTestSetup {
  std::string base;                   // pictures go to <base>_*.png
  int width = 1280, height = 720;     // the window's size for the flight, the moments and the focus
  std::vector<std::pair<int, int>> sizes;  // the window sizes photos are timed at
  bool window = false;                // the picture is drawn into a window (DrawToWindow): its check too
  std::vector<float> units;           // where the game's units are (x, y, z each), to focus on and look at
};

void PhotoTest(WorldPicture& picture, const Camera& gameCamera, double tick, const PhotoTestSetup& setup);

}  // namespace render
