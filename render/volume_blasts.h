// Explosions as real volumes, and a nuke worth the name (the modern look's World::volumeExplosions; the next looks,
// item 13; docs/render-volumes.md). Big blasts, and every nuke, are drawn as lit volumes over the game's own effects:
// a smoke simulation's density and temperature, played back over the blast's life from a file the visuals session
// makes (a flat DDS of slices and frames and a JSON file that says how to read it: the delivery form is in the doc),
// marched through in a box round each blast, lit by the sun through its own smoke, the shadow maps, the clouds' and
// the other smoke's shadows, by the sky and the small lights, and glowing by its temperature as a hot body does. While
// it is hot it is a small light itself. A nuke besides: its flash lights the sky and the whole map for a moment (and
// the clouds from below), its shock ring runs out through the volumetric clouds and clears them, the ground glows
// under it, and its mushroom cloud rises and lingers.
//
// Each frame, per blast drawn (at most `most`, and one nuke):
//  - Fill (compute): the volume at this moment, from the two frames of the file round it, each read where this
//    moment's box lies in that frame's box, into a field of the file's grid (density, temperature);
//  - DrawShadow: the volume as the sun sees it, into the smoke's shadow map (smoke.h), so that the ground, the units,
//    the light in the air and the other smoke take its shadow;
//  - Light (compute): at half the grid, the light that reaches each place in it: the sun's through the volume itself
//    (and the shadow maps, the clouds' shadow, the other smoke's), the sky's from above, the small lights';
//  - Draw: its box's far faces drawn into the scene's colour, each pixel marching from where the ray enters the box
//    to where it leaves it or meets something solid (the scene's depth, sample by sample), front to back.
// Marched in a box rather than laid into the light in the air's cells (air.h): those are 160 x 90 x 64 over the
// whole view, too coarse for a blast's shape close up; the box costs only where a blast is on the screen.
//
// Which blasts: what a script's flash says (Lights::onFlash: the game's "light particles", as large as the glow it
// draws on the ground) and the simulation's harm to an area (World::AddCombat's hits with a radius): a flash of
// `smallest` or larger, or an area of `smallestArea` or larger, is a blast; a flash of `nukeFlash` or larger a nuke.
// Several of one blast (a unit's death flashes, a nuke's flashes over its first second) are one. A dying unit's blast is
// as large as the unit (the flash the game's death explosion throws is sized by the unit's hitbox, smaller than its
// model). Only the UEF's strategic missiles are nukes here (each faction gets its own effects in its own pass, Chris
// 2026-10-21): another faction's keeps the original's look (no volume for it, its own later flashes or its harm; the
// deaths it causes make theirs).
//
// An emitter whose blueprint says VolumeReplaces = "inside" is shared with explosions too small for a volume (a
// medium unit's death): it is started as ever, and taken back (stopped, what it threw within the moment let go of) when
// a volume is born within `insideSeconds` of it and over it (onBorn, Covers; Particles::VolumeBorn).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "lights.h"
#include "linalg.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

class Lights;
class TextureCache;

class VolumeBlasts {
 public:
  VolumeBlasts(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures);
  ~VolumeBlasts();

  // A flash a script made (Lights::onFlash): its ramp texture, where, its glow's size, when (the game's clock,
  // seconds) and for how long.
  void Offer(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from = {});
  // Harm to an area (a hit with a radius: DamageArea), at that moment. `nukeHarm`: a strategic missile's own ('Nuke').
  void OfferArea(Vec3 position, float radius, double born, bool nukeHarm = false);
  // Forgets every blast (a map opened, the game started over).
  void Clear();
  // The colour a ramp texture glows in (the world sets it: Lights::RampGlow): a nuke's flash takes its hue.
  std::function<Vec3(const std::string&)> rampGlow;
  // VolumeReplaces = "inside": told of each blast as it is born or grows (where, how large, when), for the emitters
  // started just before it under it to be taken back (the world sets it: Particles::VolumeBorn); and whether one born
  // within `insideSeconds` of `when` is over that place (for those started just after it: they are not started).
  std::function<void(Vec3 at, float radius, double born)> onBorn;
  bool Covers(Vec3 at, double when) const;

  // --- a frame (World::Render), in this order ---
  // The blasts alive at `clock` (ended ones let go of), which are drawn (the files and the pipelines made the first
  // time; false: none this frame). `view`: the scene's.
  bool Update(double clock, const Camera& camera, const View& view);
  // Their light while they are hot, the nuke's flash and the ground's glow under it (before Lights::Build).
  // `night`: what the game's glowing effects are multiplied by (Lights::night); `daylight` 0 at night .. 1 at noon.
  void AddLights(Lights* lights, float night, float daylight);
  // Between passes, before the smoke's shadow map is drawn: each drawn blast's volume at this moment.
  void Fill(const View& view);
  // Into the smoke's shadow map's pass (smoke.h), with its view.
  void DrawShadow(const View& sunView);
  // Between passes, after the shadow maps, the clouds' shadows and the smoke's shadow are made (they are bound
  // here at their slots; the smoke's none when it is not drawn this frame) and the lights built (bound here).
  void Light(const View& view, gfx::Texture sunShadows, gfx::Texture cloudShadows, gfx::Texture smokeShadows, Lights* lights);
  // Into the scene's colour (a pass of colour alone, as the particles'), before the particles.
  void Draw(const View& view, gfx::Texture sceneDepth, float zNear);

