#include "app/menu_flight.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "core/log.h"
#include "core/vfs.h"
#include "core/workers.h"
#include "sim/fingerprint.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/sim_view.h"
#include "sim/terrain.h"
#include "sound/sound_system.h"

#ifdef MOHO64UI_WORLD_PICTURE
#include "render/cinecam.h"
#endif

namespace moho::ui {

// The flight's own thread: one job at a time (the simulation's load, or a beat), asked for and waited on from the
// main thread (as GameSession's beats' thread; macOS gives a new thread 512 KB of stack, the simulation's start and its
// beats run deep Lua: 8 MB there as on the main thread).
class MenuFlight::Worker {
 public:
  Worker() { StartThread(); }
  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    wake_.notify_all();
    JoinThread();
  }
  void Start(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = std::move(job);
      running_.store(true, std::memory_order_release);
    }
    wake_.notify_all();
  }
  bool Running() const { return running_.load(std::memory_order_acquire); }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return !running_.load(std::memory_order_acquire); });
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return quit_ || job_; });
        if (quit_) return;
        job = std::move(job_);
        job_ = nullptr;
      }
      job();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false, std::memory_order_release);
      }
      done_.notify_all();
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  std::function<void()> job_;
  std::atomic<bool> running_{false};
  bool quit_ = false;
#if defined(__APPLE__)
  pthread_t thread_{};
  void StartThread() {
    void* (*run)(void*) = [](void* self) -> void* {
      static_cast<Worker*>(self)->Run();
      return nullptr;
    };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    int made = pthread_create(&thread_, &attr, run, this);
    pthread_attr_destroy(&attr);
    if (made != 0) made = pthread_create(&thread_, nullptr, run, this);
    if (made != 0) {
      Logf(LogLevel::Error, "moho64ui: the menu flight's thread could not be made (%d)", made);
      std::abort();
    }
  }
  void JoinThread() { pthread_join(thread_, nullptr); }
#else
  std::thread thread_;
  void StartThread() { thread_ = std::thread([this] { Run(); }); }
  void JoinThread() { thread_.join(); }
#endif
};

MenuFlight::MenuFlight(Vfs* vfs, std::vector<std::string> hookDirs) : vfs_(vfs), hookDirs_(std::move(hookDirs)) {
  fast = std::getenv("MOHO64UI_MENUFLIGHT_FAST") != nullptr && std::string(std::getenv("MOHO64UI_MENUFLIGHT_FAST")) == "1";
  if (const char* t = std::getenv("MOHO64UI_MENUFLIGHT_THREADS")) simThreads = std::max(1, std::atoi(t));
  if (const char* t = std::getenv("MOHO64UI_MENUFLIGHT_FAST_TO")) fastUntil = static_cast<uint32_t>(std::max(0, std::atoi(t)));
}

MenuFlight::~MenuFlight() {
  Stop();
  worker_.reset();
}

MenuFlight::State MenuFlight::state() const { return static_cast<State>(state_.load()); }
Sim* MenuFlight::sim() const {
  const State s = state();
  return s == State::Ready || s == State::Running || s == State::Ended ? sim_.get() : nullptr;
}
uint32_t MenuFlight::tick() const { return sim_ && state() != State::Loading ? sim_->tick() : 0; }

void MenuFlight::Fail(const std::string& why) {
  if (state() == State::Failed) return;
  Stop();
  failedWhy_ = why;
  state_ = static_cast<int>(State::Failed);
  Logf(LogLevel::Warning, "moho64ui: the menu's flight: %s", why.c_str());
}

