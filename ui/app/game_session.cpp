#include "app/game_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "core/vfs.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/command_writer.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/world_picture.h"
#endif

#include <algorithm>
#include <cstdlib>

#include "sim/sim_view.h"
#include "sound/sound_system.h"

namespace moho::ui {

// The renderer's world picture behind the interface's WorldSource.
struct GameSession::Picture : WorldSource {
#ifdef MOHO64UI_WORLD_PICTURE
  render::WorldPicture picture;
  GameSession* session = nullptr;

  const uint32_t* Draw(const WorldCamera& c, int width, int height) override {
    render::Camera cam;
    cam.target = {c.target[0], c.target[1], c.target[2]};
    cam.distance = c.distance;
    cam.heading = c.heading;
    cam.pitch = c.pitch;
    cam.fovY = c.fovY;
    const std::vector<uint32_t>& pixels = picture.Draw(cam, width, height, session->PictureTick());
    return pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) ? pixels.data() : nullptr;
  }
  bool Project(const float world[3], float* x, float* y) override {
    return picture.Project({world[0], world[1], world[2]}, x, y);
  }
  bool GroundAt(float x, float y, float world[3]) override {
    render::Vec3 hit;
    if (!picture.GroundAt(x, y, &hit)) return false;
    world[0] = hit.x, world[1] = hit.y, world[2] = hit.z;
    return true;
  }
  uint32_t UnitAt(float x, float y) override { return picture.UnitAt(x, y); }
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) override {
    picture.UnitsIn(x0, y0, x1, y1, out);
  }
  void SetScene(const WorldScene& scene) override {
    picture.SetSelection(scene.selection);
    std::vector<render::WorldPicture::BuildPreview> previews;
    for (const WorldGhost& g : scene.ghosts) {
      render::WorldPicture::BuildPreview p;
      p.blueprint = g.blueprint;
      p.position[0] = g.position[0], p.position[1] = g.position[1], p.position[2] = g.position[2];
      p.heading = g.heading;
      p.free = g.free;
      previews.push_back(p);
    }
    picture.SetBuildPreviews(previews);
    std::vector<render::WorldPicture::Line> lines;
    for (const WorldLine& l : scene.lines) {
      render::WorldPicture::Line o;
      for (int i = 0; i < 3; ++i) o.a[i] = l.a[i], o.b[i] = l.b[i];
      o.color = l.color;
      lines.push_back(o);
    }
    picture.SetShapes(lines, {});
    picture.SetBorders(scene.borders);  // the machinery the map lies in (MapBorderAdd)
    picture.SetHover(scene.hover, scene.hoverKind);  // the brackets on the unit under the mouse
    picture.SetLifeBars(scene.lifeBars);  // the player's switch (Alt-L in the game's key map)
  }
  const uint32_t* DrawMinimap(int width, int height) override {
    const std::vector<uint32_t>& pixels = picture.DrawMinimap(width, height);
    return pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) ? pixels.data() : nullptr;
  }
  bool MapSize(float* width, float* height) override {
    picture.MapSize(width, height);
    return *width > 0 && *height > 0;
  }
  bool MinimapProject(const float world[3], float* x, float* y) override {
    return picture.MinimapProject({world[0], world[1], world[2]}, x, y);
  }
  bool MinimapGroundAt(float x, float y, float world[3]) override {
    render::Vec3 hit;
    if (!picture.MinimapGroundAt(x, y, &hit)) return false;
    world[0] = hit.x, world[1] = hit.y, world[2] = hit.z;
    return true;
  }
#else
  void SetScene(const WorldScene&) override {}
  const uint32_t* DrawMinimap(int, int) override { return nullptr; }
  bool MapSize(float*, float*) override { return false; }
  bool MinimapProject(const float*, float*, float*) override { return false; }
  bool MinimapGroundAt(float, float, float*) override { return false; }
  const uint32_t* Draw(const WorldCamera&, int, int) override { return nullptr; }
  bool Project(const float*, float*, float*) override { return false; }
  bool GroundAt(float, float, float*) override { return false; }
  uint32_t UnitAt(float, float) override { return 0xffffffffu; }
  void UnitsIn(float, float, float, float, std::vector<uint32_t>*) override {}
#endif
};

