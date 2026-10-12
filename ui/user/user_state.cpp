#include "user/user_state.h"

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
  InitFonts(vfs);
  if (!config_.prefsFile.empty()) LoadPrefs(L, config_.prefsFile);
  // Hooks of the active mods apply to the scripts of this state as well. (The original's front end
  // takes the active mods from the preferences; here they come from the host's mod list.)
  SetActiveMods(*state_, config_.modUids);
}

UserState::~UserState() {
  ui_.reset();  // the controls let go of their Lua tables while the state is still there
  threads_.reset();
  state_.reset();
}

lua_State* UserState::L() const { return state_->L(); }

UserState* UserState::From(lua_State* L) { return static_cast<UserState*>(lua_getextra(L, 1)); }

bool UserState::Boot() {
  bool ok = state_->DoScript("/lua/userInit.lua");
  Logf(LogLevel::Info, "moho64ui: userInit.lua %s", ok ? "ok" : "FAILED");
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
bool UserState::StartMainScript() {
  lua_State* L = state_->L();
  lua_newtable(L);
  lua_setglobal(L, "__EngineStats");
  if (!ui_->CreateRootFrames(L, config_.width, config_.height)) {
    Logf(LogLevel::Error, "CUIManager::Init - unable to create root frame for head %d.", 0);
    return false;
  }
  return CallUiMain("SetupUI");
}

bool UserState::StartFrontEnd() {
  uiState_ = "frontend";
  return CallUiMain("StartFrontEndUI");
}

void UserState::Frame(double seconds) {
  now_ += seconds;
  ++frame_;
  ui_->Update(state_->L(), static_cast<float>(seconds));
  threads_->RunTick(frame_);
}

}  // namespace moho::ui
