// Frames from a simulation someone else runs: after every beat the one who holds the simulation
// (the viewer's own simulation thread, or the interface's host) says Beat, and what the renderer
// needs of that beat is copied out; whoever draws asks for the frame at a moment between two beats
// and gets them blended. Beat may be called on one thread and the rest on another.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace moho {
class Sim;
class Vfs;
}  // namespace moho

namespace render {

class SimFrames {
 public:
  SimFrames();
  ~SimFrames();

  // Once the game has started: reads the armies' colours as the game shows them (the game's list
  // of colours is /lua/gamecolors.lua in `vfs`) and how every blueprint looks. On the simulation's thread.
  void ReadArmies(const moho::Sim& sim, const moho::Vfs& vfs);
  // The army the picture is for (the engine's number, from 1): from the next beat on every unit
  // and projectile says what that army knows of it (FrameUnit::recon). 0: no one army, as at first.
  void SetFocusArmy(int army) { focus_.store(army); }
  // The map's size, for the grid of what the focus army sees (TakeSight): `cells` x `cells` places
  // over the whole map, asked of the engine's ReconAt every fifth beat. Without it there is no grid.
  void SetSightGrid(float mapWidth, float mapHeight, int cells = 256);
  // What the focus army sees, when that has been worked out anew since the last call: for every
  // place the bits of `SightBits` (frame.h), row by row from z = 0. An empty grid: there is no
  // focus army any more. False: nothing new.
  bool TakeSight(std::vector<uint8_t>* grid, int* cells);
  // After every beat, on the simulation's thread. For the scripts' effects the simulation must have
  // had `collectEffects` set from before its first beat.
  void Beat(const moho::Sim& sim);

  uint32_t FirstTick() const;  // of the beats kept
  uint32_t LastTick() const;   // the newest beat
  // The frame at a moment given in ticks, blended between the two beats around it (clamped to the
  // beats kept). False while there is no beat yet.
  bool FrameAt(double tick, Frame* out);
  // The effect events of the beats since the last call, in order (each says its beat).
  void TakeEffectEvents(std::vector<FrameEffectEvent>* out);
  // How the blueprints look, once: the list is handed over and this object's is emptied.
  void TakeLooks(std::vector<BlueprintLook>* out);
  // Each army's colour, by army index from 0; empty until ReadArmies.
  std::vector<Vec3> ArmyColors() const;
  // The session's seed (the same for every player of a game and in its replay); 0 until ReadArmies.
  uint32_t Seed() const;

 private:
  struct Beat_ {
    uint32_t tick = 0;
    std::vector<FrameUnit> units;  // in entity-id order
    std::vector<FrameBone> bones;  // what the units' spans point into
    std::vector<FrameProjectile> projectiles;
    std::vector<FrameBeam> beams;
  };
  struct Scratch;  // the simulation's view structures, kept out of this header
  std::unique_ptr<Scratch> scratch_;

  mutable std::mutex mutex_;
  std::deque<Beat_> beats_;            // the newest few
  std::vector<std::string> names_;     // blueprint ids, by FrameUnit::blueprint and FrameProjectile::blueprint
                                       // (0: unknown); only grows
  std::vector<Vec3> armyColors_;
  std::vector<BlueprintLook> looks_;
  uint32_t seed_ = 0;
  std::atomic<int> focus_{0};
  float sightWidth_ = 0, sightHeight_ = 0;  // (set before the game runs)
  int sightCells_ = 0;
  std::vector<uint8_t> sight_, sightWork_;
  std::vector<uint8_t> sightEver_;  // what the focus army has seen at some time (the simulation's thread only)
  int sightEverOf_ = 0;
  bool sightNew_ = false, sightWasOn_ = false;
  uint32_t sightBeat_ = 0;
  std::vector<FrameEffectEvent> effectEvents_;  // not yet taken
  std::vector<std::string> namesForFrames_;     // the drawing side's copy of names_ (what Frame::blueprints points at)
};

}  // namespace render
