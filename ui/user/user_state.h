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
#include "sim/sim_view.h"

namespace moho {
class Sim;
class Vfs;
class ScriptState;
class ThreadScheduler;
}  // namespace moho

namespace moho::ui {

class WorldSource;
class BackdropSource;
struct GamePace;
struct WorldLine;
struct WorldScene;
struct WorldRangeGroup;
struct WorldMeshView;
struct WorldDecal;
struct WorldGhost;
class UiManager;
namespace sound {
class SoundSystem;
}

class UserState {
 public:
  struct Config {
    std::vector<std::string> hookDirs;  // the init file's `hook` table
    std::vector<std::string> protocols;  // the init file's `protocols` table (the schemes OpenURL may open)
    std::vector<std::string> modUids;   // active mods (their /hook folders apply to this state too)
    std::vector<std::string> gameArgs;  // the game's own command line, as GetCommandLineArg sees it
    std::string prefsFile;              // our preferences file (main.cpp: --prefs or the default); empty = memory only
    std::string sessionFile;            // where a launched game's configuration is written; empty = to the log
    // Starts a session from the game configuration table at stack index `config` when the lobby
    // launches a game; false with the reason for the script's LaunchFailed. Unset: nothing starts.
    std::function<bool(lua_State* L, int config, std::string* why)> launchSession;
    // Starts a single-player session (LaunchSinglePlayerSession: the campaign's operations) from
    // FAF's single-player table at stack index `info`; false with the reason. Unset: none starts.
    std::function<bool(lua_State* L, int info, std::string* why)> launchSinglePlayer;
    // The player's own files (replays, saved games): their folder (special_files.cpp), and the
    // host's replay hooks: start a session from a replay file (LaunchReplaySession; unset: none
    // can be started from this state) and the replay file of the game running or last played.
    std::string userDir;
    std::function<bool(lua_State* L, const std::string& file, std::string* why)> launchReplay;
    std::function<std::string()> currentReplay;
    // Whether a launched game is loading (the front end's WorldIsLoading(); unset: never).
    std::function<bool()> worldLoading;
    // The scripts' clock (CurrentTime, GetSystemTimeSeconds) runs on while a frame's scripts run, as the
    // original's GetSystemTimer does (FA exe 0x955730, which both read): on in a live window; off in a test
    // run, whose clock goes in fixed frame steps.
    bool liveClock = false;
    int width = 1920, height = 1080;    // the head's size in pixels
    // The script that sets the state up: the front end's, or /lua/SessionInit.lua for the state
    // a game's interface runs in (the original makes a new state for every session, FA exe 0x885de0).
    std::string bootScript = "/lua/userInit.lua";
    // The running simulation a session's state looks at; nullptr for the front end's state. Read
    // it through UserState::LiveSim (it waits for a beat running on its own thread), never here.
    Sim* liveSim = nullptr;
    // Waits until no beat runs on the beats' thread (the host's; unset: they never do).
    std::function<void()> waitForBeat;
    // The ground's height at a place of the map from the host's copy taken after each beat (asked
    // every frame: it does not wait for a beat); unset: none.
    std::function<float(float x, float z)> groundHeight;
    // Whether a beat runs on its own thread now (the host's; unset: never). What asks the
    // simulation every frame for something that can be a beat old answers from before it then.
    std::function<bool()> beatRunning;
    // The game's time with the frame's share of a beat, in seconds (the original's GameTimeSource,
    // 0x7a66a0: (beat + fraction) * 0.1; paused with the game, at its speed): the camera's game clock
    // (UseGameClock). Unset: there is no game.
    std::function<double()> gameTime;
    // The command source the player's orders are written for (the focus army's first source).
    int commandSource = 0;
    // The army that source belongs to, 0-based (-1: none, a replay's viewer): SetFocusArmy's checks.
    int ownArmy = -1;
    // Where a session's world views get the world's picture from; nullptr: there is none.
    WorldSource* world = nullptr;
    // The running game's pace (the host's); nullptr: there is no game.
    GamePace* pace = nullptr;
    // What is shown where the front end plays its background film (the main menu's): a moving
    // picture of the host's; nullptr: nothing is shown there (the film itself is not decoded).
    BackdropSource* backdrop = nullptr;
    // What plays the sounds the scripts ask for (the host's, one for all states); nullptr: nothing does.
    sound::SoundSystem* sound = nullptr;
  };

  UserState(Vfs* vfs, Config config);
  ~UserState();
  UserState(const UserState&) = delete;
  UserState& operator=(const UserState&) = delete;

