// Debris (World::blastDebris; the modern look, to be seen only: nothing goes back to the simulation): what flies off a
// unit that dies reads as torn pieces of it, and what lands stays as rubble for a while.
//
// Two kinds of piece:
//  - The game's own debris shells. Every unit's death script throws a handful (defaultexplosions.lua,
//    CreateScalableUnitExplosion: the DebrisMisc04, 09 and 010 projectiles, more for a bigger unit), which the original
//    draws as one flat grey plate of its own (/meshes/projectiles/debris_misc_04: from below or against the light they
//    read as black squares; Chris's pictures, 2026-10-18). Here each is drawn as a torn piece of the unit that threw it
//    (the dying unit nearest where it first shows), fewer of them (`shellsDrawn`) and smaller (`shellSize`); the game
//    still flies it. When it hits (the engine's `stopped`), touches the water or the game ends it, the piece goes on as
//    the debris's own: into the water it sinks, on the ground it hops and comes to rest with the rubble.
//  - Chunks the renderer throws itself (Throw): a unit on the ground throws a few with its death's explosion, from its
//    lower body; they tumble, bounce on the map's heights and lie as rubble.
// The rubble lies two minutes (`rubbleLasts`), or until there is more than `mostRubble` of it; then the oldest sinks
// into the ground over ten seconds, as the scorches fade (decals.h). Dust dulls each piece's paint some time after it
// came to rest.
//
// A piece's shape is one of a small library of irregular fragments (debris_shapes.h: torn and bent plates, folded
// panels, bits of girder and strut, jagged shards, lumps of concrete for buildings), six of them for each kind of unit, by
// what the unit is. Its skin wears a patch of the unit's own texture where its paint lies (Model::paintSpots: its paint,
// its army's colour where it has a team mask, its material; never where its paint glows), three in ten charred instead
// (sooty, dusty: one look for all units); its torn faces are scorched steel (a lump's, broken concrete), and while a
// piece is fresh they glow hot with hot metal on (`hot`, World::hotMetal, hot_metal.h: the same shader's glow as a
// wreck's torn rims). The biggest of the chunks trail the game's thin debris smoke while they fly (`smoke`). Drawn as the models are (mesh_renderer.h), lit, shadowed and seen by the rays as
// everything else, as scenery (they press no grass flat). Small: a thirtieth to a sixth of the unit across, never more
// than half a world unit (the readability rule: never to be taken for units, shots or icons from the game's height).
//
// A unit's own fragments (to come: the visuals session's wreck kit tears a unit into fitted pieces of its own model and
// texture) take the library's place where `fragments` gives some for it: each piece a model of its own, flying from
// where it sat in the unit (Piece); the game's shells then wear them too.
//
// Where deaths are seen: a frame's units (a game's, and the effects stand's units, which it marks dead when it kills
// them). A unit on the ground throws its chunks with its death's explosion: the flash its script makes on it (after a
// short wait from its death, before its death animation and its wreck: Unit.lua, DeathThread; Blasts::FlashedOn);
// without one seen, when it is gone, or four seconds after its death at the latest. One dying in the air throws them
// when it has come down to the ground (or where it was last seen, when it is gone before that); one on or in the water
// none (the game's shells are its debris).
// Everything flies on the game's clock (a paused game's stands still) under the simulation's gravity; what falls into the
// water splashes (told to the blasts, blasts.h, for the water's ripples) and sinks.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "debris_shapes.h"
#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "textures.h"

namespace render {

class Blasts;
class MeshRenderer;
class Particles;

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
  // A unit's own fragments in place of the library's (unset, or none for a unit: the library's). `unitId`: its
  // blueprint id; `unit`: its model as drawn.
  std::function<std::vector<Piece>(const std::string& unitId, const Model* unit)> fragments;
  // A unit's pieces thrown now: its blueprint id and model, its place and turn, its army's colour, how fast it was
  // moving. Its own fragments where `fragments` has some, else a handful from the library, from its lower body.
  void Throw(const std::string& unitId, const Model* model, Vec3 position, const Quat& turn, Vec3 color, Vec3 velocity, uint32_t seed);

