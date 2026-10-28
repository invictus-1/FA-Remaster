// A game started from the lobby: the host's hold on the simulation.
//
// When the lobby script launches a game, the engine builds a session from the game configuration
// (sim/session.h, the engine track's part), loads the rules and starts the simulation. The host
// then runs its beats, ten a second. There is no in-game interface yet and nothing is drawn of the
// world: the host only reports that the simulation runs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "draw/draw.h"
#include "maui/world_source.h"
#include "script/lua.hpp"

namespace moho {
class CommandStream;
class ScriptState;
class Sim;
class Fingerprint;
class SimBlueprints;
class Vfs;
}  // namespace moho

namespace moho {
struct SessionDescription;
struct ReplayHeader;
}

namespace moho::ui {

namespace sound {
class SoundSystem;
}
class ReplayTimelineSource;
class Backdrop;
class CardBackdrop;

class GameSession {
 public:
  GameSession(Vfs* vfs, std::vector<std::string> hookDirs);
  ~GameSession();

  // Start a session from the game configuration table at stack index `config` of `L` (the user
  // state: the scenario file is run there). False with the reason the original would give the
  // script's LaunchFailed ("NoConfig", "StartSpots"), or a description.
  bool Launch(lua_State* L, int config, std::string* why);
  // Start a single-player session (LaunchSinglePlayerSession: an operation of the campaign) from
  // the table FAF's SinglePlayerLaunch.lua makes, at stack index `info` (the engine's
  // SessionFromSinglePlayerInfo). False with the reason.
  bool LaunchSinglePlayer(lua_State* L, int info, std::string* why);
  // End the running session: the simulation and its picture go. (The state of the game's
  // interface must be gone before: it looks into both.)
  void End();
  // End it and start the same game again, as it was launched (a new random seed unless one was
  // given). False with the reason when there was no launch or the start fails.
  bool Relaunch(std::string* why);

  // The world's picture (the renderer's, render/world_picture.h), opened when a game is launched
  // unless wantPicture is off; nullptr when there is none (it did not open, or this build has no
  // renderer).
  bool wantPicture = true;
  // The day and the weather in the picture (the renderer's WorldPicture::SetDayAndWeather and its
  // world's settings). They are only seen. `sky` is what a game gets whose configuration says
  // nothing (set before a game is launched); a game's options RemasterDayStart, RemasterDayLength,
  // RemasterWeather and RemasterMapEdge (the lobby's, ui/data/hook) go before it.
  struct Sky {
    // The hour the game starts at, 0..24 (6 sunrise, 18 sunset); negative: the map's own fixed
    // light, as the original has it, and no weather.
    float startHour = -1;
    // How long a whole day takes in game seconds; 0: the hour stands still.
    float dayLengthSeconds = 1200;
    // "changing" (by itself: rain and storms), "cold" (by itself: snow in place of rain), "dry"
    // (by itself: mostly clear), or one kind that stays: "none" (fair), "clear", "cloudy",
    // "overcast", "rain", "storm", "fog", "snow".
    std::string weather = "changing";
    // The map's edge (the renderer's World::worldBeyond / frame / beyondHaze): "beyond" (no
    // machinery: no frame, the land and sea go on past the edge, greyed out where units cannot
    // go, no haze over it) or "frame" (the original's frame of machinery round the map, nothing
    // past it). Chris, 2026-10-14, chose between the two: "for default have option 2", the land
    // beyond; the original's frame stays a choice.
    std::string edge = "beyond";
  };
  Sky sky;
  // What plays the world's sounds (the host's sound system); nullptr: the simulation is not asked
  // for them. Set before a game is launched.
  sound::SoundSystem* sounds = nullptr;
  // The game's pace: set from the game's option at launch, changed by the interface where that
  // option allows it; Advance runs the beats that fast.
  GamePace pace;
  void PlayBeatSounds();  // the simulation's sounds of the beat just run (called by the session itself)
  WorldSource* world() const;

  // The random seed of the next launch; without one the clock gives it.
  bool seedGiven = false;
  uint32_t seed = 0;

