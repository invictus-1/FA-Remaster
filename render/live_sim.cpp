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
#include "sim/command_writer.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"  // [pings] (the ground where a ping is placed)
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
  // The game's preferences file (sim/session.h kGamePrefsPath): this run's own, held in memory and mounted before the
  // init file mounts the original's preferences folder, so the rules never read the original's Game.prefs (the
  // PreGameData of the last game played in FAF); its map folder set below once the session is known (as the engine's
  // own program does, src/app/main.cpp)
  e.vfs.MountMemoryFile(moho::kGamePrefsPath, moho::PreGameDataPrefs(""));
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
    e.vfs.SetMemoryFile(moho::kGamePrefsPath, moho::PreGameDataPrefs(moho::SessionMapFolder(e.session.header)));
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
  // (the map folder whose own blueprints the rules load: the replay's own scenario's, as the lobby wrote it)
  e.vfs.SetMemoryFile(moho::kGamePrefsPath, moho::PreGameDataPrefs(moho::SessionMapFolder(engine_->session.header)));
  return true;
}

void LiveSim::Start() { thread_ = std::thread([this] { Run(); }); }

bool LiveSim::Load(std::string* error) {
  Engine& e = *engine_;
  e.sim = std::make_unique<moho::Sim>(&e.vfs, e.hookDirs, e.uids);
  e.sim->collectEffects = true;  // the scripts' effect calls are kept for EffectEvents
  e.sim->collectProps = true;    // ... and how the scenery changes, for PropEvents
  e.sim->collectHits = true;     // ... and hits, impacts and health changes, for TakeCombat (about 1% of a beat)
  e.sim->collectImpacts = true;
  e.sim->collectHealth = true;
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

// The army's first unit destroys itself: SimCallbacks' ToggleSelfDestruct with {owner, noDelay = true} and the
// unit, sent as the army's own command source would send it.
void LiveSim::SelfDestruct(int army) {
  Engine& e = *engine_;
  std::vector<moho::UnitView> units;
  moho::UnitViews(*e.sim, &units);
  const moho::UnitView* first = nullptr;
  for (const moho::UnitView& u : units)
    if (u.army == army && (!first || u.id < first->id)) first = &u;
  if (!first || army < 1 || army > static_cast<int>(e.session.header.armies.size())) return;
  const float owner = static_cast<float>(army);
  std::string args;  // (a Lua table in the engine's byte form: 4, key and value pairs, 5)
  args.push_back(4);
  args.push_back(1);
  args.append("owner");
  args.push_back(0);
  args.push_back(0);
  args.append(reinterpret_cast<const char*>(&owner), 4);
  args.push_back(1);
  args.append("noDelay");
  args.push_back(0);
  args.push_back(3);
  args.push_back(1);
  args.push_back(5);
  moho::CommandWriter writer(e.session.header.armies[army - 1].source);
  writer.LuaSimCallback("ToggleSelfDestruct", args, {first->id});
  moho::ApplyCommands(*e.sim, writer.Take());
  moho::Logf(moho::LogLevel::Info, "render: army %d's %s (entity %u) destroys itself at beat %u, at %.1f, %.1f", army,
             first->blueprint.c_str(), first->id, e.sim->tick(), first->position[0], first->position[2]);
}

// The army's first unit is ordered to build an enhancement on itself: a Script order whose table is
// { TaskName = "EnhanceTask", Enhancement = <name> }, as the interface's script gives it.
void LiveSim::Enhance(int army, const std::string& enhancement) {
  Engine& e = *engine_;
  std::vector<moho::UnitView> units;
  moho::UnitViews(*e.sim, &units);
  const moho::UnitView* first = nullptr;
  for (const moho::UnitView& u : units)
    if (u.army == army && (!first || u.id < first->id)) first = &u;
  if (!first || army < 1 || army > static_cast<int>(e.session.header.armies.size())) return;
  std::string table;  // (a Lua table in the engine's byte form: 4, key and value pairs, 5; a string is 1, its text, 0)
  auto text = [&](const char* s) {
    table.push_back(1);
    table.append(s);
    table.push_back(0);
  };
  table.push_back(4);
  text("TaskName");
  text("EnhanceTask");
  text("Enhancement");
  text(enhancement.c_str());
  table.push_back(5);
  moho::CommandWriter writer(e.session.header.armies[army - 1].source);
  moho::StreamIssueData order;
  order.type = static_cast<int>(moho::CommandType::Script);
  order.lua = table;
  writer.IssueCommand({first->id}, order, true);
  moho::ApplyCommands(*e.sim, writer.Take());
  moho::Logf(moho::LogLevel::Info, "render: army %d's %s (entity %u) is ordered to build %s at beat %u, at %.1f, %.1f", army,
             first->blueprint.c_str(), first->id, enhancement.c_str(), e.sim->tick(), first->position[0], first->position[2]);
}

// The army's first unit is paused: the pause button's message (SetPaused = true).
void LiveSim::Pause(int army) {
  Engine& e = *engine_;
  std::vector<moho::UnitView> units;
  moho::UnitViews(*e.sim, &units);
  const moho::UnitView* first = nullptr;
  for (const moho::UnitView& u : units)
    if (u.army == army && (!first || u.id < first->id)) first = &u;
  if (!first || army < 1 || army > static_cast<int>(e.session.header.armies.size())) return;
  moho::CommandWriter writer(e.session.header.armies[army - 1].source);
  writer.ProcessInfoPair(first->id, "SetPaused", "true");
  moho::ApplyCommands(*e.sim, writer.Take());
  moho::Logf(moho::LogLevel::Info, "render: army %d's %s (entity %u) is paused at beat %u, its work at %.2f", army, first->blueprint.c_str(), first->id,
             e.sim->tick(), first->workProgress);
}

// [pings] Every army pings near its first unit: SimCallbacks' SpawnPing with the table ping.lua's DoPing hands it ({Owner =
// army - 1, Type, Location, Lifetime = 6, Mesh}: SimPing.lua makes the script entity with "/meshes/game/ping_" .. Mesh 2
// above Location), from the army's own command source; the kinds in turn. The place: 30 from the unit toward the map's
// middle, on the ground or the water there (as the mouse's place in the world is given).
void LiveSim::Ping() {
  Engine& e = *engine_;
  std::vector<moho::UnitView> units;
  moho::UnitViews(*e.sim, &units);
  static const char* const kKinds[4][2] = {{"Alert", "alert_marker"}, {"Move", "move"}, {"Attack", "attack_marker"}, {"Alert", "nuke_marker"}};
  for (int army = 1; army <= static_cast<int>(e.session.header.armies.size()); ++army) {
    const moho::UnitView* first = nullptr;
    for (const moho::UnitView& u : units)
      if (u.army == army && (!first || u.id < first->id)) first = &u;
    if (!first) continue;
    std::string t;  // (the engine's byte form of a Lua value: 0 a number, 1 a string, 4 a table's pairs up to 5)
    auto number = [&](float v) {
      t.push_back(0);
      t.append(reinterpret_cast<const char*>(&v), 4);
    };
    auto text = [&](const char* s) {
      t.push_back(1);
      t.append(s);
      t.push_back(0);
    };
    const char* const* kind = kKinds[(army - 1) % 4];
    t.push_back(4);
    text("Owner");
    number(static_cast<float>(army - 1));
    text("Type");
    text(kind[0]);
    text("Mesh");
    text(kind[1]);
    text("Lifetime");
    number(6);
    text("Location");
    float at[3] = {first->position[0], first->position[1], first->position[2]};
    if (const moho::TerrainMap* map = e.sim->map()) {
      const float dx = static_cast<float>(map->width()) * 0.5f - at[0], dz = static_cast<float>(map->height()) * 0.5f - at[2];
      const float d = std::sqrt(dx * dx + dz * dz);
      if (d > 1) at[0] += dx / d * 30, at[2] += dz / d * 30;
      at[1] = map->SurfaceHeight(at[0], at[2]);
    }
    t.push_back(4);
    for (int k = 0; k < 3; ++k) {
      number(static_cast<float>(k + 1));
      number(at[k]);
    }
    t.push_back(5);
    t.push_back(5);
    moho::CommandWriter writer(e.session.header.armies[army - 1].source);
    writer.LuaSimCallback("SpawnPing", t, {});
    moho::ApplyCommands(*e.sim, writer.Take());
    moho::Logf(moho::LogLevel::Info, "render: army %d pings (%s) at beat %u, at %.1f, %.1f", army, kind[1], e.sim->tick(), at[0], at[2]);
  }
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
    } else if (setup_.selfDestructAt && e.sim->tick() == setup_.selfDestructAt) {
      SelfDestruct(setup_.selfDestructArmy);
    } else if (setup_.enhanceAt && e.sim->tick() == setup_.enhanceAt) {
      Enhance(setup_.enhanceArmy, setup_.enhancement);
    } else if (setup_.pauseAt && e.sim->tick() == setup_.pauseAt) {
      Pause(setup_.enhanceArmy);
    }
    if (!e.stream && setup_.pingAt && e.sim->tick() == setup_.pingAt) Ping();  // [pings] (a new game's only)
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
