// The main menu flight's cost on this card (app/menu_flight.h; GameSession holds one).
#pragma once

namespace moho::ui {

// What the flight costs this card, decided once: the mean of the card's time a frame over the five seconds that
// follow the first ten in which the flight was DRAWN. (Until 2026-11-11 the ten and the five were seconds by the
// clock since it first showed: a screen over the menu or a covered window in seconds 10 to 15 left no frame to count,
// and the check never came: a card too weak for the flight kept it. The review's row D.)
//
// The limit (kLimitMs): the flight gives way to Dusk only where it would hold the menu under 30 frames a second
// (33.3 ms of the card's time a frame). Until 2026-11-13 it was 12 ms, a guess from the renderer's first measure (8 ms
// at 4K, 2026-11-09). In Chris's test of 2026-11-13 his card took 17.6 ms a frame (4K, upscale 0.59, every look on:
// the picture has gained looks since) and the flight left after fifteen seconds for the old background, on a PC on
// which the same picture runs every game. A menu at 57 frames a second is no fault; under 30 the menu's own buttons
// lag, and that is what the fallback is for. A slower card's player has a lower preset, which the flight follows.
struct FlightCost {
  static constexpr double kFrom = 10, kTo = 15;  // seconds of drawn frames: the mean is taken between them
  static constexpr double kLimitMs = 1000.0 / 30;
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
