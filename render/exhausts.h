// (unitsfx) Exhausts that look real (Chris's playthrough of 20 October, item 8: "thrusters on Izanagi need to be more
// integrated and look much more realistic"; the visuals session's look for the Izanagi's drives, her
// visuals\renderer_reply_lod.md section 62). A player's option of the modern look (World::realExhausts, viewer
// --exhausts), off by default; off, every exhaust is the game's flat band as before.
//
// Which beams: a beam a script hangs on one thing (CreateBeamEmitterOnEntity: an aircraft's BeamExhaust, a missile's
// exhaust beam, a mod's drive) whose blueprint's name says it is an engine's ("exhaust", "thrust", "drive", "afterburn",
// "engine", "vernier", "retro", the game's air_move_trail_beam and air_idle_trail_beam), or whose blueprint says so
// itself (its own field Exhaust, below). A weapon's beam, a beam between two things and a shell's tracer beam are not.
//
// How it is drawn, each drive (one beam on one bone of one thing) generic for every unit:
//  - A plume of three soft layers from the beam's start along its way, as long as the beam (its Length) and as wide as
//    it (its Thickness is the bell's radius): a CORE, white-hot, short and thin and very bright (the picture's bloom
//    takes it); a SHEATH in the beam's own colour (its texture's average times its StartColor), the whole length, a slow
//    noise flowing along it; a FRINGE, the sheath's colour gone deeper (blue to violet, orange to red), faint, longest
//    and widening. Each layer is a soft tube round the beam's line: what the eye sees of it is worked out from how close
//    the line of sight passes the line and how aslant (a tube seen end on is thicker), so it has no edge anywhere; it
//    fades where it meets the hull or the ground (the scene's depth), so there is no seam at the lip.
//  - The mouth: a disc across the bell's mouth glowing white-hot in the middle to the fringe's colour at its rim, seen
//    from behind (looking into the bell), flickering by a percent or two.
//  - At full burn (the drive's plume within a tenth of the longest it has had), on large drives only (a Thickness, the
//    bell's radius, of 0.35 or more and a plume 6 or longer; or as the blueprint's Diamonds says), three or four shock
//    diamonds in the core, 0.7 bell diameters apart, shimmering a little.
//  - A small light per drive just behind the mouth, in the plume's colour, its reach with the plume's length: on the
//    hull round it, on the ground under a low aircraft.
//  - Heat haze behind it (the picture bent a few pixels), strongest at the mouth, two plume lengths long, drawn with the
//    other heat shimmer (after the particles).
//  - The throttle followed smoothly: each drive keeps its drawn length and eases it to what its beam says now, quickly up
//    (a flare, 0.15 s) and slowly down (0.4 s), so a script that swaps a low, a mid and a full beam on the same bone, or
//    the game's idle and cruise beams, reads as one engine opening up and easing off. A new drive flares on from a third
//    of its length. A script may set the throttle every tick through SetBeamParam('LENGTH' or 'THICKNESS', n) (the
//    original game's own call for a beam on one thing); the plume eases to it the same way. How bright and how long the
//    plume is, is its throttle: its drawn length against the longest its drive has had.
//
// The blueprint's own field (optional; a beam blueprint is read by this renderer's own reader, which keeps every field):
//   Exhaust = false            -- never a plume (a beam named like an engine's that is not one)
//   Exhaust = true             -- a plume though its name does not say so
//   Exhaust = {                -- a plume, tuned (every field optional):
//     Core = { 1, 0.95, 1 },   --   the core's colour (0..1, as on screen; default white-hot of the sheath's hue)
//     Sheath = { .4, .6, 1 },  --   the sheath's (default: the beam's texture's average times its StartColor)
//     Fringe = { .5, .3, 1 },  --   the fringe's (default: the sheath's, deeper)
//     Brightness = 1, Length = 1, Width = 1,   -- times the plume's own
//     Diamonds = 1,            --   shock diamonds at full burn (0..1; default: large drives 1, others 0)
//     Light = 1, Haze = 1, Flow = 1, Flicker = 1,  -- the light's strength, the haze's, the sheath's flow speed, the flicker
//   },
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

