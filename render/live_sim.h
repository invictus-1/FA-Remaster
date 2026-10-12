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
  // How the blueprints look as the simulation has them, once the game has started; handed over once.
  void TakeLooks(std::vector<BlueprintLook>* out) { frames_.TakeLooks(out); }
  // The army the picture is for (from 1; 0: none, everything shows): see SimFrames::SetFocusArmy.
  void SetFocusArmy(int army) { frames_.SetFocusArmy(army); }
  // The session's seed, once the game has started (0 before).
  uint32_t Seed() const { return frames_.Seed(); }
  // Each army's colour as the game shows it, by army index from 0; empty until the game has started.
  std::vector<Vec3> ArmyColors() const;
  float MsPerTick() const;     // what a beat costs the simulation, averaged over the last ones
  float LoadSeconds() const;   // how long loading took

 private:
  struct Engine;  // everything of src/sim, kept out of this header
  void Run();
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