// The session from menu_session.lua (the form of the tables --session takes: `GameConfig = {...}`), run in an
// environment table of its own in `L`, and built as a lobby's launch is (SessionFromGameConfig, the scenario file run
// in `L`). Its seed is fixed (the battle is the same on every run; MOHO64UI_MENUFLIGHT_SEED another), its focus army
// none, and nothing is recorded.
bool MenuFlight::Describe(lua_State* L) {
  if (session_) return true;
  if (state() == State::Failed) return false;
  if (!vfs_->Exists(kMap)) {
    Fail(std::string("its map is not there (") + kMap + ")");
    return false;
  }
  const auto text = vfs_->ReadFile(kSessionFile);
  if (!text) {
    Fail(std::string("cannot read ") + kSessionFile);
    return false;
  }
  const int top = lua_gettop(L);
  std::string why;
  if (luaL_loadbuffer(L, text->data(), text->size(), (std::string("@") + kSessionFile).c_str()) != 0) {
    why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
  } else {
    lua_newtable(L);  // (its environment: it sets GameConfig there and nothing in the front end's globals)
    lua_pushvalue(L, -1);
    lua_setfenv(L, -3);
    lua_insert(L, top + 1);
    if (lua_pcall(L, 0, 0, 0) != 0) why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
  }
  if (why.empty()) {
    lua_settop(L, top + 1);
    lua_pushstring(L, "GameConfig");
    lua_rawget(L, top + 1);
    const int config = lua_gettop(L);
    if (!lua_istable(L, config)) why = std::string(kSessionFile) + " sets no GameConfig";
    if (why.empty()) {
      // (the parallel order's version beside it, as a lobby's launch adds it: GameSession::LaunchWith)
      lua_getfield(L, config, "GameOptions");
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "RemasterSimOrderVersion");
        lua_pushnumber(L, Sim::kSimOrderVersion);
        lua_rawset(L, -3);
      }
      lua_pop(L, 1);
      LaunchParams params;
      params.seed = 1108;
      if (const char* seed = std::getenv("MOHO64UI_MENUFLIGHT_SEED")) params.seed = static_cast<uint32_t>(std::strtoul(seed, nullptr, 10));
      auto session = std::make_unique<SessionDescription>();
      if (SessionFromGameConfig(L, config, params, session.get(), &why)) {
        session->focusArmy = -1;  // (the picture shows every army: no player's view)
        session->createReplay = false;
        session_ = std::move(session);
      }
    }
  }
  lua_settop(L, top);
  if (!session_) {
    Fail("its session did not build (" + why + ")");
    return false;
  }
  // The camera's path: the renderer's CinePath text (render/cinecam.h Parse), one key a line; '#' to the line's end is
  // a note and goes before it is read (Parse knows none).
  if (const auto path = vfs_->ReadFile(kPathFile)) {
    std::string keys;
    size_t at = 0;
    while (at < path->size()) {
      size_t end = path->find('\n', at);
      if (end == std::string::npos) end = path->size();
      std::string line = path->substr(at, end - at);
      at = end + 1;
      const size_t hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
      keys += line + ";";
    }
    pathText_ = keys;
  }
  // (MOHO64UI_MENUFLIGHT_PATH=<keys>: a path tried without a rebuild, in the same text with ';' between the keys)
  if (const char* other = std::getenv("MOHO64UI_MENUFLIGHT_PATH"); other && *other) pathText_ = other;
  Logf(LogLevel::Info, "moho64ui: the menu's flight: session %s, %zu armies, seed %u%s", session_->header.map.c_str(), session_->header.armies.size(),
       session_->header.seed, pathText_.empty() ? "; NO camera path" : "");
  return true;
}