  // The game's own debris shells, in a frame of a game (World::AddFrame, after Watch): BeginShells once, then Shell for
  // each projectile seen. Shell: true when it is one of the death's debris shells (by its blueprint: its index among the
  // frame's and its path) and the debris draws it (as a torn piece, or not at all: fewer are drawn than the game
  // throws); the caller then does not. `scale`: the size the game draws it at; `stopped`: it hit something this beat.
  void BeginShells();
  bool Shell(uint32_t id, uint32_t blueprint, const std::string& path, Vec3 at, const Quat& turn, Vec3 velocity, float scale, bool stopped);

  // Moves the chunks on to `clock` and adds those within sight of `eye` to `meshes` (between its Begin and Prepare).
  // `splashes`: told where chunks fall into the water (may be null).
  void Update(double clock, Vec3 eye, MeshRenderer* meshes, Blasts* splashes);
  // Everything thrown and lying taken away (a map opened anew).
  void Clear();
  // A check view (the viewer's --debris-show): the library's shapes in a row along `right` (level) from `at`, each `size`
  // across, turning slowly, in that unit's paint (the row above) and charred (below), with their torn faces (glowing as
  // fresh ones do, again every few seconds, with `hot`). Between the meshes' Begin and Prepare.
  void Show(const Model* unit, Vec3 at, Vec3 right, float size, double clock, MeshRenderer* meshes);

  // The map (World::Open): the ground's height (not the water's), the water's level (none: very low), its size.
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;
  float mapWidth = 0, mapHeight = 0;
  int mostRubble = 1500;  // rubble kept lying; past it the oldest sinks away
  // how long rubble lies before it sinks away (seconds; 0: until there is more than mostRubble). The world keeps it
  // till then while the battle scars are kept (World::battleScars), as the scorches it lies on.
  float rubbleLasts = 120;
  float amount = 1;          // chunks a death, times this
  float shellsDrawn = 0.65f;  // the share of the game's debris shells drawn
  float shellSize = 0.9f;     // their size against the game's plate's
  // Set by the world each frame: hot metal is on (World::hotMetal): fresh pieces' torn faces glow for `hotSeconds`;
  // where the biggest chunks' smoke goes (null: none).
  bool hot = false;
  float hotSeconds = 5;
  Particles* smoke = nullptr;
  int Flying() const { return static_cast<int>(flying_.size()); }
  int Lying() const { return static_cast<int>(lying_.size()); }
  int Shells() const { return static_cast<int>(shells_.size()); }
  int thrown = 0;  // chunks since the map was opened
  int shellsSeen = 0;  // the game's debris shells since the map was opened (drawn or not)

