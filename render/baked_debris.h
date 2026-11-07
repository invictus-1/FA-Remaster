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
//
// [debrispile] The pile is the wreck. A unit's wreck (the game's prop, World's made props: picked, reclaimed and taken away by
// the game's rules as ever) whose kind has a set is drawn as that set's chunks instead of the wreck's model (World tells each
// such wreck every frame: Wreck). Its death, seen, plays on; from where its chunks come to rest they lie at their last frame
// on the ground, never sinking, for as long as the wreck stands, and go when it goes. A wreck is matched to its death by its
// kind, its place (within the unit's size and some) and its time (the death played from ten seconds before the wreck
// appeared to four after): while a death of its kind is still to be thrown near it (the debris's `coming`), it waits, drawn
// by none; a wreck whose death was not seen (there from the start, a game played back from elsewhere, met later through the
// fog) is drawn as the break style's last frame at once, the wreck's place and heading. Should its death be thrown a moment
// later after all, the death takes it over. A death no wreck is matched to (the game left none) lies and sinks away as
// before. The chunks cool into the wreck's own textures (ModelCache::DebrisChunks, the unit's <ID>_wreck_lab_*: the same
// texture places), burnt through by the time they rest; a pile shown for an unseen death is burnt through.
//
// [debrispile] A style may bring its own chunks' mesh (<ID>_debris_<style>.scm: the same chunks and rest middles, bent and
// heated its own way; else the set's <ID>_debris.scm). MELT: the hot parts sag down and toward the side the death came from
// (its front, until the engine's killing hit says) in the vertex shader, by their height over their chunk's lowest point
// and their heat, over its first seconds, then held (meltSag; 0 for a melt bent in its bake). VAPORISE: the chunks thrown
// beyond the unit's box (strayBeyond) are debris, not the wreck: they lie and sink away as a death's without a wreck, the
// biggest trailing the debris's smoke while they fly; those within it stay as the wreck's look.
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
class Particles;
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
  // [debrispile] MELT: how far its hot parts sag (a share of their height over their chunk's lowest point, at full heat: the
  // vertex shader), over its first `meltSeconds`, then held (0: none, for a set whose melt is bent in its bake)
  float meltSag = 0.8f;  // ([debrisstyle] Chris, 2026-11-07: the live sag doubled)
  float meltSeconds = 2;
  // [debrispile] VAPORISE: the chunks that end beyond this many times the unit's box (across) are thrown debris, not the wreck:
  // they lie and sink away as a death's without a wreck; the biggest `mostTrails` of those flying trail the debris's smoke
  float strayBeyond = 1.3f;
  int mostTrails = 8;
  // [debrisairsea] the water's level (none: very low; the debris's), and where a chunk meets it (a splash; may be empty)
  float waterLevel = -1e9f;
  std::function<void(Vec3 at, float size)> splash;
  Particles* smoke = nullptr;  // (set by the debris: where the trails go, and which)
  std::string trail;

  // Whether the unit has a set (its files looked for once).
  bool Has(const std::string& unitId);
  // [debrishull] A ship's hull without the parts its set throws (<ID>_debris_hull.scm; ModelCache::DebrisHull), or null.
  const Model* HullOf(const std::string& unitId);
  // A unit dies now (`clock`, the game's): plays that style of its set at `at` (its place), turned with `turn`'s heading, in
  // its army's `color`. False where it has no set (of that style; else the break style), or too many play already: the
  // caller throws the generic debris then. `unit`: its model as drawn (its scale).
  // [debrisairsea] A set's class (its JSON's "class": land, air, naval). AIR: played where the aircraft came down, `fall` its
  // velocity then: turned to the way it was going (not its nose) and its throw stretched along it by its speed over the
  // bake's ("impact": {"speed"}, kept between half and one and a half). NAVAL: played on the water's surface where the ship
  // died (the bake's y 0 is the water), its hull the game's own sinking ship. Either on water (an aircraft coming down at
  // sea, a ship): each chunk follows its keys until it meets the water (a naval set's keys end there; an air set's where
  // its chunk first touches its ground), splashes, then sinks at the JSON's "sink" units a second (0.6), slowly tilting,
  // to the sea bed, where it lies and later sinks into the bed as the land's do. A land set on water plays nothing.
  bool Play(const std::string& unitId, const Model* unit, Vec3 at, const Quat& turn, Vec3 color, double clock, const std::string& style = "break",
            Vec3 fall = {});
  // Moves every death on to `clock` and adds those within sight of `eye` to `meshes` (between its Begin and Prepare; null: moved
  // only).
  void Update(double clock, Vec3 eye, MeshRenderer* meshes);
  // The deaths whose chunks are still hot (hot metal on): the middle of their chunks, how far they reach, their age and how
  // long they glow, their own number; for the hot metal's few lights (HotMetal::Glowing).
  void ForEachHot(const std::function<void(Vec3 centre, float radius, float age, float seconds, uint32_t id)>& f) const;
  // [debrispile] Whether a wreck of this kind is drawn as its pile (it has a set with a style: break, else melt, else vaporise,
  // the one a wreck whose death was not seen is drawn by).
  bool HasPile(const std::string& unitId);
  // [debrispile] A wreck that stands now (told every frame, before Update; one not told since the last Update is gone, and its
  // pile with it): its prop's number, its kind, where it stands (on the ground), its turn and size, when it appeared (game
  // seconds).
  void Wreck(uint32_t prop, const std::string& unitId, Vec3 at, const Quat& turn, float scale, double born);
  // [debrispile] (set by the debris) whether a death of this kind near this place is still to be thrown
  std::function<bool(const std::string& unitId, Vec3 at)> coming;
  void Clear();  // every death taken away (a map opened anew, a game played back from earlier)
  int Playing() const;  // deaths in flight
  int Sets() const { return static_cast<int>(deaths_.size()); }
  int Piles() const;  // [debrispile] wrecks drawn as their piles
  int matched = 0, unseenPiles = 0;  // [debrispile] wrecks matched to a death seen; drawn without one
  int played = 0;    // deaths played since the map was opened
  int refused = 0;   // deaths with a set that threw the generic debris (too many in flight)

 private:
  // a style: each chunk's keys (its bone, then seven numbers a key: x, y, z in model units, the turn as x, y, z, w), the keys
  // made after the bake's end for what still flew (one a frame, at the bake's rate, until it is down), how long all last
  struct Track {
    uint32_t bone = 0;
    std::vector<float> keys;
    float reach = 0;  // how far it reaches from its middle (model units: its farthest vertex)
    // [debrispile] when it comes to rest (seconds); thrown beyond the wreck (VAPORISE: Style::strays); one of the biggest
    // thrown, trailing smoke while it flies
    float rests = 0;
    bool stray = false, smokes = false;
    uint32_t touches = 0;  // [debrisairsea] the key where it first meets the bake's ground (y 0; a naval set's: its last)
    // [debrisairsea] its longest way (model units, at rest) and how far it reaches across that way: sinking, it turns to lie
    // along the bed, and rests on its thickness, not on its length
    Vec3 along{0, 0, 1};
    float thin = 0;
  };
  struct Style {
    float fps = 30;
    float seconds = 0;  // the bake's length
    float settled = 0;  // when the last chunk is down (the bake's end, or later for what flew on)
    std::vector<Track> tracks;  // in rising bone order
    // [debrispile] its chunks' mesh (<ID>_debris_<style>.scm, else <ID>_debris.scm: the same chunks, bent its own way); whether
    // its far-thrown chunks are debris rather than wreck, with smoke (vaporise); whether its hot parts sag (melt)
    const Model* model = nullptr;
    bool strays = false, melts = false;
    // [debrisairsea] land 0, air 1, naval 2; an air set's arriving speed (0: not given); how fast its chunks sink in water
    int cls = 0;
    float impactSpeed = 0, sink = 0.6f;
  };
  struct Set {
    std::unordered_map<std::string, std::unique_ptr<Style>> styles;  // by name ("break"); a missing one null (asked once)
    std::string folder, trial;  // (where its files are: the export's, and the trial folder's)
    const Model* hull = nullptr;  // [debrishull] (a ship's hull without the thrown parts; asked once)
    bool hullAsked = false;
  };
  Set* SetOf(const std::string& unitId);
  const Style* StyleOf(Set& set, const std::string& unitId, const std::string& style);
  const Style* PileStyle(Set& set, const std::string& unitId);  // [debrispile] (break, else melt, else vaporise)
  std::unique_ptr<Style> ReadStyle(const Model& chunks, const std::string& mesh, const std::string& path, const std::string& name);
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
    // [debrispile] the wreck it is drawn as (its prop's number; 0: none yet), whether it stands for a death not seen (at its
    // style's last frame, burnt through), and when its heat began (its death; for one not seen its wreck's appearing)
    uint32_t prop = 0;
    bool unseen = false;
    double heatFrom = 0;
    std::vector<uint32_t> trails;  // [debrispile] (its chunks' smoke trails, by track; 0: none)
    // [debrisairsea] on water (its place at the surface); an air set's stretch along its way; when all of it is down (the
    // style's, or later for what sinks); which chunks have splashed
    bool sea = false;
    float stretch = 1, settled = 0;
    std::vector<uint8_t> splashed;
    std::vector<FrameBone> bones;  // (this frame's pose: its chunks', where the model renderer reads them)
    Vec3 centre;                   // (this frame's: the middle of its chunks, how far they reach)
    float reach = 0;
  };
  std::vector<std::unique_ptr<Death>> deaths_;
  std::unordered_map<std::string, std::unique_ptr<Set>> sets_;  // by unit id, lower case; null: none
  double clock_ = -1;
  uint32_t serial_ = 1;
  // [debrispile] the wrecks told (Wreck), by their props' numbers
  struct Standing {
    std::string unit;
    Set* set = nullptr;
    Vec3 at;
    Quat turn;
    float scale = 1;
    double born = 0, told = -1;  // when it appeared; when it was first told (the clock)
    uint32_t frame = 0;          // the Update it was last told before
  };
  std::unordered_map<uint32_t, Standing> wrecks_;
  uint32_t frame_ = 0;
  void MatchWrecks(double clock);
  void Drop(Death& d);  // [debrispile] (its trails stopped, as it goes)
};

}  // namespace render
