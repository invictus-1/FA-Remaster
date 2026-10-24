#include "user/user_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>

#include "user/keymap.h"

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "script/threads.h"
#include "user/bindings.h"
#include "user/replay_timeline.h"

extern "C" {
extern void (*lua_gpg_prof_gc)(lua_State* L, int end);  // (third_party/lua/lgc.c: around a collection's mark and sweep)
}

namespace moho::ui {

namespace {
// The long-frame log's watch on the Lua collections (MOHO64UI_LONG_FRAMES): whose state collected, and for how long.
std::vector<const UserState*> g_liveStates;  // (the window's thread's)
std::thread::id g_windowThread;
void (*g_gcHookBefore)(lua_State*, int) = nullptr;  // (the engine's profile's, MOHO64_PROFILE_HITCH, when it was there first)
std::atomic<long long> g_beatThreadGcUs{0};
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void LongFrameGcHook(lua_State* L, int end) {
  if (g_gcHookBefore) g_gcHookBefore(L, end);
  thread_local double began = 0;
  if (!end) {
    began = NowMs();
    return;
  }
  const double ms = NowMs() - began;
  if (std::this_thread::get_id() != g_windowThread) {
    g_beatThreadGcUs += static_cast<long long>(ms * 1000.0);
    return;
  }
  const auto* owner = static_cast<const UserState*>(lua_getextra(L, 1));
  const bool ours = std::find(g_liveStates.begin(), g_liveStates.end(), owner) != g_liveStates.end();
  // (with the heap left after it, in MB: the size of the next one's work)
  static std::set<std::string> names;
  const std::string name = std::string(ours ? "(a Lua collection: the interface's state, " : "(a Lua collection: the simulation's state, ") +
                           std::to_string(lua_getgccount(L) / 1024) + " MB left)";
  LongFrames().Note(names.insert(name).first->c_str(), ms);
}
}  // namespace

LongFrameLog& LongFrames() {
  static LongFrameLog log = [] {
    LongFrameLog l;
    if (const char* v = std::getenv("MOHO64UI_LONG_FRAMES")) {
      l.thresholdMs = std::atof(v) > 0 ? std::atof(v) : 16.0;
      g_windowThread = std::this_thread::get_id();
      g_gcHookBefore = lua_gpg_prof_gc;
      lua_gpg_prof_gc = LongFrameGcHook;
      Logf(LogLevel::Info, "moho64ui: frames longer than %.1f ms are logged (MOHO64UI_LONG_FRAMES)", l.thresholdMs);
    }
    return l;
  }();
  return log;
}
double BeatThreadGcMs() { return static_cast<double>(g_beatThreadGcUs.load()) / 1000.0; }

UserState::UserState(Vfs* vfs, Config config) : config_(std::move(config)) {
  LongFrames();  // (the long-frame log's hooks, on the window's thread)
  g_liveStates.push_back(this);
  state_ = std::make_unique<ScriptState>(ScriptState::Kind::User, vfs);
  state_->SetHookDirs(config_.hookDirs);
  lua_State* L = state_->L();
  // The engine's incremental collector (third_party/lua/lgcinc.c), stepped once a frame (Frame): Lua 5.0's own
  // collector stops the frame for a whole collection, 22-38 ms at 4K in a 4v4 every 30-40 s. Ours, not the
  // original's (it forces a full one every 5 s, IUIManager::UpdateFrameRate 0x84d267). MOHO64UI_GC=classic keeps
  // 5.0's (to compare).
  const char* gcMode = std::getenv("MOHO64UI_GC");
  incrementalGc_ = !(gcMode && std::string(gcMode) == "classic") && lua_gpg_setgcmode(L, 1) == 1;
  lua_setextra(L, 1, this);
  threads_ = std::make_unique<ThreadScheduler>(L);
  ui_ = std::make_unique<UiManager>();
  RegisterThreadBindings(L, threads_.get());
  RegisterUserClasses(L);
  RegisterPrefsBindings(L);
  RegisterBasicBindings(L);
  RegisterControlBindings(L);
  RegisterFrameBindings(L);
  RegisterBitmapBindings(L);
  RegisterTextBindings(L);
  RegisterScrollbarBindings(L);
  RegisterItemListBindings(L);
  RegisterEditBindings(L);
  RegisterSimpleControlBindings(L);
  RegisterInputBindings(L);
  RegisterWindowTestBindings(L);
  RegisterLobbyBindings(L);
  RegisterSpecialFileBindings(L);
  RegisterKeyMapBindings(L);
  RegisterWorldViewBindings(L);
  RegisterSessionBindings(L);
  RegisterTimelineBindings(L);
  RegisterSoundBindings(L);
  RegisterUserUnitBindings(L);
  RegisterRangeBindings(L);
  RegisterWorldMeshBindings(L);
  RegisterMapPreviewBindings(L);
  InitFonts(vfs);
  if (!config_.prefsFile.empty()) LoadPrefs(L, config_.prefsFile);
  // Hooks of the active mods apply to the scripts of this state as well. (The original's front end
  // takes the active mods from the preferences; here they come from the host's mod list.)
  SetActiveMods(*state_, config_.modUids);
}

UserState::~UserState() {
  g_liveStates.erase(std::remove(g_liveStates.begin(), g_liveStates.end(), this), g_liveStates.end());
  units.reset();
  ranges.reset();
  worldMeshes.reset();  // (its meshes let go of their Lua tables: before the state closes; a crash at the end of a run before)
  provider.reset();
  cameras.clear();
  ui_.reset();  // the controls let go of their Lua tables while the state is still there
  threads_.reset();
  state_.reset();
}

lua_State* UserState::L() const { return state_->L(); }

Sim* UserState::LiveSim(const char* where) {
  if (config_.liveSim && config_.waitForBeat) {
    static const bool count = std::getenv("MOHO64UI_BEAT_LOG") != nullptr;
    if (count) {
      const auto t0 = std::chrono::steady_clock::now();
      config_.waitForBeat();
      const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (waited > 0.0005) {
        const char* in = CurrentBindingName();
        Logf(LogLevel::Info, "moho64ui: waited %.1f ms for the beat at %s%s%s", waited * 1000.0, *where ? where : "a script's call",
             *in ? " in " : "", in);
      }
    } else {
      config_.waitForBeat();
    }
  }
  return config_.liveSim;
}

UserState* UserState::From(lua_State* L) { return static_cast<UserState*>(lua_getextra(L, 1)); }

bool UserState::Boot() {
  bool ok = state_->DoScript(config_.bootScript);
  Logf(LogLevel::Info, "moho64ui: %s %s", config_.bootScript.c_str(), ok ? "ok" : "FAILED");
  return ok;
}

bool UserState::CallUiMain(const char* fn) {
  lua_State* L = state_->L();
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/ui/uimain.lua");
  bool ok = lua_pcall(L, 1, 1, top + 1) == 0;
  if (ok) {
    lua_pushstring(L, fn);
    lua_gettable(L, -2);
    ok = lua_pcall(L, 0, 0, top + 1) == 0;
  }
  if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, top);
  Logf(LogLevel::Info, "moho64ui: uimain.%s %s", fn, ok ? "ok" : "FAILED");
  return ok;
}