  bool Boot();             // /lua/userInit.lua
  bool StartMainScript();  // the root frames, then uimain.SetupUI()
  bool StartFrontEnd();    // uimain.StartFrontEndUI()
  // The game's loading screen in the front end's own state, as the original's DoPreload shows it
  // (FA exe 0x88bee0, the frame after a launch): its root frames' controls destroyed (the original
  // makes new root frames there, 0x84cc50; ours stay, emptied), uimain.SetupUI and
  // uimain.StartGameUI (the game script's provider) again, then provider:StartLoadingDialog().
  // EndLoadingScreen: the game's own state takes over (DoInitializing 0x88c4ad); the loading
  // screen's controls go, and this state's provider with them (the session's replaces it, 0x86aa12).
  bool StartLoadingScreen();
  void EndLoadingScreen();
  // uimain.StartGameUI() in a session's state (FA exe 0x83d240 from DoInitializing): the game
  // script makes its world interface provider. (Its loading screen was the front end's.)
  bool StartGame();
  // The world is there (after the setup's beat): provider:StopLoadingDialog(), then
  // provider:CreateGameInterface(isReplay) (DoInitializing 0x88c577, 0x88c5cd).
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
  std::unique_ptr<Extra> ranges;  // the range rings' filters and switches (range_rings.cpp)
  std::unique_ptr<Extra> worldMeshes;  // the script's meshes in the world (world_meshes.cpp)
  std::map<std::string, std::unique_ptr<ScriptObject>> cameras;  // the cameras by name (maui/worldview.cpp)
  // The playable rectangle, x0, z0, x1, z1 (SyncPlayableRect; x1 < 0: the whole map): the cameras
  // keep to it (maui/worldview.cpp).
  float playableRect[4] = {0, 0, -1, -1};
  std::vector<std::string> mapBorders;  // the mesh blueprints of the map's border (MapBorderAdd)
  // The life bars over units (the console command UI_RenderUnitBars, user/keymap.cpp): the
  // player's own switch (-1: not read from the preferences yet) and whether a script has them
  // put away for now.
  int lifeBars = -1;
  bool lifeBarsPutAway = false;
  // The console's UI_ForceLifbarsOnEnemy (the game's option "Always Show Enemy Lifebars").
  bool lifeBarsOnEnemies = false;
  // RenderOverlayEconomy(bool) (FA exe 0x847d30: the session's +0x4e8; FAF's multifunction panel's
  // "economy" button): each of the player's units on screen shows what it makes or uses
  // (worldview.cpp, DrawEconomyOverlay).
  bool economyOverlay = false;
  // TeamColorMode (FA exe 0x847e10, patched by FAF): the switch (session+0x4e9) and FAF's colour
  // list, one 0xAARRGGBB per army counted from 0 (0: none given); IconColorsForWorld resolves them.
  bool teamColors = false;
  std::vector<uint32_t> teamColorTable;
  // The camera the cinematic camera took over from (the main world view's, by name) while it is on; empty while it
  // is off (worldview.cpp: CineCameraCommand, and the input that hands the camera back).
  std::string cineCamera;
  // The photo mode while it is open (worldview.cpp: PhotoModeCommand, PhotoModeKey, PhotoModeFrame): whether it
  // paused the game (and so lets it go on when it closes); the right button's drag since the last frame, in pixels (x
  // right, y down); the wheel's notches not given yet, as seconds of up (+) or down (-); a photo being made.
  struct PhotoModeState {
    bool open = false;
    bool paused = false;
    float drag[2] = {0, 0};
    float wheel = 0;
    bool taking = false;
  };
  PhotoModeState photoMode;

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
  // The head's size, also before the main script makes the root frames (the display mode the
  // player's option chose at the start).
  void SetHeadSize(int width, int height);
  // The running simulation, once no beat runs on its own thread (it waits for one that does; the
  // engine's rule: nothing of this state may look at the simulation while it ticks). Every look
  // from this state goes through here. `where`: for the count of waits (MOHO64UI_BEAT_LOG).
  Sim* LiveSim(const char* where = "");
  bool HasSim() const { return config_.liveSim != nullptr; }
  // What the scripts ask of the simulation every frame, copied when a beat's sync comes (the
  // simulation is idle then), so that asking never waits for a beat: the focus army's economy
  // (EconomyTotals of sim/sim_view.h, its twelve numbers in order). session_bindings.cpp.
  uint64_t syncCount = 0;  // the beats' syncs delivered so far (what was asked before one is old after it)
  float economyCopy[12] = {};
  std::string focusNickname;  // the focus army's player name (GetSessionClients)
  int focusArmyCopy = -1;     // the simulation's focusArmy, 0-based (GetFocusArmy)
  uint32_t tickCopy = 0;      // its tick (GameTick, GameTime, GetGameTimeSeconds, GetGameTime)
  // The part of a tick the game's clock is past the last sync (0..1), as the original's session keeps
  // it (CWldSession+0x460, CWldSession::SessionFrame 0x895b40): each frame it grows by the game's speed
  // times ten times the frame's seconds while the last sync ticked, else it is 1; a sync that ticked
  // takes its ticks off. GameTime, GetGameTimeSeconds and GetGameTime add it; GameTick does not.
  double tickFraction = 0;
  bool lastSyncTicked = true;
  void AdvanceTickFraction(double seconds);
  bool isReplayCopy = false;  // SessionIsReplay
  std::vector<ArmyView> armiesCopy;  // ArmyViews (GetArmiesTable)
  bool economyCopied = false;  // (these copies are there: one beat's sync came)
  void CopyFromSimulation();
  double now() const { return now_; }      // seconds since the state was created, as of this frame
  // now() and, with Config::liveClock, the real time since this frame began (at most a frame's longest
  // step, 0.25 s: the clock never runs back when the next frame starts).
  double clockNow() const;
  uint32_t frame() const { return frame_; }
  const std::string& uiState() const { return uiState_; }  // GetCurrentUIState(): none, splash, frontend, game

  static UserState* From(lua_State* L);

