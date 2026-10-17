// --fsr3-check: AMD's FSR 3 upscaler (fsr3.h) tried without a map or the game's files, on a made-up scene
// drawn at a smaller size with its jitter, depth and motion, and checked against the same scene drawn at
// the display's size (fsr3_check.cpp). For each backend in turn (with --vulkan, the Vulkan one).
#pragma once
#include <functional>
#include <string>

#include "gfx.h"

namespace render {

// Runs the check, prints its numbers; true when FSR 3 beats a plain stretch where nothing moves and does
// not smear what moves. `pictures`: a .png path whose name the pictures of the results are written beside
// (empty: none).
bool Fsr3Check(gfx::Device* device, const std::function<void()>& pump, const std::string& pictures);

}  // namespace render
