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
class SimBlueprints;
class Vfs;
}  // namespace moho

namespace moho {
struct SessionDescription;
}

namespace moho::ui {

namespace sound {
class SoundSystem;
}

class GameSession {
 public:
  GameSession(Vfs* vfs, std::vector<std::string> hookDirs);
  ~GameSession();

  // Start a session from the game configuration table at stack index `config` of `L` (the user
  // state: the scenario file is run there). False with the reason the original would give the
  // script's LaunchFailed ("NoConfig", "StartSpots"), or a description.
  bool Launch(lua_State* L, int config, std::string* why);
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

  // The game's window (the live host's; nullptr without one): the world's picture is made on it
  // and shown there by the renderer, the interface laid over it (PresentToWindow). With
  // `waitForWindow` a session started before the window opens its picture in the window's first
  // frame (OpenPictureIfDue).
  void* window = nullptr;
  bool waitForWindow = false;
  void OpenPictureIfDue();
  bool DrawsToWindow() const;
  // The frame drawn and shown: the world, `target` (the interface, premultiplied ARGB, the
  // window's size, transparent where the world shows) over it. `changed`: the part of `target`
  // drawn anew since the last call (a picture new or of another size takes all of it).
  void PresentToWindow(const SoftwareTarget& target, const IntRect& changed);
  // For checking: the next shown frame is read back and saved as a PNG.
  void CaptureWindow(const std::string& path);

  bool running() const { return sim_ != nullptr; }
  Sim* sim() const { return sim_.get(); }
  // The session's mods by uid, in order.
  const std::vector<std::string>& modUids() const { return modUids_; }
  // Put the game's rules into the Lua state a session's interface runs in: the blueprints as
  // `__blueprints`, the unit categories and the category functions, the way the simulation's own
  // state has them. (The rules are loaded a second time for this; the simulation keeps its own.)
  bool LoadInterfaceRules(lua_State* userState);
  // Called with each beat's sync tables (the simulation's bytes) once set. Until then the tables
  // are kept, oldest first, for TakePendingSync: the first beats tell the interface things once only.
  std::function<void(const std::string& sync)> onSync;
  std::vector<std::string> TakePendingSync() { return std::move(pendingSync_); }
  // Run the simulation's first beat now if it has not run yet: the interface is made for a game
  // that is under way, and the first beat's sync table tells it the playable area and more.
  void RunFirstBeat();
  // The player's orders since the last beat, as the command stream's bytes; asked before each beat.
  std::function<std::string()> takeOrders;
  // Ends the other worlds alive (the menu's background), asked before the scene's samples change
  // for a new picture: they must not change while a world lives (render/world.h).
  std::function<void()> releaseWorlds;
  // The command source the player speaks for, and the ground's height at a place of the map (from
  // a copy taken after each beat: the interface may ask while a beat runs).
  int commandSource() const { return commandSource_; }
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
  bool BeatRunning() const;
  // While a beat runs there: the time that passes (the picture's moment moves on; no beat starts).
  void AdvanceClock(double seconds);
  // A beat still on its thread: wait for it and finish it (its sounds, its sync).
  void WaitForBeat();
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
  uint32_t beatFrom_ = 0;      // the tick the running beat started from
  double workerSimSeconds_ = 0, workerPictureSeconds_ = 0;  // (written there; read here after it)
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
  struct Picture;
  std::unique_ptr<Picture> picture_;
  // Without a game, on the window: the renderer's picture without a world, which shows the
  // interface (the menus, their moving background's pixels in it) over black. One picture per
  // window at a time: it goes before a game's opens and comes back after the game.
  struct MenuPicture;
  std::unique_ptr<MenuPicture> menuPicture_;
  int commandSource_ = 0;
  std::string sessionMods_;  // the session's mods as the engine keeps them (serialized Lua)
  std::unique_ptr<ScriptState> rules_;
  std::unique_ptr<SimBlueprints> interfaceBlueprints_;
  std::string map_;
  size_t armies_ = 0;
  uint32_t seed_ = 0;
  double due_ = 0;  // seconds of beats not run yet
  std::FILE* stream_ = nullptr;  // the game's record (replayOut), open while a game runs
  int recorded_ = 0;             // games recorded in this run
  void Record(const std::string& bytes);
  // The last launch, for Relaunch.
  std::unique_ptr<SessionDescription> last_;
  std::vector<std::string> lastUids_;
  Sky skyNow_;  // the running game's (kept for Relaunch)
  bool StartSession(const SessionDescription& session, const std::vector<std::string>& uids, std::string* why);
};

}  // namespace moho::ui
