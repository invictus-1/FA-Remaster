#include "live_sim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/lua.hpp"
#include "script/mount.h"
#include "script/script_state.h"
#include "sim/command_stream.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/sim_view.h"

namespace render {

struct LiveSim::Engine {
  moho::Vfs vfs;
  std::vector<std::string> hookDirs;
  std::vector<std::string> uids;
  std::string replayData;
  moho::SessionDescription session;
  std::unique_ptr<moho::Sim> sim;
  std::unique_ptr<moho::CommandStream> stream;
};

LiveSim::LiveSim() : engine_(std::make_unique<Engine>()) {}

LiveSim::~LiveSim() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool LiveSim::Open(const Setup& setup, std::string* error) {
  setup_ = setup;
  Engine& e = *engine_;
  // (the simulation's own view of the files; used here before its thread starts, by that thread after)
  if (!moho::MountFromInitFile(setup_.initFile, {}, e.vfs, e.hookDirs)) {
    *error = "the init file " + setup_.initFile + " could not be run";
    return false;
  }
  for (const std::string& folder : setup_.maps) e.vfs.Mount(folder, "/maps");
  if (!setup_.modUids.empty()) {
    std::ifstream f(setup_.modUids);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty() && line[0] != '#') e.uids.push_back(line);
    }
  }
  if (!setup.session.empty()) {
    // a new game, started the way the engine's own program starts one (src/app/main.cpp, RunGameConfig)
    moho::ScriptState st(moho::ScriptState::Kind::User, &e.vfs);
    st.SetHookDirs(e.hookDirs);
    lua_State* L = st.L();
    std::string why;
    if (!moho::PushGameConfigFile(L, setup.session, &why)) {
      *error = why;
      return false;
    }
    moho::LaunchParams params;
    params.seed = setup.seed ? setup.seed : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!moho::SessionFromGameConfig(L, -1, params, &e.session, &why)) {
      *error = "the game could not be launched: " + why;
      return false;
    }
    if (setup_.modUids.empty()) {  // the configuration's own mods
      lua_getfield(L, -1, "GameMods");
      for (int i = 1; lua_istable(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        lua_getfield(L, -1, "uid");
        if (lua_isstring(L, -1)) e.uids.push_back(lua_tostring(L, -1));
        lua_pop(L, 2);
      }
    }
    mapPath_ = e.session.header.map;
    return true;
  }
  auto data = moho::hostfs::ReadFile(setup.replay);
  if (!data) {
    *error = "cannot read the replay " + setup.replay;
    return false;
  }
  engine_->replayData = std::move(*data);
  std::string why;
  auto header = moho::ReadReplayHeader(engine_->replayData, &why);
  if (!header) {
    *error = why;
    return false;
  }
  engine_->session.header = *header;
  engine_->session.replay = true;
  mapPath_ = header->map;
  return true;
}

void LiveSim::Start() { thread_ = std::thread([this] { Run(); }); }

bool LiveSim::Load(std::string* error) {
  Engine& e = *engine_;
  e.sim = std::make_unique<moho::Sim>(&e.vfs, e.hookDirs, e.uids);
  e.sim->collectEffects = true;  // the scripts' effect calls are kept for EffectEvents
  e.sim->collectProps = true;    // ... and how the scenery changes, for PropEvents
  e.sim->sessionMods = e.session.header.mods;
  if (!e.sim->LoadRules()) {
    *error = "the game's blueprints could not be loaded (see the simulation's log)";
    return false;
  }
  if (!e.sim->Start(e.session)) {
    *error = "the session could not be started (see the simulation's log)";
    return false;
  }
  if (e.session.replay) e.stream = std::make_unique<moho::CommandStream>(e.replayData, e.session.header.streamOffset);

  frames_.ReadArmies(*e.sim, e.vfs);
  if (onStarted) onStarted(*e.sim);
  return true;
}

void LiveSim::Run() {
  const auto t0 = std::chrono::steady_clock::now();
  std::string error;
  const bool loaded = Load(&error);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loadSeconds_ = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    if (!loaded) {
      state_ = State::Failed;
      problem_ = error;
      return;
    }
  }
  Engine& e = *engine_;
  if (onBeat) onBeat(*e.sim);
    frames_.Beat(*e.sim);  // the state the game starts in
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Running;
  }
  float average = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || e.sim->tick() < target_; });
      if (stop_) return;
    }
    // as the engine's own program plays a replay: the messages before each Advance, then the beat
    if (e.stream) {
      if (e.stream->AtEnd() || !e.stream->ApplyUntilAdvance(*e.sim)) break;  // the replay is over
      if (e.sim->Paused()) {  // a paused beat uses up its Advance and moves nothing
        e.sim->PausedBeat();
        continue;
      }
    } else if (e.sim->frozen) {
      break;  // a new game that has ended
    }
    const auto b0 = std::chrono::steady_clock::now();
    e.sim->Tick();
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - b0).count();
    average = average == 0 ? ms : average * 0.9f + ms * 0.1f;
    if (onBeat) onBeat(*e.sim);
    frames_.Beat(*e.sim);
    std::lock_guard<std::mutex> lock(mutex_);
    msPerTick_ = average;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = State::Ended;
}

LiveSim::State LiveSim::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::string LiveSim::Problem() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return problem_;
}

void LiveSim::RunTo(uint32_t tick) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tick <= target_) return;
    target_ = tick;
  }
  wake_.notify_all();
}

uint32_t LiveSim::FirstTick() const {
  return frames_.FirstTick();
}

uint32_t LiveSim::LastTick() const {
  return frames_.LastTick();
}

bool LiveSim::FrameAt(double tick, Frame* out) {
  return frames_.FrameAt(tick, out);
}

void LiveSim::TakeEffectEvents(std::vector<FrameEffectEvent>* out) {
  frames_.TakeEffectEvents(out);
}

std::vector<Vec3> LiveSim::ArmyColors() const {
  return frames_.ArmyColors();
}

float LiveSim::MsPerTick() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return msPerTick_;
}

float LiveSim::LoadSeconds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loadSeconds_;
}

}  // namespace render
