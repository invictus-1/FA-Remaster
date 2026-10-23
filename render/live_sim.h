// The running simulation as a source of frames: the engine's simulation (src/sim) plays a replay,
// or a new game, on a thread of its own, and after every beat what the renderer needs is copied out. The renderer
// asks for the frame at a moment between two beats and gets them blended, exactly as with a
// recording; it never touches the simulation's objects.
//
// The simulation thread has its own view of the game's files (mounted from the same init file), so
// the two threads share nothing but the copied-out beats.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame.h"
#include "linalg.h"
#include "sim_frames.h"

namespace moho {
class Sim;
}

namespace render {

class LiveSim {
 public:
  struct Setup {
    std::string initFile;   // the game's init file (init.lua, init_faf.lua, ...): what is mounted
    std::string replay;     // a .SCFAReplay file to play, or
    std::string session;    // a game configuration to start as a new game (what a lobby hands over at
                            // Launch: a Lua file holding `GameConfig = { ... }`)
    std::string modUids;    // a text file with one mod uid per line (empty: a session's own GameMods)
    uint32_t seed = 0;      // a new game's random seed; 0: taken from the clock
    std::vector<std::string> maps;  // more folders of map folders, mounted at /maps beside what the init file mounts
    // For tests of a new game: at this beat the army's first unit (its commander) destroys itself at once,
    // by the game's own callback (what Ctrl-K sends). 0 = never.
    uint32_t selfDestructAt = 0;
    int selfDestructArmy = 1;  // 1-based
    // For tests of a new game: at this beat the army's first unit (its commander) is ordered to
    // build that enhancement on itself (the order the interface writes: a script task). 0 = never.
    uint32_t enhanceAt = 0;
    int enhanceArmy = 1;
    std::string enhancement;
    // ... and at this beat the same unit is paused (what the pause button sends). 0 = never.
    uint32_t pauseAt = 0;
    // [pings] For tests of a new game: at this beat every army pings 30 from its first unit (its commander) toward the
    // map's middle, by the game's own callback as the interface's ping.lua sends it (SimCallback SpawnPing), the kinds
    // in turn (alert, move, attack, and a nuke's mesh). 0 = never.
    uint32_t pingAt = 0;
    // [hitches] For tests of a new game: at its beat `at` army 1 is given a `from` (the cheats' CreateUnit, as the
    // console's CreateUnit makes one) standing at x, z, and `after` beats later it is ordered to upgrade to `to` (by the
    // simulation's IssueUpgrade, as an AI's script orders it). Each upgrade on its own.
    struct Upgrade {
      uint32_t at = 0, after = 30;
      float x = 0, z = 0;
      std::string from = "ueb0101", to = "ueb0201";
      bool seen = false;  // (the upgrade stands there)
    };
    std::vector<Upgrade> upgrades;
  };
  enum class State { Loading, Running, Ended, Failed };

  LiveSim();
  ~LiveSim();  // stops the simulation's thread

  // Mounts the game's files and reads the replay's header, or builds the session from the game
  // configuration (which runs the map's scenario file). False with a reason when that fails.
  bool Open(const Setup& setup, std::string* error);
  const std::string& MapPath() const { return mapPath_; }  // "/maps/SCMP_009/SCMP_009.scmap"
  // Called on the simulation's thread, when set before Start: once the game has started, and after
  // every beat (before this class copies the beat out; a hook that takes the beat's effect events
  // leaves none for it). For another taker of beats, such as a WorldPicture being tried out.
  std::function<void(const moho::Sim&)> onStarted, onBeat;
  // Starts loading and then running, on the simulation's thread.
  void Start();

  State GetState() const;
  std::string Problem() const;  // why it failed
  // The simulation may run up to this tick (it runs no further ahead). Call every frame.
  void RunTo(uint32_t tick);
  uint32_t FirstTick() const;  // of the beats kept
  uint32_t LastTick() const;   // the newest beat computed
  // The frame at a moment given in ticks, blended between the two beats around it (clamped to the
  // beats kept). False while there is no beat yet.
  bool FrameAt(double tick, Frame* out);
  // The effect events of the beats computed since the last call, in order (each says its beat).
  void TakeEffectEvents(std::vector<FrameEffectEvent>* out);
  void TakePropEvents(std::vector<FramePropEvent>* out) { frames_.TakePropEvents(out); }
  void TakeGroundChanges(std::vector<FrameGroundChange>* out) { frames_.TakeGroundChanges(out); }
  // The hits, impacts and health changes of the beats computed since the last call (SimFrames::TakeCombat).
  void TakeCombat(FrameCombat* out) { frames_.TakeCombat(out); }
  // How the blueprints look as the simulation has them, once the game has started; handed over once.
  void TakeLooks(std::vector<BlueprintLook>* out) { frames_.TakeLooks(out); }
  // [spikes] What the armies may build, for the renderer to make ahead (SimFrames::WatchBuildable, TakeBuildable).
  void WatchBuildable(bool on) { frames_.WatchBuildable(on); }
  void TakeBuildable(std::vector<std::string>* out) { frames_.TakeBuildable(out); }
  // The army the picture is for (from 1; 0: none, everything shows): see SimFrames::SetFocusArmy.
  void SetFocusArmy(int army) { frames_.SetFocusArmy(army); }
  // The grid of what the focus army sees (SimFrames::SetSightGrid, TakeSight).
  void SetSightGrid(float mapWidth, float mapHeight) { frames_.SetSightGrid(mapWidth, mapHeight); }
  bool TakeSight(std::vector<uint8_t>* grid, int* cells) { return frames_.TakeSight(grid, cells); }
  // The session's seed, once the game has started (0 before).
  uint32_t Seed() const { return frames_.Seed(); }
  // Each army's colour as the game shows it, by army index from 0; empty until the game has started.
  std::vector<Vec3> ArmyColors() const;
  float MsPerTick() const;     // what a beat costs the simulation, averaged over the last ones
  float LoadSeconds() const;   // how long loading took

 private:
  struct Engine;  // everything of src/sim, kept out of this header
  void Run();
  void SelfDestruct(int army);
  void Enhance(int army, const std::string& enhancement);
  void Pause(int army);
  void Ping();  // [pings] (Setup::pingAt)
  void UpgradeTest(uint32_t tick, Setup::Upgrade& test);  // [hitches] (Setup::upgrades)
  bool Load(std::string* error);

  Setup setup_;
  std::string mapPath_;
  std::unique_ptr<Engine> engine_;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  State state_ = State::Loading;
  std::string problem_;
  uint32_t target_ = 0;
  float msPerTick_ = 0, loadSeconds_ = 0;
  SimFrames frames_;  // the beats, copied out
};

}  // namespace render
