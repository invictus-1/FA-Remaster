// The main menu's flight (ours, not the original's: Chris approved it on 2026-11-08; docs/ui-menuflight.md): behind
// the main menu a camera flies over Seton's Clutch Remastered while a staged battle plays there, UEF against Cybran,
// experimentals and then a nuke, and starts over. The battle is an ordinary simulation of the engine's, run here beside
// the front end and apart from the game's session (GameSession's sim_ never sees it): its own scenario and script
// (ui/data/menuflight: menu_session.lua, setons_r_menu_scenario.lua, menu_battle.lua; no AI, no orders, the same seed:
// the same battle every time), on a thread of its own, at ten beats a second while the main menu shows and paused
// while it does not. The camera follows a path of keys (setons_r_flight.path, the renderer's CinePath text) by the
// battle's own clock, so the two stay in step whatever the frame rate or a slow beat.
//
// This class drives the simulation and the camera; the window's picture (GameSession: it owns the renderer's picture)
// gives it the map, the session and each beat (PrepareSession before the swap, then GameStarted, Beat), and the
// main menu's background (Backdrop, its Flight mode) asks it for the camera each frame the menu shows.
// Without a window (--menu-flight, tests) it runs the same simulation with no picture at all.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "script/lua.hpp"

namespace moho {
class Sim;
class Vfs;
struct SessionDescription;
}  // namespace moho
namespace render {
struct Camera;
class CinePath;
}  // namespace render

namespace moho::ui {
namespace sound {
class SoundSystem;
}

class MenuFlight {
 public:
  // The map the battle plays on (the scenario's), and the files in this program's own folder (/moho64ui, ui/data).
  static constexpr const char* kMap = "/maps/SETONS_R/SETONS_R.scmap";
  static constexpr const char* kSessionFile = "/moho64ui/menuflight/menu_session.lua";
  static constexpr const char* kPathFile = "/moho64ui/menuflight/setons_r_flight.path";

  MenuFlight(Vfs* vfs, std::vector<std::string> hookDirs);
  ~MenuFlight();

  // The session described once, from the game configuration file through `L` (a front end's user state: the scenario
  // file is run there, as a launch's is), and the camera's path read; false (and Failed with the reason) when either
  // cannot be (the map is not there: a PC without SETONS_R).
  bool Describe(lua_State* L);
  bool Described() const { return session_ != nullptr; }
  // The simulation made and started on the flight's own thread (its rules, then its start: some 2.5 s); nothing when
  // it is loading or running already, or failed.
  void Begin();
  // Without a window (--menu-flight): a load on the thread waited for here, so that the frames, which go faster than the
  // PC's clock there, meet the battle at the same frame on every run.
  void WaitLoaded();
  // Everything stopped: the beat or the load on the thread waited for, the simulation let go of (on a thread of its own,
  // a second or so for a big one). Begin starts it over from its first beat. (Before any game's session is made: the
  // engine's rule of one simulation at a time while a game loads, and the file system's mounts change then.)
  void Stop();

  // Where it stands. Loading: its thread makes the simulation; Ready: made, not shown yet (the picture takes it,
  // PrepareSession); Running: its beats go on while it is shown; Ended: the storyboard's end reached (the host covers
  // the picture and starts it over, Restart); Failed: not to be had this run (FailedWhy).
  enum class State { Idle, Loading, Ready, Running, Ended, Failed };
  State state() const;
  const std::string& FailedWhy() const { return failedWhy_; }
  void Fail(const std::string& why);  // (the host's: the map would not open, the picture cost too much)
  // The simulation while Ready, Running or Ended (no beat runs while the host reads it between frames: WaitForBeat).
  Sim* sim() const;
  // The picture's side: called on the flight's thread with each beat's simulation right after its tick (the
  // renderer's WorldPicture::Beat), and on this thread when a simulation is let go of (nothing more of it is read).
  std::function<void(const Sim& sim)> onBeat;
  // The host's side of the loop: the storyboard's end reached; the host covers the picture and then calls Restart: the
  // same battle from its first beat, a new simulation loaded meanwhile (Ready again: the host then starts the map shown
  // over, WorldPicture::RestartMap, and gives it the new one).
  void Restart();

