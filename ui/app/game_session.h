// A game started from the lobby: the host's hold on the simulation.
//
// When the lobby script launches a game, the engine builds a session from the game configuration
// (sim/session.h, the engine track's part), loads the rules and starts the simulation. The host
// then runs its beats, ten a second. There is no in-game interface yet and nothing is drawn of the
// world: the host only reports that the simulation runs.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "draw/draw.h"
#include "script/lua.hpp"

namespace moho {
class Sim;
class Vfs;
}  // namespace moho

namespace moho::ui {

class GameSession {
 public:
  GameSession(Vfs* vfs, std::vector<std::string> hookDirs);
  ~GameSession();

  // Start a session from the game configuration table at stack index `config` of `L` (the user
  // state: the scenario file is run there). False with the reason the original would give the
  // script's LaunchFailed ("NoConfig", "StartSpots"), or a description.
  bool Launch(lua_State* L, int config, std::string* why);

  bool running() const { return sim_ != nullptr; }
  // Run the beats that are due after `seconds` more of real time (ten beats a second).
  void Advance(double seconds);

  uint32_t tick() const;
  size_t unitCount() const;
  // Lines for the host to show or log: the map, the armies, the tick.
  std::vector<std::string> Status() const;
  // The host's own notice on the picture while a session runs (not part of the game's interface).
  void DrawStatus(DrawTarget& target) const;

 private:
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::unique_ptr<Sim> sim_;
  std::string map_;
  size_t armies_ = 0;
  uint32_t seed_ = 0;
  double due_ = 0;  // seconds of beats not run yet
};

}  // namespace moho::ui
