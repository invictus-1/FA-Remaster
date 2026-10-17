// --gfx-check: what the graphics layer (gfx.h) offers beyond drawing, tried on a device and checked by
// reading the results back: compute shaders, storage targets with levels, volumes, and passes reading what
// dispatches wrote. For each backend in turn (with --vulkan, the Vulkan one); the machines without Linux or
// a Mac at hand at least see both backends agree here.
#pragma once
#include <functional>

#include "gfx.h"

namespace render {

// Runs the checks over a few frames (pumping the window between them), prints each one's outcome; true
// when all pass.
bool GfxCheck(gfx::Device* device, const std::function<void()>& pump);

}  // namespace render
