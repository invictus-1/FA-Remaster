#include "user/user_state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "user/keymap.h"

#include "core/log.h"
#include "core/vfs.h"
#include "maui/control.h"
#include "maui/font.h"
#include "script/script_state.h"
#include "script/threads.h"
#include "user/bindings.h"

namespace moho::ui {

UserState::UserState(Vfs* vfs, Config config) : config_(std::move(config)) {
  state_ = std::make_unique<ScriptState>(ScriptState::Kind::User, vfs);
  state_->SetHookDirs(config_.hookDirs);
  lua_State* L = state_->L();
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
  threads_->RunTick(frame_);
}

}  // namespace moho::ui
