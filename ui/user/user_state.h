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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "script/lua.hpp"
#include "script/script_handle.h"

namespace moho {
class Sim;
class Vfs;
class ScriptState;
class ThreadScheduler;
}  // namespace moho

namespace moho::ui {

class WorldSource;
struct WorldLine;
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
    // The script that sets the state up: the front end's, or /lua/SessionInit.lua for the state
    // a game's interface runs in (the original makes a new state for every session, FA exe 0x885de0).
    std::string bootScript = "/lua/userInit.lua";
    // The running simulation a session's state looks at; nullptr for the front end's state.
    Sim* sim = nullptr;
    // The command source the player's orders are written for (the focus army's first source).
    int commandSource = 0;
    // Where a session's world views get the world's picture from; nullptr: there is none.
    WorldSource* world = nullptr;
  };

  UserState(Vfs* vfs, Config config);
  ~UserState();
  UserState(const UserState&) = delete;
  UserState& operator=(const UserState&) = delete;

  bool Boot();             // /lua/userInit.lua
  bool StartMainScript();  // the root frames, then uimain.SetupUI()
  bool StartFrontEnd();    // uimain.StartFrontEndUI()
  // uimain.StartGameUI() in a session's state (FA exe 0x83d240): the game script makes its world
  // interface provider, which is told at once that loading starts.
  bool StartGame();
  // The world is there: provider:CreateGameInterface(isReplay), then provider:StopLoadingDialog().
  // (Inferred: that order, and that nothing lies between them.)
  void GameLoaded();
  // provider:<fn>(args) with the `nargs` values on top of the stack; nothing happens without a provider.
  void CallProvider(const char* fn, int nargs = 0);
  // A beat's sync table from the simulation (bytes as Sim::TakeSync gives them).
  void DeliverSync(const std::string& blob);
  std::unique_ptr<ScriptObject> provider;  // the game script's world interface provider, once made
  bool gameInterface = false;              // the game's interface has been created (GameLoaded ran)
  // What a part of the engine side keeps for this state (the units of user_units.cpp).
  struct Extra {
    virtual ~Extra() = default;
  };
  std::unique_ptr<Extra> units;
  std::map<std::string, std::unique_ptr<ScriptObject>> cameras;  // the cameras by name (maui/worldview.cpp)

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
// The world interface provider and a running session's facts (session_bindings.cpp).
void RegisterSessionBindings(lua_State* L);
// Units as the interface sees them, selection (user_units.cpp). RefreshUserUnits brings them up
// to date with the simulation; InstallUnitCategoryFunctions goes after the rules are in the state.
void RegisterUserUnitBindings(lua_State* L);
void RefreshUserUnits(lua_State* L);
void InstallUnitCategoryFunctions(lua_State* L);
// Orders (user_units.cpp): an order with a place as its target for the selected units of the
// player's army ("Move", "Attack", "Patrol", "AggressiveMove", or "BuildMobile" with the
// blueprint to build); false when no unit is selected or the order is not one of these. The game
// script hears of it (commandmode.OnCommandIssued). And the orders written since the last call,
// as the command stream's bytes for the engine's ApplyCommands.
bool IssueToSelection(lua_State* L, const std::string& order, float x, float y, float z, bool clear,
                      const std::string& blueprint = std::string());
// The same with a unit as the target ("Attack", "Guard", "Repair", "Reclaim", "Capture", "Move":
// the unit's entity id); and what a right click orders by the unit under the cursor (0xffffffff:
// none): "RULEUCC_Move", "RULEUCC_Attack", ..., empty for nothing.
bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear);
// The living unit nearest to a place of the map, no farther than `radius` (0xffffffff: none): what
// stands in for the picture's answer when there is no picture.
uint32_t UnitNearPlace(lua_State* L, float x, float z, float radius);
std::string RightClickOrder(lua_State* L, uint32_t under);
// Select the player's own units among these entity ids (what a click or a box in the world
// found); with `add` they join the selection instead of replacing it.
void SelectUnitsById(lua_State* L, const std::vector<uint32_t>& ids, bool add);
// For the world's picture: the selected units' entity ids, and the lines of their orders (from
// each unit to where its orders take it, coloured by the kind of order).
void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldLine>* lines);
std::string TakeOrders(lua_State* L);
// The lobby object in its local form (lobby.cpp).
void RegisterLobbyBindings(lua_State* L);

}  // namespace moho::ui
