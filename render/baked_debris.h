// [bakeddebris] Baked death debris (World::blastDebris, with the debris; the modern look; to be seen only: the game's wreck and
// its reclaim stay as the game makes them). A unit whose remastered folder holds a set of its own (the visuals track's, made in
// Blender from the unit's real parts) dies by throwing those parts as the bake threw them, instead of the debris's generic
// chunks (debris.h: the death throws none of those, and the game's debris shells thrown near it are not drawn).
//
// A set (docs/render-answers.md, "To visuals, section 101"):
//  - <ID>_debris.scm: the chunks as one mesh, one bone a chunk (every bone a root, standing at its chunk's middle, no turn at
//    rest), in the unit's own model space and its own textures (the same texture places as its finest level; ModelCache::
//    DebrisChunks). Its second texture coordinates' u is a heat mask: where the metal is cut or molten (0..1).
//  - <ID>_debris_<style>.json, one a style (break, melt, vaporise: by the killing weapon, once the engine tells it; break
//    until then): {"fps": 30, "seconds": 2.5, "chunks": [{"bone": "Chunk_03", "keys": [[x, y, z, qw, qx, qy, qz], ...]}]}, a
//    key a frame, each the chunk's whole pose in the unit's model space (key 0 its rest), baked on flat ground at y 0.
// Found beside the unit's remastered export (/preview/<id>/), or first in a trial folder (the viewer's --debris-dir, mounted
// at /debris-trial/: each file there takes the place of the export's). Read once a kind, at its first death.
//
// Played at the unit's place, turned with its heading (the bake stands on flat ground: never tilted with a unit on a slope),
// at the unit's scale; each chunk's height laid on the ground under it (the bake's flat ground is the map's ground there, so
// on a slope a chunk neither floats nor sinks). A chunk the bake leaves still in the air or still moving at its end flies on
// as it went (the bake's own gravity, its last spin) until it meets the bake's ground, worked out once as the style is read.
// After that the chunks lie still, then sink slowly into the ground and go. Their heat mask glows with the hot metal's colour
// (World::hotMetal, hot_metal.h: white-orange to dull red as it cools, over a wreck's time), and its light, while hot, falls on
// the ground with the other hot things' (ForEachHot).
//
// At most `mostPlaying` deaths in flight at once (more throw the debris's generic chunks, as before); at most `mostLying` sets
// lying (past it the oldest sinks away at once).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace moho {
class Vfs;
}

namespace render {

class HotMetal;
class MeshRenderer;
class ModelCache;
struct Model;

class BakedDebris {
 public:
  // (set by the world) the models, the file system, the hot metal's tuning (how long a wreck glows; null: 55 seconds) and
  // whether it is on, the map's ground height (not the water's)
  ModelCache* models = nullptr;
  const moho::Vfs* files = nullptr;
  const HotMetal* heat = nullptr;
  bool hot = false;
  std::function<float(float, float)> groundHeight;
  // --- tuning ---
  int mostPlaying = 8;      // deaths in flight at once
  int mostLying = 40;       // sets lying at once
  float lieSeconds = 10;    // after the last chunk has come to rest, they lie still this long,
  float sinkSeconds = 70;   // then sink into the ground over this long (each its own height), and are gone
  float paint = 0.85f;      // their army's colour against the unit's (torn paint, a little dulled, as the debris's)

  // Whether the unit has a set (its files looked for once).
  bool Has(const std::string& unitId);
  // A unit dies now (`clock`, the game's): plays that style of its set at `at` (its place), turned with `turn`'s heading, in
  // its army's `color`. False where it has no set (of that style; else the break style), or too many play already: the
  // caller throws the generic debris then. `unit`: its model as drawn (its scale).
  bool Play(const std::string& unitId, const Model* unit, Vec3 at, const Quat& turn, Vec3 color, double clock, const std::string& style = "break");
  // Moves every death on to `clock` and adds those within sight of `eye` to `meshes` (between its Begin and Prepare; null: moved
  // only).
  void Update(double clock, Vec3 eye, MeshRenderer* meshes);
  // The deaths whose chunks are still hot (hot metal on): the middle of their chunks, how far they reach, their age and how
  // long they glow, their own number; for the hot metal's few lights (HotMetal::Glowing).
  void ForEachHot(const std::function<void(Vec3 centre, float radius, float age, float seconds, uint32_t id)>& f) const;
  void Clear();  // every death taken away (a map opened anew, a game played back from earlier)
  int Playing() const;  // deaths in flight
  int Sets() const { return static_cast<int>(deaths_.size()); }
  int played = 0;    // deaths played since the map was opened
  int refused = 0;   // deaths with a set that threw the generic debris (too many in flight)

 private:
  // a style: each chunk's keys (its bone, then seven numbers a key: x, y, z in model units, the turn as x, y, z, w), the keys
  // made after the bake's end for what still flew (one a frame, at the bake's rate, until it is down), how long all last
  struct Track {
    uint32_t bone = 0;
    std::vector<float> keys;
    float reach = 0;  // how far it reaches from its middle (model units: its farthest vertex)
  };
  struct Style {
    float fps = 30;
    float seconds = 0;  // the bake's length
    float settled = 0;  // when the last chunk is down (the bake's end, or later for what flew on)
    std::vector<Track> tracks;  // in rising bone order
  };
  struct Set {
    const Model* model = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Style>> styles;  // by name ("break"); a missing one null (asked once)
    std::string folder, trial;  // (where its files are: the export's, and the trial folder's)
    std::vector<std::vector<Vec3>> points;  // each chunk's vertices from its middle at rest (model units; where it meets the ground)
  };
  Set* SetOf(const std::string& unitId);
  const Style* StyleOf(Set& set, const std::string& unitId, const std::string& style);
  std::unique_ptr<Style> ReadStyle(const Set& set, const std::string& path);
  struct Death {
    const Set* set = nullptr;
    const Style* style = nullptr;
    Vec3 at;            // its place (on the ground there)
    Quat heading;       // its turn about the up
    float scale = 1;    // the unit's
    float radius = 0.5f;  // the unit's size (world units: how its glow's light reaches)
    Vec3 color;
    double born = 0;
    double sinkFrom = -1;  // when it began to sink away early (past mostLying; -1: on its own time)
    uint32_t id = 0;
    std::vector<FrameBone> bones;  // (this frame's pose: its chunks', where the model renderer reads them)
    Vec3 centre;                   // (this frame's: the middle of its chunks, how far they reach)
    float reach = 0;
  };
  std::vector<std::unique_ptr<Death>> deaths_;
  std::unordered_map<std::string, std::unique_ptr<Set>> sets_;  // by unit id, lower case; null: none
  double clock_ = -1;
  uint32_t serial_ = 1;
};

}  // namespace render