GameSession::GameSession(Vfs* vfs, std::vector<std::string> hookDirs) : vfs_(vfs), hookDirs_(std::move(hookDirs)) {}
GameSession::~GameSession() = default;

WorldSource* GameSession::world() const { return picture_.get(); }

// The moment the picture shows: the simulation's tick and the part of the next beat that has passed.
double GameSession::PictureTick() const { return sim_ ? static_cast<double>(sim_->tick()) + due_ / 0.1 : 0.0; }

bool GameSession::Launch(lua_State* L, int config, std::string* why) {
  if (config < 0) config = lua_gettop(L) + config + 1;
  if (sim_) {
    *why = "a session is running already";
    return false;
  }
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  auto t0 = std::chrono::steady_clock::now();
  SessionDescription session;
  LaunchParams params;
  params.seed = seedGiven ? seed : static_cast<uint32_t>(t0.time_since_epoch().count());
  if (!SessionFromGameConfig(L, config, params, &session, why)) return false;

  // The session's mods are the configuration's GameMods, by uid, in order.
  std::vector<std::string> uids;
  lua_getfield(L, config, "GameMods");
  for (int i = 1; lua_istable(L, -1); ++i) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_getfield(L, -1, "uid");
    if (lua_isstring(L, -1)) uids.push_back(lua_tostring(L, -1));
    lua_pop(L, 2);
  }
  lua_pop(L, 1);

  const ReplayHeader& h = session.header;
  Logf(LogLevel::Info, "moho64ui: session %s: %zu armies, %zu command sources, focus army %d, seed %u, %zu mods",
       h.map.c_str(), h.armies.size(), h.sources.size(), session.focusArmy + 1, h.seed, uids.size());
  auto sim = std::make_unique<Sim>(vfs_, hookDirs_, uids);
  sim->sessionMods = h.mods;
  if (!sim->LoadRules()) {
    *why = "the game's rules (blueprints) did not load";
    return false;
  }
  auto t1 = std::chrono::steady_clock::now();
  sim->collectSync = true;
#ifdef MOHO64UI_WORLD_PICTURE
  sim->collectEffects = wantPicture;  // explosions, shots, smoke: the picture's, asked for before the first beat
  sim->collectSounds = sounds != nullptr;  // what the scripts play in the world, as a list per beat
#endif
  if (!sim->Start(session)) {
    *why = "the simulation did not start";
    return false;
  }
  auto t2 = std::chrono::steady_clock::now();
  Logf(LogLevel::Info, "moho64ui: session started: rules %.2f s, simulation start-up %.2f s", secs(t0, t1), secs(t1, t2));
  sim_ = std::move(sim);
  pendingSync_ = sim_->TakeSync();
  if (session.focusArmy >= 0 && session.focusArmy < static_cast<int>(h.armies.size())) commandSource_ = h.armies[session.focusArmy].source;
  modUids_ = uids;
  sessionMods_ = h.mods;
  map_ = h.map;
  armies_ = h.armies.size();
  seed_ = h.seed;
  due_ = 0;
  pace.rate = session.gameSpeed;
  pace.adjustable = session.adjustableSpeed;
#ifdef MOHO64UI_WORLD_PICTURE
  if (wantPicture) {
    auto t3 = std::chrono::steady_clock::now();
    auto pic = std::make_unique<Picture>();
    pic->session = this;
    std::string error;
    if (pic->picture.Open(vfs_, h.map, &error)) {
      pic->picture.GameStarted(*sim_);
      // The picture is the player's: other armies' units show as the player's army knows them.
      pic->picture.SetFocusArmy(sim_->focusArmy + 1);
      pic->picture.Beat(*sim_);
      picture_ = std::move(pic);
      Logf(LogLevel::Info, "moho64ui: session: the world's picture is open, %.2f s", secs(t3, std::chrono::steady_clock::now()));
    } else {
      Logf(LogLevel::Warning, "moho64ui: session: no picture of the world: %s", error.c_str());
    }
  }
#endif
  return true;
}

