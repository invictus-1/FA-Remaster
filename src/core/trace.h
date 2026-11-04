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
// The in-game overlay's layer times (docs/profiling.md 7) are in EVERY build, not only the profiler build:
//   MOHO_LAYER_ZONE(Layer, "name")  a MOHO_ZONE that also adds its time to "name"'s sum while the overlay is on
//                                (Layer: Sim, Render, Gpu, Ui or Load, without the trace::Layer:: prefix)
//   MOHO_LAYER_SCOPE(Layer, "name") the overlay's sum only (beside a MOHO_ZONE_C, or where no profiler zone is wanted)
//   trace::Lap lap(trace::Layer::X) the stages of a Lap add to their names' sums the same way (default Layer::Sim)
//   trace::SetLayers(on)          the overlay shown or not; off, a layer zone costs one relaxed load
//   trace::TakeLayers(out)        per name: the time since the last call, its longest single zone, and how many
//   trace::LayerSlot / LayerAdd   a time measured elsewhere (the renderer's GPU frame from its timestamp queries)
// They read the clock only and write nothing but their own counters, so a game is the same with the overlay on or off.
//
// Rules (docs/profiling.md 2): a zone is a scope, so it ends where its scope ends, in the order zones began. Never
// keep a zone open across a Lua error (luaL_error / lua_error long-jump past C++ frames and their destructors do not
// run: a zone opened in a binding that raises an error never ends and the viewer reports the thread's zones as broken).
// Names are string literals; a name made at run time goes in MOHO_ZONE_TEXT.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace moho::trace {

// The overlay's layers (docs/profiling.md 7): what a time belongs to. The sim's are per beat (10 a second), the
// others per displayed frame.
enum class Layer : uint8_t { Sim, Render, Gpu, Ui, Load };

extern std::atomic<bool> g_layersOn;  // (trace.cpp; read through LayersOn)
inline bool LayersOn() { return g_layersOn.load(std::memory_order_relaxed); }
void SetLayers(bool on);  // on: the layer zones start adding; off: they stop (the sums so far stay until taken)

// The slot of a name (a string literal or another pointer that lives as long as the program), made at its first
// use; the same text is the same slot, wherever it is used. Thread-safe. At most kLayerSlots names (a name past
// them gets -1, which every call takes as "nothing").
inline constexpr int kLayerSlots = 96;
int LayerSlot(Layer layer, const char* name);
uint64_t LayerNow();                   // a steady clock, in nanoseconds
void LayerAdd(int slot, uint64_t ns);  // one zone's time; any thread (relaxed atomics)

struct LayerTime {
  Layer layer;
  const char* name;
  double ms;       // the time summed since the previous TakeLayers (a name zoned on several threads at once sums them)
  double maxMs;    // its longest single zone in that time
  uint32_t count;  // how many zones
};
// Every slot made so far, in the order they were made, with its counts since the previous call (then zeroed): a
// slot with count 0 had no zone since. Any thread; meant once per frame on the interface's.
void TakeLayers(std::vector<LayerTime>& out);

// A layer zone's time: its scope.
class LayerScope {
 public:
  explicit LayerScope(int slot) : slot_(LayersOn() ? slot : -1), t0_(slot_ >= 0 ? LayerNow() : 0) {}
  ~LayerScope() {
    if (slot_ >= 0) LayerAdd(slot_, LayerNow() - t0_);
  }
  LayerScope(const LayerScope&) = delete;
  LayerScope& operator=(const LayerScope&) = delete;

 private:
  int slot_;
  uint64_t t0_;
};

// A Lap's stages, timed for the overlay: Next ends the open stage's time (if any) and starts the next.
class LayerStages {
 public:
  explicit LayerStages(Layer layer) : layer_(layer) {}
  ~LayerStages() { End(); }
  LayerStages(const LayerStages&) = delete;
  LayerStages& operator=(const LayerStages&) = delete;
  void Next(const char* name) {
    End();
    if (!LayersOn()) return;
    slot_ = LayerSlot(layer_, name);
    t0_ = LayerNow();
  }
  void End() {
    if (slot_ < 0) return;
    LayerAdd(slot_, LayerNow() - t0_);
    slot_ = -1;
  }

 private:
  Layer layer_;
  int slot_ = -1;
  uint64_t t0_ = 0;
};

}  // namespace moho::trace

#define MOHO_TRACE_CAT2(a, b) a##b
#define MOHO_TRACE_CAT(a, b) MOHO_TRACE_CAT2(a, b)
// (the slot is looked up once per place in the code: a static of the lambda each place makes)
#define MOHO_LAYER_SCOPE(layer, name)                                                    \
  ::moho::trace::LayerScope MOHO_TRACE_CAT(moho_layer_scope_, __LINE__)([] {            \
    static const int slot = ::moho::trace::LayerSlot(::moho::trace::Layer::layer, name); \
    return slot;                                                                         \
  }())

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
#define MOHO_LAYER_ZONE(layer, name) \
  ZoneScopedN(name);                 \
  MOHO_LAYER_SCOPE(layer, name)

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
// The stages also add to the overlay's layer times (LayerStages).
class Lap {
 public:
  explicit Lap(Layer layer = Layer::Sim) : stages_(layer) {}
  ~Lap() { End(); }
  Lap(const Lap&) = delete;
  Lap& operator=(const Lap&) = delete;
  void Next(const char* name) {
    End();
    stages_.Next(name);
    if (!TracyIsConnected) return;
    ctx_ = ___tracy_emit_zone_begin(PlaceFor(name, "", 0), 1);
    open_ = true;
  }
  void End() {
    stages_.End();
    if (!open_) return;
    ___tracy_emit_zone_end(ctx_);
    open_ = false;
  }

 private:
  LayerStages stages_;
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
#define MOHO_LAP(lap, name) (lap).Next(name)
#define MOHO_LAYER_ZONE(layer, name) MOHO_LAYER_SCOPE(layer, name)

namespace moho::trace {
inline constexpr bool kBuild = false;
inline bool Connected() { return false; }
// Without the profiler only the overlay's stage times (LayerStages).
class Lap {
 public:
  explicit Lap(Layer layer = Layer::Sim) : stages_(layer) {}
  Lap(const Lap&) = delete;
  Lap& operator=(const Lap&) = delete;
  void Next(const char* name) { stages_.Next(name); }
  void End() { stages_.End(); }

 private:
  LayerStages stages_;
};
}  // namespace moho::trace

#endif