void MenuFlight::Begin() {
  if (!session_ || state() != State::Idle) return;
  if (!worker_) worker_ = std::make_unique<Worker>();
  const auto t0 = std::chrono::steady_clock::now();
  // The game's preferences file the rules read (sim/session.h kGamePrefsPath): its PreGameData's CurrentMapDir the map's
  // own folder, as a lobby's launch of a game on that map writes it (lobby.lua 2449), so that the map's own blueprints
  // load with the rules (SETONS_R's trees under /maps/SETONS_R/env: 11844 props). Set here, on this thread before the
  // load's, and on every start: the file holds what the last launch wrote (a game's map, or none: the first menu after
  // the program's start had none, the ones after a game on SETONS_R had them, and the battles differed from the first
  // beat, 2026-11-09). Our scenario's own folder (/moho64ui/menuflight, what a launch of menu_session.lua writes) has no
  // blueprints.
  if (!vfs_->SetMemoryFile(kGamePrefsPath, PreGameDataPrefs(MapFolderOf(session_->header.map))))
    Logf(LogLevel::Warning, "moho64ui: the menu's flight: %s could not be set", kGamePrefsPath);
  {
    ThreadsOverride threads(simThreads);  // (the pool is made with the Sim: one or two for the menu)
    sim_ = std::make_unique<Sim>(vfs_, hookDirs_, std::vector<std::string>{});
  }
  sim_->sessionMods = session_->header.mods;
  loadOk_ = false;
  loadWhy_.clear();
  beatToFinish_ = false;
  due_ = 0, lead_ = 0;
  endTick_ = 0;
  state_ = static_cast<int>(State::Loading);
  const bool picture = onBeat != nullptr;
  const bool sounds = collectSounds;
  worker_->Start([this, picture, sounds, t0] {
    Sim* sim = sim_.get();
    if (!sim->LoadRules()) {
      loadWhy_ = "the game's rules (blueprints) did not load";
      return;
    }
    // (what the picture takes of each beat, as a game's: GameSession::BeginLoad; nothing without a picture, or the lists
    // would only grow)
    sim->collectEffects = sim->collectProps = picture;
    sim->collectHits = sim->collectImpacts = sim->collectHealth = picture;
    sim->collectSounds = sounds;
    if (!sim->Start(*session_)) {
      loadWhy_ = "the simulation did not start";
      return;
    }
    loadSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    loadOk_ = true;
  });
}

void MenuFlight::WaitLoaded() {
  if (worker_ && state() == State::Loading) worker_->Wait();
}

void MenuFlight::Stop() {
  if (worker_) worker_->Wait();
  beatToFinish_ = false;
  if (fp_) std::fclose(fp_), fp_ = nullptr;
  sim_.reset();  // (here, not on a thread: a game's simulation may be made next, one at a time)
  if (state() != State::Failed) state_ = static_cast<int>(State::Idle);
}

void MenuFlight::Restart() {
  const auto t0 = std::chrono::steady_clock::now();
  Stop();
  ++loops_;
  cut_ = true;
  Begin();
  Logf(LogLevel::Info, "moho64ui: the menu's flight starts over (%d), the last simulation let go of in %.2f s", loops_,
       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

bool MenuFlight::TakeCut() {
  const bool c = cut_;
  cut_ = false;
  return c;
}

// (on the flight's thread: the tick, the picture's copy of it)
void MenuFlight::StartBeat() {
  beatFrom_ = sim_->tick();
  beatToFinish_ = true;
  worker_->Start([this] {
    const auto t0 = std::chrono::steady_clock::now();
    sim_->Tick();
    if (onBeat) onBeat(*sim_);
    beatSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  });
}

// (here, after the beat: its sync emptied, its fingerprint, the storyboard's end)
void MenuFlight::FinishBeat() {
  beatToFinish_ = false;
  sim_->TakeSync();  // (no interface reads them: kept, they would only pile up)
  sim_->TakePrints();
  if (const char* file = std::getenv("MOHO64UI_MENUFLIGHT_FP")) {
    if (!fp_) {
      const std::string name = std::string(file) + "." + std::to_string(loops_);
      fp_ = std::fopen(name.c_str(), "w");
      fpBeats_ = 0;
      if (fp_) std::fprintf(fp_, "%s\n", Fingerprint::StateLine(*sim_, 0).c_str());  // (beat 0's line is after the first beat's here)
    }
    if (fp_) std::fprintf(fp_, "%s\n", Fingerprint::StateLine(*sim_, ++fpBeats_).c_str());
  }
  // (Distant: the beat's once-off sounds, the battle's shots and blasts, played where they were; the listener is the
  // flight's camera, high over it: GameSession. The running sounds of units are left out.)
  if (sim_->collectSounds) {
    std::vector<SoundEvent> events;
    SoundEvents(*sim_, &events);
    if (sounds)
      for (const SoundEvent& e : events)
        if (e.kind == SoundEvent::Play && !e.bank.empty() && !e.cue.empty()) sounds->PlayAt(e.bank, e.cue, e.position);
  }
  lead_ = std::min(0.1, beatSeconds_ * 1.2);
  // The storyboard's end: its script's MenuFlight.ended (menu_battle.lua: a true global, _G.MenuFlight; the scenario's
  // script runs in ScenarioInfo.Env), read between beats; read raw, so that the sim's globals' guard for a name not
  // there (an error outside any protected call: the program would end) is never asked.
  lua_State* L = sim_->L();
  lua_pushstring(L, "MenuFlight");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "ended");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > 0) endTick_ = static_cast<uint32_t>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  if (endTick_ && state() == State::Running) {
    state_ = static_cast<int>(State::Ended);
    lastEndTick_ = endTick_;
    Logf(LogLevel::Info, "moho64ui: the menu's flight: the storyboard's end at tick %u (loop %d)", endTick_, loops_);
  }
}