bool GameSession::LoadInterfaceRules(lua_State* userState) {
  auto t0 = std::chrono::steady_clock::now();
  rules_ = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
  rules_->SetHookDirs(hookDirs_);
  SimBlueprints::StartRecording(*rules_);
  lua_State* R = rules_->L();
  if (!sessionMods_.empty() && PushSerializedLua(R, sessionMods_) && lua_istable(R, -1)) lua_setglobal(R, "__active_mods");
  else SetActiveMods(*rules_, modUids_);
  lua_settop(R, 0);
  if (!rules_->DoScript("/lua/ruleinit.lua")) return false;
  interfaceBlueprints_ = std::make_unique<SimBlueprints>();
  interfaceBlueprints_->fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  interfaceBlueprints_->CopyToSim(R, userState);
  RegisterCategoryBindings(userState, interfaceBlueprints_.get());
  Logf(LogLevel::Info, "moho64ui: session: %zu blueprints in the interface's state, %.2f s", interfaceBlueprints_->Count(),
       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

void GameSession::Advance(double seconds) {
  if (!sim_) return;
  due_ += seconds * std::pow(10.0, pace.rate / 10.0);  // (game time: the pace's rate, WLD_GetSimRate 0x88d0b0)
  if (due_ > 1.0) due_ = 1.0;  // after a long stall, catch up by at most a second of beats
  while (due_ >= 0.1) {
    due_ -= 0.1;
    if (takeOrders) {
      std::string orders = takeOrders();
      if (!orders.empty() && !ApplyCommands(*sim_, orders)) Logf(LogLevel::Warning, "moho64ui: an order was not understood by the simulation");
    }
    if (sim_->Paused()) sim_->PausedBeat();
    else sim_->Tick();
    NoteBeat();
    for (std::string& sync : sim_->TakeSync()) {
      if (onSync) onSync(sync);
      else pendingSync_.push_back(std::move(sync));
    }
  }
}

void GameSession::NoteBeat() {
#ifdef MOHO64UI_WORLD_PICTURE
  if (picture_) picture_->picture.Beat(*sim_);
#endif
  PlayBeatSounds();
}

// The world's sounds of the beat (the engine's SoundEvents): one-shots at their place, and the
// entities' running sounds, which then follow their units. (The second, "rumble" sound of a
// running sound is left out.)
// Whose sounds: the player's own and the allies' always; anything else only when the player's
// army sees it now (the simulation's own recon, the engine's ReconOf for a unit and ReconAt for a
// place: what belongs to no army, and a unit that is gone by now). What is only on radar or sonar
// is not heard: so a sound gives away no more than the picture does. (That choice is ours; what
// the original's sound manager does with unseen units is not read.)
void GameSession::PlayBeatSounds() {
  if (!sounds) return;
  std::vector<SoundEvent> events;
  SoundEvents(*sim_, &events);
  if (events.empty() && foreignLoops_.empty() && !sounds->HasLoops()) return;
  const int focus = sim_->focusArmy + 1;
  std::vector<bool> friendly;  // by army - 1
  for (const ArmyView& a : ArmyViews(*sim_)) {
    if (a.index < 1) continue;
    if (friendly.size() < static_cast<size_t>(a.index)) friendly.resize(static_cast<size_t>(a.index), false);
    bool ally = focus >= 1 && focus <= static_cast<int>(a.alliance.size()) && a.alliance[static_cast<size_t>(focus - 1)] == 2;
    friendly[static_cast<size_t>(a.index - 1)] = a.index == focus || ally;
  }
  auto isFriendly = [&](int army) { return army >= 1 && army <= static_cast<int>(friendly.size()) && friendly[static_cast<size_t>(army - 1)]; };
  std::vector<UnitView> units;  // in entity-id order
  UnitViews(*sim_, &units);
  auto isUnit = [&](uint32_t id) {
    auto it = std::lower_bound(units.begin(), units.end(), id, [](const UnitView& u, uint32_t v) { return u.id < v; });
    return it != units.end() && it->id == id;
  };
  auto seen = [&](uint32_t entity, const float place[3]) {
    if (focus < 1) return true;  // no army to ask for: an observer hears all
    uint32_t recon = isUnit(entity) ? ReconOf(*sim_, focus, entity) : ReconAt(*sim_, focus, place);
    return (recon & kReconSeenNow) != 0;
  };
  const bool log = std::getenv("MOHO64UI_SOUND_LOG") != nullptr;
  for (const SoundEvent& e : events) {
    bool ours = isFriendly(e.army);
    if (log)
      Logf(LogLevel::Info, "moho64ui: sound: tick %u %s entity %u army %d at %.0f %.0f %.0f: %s/%s", static_cast<unsigned>(sim_->tick()),
           e.kind == SoundEvent::Play ? "play" : "loop", static_cast<unsigned>(e.entity), e.army, e.position[0], e.position[1],
           e.position[2], e.bank.c_str(), e.cue.c_str());
    if (e.kind == SoundEvent::Play) {
      if (ours || seen(e.entity, e.position)) sounds->PlayAt(e.bank, e.cue, e.position);
    } else if (ours) {
      sounds->SetLoop(e.entity, e.bank, e.cue, e.position);
    } else if (e.bank.empty() || e.cue.empty()) {  // another army's running sound ends
      foreignLoops_.erase(e.entity);
      if (heardLoops_.erase(e.entity)) sounds->SetLoop(e.entity, "", "", e.position);
    } else {
      ForeignLoop& loop = foreignLoops_[e.entity];
      if (heardLoops_.erase(e.entity)) sounds->SetLoop(e.entity, "", "", e.position);  // it changes: start again below
      loop.bank = e.bank, loop.cue = e.cue;
      for (int i = 0; i < 3; ++i) loop.position[i] = e.position[i];
    }
  }
  // Running sounds follow their units; another army's start and end as it comes into sight and leaves it.
  for (const UnitView& u : units) {
    auto it = foreignLoops_.find(u.id);
    if (it != foreignLoops_.end())
      for (int i = 0; i < 3; ++i) it->second.position[i] = u.position[i];
    sounds->MoveLoop(u.id, u.position);
  }
  for (auto& entry : foreignLoops_) {
    bool hear = seen(entry.first, entry.second.position), heard = heardLoops_.count(entry.first) != 0;
    if (hear && !heard) {
      sounds->SetLoop(entry.first, entry.second.bank, entry.second.cue, entry.second.position);
      heardLoops_.insert(entry.first);
    } else if (!hear && heard) {
      sounds->SetLoop(entry.first, "", "", entry.second.position);
      heardLoops_.erase(entry.first);
    }
  }
}

void GameSession::RunFirstBeat() {
  if (!sim_ || sim_->tick() != 0) return;
  sim_->Tick();
  NoteBeat();
  for (std::string& sync : sim_->TakeSync()) {
    if (onSync) onSync(sync);
    else pendingSync_.push_back(std::move(sync));
  }
}

float GameSession::TerrainHeight(float x, float z) const {
  return sim_ && sim_->map() ? sim_->map()->TerrainHeight(x, z) : 0.0f;
}

uint32_t GameSession::tick() const { return sim_ ? sim_->tick() : 0; }
size_t GameSession::unitCount() const { return sim_ ? sim_->units().size() : 0; }

std::vector<std::string> GameSession::Status() const {
  std::vector<std::string> lines;
  if (!sim_) return lines;
  char buf[200];
  uint32_t t = sim_->tick();
  lines.push_back("The simulation is running.");
  std::snprintf(buf, sizeof buf, "Map %s, %zu armies, seed %u", map_.c_str(), armies_, seed_);
  lines.push_back(buf);
  std::snprintf(buf, sizeof buf, "Game time %u:%02u (tick %u), %zu units", t / 600, (t / 10) % 60, t, sim_->units().size());
  lines.push_back(buf);
  return lines;
}

void GameSession::DrawStatus(DrawTarget& target) const {
  if (!sim_) return;
  const int size = 18;
  FontMetrics m = GetFontMetrics("Arial", size);
  float line = m.height + 6, x = 40, y = 40;
  auto lines = Status();
  float w = 0;
  for (const auto& s : lines) w = std::max(w, StringAdvance("Arial", size, s));
  target.FillRect(Rect{x - 16, y - 12, x + w + 16, y + line * static_cast<float>(lines.size()) + 12}, 0xc0101820u);
  for (const auto& s : lines) {
    DrawString(target, "Arial", size, s, x, y + m.ascent, 0xffe0e8f0u);
    y += line;
  }
}

}  // namespace moho::ui
