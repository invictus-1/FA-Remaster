// A tick recording (the engine's MOHO64_RECORD file, or the same format written inside the original
// game) as a source of frames: what every unit's pose was at every tick.
//
// The format is described at the top of src/sim/recorder.cpp. The engine's recordings say which
// blueprint each unit is ('U' records). Older ones, and the original game's, do not: for those it
// comes from the engine's log of the same run made with MOHO64_DEBUG_CREATE=1
// ("create <tick> unit <id> bp <blueprint> ...").
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"

namespace render {

class Recording {
 public:
  bool Load(const std::string& hostPath, std::string* error);
  // How many units the recording names itself ('U' records; the engine writes them since 2026-10-12).
  int NamedUnits() const { return named_; }
  // For recordings without them: reads the "create" lines of an engine log made with
  // MOHO64_DEBUG_CREATE=1. Returns how many it found; does nothing when the recording has names.
  int LoadCreateLog(const std::string& hostPath);

  int TickCount() const { return static_cast<int>(ticks_.size()); }
  uint32_t FirstTick() const { return ticks_.empty() ? 0 : ticks_.front().tick; }
  uint32_t LastTick() const { return ticks_.empty() ? 0 : ticks_.back().tick; }
  // The frame at a moment given in ticks (10 per second), blended between the two recorded ticks
  // around it. Units are matched by id; one that exists in only one of the two is taken as it is.
  void FrameAt(double tick, Frame* out) const;
  const std::vector<std::string>& Blueprints() const { return blueprints_; }

 private:
  struct Tick {
    uint32_t tick = 0;
    size_t first = 0, count = 0;  // into units_
  };
  struct Created {
    uint32_t tick;
    uint32_t blueprint;
  };
  uint32_t BlueprintOf(uint32_t id, uint32_t tick) const;
  uint32_t BlueprintIndex(const std::string& name);
  void ResolveBlueprints();
  int named_ = 0;

  std::vector<Tick> ticks_;
  std::vector<FrameUnit> units_;  // all ticks, one after the other
  std::vector<std::string> blueprints_{""};  // index 0: unknown
  std::unordered_map<std::string, uint32_t> blueprintIndex_;
  std::unordered_map<uint32_t, std::vector<Created>> created_;  // by unit id, in tick order (ids are reused)
};

}  // namespace render
