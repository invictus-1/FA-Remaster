// [tracy] The renderer's marks in the profiler (docs/profiling.md; src/core/trace.h is the engine's and every layer's).
// Compiled in only by the profiler build (render/build.py --tracy, which defines TRACY_ENABLE for every file); without
// it every function here is empty and inline, and the program is today's.
//
// - The frame's stages on the processor: CpuTimes::Part (cpu_times.h) names each stage of a frame as it begins; each
//   stage is also a zone "render: <stage>" in Tracy's timeline, on the thread that ran it, whether --timings is on or not.
// - The frame's stages on the card: Device::Mark names them (gfx.h); each backend, as it takes a finished frame's times,
//   hands them here, and each stage is a plot "render gpu: <stage>" (milliseconds), with the card's whole frame as
//   "render gpu: frame". (Tracy's own GPU zones need the card's timestamps on the processor's clock in both backends:
//   a later step.)
// Nothing here reads or writes game state; nothing is done while no viewer is connected.
#pragma once

#include <utility>
#include <vector>

namespace render::trace {

#if defined(TRACY_ENABLE)
// The stage `name` (a string literal; nullptr: none) begins on this thread: the one before it ends.
void Stage(const char* name);
// A finished frame's stages on the card (name, ms), in their order.
void GpuTimes(const std::vector<std::pair<const char*, double>>& times);
#else
inline void Stage(const char*) {}
inline void GpuTimes(const std::vector<std::pair<const char*, double>>&) {}
#endif

}  // namespace render::trace