void MenuFlight::Poll() {
  if (state() == State::Loading && worker_ && !worker_->Running()) {
    if (!loadOk_) {
      Fail(loadWhy_);
      return;
    }
    state_ = static_cast<int>(State::Ready);
    Logf(LogLevel::Info, "moho64ui: the menu's flight: its simulation is ready, %.2f s (%zu units)", loadSeconds_, sim_->units().size());
    if (!path_ && !pathText_.empty()) {
      const TerrainMap* map = sim_->map();
      ReadPath([map](float x, float z) { return map ? map->SurfaceHeight(x, z) : 0.0f; });
    }
  }
}

void MenuFlight::Advance(double seconds, bool shown) {
  using Clock = std::chrono::steady_clock;
  Poll();
  if (state() == State::Ready && shown) state_ = static_cast<int>(State::Running);  // (its first beat when first shown)
  if (state() != State::Running) {
    shownLast_ = shown;
    return;
  }
  if (!shown) {
    if (shownLast_) hiddenSince_ = Clock::now();
    shownLast_ = false;
    if (beatToFinish_ && !worker_->Running()) FinishBeat();
    return;
  }
  if (!shownLast_ && hiddenSince_ != Clock::time_point{} &&
      std::chrono::duration<double>(Clock::now() - hiddenSince_).count() > kRestartAfterHidden) {
    // (hidden a long while: the battle starts over as at its end, covered by the host)
    shownLast_ = true;
    hiddenSince_ = {};
    if (worker_->Running()) worker_->Wait();
    if (beatToFinish_) FinishBeat();
    state_ = static_cast<int>(State::Ended);
    Logf(LogLevel::Info, "moho64ui: the menu's flight: hidden over %.0f s, it starts over", kRestartAfterHidden);
    return;
  }
  shownLast_ = true;
  if (fast || (sim_->tick() < fastUntil && !beatToFinish_)) {  // (tests: up to 50 beats a frame, each one through before the next)
    for (int i = 0; i < 50 && state() == State::Running && (fast || sim_->tick() < fastUntil); ++i) {
      StartBeat();
      worker_->Wait();
      FinishBeat();
    }
    due_ = 0;
    return;
  }
  if (worker_->Running()) {  // (a beat on the thread: the time counts)
    due_ = std::min(1.0, due_ + seconds);
    return;
  }
  if (beatToFinish_) FinishBeat();
  if (state() != State::Running) return;
  due_ = std::min(1.0, due_ + seconds);
  // (one at a time on the thread, begun early by about as long as the last took: it is through when the picture's
  // moment reaches it, as GameSession::RunDueBeats)
  if (due_ >= 0.1 - lead_) {
    due_ -= 0.1;
    StartBeat();
  }
}

double MenuFlight::Moment() const {
  if (!sim_ || (state() != State::Running && state() != State::Ended)) return 0;
  const double tick = worker_ && worker_->Running() && beatToFinish_ ? static_cast<double>(beatFrom_) + 1.0 : static_cast<double>(sim_->tick());
  return tick + std::max(-1.0, std::min(1.0, due_ / 0.1));
}

double MenuFlight::pathLength() const {
#ifdef MOHO64UI_WORLD_PICTURE
  return path_ ? path_->Length() : 0;
#else
  return 0;
#endif
}

