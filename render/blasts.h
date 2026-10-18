// Blasts: what the game's explosions do to the world round them, to be seen only (the modern look; nothing goes back to
// the simulation: the renderer only reads the game).
//
// The list: every explosion the game shows leaves a trace in what its scripts make, and that is all the renderer is told
// of it. A unit's death and a shell's impact lay a scorch or a crater on the ground (CreateDecal, CreateSplat: a decal
// Decals::IsScar names), and a death and many impacts throw a flash of light (CreateLightParticle). Effects tells the
// list of each (FromScar, FromFlash): where, how large (the scorch's size across, or the flash's glow size, which the
// game's own death script makes 2 to 3 times the unit's radius: defaultexplosions.lua, CreateScalableUnitExplosion),
// and when. A scorch and a flash of one explosion (the same place, the same moment) are one blast. The list keeps the
// last half minute's (Recent), for whatever else wants to know where things blew up (the water's ripples).
//
// What a blast does, each a player's option of the world's (World::blastWaves, blastDust):
//  - its wave: a ring that runs out from it over the ground, quick (10 to 45 world units a second, by its size) and
//    reaching some times its size (2.5 + 5 x size, at most 60). Where the ring passes, grass and plants are pressed
//    flat away from the blast (grass.h, by the field this module makes over the grass's square, Field) and trees and
//    bushes bend away from it (mesh_renderer.cpp, in their sway), then spring back with a swing past upright that dies
//    away: grass within a second, trees in two or three. The shaders read the blasts whose waves are under way near
//    what the camera sees (kBlastShaderBlock, at most kMost): everything else is left as it was.
//  - its dust: a bigger blast (size 0.8 and up) kicks a low ring of dust off the ground, coloured as the ground there
//    (Terrain::GroundColours), soft where it meets the ground, lit as the game's smoke is (particles.cpp), short-lived;
//    low and thin (the readability rule: it must not hide units).
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

// The blasts' waves as shaders read them (register b1), and what they do at a place. BlastPush(xz, tree): which way
// and how far (radians from upright) what stands at xz is bent this moment, summed over the blasts whose waves have
// reached it; `tree` 0 for grass and plants (bent far over, up again within a second with a swing past upright), 1
// for trees (bent less, slower).
extern const char kBlastShaderBlock[];

class Blasts {
 public:
  explicit Blasts(gfx::Device* device) : device_(device) {}
  ~Blasts();

  struct Blast {
    Vec3 at;              // where (the place the script gave: on the ground, or on the water)
    float size = 1;       // how large, world units: about the explosion's radius
    double born = 0;      // when, on the effects' clock (seconds)
    bool water = false;   // on water (the ground there lies under the water's surface)
    bool splash = false;  // no explosion: a piece of debris fallen into the water (debris.h); no wave, no dust
  };
  // What the game made (Effects): a flash of light of that glow size (CreateLightParticle's), a scorch or a crater of
  // that size across (a scar decal's: Decals::IsScar), at a place and a moment. Both of one explosion make one blast.
  // `entity`: what the flash was made on (a unit's death explosion is made on the unit), or none.
  void FromFlash(Vec3 at, float glowSize, double born, uint32_t entity = 0xffffffffu);
  void FromScar(Vec3 at, float sizeX, float sizeZ, double born);
  // Whether a flash was made on that entity at `since` or later (within the last ten seconds). A unit's death explosion
  // is one: its script makes it after a short wait, before its death animation and its wreck (Unit.lua, DeathThread),
  // and the debris throws a dying unit's chunks then (Debris::Watch).
  bool FlashedOn(uint32_t entity, double since) const;
  // A shot's impact, where the game says one happened (the engine's per-beat lists, src/sim/sim_view.h: renderer
  // request 36): where, and the shot's DamageRadius and Damage (its weapon's numbers; a HitEvent of an area's damage
  // has them as radius and amount, its position the blast's centre). Sized as the scorch the game lays for it (0.75
  // to 0.95 of DamageRadius across, less below 100 damage: /lua/sim/Projectile.lua), so the two make one blast; a
  // shot without a radius (a direct hit) a small one. The game lays its scorch for one impact in ten close together
  // only: this hears of every one. (The several hits of one area's damage, one a target, make one blast here.)
  void FromImpact(Vec3 at, float damageRadius, float damage, double born);
  // A blast of a size (world units) itself: the effects stand's --fx-blast; and a piece of debris falling into water.
  void Add(Vec3 at, float size, double born, bool splash = false);
  // The blasts of the last half minute (at most kKept), the oldest first.
  static constexpr size_t kKept = 512;
  const std::deque<Blast>& Recent() const { return recent_; }
  void Clear() {  // (a map opened anew)
    recent_.clear();
    fresh_.clear();
    flashes_.clear();
  }
  // The blasts listed since the last call (at most 256: the newest kept), for whoever acts on each once (the water's
  // ripples: Water::AddRipple where `water`, with the size; a splash's size is its piece's).
  void TakeNew(std::vector<Blast>* out) {
    out->swap(fresh_);
    fresh_.clear();
  }
  int told = 0;  // blasts listed since the map was opened (merged ones once)
  // The map under them (World::Open): the ground's height (not the water's) and the water's level (none: very low).
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;
  // How large a blast must be to kick up dust (world units).
  float dustFrom = 0.8f;

