// The replay timeline (ours, not the original's: Chris's approval of 2026-10-23): a bar over a replay with its length,
// where it is, the game's moments and how much fighting there was, and jumps to any point. The host keeps the replay
// (app/game_session.cpp: its length, the moments' scan, the jumps); the bar is the interface's own
// (ui/data/lua/timeline.lua, of the game's own controls); the functions between the two are here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho::ui {

class ReplayTimelineSource {
 public:
  virtual ~ReplayTimelineSource() = default;
  struct State {
    uint32_t ticks = 0;      // the replay's length in beats
    uint32_t tick = 0;       // the beat the game is at
    uint32_t seekTo = 0;     // a jump under way: the beat it goes to (0: none)
    bool scanning = false;   // the moments are still being looked for
    uint32_t scanned = 0;    // how far the scan has got (a beat)
    size_t moments = 0;      // moments found so far
    size_t buckets = 0;      // fighting buckets (5 s each) found so far
    bool kept = false;       // what a jump back keeps across the restart is there (TakeKept)
  };
  struct Moment {
    int tick = 0;
    std::string kind;  // "CommanderDied", "ExperimentalFinished", "ExperimentalDied", "ArmyDefeated", "NukeLaunched", "NukeLanded"
    int army = -1;     // 0-based
    float pos[3] = {0, 0, 0};
    std::string unit;
  };
  struct Fight {  // every army's fighting in those 5 s, summed
    int tick0 = 0;
    float damage = 0;
    int hits = 0, deaths = 0;
  };
  virtual State Now() const = 0;
  virtual void MomentsFrom(size_t first, std::vector<Moment>* out) const = 0;
  virtual void FightFrom(size_t first, std::vector<Fight>* out) const = 0;
  // Jump to a beat: forward the replay runs ahead to it; back it starts over and runs ahead. `keep` (the
  // interface's, serialized Lua: its camera) is kept across a jump back, for the new interface (TakeKept). False
  // with the reason when there is nothing to jump to.
  virtual bool Seek(uint32_t tick, const std::string& keep, std::string* why) = 0;
  virtual std::string TakeKept() = 0;
};

// moho64ui_ReplayTimeline, moho64ui_ReplayMoments, moho64ui_ReplayFight, moho64ui_ReplaySeek, moho64ui_ReplayTakeKept
// (replay_timeline.cpp): nil where the state has no replay timeline (UserState::Config::timeline).
void RegisterTimelineBindings(lua_State* L);
// The console's remaster_timeline [on|off] (the key "Replay timeline", Ctrl-T at first): the bar shown or hidden
// while a replay plays.
void TimelineCommand(lua_State* L, const std::string& value);

}  // namespace moho::ui
