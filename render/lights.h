// Small lights: the flash of an explosion, later muzzle flashes, shots, lamps on units and buildings.
// Each is a point that lights what is within its radius, fading to nothing at the radius.
//
// So that thousands stay cheap, the lights are sorted every frame into a grid of squares laid over
// the ground (in this game everything of interest is near the ground, so a flat grid does what
// other renderers need a grid in the picture's depth for): each square lists the lights that reach
// it, and a pixel looks only at the lights of the square its place on the ground is in. The grid
// and the lights go to the shaders in one buffer (lighting.h reads it).
//
// Only the modern look uses them. The original game has no such lights: there the "light particle"
// of an explosion is a glow laid on the ground (decals.h), which the original look keeps.
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

class Lights {
 public:
  Lights(gfx::Device* device, const moho::Vfs* vfs);

  // Empties this frame's list; `now` is the effects' clock, seconds.
  void Begin(double now);
  // A light for this frame. `color` is linear, as bright as the surface it lights fully would be
  // (1, 1, 1: about as the sun lights it).
  void Add(Vec3 position, float radius, Vec3 color);
  // A beam: light within `coneDegrees` (the whole opening angle) about `direction`, full to seven
  // tenths of the way out from its middle and fading to nothing at its edge, reaching `radius`
  // along the beam; and `glow` of it to all sides within a fifth of the radius, so that the lamp's
  // own surroundings are not left dark.
  void AddBeam(Vec3 position, float radius, Vec3 color, Vec3 direction, float coneDegrees, float glow);
  // A light that lives `seconds` from `born` and takes its colour and strength along that time from
  // a ramp texture (the scripts' "light particle": `size` is what the original draws its glow with;
  // the light reaches a few times as far).
  void Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds);
  // The colour a thing glows in whose particles take theirs from that ramp texture: the ramp's
  // strongest place (linear light), for the light a shot in flight throws.
  Vec3 RampGlow(const std::string& ramp);
  // Sorts this frame's lights (those added, and the flashes alive now) into the grid, hands them to
  // the shaders and writes where the grid is into `view`. Call once a frame, before the scene's passes.
  void Build(View* view);
  // Binds the lights' buffer for the pass just begun (every scene pass whose shaders light surfaces).
  void Bind();

  float strength = 3.0f;      // how bright a flash of full ramp colour is
  float reach = 3.0f;         // a flash's radius, in sizes of its glow
  float night = 1;            // what flashes are multiplied by this frame (the night's dimming of the game's effects)
  int count = 0;              // lights in the last Build

 private:
  struct Light {
    Vec3 position;
    float radius;
    Vec3 color;
    float glow = 1;       // a beam: the share that still shines to all sides close to the lamp
    Vec3 direction;       // a beam: where it points
    float cosEdge = -2;   // a beam: the cosine of half its opening angle; below -1: it shines to all sides
  };
  struct FlashLight {
    const std::vector<Vec3>* ramp;  // colour x alpha along its time
    Vec3 position;
    float size;
    double born;
    float seconds;
  };
  const std::vector<Vec3>* Ramp(const std::string& path);

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  gfx::Buffer buffer_;
  std::vector<Light> lights_;
  std::vector<FlashLight> flashes_;
  std::unordered_map<std::string, std::vector<Vec3>> ramps_;
  std::vector<float> rows_;
  std::vector<uint32_t> cellCount_, cellFirst_, indices_;
  double now_ = 0;
};

}  // namespace render