void MenuFlight::ReadPath(const std::function<float(float, float)>& ground) {
#ifdef MOHO64UI_WORLD_PICTURE
  auto path = std::make_unique<render::CinePath>();
  std::string error;
  if (!path->Parse(pathText_, ground, &error)) {
    Logf(LogLevel::Warning, "moho64ui: the menu's flight: its camera path cannot be read (%s)", error.c_str());
    return;
  }
  Logf(LogLevel::Info, "moho64ui: the menu's flight: a camera path of %zu keys, %.0f s", path->keys.size(), path->Length());
  path_ = std::move(path);
#else
  (void)ground;
#endif
}

bool MenuFlight::View(const std::function<float(float, float)>& ground, render::Camera* camera) const {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!path_ || path_->keys.empty() || (state() != State::Running && state() != State::Ended)) return false;
  // (the battle's seconds along the path: the camera meets the action where the storyboard puts it; past the path's
  // end it holds its last pose, the cover's, until the battle starts over)
  const float t = static_cast<float>(Moment() / 10.0) + path_->keys.front().time;
  *camera = render::CineCamera::MakeView(path_->At(t), ground);
  return true;
#else
  (void)ground, (void)camera;
  return false;
#endif
}

bool MenuFlight::StartView(render::Camera* camera) const {
#ifdef MOHO64UI_WORLD_PICTURE
  if (!path_ || path_->keys.empty() || !sim_ || !sim_->map()) return false;
  const TerrainMap* map = sim_->map();
  *camera = render::CineCamera::MakeView(path_->keys.front().pose, [map](float x, float z) { return map->SurfaceHeight(x, z); });
  return true;
#else
  (void)camera;
  return false;
#endif
}

// (MenuFlight::PushInfo's costOf, for tests)
static int FlightCostOf(lua_State* S) {
  luaL_checktype(S, 1, LUA_TTABLE);
  FlightCost cost;
  double mean = 0;
  bool known = false;
  for (int i = 1;; ++i) {
    lua_rawgeti(S, 1, i);
    if (!lua_istable(S, -1)) {
      lua_pop(S, 1);
      break;
    }
    lua_rawgeti(S, -1, 1);
    lua_rawgeti(S, -2, 2);
    lua_rawgeti(S, -3, 3);
    const double dt = lua_tonumber(S, -3), ms = lua_tonumber(S, -2);
    const bool drawn = lua_isnil(S, -1) || lua_toboolean(S, -1);
    lua_pop(S, 4);
    if (drawn && cost.Frame(dt, ms, &mean)) known = true;
  }
  if (known) {
    lua_pushnumber(S, mean);
    lua_pushnumber(S, cost.frames);
  } else {
    lua_pushnil(S);
    lua_pushnil(S);
  }
  lua_pushnumber(S, cost.shown);
  return 3;
}

void MenuFlight::PushInfo(lua_State* L) const {
  static const char* const kStates[] = {"idle", "loading", "ready", "running", "ended", "failed"};
  lua_newtable(L);
  auto str = [&](const char* k, const std::string& v) {
    lua_pushstring(L, k);
    lua_pushstring(L, v.c_str());
    lua_rawset(L, -3);
  };
  auto num = [&](const char* k, double v) {
    lua_pushstring(L, k);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
  };
  str("state", kStates[state_.load()]);
  str("map", session_ ? session_->header.map : std::string());
  str("fallbackReason", failedWhy_);
  num("tick", tick());
  num("moment", Moment());
  num("loops", loops_);
  num("endTick", endTick_);
  num("lastEndTick", lastEndTick_);
  num("pathLength", pathLength());
  num("units", sim_ && sim() ? static_cast<double>(sim_->units().size()) : 0);
  lua_pushstring(L, "paused");
  lua_pushboolean(L, !shownLast_);
  lua_rawset(L, -3);
  // costOf(frames) -> the mean (ms a frame) and the frames counted, or nil; then the seconds shown. For tests: the
  // host's own FlightCost played over a list of frames, each {dt, the card's ms, drawn (false: a frame that did
  // not draw the flight: not given to it, as GameSession gives none)}.
  lua_pushstring(L, "costOf");
  lua_pushcfunction(L, FlightCostOf);
  lua_rawset(L, -3);
}

}  // namespace moho::ui