  // A record of each game, to run it again elsewhere (--replay-out <prefix>): `<prefix>.stream`
  // gets, for every beat, the bytes of the orders handed to the simulation before it (each batch
  // as the command writer gave it) and then the stream's "advance one beat"; a paused beat
  // leaves nothing but the orders given in it. `<prefix>.seed` gets the game's seed and map as a
  // line of text. A second game of the same run (a restart) writes `<prefix>.2.stream` and so
  // on. With the launch table (the host's --session file, or what the lobby's launch wrote:
  // `<prefix>.session.lua` unless --session-out names another place) and the seed, the engine's
  // own program can play the stream back. Empty: nothing is written.
  // `replayTable` (the launch table's file) and `replayMaps` (the map folders beside the
  // game's own, --maps) are named in the seed file too, as lines `session <path>` and
  // `maps <folder>`, so that whoever plays the record back need not guess them.
  std::string replayOut;
  std::string replayTable;
  std::vector<std::string> replayMaps;
  // The lobby's table beside the record (`<prefix>.session.lua`, no --session-out): the second game of the run gets its
  // own, `<prefix>.2.session.lua` as its record (a restart's is copied from the game before). TableFile(): the file the
  // next game's table goes to.
  bool numberTables = false;
  std::string TableFile() const;

  // The game's own replays, the original's way: every game played is written as it runs to
  // `<userDir>/replays/<profile>/LastGame.SCFAReplay` (the session's header, then the command
  // stream: what the original's replay files hold), which the game's Replay dialog lists,
  // copies under a name (CopyCurrentReplay) and plays back (LaunchReplaySession). `userDir`: the
  // player's own files (ours: the folder of the preferences file; the original's is the
  // documents folder); empty: no replay is written. `currentReplay`: the replay file of the game
  // running or last played, "" for none.
  std::string userDir;
  std::string currentReplay;
  // Start a session from a replay file (a host path): its header makes the session, a replay
  // that the player watches (SessionIsReplay; the focus army none), and its command stream gives
  // the orders, beat by beat. False with the reason when it cannot start.
  bool LaunchReplay(lua_State* L, const std::string& file, std::string* why);
  bool playingReplay() const { return play_ != nullptr; }
  // The replay timeline's host side (ours: user/replay_timeline.h) while a replay plays, for its interface; nullptr
  // otherwise. The replay's length is its stream's Advances, counted when it opens; the moments are looked for on a
  // thread of their own from then on (StartMomentScan: the engine's ScanReplayMoments, sim/moments.h), kept
  // across a jump back and stopped when the replay closes. A jump forward runs the beats ahead as fast as they go
  // (fastForwardTo, the world's sounds and shakes left out meanwhile); a jump back starts the replay over (a
  // restart, RestartSession's way: the interface is made anew) and then runs ahead.
  ReplayTimelineSource* timeline();
  // How the host mounted the game, for the scan's own copy (--init, --folder; the maps are replayMaps), and where the
  // scan stops (0: at the replay's end; --scan-ticks, for tests).
  std::string scanInit;
  std::map<std::string, std::string> scanFolders;
  int scanTicks = 0;

  // The game's window (the live host's; nullptr without one): the world's picture is made on it
  // and shown there by the renderer, the interface laid over it (PresentToWindow). With
  // `waitForWindow` a session started before the window opens its picture in the window's first
  // frame (OpenPictureIfDue).
  void* window = nullptr;
  bool waitForWindow = false;
  void OpenPictureIfDue();
  // The front end's background (app/backdrop.h; nullptr: none): while it is begun the direct way
  // and not stopped, the window's picture without a game shows its map under the menus, from the
  // camera it gives each frame the movie showed (Backdrop::TakeView); else black under the
  // interface (the map is kept, as through the loading screen, until the game's is swapped in).
  // The window has ONE picture of the renderer's (render request 37): OpenWindowPicture makes it
  // at the start, before the window shows, without a map (the interface shows at once), and begins
  // the background's map behind it; every map after (a game's, the menus' again) is opened behind
  // the one shown while the window goes on showing frames, and swapped in when ready.
  Backdrop* menuBackdrop = nullptr;
  void OpenWindowPicture();
  bool DrawsToWindow() const;
  // Where the interface's controls hand a picture over the whole window to the graphics card (the
  // loading screen's film, the "IN TRANSIT" cover: maui/world_source.h, render request 38), for
  // UserState::Config::cardBackdrop; InterfaceRendered once a frame after the interface's drawing
  // (what it handed over is the frame's).
  CardBackdrop* cardBackdrop();
  void InterfaceRendered();
  // The frame drawn and shown: the world, `target` (the interface, premultiplied ARGB, the
  // window's size, transparent where the world shows) over it. `changed`: the part of `target`
  // drawn anew since the last call (a picture new or of another size takes all of it).
  void PresentToWindow(const SoftwareTarget& target, const IntRect& changed);
  // Once a frame, before its input is read: the renderer's wait for the frame's time with Low Latency on
  // (WorldPicture::WaitForNextFrame on the picture about to be shown; FramePacing::lowLatency).
  void WaitForNextFrame();
  // For checking: the next shown frame is read back and saved as a PNG.
  void CaptureWindow(const std::string& path);