 private:
  Config config_;
  std::unique_ptr<ScriptState> state_;
  std::unique_ptr<ThreadScheduler> threads_;
  std::unique_ptr<UiManager> ui_;
  double now_ = 0;
  double frameBegan_ = 0;  // (steady clock seconds when this frame began: clockNow)
  uint32_t frame_ = 0;
  std::string uiState_ = "none";
};

// Preferences: GetPreference / SetPreference / SavePreferences (prefs.cpp).
void RegisterPrefsBindings(lua_State* L);
void LoadPrefs(lua_State* L, const std::string& file);
// Take over the original game's profiles once, when our preferences file does not exist yet
// (Chris's decision, 2026-10-12: take over the player's existing profile once, on first start; read
// his Game.prefs, never write it): `source` (the original's Game.prefs) is read, never written, and of each
// profile with a name only Name, options, UserKeyMap, UserDebugKeyMap, UserKeyMapName and
// UserKeyActions go into this state's preferences, with profile.current. Nothing else of it (no
// mod lists, window places, lobby choices). Returns how many profiles were taken; 0 with the
// reason when none (the file cannot be read, does not load, its version.major is not 1, or it has
// no named profile), the preferences then left as they were.
int ImportPreferences(lua_State* L, const std::string& source, std::string* why);
// Write that state's preferences to the host's file (SavePreferences() from the engine's side). The
// original keeps one store for every state and also writes it when a game is launched
// (func_DoPreload 0x88bee0, USER_SavePreferences 0x8c91a0) and when the program closes
// (CScApp::Destroy 0x8d0f20); our states each keep a copy, so the host writes at those moments
// and when a game's state goes (what it changed would be lost with it).
void SavePreferencesOf(lua_State* L);
// The text of the game's own /preferences/Game.prefs (moho::kGamePrefsPath, held in memory: sim/session.h),
// which FAF's /lua/system/Blueprints.lua reads for PreGameData (LoadPreGameData: CurrentMapDir, whose *.bp files
// load with the rules; IconReplacements): `PreGameData = { ... }` with CurrentMapDir = `mapDir` (the session's
// map folder, as the lobby writes it: moho::SessionMapFolder) and the rest of what the lobby left in the
// preferences of L (nullptr: none). Set before the rules load (the original saves its preferences then:
// DoPreload 0x88bee0).
std::string PreGameDataText(lua_State* L, const std::string& mapDir);
// Time, command line, interface state, front-end data, Random (basics.cpp).
void RegisterBasicBindings(lua_State* L);
// A script has asked for the program to end (ExitApplication): the host's loops stop after the
// frame. One flag for the whole program, whichever state asked.
bool ExitRequested();
// A script has asked to leave the running game (ExitGame: back to the front end) or to start it
// over (RestartSession): 1 or 2, once, then 0 again. The host acts on it between two frames.
int TakeSessionRequest();
// The world interface provider and a running session's facts (session_bindings.cpp).
void RegisterSessionBindings(lua_State* L);
// Units as the interface sees them, selection (user_units.cpp). RefreshUserUnits brings them up
// to date with the simulation; InstallUnitCategoryFunctions goes after the rules are in the state.
void RegisterUserUnitBindings(lua_State* L);
void RefreshUserUnits(lua_State* L);
// A command's id as the original hands it to scripts: a SIGNED int made a Lua number (a float). GetCommandQueue's "ID"
// (FAF's patch: SetInteger, FA exe 0x129115c) and commandgraph.lua's OnCommandDragEnd(event, cmdId) (Call_ObjectInt,
// 0x823f94). So an engine-made command's id 0x80000000 | index comes out negative and rounded to 24 bits, there too.
// Every id we hand to Lua goes through this, so that the scripts' comparisons match.
inline void PushCommandId(lua_State* L, uint32_t id) { lua_pushnumber(L, static_cast<lua_Number>(static_cast<int32_t>(id))); }
// PlaySound, StopSound, SetVolume and the others (sound_bindings.cpp).
void RegisterSoundBindings(lua_State* L);
// The host's test functions that write a file a script names (moho64ui_SoundMix's file) write it only in a run
// started with a test script (--lua or --game-lua), never for a game's or a mod's script (sound_bindings.cpp). The
// same gate keeps the tests' made-up time of the graphics card (moho64ui_SetFrameCostForTests, FrameCostMeter).
void AllowTestFileWrites(bool allowed);
bool TestFileWritesAllowed();
// GetSpecialFiles, LaunchReplaySession and their kin: replays and saved games (special_files.cpp).
void RegisterSpecialFileBindings(lua_State* L);
void InstallUnitCategoryFunctions(lua_State* L);
// Orders (user_units.cpp): an order with a place as its target for the selected units of the
// player's army ("Move", "Attack", "Patrol", "AggressiveMove", or "BuildMobile" with the
// blueprint to build); false when no unit is selected or the order is not one of these. The game
// script hears of it (commandmode.OnCommandIssued). And the orders written since the last call,
// as the command stream's bytes for the engine's ApplyCommands.
bool IssueToSelection(lua_State* L, const std::string& order, float x, float y, float z, bool clear,
                      const std::string& blueprint = std::string());
// A right click's order held and dragged (CFormation, FA exe 0x838860): `form` turns a group's Move, Attack on a unit or
// Patrol into FormMove, FormAttack or FormPatrol; `heading` (radians about the up axis; NaN: the units' mean towards
// the target) is the formation's facing.
bool IssueFormationOrder(lua_State* L, const std::string& order, float x, float y, float z, uint32_t entity, bool clear,
                         bool ctrl, bool form, float heading);
// The same with a unit as the target ("Attack", "Guard", "Repair", "Reclaim", "Capture", "Move":
// the unit's entity id); and what a right click orders by the unit under the cursor (0xffffffff:
// none): "RULEUCC_Move", "RULEUCC_Attack", ..., empty for nothing. `coordWith` (an Attack): the order it is
// coordinated with (the command stream's coordinated id; 0xffffffff none): a double click on another attack's
// waypoint (AttackClick in user_units.cpp).
bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear, bool ctrl = false,
                        bool form = false, float heading = NAN, uint32_t coordWith = 0xffffffffu);
