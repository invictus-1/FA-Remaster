// Shields that look like energy (World::energyShields, viewer --energy-shields; docs/render-lighting.md, item 11): the
// modern look's own way of drawing a shield's bubble, in place of the plain see-through shell the model renderer draws
// (mesh_renderer.cpp, kind 3). Off by default; off, nothing of this runs and the picture is as before.
//
// What it adds to the plain bubble (whose look it keeps underneath: its faction's tint, brighter toward the rim, its
// pattern drifting at two sizes):
//  - What is behind the bubble bends: the picture behind its near side (Post::CopyBehind, the copy the modern water and
//    the heat shimmer read) is taken from a little toward the bubble's middle, most just inside the rim and hardly at
//    all where the bubble is seen face on, so the units inside stay readable; the shell shimmers a little.
//  - Each hit sends rings across the shell: from where it struck, three crests running outward over the sphere, fading
//    as they spread and within two seconds; several hits at once, each its own (at most 8 a bubble, the newest). The
//    rings bend what is behind as they pass, and glow.
//  - Its pattern flares: round where it was just struck, and all over as the shield weakens (its unit's shield ratio,
//    FrameUnit::shieldRatio); below two fifths it stutters.
//  - Its light falls on what is inside it: the ground under it and the units in it take its tint, faint in the middle
//    and stronger toward the rim, ending at the shell (overlapping bubbles each add theirs).
// Where hits come from: the shield's script makes a flash at its bubble's centre for each hit (shield.lua,
// CreateImpactEffect: the ShieldSection mesh, turned by OrientFromDir toward where it struck, its emitters moved out
// along its +z), so a flash's +z is the way from the centre to the hit and its age is the hit's.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "post.h"
#include "view.h"

namespace render {

class EnergyShields {
 public:
  explicit EnergyShields(gfx::Device* device) : device_(device) {}

  // --- a frame (World::AddFrame lists them) ---
  // Forgets the frame before's bubbles, hits and owners.
  void Begin();
  // A shield's bubble: its dome (a model whose first level is a shield's, ModelLod::shield) where the frame has it.
  void AddBubble(const Model* model, const Mat4& world);
  // A hit: a flash made at a bubble's centre `at`, `toward` the way from there to where it struck (unit length),
  // `age` seconds ago.
  void AddHit(Vec3 at, Vec3 toward, float age);
  // A unit with a shield, and how much of its shield is left (0..1): the bubble round it weakens with it.
  void AddOwner(Vec3 position, float shieldRatio);
  bool Any() const { return !bubbles_.empty(); }
  // Makes the shaders now, if not yet (World::Open, when the option is on as a map opens, instead of a hitch of tens of
  // milliseconds at the first bubble in play). False where they cannot be made.
  bool Prepare();

  // Draws the bubbles over the solid scene and the water, before the other see-through things (they need a pass
  // into the scene's colour and depth open: Post::ContinueScene). Leaves such a pass open. The first call makes
  // the shaders; where they cannot be made, nothing is drawn (said once).
  void Draw(const View& view, Post* post);

  // How much of each (1: as made): the bending of what is behind, the light on what is inside, the rings.
  float bend = 1, light = 1, rings = 1;
  // How much of its shield a bubble has left when no unit is found under it (the effects stand: no units in its
  // frame); 0..1.
  float healthUnknown = 1;
  int drawn = 0;  // bubbles drawn in the last frame

 private:
  struct Hit {
    Vec3 at, toward;
    float age;
  };
  struct Owner {
    Vec3 at;
    float ratio;
  };
  struct Bubble {
    const Model* model;
    Mat4 world;
    Vec3 origin;       // where the frame has it (its flashes are made there)
    Vec3 centre;       // its sphere's, from the dome's bounds
    float radius;
    float health = -1;  // -1: no unit found under it
    float distance = 0;  // from the eye (drawn from the farthest)
    int hits = 0;
    float hit[8][4];  // toward, age
  };
  bool Init();
  gfx::Device* device_;
  int state_ = 0;  // 0 the shaders not tried yet, 1 made, -1 they cannot be
  gfx::Pipeline inside_, shell_;
  std::vector<Bubble> bubbles_;
  std::vector<Hit> hits_;
  std::vector<Owner> owners_;
  std::vector<Bubble*> order_;
};

// A test scene for the viewer (--energy-test n; for looking and timing without a game): n shield generators of the
// four factions (UEB4202, UAB4202, URB4202, XSB4202, their shields' sizes and heights as their blueprints give them)
// in two rows round `around`, overlapping, their bubbles up and each hit every `every` seconds from a direction above
// the ground, as the game's shield script makes the flashes (the ShieldSection mesh at the bubble's centre turned
// toward the hit, for two seconds); their shields at different strengths (1, 0.75, 0.5, 0.25, again). As a game's
// frame for World::AddFrame at `clock` (the world's clock); `names` is kept by the caller between frames.
void EnergyShieldsTestFrame(int count, Vec3 around, double clock, float every, const std::function<float(float, float)>& ground,
                            Frame* frame, std::vector<std::string>* names);

}  // namespace render
