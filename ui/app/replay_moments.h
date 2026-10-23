// The replay timeline's moments: a STAND-IN for the engine's scan (ScanReplayMoments, sim/moments.h, not on main
// yet). The shape is the engine's proposal (docs/engine-answers.md "ui 48"), with the Kind list the engine confirmed
// on 2026-10-23 (CommanderDied, ExperimentalFinished, ExperimentalDied, ArmyDefeated, NukeLaunched, NukeLanded). The
// scanner here plays nothing: it hands out a fixed list spread over the replay's length over some twelve seconds, as the
// engine's would fill in while it plays the replay in the background.
//
// TO SWAP FOR THE ENGINE'S: in ui/app/game_session.cpp include "sim/moments.h" in place of this header, delete this
// file and replay_moments.cpp, and fill the engine's ScanArgs in GameSession::StartMomentScan (the one place that
// makes them). The names below are the engine's, in the namespace moho::ui, so that the session's code finds the
// engine's own (namespace moho) once this file is gone. KindName stays the session's (game_session.cpp would then
// keep it).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sim/entity.h"  // (Vec3)
#include "sim/replay.h"

namespace moho::ui {

struct Moment {
  enum class Kind { CommanderDied, ExperimentalFinished, ExperimentalDied, ArmyDefeated, NukeLaunched, NukeLanded };
  int tick = 0;   // the beat it happened in
  Kind kind = Kind::CommanderDied;
  int army = -1;  // the army it belongs to, 0-based as the simulation counts them (INFERRED: the proposal does not say)
  Vec3 pos;       // where
  std::string unit;  // the unit's blueprint id ("url0402"); empty for ArmyDefeated
};
using Kind = Moment::Kind;

// How much fighting an army did in 5 s (50 beats) from tick0 on (the engine: summed from the same per-beat hit,
// impact and health lists as render 36).
struct FightBucket {
  int tick0 = 0;
  float damage = 0;
  int hits = 0;
  int deaths = 0;
};

// What the scan fills while the interface reads it: so it is behind a lock. (The proposal's `Moments* out` says
// nothing of how the interface reads it during the scan: a question for the engine, see the ui's report.)
struct Moments {
  mutable std::mutex lock;
  std::vector<Moment> moments;                  // in the order found (by tick)
  std::vector<std::vector<FightBucket>> fight;  // [army][bucket], every army's buckets in order
};

// What the scan needs. The engine's own will hold what a copy of the game needs to run (as --together builds one);
// the stand-in only needs the length and the armies.
struct ScanArgs {
  ReplayHeader header;   // the replay's header (its armies: the players are those with a command source)
  std::string file;      // the replay file (a host path)
  uint32_t ticks = 0;    // the replay's length in beats (its stream's Advances)
};

// Fills `out` as the replay is scanned; `tick` the beat reached (the interface shows how far it got); stops early,
// returning false, when `cancel` is set. True when it went through.
bool ScanReplayMoments(const ScanArgs& args, Moments* out, std::atomic<int>* tick, const std::atomic<bool>* cancel);

}  // namespace moho::ui
