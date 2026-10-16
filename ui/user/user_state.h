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
struct WorldRangeGroup;
struct WorldMeshView;
struct WorldGhost;
class UiManager;
namespace sound {
class SoundSystem;
}

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
    // The player's own files (replays, saved games): their folder (special_files.cpp), and the
    // host's replay hooks: start a session from a replay file (LaunchReplaySession; unset: none
    // can be started from this state) and the replay file of the game running or last played.
    std::string userDir;
    std::function<bool(lua_State* L, const std::string& file, std::string* why)> launchReplay;
    std::function<std::string()> currentReplay;
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
    // The command source the player's orders are written for (the focus army's first source).
    int commandSource = 0;
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
  std::unique_ptr<Extra> ranges;  // the range rings' filters and switches (range_rings.cpp)
  std::unique_ptr<Extra> worldMeshes;  // the script's meshes in the world (world_meshes.cpp)
  std::map<std::string, std::unique_ptr<ScriptObject>> cameras;  // the cameras by name (maui/worldview.cpp)
  std::vector<std::string> mapBorders;  // the mesh blueprints of the map's border (MapBorderAdd)
  // The life bars over units (the console command UI_RenderUnitBars, user/keymap.cpp): the
  // player's own switch (-1: not read from the preferences yet) and whether a script has them
  // put away for now.
  int lifeBars = -1;
  bool lifeBarsPutAway = false;
  // The console's UI_ForceLifbarsOnEnemy (the game's option "Always Show Enemy Lifebars").
  bool lifeBarsOnEnemies = false;

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
  bool isReplayCopy = false;  // SessionIsReplay
  std::vector<ArmyView> armiesCopy;  // ArmyViews (GetArmiesTable)
  bool economyCopied = false;  // (these copies are there: one beat's sync came)
  void CopyFromSimulation();
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
// PlaySound, StopSound, SetVolume and the others (sound_bindings.cpp).
void RegisterSoundBindings(lua_State* L);
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
// The same with a unit as the target ("Attack", "Guard", "Repair", "Reclaim", "Capture", "Move":
// the unit's entity id); and what a right click orders by the unit under the cursor (0xffffffff:
// none): "RULEUCC_Move", "RULEUCC_Attack", ..., empty for nothing.
bool IssueToSelectionAt(lua_State* L, const std::string& order, uint32_t entity, bool clear);
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
  // The game's own graphics settings (its Video tab; Chris, 2026-10-15: they drive the renderer's
  // switches and mean what they say): graphics_Fidelity 0..2 (Low: no ambient occlusion, plain
  // ground), shadow_Fidelity 0..3 (Off: none; Low, Medium: the sun's; High: the lamps' too),
  // SC_AntiAliasingSamples (the scene's samples 1 / 2 / 4, from the next picture made),
  // SC_CameraScaleLOD 0..2 (model detail 0.5 / 0.75 / 1). Until told: today's full look.
  int fidelity = 2, shadows = 3, sceneSamples = 4;
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
};
LookSettings& Looks();
// The names players give their units, as the original draws them under the life bars (its console
// variables ui_RenderCustomNames, ui_CustomNameColor, ui_CustomNameFontSize, ui_CutsomNameFont, as
// spelt there; FA exe descriptors 0xf5b2d0.., defaults: on, 0xff00aa00, 12, "Arial"; the game's
// option "Always Show Custom Names" sends 0 at the start). Read by a helper from the exe (REQUESTS 23).
struct CustomNameStyle {
  bool on = true;
  uint32_t color = 0xff00aa00u;
  int size = 12;
  std::string font = "Arial";
};
CustomNameStyle& CustomNames();
// A unit's custom name (nullptr: none or no such unit), and whether any unit has one (user_units.cpp).
const std::string* CustomNameOf(lua_State* L, uint32_t id);
bool AnyCustomNames(lua_State* L);
// The frame rate the player's options ask for. Ours: "Frame Rate" in the Video tab (Chris,
// 2026-10-15, via the renderer track: "have options for all and default is vsync on"):
// remaster_frame_rate vsync | uncapped | <frames a second>. The game's own two, "Vertical Sync"
// (SC_VerticalSync) and "Frametime" (SC_FrameTimeClamp, the shortest frame in milliseconds),
// set the same when changed in the menu. The window's loop keeps to it.
struct FramePacing {
  bool verticalSync = true;   // the screen's own rate
  double frameMs = 8;         // otherwise the shortest frame; 0: as fast as it goes
};
FramePacing& Pacing();
// Where a live window's frame time goes (for the log at the end of a window run): the world's
// picture (WorldPicture::Draw: the renderer's frame and its read-back), in seconds and calls.
struct FrameTimes {
  double worldDraw = 0;
  int worldDraws = 0;
  int heldPictures = 0;  // frames whose moment lay past the newest beat there was: the world stood still
  // the game's beats: the simulation's own, the picture's per-beat update, the sounds
  double simBeats = 0, pictureBeats = 0, soundBeats = 0;
  int beats = 0;
};
FrameTimes& Times();
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
// Select the player's own units among these entity ids (what a click or a box in the world
// found); with `add` they join the selection instead of replacing it.
// `byPriority`: only those of the best selection priority among them (a selection by box).
void SelectUnitsById(lua_State* L, const std::vector<uint32_t>& ids, bool add, bool byPriority = false);
// The console commands the game's key map binds its selection keys to (user_units.cpp):
// UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto] [+excludeengineers] <category
// words> (FA exe 0x866a30) and UI_ExpandCurrentSelection (0x866970: the player's units in view
// of the kinds that are selected join the selection; no walls, no building being upgraded).
void SelectByCategoryCommand(lua_State* L, const std::string& arguments);
void ExpandCurrentSelection(lua_State* L);
// Select the player's units of the same kind (blueprint) as `unit` among those entity ids; with
// `add` they join the selection. False when `unit` is not one of the player's.
bool SelectSameKind(lua_State* L, uint32_t unit, const std::vector<uint32_t>& among, bool add);
// For the world's picture: the selected units' entity ids, and the lines of their orders (from
// each unit to where its orders take it, coloured by the kind of order).
// `ghosts` (may be null): the buildings the player's units, selected or not, have been ordered to
// build and have not begun, each where it will stand.
// `queueShown`: the selected units' orders are to be seen (the original shows them while Shift is
// held): only then `lines` are given, and `queued` (may be null) gets the sites of the buildings
// in those orders, begun or not.
// `waypoints` (may be null): the mark of each of those orders at its place, as the game's own table
// says (/lua/ui/game/commandgraphparams.lua: a texture and a colour per kind of order); the lines
// take their colour from the same table.
struct WorldWaypoint {
  float position[3] = {0, 0, 0};
  std::string texture;
  uint32_t color = 0xffffffffu;  // 0xAARRGGBB
  float scale = 1;
};
void SelectionForWorld(lua_State* L, std::vector<uint32_t>* ids, std::vector<WorldLine>* lines, std::vector<WorldGhost>* ghosts = nullptr,
                       bool queueShown = true, std::vector<WorldGhost>* queued = nullptr, std::vector<WorldWaypoint>* waypoints = nullptr);
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
// Range rings (range_rings.cpp): SetOverlayFilter / SetOverlayFilters; the console variables
// range_RenderSelected / range_RenderHighlighted / range_RenderBuild (the name in upper case;
// false: not one of them); and the rings to draw now, given the unit under the mouse
// (0xffffffff: none) and the building about to be placed (nullptr: none).
void RegisterRangeBindings(lua_State* L);
bool RangeConsoleCommand(lua_State* L, const std::string& upperName, const std::string& value);
void RangeRingsForWorld(lua_State* L, uint32_t hover, const WorldGhost* build, std::vector<WorldRangeGroup>* out);
// The meshes the game's script places in the world (world_meshes.cpp): InternalCreateWorldMesh
// and its methods, AddCommandFeedbackBlip; and the list to draw now (hidden ones left out, an
// order's mark gone when its time is over).
void RegisterWorldMeshBindings(lua_State* L);
void WorldMeshesForWorld(lua_State* L, std::vector<WorldMeshView>* out);
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
