// The running simulation as a source of frames: the engine's simulation (src/sim) plays a replay on
// a thread of its own, and after every beat what the renderer needs is copied out. The renderer
// asks for the frame at a moment between two beats and gets them blended, exactly as with a
// recording; it never touches the simulation's objects.
//
// The simulation thread has its own view of the game's files (mounted from the same init file), so
// the two threads share nothing but the copied-out beats.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace render {

class LiveSim {
 public:
  struct Setup {
    std::string initFile;   // the game's init file (init.lua, init_faf.lua, ...): what is mounted
    std::string replay;     // a .SCFAReplay file
    std::string modUids;    // a text file with one mod uid per line (may be empty)
  };
  enum class State { Loading, Running, Ended, Failed };

  LiveSim();
  ~LiveSim();  // stops the simulation's thread

  // Reads the replay's header. False with a reason when it cannot be read.
  bool Open(const Setup& setup, std::string* error);
  const std::string& MapPath() const { return mapPath_; }  // "/maps/SCMP_009/SCMP_009.scmap"
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
  // Each army's colour as the game shows it, by army index from 0; empty until the game has started.
  std::vector<Vec3> ArmyColors() const;
  float MsPerTick() const;     // what a beat costs the simulation, averaged over the last ones
  float LoadSeconds() const;   // how long loading took

 private:
  struct Beat {
    uint32_t tick = 0;
    std::vector<FrameUnit> units;  // in entity-id order
  };
  struct Engine;  // everything of src/sim, kept out of this header
  void Run();
  bool Load(std::string* error);
  void Snapshot();

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
  std::deque<Beat> beats_;             // the newest few
  std::vector<std::string> names_;     // blueprint ids, by FrameUnit::blueprint (0: unknown); only grows
  std::vector<Vec3> armyColors_;
  float msPerTick_ = 0, loadSeconds_ = 0;

  std::vector<std::string> namesForFrames_;  // the renderer's copy of names_ (what Frame::blueprints points at)
};

}  // namespace render
