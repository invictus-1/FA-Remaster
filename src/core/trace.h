// The profiler's marks (docs/profiling.md): one timeline for every layer (sim, AI, Lua, effects, renderer, interface,
// loading), seen in the Tracy viewer (third_party/tracy, Tracy 0.14.1, BSD 3-clause). Every layer marks its code with
// these macros only, never with Tracy's own, so that the code does not depend on Tracy and a build without it has
// nothing of it.
//
// Off by default: the macros are compiled in only by a build with the switch (tools/build_zig.py --tracy, CMake
// -DMOHO64_TRACY=ON; both define TRACY_ENABLE and the settings below for every file). Without it every macro is
// empty and the program is today's, byte for byte in behaviour. With it the marks cost a few ns each while no viewer
// is connected (TRACY_ON_DEMAND: nothing is collected until one connects) and they never read or write game state,
// so a game plays the same with or without them, connected or not (proven on the 16 replays, B1, b8 and B3-16k).
//
//   MOHO_ZONE("name")            a zone for the rest of the enclosing scope (a string literal)
//   MOHO_ZONE_C("name", 0xRRGGBB) the same with a colour
//   MOHO_ZONE_N(var, "name")     a second zone in the same scope (its own variable name)
//   MOHO_ZONE_TEXT(str, len)     text shown with the innermost MOHO_ZONE of this scope (copied; e.g. a file name)
//   MOHO_ZONE_VALUE(u64)         a number shown with it (e.g. a count of items)
//   MOHO_FRAME_START(name) / MOHO_FRAME_END(name)   a named frame: the sim's "beat", the renderer's "frame"
//                                (`name` must be the same pointer each time: a string literal or a static array)
//   MOHO_FRAME_MARK              the end of the program's main frame (one per displayed frame: the window loop)
//   MOHO_PLOT("name", value)     a value over time (counts, memory); a string literal
//   MOHO_MESSAGE(str, len)       a line in the viewer's message list (copied)
//   MOHO_THREAD_NAME("name")     this thread's name in the timeline (copied)
//   MOHO_LAP(lap, "name")        with `moho::trace::Lap lap;`: ends lap's zone and opens "name" (stages that follow
//                                one another without a scope each: the beat's stages, sim.cpp Tick)
//   moho::trace::Connected()     a viewer or capture is connected now (false without the switch)
//
// Rules (docs/profiling.md 2): a zone is a scope, so it ends where its scope ends, in the order zones began. Never
// keep a zone open across a Lua error (luaL_error / lua_error long-jump past C++ frames and their destructors do not
// run: a zone opened in a binding that raises an error never ends and the viewer reports the thread's zones as broken).
// Names are string literals; a name made at run time goes in MOHO_ZONE_TEXT.
#pragma once

#include <cstdint>

#if defined(TRACY_ENABLE)

#if !defined(TRACY_ON_DEMAND)
#error "TRACY_ENABLE without TRACY_ON_DEMAND: build with tools/build_zig.py --tracy or CMake -DMOHO64_TRACY=ON (docs/profiling.md)"
#endif

#include "tracy/Tracy.hpp"
#include "tracy/TracyC.h"

#define MOHO_ZONE(name) ZoneScopedN(name)
#define MOHO_ZONE_C(name, color) ZoneScopedNC(name, color)
#define MOHO_ZONE_N(var, name) ZoneNamedN(var, name, true)
#define MOHO_ZONE_TEXT(str, len) ZoneText(str, len)
#define MOHO_ZONE_VALUE(v) ZoneValue(static_cast<uint64_t>(v))
#define MOHO_FRAME_START(name) FrameMarkStart(name)
#define MOHO_FRAME_END(name) FrameMarkEnd(name)
#define MOHO_FRAME_MARK FrameMark
#define MOHO_PLOT(name, value) TracyPlot(name, static_cast<int64_t>(value))
#define MOHO_PLOT_F(name, value) TracyPlot(name, static_cast<double>(value))
#define MOHO_MESSAGE(str, len) TracyMessage(str, len)
#define MOHO_THREAD_NAME(name) tracy::SetThreadName(name)
#define MOHO_LAP(lap, name) (lap).Next(name)

namespace moho::trace {

inline constexpr bool kBuild = true;  // this is the profiler build
inline bool Connected() { return TracyIsConnected; }

inline constexpr char kBeatFrame[] = "beat";  // the sim's frame (sim.cpp Tick): one per beat

// A name made at run time that lives as long as the program (a pool stage's name): its zone's place, made once per
// name and kept (Tracy reads it whenever it likes); the same text gives the same place, so the viewer's statistics
// count it as one zone wherever it is opened. Thread-safe.
const ___tracy_source_location_data* PlaceFor(const char* name, const char* prefix, uint32_t color);

// Stages that follow one another: Next ends the open zone (if any) and opens the next; the destructor ends the last.
// A stage's zone is one place per name (PlaceFor), so a stage opened at several places of the code (the armies'
// navigators+paths, steering and weapons) is one line in the statistics.
class Lap {
 public:
  Lap() = default;
  ~Lap() { End(); }
  Lap(const Lap&) = delete;
  Lap& operator=(const Lap&) = delete;
  void Next(const char* name) {
    End();
    if (!TracyIsConnected) return;
    ctx_ = ___tracy_emit_zone_begin(PlaceFor(name, "", 0), 1);
    open_ = true;
  }
  void End() {
    if (!open_) return;
    ___tracy_emit_zone_end(ctx_);
    open_ = false;
  }

 private:
  TracyCZoneCtx ctx_{};
  bool open_ = false;
};

}  // namespace moho::trace

#else  // no switch: nothing

#define MOHO_ZONE(name) ((void)0)
#define MOHO_ZONE_C(name, color) ((void)0)
#define MOHO_ZONE_N(var, name) ((void)0)
#define MOHO_ZONE_TEXT(str, len) ((void)0)
#define MOHO_ZONE_VALUE(v) ((void)0)
#define MOHO_FRAME_START(name) ((void)0)
#define MOHO_FRAME_END(name) ((void)0)
#define MOHO_FRAME_MARK ((void)0)
#define MOHO_PLOT(name, value) ((void)0)
#define MOHO_PLOT_F(name, value) ((void)0)
#define MOHO_MESSAGE(str, len) ((void)0)
#define MOHO_THREAD_NAME(name) ((void)0)
#define MOHO_LAP(lap, name) ((void)0)

namespace moho::trace {
inline constexpr bool kBuild = false;
inline bool Connected() { return false; }
struct Lap {
  Lap() {}  // (user-provided: no "unused variable" warning where only MOHO_LAP uses it)
  void End() {}
};
}  // namespace moho::trace

#endif