 private:
  struct Chunk {
    Vec3 at, velocity, spin;  // spin: radians a second about each axis
    Quat turn;
    float size = 0.1f, scale = 0.1f;  // its size across (world units); what its model is drawn at
    Vec3 color;                       // its army's
    const Model* look = nullptr;      // its skin
    const Model* torn = nullptr;      // its torn faces (none: a unit's own fragment)
    const Model* dusty = nullptr;     // its skin once dust has settled on it (none: it stays as it is)
    bool solid = false;               // (a lump: its fractures are large, drawn at any distance)
    bool smokes = false;              // (one of the biggest: it trails smoke while it flies)
    uint32_t trail = 0;               // its smoke's emitter
    int bounces = 0;
    double born = 0, settled = 0, fading = 0, sank = 0;  // thrown; came to rest; began to sink away; went into the water
    float dustAfter = 0;              // seconds after it came to rest that its paint is dulled
    Mat4 world;                       // (lying: where it is drawn)
    uint32_t key = 0;                 // (for its motion, mesh_renderer.h; its torn faces' is key | 0x20000000)
  };
  // a dying unit being watched: whether its chunks are thrown, and where it was last
  struct Dying {
    uint32_t blueprint = 0, army = 0;
    Vec3 at, velocity;
    Quat turn;
    double seen = 0, died = 0;  // when it was seen last; first seen dead
    uint32_t stamp = 0;
    bool thrown = false, air = false;
    std::string unit;              // its blueprint id
    const Model* model = nullptr;  // as drawn
    Vec3 color;                    // its army's
    float radius = 1;              // half its size across (world units)
  };
  // one of the game's debris shells
  struct GameShell {
    Chunk piece;         // what is drawn (where the game has it)
    bool drawn = false;  // (one of the share drawn)
    bool taken = false;  // (its piece is the debris's own now: TakeOver)
    uint32_t stamp = 0;  // the last frame it was in (BeginShells')
  };
  // A unit's looks: for each shape of the library its painted skin (each its own mesh, its texture places laid
  // onto a patch of the unit's paint); and the few shapes its pieces are (`picks`: kPicks of the library, as its kind
  // has them, the same every run). Every shape a pass draws is a draw of its own, so a unit's pieces keep to a few: a
  // close look at a heap of one unit's rubble then costs about what the first version's six shapes did.
  static constexpr int kPicks = 6;
  struct Looks {
    std::array<std::unique_ptr<Model>, kDebrisShapes> painted;
    std::vector<gfx::Buffer> buffers;
    std::vector<int> picks;
  };
  void MakeShapes();
  const Looks& LooksOf(const Model* unit, char kind);
  std::vector<Vec3> ClearSpots(const Model* unit, float piece);
  // a piece of the library for a unit (of that kind: its blueprint id's third letter), its army's colour, that size
  void Dress(Chunk* c, const Model* unit, char kind, Vec3 color);
  // a piece goes into the water (from flying, or a game's shell the game ended there)
  void Sink(Chunk* c, double clock);
  void TakeOver(GameShell* s, double clock);
  void StopTrail(Chunk* c);
  void Draw(MeshRenderer* meshes, const Chunk& c, const Mat4& world, const Model* look, double clock, bool near);
  float Ground(float x, float z) const { return groundHeight ? groundHeight(x, z) : 0.0f; }
  float Random();
  gfx::Device* device_;
  TextureCache* textures_;
  std::vector<DebrisShape> shapes_;
  // each shape's skin's corner numbers (every unit's mesh of it has its corners in the same order), its charred skin and
  // its torn faces (one of each for all units)
  struct Shape {
    gfx::Buffer skinIndices;
    uint32_t skinCount = 0;
    std::unique_ptr<Model> charred;
    gfx::Buffer charredVertices;
    std::unique_ptr<Model> torn;
    gfx::Buffer tornVertices, tornIndices;
  };
  Shape shape_[kDebrisShapes];
  gfx::Texture steel_, steelMaterial_, concrete_, concreteMaterial_;  // (the torn faces' textures)
  gfx::Texture charred_, charredMaterial_;                            // (the charred skin's)
  bool shapesMade_ = false;
  std::unordered_map<const Model*, Looks> looks_;
  std::vector<Chunk> flying_;
  std::deque<Chunk> lying_;  // in the order they came to rest
  std::unordered_map<uint32_t, Dying> dying_;
  std::unordered_map<uint32_t, GameShell> shells_;
  std::vector<int8_t> shellKinds_;  // (by blueprint index: -1 not known yet, 0 not a debris shell, 1 one)
  uint32_t shellStamp_ = 0;
  bool shellFrame_ = false;  // (BeginShells since the last Update)
  uint32_t stamp_ = 0, nextKey_ = 1, serial_ = 1, seed_ = 0x9e3779b9u;
  double clock_ = -1;
};

}  // namespace render
