// --framegen-check: AMD's FSR 3 frame generation (frame_generation.h) tried without a map or the game's files, on a
// made-up scene whose state at any time is known exactly: two frames drawn a time apart, with their depth and
// motion, and the picture it makes between them compared with the scene drawn at the time halfway
// (framegen_check.cpp). For each backend in turn (with --vulkan, the Vulkan one).
#pragma once
#include <functional>
#include <string>

#include "gfx.h"

namespace render {

// Runs the check, prints its numbers; true when the frame made beats a plain blend of the two frames where things
// move, and gives the frame itself after a reset and for a still scene. `pictures`: a .png path whose name the
// pictures of the results are written beside (empty: none).
bool FramegenCheck(gfx::Device* device, const std::function<void()>& pump, const std::string& pictures);

}  // namespace render
