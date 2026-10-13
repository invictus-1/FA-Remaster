// A game started from the lobby: the host's hold on the simulation.
//
// When the lobby script launches a game, the engine builds a session from the game configuration
// (sim/session.h, the engine track's part), loads the rules and starts the simulation. The host
// then runs its beats, ten a second. There is no in-game interface yet and nothing is drawn of the
// world: the host only reports that the simulation runs.
#pragma once
#include <cstdint>
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
class ScriptState;
class Sim;
class SimBlueprints;
class Vfs;
}  // namespace moho

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

  // The world's picture (the renderer's, render/world_picture.h), opened when a game is launched
  // unless wantPicture is off; nullptr when there is none (it did not open, or this build has no
  // renderer).
  bool wantPicture = true;
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
  // The command source the player speaks for, and the ground's height at a place of the map.
  int commandSource() const { return commandSource_; }
  float TerrainHeight(float x, float z) const;
  // Run the beats that are due after `seconds` more of real time (ten beats a second).
  void Advance(double seconds);

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
  void NoteBeat();  // a beat has run: the picture hears of it
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::unique_ptr<Sim> sim_;
  std::vector<std::string> modUids_;
  std::vector<std::string> pendingSync_;
  struct Picture;
  std::unique_ptr<Picture> picture_;
  int commandSource_ = 0;
  std::string sessionMods_;  // the session's mods as the engine keeps them (serialized Lua)
  std::unique_ptr<ScriptState> rules_;
  std::unique_ptr<SimBlueprints> interfaceBlueprints_;
  std::string map_;
  size_t armies_ = 0;
  uint32_t seed_ = 0;
  double due_ = 0;  // seconds of beats not run yet
};

}  // namespace moho::ui