bool UserState::RunString(const std::string& code, const std::string& chunkName) {
  return state_->DoString(code, chunkName);
}

// The original's interface manager (FA exe 0x84cc50): the global __EngineStats, a root frame per
// head, then the main script.
void UserState::SetHeadSize(int width, int height) {
  config_.width = width, config_.height = height;
  ui_->Resize(width, height);
}

bool UserState::StartMainScript() {
  lua_State* L = state_->L();
  // __EngineStats: the original keeps its statistics tree here; ours is the empty tree (a name
  // and no children) until there are statistics to show.
  lua_newtable(L);
  lua_pushstring(L, "Name");
  lua_pushstring(L, "");
  lua_rawset(L, -3);
  lua_pushstring(L, "Children");
  lua_newtable(L);
  lua_rawset(L, -3);
  lua_setglobal(L, "__EngineStats");
  if (!ui_->CreateRootFrames(L, config_.width, config_.height)) {
    Logf(LogLevel::Error, "CUIManager::Init - unable to create root frame for head %d.", 0);
    return false;
  }
  if (!CallUiMain("SetupUI")) return false;
  // The key handler's start (FA exe 0x839690): key names and the default bindings. Inferred:
  // that this comes after SetupUI.
  LoadKeyMappings(L);
  return true;
}

bool UserState::StartLoadingScreen() {
  uiState_ = "game";  // (the original's interface mode 3 from here: GetCurrentUIState() is "game")
  for (int h = 0; h < ui_->NumFrames(); ++h) ui_->GetFrame(h)->ClearChildren(state_->L());
  if (!CallUiMain("SetupUI") || !CallUiMain("StartGameUI")) return false;
  CallProvider("StartLoadingDialog");
  return true;
}

void UserState::EndLoadingScreen() {
  for (int h = 0; h < ui_->NumFrames(); ++h) ui_->GetFrame(h)->ClearChildren(state_->L());
  if (provider) provider->UnbindLua();
  provider.reset();
}

bool UserState::StartGame() {
  uiState_ = "game";
  return CallUiMain("StartGameUI");
}

void UserState::GameLoaded() {
  ++syncCount;
  RefreshUserUnits(state_->L());
  CopyFromSimulation();
  // CreateGameInterface(isReplay): a replay's screen has no orders panel (FAF's gamemain.lua 286).
  lua_State* L = state_->L();
  const int top = lua_gettop(L);
  lua_getglobal(L, "SessionIsReplay");
  const bool replay = lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_toboolean(L, -1);
  lua_settop(L, top);
  // The original's order (DoInitializing 0x88c3f0, after the setup's beat): StopLoadingDialog
  // (0x88c577: FAF lays its fading "IN TRANSIT" cover over the screen), then CreateGameInterface
  // (0x88c5cd), then OnStart; the game's beats run after that.
  CallProvider("StopLoadingDialog");
  lua_pushboolean(L, replay ? 1 : 0);
  CallProvider("CreateGameInterface", 1);
  gameInterface = true;
}

bool UserState::StartFrontEnd() {
  uiState_ = "frontend";
  return CallUiMain("StartFrontEndUI");
}

double UserState::clockNow() const {
  if (!config_.liveClock) return now_;
  const double real = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return now_ + std::clamp(real - frameBegan_, 0.0, 0.25);
}

void UserState::Frame(double seconds) {
  now_ += seconds;
  frameBegan_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  ++frame_;
  AdvanceTickFraction(seconds);  // (the game's clock first, as the original's SessionFrame)
  ui_->Update(state_->L(), static_cast<float>(seconds));
  AdvanceCameras(state_->L(), static_cast<float>(seconds));
  DeliverChat(state_->L());
  DeliverConsoleLines(state_->L());  // (the console's lines from other threads: user/keymap.cpp)
  threads_->RunTick(frame_);
  // (the collector's step: every frame, menus and loading screens too, as an incremental state no longer collects by
  // itself; `point' always, the interface's collections need not fall on the game's beats)
  if (incrementalGc_) lua_gpg_gcstep(state_->L(), 1);
}

}  // namespace moho::ui
