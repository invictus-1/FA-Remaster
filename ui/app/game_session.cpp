#include "app/game_session.h"

#include <algorithm>
#include <chrono>
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

namespace moho::ui {

// The map's border goes to the picture through WorldPicture::SetBorders (the renderer's answer to
// request 3). The call is made only where the renderer's code has it: the answer reaches main with
// the next merge of the renderer's branch, and this file builds before and after it. (Once it is
// there for good, call it plainly.)
template <class P>
auto SetBordersIfThere(P& picture, const std::vector<std::string>& borders, int) -> decltype(picture.SetBorders(borders), void()) {
  picture.SetBorders(borders);
}
template <class P>
void SetBordersIfThere(P&, const std::vector<std::string>&, long) {}

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
    SetBordersIfThere(picture, scene.borders, 0);
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
#ifdef MOHO64UI_WORLD_PICTURE
  if (wantPicture) {
    auto t3 = std::chrono::steady_clock::now();
    auto pic = std::make_unique<Picture>();
    pic->session = this;
    std::string error;
    if (pic->picture.Open(vfs_, h.map, &error)) {
      pic->picture.GameStarted(*sim_);
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
  due_ += seconds;
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
