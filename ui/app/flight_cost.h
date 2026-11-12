// The main menu flight's cost on this card (app/menu_flight.h; GameSession holds one).
#pragma once

namespace moho::ui {

// What the flight costs this card, decided once: the mean of the card's time a frame over the five seconds that
// follow the first ten in which the flight was DRAWN. (Until 2026-11-11 the ten and the five were seconds by the
// clock since it first showed: a screen over the menu or a covered window in seconds 10 to 15 left no frame to count,
// and the check never came: a card too weak for the flight kept it. The review's row D.)
struct FlightCost {
  static constexpr double kFrom = 10, kTo = 15;  // seconds of drawn frames: the mean is taken between them
  double shown = 0;  // the seconds of frames that drew the flight (one frame counts 0.25 s at most: a hitch is no time shown)
  double gpuMs = 0;  // the card's time of the frames counted
  int frames = 0;
  bool decided = false;
  // One frame that drew the flight, `dt` seconds after the frame before, the card's time of it. True once: the mean
  // is known (*mean, ms a frame).
  bool Frame(double dt, double frameGpuMs, double* mean) {
    if (decided) return false;
    const double before = shown;
    shown += dt < 0 ? 0 : dt > 0.25 ? 0.25 : dt;
    if (before >= kFrom && before < kTo) gpuMs += frameGpuMs, ++frames;
    if (shown < kTo || frames == 0) return false;
    decided = true;
    *mean = gpuMs / frames;
    return true;
  }
};

}  // namespace moho::ui
