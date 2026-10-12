// The world's picture as pixels, for a host that has its own window and its own way of drawing
// (the interface's program with its software backend): the host holds the simulation, tells this
// class about every beat, and asks once a frame for the world as seen by a camera, as a block of
// pixels it then shows under its own controls. The same object answers where things are on that
// picture and what is under a point of it.
//
// This is the first, simple way to join the two (docs/render-answers.md). It costs a read-back of
// the picture from the graphics card every frame; the way without that is one window and one
// device for both, the interface's controls drawn by the renderer's device on top of the world.
//
// Use, with a simulation `sim` and the game's files mounted in `vfs`:
//   sim.collectEffects = true;                       before the simulation's first beat
//   render::WorldPicture picture;
//   picture.Open(&vfs, "/maps/SCMP_009/SCMP_009.scmap", &error);
//   picture.GameStarted(sim);                        once the session has started
//   picture.Beat(sim);                               after every Tick() (and once for the state at the start)
//   const std::vector<uint32_t>& pixels = picture.Draw(camera, width, height, tick);
// `tick` is the moment to show, in ticks; give the simulation's tick plus the part of the next
// beat that has passed (0..1) and units move smoothly between beats. The picture always shows a
// moment that lies one beat back, between the last two beats it was told of, because the place of
// a unit between two beats can only be known once the second one exists.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera.h"
#include "linalg.h"

namespace moho {
class Sim;
class Vfs;
}  // namespace moho

namespace render {

class World;

class WorldPicture {
 public:
  WorldPicture();
  ~WorldPicture();

  // Makes an invisible window and a graphics device and loads the map. `vfs` must outlive this.
  bool Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error);
  void GameStarted(const moho::Sim& sim);
  void Beat(const moho::Sim& sim);

  // The world as `camera` sees it, `width` x `height` pixels, each 0xAARRGGBB, row by row from the
  // top (the form of the interface's moho::ui::Image). Valid until the next Draw.
  const std::vector<uint32_t>& Draw(const Camera& camera, int width, int height, double tick);

  // For the picture last drawn; x and y in its pixels.
  // Where a place in the world is on the picture. False when it is behind the camera.
  bool Project(Vec3 world, float* x, float* y) const;
  // The place on the ground (or the water's surface) under a point of the picture.
  bool GroundAt(float x, float y, Vec3* hit) const;
  // The entity id of the unit under a point of the picture; 0xffffffff: none.
  uint32_t UnitAt(float x, float y) const;
  // The units whose middle lies in a rectangle of the picture.
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const;

  // The height of what one stands on at a place of the map (ground or water surface).
  float GroundHeight(float x, float z) const;
  // The world itself, for its settings (look, shadows, icons, ...). Null before Open.
  World* world();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