  bool running() const { return sim_ != nullptr; }
  // A launch is loaded the original's way (its session states, WLD_Frame 0x88cae0): the launch call
  // only takes the session's description (the load is then due: LoadDue); the host shows the game's
  // loading screen in the front end's own state and calls BeginLoad, which runs the simulation's
  // rules and its start on the worker thread (the original: its "Map loader" background task and
  // its Sim thread) while the front end goes on drawing; LoadDone says when that is through, and
  // FinishLoad makes it the running session (its picture opens) or gives the reason it failed.
  // Loading(): from the launch call until FinishLoad (the front end's WorldIsLoading()).
  bool LoadDue() const { return load_ == Load::Due; }
  // The folder of the map being launched ("/maps/SETONS_R"; empty when no launch is due or loading).
  std::string LoadingMapFolder() const;
  bool Loading() const { return load_ != Load::None; }
  void BeginLoad();
  // Once a frame while a launch loads: the loaded simulation handed to the game's map opening behind (ServeLoad).
  void ServeLoad();
  bool LoadDone() const;
  bool FinishLoad(std::string* why);
  Sim* sim() const { return sim_.get(); }
  // The session's mods by uid, in order.
  const std::vector<std::string>& modUids() const { return modUids_; }
  const std::string& sessionMods() const { return sessionMods_; }  // (the engine's serialized list)
  // Put the game's rules into the Lua state a session's interface runs in: the blueprints as
  // `__blueprints`, the unit categories and the category functions, the way the simulation's own
  // state has them, copied from the session's one rules state, the simulation's own load (Sim::TakeRules
  // of `sim`; the session's mods `uids` and `sessionMods`, the engine's serialized list, for rules of its own when the
  // simulation kept none). On the loading thread (prepareInterface), or on this one after the load.
  bool LoadInterfaceRules(lua_State* userState, Sim* sim, const std::vector<std::string>& uids, const std::string& sessionMods);
  // The interface's own state for the session, made by the host on the loading thread right after the simulation's
  // start (BeginLoad's work, before LoadDone), as the original's "Map loader" task makes it (func_WorldSessionUserLoad
  // 0x885de0): the loading simulation, the session's mod uids and its serialized mods. What it makes is the host's,
  // handed over through the worker's wait (FinishLoad). Unset: nothing is made there.
  std::function<void(Sim* sim, const std::vector<std::string>& uids, const std::string& sessionMods)> prepareInterface;
  const SimBlueprints* interfaceBlueprints() const { return interfaceBlueprints_.get(); }
  // Called with each beat's sync tables (the simulation's bytes) once set. Until then the tables
  // are kept, oldest first, for TakePendingSync: the first beats tell the interface things once only.
  std::function<void(const std::string& sync)> onSync;
  std::vector<std::string> TakePendingSync() { return std::move(pendingSync_); }
  // The sync tables the simulation's start itself made (its setup: OnPopulate, OnStart; the
  // original's "beat 0", which DoInitializing 0x88c3f0 hands the scripts BEFORE the game's screen
  // is made, while beat 1's and on come after it; the engine keeps it apart, ui/REQUESTS.md 33).
  // An operation's has Sync.CampaignMode, which the screen's making reads (the objectives panel).
  std::vector<std::string> TakeSetupSync() { return std::move(setupSync_); }
  // Run the simulation's first beat now if it has not run yet: the interface is made for a game
  // that is under way, and the first beat's sync table tells it the playable area and more.
  void RunFirstBeat();
  // The same, its tick (and the picture's copy of it) on the beats' thread (ours, only about time: the window's frame
  // the game's interface starts in no longer waits for it); FinishFirstBeat waits for it and does the rest here (its
  // record, its sounds, its sync kept for TakePendingSync). With no beats' thread: RunFirstBeat.
  void StartFirstBeat();
  void FinishFirstBeat();
  // While the game's interface is being started over two frames or more (main.cpp): no beat runs and no time is
  // counted for the beats (the original starts its beats only after DoInitializing, 0x88c3f0), and the window shows the
  // loading screen as before the load's end (black under the interface: the game's world view is not made yet, so its
  // picture has no camera). Off again: the world is shown (as OpenPicture left it).
  void HoldBeats(bool on);
  bool BeatsHeld() const { return holdBeats_; }
  // The player's orders since the last beat, as the command stream's bytes; asked before each beat.
  std::function<std::string()> takeOrders;
  // Ends the other worlds alive (the menu's background), asked before the scene's samples change
  // for a new picture: they must not change while a world lives (render/world.h).
  std::function<void()> releaseWorlds;
  // The command source the player speaks for, and the ground's height at a place of the map (from
  // a copy taken after each beat: the interface may ask while a beat runs).
  int commandSource() const { return commandSource_; }
  // The army the player's command source belongs to, 0-based; -1 in a replay (its viewer has none).
  int OwnArmy() const;
  float TerrainHeight(float x, float z) const;
  // Run the beats that are due after `seconds` more of real time (ten beats a second).
  void Advance(double seconds);
  // For measurements (--fast-forward): the beats run as fast as they go, up to 50 a frame, until
  // this beat; then real time again.
  uint32_t fastForwardTo = 0;
  bool CatchingUp() const;
  // Beats on a thread of their own (the window path): the simulation's tick and the picture's copy
  // of it run there while the window keeps showing frames of the world. Everything else that
  // touches the simulation (the interface's scripts and input, orders, sounds, sync) waits until
  // it is done: while BeatRunning(), the window only shows frames.
  bool beatsOnThread = false;
  // The simulation's worker pool (core/workers.h ThreadsOverride, when the Sim is made): 0 leaves it to MOHO64_THREADS
  // (1 when unset: the headless tests' way, many at once). A game in a window takes the machine's cores (main.cpp).
  int simThreads = 0;
  bool BeatRunning() const;
  // While a beat runs there: the time that passes (the picture's moment moves on; no beat starts).
  void AdvanceClock(double seconds);
  // A beat still on its thread: wait for it and finish it (its sounds, its sync).
  void WaitForBeat();
  // The world picture's own numbers for the performance statistics (WorldPicture::LastStats: the
  // frame last shown), as lines of text; none without a picture.
  void StatsLines(std::vector<std::string>* lines) const;
  // Only wait for it (UserState::LiveSim): its sounds and sync come in the next Advance, so no
  // script runs inside a script that asked.
  void WaitForWorker();