class Lights;

class Exhausts {
 public:
  Exhausts(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {}

  // A new frame (World::Begin): whether the look is on for it, and the effects' clock (seconds). Drives not added for two
  // seconds are forgotten.
  void Begin(double clock, bool on);
  bool On() const { return on_; }

  // From the effects (effects.cpp), while it is on: is that beam blueprint (as the game names it; `faction` and
  // `remastered` as the beams read it, ribbons.h) an engine's exhaust drawn as a plume?
  bool Takes(const std::string& blueprint, const std::string& faction, bool remastered);
  // A drive this frame: `key` the same from frame to frame for one beam's bone on one thing (whatever beam the script hangs
  // there), the beam's start, the way its bone points (unit length; the blueprint's Length may be negative: backward),
  // what SetBeamParam set (`length`, `thickness`; below -1e29: not set), and where the thing it is on stands (its own
  // hull casts no ray-traced shadow of the drive's light).
  void Add(uint64_t key, const std::string& blueprint, const std::string& faction, bool remastered, Vec3 start, Vec3 forward,
           float length, float thickness, Vec3 origin);

  // From the world's Render, while it is on: the drives' lights (before Lights::Build; `night` as Lights::night).
  void AddLights(Lights* lights, float night);
  // The plumes, into the scene's colour alone (Post::BeginColorOnly), after everything solid and the water: they test the
  // scene's depth themselves (`zNear` the camera's).
  void Draw(const View& view, gfx::Texture sceneDepth, float zNear);
  // The heat haze behind the drives: PrepareHaze (out of a pass) says whether any is to be drawn this frame, DrawHaze
  // bends `behind` (the picture as it is, one sample a pixel) into the scene's colour.
  bool PrepareHaze(const View& view);
  void DrawHaze(const View& view, gfx::Texture behind, gfx::Texture sceneDepth, float zNear);

  // Tuning (viewer --exhaust-tune): all plumes' brightness, their lights', the haze's, times their own.
  float brightness = 1, lightStrength = 1, hazeStrength = 1;
  int drawn = 0;  // drives drawn in the last frame

 private:
  struct Look {  // a beam blueprint's, as a plume
    bool read = false, ok = false;
    int exhaust = -1;  // its own field Exhaust: -1 none, 0 false, 1 true or a table
    float length = 1, halfWidth = 0.1f;
    Vec3 sheath{0.45f, 0.65f, 1.0f}, core{-1, -1, -1}, fringe{-1, -1, -1};  // (core and fringe below 0: from the sheath)
    float brightness = 1, lengthScale = 1, widthScale = 1, diamonds = -1, light = 1, haze = 1, flow = 1, flicker = 1;
  };
  struct Drive {  // one beam's bone on one thing, kept from frame to frame
    Vec3 start, axis, origin;
    float drawn = 0, longest = 0, radius = 0.1f, fadeIn = 0;
    double seen = -1;
    bool fresh = true;  // (added this frame)
    const Look* look = nullptr;
    float target = 0, targetRadius = 0.1f;
    uint32_t seed = 0;
  };
  const Look& LookOf(const std::string& blueprint, const std::string& faction, bool remastered);
  Vec3 TextureColor(const std::string& path);
  bool Ready();

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  bool on_ = false;
  double clock_ = 0, lastClock_ = -1;
  float step_ = 0;  // seconds since the last frame
  std::unordered_map<std::string, Look> looks_;
  std::unordered_map<std::string, Vec3> textureColors_;
  std::unordered_map<uint64_t, Drive> drives_;
  std::vector<float> rows_, hazeRows_;  // this frame's plumes and hazes, as the shaders take them
  int hazeCount_ = 0;
  int tried_ = 0;  // the pipelines: 0 not yet, 1 made, -1 cannot be made
  gfx::Pipeline plume_, haze_;
};

}  // namespace render
