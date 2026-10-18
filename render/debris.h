// Debris (World::debris; the modern look, to be seen only: nothing goes back to the simulation): when a unit or a
// building dies, a handful of small chunks fly out of it, tumble, bounce on the ground and come to rest, and stay as
// small rubble round where it died. The rubble lies two minutes (`rubbleLasts`), or until there is more than
// `mostRubble` of it; then the oldest sinks into the ground over ten seconds, as the scorches fade (decals.h).
//
// A chunk is a small rough shape (six of them, some blocky, some flat, some wedges) wearing a patch of the dead unit's
// own textures (its finest level's: its paint, its army's colour where it has a team mask, its metal), half of them
// burnt as its wreck is (ModelLod::wreck). They are drawn as the models are (mesh_renderer.h), lit, shadowed and seen
// by the rays as everything else, as scenery (they press no grass flat). Small: a twentieth to a sixth of the unit
// across, never more than half a world unit (the readability rule).
//
// A unit's own fragments (to come: the visuals session's wreck kit tears a unit into fitted pieces of its own model and
// texture) take the generic chunks' place where `fragments` gives some for it: each piece a model of its own, flying
// from where it sat in the unit (Piece).
//
// Where deaths are seen: a frame's units (a game's, and the effects stand's units, which it marks dead when it kills
// them). A unit on the ground throws its chunks with its death's explosion: the flash its script makes on it (after a
// short wait from its death, before its death animation and its wreck: Unit.lua, DeathThread; Blasts::FlashedOn);
// without one seen, when it is gone, or four seconds after its death at the latest. One dying in the air throws them
// when it has come down to the ground (or where it was last seen, when it is gone before that); one on or in the water
// none.
// The chunks fly on the game's clock (a paused game's stand still) under the simulation's gravity, and bounce off the
// map's heights; one that falls into the water is gone, with a splash told to the blasts (blasts.h, for the water's
// ripples).
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "textures.h"

namespace render {

class Blasts;
class MeshRenderer;

class Debris {
 public:
  Debris(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  ~Debris();

  // The units of a frame (at most once a frame): what has died throws its chunks (see above). `model`: a unit's model by
  // its blueprint id (as drawn); `color`: an army's colour (from 0); `flashes`: the flashes made on entities (may be
  // null: then each throws them as soon as it is seen dead). Only units the army the picture is for sees now.
  void Watch(const Frame& frame, double clock, const std::function<const Model*(const std::string&)>& model,
             const std::function<Vec3(uint32_t)>& color, const Blasts* flashes);
  // One piece a death throws: its model, drawn at `scale`; where it sat in the unit (its middle, in the unit's own frame,
  // world units) and how it was turned there; its size across (world units: how it bounces and lies).
  struct Piece {
    const Model* model = nullptr;
    Vec3 rest;
    Quat turn;
    float scale = 1, size = 0.1f;
  };
  // A unit's own fragments in place of the generic chunks (unset, or none for a unit: the generic chunks). `unitId`: its
  // blueprint id; `unit`: its model as drawn.
  std::function<std::vector<Piece>(const std::string& unitId, const Model* unit)> fragments;
  // A unit's pieces thrown now: its blueprint id and model, its place and turn, its army's colour, how fast it was
  // moving. Its own fragments where `fragments` has some, else a handful of the generic chunks from its lower body.
  void Throw(const std::string& unitId, const Model* model, Vec3 position, const Quat& turn, Vec3 color, Vec3 velocity, uint32_t seed);
  // Moves the chunks on to `clock` and adds those within sight of `eye` to `meshes` (between its Begin and Prepare).
  // `splashes`: told where chunks fall into the water (may be null).
  void Update(double clock, Vec3 eye, MeshRenderer* meshes, Blasts* splashes);
  // Everything thrown and lying taken away (a map opened anew).
  void Clear();

  // The map (World::Open): the ground's height (not the water's), the water's level (none: very low), its size.
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;
  float mapWidth = 0, mapHeight = 0;
  int mostRubble = 1500;  // rubble kept lying; past it the oldest sinks away
  // how long rubble lies before it sinks away (seconds; 0: until there is more than mostRubble). The world keeps it
  // till then while the battle scars are kept (World::battleScars), as the scorches it lies on.
  float rubbleLasts = 120;
  float amount = 1;       // chunks a death, times this
  int Flying() const { return static_cast<int>(flying_.size()); }
  int Lying() const { return static_cast<int>(lying_.size()); }
  int thrown = 0;  // chunks since the map was opened

 private:
  struct Chunk {
    Vec3 at, velocity, spin;  // spin: radians a second about each axis
    Quat turn;
    float size = 0.1f, scale = 0.1f;  // its size across (world units); what its model is drawn at
    Vec3 color;  // its army's
    const Model* look = nullptr;
    int bounces = 0;
    double settled = 0, fading = 0;  // when it came to rest; when it began to sink away (0: not yet)
    Mat4 world;                      // (lying: where it is drawn)
    uint32_t key = 0;                // (for its motion, mesh_renderer.h)
  };
  // a dying unit being watched: whether its chunks are thrown, and where it was last
  struct Dying {
    uint32_t blueprint = 0, army = 0;
    Vec3 at, velocity;
    Quat turn;
    double seen = 0, died = 0;  // when it was seen last; first seen dead
    uint32_t stamp = 0;
    bool thrown = false, air = false;
  };
  static constexpr int kShapes = 6;
  void MakeShapes();
  // a unit's chunk models: each shape painted (0..5) and burnt (6..11)
  const std::array<std::unique_ptr<Model>, 2 * kShapes>& Looks(const Model* unit);
  float Ground(float x, float z) const { return groundHeight ? groundHeight(x, z) : 0.0f; }
  float Random();
  gfx::Device* device_;
  TextureCache* textures_;
  struct Shape {
    gfx::Buffer vertices, indices;
    uint32_t vertexCount = 0, indexCount = 0;
  };
  Shape shapes_[kShapes];
  bool shapesMade_ = false;
  std::unordered_map<const Model*, std::array<std::unique_ptr<Model>, 2 * kShapes>> looks_;
  std::vector<Chunk> flying_;
  std::deque<Chunk> lying_;  // in the order they came to rest
  std::unordered_map<uint32_t, Dying> dying_;
  uint32_t stamp_ = 0, nextKey_ = 1, serial_ = 1, seed_ = 0x9e3779b9u;
  double clock_ = -1;
};

}  // namespace render
