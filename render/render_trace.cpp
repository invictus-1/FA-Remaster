#include "render_trace.h"

#if defined(TRACY_ENABLE)

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/trace.h"

namespace render::trace {

namespace {

constexpr uint32_t kColour = 0x3b82f6;  // (the renderer's zones in the timeline: blue)

// This thread's open stage (a frame's stages follow one another without a scope each, as the engine's Lap)
thread_local TracyCZoneCtx stageZone{};
thread_local bool stageOpen = false;

}  // namespace

void Stage(const char* name) {
  if (stageOpen) {
    ___tracy_emit_zone_end(stageZone);
    stageOpen = false;
  }
  if (!name || !TracyIsConnected) return;
  stageZone = ___tracy_emit_zone_begin(moho::trace::PlaceFor(name, "render: ", kColour), 1);
  stageOpen = true;
}

void GpuTimes(const std::vector<std::pair<const char*, double>>& times) {
  if (!TracyIsConnected || times.empty()) return;
  // (a plot is known by its name's address, which must last: one kept string per stage, made the first time)
  static std::mutex mutex;
  static std::unordered_map<const char*, std::string>* names = new std::unordered_map<const char*, std::string>();
  std::lock_guard<std::mutex> lock(mutex);
  double frame = 0;
  for (const auto& t : times) {
    auto it = names->find(t.first);
    if (it == names->end()) it = names->emplace(t.first, std::string("render gpu: ") + t.first).first;
    ___tracy_emit_plot(it->second.c_str(), t.second);
    frame += t.second;
  }
  ___tracy_emit_plot("render gpu: frame", frame);
}

}  // namespace render::trace

#endif