  // --- a nuke, for the world's other looks ---
  // What its flash adds to the sky's light now (linear, as the sky's colours are; 0: none). World::Begin.
  Vec3 SkyFlash(double clock) const;
  // Its flash in the clouds (the volumetric clouds lit from within over it, as by lightning): where, how far, how
  // bright (false: none).
  bool CloudFlash(double clock, Vec3* at, float* reach, Vec3* light) const;
  // Its shock ring through the volumetric clouds: the ring's middle (x, z), its radius and width (world units) and
  // how strongly it clears and pushes them (0..1). False: none now.
  bool CloudRing(double clock, float* x, float* z, float* radius, float* width, float* strength) const;

  // --- tuning ---
  int most = 4;                // blasts drawn at once (besides one nuke)
  float smallest = 5;          // a flash's size (its glow's) from which it is a blast
  float smallestArea = 3;      // the radius of harm to an area from which it is a blast
  float nukeFlash = 100;       // a flash's size from which it is a nuke
  float blastPerFlash = 0.32f; // a blast's radius (the file's unit) per size of its flash
  float blastPerArea = 0.9f;   // ... per radius of its harm
  float largest = 10;          // the largest a blast is (world units; a strategic bomb's) but for a dying unit's
  float deathPerWidth = 1.2f;  // a dying unit's blast's radius per half its model's width, when that is more than its flash says
  float largestDeath = 20;     // the largest a dying unit's blast is (an experimental's, a battleship's)
  float insideSeconds = 0.3f;  // VolumeReplaces = "inside": how near a blast's birth an emitter is taken back
  float nukeRadius = 40;       // a nuke's radius when nothing says otherwise (the game's strategic missiles' outer ring)
  int steps = 48;              // the most steps a pixel marches through a volume
  float detail = 0.45f;        // how much a finer noise frays the volumes' edges and shades their folds (0: none)
  float detailSize = 1.5f;     // that noise repeats every this many of a blast's radii
  float glowUnit = 14.0f;      // how bright a file's glow of 1 is (linear light; the sun at noon lights white about 1.4)
  float ringSpeed = 20.0f;     // how fast a nuke's ring first runs out through the clouds (world units a second; it slows)

  // --- numbers, of the last frame ---
  int alive = 0, drawn = 0;
  bool nukeDrawn = false;

 private:
  struct Sequence {
    bool tried = false, ok = false;
    int nx = 0, ny = 0, nz = 0, frames = 0, slicesAcross = 1, framesAcross = 1;
    int atlasWidth = 0, atlasHeight = 0;
    std::vector<float> times;        // seconds of each frame
    std::vector<float> boxes;        // six per frame: middle and half-size, in radii, from the origin (y up)
    std::vector<float> heat;         // per frame, 0..1: how much of it glows and how hot (its light)
    std::vector<std::string> ramps;  // the flashes' ramps it is for ("ramp_nuke_04"; none: the kind's own, "blast" or "nuke")
    float density = 6, kelvin = 2600, glow = 1;
    Vec3 albedo{0.45f, 0.43f, 0.40f};
    gfx::Texture atlas;
    float Seconds() const { return times.empty() ? 0 : times.back(); }
  };
  struct Blast {
    Vec3 at;              // where it went off
    float radius = 1;     // the file's unit, world units
    double born = 0;
    bool nuke = false;
    float flashSize = 0;  // its largest flash
    std::string ramp;     // that flash's ramp (a nuke's flash takes its hue)
    Vec3 flashColor{1, 0.9f, 0.75f};
    bool colored = false;
    mutable const Sequence* sequence = nullptr;  // (its file, chosen the first time it is wanted)
    mutable bool chosen = false;
  };
  // What one drawn blast is this frame, and its volumes on the card (kept from frame to frame per place).
  struct Slot {
    gfx::Texture field, light;  // density and temperature at the file's grid; the light at half of it
    int nx = 0, ny = 0, nz = 0;
    // this frame's
    int blast = -1;
    const Sequence* sequence = nullptr;
    Vec3 low, size;             // the box (world)
    float fade = 1;
    int frameA = 0, frameB = 0;
    float share = 0;
    Vec3 lowA, sizeA, lowB, sizeB;
    float age = 0;
  };
  bool Ready();
  void Scan();
  Sequence* SequenceOf(const Blast& b);
  bool Load(const std::string& name, Sequence* s);
  void Place(Slot* slot, const Blast& b, Sequence* s, double clock);
  bool Room(Slot* slot, const Sequence& s);
  void Constants(const Slot& s, const View& view, float* out) const;
  void Start(Vec3 at, float radius, double born, bool nuke, float flashSize, const std::string& ramp);
  const Blast* NukeNow(double clock) const;
  // Whether another faction's strategic missile (quiet_), or a nuke, went off near there lately (a second before to 30 s after).
  bool Quiet(Vec3 at, double born) const;
  bool NukeNear(Vec3 at, double born) const;

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  std::vector<Blast> blasts_;
  std::vector<Vec4> rings_;  // harm to an area as large as a nuke's lately: x, z, radius, when
  std::vector<Vec4> quiet_;  // another faction's strategic missiles lately (no volume): x, z, radius, when
  std::unordered_map<std::string, Sequence> sequences_;
  Slot slots_[8];
  int slotCount_ = 0;
  gfx::Pipeline fill_, light_, noise_, draw_, shadow_;
  gfx::Texture detail_;
  bool tried_ = false, made_ = false, noiseMade_ = false, scanned_ = false;
  double clock_ = 0;
};

}  // namespace render