  uint32_t tick() const;
  double PictureTick() const;
  size_t unitCount() const;
  // Lines for the host to show or log: the map, the armies, the tick.
  std::vector<std::string> Status() const;
  // The host's own notice on the picture while a session runs (not part of the game's interface).
  void DrawStatus(DrawTarget& target) const;

 private:
  // Running sounds of other armies' units, by entity: bank, cue and where; and those of them the
  // player hears at the moment (see PlayBeatSounds).
  struct ForeignLoop {
    std::string bank, cue;
    float position[3] = {0, 0, 0};
  };
  std::map<uint32_t, ForeignLoop> foreignLoops_;
  std::set<uint32_t> heardLoops_;
  void NoteBeat();  // a beat has run: the picture hears of it, then the sounds and shakes
  void NotePictureBeat();  // the picture's copy of the beat (on the beats' thread, or here)
  void NoteBeatHere();     // the beat's sounds and camera shakes (always here, after the beat)
  void TimedTick();
  class BeatWorker;
  std::unique_ptr<BeatWorker> worker_;
  bool beatToFinish_ = false;  // a beat ran on the worker: its sounds and sync are still to come
  bool firstBeatToFinish_ = false;  // the first beat ran on the worker (StartFirstBeat)
  bool holdBeats_ = false;          // (HoldBeats)
  uint32_t beatFrom_ = 0;      // the tick the running beat started from
  double workerSimSeconds_ = 0, workerPictureSeconds_ = 0, workerFingerprintSeconds_ = 0;  // (written there; read here after it)
  // How early (in game seconds) a beat starts on its thread: about as long as the last one took,
  // so the next beat is there when the picture's moment reaches it (the picture never waits).
  double lead_ = 0;
  std::vector<uint16_t> heights_;  // the ground's heights as of the last beat (TerrainHeight)
  int heightsW_ = 0, heightsH_ = 0;
  float heightsScale_ = 0;
  uint64_t heightsChanges_ = ~0ull;
  void CopyHeights();
  std::vector<int> allies_;  // the focus army's allies as of the last beat (the picture's)
  void CopyAllies();
  double leadBeats_ = 0;         // the beats' own share of it (the longest lately, fading)
  double lastFrameSeconds_ = 0;  // the last frame's real time (Advance's `seconds`)
  void StartBeatOnThread();
  void FinishBeat();
  void DeliverBeatSync();
  // The game's replay file being written (LastGame), and a replay being played back: its bytes
  // and the stream over them. `profile_`: the player's profile name at the launch.
  std::FILE* replay_ = nullptr;
  std::string playData_;
  std::unique_ptr<CommandStream> play_;
  bool lastWasReplay_ = false;
  std::string profile_;
  struct Timeline;
  std::unique_ptr<Timeline> timeline_;
  uint32_t seekTo_ = 0;  // a jump of the timeline under way: the beat it goes to (0: none)
  bool seekBack_ = false;  // (that jump goes back: it waits for the replay to start over, Relaunch)
  bool relaunching_ = false;  // (End called by Relaunch: the replay goes on, its moments' scan with it)
  void StartMomentScan(const ReplayHeader& header, const std::string& file, const std::vector<std::string>& uids);
  void StopMomentScan();
  void SeekReached();  // after beats: a jump that got there (or to the replay's end) is over
  void ReadLookOptions(lua_State* L, int options);  // the lobby's Remaster... options into skyNow_
  void StartReplayFile(const SessionDescription& session);
  void OpenPicture();
  bool pictureDue_ = false;
  std::string captureTo_;  // (the next shown frame is saved here: CaptureWindow)
  void ViewerControls(const std::string& orders);  // the player's pause / resume while a replay plays
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::unique_ptr<Sim> sim_;
  std::vector<std::string> modUids_;
  std::vector<std::string> pendingSync_;
  std::vector<std::string> setupSync_;
  // The window's one picture (see OpenWindowPicture; nullptr: none yet, no window, or it would not
  // open: windowPictureFailed_), and the game's world source over it or over a picture of its own
  // (Picture: declared after it, as it goes first).
  struct WindowPicture;
  std::unique_ptr<WindowPicture> windowPicture_;
  bool windowPictureFailed_ = false;
  bool EnsureWindowPicture(bool remake);
  void BeginWindowMap(bool game, const std::string& map);  // (the game's map or the menus' background's, behind)
  void ServeWindowMap();
  void GiveBackdrop(WindowPicture& wp);
  struct CardSink;
  std::unique_ptr<CardSink> card_;
  struct Picture;
  std::unique_ptr<Picture> picture_;
  void DressGamePicture(Picture& pic);
  std::string failedMenuMap_;  // (a background's map that would not open: not tried again)
  int commandSource_ = 0;
  int pictureFocus_ = -2;    // the focus army the picture was last given (0-based; -2: none yet)
  std::string sessionMods_;  // the session's mods as the engine keeps them (serialized Lua)
  std::unique_ptr<SimBlueprints> interfaceBlueprints_;
  std::string map_;
  size_t armies_ = 0;
  uint32_t seed_ = 0;
  double due_ = 0;  // seconds of beats not run yet
  std::FILE* stream_ = nullptr;  // the game's record (replayOut), open while a game runs
  int recorded_ = 0;             // games recorded in this run
  std::string lastTable_;        // the table file of the game recorded last
  bool tableAgain_ = false;      // the game starting is a restart (Relaunch): its table is the last one
  void Record(const std::string& bytes);
  // The recorded game's fingerprint beside its record (<replayOut>.fp: the engine's sim/fingerprint.h, one line per
  // beat, paused ones included, beat 0 after Start), so that a playback can name the first beat it leaves the
  // game at (tools/twocopies.py --compare). Only for a live game with --replay-out.
  std::shared_ptr<Fingerprint> fp_;
  uint32_t fpBeats_ = 0;
  void FingerprintBeat();
  // The last launch, for Relaunch.
  std::unique_ptr<SessionDescription> last_;
  std::vector<std::string> lastUids_;
  Sky skyNow_;  // the running game's (kept for Relaunch)
  // The launch being loaded (BeginLoad): its description, its simulation until FinishLoad hands it
  // to sim_, and what the worker found (written there, read here after it).
  enum class Load { None, Due, Running };
  Load load_ = Load::None;
  std::unique_ptr<SessionDescription> loadSession_;
  std::unique_ptr<Sim> loadSim_;
  std::vector<std::string> loadUids_;
  bool loadOk_ = false;
  bool loadPrepared_ = false;  // (the loaded simulation handed to the map opening behind: ServeLoad)
  std::string loadWhy_;
  double loadRulesSeconds_ = 0, loadStartSeconds_ = 0;
  void PrepareSession(const SessionDescription& session, const std::vector<std::string>& uids);
};

}  // namespace moho::ui