// The right click's transport orders on a unit ("CallTransport", "Transport": RightClickOrder's answers).
bool IssueTransportOrderAt(lua_State* L, const std::string& order, uint32_t entity, bool clear);
// A Script order at a place for the selected units, as the game's own script allows it
// (VerifyScriptCommand): FAF's attack-move, an ability. True when the click is used up.
bool IssueScriptAt(lua_State* L, float x, float y, float z, bool clear);
// FAF's attack-move to a place for the selected units (Alt and a right click on the ground).
bool IssueAttackMoveAt(lua_State* L, float x, float y, float z, bool clear);
// Where the main camera listens from, for the world's sounds (worldview.cpp): its eye, the place
// it looks at, the direction to the screen's right along the ground, and its zoom. False before
// the camera is placed.
bool CameraListener(lua_State* L, float eye[3], float focus[3], float right[3], float* zoom);
// A blast shakes the world's cameras (the simulation's Entity:ShakeCamera: where, how far it
// reaches, how hard at its middle and at its reach, how long; worldview.cpp). The host posts a
// beat's shakes; the next frame's camera step takes them.
void PostCameraShake(const float at[3], float radius, float maxAtMiddle, float minAtRadius, float seconds);
// The console's cam_ShakeMult (the game's option "Shake intensity", 0 to 1): how much shake.
void SetCameraShakeMult(float mult);
float CameraShakeMult();
// The camera's wheel and keys as the game's Options menu (Controls tab) sets them through the
// console, defaults the original's (FA exe; the variables' addresses beside them): cam_ZoomAmount
// (each wheel notch zooms by 2^-amount; "Zoom Wheel Sensitivity" / 100), ui_KeyboardPanSpeed
// (0xf57aa4, 90 a frame) and ui_KeyboardPanAccelerateMultiplier (0xf57aa8, times 4 with Ctrl),
// ui_KeyboardRotateSpeed (0xf57aac, 10) and ui_KeyboardRotateAccelerateMultiplier (0xf57ab0,
// times 2), ui_ScreenEdgeScrollView (0xf57a8c, on) and ui_ArrowKeysScrollView (0xf57887, on).
// One set for the whole program, as the console's are (worldview.cpp).
struct CameraControls {
  float zoomAmount = 0.4f;
  float panSpeed = 90.0f, panFast = 4.0f;
  float turnSpeed = 10.0f, turnFast = 2.0f;
  bool edgePans = true, arrowsPan = true;
};
CameraControls& CameraKeys();
// "Lock Fullscreen Cursor To Window" (the game's Options; console SC_ToggleCursorClip 0|1, FA exe
// 0x8d41b0: 1 clips the cursor to a window that fills the screen, 0 lets it go): off at first, as
// in the original, where the mouse may leave a full screen for a second screen. The window's loop
// keeps to it (run_window.cpp).
struct CursorLock {
  bool on = false;
};
CursorLock& LockCursor();
// The display mode, the game's Options, Video tab, "Primary Adapter" (console SC_PrimaryAdapter,
// FA exe ResolutionCommands::SC_PrimaryAdapter 0x8d34d0): "windowed" (a window of the size kept in
// the preferences as Windows.Main.Previous.width / height, the exe's 1024x768 when there is none),
// a full screen "width,height,rate", or "overridden" (the command line chose the mode: nothing to
// do). A full screen here is always the screen's own size, without a frame; fewer pixels come from
// Upscaling. The window's loop takes the request (run_window.cpp). `overridden`: --fullscreen or
// --size chose the mode at the start, and the menu then offers only "Command Line Override", as
// the original does with its command-line switches (StartupHelpers::SetupPrimaryAdapterSettings,
// FA exe 0x8d21e0).
struct DisplayMode {
  enum Request { kNone, kWindowed, kFullScreen };
  Request request = kNone;
  int windowedWidth = 1024, windowedHeight = 768;
  bool overridden = false;
};
DisplayMode& Display();
// The program's own looks, each a player's choice in the game's Options menu (Video tab; Chris,
// 2026-10-15: "The new looks will go into options menu for each player"). The options send them
// as console commands (remaster_effects, remaster_effects_quality, remaster_tracers,
// remaster_rich_ground, remaster_snow_tracks; keymap.cpp), and the game's picture takes them
// every frame (ui/app/game_session.cpp). One set for the whole program, as the console's are.
struct LookSettings {
  bool remasteredEffects = true;  // the visuals track's remastered effects (false: the original's)
  float effectsQuality = 1.0f;    // the share of particles effects throw out
  bool tracers = true;            // tank shells' hot tracers (false: the original's plain line)
  bool richGround = true;         // a remastered map's richer ground
  bool snowTracks = true;         // units tread tracks into lying snow
  float upscale = 1.0f;           // AMD FSR 1: the share of the picture's size the scene is drawn at (1: off)
  // AMD's FSR 3 in place of FSR 1, at that share (at 1: smoother edges at the screen's own size; the renderer's
  // request 30). It needs one sample a pixel: on, the Anti-Aliasing gives way (SceneSamples), from the next
  // map loaded as that setting. Off as at first.
  bool temporalUpscale = false;
  // The lighting upgrade's four switches (the renderer's request 31; Chris, 2026-10-17: all of it in, bundled
  // to keep the menu short), each one WorldPicture call: Atmosphere (light in the air, volumetric clouds, the
  // sky from the atmosphere's tables; needs a time of day), Soft Shadows (with contact shadows), Ray Tracing
  // (bounce light and reflections, the reflections from the next map loaded; only where the card can) and
  // Weather Effects (lit rain and snow, rain's splashes; each unit's own snow, in the renderer's bundle). ON by
  // default (Chris, 2026-10-18: every remastered look on at its highest, "if they can't handle it they lower it";
  // ray tracing only where the card can: the renderer leaves it out elsewhere).
  bool atmosphere = true, softShadows = true, rayTracing = true, weatherEffects = true;
  // Ground Detail (the renderer's request 33): grass near the camera, pressed flat round units and buildings; later
  // the remastered ground's decals and battle scars; deep tracks and wildlife (the renderer's bundle). ON by default.
  bool groundDetail = true;
  // Frame Generation (the renderer's request 34; Chris's list of 2026-10-17, item 7): AMD's FSR 3 frame generation,
  // a picture made between each two drawn ones (twice the frames shown, a little more delay). Needs one sample a
  // pixel, as FSR 3's upscaler: with the Anti-Aliasing on it does nothing (its tooltip says so). Off: a speed
  // trade-off, not a look (Chris, 2026-10-18), as the two upscalers.
  bool frameGeneration = false;
  // Water Ripples (the renderer's World::waterRipples, render/ripples.h; Chris's next looks of 2026-10-17): shells
  // landing in the water, ships, hover units and units wading leave rings and wakes that spread and die out. The
  // Remastered Preset turns it on with Weather Effects (High and Ultra). ON by default.
  bool waterRipples = true;
  // Cinematic Lens Blur (World::lens, render/cinecam.h's Lens): a real lens's depth of field, only while the
  // cinematic camera of a replay shows (remaster_cinecam), its blur going as the picture eases back to the game's
  // camera; focused on the unit it follows, else on what is at the picture's middle. In no preset. ON by default (it
  // acts only while the cinematic camera shows).
  bool lens = true;
  // Battle Effects (the renderer's bundle, agreed 2026-10-18): blast waves, dust and debris, smoke and fire lit with
  // their shadows, heat shimmer, battle scars on units (World's blastWaves .. unitScars), and since render e8a4d56 hot
  // metal, movement dust and explosions as volumes (hotMetal, movementDust, volumeExplosions). Energy Shields (World::
  // energyShields): shields that look like energy. Hidden Unit Outlines (WorldPicture::SetHiddenOutlines): units
  // seen behind trees and cliffs, only what the player may see, none in the Original look. All ON by default.
  bool battleEffects = true, energyShields = true, hiddenOutlines = true;
  // cam_DefaultMiniLOD (the minimap's mesh level; FAF's minimap.lua and multifunction.lua set it from the profile's
  // "Disable meshes", disableMinimapMesh, ticked by default: ConExecute("cam_DefaultMiniLOD 0"), else "... 1.8"): not 0,
  // the minimap stamps the script's meshes (pings, beacons) under its icons (the renderer's SetMinimapMeshes, render
  // 4a061d2). INFERRED: 0 until the script sets it (it does when the minimap is made).
  float minimapLod = 0;
  // The Photographic Look (the renderer's World::photoLook, render/photo_look.h; Chris, 2026-10-18: "an option in
  // game as in you can choose which one you want"): 0 off (today's picture), 1 Natural (the adapting eye and the lens
  // touches with today's colours: the grade "plain"), 2 Film, 3 Warm, 4 Cold War (the graded looks). A style pick, not
  // a quality step: Off unless chosen (the renderer's proposal, told to Chris), in no preset. Modern look only.
  int photoLook = 0;
  // The Season (the renderer's WorldPicture::SetSeason, render/season.h; Chris, 2026-10-18: seasons as a player's
  // option, visuals' autumn art for SETONS_R): 0 Summer (today's), 1 Autumn (where the map has an autumn look: its
  // ground, plants and props in autumn art, leaves in the wind, autumn weather). A style pick: Summer unless chosen,
  // in no preset; the picture's alone (the simulation keeps the map's blueprints), taken when a map opens.
  int season = 0;
  // Whether the graphics card can ray trace (WorldPicture::RayTracingSupported, asked after a picture opened,
  // the menu's or a game's): -1 not known yet, 0 no, 1 yes.
  int rayTracingSupported = -1;
  // The game's own graphics settings (its Video tab; Chris, 2026-10-15: they drive the renderer's
  // switches and mean what they say): graphics_Fidelity 0..2 (Low: no ambient occlusion, plain
  // ground), shadow_Fidelity 0..3 (Off: none; Low, Medium: the sun's; High: the lamps' too),
  // SC_AntiAliasingSamples (the scene's samples 1 / 2 / 4, from the next picture made),
  // SC_CameraScaleLOD 0..2 (model detail 0.5 / 0.75 / 1). Until told: today's full look.
  int fidelity = 2, shadows = 3, sceneSamples = 4;
  // The original's two debugging switches of the mouse's selection (console variables, CWldSession::CanSelectUnit
  // 0x865830 and ReleaseDrag 0x865920): with UI_SelectAnything and cheats on, the mouse selects any unit, the
  // player's or not; with ui_DebugAltClick, Alt and a click on another army's unit looks through that army.
  bool selectAnything = false, debugAltClick = false;
  // The samples a pixel the scene is drawn with: the player's Anti-Aliasing, or 1 under FSR 3.
  int SceneSamples() const { return temporalUpscale ? 1 : sceneSamples; }
  float meshDetail = 1.0f;
  // ren_MipSkipLevels 0..2 (the game's Texture Detail: High 0, Medium 1, Low 2): the largest sizes
  // of each big texture left out, from the next picture made (WorldPicture::SetTextureDetail).
  int textureDetail = 0;
  // "Graphics API" (Chris, 2026-10-15): Vulkan in place of Direct3D 12, the default; taken when a
  // picture is made (WorldPicture::SetVulkan before Open).
  bool vulkan = false;
  // "Look" (Chris's rule: every new look is a player's option, the original's way included): the
  // renderer's original look of lighting, sky, water, ground shading, unit shaders, glow and the
  // build look all together (World::look, the viewer's key L), at once; the other looks stay apart.
  bool originalLook = false;
  // The strategic icons (the game's "Scale strategic icons", ui_StrategicIconScale 1 / 1.5 / 2, and
  // "Always Show Strategic Icons", ui_AlwaysRenderStrategicIcons; the renderer's StrategicIcons
  // playerScale and always), and the modern look's glow (Bloom Render, ren_bloom, and Bloom
  // Intensity, ren_BloomBlurKernelScale 1.0..1.7; World::bloom, bloomIntensity). From the next frame.
  float iconScale = 1.0f;
  bool iconsAlways = false;
  // The cinematics' and the score screen's switches (console UI_NisRenderIcons, ren_SelectBoxes,
  // ren_Oblivion; gamemain.lua's NIS mode, score.lua): the strategic icons, the selection marks, the
  // world itself (off: black under the interface). WorldPicture::SetStrategicIcons,
  // SetSelectionMarks, SetWorldShown (renderer request 27).
  bool strategicIcons = true, selectionMarks = true, worldShown = true;
  bool bloom = true;
  float bloomIntensity = 1.5f;
  // ui_SelectTolerance (FAF's world view sets it from the options' Selection Threshold, 10 / 10 /
  // 20 for normal play, reclaim mode and replays, and -1000 in its ignore mode, where clicks pick
  // no unit) and ui_CommandClickScale (its ignore mode's 0: no waypoint is clicked; waypoints are
  // not clickable here yet). Below 0 nothing is picked under the mouse; otherwise the renderer's
  // own margin applies (World::UnitAt: ten pixels, the option's default).
  float selectTolerance = 10.0f;
  float commandClickScale = 1.0f;
};
LookSettings& Looks();
// The names players give their units, as the original draws them under the life bars (its console
// variables ui_RenderCustomNames, ui_CustomNameColor, ui_CustomNameFontSize, ui_CutsomNameFont, as
// spelt there; FA exe descriptors 0xf5b2d0.., defaults: on, 0xff00aa00, 12, "Arial"; the game's
// option "Always Show Custom Names" sends 0 at the start). Read by a helper from the exe (REQUESTS 23).
// Below them, a unit's selection sets (its control groups: "1 3 "), as sub_85E3A0 draws them:
// ui_RenderSelectionSetNames (on in the exe and never set by the game's scripts at the start; a key
// action toggles it) and ui_SelectionSetNamesColor (0xff00aa00); the same font.
struct CustomNameStyle {
  bool on = true;
  uint32_t color = 0xff00aa00u;
  int size = 12;
  std::string font = "Arial";
  bool setsOn = true;
  uint32_t setsColor = 0xff00aa00u;
};
CustomNameStyle& CustomNames();
// The command graph's console variables (the order lines while Shift is held; FA exe objects
// 0xf5aee0..0xf5af80, their values at 0xf57cc0..0xf57cdc), at the exe's defaults. Each time the graph
// is made (Shift pressed) the game's /lua/ui/game/commandwaypoint.lua CommandWaypointParams writes
// over them (0x825150: 32, 35, 2, 1, 7, 100, 1, as the stock file has them); the console sets them
// too (ui_CurveSegments 40 ...). ui_CommandClickScale stays in LookSettings (commandClickScale).
struct CommandGraphVars {
  int curveSegments = 20;          // ui_CurveSegments: the points of a leg's curve, less one
  float curveSmoothness = 50;      // ui_CurveSmoothness: how far the curve bends, in leg widths
  float pathSmoothness = 2;        // ui_PathSmoothness (the path preview's; the preview is never on)
  int maxNodeUnits = 1;            // ui_CommandGraphMaxNodeUnits: the most a leg counts for at a node
  float minWaypointSize = 7;       // ui_MinWaypointSize, pixels (half the mark's side)
  float maxWaypointSize = 100;     // ui_MaxWaypointSize
  float waypointLineScale = 1;     // ui_WaypointLineScale: every leg's width times this
  // ui_MaxTextLOD (0xf57ccc, "LOD level that timer text dissapears"; the Lua file does not set it): a
  // node's time-left text in the normal state is not drawn where the camera's level of detail is
  // this or more (OrderEta)
  float maxTextLod = 80;
};
CommandGraphVars& GraphVars();
// A unit's custom name (nullptr: none or no such unit), its selection sets as the original writes
// them ("1 3 ": sorted, each with a space after; "" for none), and whether any unit has a name or a
// set (user_units.cpp).
const std::string* CustomNameOf(lua_State* L, uint32_t id);
std::string SelectionSetsText(lua_State* L, uint32_t id);
bool AnyCustomNames(lua_State* L);
bool AnySelectionSets(lua_State* L);
// The frame rate the player's options ask for. Ours: "Frame Rate" in the Video tab (Chris,
// 2026-10-15, via the renderer track: "have options for all and default is vsync on"):
// remaster_frame_rate vsync | uncapped | <frames a second>. The game's own two, "Vertical Sync"
// (SC_VerticalSync) and "Frametime" (SC_FrameTimeClamp, the shortest frame in milliseconds),
// set the same when changed in the menu. The window's loop keeps to it.
// "Low Latency" beside it (the renderer's latency limiter, look 7 of Chris's list of 2026-10-17, render 0ed9975;
// WorldPicture::SetLowLatency on the window's pictures, WaitForNextFrame before each frame's input):
// remaster_low_latency 0 | 1. ON by default (Chris, 2026-10-18: every new thing on; the renderer's ask).
struct FramePacing {
  bool verticalSync = true;   // the screen's own rate
  double frameMs = 8;         // otherwise the shortest frame; 0: as fast as it goes
  bool lowLatency = true;     // one frame waiting to be shown, each begun just in time
};
FramePacing& Pacing();
// The performance statistics drawn over the interface (docs/performance-lessons.md 5.5 and 6):
// the console's remaster_stats [on|off] (no value: toggles), bound to a key by the program's own
// key action "remaster_stats" (Ctrl-Alt-S at first). Drawn by the window's loop (run_window.cpp).
struct StatsOverlay {
  bool on = false;
};
StatsOverlay& Overlay();
// Where a live window's frame time goes (for the log at the end of a window run): the world's
// picture (WorldPicture::Draw: the renderer's frame and its read-back), in seconds and calls.
struct FrameTimes {
  double worldDraw = 0;
  int worldDraws = 0;
  int heldPictures = 0;  // frames whose moment lay past the newest beat there was: the world stood still
  double simLuaKB = 0;   // the simulation's Lua heap after the last beat (lua_getgccount; the stats overlay)
  // the game's beats: the simulation's own, the picture's per-beat update, the sounds
  double simBeats = 0, pictureBeats = 0, soundBeats = 0;
  int beats = 0;
};
FrameTimes& Times();
// The graphics card's time per frame of a game's world picture, for the first-start graphics check (this program's
// own, not the original's: ui/data/hook/lua/ui/game/gamemain.lua; Chris, 2026-10-18: "if the PC can't hold 60 fps,
// turn ray tracing off first, then step the preset down"). The game's script starts a measurement
// (moho64ui_MeasureFrameCost) and reads its result (moho64ui_FrameCost). The window's loop gives it the card's time
// of every frame of the game's picture it shows (WorldPicture::LastStats().gpuMs: the card's own, not the frame
// interval, which vertical sync holds at the screen's rate); the first kSettle frames after the start are left out
// (the card's times come back a few frames late, and a setting just changed must have reached the picture), then
// kSamples frames make the result, their median. A run without a window draws no frames on the card and never
// finishes a measurement. In a run with a test script (TestFileWritesAllowed) moho64ui_SetFrameCostForTests(ms)
// makes every frame of a game count as one of that many milliseconds (GameSession::Advance), and the card's own
// are left out.
struct FrameCostMeter {
  static constexpr int kSettle = 30, kSamples = 120;
  bool measuring = false;
  int settle = 0;
  std::vector<float> samples;
  float result = -1;  // the last measurement's median, ms; below 0: none (or one under way)
  float fakeMs = -1;  // the tests' time a frame; below 0: none
  void Start();
  void Note(float gpuMs);  // a frame of the game's picture, the card's milliseconds (0: not known, left out)
};
FrameCostMeter& FrameCost();
// The unit under the mouse in the main world view (0xffffffff: none, or the mouse is over
// something else), by the picture last drawn (worldview.cpp); an army's colour as scripts take
// colours, "aarrggbb" (session_bindings.cpp; army counted from 1).
uint32_t UnitUnderMouse(lua_State* L);
std::string ArmyColorOf(lua_State* L, int army);
// The world camera (worldview.cpp): show a box of the world, at once or in `seconds` (the
// original's camera slot 13, FA exe 0x7a83e0); the entity ids of the units in the main world
// view's picture (false without a picture; then PlaceInCameraView says roughly whether a place
// is in view); the place of the world under the mouse (false without a world view).
void WorldCameraShowBox(lua_State* L, const float lo[3], const float hi[3], float seconds);
// The cinematic camera for replays and the photo (worldview.cpp; ours, not the original's: the renderer's
// CineCamera and TakePhoto, Chris's approval of 2026-10-17). The console's remaster_cinecam [on|off] (no word:
// it turns over), in a replay only (in a game one plays it says so and does nothing): on, it takes over from the
// main world view's camera, following the first selected unit when there is one; any camera input of the player's
// or a script's move of that camera hands it back. remaster_photo: the world at twice the view's size, as a PNG in
// <the preferences' folder>/screenshots, named by the PC clock's date and time (in any game).
void CineCameraCommand(lua_State* L, const std::string& value);
void CinePhotoCommand(lua_State* L);
// The photo mode (worldview.cpp; ours, not the original's: Chris's approval of 2026-10-18, the visuals session's look
// 7). The console's remaster_photo_mode [on|off] (no word: it turns over; the key "Photo mode", Alt-P at first), in a
// game or a replay: a free camera, the picture's own time of day, weather and lens, and photos up to four times the
// screen's size; in a game one plays alone and in a replay the game pauses meanwhile. While it is open the photo
// mode's keys go to it before the key map (PhotoModeKey: true when the key was its own, and no key map action runs).
void PhotoModeCommand(lua_State* L, const std::string& value);
bool PhotoModeKey(lua_State* L, int vk, bool repeat);
bool PhotoModeOpen(lua_State* L);
bool UnitsOnScreen(lua_State* L, std::vector<uint32_t>* ids);
bool PlaceInCameraView(lua_State* L, const float place[3]);
bool MouseWorldPlace(lua_State* L, float out[3]);
// How the unit under the mouse is to be marked (the original's choice of bracket, FA exe
// 0x7fd3f0): 0 not at all (no such unit, or it is selected and has its mark already), 1 the
// player's own or an ally's, 2 an enemy's, 3 a neutral's.
int HoverKindOf(lua_State* L, uint32_t entity);
// The living unit nearest to a place of the map, no farther than `radius` (0xffffffff: none): what
// stands in for the picture's answer when there is no picture.
uint32_t UnitNearPlace(lua_State* L, float x, float z, float radius);
std::string RightClickOrder(lua_State* L, uint32_t under);
// Reclaim of what is not a unit: the tree, rock or wreck at a place (the simulation's
// ReclaimableAt: the nearest whose box lies within `within` map units of the place). The
// selected units get a Reclaim order naming it; false when nothing reclaimable is there or the
// selection holds no unit that can reclaim.
bool IssueReclaimAtPlace(lua_State* L, const float at[3], float within, bool clear);
// The same question without the order: would a reclaim there take something? (For the cursor.)
bool CanReclaimAtPlace(lua_State* L, const float at[3], float within);
// Select the player's own units among these entity ids that the mouse may select; with `add` they
// join the selection instead of replacing it.
void SelectUnitsById(lua_State* L, const std::vector<uint32_t>& ids, bool add);
// The mouse's selection as the original's (user_units.cpp: CWldSession::ReleaseDrag, DragRelease,
// HandleDoubleClickSelection): a click on a unit (0xffffffff: none) with its modifiers, a box over
// these units, a double click on a unit with the units in view; and the unit the mouse is over of
// the one the picture finds (a unit on a factory's pad or on a transport: the parent).
void ClickSelect(lua_State* L, uint32_t unit, bool shift, bool ctrl, bool alt = false);
void BoxSelect(lua_State* L, const std::vector<uint32_t>& ids, bool shift);
bool DoubleClickSelect(lua_State* L, uint32_t unit, const std::vector<uint32_t>& inView);
uint32_t HoverOf(lua_State* L, uint32_t picked);
// The console commands the game's key map binds its selection keys to (user_units.cpp):
// UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto] [+excludeengineers] <category
// words> (FA exe 0x866a30) and UI_ExpandCurrentSelection (0x866970: the player's units in view
// of the kinds that are selected join the selection; no walls, no building being upgraded).
void SelectByCategoryCommand(lua_State* L, const std::string& arguments);
void ExpandCurrentSelection(lua_State* L);
// For the world's picture: the selected units' entity ids.
// `ghosts` (may be null): the buildings the player's units, selected or not, have been ordered to
// build and have not begun, each where it will stand.
// `queueShown`: the selected units' orders are to be seen (the original shows them while Shift is
// held): only then `queued` (may be null) gets the sites of the buildings in those orders, begun or
// not. (The lines of the orders: OrderLinesForWorld.)
void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldGhost>* ghosts = nullptr, bool queueShown = true,
                       std::vector<WorldGhost>* queued = nullptr);
