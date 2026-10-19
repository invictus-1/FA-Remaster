// The remastered build look (Chris's list of 17 October, look 19; renderer item 19): building made more alive in the
// modern look, a player's option (World::remasteredBuild, viewer --remastered-build), off by default. The original
// look, and the modern look with the original effects chosen, keep the build look as it was.
//
// Three parts:
//  1. Build beams light what they touch. Every build beam the game's scripts draw (a beam blueprint named
//     "build_beam": build_beam_01 to 03, aeon_build_beam_01 to 06, seraphim_build_beam_01 and 02; Effects hands them
//     over) is a small light where it meets what it builds, flickering a little (an arc's quick flicker for the UEF
//     and the Cybrans, a slow breath for the Aeon and the Seraphim), in the beam's own colour taken toward white, and a
//     faint light half way along a long beam. Where it meets: where the beam enters the box of the unit under
//     construction it points at (the game's beam end often lies inside the unit, or at its foot; the box as the unit is
//     drawn while built: the Seraphim's smaller, the Aeon's smaller and sunk), pulled back a little toward the builder,
//     so that the lit spot is on the side the builder works on. Beams of one builder that end together are one light.
//  2. Sparks, thrown where each beam meets the unit: moved on the graphics card, each a short streak that bounces off
//     what the scene's depth shows (the unit being built, the ground, anything solid in view) and off the ground's
//     heights where the picture does not show it, dies in water, and cools in colour as it ages. Per faction, so each
//     stays itself: the UEF's welding sparks (white-blue hot, through yellow and orange to a dull red, bouncing), the
//     Cybran's smaller, quicker, redder sparks, the Aeon's drops of liquid gold (heavy, hardly bouncing, glinting),
//     the Seraphim's motes of light (rising slowly, drifting, fading in and out). Where a beam meets the surface is
//     found on the card along the beam from the scene's depth (near what it builds), so the sparks come off the surface
//     the camera sees; each is drawn as a small capsule along its motion, round at its ends, a pixel wide at least.
//  3. A better build shimmer: the mesh shader's variant built with REMASTERED_BUILD (kBuildShaderBlock) in place of
//     its BuildSolid and of the surface of what is under construction. Each faction's look is kept and improved:
//       UEF: built from the top down as before (the game's box of light sinks round what is not there yet); the level
//         of the work is a white-blue weld seam with the fresh metal above it still glowing orange and cooling within a
//         few hundredths of the height; below, the blue lattice as before, its lines kept a pixel wide at any distance
//         (no crawling moire far off), a thin scan line running up it.
//       Cybran: plate by plate in no order as before; each new plate comes in with its edges white-hot, then red, then
//         cool; where plates are missing the red lattice, anti-aliased, with the build bots' glints crawling along it.
//       Aeon: rising out of the ground, a little smaller at first, as before; the liquid metal is gold (the game's
//         pool it rises from too, so the two are one), its surface flowing (ripples running over it, a faint warm light
//         of its own), and it sets from the foot up over the last two fifths with a pale blue seam where it sets (the
//         game's Aeon build glows are blue), in place of all of it setting at once.
//       Seraphim: growing from a quarter of its size, dark and lit from within, as before; the light is a net of slow
//         flowing veins (stronger toward the outline), and the surface takes over through a dissolving front with a
//         bright rim, in place of a plain cross-fade.
//     In all four the team colour comes in over the last tenth, as before, and the unit stays readable as itself.
//
// Off, nothing of this runs and nothing is drawn differently (the mesh shader's variant is not used, the effects list
// no beams). Costs and pictures: docs/render-build.md.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class Lights;
class ModelCache;
struct Frame;

// The mesh shader's code for the remastered build look (mesh_renderer.cpp, its REMASTERED_BUILD variant): after the
// view, ray and lighting blocks, before the mesh shader itself.
extern const char kBuildShaderBlock[];

class BuildEffects {
 public:
  explicit BuildEffects(gfx::Device* device) : device_(device) {}
  ~BuildEffects();