  // --- a frame (World::Render) ---
  // The blasts whose waves (`waves`) or dust (`dust`) are under way at `clock` and reach into what `view` sees, at
  // most kMost, the nearest to `focus` first: their constants for the shaders (kBlastShaderBlock). Active: how many.
  static constexpr int kMost = 32;
  void Prepare(double clock, const View& view, Vec3 focus, bool waves, bool dust);
  int Active() const { return active_; }
  bool Waves() const { return wavesNow_ && active_ > 0; }  // (this frame's constants are for the waves too)
  const void* Constants() const { return gpu_.data(); }
  uint32_t ConstantsSize() const { return static_cast<uint32_t>(sizeof(float) * 4 * (1 + 3 * static_cast<size_t>(std::max(active_, 1)))); }
  // The waves over the grass's square (GroundCover's area: its first cell's x, z, a cell's size, cells a side): which
  // way and how far the grass is bent at each texel (RG16F; Field). Dispatches: before the scene's pass. False: none
  // this frame (no wave there, or its shader could not be made).
  bool PrepareField(const float area[4]);
  gfx::Texture Field() const { return field_; }
  // The dust rings, into the scene's colour alone (the particles' pass: it tests the scene's depth itself).
  // `heights`, `heightUnit`: Terrain::HeightTexture, HeightUnit; `groundColours`: Terrain::GroundColours.
  void DrawDust(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                float mapWidth, float mapHeight);
  int dustPuffs = 0;  // (the last DrawDust: puffs gone through)

 private:
  bool Init();  // (its shaders, the first time they are wanted: false where they cannot be made)
  // (one of the list, as the shaders read it: three float4s)
  void Write(const Blast& b, double clock, float* out) const;
  float Reach(float size) const;
  float Speed(float size) const;
  float Lasts(const Blast& b) const;  // how long after it its wave or its dust is still under way
  gfx::Device* device_;
  std::deque<Blast> recent_;
  std::vector<Blast> fresh_;  // (TakeNew's)
  std::deque<std::pair<uint32_t, double>> flashes_;  // (the flashes made on entities: which, when; FlashedOn's)
  std::vector<float> gpu_ = std::vector<float>(4 * (1 + 3 * kMost), 0.0f);
  int active_ = 0, dustBlasts_ = 0;
  double lastClock_ = -1;
  bool wavesNow_ = false, dustNow_ = false;
  int tried_ = 0;  // 0 not yet, 1 made, -1 they cannot be
  gfx::Pipeline fieldPass_, dust_;
  gfx::Texture field_;
};

}  // namespace render