// The time left at a node of the order lines while Shift is held: the original's ETA text (FA exe
// 0x828610, the last step of the command graph's drawing 0x829190, in screen space), worked out by
// user_units.cpp (WorkOutEtas). Only the nodes of orders with time left (the start nodes never). The
// world view draws each (WorldView::DrawOrderEtas): white "Andale Mono" of 10 pixels, its pen one pixel
// right of the point (x + size, y, z - size) (the corner of the node's mark up and right, by the
// node's size unclamped) and its baseline the font's descent and a pixel above it, both rounded; a
// node in the normal state only where the camera's level of detail there is below ui_MaxTextLOD; none
// at all while the player's option display_eta ("Show Waypoint ETAs") is off.
struct OrderEta {
  float position[3] = {0, 0, 0};  // the node's place
  float size = 0;                 // the node's size times its state's waypoint_scale (world units)
  int state = 0;                  // 0 normal, 1 highlighted, 2 selected (0x8281e0's)
  int ticks = 0;                  // the time left, in beats (10 a second)
  std::string text;               // "ETA: %d.%02d", minutes and seconds (0x8287a1): "ETA: 1.05"
};
// The order lines and their waypoints' marks while Shift is held (`shown`), as the original's
// command graph works them out (user_units.cpp: the orders of every unit whose orders the interface
// holds, from the game's commandgraphparams.lua and commandwaypoint.lua): into the scene's
// orderTextures, orderLegs (highlighted first, then selected, then the rest), orderMarks and
// orderSettings; empty lists when not shown. `rollover`: the unit under the mouse (0xffffffff: none).
// `etas` (may be null): the time-left texts at the graph's nodes (OrderEta), for the world view to draw.
void OrderLinesForWorld(lua_State* L, bool shown, uint32_t rollover, WorldScene* scene, std::vector<OrderEta>* etas = nullptr);
// The waypoint under the mouse and dragging it, as the original's command graph and its UICommandDragger
// (user_units.cpp). `screenOf`: where a place of the world is on the screen and how many map units a pixel is
// there (false: not in view). UpdateHoveredWaypoint works out the waypoint under the mouse at (x, y) while
// `active` (Shift held over a world view whose highlight is on, not a minimap; else there is none), keeps it as
// the session's and gives it: its order's id, 0xffffffff for none. HoveredWaypointOffer: what the hovered waypoint
// offers the selection (the world view's tips, FA exe 0x870612): JoinAttack, an Attack or FormAttack none of the
// selected units carries (a double click joins it); ToPatrol, a Move or FormMove a selected unit carries that the
// selection's moves can be turned into a patrol at (the cursor's MOVE2PATROLCOMMAND, ConvertMovesToPatrol). A drag: begun
// on a hovered waypoint (false: none, or no focus army), its node moved to the ground under the mouse, let go
// (the order's new target sent when it has one; `playable` the playable rectangle x0, z0, x1, z1; gives the
// order's id for the script), or cancelled; DraggedBuildingSite: a building's order being dragged, its ghost where
// the mouse is and whether it can stand there, and `from` the order's own place (NAN: none).
// RemoveHoveredWaypointOrder: the right button let go with Shift and Ctrl (the release's own modifiers) over a
// hovered waypoint, with a focus army: its order is taken out of the queues of all the units that carry it (the
// command stream's RemoveCommandFromQueue; gone from the interface's queues at once); true: the release did that (or
// found the order gone) and gives no other order.
using WaypointScreen = std::function<bool(const float place[3], float* x, float* y, float* unitsPerPixel)>;
uint32_t UpdateHoveredWaypoint(lua_State* L, bool active, float x, float y, const WaypointScreen& screenOf);
uint32_t HoveredWaypoint(lua_State* L);
enum class WaypointOffer { None, JoinAttack, ToPatrol };
WaypointOffer HoveredWaypointOffer(lua_State* L);
bool BeginWaypointDrag(lua_State* L, uint32_t command);
void MoveWaypointDrag(lua_State* L, const float at[3]);
uint32_t ReleaseWaypointDrag(lua_State* L, const float at[3], const float playable[4]);
void CancelWaypointDrag(lua_State* L);
bool RemoveHoveredWaypointOrder(lua_State* L, bool shift, bool ctrl);
bool DraggedBuildingSite(lua_State* L, WorldGhost* site, float from[3]);
// The clicks on a hovered waypoint (user_units.cpp "Clicks on a hovered waypoint"). AttackClick: an Attack the click
// gives (SCommandModeData::HandleEvent 0x81fdad) on the unit `under` (0xffffffff: on the ground at `at`); `dbl`: the
// click is a double click, so the selection's last orders go first (the first click's attack), and on a unit, when
// `hovered` (the waypoint under the mouse as the click was pressed) is an attack none of the selection carries, the
// attack is coordinated with it and "Coordinated Attack!" shows at (sx, sy). ConvertMovesToPatrol: a Move the click
// gives, with `hovered` a move the selection's moves can become a patrol at: they become one instead (true), and
// "Patrol Initiated to location!" shows at (sx, sy); false: give the Move.
bool AttackClick(lua_State* L, uint32_t under, const float at[3], uint32_t hovered, bool dbl, bool clear, bool ctrl, bool form,
                 float heading, float sx, float sy);
