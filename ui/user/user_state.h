// The user Lua state: the state the game's interface scripts run in (/lua/ui, /lua/maui, ...).
//
// The original creates it at application start and runs /lua/userInit.lua in it. Its interface
// manager then makes the root frame of each head and enters the interface through
// /lua/ui/uimain.lua: SetupUI() first, then StartSplashScreen() or StartFrontEndUI() (and
// StartGameUI() when a session starts). Script threads of this state are stepped once per frame:
// WaitFrames is coroutine.yield.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho {
class Vfs;
class ScriptState;
class ThreadScheduler;
}  // namespace moho

namespace moho::ui {

class UiManager;

class UserState {
 public:
  struct Config {
    std::vector<std::string> hookDirs;  // the init file's `hook` table
    std::vector<std::string> modUids;   // active mods (their /hook folders apply to this state too)
    std::vector<std::string> gameArgs;  // the game's own command line, as GetCommandLineArg sees it
    std::string prefsFile;              // our preferences file; empty = preferences live in memory only
    std::string sessionFile;            // where a launched game's configuration is written; empty = to the log
    // Starts a session from the game configuration table at stack index `config` when the lobby
    // launches a game; false with the reason for the script's LaunchFailed. Unset: nothing starts.
    std::function<bool(lua_State* L, int config, std::string* why)> launchSession;
    int width = 1920, height = 1080;    // the head's size in pixels
  };

  UserState(Vfs* vfs, Config config);
  ~UserState();
  UserState(const UserState&) = delete;
  UserState& operator=(const UserState&) = delete;

  bool Boot();             // /lua/userInit.lua
  bool StartMainScript();  // the root frames, then uimain.SetupUI()
  bool StartFrontEnd();    // uimain.StartFrontEndUI()

  // One frame: the clock moves on by `seconds`; controls that asked for it get OnFrame; then the
  // script threads that are due run. (The order of those two within a frame is inferred.)
  void Frame(double seconds);

  // import('/lua/ui/uimain.lua').<fn>() with a traceback on error.
  bool CallUiMain(const char* fn);
  // Run a chunk of source in the state (errors are logged).
  bool RunString(const std::string& code, const std::string& chunkName);

  lua_State* L() const;
  UiManager& ui() { return *ui_; }
  const Config& config() const { return config_; }
  double now() const { return now_; }      // seconds since the state was created
  uint32_t frame() const { return frame_; }
  const std::string& uiState() const { return uiState_; }  // GetCurrentUIState(): none, splash, frontend, game

  static UserState* From(lua_State* L);

 private:
  Config config_;
  std::unique_ptr<ScriptState> state_;
  std::unique_ptr<ThreadScheduler> threads_;
  std::unique_ptr<UiManager> ui_;
  double now_ = 0;
  uint32_t frame_ = 0;
  std::string uiState_ = "none";
};

// Preferences: GetPreference / SetPreference / SavePreferences (prefs.cpp).
void RegisterPrefsBindings(lua_State* L);
void LoadPrefs(lua_State* L, const std::string& file);
// Time, command line, interface state, front-end data, Random (basics.cpp).
void RegisterBasicBindings(lua_State* L);
// The lobby object in its local form (lobby.cpp).
void RegisterLobbyBindings(lua_State* L);

}  // namespace moho::ui