  // A new frame (World::Begin): whether the look is on for it; the beams and sites listed are forgotten. `side`: a
  // picture for something else (the minimap's): nothing is listed, and the sparks under way are kept for the next.
  void Begin(double clock, bool on, bool side = false);
  bool On() const { return on_; }

  // From the effects (effects.cpp), while it is on:
  // Is that beam blueprint a build beam?
  static bool IsBuildBeam(const std::string& blueprint);
  // A build beam drawn this frame: from `start` to `end`, of a faction's ("uef", ...; "": not known), the colour it
  // glows in (black: it does not glow), `key` the same from frame to frame for one builder's beam to one thing.
  void AddBeam(Vec3 start, Vec3 end, const std::string& faction, Vec3 glow, uint64_t key);
  // The frame's units under construction (where the beams meet them).
  void TakeSites(const Frame& frame, ModelCache* models);

  // From the world's Render, while it is on:
  // The beams' lights, and the sparks thrown this frame. Before Lights::Build. `night`: what the effects' lights are
  // multiplied by (Lights::night).
  void AddLights(Lights* lights, float night);
  // The sparks moved on and bounced off the scene's depth (a dispatch: it ends the pass under way). `heights`: the
  // ground's (Terrain::HeightTexture, one a grid point) and their unit; `water` the water's level (-1e9: none).
  void Simulate(const View& view, const Camera& camera, gfx::Texture sceneDepth, gfx::Texture heights, float heightUnit, float mapWidth,
                float mapHeight, float water);
  // The sparks drawn into the scene's colour (a pass into it alone: Post::BeginColorOnly), behind what is solid.
  void Draw(const View& view, gfx::Texture sceneDepth, const Camera& camera);
  // Whether there are sparks that may be alive (else Simulate and Draw do nothing).
  bool Busy() const { return on_ && (spawnCount_ > 0 || clock_ < aliveUntil_); }

  // Numbers of the last frame, for the viewer's report: beams listed, beam ends lit, sparks thrown.
  int beams = 0, lit = 0, thrown = 0;
  // Tuning: how bright a beam's light is, how many sparks a beam end throws (1: as tuned).
  float lightStrength = 1, sparkRate = 1;

  static constexpr int kSide = 64;                // the sparks' state is kSide x kSide
  static constexpr int kCapacity = kSide * kSide;  // sparks alive at most
  static constexpr int kMostNew = 512;            // sparks thrown in one frame at most

 private:
  struct Beam {
    Vec3 start, end, glow;
    int faction;  // 0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim; -1 not known
    uint64_t key;
  };
  struct Site {  // a unit under construction: its box
    Vec3 centre, half;
    Quat turn;
    int faction;
    float fraction;
  };
  struct Source {  // a beam end that throws sparks, kept from frame to frame by its key
    float carry = 0;      // sparks owed
    double burstAt = 0;   // the next burst (the UEF's and the Cybran's)
    double seen = 0;
  };
  bool Ready();
  const Site* SiteOf(const Beam& b) const;

  gfx::Device* device_;
  bool on_ = false;
  double clock_ = 0, lastClock_ = -1, lastStep_ = -1, aliveUntil_ = -1;
  std::vector<Beam> beams_;
  std::vector<Site> sites_;
  std::unordered_map<uint64_t, Source> sources_;
  uint32_t random_ = 12345;
  float Random();  // 0..1
  // the sparks thrown this frame: four float4 each (where and life; velocity and kind; the beam's start; its end)
  std::vector<float> spawns_;
  int spawnCount_ = 0;
  int head_ = 0;  // where the next spark thrown goes in the state
  // the card's side: the state (place and age; velocity, kind and life) twice over, read one and written the other
  int tried_ = 0;  // 0 not yet, 1 made, -1 cannot be made
  gfx::Pipeline step_, draw_;
  gfx::Texture place_[2], motion_[2];
  int current_ = 0;
  bool fresh_ = true;  // (the state read holds nothing yet)
  bool drawable_ = false;  // (a state has been written)
};

}  // namespace render