bool ConvertMovesToPatrol(lua_State* L, uint32_t hovered, float sx, float sy);
// /lua/ui/uimain.lua StartCursorText(x, y, text, color, time, flash) and StopCursorText() (FA exe 0x83ecf0, 0x83edf0):
// the game's script shows a text beside the pointer (`time` seconds, -1 until stopped; `color` 0xAARRGGBB).
void StartCursorText(lua_State* L, float x, float y, const char* text, uint32_t color, float time, bool flash);
void StopCursorText(lua_State* L);
// A build template: buildings that are placed together, each where it stood beside the first
// (the original's struct_BuildTemplate: 0x823b40, entries of 44 bytes with place, order and
// blueprint; to scripts {width, height, {blueprint, order, x, z}, ...}: 0x8472f0).
struct BuildTemplate {
  struct Building {
    std::string blueprint;
    int order = 0;     // what the original sorts the buildings by
    float x = 0, z = 0;  // where it stands beside the place the template is put
  };
  float width = 0, height = 0;  // the ground the whole takes: the spacing when a row of it is drawn
  std::vector<Building> buildings;
};
// The template being placed (GenerateBuildTemplateFromSelection, SetActiveBuildTemplate), or
// nullptr when there is none.
const BuildTemplate* ActiveBuildTemplate(lua_State* L);
// How far apart buildings of a kind stand in a row (along x and along z, map units): the larger
// of the blueprint's footprint and its skirt, at least 1.
void BuildSpacingOf(lua_State* L, const std::string& blueprint, float* alongX, float* alongZ);
// Whether a building of that kind at that site would lie over a building the player's units
// have been ordered to build and have not begun (the ground of the two meets; edges may
// touch). `adding`: the new order comes after the selected units' orders (Shift); without it
// their own queued buildings do not count, since the new order takes their place.
bool SiteMeetsOrdered(lua_State* L, const std::string& blueprint, const float position[3], bool adding);
// The buildings the interface knows of (any army's, as far as the player sees them) within
// `within` map units of a place, each with its blueprint and where it stands: for the outlines
// the world view draws round them while a building is being placed. At most 600.
void StructuresForWorld(lua_State* L, const float near[3], float within, std::vector<WorldGhost>* out);
std::string TakeOrders(lua_State* L);
// The chat messages the script has sent since the last frame come back to it (session_bindings.cpp).
void DeliverChat(lua_State* L);
// For other parts of the engine side (user_units.cpp): the entity ids of all units the interface
// knows, or of the selected ones; and a unit's blueprint, place and army (counted from 1), false
// when there is no such unit.
void UserUnitIds(lua_State* L, bool selectedOnly, std::vector<uint32_t>* ids);
bool UserUnitFacts(lua_State* L, uint32_t id, std::string* blueprint, float position[3], int* army);
// The simulation's view of a living unit the interface knows (nullptr: none; valid until the next sync), and
// its toggles now (UnitStateView::scriptBits; false when not known): for range_rings.cpp (user_units.cpp).
const UnitView* UserUnitView(lua_State* L, uint32_t id);
bool UserUnitScriptBits(lua_State* L, uint32_t id, uint32_t* bits);
// Range rings (range_rings.cpp): SetOverlayFilter / SetOverlayFilters; the console variables
// range_RenderSelected / range_RenderHighlighted / range_RenderBuild / range_Fill /
// range_InnerThicknessCoeff / range_OuterThicknessCoeff / ren_Ranges (the name in upper case; false: not one
// of them), and such a variable's spelling, kind ('b' or 'f') and value now; and the rings to draw now, in the
// order drawn, for what the world view says of this frame:
struct RangeView {
  uint32_t rollover = 0xffffffffu;  // the unit under the mouse (the session's rollover unit)
  bool placing = false;             // a build mode with a blueprint, the mouse over the world:
  std::string blueprint;            // the building about to be placed
  float mouse[3] = {0, 0, 0};       // and the place of the world under the mouse
  float zoomShare = 1;              // the camera's zoom over its farthest zoom (GetZoom / GetMaxZoom)
  float playable = 512;             // the longer side of the playable map
  bool interpolate = true;          // the units where the picture draws them this frame
  // The units in the camera's view (false: not known; then PlaceInCameraView says).
  std::function<bool(std::vector<uint32_t>*)> unitsInView;
};
void RegisterRangeBindings(lua_State* L);
bool RangeConsoleCommand(lua_State* L, const std::string& upperName, const std::string& value);
bool RangeConVar(lua_State* L, const std::string& upperName, const char** name, char* kind, double* value);
void RangeRingsForWorld(lua_State* L, const RangeView& view, std::vector<WorldRangeGroup>* out);
// The meshes the game's script places in the world (world_meshes.cpp): InternalCreateWorldMesh
// and its methods, AddCommandFeedbackBlip; and the list to draw now (hidden ones left out, an
// order's mark gone when its time is over).
void RegisterWorldMeshBindings(lua_State* L);
void WorldMeshesForWorld(lua_State* L, std::vector<WorldMeshView>* out);
// The decals the script lays on the ground (world_meshes.cpp: _c_CreateDecal, the ScriptedDecal
// methods), those with a texture.
void UserDecalsForWorld(lua_State* L, std::vector<WorldDecal>* out);
// The place on the ground under a point of the screen, by the main world view (worldview.cpp);
// false without one.
bool ScreenToWorld(lua_State* L, float x, float y, float out[3]);
// The economy overlay's numbers (RenderOverlayEconomy): each living unit of the player's that makes
// or uses something, where it stands and its energy and mass a second, made less what it asks for
// (user_units.cpp).
struct EconomyMark {
  float position[3];
  float energy, mass;
};
void EconomyMarks(lua_State* L, std::vector<EconomyMark>* out);
// A unit's place and turn as the last beat left them (position x, y, z; orientation a quaternion
// x, y, z, w); false when the interface has no such living unit (user_units.cpp).
bool UnitPlace(lua_State* L, uint32_t id, float position[3], float orientation[4]);
// The strategic icons' colours by army counted from 0 while TeamColorMode is on and there is a focus
// army (empty: the armies' own colours; session_bindings.cpp).
void IconColorsForWorld(lua_State* L, std::vector<uint32_t>* out);
// The player asks for the game to pause or to go on (user_units.cpp): written into the orders
// like every command, so the simulation pauses with a beat (CWldSession::RequestPause / Resume,
// FA exe 0x8942b0 / 0x894330).
void RequestSessionPause(lua_State* L, bool pause);
void RequestSessionEnd(lua_State* L);  // the command stream's EndGame
// The game's speed in the speed keys' steps, and setting it (session_bindings.cpp): only where
// the game's option lets the players change it (the client manager's byte +0x454).
int GameSpeedOf(lua_State* L);
void SetGameSpeedTo(lua_State* L, int rate);
// The lobby object in its local form (lobby.cpp).
void RegisterLobbyBindings(lua_State* L);

}  // namespace moho::ui