  // Once a frame the main menu's background is drawn or would be: `shown`, whether it shows this frame; `seconds`, the
  // frame's time. While shown the battle's clock runs and the beats due start on the thread; hidden, both wait (a
  // hidden spell of more than kRestartAfterHidden seconds starts the battle over at the next showing).
  void Advance(double seconds, bool shown);
  // A load on the thread through: Ready (or Failed); Advance does it too (the window's host asks before the battle is
  // shown: its simulation goes to the map opening behind while Ready, GameSession::ServeMenuFlight).
  void Poll();
  static constexpr double kRestartAfterHidden = 120;
  // The battle's moment (the tick and the share of the next beat gone by: the picture's `tick`), and the camera there
  // (the path's pose at the battle's seconds, CineCamera::MakeView over `ground`, the picture's). True once the battle
  // shows (Running or Ended).
  double Moment() const;
  bool View(const std::function<float(float, float)>& ground, render::Camera* camera) const;
  // The first pose's camera, for the map's warm frames before the swap (WorldPicture::SetLoadCamera), over the
  // simulation's own ground.
  bool StartView(render::Camera* camera) const;
  // A cut is due (the battle started over): the host tells the renderer (CameraCut) once.
  bool TakeCut();
  // Started over this many times; the tick the beats are at; the beats wait for a showing.
  int loops() const { return loops_; }
  uint32_t tick() const;
  bool Paused() const { return !shownLast_; }
  double pathLength() const;
  double lastBeatMs() const { return beatSeconds_ * 1000.0; }  // (the last beat's time on its thread: the log)

  // Tests and measurements (MOHO64UI_MENUFLIGHT_FAST=1: up to 50 beats a frame, not ten a second; a run without a window
  // reaches the storyboard's end in seconds). MOHO64UI_MENUFLIGHT_FP=<file>: each beat's fingerprint line (the engine's
  // sim/fingerprint.h), a run's own numbered <file>.<loop>.
  bool fast = false;
  // (MOHO64UI_MENUFLIGHT_FAST_TO=<tick>: as fast up to that tick, then ten a second: a window's pictures of the battle's
  // later moments without waiting minutes for them)
  uint32_t fastUntil = 0;
  // The simulation's worker pool (core/workers.h ThreadsOverride): 1-2 for the menu (a game takes them all).
  int simThreads = 2;
  // The battle's sounds (the option Menu Battle Sound, Distant): the simulation lists them (collectSounds), the host
  // plays them. Off: none are made.
  bool collectSounds = false;
  sound::SoundSystem* sounds = nullptr;  // (what plays them: each beat's once-off sounds, after the beat, here)

  // Tests (moho64ui_MenuFlight): the state as a table.
  void PushInfo(lua_State* L) const;

 private:
  class Worker;
  void StartBeat();
  void FinishBeat();
  void ReadPath(const std::function<float(float, float)>& ground);
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::unique_ptr<SessionDescription> session_;
  std::string pathText_;  // (the path file, its # comments taken out)
  std::unique_ptr<render::CinePath> path_;  // (parsed over the simulation's ground once a simulation is there)
  std::unique_ptr<Worker> worker_;
  std::unique_ptr<Sim> sim_;
  std::atomic<int> state_{static_cast<int>(State::Idle)};
  std::string failedWhy_;
  bool loadOk_ = false;  // (written on the thread, read here after its wait)
  std::string loadWhy_;
  double loadSeconds_ = 0;
  bool beatToFinish_ = false;
  uint32_t beatFrom_ = 0;
  double due_ = 0;          // seconds of beats due and not begun
  double lead_ = 0;         // (a beat starts this early: about as long as the last took)
  double beatSeconds_ = 0;  // (the last beat's time on the thread)
  bool shownLast_ = false;
  std::chrono::steady_clock::time_point hiddenSince_{};
  bool restartAtShowing_ = false;
  bool cut_ = false;
  int loops_ = 0;
  uint32_t endTick_ = 0;
  uint32_t lastEndTick_ = 0;  // (the last run's end, kept over its start again: tests)
  std::FILE* fp_ = nullptr;  // (MOHO64UI_MENUFLIGHT_FP)
  uint32_t fpBeats_ = 0;
  std::thread letGo_;  // (a simulation let go of)
  void LetGo(std::unique_ptr<Sim> sim);
};

}  // namespace moho::ui
