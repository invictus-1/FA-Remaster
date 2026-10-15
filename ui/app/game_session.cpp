#include "app/game_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/command_stream.h"
#include "sim/command_writer.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/world.h"
#include "render/world_picture.h"
#endif

#include <algorithm>
#include <cstdlib>

#include "sim/sim_view.h"
#include "user/user_state.h"
#include "sound/sound_system.h"

namespace moho::ui {

// The renderer's world picture behind the interface's WorldSource.
struct GameSession::Picture : WorldSource {
#ifdef MOHO64UI_WORLD_PICTURE
  render::WorldPicture picture;
  GameSession* session = nullptr;
  LookSettings lookNow;      // the player's looks as last given to the picture
  bool lookApplied = false;
  // On the game's window (UseWindow): the world view's Draw only notes its camera and size, and
  // the frame is drawn and shown by PresentToWindow with the interface over it.
  bool direct = false;
  render::Camera camera;
  int cameraW = 0, cameraH = 0;
  bool vsync = true;
  bool DrawsToWindow() const override { return direct; }

  const uint32_t* Draw(const WorldCamera& c, int width, int height) override {
    render::Camera cam;
    cam.target = {c.target[0], c.target[1], c.target[2]};
    cam.distance = c.distance;
    cam.heading = c.heading;
    cam.pitch = c.pitch;
    cam.fovY = c.fovY;
    if (direct) {  // (drawn and shown with the interface by PresentToWindow)
      camera = cam, cameraW = width, cameraH = height;
      static const uint32_t token = 0;
      return &token;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint32_t>& pixels = picture.Draw(cam, width, height, session->PictureTick());
    FrameTimes& times = Times();
    times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ++times.worldDraws;
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
      o.width = l.width;
      lines.push_back(o);
    }
    picture.SetShapes(lines, {});
    // The range rings in groups: the renderer draws a group as one outline.
    std::vector<render::WorldPicture::RangeGroup> ranges;
    for (const WorldRangeGroup& g : scene.ranges) {
      render::WorldPicture::RangeGroup o;
      o.color = g.color, o.glow = g.glow;
      o.outerNear = g.outer[0], o.outerFar = g.outer[1];
      o.innerNear = g.inner[0], o.innerFar = g.inner[1];
      for (const WorldRangeRing& r : g.rings) {
        render::WorldPicture::RangeRing ring;
        for (int i = 0; i < 3; ++i) ring.centre[i] = r.centre[i];
        ring.radius = r.radius, ring.innerRadius = r.innerRadius;
        o.rings.push_back(ring);
      }
      ranges.push_back(std::move(o));
    }
    picture.SetRanges(ranges, scene.zoomNear, scene.zoomFar);
    // The meshes the game's script has placed in the world, and the marks where orders landed.
    std::vector<render::WorldPicture::WorldMesh> meshes;
    for (const WorldMeshView& m : scene.meshes) {
      render::WorldPicture::WorldMesh o;
      o.mesh = m.mesh, o.texture = m.texture, o.shader = m.shader;
      o.uniformScale = m.uniformScale;
      for (int i = 0; i < 3; ++i) o.position[i] = m.position[i], o.scale[i] = m.scale[i];
      for (int i = 0; i < 4; ++i) o.orientation[i] = m.orientation[i];
      o.color = m.color, o.hidden = m.hidden;
      o.lifetime = m.lifetime, o.age = m.age, o.fractionComplete = m.fractionComplete;
      meshes.push_back(std::move(o));
    }
    picture.SetWorldMeshes(meshes);
    picture.SetBorders(scene.borders);  // the machinery the map lies in (MapBorderAdd)
    picture.SetHover(scene.hover, scene.hoverKind);  // the brackets on the unit under the mouse
    picture.SetLifeBars(scene.lifeBars);  // the player's switch (Alt-L in the game's key map)
    // Life bars over the player's and his allies' units only, as the original (render request
    // 21), unless the player asks for the enemies' too.
    picture.SetLifeBarsOnEnemies(scene.lifeBarsOnEnemies);
    // The player's own looks (the Options menu's Video tab), when they change.
    const LookSettings& looks = Looks();
    if (!lookApplied || looks.remasteredEffects != lookNow.remasteredEffects) picture.SetRemasteredEffects(looks.remasteredEffects);
    if (!lookApplied || looks.effectsQuality != lookNow.effectsQuality) picture.SetEffectsQuality(looks.effectsQuality);
    if (!lookApplied || looks.upscale != lookNow.upscale) picture.SetUpscale(looks.upscale);
    if (render::World* w = picture.world()) {
      w->ribbons.tracers = looks.tracers;
      w->terrain.richGround = looks.richGround;
      w->tracks.on = looks.snowTracks;
    }
    lookNow = looks;
    lookApplied = true;
    if (session && session->sim_) {
      const int focus = session->sim_->focusArmy + 1;
      std::vector<int> allies;
      for (const ArmyView& a : ArmyViews(*session->sim_))
        if (focus >= 1 && a.index >= 1 && a.index != focus && focus <= static_cast<int>(a.alliance.size()) &&
            a.alliance[static_cast<size_t>(focus - 1)] == 2)
          allies.push_back(a.index);
      picture.SetAllies(allies);
    }
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

namespace {
// The player's profile name (the game's own prefs.lua: GetCurrentProfile().Name), "" for none.
std::string ProfileName(lua_State* L) {
  const int top = lua_gettop(L);
  std::string name;
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/user/prefs.lua");
  if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
    lua_getfield(L, -1, "GetCurrentProfile");
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
      lua_getfield(L, -1, "Name");
      if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return name;
}
// (UTF-8 text and paths both ways; std::filesystem's own u8 forms are char8_t under C++20)
std::filesystem::path PathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string TextOf(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::FILE* OpenForWriting(const std::filesystem::path& file) {
#ifdef _WIN32
  return _wfopen(file.wstring().c_str(), L"wb");
#else
  return std::fopen(TextOf(file).c_str(), "wb");
#endif
}
}  // namespace

bool GameSession::Launch(lua_State* L, int config, std::string* why) {
  if (config < 0) config = lua_gettop(L) + config + 1;
  if (sim_) {
    *why = "a session is running already";
    return false;
  }
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

  // The day and the weather: the game's options where it has them (the lobby's Remaster...
  // options), the host's own otherwise.
  skyNow_ = sky;
  seed_ = session.header.seed;
  lua_getfield(L, config, "GameOptions");
  if (lua_istable(L, -1)) ReadLookOptions(L, lua_gettop(L));
  lua_pop(L, 1);
  profile_ = ProfileName(L);
  playData_.clear();
  play_.reset();
  if (!StartSession(session, uids, why)) return false;
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = false;
  return true;
}

void GameSession::ReadLookOptions(lua_State* L, int options) {
  auto option = [&](const char* key) -> std::string {
    lua_getfield(L, options, key);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
  };
  const std::string start = option("RemasterDayStart"), length = option("RemasterDayLength"), weather = option("RemasterWeather");
  if (start == "map") skyNow_.startHour = -1;
  else if (start == "dawn") skyNow_.startHour = 6.5f;   // (the renderer: at 6.0 the land is nearly as dark as at night)
  else if (start == "morning") skyNow_.startHour = 9.0f;
  else if (start == "noon") skyNow_.startHour = 12.0f;
  else if (start == "afternoon") skyNow_.startHour = 15.0f;
  else if (start == "dusk") skyNow_.startHour = 17.5f;  // (likewise 18.0)
  else if (start == "night") skyNow_.startHour = 0.0f;
  else if (start == "random") skyNow_.startHour = static_cast<float>(seed_ % 96u) * 0.25f;  // every player draws the same
  if (length == "still") skyNow_.dayLengthSeconds = 0;
  else if (!length.empty() && std::atof(length.c_str()) > 0) skyNow_.dayLengthSeconds = static_cast<float>(std::atof(length.c_str()) * 60.0);  // minutes
  if (!weather.empty()) skyNow_.weather = weather;
  const std::string edge = option("RemasterMapEdge");
  if (edge == "frame" || edge == "beyond") skyNow_.edge = edge;
}

bool GameSession::LaunchReplay(lua_State* L, const std::string& file, std::string* why) {
  if (sim_) {
    *why = "a session is running already";
    return false;
  }
  auto data = hostfs::ReadFile(file);
  if (!data) {
    *why = "cannot read " + file;
    return false;
  }
  auto h = ReadReplayHeader(*data, why);
  if (!h) return false;
  SessionDescription session;
  session.header = *h;
  session.replay = true;     // (the simulation's SessionIsReplay)
  session.focusArmy = -1;    // the player watches: no army of his (INFERRED: the original's start)
  session.adjustableSpeed = true;
  // The mods are the header's (GameMods as the launch had them: a list of tables with a uid).
  std::vector<std::string> uids;
  const int top = lua_gettop(L);
  if (PushSerializedLua(L, h->mods) && lua_istable(L, -1)) {
    for (int i = 1;; ++i) {
      lua_rawgeti(L, top + 1, i);
      if (!lua_istable(L, -1)) break;
      lua_getfield(L, -1, "uid");
      if (lua_isstring(L, -1)) uids.push_back(lua_tostring(L, -1));
      lua_pop(L, 2);
    }
  }
  lua_settop(L, top);
  // The look of the game: the scenario's Options, as the lobby gave them.
  skyNow_ = sky;
  seed_ = h->seed;
  if (PushSerializedLua(L, h->scenario) && lua_istable(L, -1)) {
    lua_getfield(L, -1, "Options");
    if (lua_istable(L, -1)) {
      ReadLookOptions(L, lua_gettop(L));
      // A game played here with the engine's script fixes is played back with them (the mark
      // StartReplayFile writes; ui request engine 23). Without the mark (the original's
      // recordings) the engine's own rule stands: no fixes for a replay.
      lua_getfield(L, -1, "RemasterFixes");
      if (lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "on") session.fixes = 1;
    }
  }
  lua_settop(L, top);
  profile_ = ProfileName(L);
  playData_ = std::move(*data);
  play_.reset();
  if (!StartSession(session, uids, why)) {
    playData_.clear();
    return false;
  }
  play_ = std::make_unique<CommandStream>(playData_, h->streamOffset);
  last_ = std::make_unique<SessionDescription>(session);
  lastUids_ = uids;
  lastWasReplay_ = true;
  Logf(LogLevel::Info, "moho64ui: playing the replay %s (%zu bytes, %zu armies, %s)", file.c_str(), playData_.size(), h->armies.size(),
       sim_->fixes ? "with the engine's script fixes, as it was played" : "without the engine's script fixes");
  return true;
}

// The replay file of a game being played (LastGame), the original's format (the header as the
// engine's ReadReplayHeader reads it, sim/replay.cpp, then the command stream): so the game's
// Replay dialog lists it and the original game could open it too.
void GameSession::StartReplayFile(const SessionDescription& session) {
  if (replay_) std::fclose(replay_), replay_ = nullptr;
  if (session.replay || userDir.empty() || profile_.empty()) return;
  std::error_code ec;
  const std::filesystem::path folder = PathOf(userDir) / "replays" / PathOf(profile_);
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path file = folder / "LastGame.SCFAReplay";
  replay_ = OpenForWriting(file);
  if (!replay_) {
    Logf(LogLevel::Warning, "moho64ui: cannot write the replay %s", TextOf(file).c_str());
    return;
  }
  currentReplay = TextOf(file);
  const ReplayHeader& h = session.header;
  std::string out;
  auto cstr = [&](const std::string& s) { out += s, out.push_back('\0'); };
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  };
  auto blob = [&](const std::string& s) { u32(static_cast<uint32_t>(s.size())), out += s; };
  cstr(h.version.empty() ? "Supreme Commander v1.50.3839" : h.version);
  cstr("\r\n");
  cstr((h.replayVersion.empty() ? std::string("Replay v1.9") : h.replayVersion) + "\r\n" + h.map);
  cstr("\r\n\x1a");
  blob(h.mods);
  // (A game played with the engine's script fixes says so in its scenario's Options,
  // RemasterFixes = "on", so that its replay is played with them too; the original ignores it.)
  std::string scenario = h.scenario;
  if (sim_ && sim_->fixes) {
    lua_State* S = sim_->L();
    const int top = lua_gettop(S);
    if (PushSerializedLua(S, h.scenario) && lua_istable(S, -1)) {
      lua_pushstring(S, "Options");
      lua_rawget(S, top + 1);
      if (lua_istable(S, -1)) {
        lua_pushstring(S, "RemasterFixes");
        lua_pushstring(S, "on");
        lua_rawset(S, -3);
        scenario.clear();  // (SerializeLua appends)
        SerializeLua(S, top + 1, &scenario);
      }
    }
    lua_settop(S, top);
  }
  blob(scenario);
  out.push_back(static_cast<char>(h.sources.size()));
  for (const ReplayHeader::Source& s : h.sources) cstr(s.name), u32(static_cast<uint32_t>(s.pausesAllowed));
  out.push_back(h.cheats ? 1 : 0);
  out.push_back(static_cast<char>(h.armies.size()));
  for (const ReplayHeader::Army& a : h.armies) {
    blob(a.options);
    for (int s : a.sources) out.push_back(static_cast<char>(s));
    out.push_back(static_cast<char>(0xff));
  }
  u32(h.seed);
  std::fwrite(out.data(), 1, out.size(), replay_);
  std::fflush(replay_);
  Logf(LogLevel::Info, "moho64ui: this game's replay is %s", currentReplay.c_str());
}

void GameSession::Record(const std::string& bytes) {
  if (bytes.empty()) return;
  if (replay_) {
    std::fwrite(bytes.data(), 1, bytes.size(), replay_);
    std::fflush(replay_);
  }
  if (!stream_) return;
  std::fwrite(bytes.data(), 1, bytes.size(), stream_);
  std::fflush(stream_);  // (a game that ends badly keeps what it had)
}

void GameSession::End() {
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  if (replay_) std::fclose(replay_), replay_ = nullptr;
  play_.reset();
  if (sounds) sounds->StopWorld();
  onSync = nullptr;
  takeOrders = nullptr;
  picture_.reset();
  sim_.reset();
  interfaceBlueprints_.reset();
  rules_.reset();
  pendingSync_.clear();
  foreignLoops_.clear();
  heardLoops_.clear();
  due_ = 0;
  pace = GamePace();
}

bool GameSession::Relaunch(std::string* why) {
  if (!last_) {
    *why = "no game was launched";
    return false;
  }
  End();
  SessionDescription again = *last_;
  if (lastWasReplay_) {  // a replay starts over from its first beat, with its own seed
    if (!StartSession(again, lastUids_, why)) return false;
    play_ = std::make_unique<CommandStream>(playData_, again.header.streamOffset);
    return true;
  }
  if (!seedGiven) again.header.seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  return StartSession(again, lastUids_, why);
}

bool GameSession::StartSession(const SessionDescription& session, const std::vector<std::string>& uids, std::string* why) {
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  auto t0 = std::chrono::steady_clock::now();
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
  sim->collectProps = wantPicture;    // wreckage, and trees reclaimed, burnt or knocked over (the picture takes the list)
  sim->collectSounds = sounds != nullptr;  // what the scripts play in the world, as a list per beat
  sim->collectShakes = true;               // and the camera shakes they ask for (PostCameraShake)
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
  StartReplayFile(session);
  if (stream_) std::fclose(stream_), stream_ = nullptr;
  if (!replayOut.empty()) {  // the game's record: see the header
    const std::string base = replayOut + (recorded_ ? "." + std::to_string(recorded_ + 1) : "");
    ++recorded_;
    stream_ = std::fopen((base + ".stream").c_str(), "wb");
    if (!stream_) Logf(LogLevel::Warning, "moho64ui: cannot write %s.stream", base.c_str());
    else Logf(LogLevel::Info, "moho64ui: this game is recorded in %s.stream (seed %u)", base.c_str(), h.seed);
    if (std::FILE* f = std::fopen((base + ".seed").c_str(), "wb")) {
      std::fprintf(f, "seed %u\nmap %s\narmies %zu\nfocus army %d\n", h.seed, h.map.c_str(), h.armies.size(), session.focusArmy + 1);
      if (!replayTable.empty()) std::fprintf(f, "session %s\n", replayTable.c_str());
      for (const std::string& folder : replayMaps) std::fprintf(f, "maps %s\n", folder.c_str());
      std::fclose(f);
    }
  }
  pace.rate = session.gameSpeed;
  pace.adjustable = session.adjustableSpeed;
  pictureDue_ = false;
  if (wantPicture) {
    // (A window that is still to come: the picture opens on it in its first frame,
    // OpenPictureIfDue.)
    if (waitForWindow && !window) pictureDue_ = true;
    else OpenPicture();
  }
  return true;
}

#ifdef MOHO64UI_WORLD_PICTURE
struct GameSession::MenuPicture {
  render::WorldPicture picture;
  bool vsync = true;
};
#else
struct GameSession::MenuPicture {};
#endif

bool GameSession::DrawsToWindow() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return window != nullptr && (!picture_ || picture_->direct);
#else
  return false;
#endif
}

#ifdef MOHO64UI_WORLD_PICTURE
namespace {
// What a picture showed in the window, read back by the renderer (RGBA bytes), saved as a PNG.
void SaveWindowCapture(const std::vector<uint8_t>& rgba, int w, int h, const std::string& path) {
  if (rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    Logf(LogLevel::Warning, "moho64ui: the window's picture was not read back");
    return;
  }
  std::vector<uint32_t> argb(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (size_t i = 0; i < argb.size(); ++i)
    argb[i] = 0xff000000u | static_cast<uint32_t>(rgba[i * 4]) << 16 | static_cast<uint32_t>(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2];
  SoftwareTarget shot(w, h);
  shot.DrawPixels(argb.data(), w, h, 0, 0);
  if (shot.SavePng(path)) Logf(LogLevel::Info, "moho64ui: the window's picture is saved as %s", path.c_str());
}
}  // namespace
#endif

void GameSession::PresentToWindow(const SoftwareTarget& target) {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!window) return;
  if (!picture_) {  // no game: the menu's picture, the interface alone over black
    if (!menuPicture_) {
      auto menu = std::make_unique<MenuPicture>();
      menu->picture.UseWindow(window);
      std::string error;
      if (!menu->picture.Open(vfs_, "", &error)) {
        Logf(LogLevel::Warning, "moho64ui: the window's menu picture did not open: %s", error.c_str());
        return;
      }
      menuPicture_ = std::move(menu);
    }
    MenuPicture& menu = *menuPicture_;
    const int w = target.Width(), h = target.Height();
    menu.picture.SetInterfaceLayer(target.pixels().data(), w, h, w, 0, 0, w, h);
    const bool vsync = Pacing().verticalSync;
    if (vsync != menu.vsync) menu.picture.SetVsync(vsync), menu.vsync = vsync;
    std::vector<uint8_t> rgba;
    if (!captureTo_.empty()) menu.picture.CaptureNextWindowFrame(&rgba);
    menu.picture.DrawToWindow(render::Camera(), w, h, 0.0);
    if (!captureTo_.empty()) SaveWindowCapture(rgba, w, h, captureTo_), captureTo_.clear();
    return;
  }
  if (!picture_->direct) return;
  Picture& pic = *picture_;
  const int w = target.Width(), h = target.Height();
  // The whole interface layer every frame (the renderer unions changed parts; the layer is drawn
  // anew each frame, so all of it counts as changed).
  pic.picture.SetInterfaceLayer(target.pixels().data(), w, h, w, 0, 0, w, h);
  const bool vsync = Pacing().verticalSync;
  if (vsync != pic.vsync) pic.picture.SetVsync(vsync), pic.vsync = vsync;
  std::vector<uint8_t> rgba;
  if (!captureTo_.empty()) pic.picture.CaptureNextWindowFrame(&rgba);
  const auto t0 = std::chrono::steady_clock::now();
  if (pic.cameraW > 0) pic.picture.DrawToWindow(pic.camera, w, h, PictureTick());
  FrameTimes& times = Times();
  times.worldDraw += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ++times.worldDraws;
  if (!captureTo_.empty()) SaveWindowCapture(rgba, w, h, captureTo_), captureTo_.clear();
#else
  (void)target;
#endif
}

void GameSession::CaptureWindow(const std::string& path) {
#ifdef MOHO64UI_WORLD_PICTURE
  captureTo_ = path;
#else
  (void)path;
#endif
}

void GameSession::OpenPictureIfDue() {
  if (!pictureDue_ || !window || !sim_) return;
  pictureDue_ = false;
  OpenPicture();
}

void GameSession::OpenPicture() {
#ifdef MOHO64UI_WORLD_PICTURE
  auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  {
    auto t3 = std::chrono::steady_clock::now();
    auto pic = std::make_unique<Picture>();
    pic->session = this;
    // (On the game's window when it has one: the renderer shows the picture there itself, the
    // interface laid over it, without reading it back; see PresentToWindow.)
    menuPicture_.reset();  // (one picture per window: the menu's goes first)
    if (window) pic->picture.UseWindow(window), pic->direct = true;
    std::string error;
    if (pic->picture.Open(vfs_, map_, &error)) {
      pic->picture.GameStarted(*sim_);
      // The picture is the player's: other armies' units show as the player's army knows them.
      pic->picture.SetFocusArmy(sim_->focusArmy + 1);
      // The map's edge, one of the two looks (the renderer's answer to ui request 20: all three
      // switches set, as their defaults are neither look).
      if (render::World* w = pic->picture.world()) {
        const bool frame = skyNow_.edge == "frame";
        w->worldBeyond = !frame;
        w->frame = frame;
        w->beyondHaze = false;
        Logf(LogLevel::Info, "moho64ui: session: the map's edge: %s", frame ? "the original's frame of machinery" : "no machinery, the land beyond");
      }
      if (skyNow_.startHour >= 0) {
        // Weather that changes by itself is the renderer's (by the game's seed and clock); one
        // kind that stays is set on its world here, with the values its own kinds have
        // (render/weather.cpp, kKinds), and with what that weather has done to the ground by now.
        struct Kind {
          const char* name;
          float clouds, rain, snow, fog;
        };
        static const Kind kKinds[] = {{"none", 0.35f, 0, 0, 0},     {"clear", 0.10f, 0, 0, 0},      {"cloudy", 0.65f, 0, 0, 0},
                                      {"overcast", 0.92f, 0, 0, 0.1f}, {"rain", 0.85f, 0.5f, 0, 0},  {"storm", 1.0f, 1.0f, 0, 0.25f},
                                      {"fog", 0.5f, 0, 0, 1.0f},    {"snow", 0.9f, 0, 0.8f, 0.1f}};
        const std::string& kind = skyNow_.weather;
        const bool byItself = kind == "changing" || kind == "cold" || kind == "dry";
        pic->picture.SetDayAndWeather(skyNow_.startHour, skyNow_.dayLengthSeconds, byItself);
        if (render::World* w = pic->picture.world()) {
          w->climate = kind == "cold" ? render::Climate::Cold : kind == "dry" ? render::Climate::Dry : render::Climate::Temperate;
          for (const Kind& k : kKinds) {
            if (kind != k.name) continue;
            w->cloudCover = k.clouds, w->rain = k.rain, w->snow = k.snow, w->fog = k.fog;
            w->wet = k.rain > 0 ? 1.0f : 0.0f;
            w->snowLying = k.snow > 0 ? 1.0f : 0.0f;
          }
        }
        Logf(LogLevel::Info, "moho64ui: session: the day starts at %.2f h, a day takes %.0f game seconds, weather \"%s\"",
             skyNow_.startHour, skyNow_.dayLengthSeconds, kind.c_str());
      }
      pic->picture.Beat(*sim_);
      picture_ = std::move(pic);
      Logf(LogLevel::Info, "moho64ui: session: the world's picture is open, %.2f s", secs(t3, std::chrono::steady_clock::now()));
    } else {
      Logf(LogLevel::Warning, "moho64ui: session: no picture of the world: %s", error.c_str());
    }
  }
#endif
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
    if (play_) {  // a replay: its own orders, beat by beat; the player's only pause and resume it
      if (takeOrders) ViewerControls(takeOrders());
      if (sim_->Paused()) {
        sim_->PausedBeat();
      } else if (!play_->AtEnd() && play_->ApplyUntilAdvance(*sim_)) {
        if (sim_->Paused()) sim_->PausedBeat();
        else TimedTick();
      } else {
        continue;  // the replay is over: the game stands where it ended
      }
      NoteBeat();
      for (std::string& sync : sim_->TakeSync()) {
        if (onSync) onSync(sync);
        else pendingSync_.push_back(std::move(sync));
      }
      continue;
    }
    if (takeOrders) {
      std::string orders = takeOrders();
      Record(orders);
      if (!orders.empty() && !ApplyCommands(*sim_, orders)) Logf(LogLevel::Warning, "moho64ui: an order was not understood by the simulation");
    }
    if (sim_->Paused()) sim_->PausedBeat();
    else {
      TimedTick();
      Record(CommandWriter::AdvanceMessage(1));
    }
    NoteBeat();
    for (std::string& sync : sim_->TakeSync()) {
      if (onSync) onSync(sync);
      else pendingSync_.push_back(std::move(sync));
    }
  }
}

// The player's orders while a replay plays: only his pause and resume count (the messages
// RequestPause and Resume of the command stream, sim/command_stream.cpp 4 and 5), and they pause
// the replay for him alone. INFERRED: what the original does with them in a replay (its replay
// has no command source of the player's); the replay's own pauses are played as recorded.
void GameSession::ViewerControls(const std::string& orders) {
  size_t p = 0;
  while (p + 3 <= orders.size()) {
    const int type = static_cast<unsigned char>(orders[p]);
    const size_t length = static_cast<unsigned char>(orders[p + 1]) | (static_cast<size_t>(static_cast<unsigned char>(orders[p + 2])) << 8);
    if (length < 3) break;
    if (type == 4) sim_->pauseOwner = 0;
    else if (type == 5) sim_->pauseOwner = -1;
    p += length;
  }
}

void GameSession::TimedTick() {  // (a beat of the simulation, timed for the window's log)
  const auto ts = std::chrono::steady_clock::now();
  sim_->Tick();
  Times().simBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
  ++Times().beats;
}

void GameSession::NoteBeat() {
#ifdef MOHO64UI_WORLD_PICTURE
  const auto tp = std::chrono::steady_clock::now();
  if (picture_) picture_->picture.Beat(*sim_);
  Times().pictureBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count();
#endif
  const auto tsnd = std::chrono::steady_clock::now();
  PlayBeatSounds();
  Times().soundBeats += std::chrono::duration<double>(std::chrono::steady_clock::now() - tsnd).count();
  // The beat's camera shakes (the engine's ShakeEvents) go to the world's cameras.
  std::vector<ShakeEvent> shakes;
  ShakeEvents(*sim_, &shakes);
  for (const ShakeEvent& e : shakes) PostCameraShake(e.position, e.radius, e.maxShakeEpicenter, e.minShakeAtRadius, e.interval);
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
      Logf(LogLevel::Info, "moho64ui: sound: tick %u %s entity %u army %d at %.0f %.0f %.0f: %s/%s (%s)", static_cast<unsigned>(sim_->tick()),
           e.kind == SoundEvent::Play ? "play" : "loop", static_cast<unsigned>(e.entity), e.army, e.position[0], e.position[1],
           e.position[2], e.bank.c_str(), e.cue.c_str(), ours ? "ours" : seen(e.entity, e.position) ? "seen" : "not seen");
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
  // (A replay's first beat is its stream's first: what came before the first Advance, then it.)
  if (play_ && !play_->AtEnd()) play_->ApplyUntilAdvance(*sim_);
  sim_->Tick();
  Record(CommandWriter::AdvanceMessage(1));
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
