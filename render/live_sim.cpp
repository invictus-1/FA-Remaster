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
#include "script/mount.h"
#include "sim/command_stream.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/sim_view.h"

namespace render {
namespace {

constexpr size_t kBeatsKept = 6;

// A colour of the game's list: "ffRRGGBB".
bool HexColor(const std::string& s, Vec3* out) {
  if (s.size() != 8) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (!end || *end) return false;
  *out = {static_cast<float>((v >> 16) & 0xff) / 255.0f, static_cast<float>((v >> 8) & 0xff) / 255.0f, static_cast<float>(v & 0xff) / 255.0f};
  return true;
}

// The quoted strings of the table `<name> = { ... }` in a Lua file, in order.
std::vector<std::string> QuotedList(const std::string& text, const std::string& name) {
  std::vector<std::string> list;
  size_t at = 0;
  for (;;) {  // the name as a field: followed by '=' and '{'
    at = text.find(name, at);
    if (at == std::string::npos) return list;
    size_t p = at + name.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p < text.size() && text[p] == '=') {
      ++p;
      while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
      if (p < text.size() && text[p] == '{') {
        at = p + 1;
        break;
      }
    }
    at += name.size();
  }
  for (size_t p = at; p < text.size() && text[p] != '}';) {
    if (text[p] == '-' && p + 1 < text.size() && text[p + 1] == '-') {
      while (p < text.size() && text[p] != '\n') ++p;
    } else if (text[p] == '"' || text[p] == '\'') {
      const size_t e = text.find(text[p], p + 1);
      if (e == std::string::npos) break;
      list.push_back(text.substr(p + 1, e - p - 1));
      p = e + 1;
    } else {
      ++p;
    }
  }
  return list;
}

}  // namespace

struct LiveSim::Engine {
  moho::Vfs vfs;
  std::vector<std::string> hookDirs;
  std::vector<std::string> uids;
  std::string replayData;
  moho::SessionDescription session;
  std::unique_ptr<moho::Sim> sim;
  std::unique_ptr<moho::CommandStream> stream;
  std::vector<moho::UnitView> views;
  std::unordered_map<std::string, uint32_t> nameIndex;
};

LiveSim::LiveSim() : engine_(std::make_unique<Engine>()) { names_.push_back(""); }

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
  if (!moho::MountFromInitFile(setup_.initFile, {}, e.vfs, e.hookDirs)) {
    *error = "the init file " + setup_.initFile + " could not be run";
    return false;
  }
  if (!setup_.modUids.empty()) {
    std::ifstream f(setup_.modUids);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty() && line[0] != '#') e.uids.push_back(line);
    }
  }
  e.sim = std::make_unique<moho::Sim>(&e.vfs, e.hookDirs, e.uids);
  e.sim->sessionMods = e.session.header.mods;
  if (!e.sim->LoadRules()) {
    *error = "the game's blueprints could not be loaded (see the simulation's log)";
    return false;
  }
  if (!e.sim->Start(e.session)) {
    *error = "the session could not be started (see the simulation's log)";
    return false;
  }
  e.stream = std::make_unique<moho::CommandStream>(e.replayData, e.session.header.streamOffset);

  // the armies' colours: a colour a script set, else the lobby's colour number in the game's list
  std::vector<Vec3> palette;
  if (auto text = e.vfs.ReadFile("/lua/gamecolors.lua")) {
    for (const std::string& s : QuotedList(*text, "ArmyColors")) {
      Vec3 c;
      if (HexColor(s, &c)) palette.push_back(c);
    }
  }
  std::vector<Vec3> colors;
  for (const moho::ArmyView& a : moho::ArmyViews(*e.sim)) {
    Vec3 c{0.6f, 0.6f, 0.6f};
    const int index = a.armyColor > 0 ? a.armyColor : a.playerColor;
    if (a.rgb[0] >= 0) c = {static_cast<float>(a.rgb[0]) / 255.0f, static_cast<float>(a.rgb[1]) / 255.0f, static_cast<float>(a.rgb[2]) / 255.0f};
    else if (index >= 1 && index <= static_cast<int>(palette.size())) c = palette[static_cast<size_t>(index) - 1];
    else if (a.civilian) c = {0.87f, 0.72f, 0.53f};  // the list's CivilianArmyColor, "BurlyWood"
    colors.push_back(c);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  armyColors_ = std::move(colors);
  return true;
}

// Copies what the renderer needs of the beat just computed.
void LiveSim::Snapshot() {
  Engine& e = *engine_;
  moho::UnitViews(*e.sim, &e.views);
  Beat beat;
  beat.tick = e.sim->tick();
  beat.units.reserve(e.views.size());
  std::vector<std::string> fresh;
  for (const moho::UnitView& v : e.views) {
    FrameUnit u;
    u.id = v.id;
    auto it = e.nameIndex.find(v.blueprint);
    if (it == e.nameIndex.end()) {
      it = e.nameIndex.emplace(v.blueprint, static_cast<uint32_t>(e.nameIndex.size() + 1)).first;
      fresh.push_back(v.blueprint);
    }
    u.blueprint = it->second;
    std::memcpy(u.position, v.position, sizeof u.position);
    std::memcpy(u.orientation, v.orientation, sizeof u.orientation);
    u.health = v.health;
    u.fractionComplete = v.fractionComplete;
    u.dead = v.dead;
    u.beingBuilt = v.beingBuilt;
    beat.units.push_back(u);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::string& n : fresh) names_.push_back(std::move(n));
  beats_.push_back(std::move(beat));
  while (beats_.size() > kBeatsKept) beats_.pop_front();
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
  Snapshot();  // the state the game starts in
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
    if (e.stream->AtEnd() || !e.stream->ApplyUntilAdvance(*e.sim)) break;  // the replay is over
    if (e.sim->Paused()) {  // a paused beat uses up its Advance and moves nothing
      e.sim->PausedBeat();
      continue;
    }
    const auto b0 = std::chrono::steady_clock::now();
    e.sim->Tick();
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - b0).count();
    average = average == 0 ? ms : average * 0.9f + ms * 0.1f;
    Snapshot();
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
  std::lock_guard<std::mutex> lock(mutex_);
  return beats_.empty() ? 0 : beats_.front().tick;
}

uint32_t LiveSim::LastTick() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return beats_.empty() ? 0 : beats_.back().tick;
}

bool LiveSim::FrameAt(double tick, Frame* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->units.clear();
  for (size_t i = namesForFrames_.size(); i < names_.size(); ++i) namesForFrames_.push_back(names_[i]);
  out->blueprints = &namesForFrames_;
  if (beats_.empty()) return false;
  tick = std::clamp(tick, static_cast<double>(beats_.front().tick), static_cast<double>(beats_.back().tick));
  size_t i = 0;
  while (i + 1 < beats_.size() && static_cast<double>(beats_[i + 1].tick) <= tick) ++i;
  const Beat& a = beats_[i];
  const Beat* b = i + 1 < beats_.size() ? &beats_[i + 1] : nullptr;
  const float t = b ? static_cast<float>((tick - a.tick) / static_cast<double>(b->tick - a.tick)) : 0;
  out->tick = a.tick;
  BlendUnits(a.units.data(), a.units.size(), b ? b->units.data() : nullptr, b ? b->units.size() : 0, t, &out->units);
  return true;
}

std::vector<Vec3> LiveSim::ArmyColors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return armyColors_;
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
